#include "proc/thread.h"
#include "proc/thread_group.h"
#include "defs.h"
#include "kqueue_types.h"
#include "hlist.h"
#include "list.h"
#include <mm/memlayout.h>
#include <mm/page.h>
#include "param.h"
#include "printf.h"
#include "proc/tq.h"
#include "proc/rq.h"
#include "proc/sched.h"
#include "proc_private.h"
#include "lock/rcu.h"
#include "lock/spinlock.h"
#include "riscv.h"
#include "signal.h"
#include <mm/slab.h>
#include "string.h"
#include "accounting.h"
#include "vfs/fs.h"
#include "types.h"
#include "vfs/file.h"
#include "vfs/fs.h"
#include <mm/vm.h>
#include "errno.h"
#include "tty/session.h"
#include "tty/tty.h"
#include "proc/pgroup.h"

// Wake the vfork parent when child exits or execs.
// The vfork parent is blocked in UNINTERRUPTIBLE state waiting for us.
// After waking, clear the vfork_parent pointer so we don't wake twice.
// Assuming no other thread will change p->vfork_parent after this point, so no
// need to hold p->lock.
void vfork_done(struct thread *p) {
    struct thread *vfork_parent = p->vfork_parent;
    p->vfork_parent = NULL; // Clear so we don't wake again on exit after exec

    if (vfork_parent != NULL) {
        scheduler_wakeup(vfork_parent);
    }
}

// Pass p's abandoned children to init.
// Caller must not hold pid_wlock (acquired internally).
// Resets each child's exit signal to SIGCHLD before reparenting.
void reparent(struct thread *p) {
    rcu_read_lock();
    struct thread *initproc = __proctab_get_initproc();
    struct thread *parent = p->parent;
    struct thread *child, *tmp;
    bool zombie_found = false;

    assert(initproc != NULL, "reparent: initproc is NULL");
    assert(p != initproc, "reparent: p is init process");

    // Try to acquire both locks without blocking
    pid_wlock();

    list_foreach_node_safe(&p->children, child, tmp, siblings) {
        // make sure the child isn't still in exit() or arch_context_switch().
        child->signal.esignal = SIGCHLD; // reset to default exit signal
        if (__thread_state_get(child) == THREAD_ZOMBIE) {
            zombie_found = true;
        }
        detach_child(p, child);
        attach_child(initproc, child);
    }

    pid_wunlock();
    rcu_read_unlock();

    if (zombie_found) {
        if (initproc != NULL && initproc != parent && initproc != p &&
            p->signal.esignal > 0) {
            kill_thread(initproc, p->signal.esignal);
        }
        scheduler_wakeup_interruptible(initproc);
    }
}

static bool zombie_child_is_reapable(struct thread *child) {
    if (!THREAD_ZOMBIE(child))
        return false;

    struct thread_group *tg = child->thread_group;
    if (tg == NULL || !thread_is_group_leader(child))
        return true;

    return __atomic_load_n(&tg->live_threads, __ATOMIC_ACQUIRE) <= 0;
}

