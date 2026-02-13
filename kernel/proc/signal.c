/*
 * Signal handling for xv6
 *
 * LOCKING:
 * Signal operations use a unified lock approach (like Linux sighand->siglock).
 * All signal state is protected by sigacts->lock:
 *   - Signal actions (sigacts->sa[])
 *   - Per-thread signal masks (thread->signal.sig_mask, sig_saved_mask)
 *   - Per-thread pending signals (thread->signal.sig_pending_mask,
 * sig_pending[])
 *
 * Key rules:
 * - sigacts->lock must be held when reading/writing any signal state
 * - Release sigacts->lock BEFORE scheduler operations (wakeup, yield)
 * - Copy data from protected structures before releasing lock if needed after
 *
 * This is simpler than the old two-lock (tcb_lock + sigacts_lock) approach
 * and matches Linux's design where sighand->siglock is THE signal lock.
 *
 * The THREAD_FLAG_SIGPENDING flag provides O(1) checks for pending signals.
 * recalc_sigpending_tsk() updates this flag and must be called after any
 * change to signal.sig_pending_mask or signal.sig_mask.
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "signal.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include <mm/slab.h>
#include "proc/sched.h"
#include "list.h"
#include "bits.h"
#include "smp/ipi.h"
#include "clone_flags.h"
#include "errno.h"

static slab_cache_t __sigacts_pool;
static slab_cache_t __ksiginfo_pool;

// Forward declarations for helper functions
static void sigacts_assert_holding(sigacts_t *sa);

sig_defact signo_default_action(int signo) {
    switch (signo) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
        return SIG_ACT_IGN;
    case SIGALRM:
    case SIGUSR1:
    case SIGUSR2:
    // case SIGCLD:
    // case SIGEMT:
    case SIGHUP:
    case SIGINT:
    case SIGIO:
    case SIGKILL:
    // case SIGLOST:
    case SIGPIPE:
    // case SIGPOLL:
    case SIGPROF:
    case SIGPWR:
    case SIGSTKFLT:
    case SIGTERM:
    case SIGVTALRM:
        return SIG_ACT_TERM;
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return SIG_ACT_STOP;
    case SIGCONT:
        return SIG_ACT_CONT;
    case SIGABRT:
    case SIGBUS:
    case SIGILL:
    // case SIGIOT:
    case SIGQUIT:
    case SIGSEGV:
    case SIGSYS:
    case SIGTRAP:
    // case SIGUNUSED
    case SIGXCPU:
    case SIGXFSZ:
    case SIGFPE:
        return SIG_ACT_CORE;
    default:
        return SIG_ACT_INVALID; // Invalid signal number
    }
}

#define SIG_MANDATORY_MASK (SIGNO_MASK(SIGKILL) | SIGNO_MASK(SIGSTOP))

/*
 * Recalculate the TIF_SIGPENDING flag for a task.
 *
 * This checks if there are any pending signals that are not blocked.
 * If so, the SIGPENDING flag is set; otherwise it's cleared.
 *
 * Following Linux's approach:
 * - Set flag if pending & ~blocked has any bits set
 * - Returns true if flag was set, false otherwise
 *
 * Caller must hold sigacts_lock (or ensure sigacts won't change).
 */
bool recalc_sigpending_tsk(struct thread *p) {
    if (!p || !p->sigacts) {
        return false;
    }

    sigset_t pending = smp_load_acquire(&p->signal.sig_pending_mask);
    sigset_t blocked = p->signal.sig_mask;

    // Also check thread group shared pending signals
    if (p->thread_group != NULL) {
        sigset_t shared =
            smp_load_acquire(&p->thread_group->shared_pending.sig_pending_mask);
        pending |= shared;
    }

    if ((pending & ~blocked) != 0) {
        THREAD_SET_SIGPENDING(p);
        return true;
    }

    /*
     * We must never clear the flag in another thread, or in current
     * when it's possible the current syscall is returning -ERESTART*.
     * So we only clear it for the current process.
     */
    return false;
}

/*
 * Recalculate TIF_SIGPENDING for the current process.
 * This can clear the flag if no signals are pending.
 */
void recalc_sigpending(void) {
    struct thread *p = current;
    if (!p || !p->sigacts) {
        return;
    }

    sigacts_t *sa = p->sigacts;
    sigacts_lock(sa);
    if (!recalc_sigpending_tsk(p)) {
        // No pending signals, safe to clear flag for current process
        THREAD_CLEAR_SIGPENDING(p);
    }
    sigacts_unlock(sa);
}

void sigpending_init(struct thread *p) {
    if (!p) {
        return; // Invalid pointer
    }
    for (int i = 0; i < NSIG; i++) {
        sigpending_t *sq = &p->signal.sig_pending[i];
        list_entry_init(&sq->queue);
    }
}

void sigpending_destroy(struct thread *p) {
    if (!p) {
        return; // Invalid pointer
    }
    // Called at process exit - sigacts should already be locked or no longer
    // shared
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }
    // Ensure all per-signal queues are already empty. Do NOT silently purge
    // here.
    for (int i = 1; i <= NSIG; i++) {
        sigpending_t *sq = &p->signal.sig_pending[i - 1];
        assert(LIST_IS_EMPTY(&sq->queue),
               "sigpending_destroy: pending signals not empty for signal %d",
               i);
    }
    assert(p->signal.sig_pending_mask == 0,
           "sigpending_destroy: pending mask not zero");
}

// Copy pending signal state from src to dst during fork/clone.
// Will suppose the caller is holding sigacts lock
void sigpending_clone(struct thread_signal *dst, struct thread_signal *src,
                      uint64 clone_flags, int esignal) {
    // Copy per-thread signal mask from parent
    dst->sig_mask = src->sig_mask;
    dst->sig_saved_mask = src->sig_saved_mask;

    if (clone_flags & CLONE_THREAD) {
        // For CLONE_THREAD, the child does not send a signal to the parent
        // on exit (Linux behavior). The exit signal is 0.
        dst->esignal = 0;
    } else {
        // signal to be sent to parent on exit
        dst->esignal = esignal;
    }
}

void sigstack_init(stack_t *stack) {
    if (!stack) {
        return; // Invalid pointer
    }
    stack->ss_sp = NULL; // No stack allocated yet
    stack->ss_flags = SS_DISABLE;
    stack->ss_size = 0; // Size is zero initially
}

