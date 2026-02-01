#include "proc/proc.h"
#include "defs.h"
#include "hlist.h"
#include "list.h"
#include <mm/memlayout.h>
#include <mm/page.h>
#include "param.h"
#include "printf.h"
#include "proc/proc_queue.h"
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


// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void) {
    struct proc *np;
    struct proc *p = myproc();

    if (!PROC_USER_SPACE(p)) {
        return -1;
    }

    // Allocate process.
    if (user_proc_create(&np, p->kstack_order) < 0) {
        return -1;
    }

    // Copy user memory from parent to child.
    vm_t *new_vm = vm_copy(p->vm, (uint64)np->trapframe);
    if (new_vm == NULL) {
        freeproc(np);
        return -1;
    }

    // Clone VFS cwd and root inode references
    struct fs_struct *fs_clone = vfs_struct_clone(p->fs, 0);
    if (IS_ERR_OR_NULL(fs_clone)) {
        vm_put(new_vm);
        freeproc(np);
        return -1;
    }

    // Clone VFS file descriptor table - must be done after releasing parent
    // lock because vfs_fdup may call cdev_dup which needs a mutex
    struct vfs_fdtable *new_fdtable = vfs_fdtable_clone(p->fdtable, 0);
    if (IS_ERR_OR_NULL(new_fdtable)) {
        vm_put(new_vm);
        vfs_struct_put(fs_clone);
        freeproc(np);
        return -1;
    }

    proc_lock(p);
    proc_lock(np);

    // copy the process's signal actions.
    if (p->sigacts) {
        np->sigacts = sigacts_dup(p->sigacts);
        if (np->sigacts == NULL) {
            proc_unlock(p);
            proc_unlock(np);
            vm_put(new_vm);
            vfs_struct_put(fs_clone);
            vfs_fdtable_put(new_fdtable);
            freeproc(np);
            return -1;
        }
    }

    np->fdtable = new_fdtable;
    np->fs = fs_clone;
    np->vm = new_vm;

    // copy saved user registers.
    *(np->trapframe) = *(p->trapframe);

    // Cause fork to return 0 in the child.
    np->trapframe->trapframe.a0 = 0;

    // VFS cwd and rooti already cloned above
    safestrcpy(np->name, p->name, sizeof(p->name));

    attach_child(p, np);
    PROC_SET_USER_SPACE(np);
    __proc_set_pstate(np, PSTATE_UNINTERRUPTIBLE);

    // Initialize the child's scheduling entity with parent's info
    rq_task_fork(np->sched_entity);

    proc_unlock(p);
    proc_unlock(np);

    // Wake up the new child process
    // Note: pi_lock no longer needed - rq_lock serializes wakeups
    scheduler_wakeup(np);

    return np->pid;
}