// Exit the current thread.  Does not return.
// An exited thread remains in the zombie state
// until its parent calls wait().
//
// Thread group semantics (CLONE_THREAD):
// - Non-leader threads: remove from thread group, detach from proc table,
//   and self-reap (no zombie visible to parent).
// - Group leader: becomes zombie only when the last thread in the group exits.
//   While other threads are still alive, the leader stays in ZOMBIE state
//   but is not yet visible to parent's wait() (the leader's zombie is
//   "delayed" until the entire group exits).
// - Single-threaded processes (no CLONE_THREAD): behave as before.
void exit(int status) {
    struct thread *p = current;
    assert(p != __proctab_get_initproc(), "init exiting");

    ACCT_INC(p->thread_group, sched_exits);

    // Release per-CPU FPU ownership early — this thread will never
    // return to user space, so no point keeping it as the HW FP owner.
    // Save HW FP state first so gdbstub_exit_stop() can read FP registers.
    // No need to scan all CPUs — seq-based ownership means stale entries
    // on other CPUs are harmless (a new thread with the same TID will
    // get a fresh fpu_seq that won't match the stale per-CPU seq).
    if (mycpu()->fpu_owner_tid == p->pid &&
        p->fpu_seq == mycpu()->fpu_seq) {
        if (THREAD_FPU_USED(p) && p->fpu_state != NULL)
            fpu_save_state(p->fpu_state);
        mycpu()->fpu_owner_tid = 0;
    }

    // If this thread is in a thread group undergoing group exit,
    // use the group exit code.
    struct thread_group *tg = p->thread_group;
    if (tg != NULL && thread_is_group_leader(p) &&
        !thread_group_exiting(tg) &&
        __atomic_load_n(&tg->live_threads, __ATOMIC_ACQUIRE) > 1) {
        thread_group_exit(p, status);
    }
    if (tg != NULL && thread_group_exiting(tg) && tg->group_exit_task != p) {
        status = tg->group_exit_code;
    }

    // Notify GDB stub before tearing down resources (VM, fds, etc.).
    // For single-threaded or group leader: this is a process exit.
    // For non-leader clone threads: this is a thread exit.
    {
        extern void gdbstub_exit_stop(struct thread *, int, int);
        int is_leader = thread_is_group_leader(p) || tg == NULL;
        int is_last = is_leader || (tg != NULL &&
                      __atomic_load_n(&tg->live_threads,
                                      __ATOMIC_RELAXED) <= 1);
        gdbstub_exit_stop(p, status, is_last);
    }

    // Wake vfork parent FIRST - they're sharing our address space
    // and need to resume before we tear anything down
    vfork_done(p);

    // CLONE_CHILD_CLEARTID: zero the TID word at the stored address and
    // issue a futex wake so pthread_join can detect thread exit.
    // Must be done while VM is still valid.
    if (p->clear_child_tid != 0) {
        int zero = 0;
        vm_copyout(p->vm, p->clear_child_tid, (char *)&zero, sizeof(zero));
        futex_wake_addr(p->vm, p->clear_child_tid, 1);
        p->clear_child_tid = 0;
    }

    // VFS file descriptor table cleanup (closes all VFS files)
    if (p->fdtable != NULL) {
        vfs_fdtable_put(p->fdtable);
        p->fdtable = NULL;
    }

    if (p->fs != NULL) {
        vfs_struct_put(p->fs);
        p->fs = NULL;
    }

    // Release session reference
    // (deferred to inside pid_wlock below, where we also call
    //  pgroup_remove_thread and session_remove_thread)
    bool last_in_group = true;
    bool is_leader = thread_is_group_leader(p);

    // For CLONE_THREAD non-leader threads: self-reap without becoming
    // a visible zombie. The parent only sees the group leader.
    if ((p->clone_flags & CLONE_THREAD) && !is_leader) {
        // Non-leader thread in a thread group: self-cleanup.

        // Reparent any children to init (non-leaders can fork).
        // Must be done before removing from proctab.
        reparent(p);

        // Remove from thread group and proc table atomically under pid_wlock.
        pid_wlock();
        pgroup_remove_thread(p);
        session_remove_thread(p->session, p);
        if (tg != NULL) {
            last_in_group = thread_group_remove(p);
        }
        proctab_proc_remove(p);
        pid_wunlock();
        __free_pid();

        // If we're the last thread and the leader is already zombie,
        // wake the parent so it can reap the leader.
        // Must happen before thread_group_put drops our reference.
        if (last_in_group && tg != NULL && tg->group_leader != NULL) {
            struct thread *leader = tg->group_leader;
            leader->xstate = status;
            pid_rlock();
            struct thread *parent = leader->parent;
            pid_runlock();
            if (parent != NULL) {
                if (leader->signal.esignal > 0) {
                    kill_thread(parent, leader->signal.esignal);
                }
                scheduler_wakeup_interruptible(parent);
            }
        }

        // Self-cleanup: release resources that thread_destroy would
        // normally handle. We can't free our own kstack (we're running
        // on it), so that's deferred to context_switch_finish via the
        // THREAD_FLAG_SELF_REAP flag.
        if (p->sigacts != NULL) {
            sigacts_put(p->sigacts);
            p->sigacts = NULL;
        }
        if (p->vm != NULL) {
            vm_put(p->vm);
            p->vm = NULL;
        }
        // Release FPU state.  Seq-based ownership means stale
        // per-CPU entries are harmless — no need to scan all CPUs.
        if (p->fpu_state != NULL) {
            kvfree(p->fpu_state);
            p->fpu_state = NULL;
        }
        // Purge pending signals (sigacts already NULL, lock check skipped)
        sigpending_empty(p, 0);
        sigpending_destroy(p);
        // Release thread_group reference
        if (p->thread_group != NULL) {
            thread_group_put(p->thread_group);
            p->thread_group = NULL;
        }
        // (session/pgroup references already released above under pid_wlock)

        // Mark as zombie requiring self-reap.  context_switch_finish
        // will free the kstack after switching to the next thread's stack.
        tcb_lock(p);
        p->xstate = status;
        THREAD_SET_SELF_REAP(p);
        __thread_state_set(p, THREAD_ZOMBIE);
        tcb_unlock(p);

        scheduler_yield();
        panic("exit: non-leader thread should not return");
    }

    // Leader thread or single-threaded process: remove from thread group
    // under pid_wlock, then proceed with standard exit path.
    struct tty *hangup_tty = NULL;
    if (tg != NULL) {
        pid_wlock();

        /* POSIX: when a session leader with a controlling terminal
         * exits, hang up the terminal (SIGHUP to foreground pgroup,
         * sever the PTY pipe link so the master gets EOF). */
        {
            struct session *sess = p->session;
            if (sess != NULL &&
                sess->sid == thread_tgid(p) &&
                sess->ctrl_tty != NULL) {
                hangup_tty = sess->ctrl_tty;
                tty_ref(hangup_tty);  /* prevent free while we hold ptr */
                session_hangup(sess);
            }
        }

        pgroup_remove_thread(p);
        session_remove_thread(p->session, p);
        last_in_group = thread_group_remove(p);
        pid_wunlock();
    }

    /* Outside pid_wlock: unregister /dev/pts/N from devtmpfs so the
     * device node disappears immediately (can't call cdev_unregister
     * under pid_wlock because it takes filesystem locks). */
    if (hangup_tty != NULL) {
        pty_hangup_cleanup(hangup_tty);
        tty_unref(hangup_tty);
    }

    // Leader thread or single-threaded process: standard exit path
    reparent(p);

    /* kqueue: notify EVFILT_PROC watchers of exit */
    kqueue_proc_notify(p, NOTE_EXIT, status);

    tcb_lock(p);
    p->xstate = status;
    __thread_state_set(p, THREAD_ZOMBIE);
    tcb_unlock(p);

    // Read parent under pid_rlock to avoid racing with reparent() on
    // another CPU which temporarily NULLs child->parent inside
    // detach_child before attach_child sets it to initproc.
    pid_rlock();
    struct thread *parent = p->parent;
    pid_runlock();

    // For a group leader: only notify parent if this is the last thread
    // (or if there's no thread group). Non-last leaders stay zombie silently
    // until the last thread exits and wakes the parent.
    if (last_in_group || tg == NULL) {
        // Notify parent: send exit signal first, then wake.
        //
        // Order matters: kill_thread → scheduler_wakeup_interruptible.
        // If we wake the parent first, it re-enters waitpid and toggles
        // between RUNNING and INTERRUPTIBLE in the on_cpu spin loop.
        // When kill_thread's signal_notify then catches the parent in
        // INTERRUPTIBLE, __do_scheduler_wakeup's retry loop can livelock
        // against the state toggling, preventing the zombie from ever
        // yielding and clearing on_cpu.
        //
        // By sending the signal first (while the parent is still genuinely
        // sleeping), signal_notify's wakeup succeeds cleanly.  The
        // subsequent scheduler_wakeup_interruptible is then either a no-op
        // (parent already woken by signal_notify) or the primary wake
        // (when the exit signal is 0 or ignored).
        if (parent != NULL) {
            if (p->signal.esignal > 0) {
                kill_thread(parent, p->signal.esignal);
            }
            // Wake all threads in the parent's thread group — any of them
            // may be blocked in wait()/waitpid() now that POSIX allows
            // cross-thread child reaping.
            struct thread_group *ptg = parent->thread_group;
            if (ptg != NULL) {
                pid_rlock();
                struct thread *w, *w_tmp;
                list_foreach_node_safe(&ptg->thread_list, w, w_tmp, tg_entry) {
                    scheduler_wakeup_interruptible(w);
                }
                pid_runlock();
            } else {
                scheduler_wakeup_interruptible(parent);
            }
        }
    }

    scheduler_yield();
    panic("exit: __exit_yield should not return");
}