ksiginfo_t *ksiginfo_alloc(void) {
    ksiginfo_t *ksi = slab_alloc(&__ksiginfo_pool);
    if (!ksi) {
        return NULL; // Allocation failed
    }
    memset(ksi, 0, sizeof(ksiginfo_t));
    list_entry_init(&ksi->list_entry);
    ksi->sender = NULL; // No sender initially
    return ksi;
}

void sigacts_lock(sigacts_t *sa) { spin_lock(&sa->lock); }

void sigacts_unlock(sigacts_t *sa) { spin_unlock(&sa->lock); }

int sigacts_holding(sigacts_t *sa) { return spin_holding(&sa->lock); }

static void sigacts_assert_holding(sigacts_t *sa) {
    assert(sigacts_holding(sa), "sigacts lock not held");
}

void ksiginfo_free(ksiginfo_t *ksi) {
    if (ksi) {
        slab_free(ksi);
    }
}

// Clean the signal queue of the given process for the specified signal number.
// If signo is 0, all signals in the queue are cleaned.
// Ksiginfo being cleaned will be freed.
// The caller must hold sigacts->lock.
// Returns 0 on success, -1 on error.
int sigpending_empty(struct thread *p, int signo) {
    if (!p) {
        return -EINVAL; // Invalid process pointer
    }
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }

    if (signo == 0) {
        // Purge all signal queues (signals numbered 1..NSIG map to index
        // signo-1)
        for (int i = 1; i <= NSIG; i++) {
            ksiginfo_t *ksi = NULL;
            ksiginfo_t *tmp = NULL;
            sigpending_t *sq = &p->signal.sig_pending[i - 1];
            list_foreach_node_safe(&sq->queue, ksi, tmp, list_entry) {
                list_node_detach(ksi, list_entry);
                ksiginfo_free(ksi);
            }
        }
        p->signal.sig_pending_mask = 0;
        // Update sigpending flag after clearing all pending signals
        THREAD_CLEAR_SIGPENDING(p);
        return 0;
    }

    if (SIGBAD(signo)) {
        return -EINVAL; // Invalid signal number
    }

    ksiginfo_t *ksi = NULL;
    ksiginfo_t *tmp = NULL;
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    list_foreach_node_safe(&sq->queue, ksi, tmp, list_entry) {
        list_node_detach(ksi, list_entry);
        ksiginfo_free(ksi); // Free the ksiginfo after removing it
    }
    sigdelset(&p->signal.sig_pending_mask, signo);
    // Update sigpending flag after modifying pending mask (caller already holds
    // sigacts lock)
    recalc_sigpending_tsk(p);
    return 0;
}

static void __sig_reset_act_mask(sigacts_t *sa, int signo) {
    sigdelset(&sa->sa_sigterm, signo);
    sigdelset(&sa->sa_sigignore, signo);
    // sigdelset(&sa->sa_usercatch, signo);
    if (signo != SIGSTOP) {
        sigdelset(&sa->sa_sigstop, signo);
    }
    if (signo != SIGCONT) {
        sigdelset(&sa->sa_sigcont, signo);
    }
    // sigdelset(&sa->sa_sigcore, signo);
}

static int __sig_setdefault(sigacts_t *sa, int signo) {
    if (!sa || SIGBAD(signo)) {
        return -EINVAL; // Invalid signal number or signal actions
    }
    sig_defact defact = signo_default_action(signo);
    if (defact == SIG_ACT_INVALID) {
        return 0; // Ignore invalid signal number
    }

    __sig_reset_act_mask(sa, signo);
    switch (defact) {
    case SIG_ACT_IGN:
        sigaddset(&sa->sa_sigignore, signo);
        break;
    case SIG_ACT_CONT:
        sigaddset(&sa->sa_sigcont, signo);
        break;
    case SIG_ACT_STOP:
        sigaddset(&sa->sa_sigstop, signo);
        break;
    case SIG_ACT_TERM:
    // @TODO: For now handle SIG_ACT_CORE and SIG_ACT_INVALID by terminating the
    // process
    case SIG_ACT_CORE:
    case SIG_ACT_INVALID:
        sigaddset(&sa->sa_sigterm, signo);
        break;
    default:
        return -EINVAL;
    }

    sa->sa[signo].sa_handler = SIG_DFL;
    sa->sa[signo].sa_flags = 0;            // Reset flags
    sigemptyset(&(sa->sa[signo].sa_mask)); // Reset signal mask
    return 0;
}

// Initialize the first signal actions
sigacts_t *sigacts_init(void) {
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (!sa) {
        return NULL;
    }
    memset(sa, 0, sizeof(sigacts_t));
    sigemptyset(&sa->sa_sigterm);
    sigemptyset(&sa->sa_sigstop);
    sigemptyset(&sa->sa_sigcont);
    sigemptyset(&sa->sa_sigignore);
    spin_init(&sa->lock, "sigacts_lock");
    sa->refcount = 1;

    for (int i = 1; i <= NSIG; i++) {
        assert(__sig_setdefault(sa, i) == 0,
               "sigacts_init: failed to set default action for signal %d", i);
    }
    return sa;
}

sigacts_t *sigacts_dup(sigacts_t *psa, uint64 clone_flags) {
    if (!psa) {
        return NULL;
    }
    if (clone_flags & CLONE_SIGHAND) {
        // Share the signal actions
        // simply increase the reference count
        atomic_inc(&psa->refcount);
        return psa;
    }
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (sa) {
        sigacts_lock(psa);
        memmove(sa, psa, sizeof(sigacts_t));
        sigacts_unlock(psa);

        // CRITICAL: Reinitialize the lock and refcount after copying!
        // memmove copies the locked spinlock state, which would make
        // the new sigacts appear to be locked by someone else.
        spin_init(&sa->lock, "sigacts_lock");
        sa->refcount = 1;
    }
    return sa;
}

void sigacts_put(sigacts_t *sa) {
    if (sa != NULL && !atomic_dec_unless(&sa->refcount, 1)) {
        slab_free(sa);
    }
}

void signal_init(void) {
    slab_cache_init(&__sigacts_pool, "sigacts", sizeof(sigacts_t),
                    SLAB_FLAG_STATIC);
    slab_cache_init(&__ksiginfo_pool, "ksiginfo", sizeof(ksiginfo_t),
                    SLAB_FLAG_STATIC);
}

