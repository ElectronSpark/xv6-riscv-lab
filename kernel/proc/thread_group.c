/**
 * @file thread_group.c
 * @brief POSIX thread group (process) abstraction — implementation
 *
 * Implements thread group lifecycle, membership management, and
 * group-level signal delivery following the POSIX/Linux model.
 *
 * Design:
 * - Each thread has a pointer to its thread_group (never NULL for active threads)
 * - A thread_group is allocated from a slab cache
 * - Refcounted: each member thread holds one reference
 * - Thread group leader is the first thread; its PID is the TGID
 * - Process-directed signals go to shared_pending; any eligible thread handles them
 * - exit_group() sends SIGKILL to all threads in the group
 *
 * Locking:
 *   All thread_group fields are protected by the global pid_lock (rwlock):
 *     - pid_wlock for mutations (add/remove thread, group_exit)
 *     - pid_rlock for read-only traversal (signal delivery, queries)
 *   Shared pending signal state (enqueue/dequeue of ksiginfo) is additionally
 *   serialized by sigacts->lock (shared among all group threads via CLONE_SIGHAND).
 *
 *   Lock ordering:  pid_lock > sigacts.lock > tcb_lock
 */

#include "proc/thread_group.h"
#include "proc/thread.h"
#include "signal.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "list.h"
#include <mm/slab.h>
#include <smp/atomic.h>
#include "proc/sched.h"
#include "errno.h"
#include <smp/ipi.h>

static slab_cache_t __tg_pool;

#define TG_MAX_SIGINFO_PER_SIGNAL 8

static int __tg_siginfo_queue_len(sigpending_t *sq) {
    int n = 0;
    ksiginfo_t *pos = NULL;
    ksiginfo_t *tmp = NULL;
    list_foreach_node_safe(&sq->queue, pos, tmp, list_entry) {
        n++;
    }
    return n;
}

// ───── Subsystem initialization ─────

void thread_group_init(void) {
    slab_cache_init(&__tg_pool, "thread_group",
                    sizeof(struct thread_group), SLAB_FLAG_STATIC);
}

// ───── Reference counting ─────

void thread_group_get(struct thread_group *tg) {
    if (tg == NULL) return;
    atomic_inc(&tg->refcount);
}

void thread_group_put(struct thread_group *tg) {
    if (tg == NULL) return;
    if (atomic_dec_unless(&tg->refcount, 1)) {
        // refcount was > 1, decremented; still alive
        return;
    }
    // refcount was 1 → 0: free the thread group
    tg_shared_pending_destroy(tg);
    slab_free(tg);
}

// ───── Shared pending signal helpers ─────

void tg_shared_pending_init(struct thread_group *tg) {
    assert(tg != NULL, "tg_shared_pending_init: NULL");
    tg->shared_pending.sig_pending_mask = 0;
    for (int i = 0; i < NSIG; i++) {
        list_entry_init(&tg->shared_pending.sig_pending[i].queue);
    }
}

void tg_shared_pending_destroy(struct thread_group *tg) {
    if (tg == NULL) return;
    // Free any queued ksiginfo entries
    for (int i = 0; i < NSIG; i++) {
        ksiginfo_t *ksi = NULL;
        ksiginfo_t *tmp = NULL;
        list_foreach_node_safe(&tg->shared_pending.sig_pending[i].queue,
                               ksi, tmp, list_entry) {
            list_entry_detach(&ksi->list_entry);
            ksiginfo_free(ksi);
        }
    }
    tg->shared_pending.sig_pending_mask = 0;
}

// ───── Thread group lifecycle ─────

int thread_group_alloc(struct thread *leader) {
    assert(leader != NULL, "thread_group_alloc: NULL leader");

    struct thread_group *tg = slab_alloc(&__tg_pool);
    if (tg == NULL) {
        return -ENOMEM;
    }

    memset(tg, 0, sizeof(*tg));
    list_entry_init(&tg->thread_list);
    tg->group_leader = leader;
    // TGID will be set after the leader gets a PID assigned
    // (in proctab_proc_add). For now set to -1.
    tg->tgid = -1;
    __atomic_store_n(&tg->live_threads, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&tg->refcount, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&tg->group_exit, 0, __ATOMIC_SEQ_CST);
    tg->group_exit_code = 0;
    tg->group_exit_task = NULL;
    tg->group_stop_count = 0;
    tg->group_stop_signo = 0;

    tg_shared_pending_init(tg);

    // Link leader into the thread group
    leader->thread_group = tg;
    list_entry_init(&leader->tg_entry);
    list_entry_push(&tg->thread_list, &leader->tg_entry);

    return 0;
}

