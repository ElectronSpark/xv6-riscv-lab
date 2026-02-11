#include "proc/thread.h"
#include "proc/thread_group.h"
#include "defs.h"
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
#include "types.h"
#include "vfs/file.h"
#include "vfs/fs.h"
#include <mm/vm.h>
#include "errno.h"

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
        // make sure the child isn't still in exit() or swtch().
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
        scheduler_wakeup_interruptible(initproc);
        if (initproc != NULL && initproc != parent && initproc != p &&
            p->signal.esignal > 0) {
            kill_thread(initproc, p->signal.esignal);
        }
    }
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

    // If this thread is in a thread group undergoing group exit,
    // use the group exit code.
    struct thread_group *tg = p->thread_group;
    if (tg != NULL && thread_group_exiting(tg) && tg->group_exit_task != p) {
        status = tg->group_exit_code;
    }

    // Wake vfork parent FIRST - they're sharing our address space
    // and need to resume before we tear anything down
    vfork_done(p);

    // VFS file descriptor table cleanup (closes all VFS files)
    if (p->fdtable != NULL) {
        vfs_fdtable_put(p->fdtable);
        p->fdtable = NULL;
    }

    if (p->fs != NULL) {
        vfs_struct_put(p->fs);
        p->fs = NULL;
    }

    // Remove from thread group and (for non-leader CLONE_THREAD) from
    // proc table, all under pid_wlock to satisfy thread_group_remove's
    // locking requirement and avoid extra lock/unlock round-trips.
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
                scheduler_wakeup_interruptible(parent);
                if (leader->signal.esignal > 0) {
                    kill_thread(parent, leader->signal.esignal);
                }
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
        // Purge pending signals (sigacts already NULL, lock check skipped)
        sigpending_empty(p, 0);
        sigpending_destroy(p);
        // Release thread_group reference
        if (p->thread_group != NULL) {
            thread_group_put(p->thread_group);
            p->thread_group = NULL;
        }

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
    if (tg != NULL) {
        pid_wlock();
        last_in_group = thread_group_remove(p);
        pid_wunlock();
    }

    // Leader thread or single-threaded process: standard exit path
    reparent(p);

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
        // Wake parent BEFORE we yield - this is the Linux pattern.
        // Always wake parent regardless of exit signal (handles threads with
        // esignal=0 or ignored signals). Then send the exit signal if set.
        if (parent != NULL) {
            scheduler_wakeup_interruptible(parent);
            if (p->signal.esignal > 0) {
                kill_thread(parent, p->signal.esignal);
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
// Locking: holds pid_rlock while scanning the children list.
// Upgrades to pid_wlock (via try_upgrade or runlock+wlock) to detach
// a zombie child, remove it from the proc table, and free its PID.
int wait(uint64 addr) {
    int pid = -1;
    int xstate = 0;
    struct thread *p = current;
    struct thread *child, *tmp;

    pid_rlock();

    for (;;) {
        // Set INTERRUPTIBLE BEFORE scanning - this is the Linux pattern.
        // Any child that calls wakeup_interruptible() while we're scanning
        // will change our state back to RUNNING (or WAKENING if on_cpu).
        __thread_state_set(p, THREAD_INTERRUPTIBLE);

        // Scan through table looking for exited children.
        list_foreach_node_safe(&p->children, child, tmp, siblings) {
            // Thread state will never transition back from ZOMBIE, so no need
            // to lock the child.
            if (THREAD_ZOMBIE(child)) {
                // Make sure the zombie child has fully switched out of CPU.
                // The on_cpu window is very short (just context_switch_finish),
                // so we spin-wait with cpu_relax() rather than yielding.
                // This avoids a tight loop that could starve other threads.
                int spin_count = 0;
                while (smp_load_acquire(&child->sched_entity->on_cpu)) {
                    cpu_relax();
                    spin_count++;
                    // If spinning too long, yield to let other CPU progress
                    if (spin_count > 1000) {
                        __thread_state_set(p, THREAD_RUNNING);
                        pid_runlock();
                        scheduler_yield(); // Give other CPUs a chance
                        pid_rlock();
                        __thread_state_set(p, THREAD_INTERRUPTIBLE);
                        spin_count = 0;
                    }
                }
                // Found one - set state to RUNNING before returning.
                // If we were in WAKENING, rq_flush_wake_list will skip us
                // when it sees our state is no longer WAKENING.
                __thread_state_set(p, THREAD_RUNNING);
                xstate = child->xstate;
                pid = child->pid;
                if (!pid_try_lock_upgrade()) {
                    // Failed to upgrade, we need to release and reacquire the
                    // lock to avoid deadlock.
                    pid_runlock();
                    pid_wlock();
                }
                detach_child(p, child);
                proctab_proc_remove(child);
                pid_wunlock();
                __free_pid(); // Release the PID slot (lock-free)
                thread_destroy(child);
                goto ret_unlocked;
            }
        }

        // No point waiting if we don't have any children.
        if (p->children_count == 0) {
            __thread_state_set(p, THREAD_RUNNING);
            pid = -1;
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