// Cap for number of queued ksiginfo entries per signal when SA_SIGINFO set.
#define MAX_SIGINFO_PER_SIGNAL 8

// Helper: count ksiginfo entries currently queued for a signal.
static int __siginfo_queue_len(struct thread *p, int signo) {
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    int n = 0;
    ksiginfo_t *pos = NULL;
    ksiginfo_t *tmp = NULL;
    list_foreach_node_safe(&sq->queue, pos, tmp, list_entry) { n++; }
    return n;
}

int __signal_send(struct thread *p, ksiginfo_t *info) {
    if (p == NULL || info == NULL) {
        return -EINVAL; // No threads available
    }
    if (SIGBAD(info->signo)) {
        return -EINVAL; // Invalid thread or signal number
    }

    // Check thread validity - use atomic load for lockless initial check
    enum thread_state pstate = __thread_state_get(p);
    if (pstate == THREAD_UNUSED || pstate == THREAD_ZOMBIE ||
        THREAD_KILLED(p)) {
        return -ESRCH; // Thread is not valid or already killed
    }

    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return -EINVAL; // No signal actions available
    }

    // Lock sigacts - this is the unified signal lock
    sigacts_lock(sa);

    // ignored signals are not sent
    if (sigismember(&sa->sa_sigignore, info->signo)) {
        sigacts_unlock(sa);
        return 0;
    }

    sigaction_t *act = &sa->sa[info->signo];
    if (act->sa_flags & SA_SIGINFO) {
        assert(info->signo != SIGKILL && info->signo != SIGSTOP,
               "signal_send: SA_SIGINFO set for SIGKILL or SIGSTOP");
        // Enforce per-signal queue cap. If cap reached, drop oldest entry.
        int qlen = __siginfo_queue_len(p, info->signo);
        if (qlen >= MAX_SIGINFO_PER_SIGNAL) {
            // Drop head (oldest) then append new info to tail.
            sigpending_t *sq = &p->signal.sig_pending[info->signo - 1];
            if (!LIST_IS_EMPTY(&sq->queue)) {
                ksiginfo_t *old =
                    LIST_FIRST_NODE(&sq->queue, ksiginfo_t, list_entry);
                if (old) {
                    list_entry_detach(&old->list_entry);
                    ksiginfo_free(old);
                }
            }
        }
        ksiginfo_t *ksi = ksiginfo_alloc();
        if (!ksi) {
            // Allocation failed: keep non-RT semantics by setting the pending
            // bit below, but skip queuing siginfo payload.
            goto after_enqueue; // fall through to set pending bit & notify
        }
        *ksi = *info; // Copy the signal info
        list_entry_init(&ksi->list_entry);
        // Add to pending queue
        list_node_push(&p->signal.sig_pending[info->signo - 1].queue, ksi,
                       list_entry);
    }

after_enqueue:
    // Always record the signal as pending (even for stop signals) to allow
    // later logic (e.g., mask changes) to notice it.
    sigaddset(&p->signal.sig_pending_mask, info->signo);

    // Update sigpending flag after adding to pending mask
    recalc_sigpending_tsk(p);

    bool is_stop = sigismember(&sa->sa_sigstop, info->signo) &&
                   !sigismember(&p->signal.sig_mask, info->signo);
    bool is_cont = sigismember(&sa->sa_sigcont, info->signo) &&
                   !sigismember(&p->signal.sig_mask, info->signo);
    bool is_term = sigismember(&sa->sa_sigterm, info->signo);
    sigset_t sigmask = p->signal.sig_mask;

    // Release sigacts lock before scheduler operations
    sigacts_unlock(sa);

    // For scheduler operations, we need tcb_lock to check/modify state
    if (is_stop) {
        // Stop signals: The thread will enter THREAD_STOPPED voluntarily when
        // it Process signals in handle_signal(). If it's currently sleeping in
        // an interruptible state, wake it up so it can process the stop signal.
        tcb_lock(p);
        pstate = __thread_state_get(p);
        if (THREAD_IS_INTERRUPTIBLE(pstate)) {
            // Wake up interruptible sleeper so it can handle the stop signal
            tcb_unlock(p);
            scheduler_wakeup(p);
        } else if (pstate == THREAD_RUNNING) {
            tcb_unlock(p);
            // Thread is running, send IPI so it handles the stop signal
            // promptly
            int target_cpu = smp_load_acquire(&p->sched_entity->cpu_id);
            if (target_cpu != cpuid()) {
                ipi_send_single(target_cpu, IPI_REASON_RESCHEDULE);
            } else {
                SET_NEEDS_RESCHED();
            }
        } else {
            tcb_unlock(p);
        }
        // If uninterruptible, the thread will handle the stop signal when it
        // wakes up
    }
    if (is_cont) {
        // Continue signal: Wake up the thread from THREAD_STOPPED state.
        scheduler_wakeup_stopped(p);
    }

    // If the action is to terminate the thread, set the killed flag
    if (is_term) {
        THREAD_SET_KILLED(p);
        if (THREAD_STOPPED(p)) {
            // If the thread is stopped, we need to wake it up.
            scheduler_wakeup_stopped(p);
        }
    }

    // Check if signal is pending (unmasked) and notify if thread is sleeping
    sigset_t pending_unmasked =
        smp_load_acquire(&p->signal.sig_pending_mask) & ~sigmask;
    if (pending_unmasked != 0) {
        tcb_lock(p);
        signal_notify(p);
        tcb_unlock(p);
    }

    return 0; // Signal sent successfully
}

int signal_send(int pid, ksiginfo_t *info) {
    struct thread *p = NULL;
    if (pid < 0 || info == NULL || SIGBAD(info->signo)) {
        return -EINVAL; // Invalid PID or signal number
    }
    rcu_read_lock();
    if (get_pid_thread(pid, &p) != 0) {
        rcu_read_unlock();
        return -ESRCH; // No thread found
    }
    if (p == NULL) {
        rcu_read_unlock();
        return -ESRCH; // No thread found
    }
    assert(p != NULL, "signal_send: thread is NULL");

    int ret;
    // If the target has a thread group and is the group leader (i.e., pid ==
    // tgid), deliver as a process-directed signal to the thread group's
    // shared_pending. This matches POSIX kill() semantics: kill(pid) sends to
    // the process.
    struct thread_group *tg = p->thread_group;
    if (tg != NULL && tg->tgid == pid) {
        ret = tg_signal_send(tg, info);
    } else {
        // Thread-directed signal (pid is a TID, not a TGID)
        ret = __signal_send(p, info);
    }
    rcu_read_unlock();
    return ret;
}