// Caller must hold pid_wlock.
void thread_group_add(struct thread_group *tg, struct thread *child) {
    assert(tg != NULL, "thread_group_add: NULL tg");
    assert(child != NULL, "thread_group_add: NULL child");
    pid_assert_wholding();

    child->thread_group = tg;
    list_entry_init(&child->tg_entry);

    list_entry_push(&tg->thread_list, &child->tg_entry);
    atomic_inc(&tg->live_threads);

    thread_group_get(tg); // One ref per member thread
}

// Caller must hold pid_wlock.
bool thread_group_remove(struct thread *p) {
    if (p == NULL) return true;
    struct thread_group *tg = p->thread_group;
    if (tg == NULL) return true;

    pid_assert_wholding();

    bool last = false;

    if (!LIST_ENTRY_IS_DETACHED(&p->tg_entry)) {
        list_entry_detach(&p->tg_entry);
    }
    int remaining = atomic_sub(&tg->live_threads, 1);
    // atomic_sub returns the *previous* value, so remaining-1 == new count
    if (remaining <= 1) {
        last = true;
    }

    // Don't clear p->thread_group here — the leader's zombie state
    // still needs it for wait() to read tgid. It will be cleared
    // in thread_destroy.

    return last;
}

// ───── Queries ─────

bool thread_is_group_leader(struct thread *p) {
    if (p == NULL || p->thread_group == NULL) return true;
    return p->thread_group->group_leader == p;
}

int thread_tgid(struct thread *p) {
    if (p == NULL) return -1;
    if (p->thread_group == NULL) return p->pid;
    int tgid = p->thread_group->tgid;
    return (tgid > 0) ? tgid : p->pid;
}

// ───── Group exit ─────

void thread_group_exit(struct thread *p, int code) {
    if (p == NULL) return;
    struct thread_group *tg = p->thread_group;
    if (tg == NULL) {
        exit(code);
        return;
    }

    // Only the first exit_group caller wins
    int expected = 0;
    if (!__atomic_compare_exchange_n(&tg->group_exit, &expected, 1,
                                     false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST)) {
        // Another thread already called exit_group
        exit(code);
        return;
    }

    tg->group_exit_code = code;
    tg->group_exit_task = p;

    // Send SIGKILL to all other threads in the group.
    // Acquire pid_rlock to safely iterate the thread list.
    pid_rlock();
    struct thread *t;
    struct thread *tmp;
    list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
        if (t == p) continue;
        // Set the killed flag directly — SIGKILL bypasses all signal logic
        THREAD_SET_KILLED(t);
        THREAD_SET_SIGPENDING(t);
        // If thread is sleeping, wake it up
        tcb_lock(t);
        if (THREAD_SLEEPING(t) || THREAD_STOPPED(t)) {
            __thread_state_set(t, THREAD_RUNNING);
        }
        tcb_unlock(t);
    }
    pid_runlock();

    // Caller should call exit(code) after this
    exit(code);
}

// ───── Thread group signal delivery ─────

/**
 * Pick an eligible thread from the group to handle a signal.
 * Preference: 1) the group leader, 2) any thread that doesn't block the signal.
 * Returns NULL if no eligible thread found (all block the signal).
 * Caller must hold pid_rlock or pid_wlock.
 */
static struct thread *__tg_pick_thread(struct thread_group *tg, int signo) {
    if (tg == NULL) return NULL;

    // First try the group leader (common case)
    struct thread *leader = tg->group_leader;
    if (leader != NULL && leader->sigacts != NULL) {
        if (!sigismember(&leader->sigacts->sa_sigmask, signo) &&
            !THREAD_IS_ZOMBIE(__thread_state_get(leader)) &&
            __thread_state_get(leader) != THREAD_UNUSED) {
            return leader;
        }
    }

    // Otherwise find any eligible thread
    struct thread *t;
    struct thread *tmp;
    list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
        if (t == leader) continue;
        enum thread_state st = __thread_state_get(t);
        if (st == THREAD_UNUSED || st == THREAD_ZOMBIE) continue;
        if (t->sigacts == NULL) continue;
        if (!sigismember(&t->sigacts->sa_sigmask, signo)) {
            return t;
        }
    }

    // All threads block this signal — deliver to leader anyway
    // (it will be pending until unmasked)
    return leader;
}

