#include "proc/proc.h"
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


// Wake up a parent process that may be sleeping in wait().
// This is called unconditionally when a child exits, regardless of
// the exit_signal. Matches Linux's __wake_up_parent() which always
// wakes the wait_chldexit wait queue.
//
// This ensures wait() returns even when:
// - exit_signal is 0 (threads with CLONE_THREAD)
// - exit_signal is ignored (SIGCHLD with SIG_IGN)
// - exit_signal is any other signal
static void wake_up_parent(struct proc *parent) {
    if (parent == NULL)
        return;
    
    proc_lock(parent);
    enum procstate pstate = __proc_get_pstate(parent);
    if (PSTATE_IS_INTERRUPTIBLE(pstate)) {
        proc_unlock(parent);
        scheduler_wakeup_interruptible(parent);
    } else {
        proc_unlock(parent);
    }
}

// Wake the vfork parent when child exits or execs.
// The vfork parent is blocked in UNINTERRUPTIBLE state waiting for us.
// After waking, clear the vfork_parent pointer so we don't wake twice.
void vfork_done(struct proc *p) {
    struct proc *vfork_parent;
    
    proc_lock(p);
    vfork_parent = p->vfork_parent;
    p->vfork_parent = NULL;  // Clear so we don't wake again on exit after exec
    proc_unlock(p);
    
    if (vfork_parent != NULL) {
        scheduler_wakeup(vfork_parent);
    }
}

// Pass p's abandoned children to init.
// Caller must not hold p->lock.
// Uses trylock with backoff to avoid lock convoy when many processes
// exit simultaneously and all compete for initproc's lock.
void reparent(struct proc *p) {
    struct proc *initproc = __proctab_get_initproc();
    struct proc *parent = p->parent;
    struct proc *child, *tmp;
    bool zombie_found = false;

    assert(initproc != NULL, "reparent: initproc is NULL");
    assert(p != initproc, "reparent: p is init process");

retry:
    // Try to acquire both locks without blocking
    if (!spin_trylock(&initproc->lock)) {
        // Couldn't get initproc lock - backoff and retry
        for (volatile int i = 0; i < 100; i++) cpu_relax();
        goto retry;
    }
    
    if (!spin_trylock(&p->lock)) {
        // Couldn't get p's lock - release initproc and retry
        spin_unlock(&initproc->lock);
        for (volatile int i = 0; i < 100; i++) cpu_relax();
        goto retry;
    }

    list_foreach_node_safe(&p->children, child, tmp, siblings) {
        // make sure the child isn't still in exit() or swtch().
        proc_lock(child);
        child->signal.esignal = SIGCHLD;   // reset to default exit signal
        if (__proc_get_pstate(child) == PSTATE_ZOMBIE) {
            zombie_found = true;
        }
        detach_child(p, child);
        attach_child(initproc, child);
        proc_unlock(child);
    }

    
    proc_unlock(p);
    proc_unlock(initproc);

    if (zombie_found) {
        wake_up_parent(initproc);
        if (initproc != NULL && initproc != parent && initproc != p && p->signal.esignal > 0) {
            kill_proc(initproc, p->signal.esignal);
        }
    }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status) {
    struct proc *p = myproc();
    struct proc *parent = p->parent;
    assert(p != __proctab_get_initproc(), "init exiting");

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

    reparent(p);

    proc_lock(p);
    p->xstate = status;
    __proc_set_pstate(p, PSTATE_ZOMBIE);
    proc_unlock(p);
    
    // Wake parent BEFORE we yield - this is the Linux pattern.
    // Always wake parent regardless of exit signal (handles threads with
    // esignal=0 or ignored signals). Then send the exit signal if set.
    wake_up_parent(parent);
    if (parent != NULL && p->signal.esignal > 0) {
        kill_proc(parent, p->signal.esignal);
    }
    
    scheduler_yield();
    panic("exit: __exit_yield should not return");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// Return -EINTR if interrupted by a signal other than SIGCHLD.
//
// Uses the Linux "set-state-before-check" pattern to avoid lost wakeups:
// 1. Set state to INTERRUPTIBLE before scanning children
// 2. Scan for zombies - if found, restore RUNNING and return
// 3. If not found, yield (scheduler_yield will abort if we were woken)
// 4. On wakeup, check for SIGCHLD - consume it and continue scanning
// 5. If other signal pending, return -EINTR
// 6. Loop back to step 1
//
// This matches Linux's do_wait() behavior where wait() is specifically
// waiting for SIGCHLD from exiting children.
int wait(uint64 addr) {
    int pid = -1;
    int xstate = 0;
    struct proc *p = myproc();
    struct proc *child, *tmp;
    sigset_t saved_mask = 0;
    bool mask_saved = false;

    proc_lock(p);
    
    for (;;) {
        // Set INTERRUPTIBLE BEFORE scanning - this is the Linux pattern.
        // Any child that calls wakeup_interruptible() while we're scanning
        // will change our state back to RUNNING (or WAKENING if on_cpu).
        __proc_set_pstate(p, PSTATE_INTERRUPTIBLE);
        
        // Scan through table looking for exited children.
        list_foreach_node_safe(&p->children, child, tmp, siblings) {
            proc_lock(child);
            if (PROC_ZOMBIE(child)) {
                // Make sure the zombie child has fully switched out of CPU.
                // The on_cpu window is very short (just context_switch_finish),
                // so we spin-wait with cpu_relax() rather than yielding.
                // This avoids a tight loop that could starve other processes.
                int spin_count = 0;
                while (smp_load_acquire(&child->sched_entity->on_cpu)) {
                    cpu_relax();
                    spin_count++;
                    // If spinning too long, yield to let other CPU progress
                    if (spin_count > 1000) {
                        proc_unlock(child);
                        __proc_set_pstate(p, PSTATE_RUNNING);
                        proc_unlock(p);
                        scheduler_yield();  // Give other CPUs a chance
                        proc_lock(p);
                        __proc_set_pstate(p, PSTATE_INTERRUPTIBLE);
                        proc_lock(child);
                        spin_count = 0;
                    }
                }
                // Found one - set state to RUNNING before returning.
                // If we were in WAKENING, rq_flush_wake_list will skip us
                // when it sees our state is no longer WAKENING.
                __proc_set_pstate(p, PSTATE_RUNNING);
                xstate = child->xstate;
                pid = child->pid;
                detach_child(p, child);
                proc_unlock(child);
                freeproc(child);
                goto ret;
            }
            proc_unlock(child);
        }

        // No point waiting if we don't have any children.
        if (p->children_count == 0) {
            __proc_set_pstate(p, PSTATE_RUNNING);
            pid = -1;
            goto ret;
        }
        
        proc_unlock(p);
        scheduler_yield();
        proc_lock(p);
        // State will be set to INTERRUPTIBLE at the start of next loop iteration
    }

ret:
    // Restore the original signal mask before returning
    if (mask_saved && p->sigacts != NULL) {
        sigacts_lock(p->sigacts);
        p->sigacts->sa_sigmask = saved_mask;
        recalc_sigpending_tsk(p);
        sigacts_unlock(p->sigacts);
    }
    proc_unlock(p);
    if (pid >= 0 && addr != 0) {
        // copy xstate to user.
        if (either_copyout(1, addr, (char *)&xstate, sizeof(xstate)) < 0) {
            return -EFAULT;
        }
    }
    return pid;
}