bool signal_pending(struct thread *p) {
    if (!p) {
        return false;
    }
    // Fast path: just check the flag
    return THREAD_SIGPENDING(p);
}

// Version that takes sigacts as parameter when caller already holds
// sigacts_lock Caller must hold tcb_lock but NOT sigacts_lock (we acquire it
// here)
bool signal_pending_locked(struct thread *p, sigacts_t *sa) {
    if (!p || !sa) {
        return false;
    }
    // Fast path: just check the flag
    return THREAD_SIGPENDING(p);
}

int signal_notify(struct thread *p) {
    if (!p) {
        return -EINVAL;
    }
    proc_assert_holding(p);
    if (THREAD_AWOKEN(p)) {
        return 0;
    }
    if (!THREAD_SLEEPING(p)) {
        return -EAGAIN; // Process is not sleeping
    }
    if (__thread_state_get(p) == THREAD_INTERRUPTIBLE) {
        // Must follow wakeup locking protocol:
        // - Release tcb_lock (must NOT be held during wakeup)
        // - Call wakeup (no pi_lock needed - rq_lock serializes)
        // - Reacquire tcb_lock
        tcb_unlock(p);
        scheduler_wakeup_interruptible(p);
        tcb_lock(p);
        return 0; // Success
    }
    return -EAGAIN; // Thread not in interruptible state
}

bool signal_terminated(struct thread *p) {
    if (!p) {
        return 0;
    }
    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return false; // No sigacts means no termination signals
    }
    sigacts_lock(sa);
    sigset_t masked = p->signal.sig_pending_mask & ~p->signal.sig_mask;
    bool terminated = (masked & sa->sa_sigterm) != 0;
    sigacts_unlock(sa);
    return terminated;
}

bool signal_test_clear_stopped(struct thread *p) {
    if (!p) {
        return 0;
    }
    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return THREAD_STOPPED(
            p); // No sigacts, just return current stopped state
    }

    sigacts_lock(sa);
    sigset_t sigmask = p->signal.sig_mask;
    sigset_t sigstop_mask = sa->sa_sigstop;
    sigset_t sigcont_mask = sa->sa_sigcont;
    sigset_t masked = p->signal.sig_pending_mask & ~sigmask;
    sigset_t pending_stopped = masked & sigstop_mask;
    sigset_t pending_cont = masked & sigcont_mask;

    if (pending_cont) {
        // A continue-category signal is pending. Determine if any of them
        // have user handlers installed. We resume the thread in all cases.
        bool user_handler = false;
        for (int signo = 1; signo <= NSIG; signo++) {
            if (sigismember(&sigcont_mask, signo) > 0 &&
                sigismember(&pending_cont, signo) > 0) {
                sigaction_t *act = &sa->sa[signo];
                if (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
                    user_handler = true;
                    break;
                }
            }
        }
        // Clear all pending stop signals (they are canceled by any continue)
        p->signal.sig_pending_mask &= ~sigstop_mask;
        if (!user_handler) {
            // Default action: consume the continue signals here so they are not
            // delivered.
            p->signal.sig_pending_mask &= ~pending_cont;
        }
        // Recalc after modifying pending mask
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);
        return 0; // Do not request stop.
    }

    if (pending_stopped) {
        // Consume all pending stop signals (they stop the thread) and request
        // STOPPED state.
        p->signal.sig_pending_mask &= ~pending_stopped;
        // Recalc after modifying pending mask
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);
        return 1; // Caller will transition to THREAD_STOPPED.
    }

    sigacts_unlock(sa);
    // No new stop/cont signals; indicate whether thread is already stopped.
    return THREAD_STOPPED(p);
}

int signal_restore(struct thread *p, ucontext_t *context) {
    if (p == NULL || context == NULL) {
        return -EINVAL; // Invalid thread or context
    }

    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return -EINVAL;
    }
    sigacts_lock(sa);

    p->signal.sig_stack = context->uc_stack;
    p->signal.sig_ucontext = (uint64)context->uc_link;

    if (p->signal.sig_ucontext == 0) {
        p->signal.sig_mask = p->signal.sig_saved_mask; // Reset to original mask
    } else {
        p->signal.sig_mask = context->uc_sigmask;
        p->signal.sig_mask |=
            p->signal.sig_saved_mask; // Update the signal mask
    }

    sigdelset(&p->signal.sig_mask, SIGKILL);
    sigdelset(&p->signal.sig_mask, SIGSTOP);
    sigdelset(&sa->sa_sigignore, SIGKILL);
    sigdelset(&sa->sa_sigignore, SIGSTOP);
    // Recalc sigpending after changing blocked mask
    recalc_sigpending_tsk(p);
    sigacts_unlock(sa);

    return 0; // Success
}

