#include "proc/proc.h"
#include "clone_flags.h"
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

// Entry wrapper for forked user processes.
// This is called as the entry point from context switch.
static void forkret_entry(struct context *prev) {
    assert(PROC_USER_SPACE(myproc()),
           "kernel process %d tries to return to user space", myproc()->pid);
    assert(prev != NULL, "forkret_entry: prev context is NULL");

    // Finish the context switch first - this releases the rq lock
    context_switch_finish(proc_from_context(prev), myproc(), 0);
    mycpu()->noff = 0;  // in a new process, noff should be 0
    intr_on();
    // Note quiescent state for RCU - context switch is a quiescent state.
    // Callback processing is now handled by per-CPU RCU kthreads.
    rcu_check_callbacks();

    // Now safe to do the rest without holding scheduler locks
    smp_mb();
    usertrapret();
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
// Caller must always provide valid clone_args.
int proc_clone(struct clone_args *args) {
    struct proc *ret_ptr;
    struct proc *p = myproc();

    if (args == NULL) {
        return -EINVAL;
    }

    if (!PROC_USER_SPACE(p)) {
        return -EINVAL;
    }

    // When CLONE_VM is specified without CLONE_VFORK, stack and entry must be provided.
    // CLONE_VFORK is special: child shares parent's stack temporarily and must exec/exit.
    if ((args->flags & CLONE_VM) && !(args->flags & CLONE_VFORK) &&
        (args->stack == 0 || args->entry == 0)) {
        return -EINVAL;
    }

    // When stack is specified, stack_size must be valid.
    if (args->stack != 0 && (args->stack_size < USERSTACK_MINSZ ||
                             (args->stack_size & (PAGE_SIZE - 1)) != 0)) {
        return -EINVAL;
    }

    // Allocate process.
    ret_ptr = allocproc(forkret_entry, 0, 0, p->kstack_order);
    if (IS_ERR_OR_NULL(ret_ptr)) {
        goto out;
    }

    // Copy user memory from parent to child.
    vm_t *new_vm = NULL;
    if (args->flags & CLONE_VM) {
        // Share the VM
        new_vm = p->vm;
        vm_dup(new_vm);
    } else {
        new_vm = vm_copy(p->vm);
        if (IS_ERR_OR_NULL(new_vm)) {
            freeproc(ret_ptr);
            ret_ptr = (void*)new_vm;
            goto out;
        }
    }
    ret_ptr->vm = new_vm;

    // Clone VFS cwd and root inode references
    struct fs_struct *fs_clone = vfs_struct_clone(p->fs, args->flags);
    if (IS_ERR_OR_NULL(fs_clone)) {
        freeproc(ret_ptr);
        ret_ptr = (void*)fs_clone;
        goto out;
    }
    ret_ptr->fs = fs_clone;

    // Clone VFS file descriptor table - must be done after releasing parent
    // lock because vfs_fdup may call cdev_dup which needs a mutex
    struct vfs_fdtable *new_fdtable = vfs_fdtable_clone(p->fdtable, args->flags);
    if (IS_ERR_OR_NULL(new_fdtable)) {
        freeproc(ret_ptr);
        ret_ptr = (void*)new_fdtable;
        goto out;
    }
    ret_ptr->fdtable = new_fdtable;

    // copy the process's signal actions.
    if (p->sigacts) {
        ret_ptr->sigacts = sigacts_dup(p->sigacts, args->flags);
        if (ret_ptr->sigacts == NULL) {
            freeproc(ret_ptr);
            return -ENOMEM;
        }
    }

    // signal to be sent to parent on exit
    ret_ptr->signal.esignal = args->esignal;
    ret_ptr->clone_flags = args->flags;

    // copy saved user registers.
    *(ret_ptr->trapframe) = *(p->trapframe);

    if (args->entry != 0) {
        // If entry point specified, set child's sepc to it
        ret_ptr->trapframe->trapframe.sepc = args->entry;
    }

    if (args->stack != 0) {
        // If stack specified, set child's sp to top of the stack
        uint64 stack_top = (args->stack + args->stack_size) & ~0xFUL;
        ret_ptr->trapframe->trapframe.sp = stack_top;
    }

    // Cause fork to return 0 in the child.
    ret_ptr->trapframe->trapframe.a0 = 0;

    // VFS cwd and rooti already cloned above
    safestrcpy(ret_ptr->name, p->name, sizeof(p->name));

    proc_lock(p);
    proc_lock(ret_ptr);
    attach_child(p, ret_ptr);
    PROC_SET_USER_SPACE(ret_ptr);
    __proc_set_pstate(ret_ptr, PSTATE_UNINTERRUPTIBLE);

    // Initialize the child's scheduling entity with parent's info
    rq_task_fork(ret_ptr->sched_entity);

    // For vfork, set up the parent-child relationship so child can wake parent
    if (args->flags & CLONE_VFORK) {
        ret_ptr->vfork_parent = p;
        // Set parent state BEFORE waking child to avoid race:
        // child might exit before parent goes to sleep
        __proc_set_pstate(p, PSTATE_UNINTERRUPTIBLE);
    } else {
        ret_ptr->vfork_parent = NULL;
    }
    
    proc_unlock(p);
    proc_unlock(ret_ptr);

    // Wake up the new child process
    // Note: pi_lock no longer needed - rq_lock serializes wakeups
    scheduler_wakeup(ret_ptr);

    // For vfork, parent blocks until child exits or execs
    if (args->flags & CLONE_VFORK) {
        scheduler_yield();
        // When we return here, child has called exec() or exit()
    }

out:
    if (IS_ERR(ret_ptr)) {
        return PTR_ERR(ret_ptr);
    } else if (ret_ptr == NULL) {
        return -ENOMEM;
    }
    return ret_ptr->pid;
}