int tg_signal_send(struct thread_group *tg, struct ksiginfo *info) {
    if (tg == NULL || info == NULL) return -EINVAL;
    if (SIGBAD(info->signo)) return -EINVAL;

    int signo = info->signo;

    // Check if the group is already dead
    if (__atomic_load_n(&tg->live_threads, __ATOMIC_ACQUIRE) <= 0) {
        return -ESRCH;
    }

    // For SIGKILL, bypass shared_pending and send directly to all threads
    if (signo == SIGKILL) {
        pid_rlock();
        struct thread *t;
        struct thread *tmp;
        list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
            THREAD_SET_KILLED(t);
            THREAD_SET_SIGPENDING(t);
            tcb_lock(t);
            if (THREAD_SLEEPING(t) || THREAD_STOPPED(t)) {
                __thread_state_set(t, THREAD_RUNNING);
            }
            tcb_unlock(t);
        }
        // Also record in shared pending for completeness
        sigaddset(&tg->shared_pending.sig_pending_mask, signo);
        pid_runlock();
        return 0;
    }

    // For other signals, add to shared_pending and pick a thread.
    // Acquire pid_rlock to iterate the thread_list and read sigacts.
    pid_rlock();

    struct thread *leader = tg->group_leader;

    // Classify the signal early — we need to know if it's SIGCONT/SIGSTOP
    // before the dedup check, because SIGCONT must always cancel pending
    // stops and wake stopped threads even if SIGCONT is already pending.
    bool is_cont = false;
    bool is_stop = false;
    bool is_term = false;
    sigset_t stop_mask = 0;
    if (leader != NULL && leader->sigacts != NULL) {
        sigacts_lock(leader->sigacts);

        // Check if signal should be ignored
        if (sigismember(&leader->sigacts->sa_sigignore, signo)) {
            sigacts_unlock(leader->sigacts);
            pid_runlock();
            return 0;
        }

        is_cont = sigismember(&leader->sigacts->sa_sigcont, signo) > 0;
        is_stop = sigismember(&leader->sigacts->sa_sigstop, signo) > 0;
        is_term = sigismember(&leader->sigacts->sa_sigterm, signo) > 0;
        if (is_cont) {
            stop_mask = leader->sigacts->sa_sigstop;
        }

        // SIGCONT side-effects: cancel pending stops and clear per-thread
        // stop pending bits. This must happen even if SIGCONT is already
        // pending (a second SIGCONT must still cancel a second SIGSTOP).
        if (is_cont) {
            tg->shared_pending.sig_pending_mask &= ~stop_mask;
            struct thread *t;
            struct thread *tmp;
            list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
                t->signal.sig_pending_mask &= ~stop_mask;
            }
        }

        // SIGSTOP side-effects: cancel pending SIGCONT from shared pending
        if (is_stop) {
            sigset_t cont_mask = leader->sigacts->sa_sigcont;
            tg->shared_pending.sig_pending_mask &= ~cont_mask;
        }

        sigaction_t *act = &leader->sigacts->sa[signo];
        if (act->sa_flags & SA_SIGINFO) {
            sigpending_t *sq = &tg->shared_pending.sig_pending[signo - 1];

            // Enforce per-signal queue cap: drop oldest if at limit
            int qlen = __tg_siginfo_queue_len(sq);
            if (qlen >= TG_MAX_SIGINFO_PER_SIGNAL) {
                if (!LIST_IS_EMPTY(&sq->queue)) {
                    ksiginfo_t *old = LIST_FIRST_NODE(
                        &sq->queue, ksiginfo_t, list_entry);
                    if (old) {
                        list_entry_detach(&old->list_entry);
                        ksiginfo_free(old);
                    }
                }
            }

            // Allocate a COPY — the caller's info may be stack-allocated,
            // so we must never link it directly into the queue.
            ksiginfo_t *ksi = ksiginfo_alloc();
            if (ksi != NULL) {
                *ksi = *info;
                list_entry_init(&ksi->list_entry);
                list_entry_push(&sq->queue, &ksi->list_entry);
            }
        } else {
            // Standard (non-SA_SIGINFO) signal: no queue, just the pending bit.
            // If already pending, there's nothing more to do — UNLESS this is
            // SIGCONT, which must always wake stopped threads.
            if (sigismember(&tg->shared_pending.sig_pending_mask, signo)
                && !is_cont) {
                sigacts_unlock(leader->sigacts);
                pid_runlock();
                return 0;
            }
        }
        sigacts_unlock(leader->sigacts);
    } else {
        // No leader/sigacts — fall back to standard dedup
        if (sigismember(&tg->shared_pending.sig_pending_mask, signo)
            && !is_cont) {
            pid_runlock();
            return 0;
        }
    }

    sigaddset(&tg->shared_pending.sig_pending_mask, signo);

    if (is_cont) {
        // SIGCONT: wake ALL stopped threads in the group
        struct thread *t;
        struct thread *tmp;
        list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
            THREAD_SET_SIGPENDING(t);
            if (THREAD_STOPPED(t)) {
                scheduler_wakeup_stopped(t);
            } else {
                tcb_lock(t);
                if (__thread_state_get(t) == THREAD_INTERRUPTIBLE) {
                    __thread_state_set(t, THREAD_RUNNING);
                }
                tcb_unlock(t);
            }
        }
    } else {
        // Pick a single thread to wake up for delivery
        struct thread *target = __tg_pick_thread(tg, signo);

        if (target != NULL) {
            THREAD_SET_SIGPENDING(target);

            if (is_term && THREAD_STOPPED(target)) {
                // Terminal signal → wake stopped thread so it can exit
                scheduler_wakeup_stopped(target);
            } else {
                tcb_lock(target);
                enum thread_state st = __thread_state_get(target);
                if (st == THREAD_INTERRUPTIBLE) {
                    __thread_state_set(target, THREAD_RUNNING);
                } else if (is_stop && st == THREAD_RUNNING) {
                    // Stop signal to running thread: send IPI for fast processing
                    tcb_unlock(target);
                    int target_cpu = smp_load_acquire(
                        &target->sched_entity->cpu_id);
                    if (target_cpu != cpuid()) {
                        ipi_send_single(target_cpu, IPI_REASON_RESCHEDULE);
                    } else {
                        SET_NEEDS_RESCHED();
                    }
                    goto out;
                }
                tcb_unlock(target);
            }
        }
    }