int sigaction(int signum, struct sigaction *act, struct sigaction *oldact) {
    if (signum < 1 || signum > NSIG) {
        return -EINVAL; // Invalid signal number
    }
    if (signum == SIGKILL || signum == SIGSTOP) {
        return -EINVAL; // SIGKILL and SIGSTOP cannot be caught or ignored
    }

    struct thread *p = current;
    assert(p != NULL, "sys_sigaction: current returned NULL");

    if (!THREAD_USER_SPACE(p)) {
        return -EPERM; // sigaction can only be called in user space
    }

    sigacts_t *sa = p->sigacts;

    // Use only sigacts_lock for signal state protection
    sigacts_lock(sa);

    if (oldact) {
        *oldact = sa->sa[signum];
    }

    if (act) {
        bool clear_pending = false;
        __sig_reset_act_mask(sa, signum);

        if (act->sa_handler == SIG_IGN) {
            sigaddset(&sa->sa_sigignore, signum);
            sa->sa[signum] = *act;
            clear_pending = true;
        } else if (act->sa_handler == SIG_DFL) {
            if (__sig_setdefault(sa, signum) != 0) {
                sigacts_unlock(sa);
                return -EINVAL; // Failed to set default action
            }

            // For default-ignored signals, pending instances are discarded.
            if (sigismember(&sa->sa_sigignore, signum) > 0) {
                clear_pending = true;
            }

            // After changing to SIG_DFL, check if any pending signals
            // are now termination signals and update THREAD_KILLED accordingly
            sigset_t pending_term = p->signal.sig_pending_mask &
                                    sa->sa_sigterm & ~p->signal.sig_mask;
            if (pending_term != 0) {
                THREAD_SET_KILLED(p);
            }
        } else {
            // User-installed handler: preserve user-supplied disposition data.
            sa->sa[signum] = *act;
            sigdelset(&sa->sa[signum].sa_mask, SIGKILL); // Cannot be blocked
            sigdelset(&sa->sa[signum].sa_mask, SIGSTOP); // Cannot be blocked
        }

        if (clear_pending) {
            // Only ignored dispositions consume pending signals.
            if (sigpending_empty(p, signum) != 0) {
                sigacts_unlock(sa);
                return -EINVAL; // Failed to clear pending signals
            }
            if (p->thread_group != NULL) {
                tg_sigpending_empty(p->thread_group, signum);
            }
        }
    }

    sigacts_unlock(sa);
    return 0; // Success
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (set != NULL &&
        how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
        return -EINVAL; // Invalid operation
    }
    struct thread *p = current;
    assert(p != NULL, "sigprocmask: current returned NULL");

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigprocmask: sigacts is NULL");

    // Use only sigacts_lock for signal state protection
    sigacts_lock(sa);
    if (oldset) {
        *oldset = p->signal.sig_mask; // Save the old mask
    }

    // POSIX: if set is NULL, do not change mask (how is ignored).
    if (set != NULL) {
        if (how == SIG_SETMASK) {
            p->signal.sig_saved_mask = *set; // Set the new mask
            p->signal.sig_mask = *set;       // Update the signal mask
        } else if (how == SIG_BLOCK) {
            p->signal.sig_saved_mask |= *set; // Block the signals in the set
            p->signal.sig_mask |= *set;       // Update the signal mask
        } else if (how == SIG_UNBLOCK) {
            p->signal.sig_saved_mask &= ~(*set); // Unblock the signals in set
            p->signal.sig_mask &= ~(*set);       // Update the signal mask
        }
    }

    // Mandatory signals cannot be blocked
    sigdelset(&p->signal.sig_saved_mask, SIGKILL);
    sigdelset(&p->signal.sig_saved_mask, SIGSTOP);
    sigdelset(&p->signal.sig_mask, SIGKILL);
    sigdelset(&p->signal.sig_mask, SIGSTOP);

    // Recalc sigpending flag after changing blocked mask
    recalc_sigpending_tsk(p);

    // Check if newly unmasked signals are pending
    sigset_t pending_unmasked =
        p->signal.sig_pending_mask & ~p->signal.sig_mask;

    // If newly unmasked termination signals are pending, set THREAD_KILLED
    sigset_t pending_term = pending_unmasked & sa->sa_sigterm;
    if (pending_term != 0) {
        THREAD_SET_KILLED(p);
    }
    sigacts_unlock(sa);

    // If newly unmasked signals are pending and thread is sleeping, wake it.
    // Need tcb_lock for signal_notify (which checks thread state)
    if (pending_unmasked != 0) {
        tcb_lock(p);
        (void)signal_notify(p);
        tcb_unlock(p);
    }
    return 0; // Success
}

int sigpending(struct thread *p, sigset_t *set) {
    if (!set) {
        return -EINVAL; // Invalid set pointer
    }
    assert(p != NULL, "sigpending: current returned NULL");

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigpending: sigacts is NULL");

    // Use only sigacts_lock - it protects both masks
    sigacts_lock(sa);
    sigset_t mask = p->signal.sig_mask;
    *set = mask & p->signal.sig_pending_mask; // Get the pending signals
    sigacts_unlock(sa);

    return 0; // Success
}

int sigreturn(void) {
    struct thread *p = current;
    assert(p != NULL, "sys_sigreturn: current returned NULL");

    if (!THREAD_USER_SPACE(p)) {
        return -EPERM; // sigreturn can only be called in user space
    }

    sigacts_t *sa = p->sigacts;
    sigacts_lock(sa);
    if (p->signal.sig_ucontext == 0) {
        sigacts_unlock(sa);
        return -EINVAL; // No signal trap frame to restore
    }
    sigacts_unlock(sa);

    // Call restore_sigframe without holding sigacts_lock since it calls
    // vm_copyin which needs vm_rlock (sleep lock)
    ucontext_t uc = {0};
    if (restore_sigframe(p, &uc) != 0) {
        // @TODO:
        exit(-1); // Restore failed, exit the thread
    }

    // signal_restore now acquires sigacts_lock internally
    assert(signal_restore(p, &uc) == 0, "sigreturn: signal_restore failed");

    return 0; // Success
}

// Dequeue signal - caller provides the sigaction copy.
// Caller must hold sigacts->lock.
static ksiginfo_t *__dequeue_signal_update_pending_nolock(struct thread *p,
                                                          int signo,
                                                          sigaction_t *act) {
    if (p == NULL || act == NULL) {
        return ERR_PTR(-EINVAL);
    }
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }

    if (SIGBAD(signo)) {
        return ERR_PTR(-EINVAL);
    }

    assert(act->sa_handler != SIG_IGN,
           "__dequeue_signal_update_pending_nolock: signal handler is SIG_IGN");
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    if ((act->sa_flags & SA_SIGINFO) == 0) {
        assert(LIST_IS_EMPTY(&sq->queue),
               "sig_pending is not empty for a non-SA_SIGINFO signal");
        sigdelset(&p->signal.sig_pending_mask, signo);
        // Caller should call recalc_sigpending while still holding lock
        return NULL; // No signal info to return
    }

    // Pop exactly one ksiginfo (FIFO order: head of list).
    if (LIST_IS_EMPTY(&sq->queue)) {
        // Queue empty but bit set implies inconsistency; clear defensively.
        sigdelset(&p->signal.sig_pending_mask, signo);
        return NULL;
    }
    ksiginfo_t *info = LIST_FIRST_NODE(&sq->queue, ksiginfo_t, list_entry);
    assert(info->signo == signo,
           "__dequeue_signal_update_pending_nolock: pos->signo != signo");
    list_entry_detach(&info->list_entry);
    // If queue now empty, clear pending bit; else leave it set for further
    // delivery.
    if (LIST_IS_EMPTY(&sq->queue)) {
        sigdelset(&p->signal.sig_pending_mask, signo);
    }
    return info;
}