// Wait for a child thread to exit and return its pid.
// Return -1 if this thread has no children.
//
// Uses the Linux "set-state-before-check" pattern to avoid lost wakeups:
// 1. Set state to INTERRUPTIBLE before scanning children
// 2. Scan for zombies - if found, restore RUNNING and return
// 3. If not found, yield (scheduler_yield will abort if we were woken)
// 4. Loop back to step 1
//
// POSIX semantics: any thread in a process can wait on children forked
// by any other thread in the same process, so we scan the children lists
// of all threads in the thread group (matching Linux's do_wait behaviour).
//
// Locking: holds pid_rlock while scanning the children list.
// Upgrades to pid_wlock (via try_upgrade or runlock+wlock) to detach
// a zombie child, remove it from the proc table, and free its PID.
int wait(uint64 addr) {
    int pid = -1;
    int xstate = 0;
    struct thread *p = current;
    struct thread_group *tg = p->thread_group;
    struct thread *child, *tmp;

    pid_rlock();

    for (;;) {
        // Set INTERRUPTIBLE BEFORE scanning - this is the Linux pattern.
        // Any child that calls wakeup_interruptible() while we're scanning
        // will change our state back to RUNNING (or WAKENING if on_cpu).
        __thread_state_set(p, THREAD_INTERRUPTIBLE);

        int total_children = 0;

        // Scan children of ALL threads in our thread group.
        struct thread *thr, *thr_tmp;
        list_foreach_node_safe(&tg->thread_list, thr, thr_tmp, tg_entry) {
            total_children += thr->children_count;

            list_foreach_node_safe(&thr->children, child, tmp, siblings) {
                // Thread state will never transition back from ZOMBIE, so no
                // need to lock the child.
                if (zombie_child_is_reapable(child)) {
                    // Transition to RUNNING immediately — we've found a zombie
                    // and must NOT be in INTERRUPTIBLE during the on_cpu spin.
                    __thread_state_set(p, THREAD_RUNNING);
                    // Spin-wait for the zombie to finish context_switch_finish
                    // which clears on_cpu.
                    int spin_count = 0;
                    while (smp_load_acquire(&child->sched_entity->on_cpu)) {
                        cpu_relax();
                        spin_count++;
                        if (spin_count > 1000) {
                            pid_runlock();
                            scheduler_yield(); // Give other CPUs a chance
                            pid_rlock();
                            spin_count = 0;
                        }
                    }
                    xstate = child->xstate;
                    pid = child->pid;
                    if (!pid_try_lock_upgrade()) {
                        pid_runlock();
                        pid_wlock();
                    }
                    detach_child(child->parent, child);
                    proctab_proc_remove(child);
                    pid_wunlock();
                    procfs_evict_pid(child->tgid);
                    __free_pid();
                    thread_destroy(child);
                    goto ret_unlocked;
                }
            }
        }

        // No point waiting if there are no children across the whole group.
        if (total_children == 0) {
            __thread_state_set(p, THREAD_RUNNING);
            pid = -ECHILD;
            goto ret;
        }

        pid_runlock();
        scheduler_yield();
        pid_rlock();
        // State will be set to INTERRUPTIBLE at the start of next loop
        // iteration
    }

ret:
    pid_runlock();
ret_unlocked:
    if (pid >= 0 && addr != 0) {
        // copy xstate to user.
        if (either_copyout(1, addr, (char *)&xstate, sizeof(xstate)) < 0) {
            return -EFAULT;
        }
    }
    return pid;
}