out:
    pid_runlock();

    return 0;
}

bool tg_signal_pending(struct thread_group *tg, struct thread *p) {
    if (tg == NULL || p == NULL || p->sigacts == NULL) return false;
    sigset_t shared = smp_load_acquire(&tg->shared_pending.sig_pending_mask);
    sigset_t blocked = p->sigacts->sa_sigmask;
    return (shared & ~blocked) != 0;
}

// Caller must hold sigacts lock and pid_rlock (or pid_wlock).
struct ksiginfo *tg_dequeue_signal(struct thread_group *tg, int signo) {
    if (tg == NULL || SIGBAD(signo)) return NULL;

    sigpending_t *sq = &tg->shared_pending.sig_pending[signo - 1];
    ksiginfo_t *ksi = NULL;

    if (!LIST_IS_EMPTY(&sq->queue)) {
        // Dequeue the first entry
        list_node_t *first = sq->queue.next;
        ksi = container_of(first, ksiginfo_t, list_entry);
        list_entry_detach(&ksi->list_entry);
    }

    // Clear the pending bit if no more entries and no other reason to keep it
    if (LIST_IS_EMPTY(&sq->queue)) {
        sigdelset(&tg->shared_pending.sig_pending_mask, signo);
    }

    return ksi;
}

// Caller must hold pid_rlock or pid_wlock.
void tg_recalc_sigpending(struct thread_group *tg) {
    if (tg == NULL) return;
    struct thread *t;
    struct thread *tmp;
    list_foreach_node_safe(&tg->thread_list, t, tmp, tg_entry) {
        if (t->sigacts == NULL) continue;
        // Check both per-thread and shared pending
        sigset_t blocked = t->sigacts->sa_sigmask;
        sigset_t thread_pending = smp_load_acquire(&t->signal.sig_pending_mask);
        sigset_t shared = tg->shared_pending.sig_pending_mask;
        if (((thread_pending | shared) & ~blocked) != 0) {
            THREAD_SET_SIGPENDING(t);
        } else {
            THREAD_CLEAR_SIGPENDING(t);
        }
    }
}