static int __deliver_signal(struct thread *p, int signo, ksiginfo_t *info,
                            sigaction_t *sa, bool *repeat) {
    // NOTE: This function is called WITHOUT tcb_lock held to allow
    // push_sigframe to acquire vm_wlock (sleep lock). The caller must
    // ensure the signal state (sa, info) was captured while holding the lock.
    if (repeat) {
        *repeat = false; // Default to not repeat
    }

    if (p == NULL || sa == NULL) {
        return -1;
    }

    if (sa->sa_handler == SIG_IGN) {
        return 0; // Signal is ignored
    }

    if (sa->sa_flags & SA_SIGINFO) {
        assert(info != NULL, "__deliver_signal: SA_SIGINFO but info is NULL");
    }

    // Other than SIG_IGN and SIG_CONT, all signal handlers must be placed
    // beyond the first page of the address space.
    if ((uint64)sa->sa_handler < PAGE_SIZE) {
        printf("__deliver_signal: invalid signal handler address %p for signal "
               "%d\n",
               sa->sa_handler, signo);
        tcb_lock(p);
        THREAD_SET_KILLED(p);
        tcb_unlock(p);
        return 0;
    }

    int ret = 0;
    if (THREAD_USER_SPACE(current)) {
        // If the thread has user space, push the signal to its user stack
        // This may call vm_try_growstack which needs vm_wlock (sleep lock)
        ret = push_sigframe(p, signo, sa, info);
    }

    // Acquire sigacts_lock to update signal masks
    sigacts_t *sigacts = p->sigacts;
    sigacts_lock(sigacts);
    if ((sa->sa_flags & SA_NODEFER) == 0) {
        sigaddset(&p->signal.sig_mask, signo);
    }

    p->signal.sig_mask |= sa->sa_mask; // Block the signal during delivery
    sigdelset(&p->signal.sig_mask, SIGKILL);
    sigdelset(&p->signal.sig_mask, SIGSTOP);

    // Recalc sigpending flag after blocking signals
    recalc_sigpending_tsk(p);

    if ((sa->sa_flags & SA_RESETHAND) != 0) {
        // Reset the signal handler to default action
        assert(__sig_setdefault(sigacts, signo) == 0,
               "__deliver_signal: __sig_setdefault failed");
    }
    sigacts_unlock(sigacts);

    return ret;
}

void handle_signal(void) {
    struct thread *p = current;
    assert(p != NULL, "handle_signal: current returned NULL");
    if (p->sigacts == NULL) {
        return; // No signal actions defined
    }
    sigacts_t *sa = p->sigacts;
    struct thread_group *tg = p->thread_group;

    for (;;) {
        // Gather all signal info with sigacts_lock - this protects all signal
        // state
        sigacts_lock(sa);
        sigset_t sigmask = p->signal.sig_mask;
        sigset_t sigterm = sa->sa_sigterm;
        sigset_t sigstop = sa->sa_sigstop;
        sigset_t sigcont = sa->sa_sigcont;
        sigset_t pending = p->signal.sig_pending_mask;

        // Merge in shared pending signals from thread group
        sigset_t shared_pending = 0;
        if (tg != NULL) {
            shared_pending =
                smp_load_acquire(&tg->shared_pending.sig_pending_mask);
            pending |= shared_pending;
        }

        sigset_t masked = pending & ~sigmask;

        // Check termination
        if ((masked & sigterm) || THREAD_KILLED(p)) {
            THREAD_SET_KILLED(p);
            sigacts_unlock(sa);
            break;
        }

        // Check stop/continue
        sigset_t pending_cont = masked & sigcont;
        sigset_t pending_stop = masked & sigstop;

        if (pending_cont) {
            // Continue cancels stop - clear stop signals from both
            // per-thread and shared pending
            p->signal.sig_pending_mask &= ~sigstop;
            if (tg != NULL) {
                tg->shared_pending.sig_pending_mask &= ~sigstop;
            }

            // Check if any pending SIGCONT has a user handler
            bool user_handler = false;
            for (int signo = 1; signo <= NSIG; signo++) {
                if (sigismember(&sigcont, signo) > 0 &&
                    sigismember(&pending_cont, signo) > 0) {
                    sigaction_t *act = &sa->sa[signo];
                    if (act->sa_handler != SIG_DFL &&
                        act->sa_handler != SIG_IGN) {
                        user_handler = true;
                        break;
                    }
                }
            }

            if (!user_handler) {
                // Default action: consume the continue signals here
                // from both per-thread and shared pending
                p->signal.sig_pending_mask &= ~pending_cont;
                if (tg != NULL) {
                    tg->shared_pending.sig_pending_mask &= ~pending_cont;
                }
                // Recalc sigpending flag after modifying pending mask
                recalc_sigpending_tsk(p);
                sigacts_unlock(sa);
                continue; // No handler to call, loop back
            }
            // If user_handler is true, leave pending_cont bits set
            // and fall through to deliver the signal to the user handler
            // (don't call continue - let the delivery code below handle it)
        } else if (pending_stop) {
            // Clear stop signals from both per-thread and shared pending,
            // then enter stopped state
            p->signal.sig_pending_mask &= ~pending_stop;
            if (tg != NULL) {
                tg->shared_pending.sig_pending_mask &= ~pending_stop;
            }
            // Recalc sigpending flag after modifying pending mask
            recalc_sigpending_tsk(p);
            sigacts_unlock(sa);

            // Use tcb_lock for state transition
            tcb_lock(p);
            __thread_state_set(p, THREAD_STOPPED);
            tcb_unlock(p);
            scheduler_yield();
            continue; // Re-check after wakeup
        }

        // Find first deliverable signal
        int signo = (masked != 0) ? bits_ffsg(masked) : 0;
        if (signo == 0 || signo > NSIG) {
            sigacts_unlock(sa);
            break; // No pending signals
        }

        // Skip stop signals (they were handled above and consumed)
        // Note: SIGCONT with user handler was NOT consumed above, so don't skip
        // it
        if (sigismember(&sigstop, signo)) {
            sigacts_unlock(sa);
            continue;
        }

        // Copy sigaction and dequeue while holding sigacts_lock
        sigaction_t sa_copy = sa->sa[signo];

        // Determine if the signal is from per-thread pending or shared pending,
        // and dequeue from the appropriate queue.
        bool from_shared = false;
        if (sigismember(&p->signal.sig_pending_mask, signo)) {
            // Per-thread pending — dequeue from per-thread queue
            from_shared = false;
        } else if (tg != NULL &&
                   sigismember(&tg->shared_pending.sig_pending_mask, signo)) {
            // Shared pending from thread group
            from_shared = true;
        } else {
            sigacts_unlock(sa);
            continue; // Signal was consumed elsewhere, try again
        }

        bool repeat = false;
        ksiginfo_t *info = NULL;
        if (from_shared && tg != NULL) {
            // Dequeue from thread group's shared pending.
            // sigacts lock is already held (which serializes shared_pending
            // access since all group threads share the same sigacts via
            // CLONE_SIGHAND). pid_rlock is NOT needed here because the
            // shared_pending queues are protected by sigacts->lock.
            info = tg_dequeue_signal(tg, signo);
        } else {
            info = __dequeue_signal_update_pending_nolock(p, signo, &sa_copy);
            assert(!IS_ERR(info),
                   "handle_signal: __dequeue_signal_update_pending failed");
        }

        // Recalc sigpending after dequeue modified the pending mask
        recalc_sigpending_tsk(p);

        // Release sigacts_lock before calling __deliver_signal, which may need
        // to acquire vm_wlock (sleep lock) via push_sigframe/vm_try_growstack
        sigacts_unlock(sa);

        assert(__deliver_signal(p, signo, info, &sa_copy, &repeat) == 0,
               "handle_signal: __deliver_signal failed");

        // Check repeat condition with sigacts_lock only
        if (sa_copy.sa_flags & SA_SIGINFO) {
            sigacts_lock(sa);
            bool unmasked = sigismember(&p->signal.sig_mask, signo) == 0;
            bool still_pending =
                sigismember(&p->signal.sig_pending_mask, signo) > 0;
            // Also check thread group shared_pending for more queued entries
            if (!still_pending && tg != NULL) {
                still_pending =
                    sigismember(&tg->shared_pending.sig_pending_mask, signo) >
                    0;
            }
            sigacts_unlock(sa);

            if (unmasked && still_pending) {
                repeat = true;
            }
        }

        if (info) {
            ksiginfo_free(info);
        }

        if (!repeat) {
            break;
        }
    }

    // Recalculate SIGPENDING after delivering/consuming all signals.
    // recalc_sigpending_tsk (used inside the loop) can only SET the flag;
    // we need recalc_sigpending (which checks shared_pending too) to CLEAR
    // it when no unmasked signals remain.
    recalc_sigpending();

    if (THREAD_KILLED(p)) {
        exit(-1);
    }
}