// waitpid() with POSIX semantics:
//   pid  > 0 : wait for the specific child pid
//   pid == -1: wait for any child (same as wait)
// options:
//   WNOHANG   (1): return 0 immediately if no matching zombie/stopped child
//   WUNTRACED (2): also return when a child has stopped
// Status encoding (POSIX):
//   exited:  (exit_code << 8) | 0x00
//   stopped: (stop_sig  << 8) | 0x7f
//
// POSIX: any thread in a process can wait on children forked by any other
// thread in the same process.  We scan the children lists of all threads
// in the calling thread's thread group (matching Linux's do_wait behaviour).
int waitpid(int target_pid, uint64 addr, int options) {
    const int WNOHANG   = 1;
    const int WUNTRACED = 2;
    if (options & ~(WNOHANG | WUNTRACED)) {
        return -EINVAL;
    }

    struct thread *p = current;
    struct thread_group *tg = p->thread_group;

    int pid = -1;
    int xstate = 0;
    struct thread *child, *tmp;

    pid_rlock();

    for (;;) {
        bool has_match = false;

        __thread_state_set(p, THREAD_INTERRUPTIBLE);

        // Scan children of ALL threads in our thread group.
        struct thread *thr, *thr_tmp;
        list_foreach_node_safe(&tg->thread_list, thr, thr_tmp, tg_entry) {
            list_foreach_node_safe(&thr->children, child, tmp, siblings) {
                if (target_pid > 0 && child->pid != target_pid) {
                    continue;
                }
                has_match = true;

                // Check for stopped child (WUNTRACED)
                if ((options & WUNTRACED) && THREAD_STOPPED(child)) {
                    __thread_state_set(p, THREAD_RUNNING);
                    pid = child->pid;
                    // Encode stopped status: (signal << 8) | 0x7f
                    int stopsig = child->signal.stop_signal;
                    xstate = (stopsig << 8) | 0x7f;
                    goto ret;
                }

                if (zombie_child_is_reapable(child)) {
                    __thread_state_set(p, THREAD_RUNNING);
                    int spin_count = 0;
                    while (smp_load_acquire(&child->sched_entity->on_cpu)) {
                        cpu_relax();
                        spin_count++;
                        if (spin_count > 1000) {
                            pid_runlock();
                            scheduler_yield();
                            pid_rlock();
                            spin_count = 0;
                        }
                    }
                    // Encode exited status: (exit_code << 8) | 0x00
                    xstate = (child->xstate & 0xff) << 8;
                    pid = child->pid;
                    if (!pid_try_lock_upgrade()) {
                        pid_runlock();
                        pid_wlock();
                    }
                    detach_child(child->parent, child);
                    proctab_proc_remove(child);
                    pid_wunlock();
                    procfs_evict_pid(child->tgid);
                    __free_pid();
                    thread_destroy(child);
                    goto ret_unlocked;
                }
            }
        }

        if (!has_match) {
            __thread_state_set(p, THREAD_RUNNING);
            pid = -ECHILD;
            goto ret;
        }

        if (options & WNOHANG) {
            __thread_state_set(p, THREAD_RUNNING);
            pid = 0;
            goto ret;
        }

        pid_runlock();
        scheduler_yield();
        pid_rlock();
    }

ret:
    pid_runlock();
ret_unlocked:
    if (pid > 0 && addr != 0) {
        if (either_copyout(1, addr, (char *)&xstate, sizeof(xstate)) < 0) {
            return -EFAULT;
        }
    }
    return pid;
}