// Kill the threads with the given pid (process-directed signal).
// When the target has a thread group, this sends to the group (POSIX kill()).
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid, int signum) {
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);

    return signal_send(pid, &info);
}

// Kill the given thread directly (thread-directed signal).
// Instead of looking up by pid, directly send signal to the given thread.
int kill_thread(struct thread *p, int signum) {
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);

    rcu_read_lock();
    int ret = __signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

// Send a signal to a specific thread within a specific thread group (tgkill).
// This is the POSIX tgkill(tgid, tid, sig) function.
// Returns 0 on success, negative errno on failure.
int tgkill(int tgid, int tid, int signum) {
    if (tgid < 0 || tid < 0 || SIGBAD(signum)) {
        return -EINVAL;
    }
    struct thread *p = NULL;
    rcu_read_lock();
    if (get_pid_thread(tid, &p) != 0 || p == NULL) {
        rcu_read_unlock();
        return -ESRCH;
    }
    // Verify the thread belongs to the specified thread group
    if (p->thread_group == NULL || p->thread_group->tgid != tgid) {
        rcu_read_unlock();
        return -ESRCH;
    }
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);
    int ret = __signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

// Send a signal to a specific thread by TID (tkill).
// This is the POSIX tkill(tid, sig) function.
int tkill(int tid, int signum) {
    if (tid < 0 || SIGBAD(signum)) {
        return -EINVAL;
    }
    struct thread *p = NULL;
    rcu_read_lock();
    if (get_pid_thread(tid, &p) != 0 || p == NULL) {
        rcu_read_unlock();
        return -ESRCH;
    }
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);
    int ret = __signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

// Check if thread should be terminated.
// This only checks the THREAD_KILLED flag which is set atomically by
// __signal_send when a termination signal is delivered. No locks needed.
int killed(struct thread *p) {
    if (!p) {
        return 0;
    }
    return THREAD_KILLED(p);
}

/**
 * signal_send_to_tgroup - send a process-directed signal to a thread group
 * @tgid: thread group ID (process ID)
 * @info: signal information
 *
 * Delivers the signal to any suitable thread in the thread group.
 * Used by kill_from_kernel() when targeting a process ID.
 *
 * Returns 0 on success, or negative errno on failure.
 */
static int signal_send_to_tgroup(int tgid, ksiginfo_t *info) {
    struct thread *leader = NULL;

    rcu_read_lock();

    if (get_pid_thread(tgid, &leader) != 0 || leader == NULL) {
        rcu_read_unlock();
        return -ESRCH;
    }

    // If pid doesn't refer to a group leader, find the real one
    struct thread_group *tg = leader->thread_group;
    if (tg != NULL && tg->tgid == tgid) {
        // Good — use tg_signal_send which already handles shared_pending
        int ret = tg_signal_send(tg, info);
        rcu_read_unlock();
        return ret;
    }

    // Fallback: tgid matched a non-leader TID, or thread has no group.
    // Navigate to the real leader if possible.
    if (tg != NULL && tg->group_leader != NULL) {
        int ret = tg_signal_send(tg, info);
        rcu_read_unlock();
        return ret;
    }

    // No thread group — send directly to the thread
    int ret = __signal_send(leader, info);
    rcu_read_unlock();
    return ret;
}

/**
 * kill_from_kernel - send a signal from kernel context (no current thread)
 * @pid: target process/thread group ID
 * @signum: signal number to send
 *
 * Used by interrupt handlers (e.g., console ^C) where there is no
 * user-space caller. Sets sender to NULL/pid 0.
 *
 * Returns 0 on success, negative errno on failure.
 */
int kill_from_kernel(int pid, int signum) {
    if (SIGBAD(signum) && signum != 0) {
        return -EINVAL;
    }

    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = NULL; // Sent from kernel
    info.info.si_pid = 0;

    // Signal 0 is used to check if the process exists
    if (signum == 0) {
        struct thread *p = NULL;
        rcu_read_lock();
        if (get_pid_thread(pid, &p) != 0 || p == NULL) {
            rcu_read_unlock();
            return -ESRCH;
        }
        rcu_read_unlock();
        return 0;
    }

    return signal_send_to_tgroup(pid, &info);
}

/**
 * kill_proc - send a signal directly to a thread/thread group
 * @p: target thread
 * @signum: signal number to send
 *
 * For thread groups, selects a suitable thread to receive the signal.
 * Used internally (e.g., exit.c sending SIGCHLD to parent).
 *
 * Returns 0 on success, negative errno on failure.
 */
int kill_proc(struct thread *p, int signum) {
    if (!p || SIGBAD(signum)) {
        return -EINVAL;
    }

    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = current ? thread_tgid(current) : 0;

    rcu_read_lock();

    struct thread_group *tg = p->thread_group;
    if (tg != NULL) {
        // Use tg_signal_send for proper shared_pending handling
        int ret = tg_signal_send(tg, &info);
        rcu_read_unlock();
        return ret;
    }

    // No thread group — send directly
    int ret = __signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

/**
 * sigsuspend - temporarily replace signal mask and wait for a signal
 * @mask: the temporary signal mask to use while waiting
 *
 * Atomically:
 * 1. Saves the current signal mask
 * 2. Sets the signal mask to @mask
 * 3. Suspends until a signal is caught
 * 4. Restores the original signal mask
 *
 * Returns -EINTR when a signal is caught (always returns error).
 * This is POSIX/pthread-compatible behavior.
 */
int sigsuspend(const sigset_t *mask) {
    struct thread *p = current;
    if (!p || !mask) {
        return -EINVAL;
    }

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigsuspend: sigacts is NULL");

    sigacts_lock(sa);

    // Save the current mask and set the temporary one
    sigset_t saved = p->signal.sig_mask;
    p->signal.sig_saved_mask = p->signal.sig_mask; // Also save original
    p->signal.sig_mask = *mask;
    // SIGKILL and SIGSTOP cannot be blocked
    sigdelset(&p->signal.sig_mask, SIGKILL);
    sigdelset(&p->signal.sig_mask, SIGSTOP);

    // Check if there are already pending signals now unblocked
    sigset_t pending_unmasked =
        p->signal.sig_pending_mask & ~p->signal.sig_mask;
    // Also check thread group shared pending
    struct thread_group *tg = p->thread_group;
    if (tg != NULL) {
        pending_unmasked |=
            tg->shared_pending.sig_pending_mask & ~p->signal.sig_mask;
    }
    if (pending_unmasked != 0) {
        // Signals already pending and unblocked — restore and return
        p->signal.sig_mask = saved;
        p->signal.sig_saved_mask = saved;
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);
        return -EINTR;
    }

    recalc_sigpending_tsk(p);
    sigacts_unlock(sa);

    // Sleep until a signal arrives
    __thread_state_set(p, THREAD_INTERRUPTIBLE);
    scheduler_yield();

    // Do NOT restore the mask here. The temporary mask stays active so
    // handle_signal() (called from usertrapret) can deliver the signal.
    // push_sigframe() saves the current (temporary) mask in uc_sigmask,
    // and signal_restore() (sigreturn) restores from sig_saved_mask
    // when the outermost frame is popped.

    return -EINTR; // sigsuspend always returns -EINTR
}

/**
 * sigwait - wait for a signal from a specified set
 * @set: the set of signals to wait for
 * @sig: pointer to store the signal number that was delivered
 *
 * Unlike sigsuspend, sigwait removes the signal from pending and returns
 * the signal number without invoking the signal handler.
 *
 * Returns 0 on success, or negative errno on failure.
 * This is POSIX/pthread-compatible behavior.
 */
int sigwait(const sigset_t *set, int *sig) {
    struct thread *p = current;
    if (!p || !set || !sig) {
        return -EINVAL;
    }

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigwait: sigacts is NULL");

    while (1) {
        sigacts_lock(sa);

        // Check for pending signals in the wait set (per-thread)
        sigset_t pending_wanted = p->signal.sig_pending_mask & *set;
        // Also check thread group shared pending
        struct thread_group *tg = p->thread_group;
        if (tg != NULL) {
            pending_wanted |= tg->shared_pending.sig_pending_mask & *set;
        }

        if (pending_wanted != 0) {
            // Find the first pending signal in the set
            for (int signo = 1; signo <= NSIG; signo++) {
                if (!sigismember(&pending_wanted, signo)) {
                    continue;
                }

                // Try per-thread pending first
                if (sigismember(&p->signal.sig_pending_mask, signo)) {
                    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
                    if (!LIST_IS_EMPTY(&sq->queue)) {
                        ksiginfo_t *ksi =
                            LIST_FIRST_NODE(&sq->queue, ksiginfo_t, list_entry);
                        if (ksi) {
                            list_entry_detach(&ksi->list_entry);
                            ksiginfo_free(ksi);
                        }
                    }
                    if (LIST_IS_EMPTY(&sq->queue)) {
                        sigdelset(&p->signal.sig_pending_mask, signo);
                    }
                } else if (tg != NULL &&
                           sigismember(&tg->shared_pending.sig_pending_mask,
                                       signo)) {
                    // Dequeue from shared pending
                    ksiginfo_t *ksi = tg_dequeue_signal(tg, signo);
                    if (ksi) {
                        ksiginfo_free(ksi);
                    }
                }

                *sig = signo;
                recalc_sigpending_tsk(p);
                sigacts_unlock(sa);
                return 0;
            }
        }

        // No signal yet — temporarily unblock the waited signals so that
        // __signal_send can see them as unblocked and call signal_notify
        // to wake us. Without this, blocked signals would not trigger
        // a wakeup and we'd sleep forever.
        sigset_t saved_mask = p->signal.sig_mask;
        p->signal.sig_mask &= ~(*set); // Temporarily unblock waited signals
        sigdelset(&p->signal.sig_mask, SIGKILL);
        sigdelset(&p->signal.sig_mask, SIGSTOP);
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);

        // Sleep until one arrives
        __thread_state_set(p, THREAD_INTERRUPTIBLE);
        scheduler_yield();

        // Restore the original mask before re-checking
        sigacts_lock(sa);
        p->signal.sig_mask = saved_mask;
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);

        // Check if we were killed
        if (killed(p)) {
            return -EINTR;
        }
    }
}
