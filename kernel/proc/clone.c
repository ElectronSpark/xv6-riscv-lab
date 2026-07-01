/**
 * @file clone.c
 * @brief Architecture-independent thread cloning (fork/clone).
 *
 * Implements thread_clone() and its fork-return wrapper.
 * Architecture-specific child register adjustments are handled
 * by arch_clone_child_regs() from <arch_thread.h>.
 */

#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/chrome_lifecycle.h"
#include "clone_flags.h"
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
#include "types.h"
#include "vfs/file.h"
#include "vfs/fs.h"
#include "maple_tree.h"
#include <mm/vm.h>
#include "errno.h"
#include "proc/pgroup.h"
#include "tty/session.h"
#include "arch_thread.h"
#include "accounting.h"
#include "resource.h"

static const char *clone_asset_vma_path(vma_t *vma)
{
    if (vma == NULL || vma->file == NULL)
        return "-";
    if (vma->file->opened_path != NULL &&
        vma->file->opened_path[0] != '\0')
        return vma->file->opened_path;
    if (vma->file->inode.inode != NULL &&
        vma->file->inode.inode->name != NULL)
        return vma->file->inode.inode->name;
    return "(unnamed)";
}

static int clone_asset_fd_valid(struct vfs_file *file)
{
    return (uint64)file > NOFILE;
}

static int clone_asset_path_match(const char *path)
{
    if (path == NULL)
        return 0;
    return strstr(path, "icudtl.dat") != NULL ||
           strstr(path, "v8_context_snapshot.bin") != NULL;
}

static void chrome_clone_dump_asset_vmas(struct thread *p, const char *tag)
{
    vm_t *vm;
    int found = 0;

    if (p == NULL || p->vm == NULL)
        return;
    vm = p->vm;

    vm_rlock(vm);
    vma_t *vma;
    uint64 index = 0;
    mt_for_each(&vm->vm_mt, vma, index, (uint64)(-1ULL)) {
        const char *path = clone_asset_vma_path(vma);

        if (!clone_asset_path_match(path))
            continue;

        found++;
        printf("chrome-clone-asset-vma: tag=%s pid=%d tgid=%d name=%s map=[0x%lx-0x%lx) %c%c%c %s flags=0x%lx pgoff=0x%lx file=%p path=%s\n",
               tag, p->pid, p->tgid, p->name, vma->start, vma->end,
               (vma->flags & PROT_READ) ? 'r' : '-',
               (vma->flags & PROT_WRITE) ? 'w' : '-',
               (vma->flags & PROT_EXEC) ? 'x' : '-',
               (vma->flags & VMA_FLAG_SHARED) ? "shared" : "private",
               vma->flags, vma->pgoff, (void *)vma->file, path);
    }
    vm_runlock(vm);

    if (found == 0) {
        printf("chrome-clone-asset-vma: tag=%s pid=%d tgid=%d name=%s found=0\n",
               tag, p->pid, p->tgid, p->name);
    }
}

static void chrome_clone_dump_asset_fds(struct thread *p, const char *tag)
{
    struct vfs_fdtable *fdt;
    int found = 0;

    if (p == NULL || p->fdtable == NULL)
        return;
    fdt = p->fdtable;

    spin_lock(&fdt->lock);
    for (int fd = 0; fd < NOFILE; fd++) {
        struct vfs_file *file = fdt->files[fd];
        const char *path;

        if (!clone_asset_fd_valid(file))
            continue;
        path = file->opened_path;
        if (path == NULL || path[0] == '\0')
            path = "(no-path)";
        if (fd != 3 && !clone_asset_path_match(path))
            continue;

        found++;
        printf("chrome-clone-asset-fd: tag=%s pid=%d tgid=%d name=%s fd=%d cloexec=%d kind=%d flags=0x%x file=%p path=%s\n",
               tag, p->pid, p->tgid, p->name, fd,
               !!(fdt->cloexec_bitmap[fd >> 6] & (1ULL << (fd & 63))),
               file->f_kind, file->f_flags, (void *)file, path);
    }
    spin_unlock(&fdt->lock);

    if (found == 0) {
        printf("chrome-clone-asset-fd: tag=%s pid=%d tgid=%d name=%s found=0\n",
               tag, p->pid, p->tgid, p->name);
    }
}

static void chrome_clone_dump_asset_state(struct thread *parent,
                                          struct thread *child,
                                          uint64 flags)
{
    if (!chrome_trace_value_enabled("chrome_fd_trace"))
        return;
    if (!chrome_lifecycle_thread_match(parent) &&
        !chrome_lifecycle_thread_match(child))
        return;
    if (flags & CLONE_THREAD)
        return;

    chrome_clone_dump_asset_vmas(parent, "parent-prewake");
    chrome_clone_dump_asset_vmas(child, "child-prewake");
    chrome_clone_dump_asset_fds(parent, "parent-prewake");
    chrome_clone_dump_asset_fds(child, "child-prewake");
}

static int chrome_clone_fdtable_trace_enabled(void)
{
    static int initialized;
    static int enabled;

    if (!initialized) {
        enabled = chrome_trace_value_enabled("chrome_clone_fdtable_trace") ||
                  chrome_trace_value_enabled("chrome_exec_fdtable_trace");
        initialized = 1;
    }
    return enabled;
}

static void chrome_clone_dump_fdtable_state(struct thread *parent,
                                            struct thread *child,
                                            uint64 flags)
{
    if (flags & CLONE_THREAD)
        return;
    if (!chrome_clone_fdtable_trace_enabled())
        return;
    if (!chrome_lifecycle_trace_match(parent) &&
        !chrome_lifecycle_trace_match(child))
        return;

    vfs_fdtable_debug_dump(child, "clone-child-prewake-lite", 64);
}

static void clone_destroy_unstarted_child(struct thread *child)
{
    if (child == NULL)
        return;

    pid_wlock();
    if (child->parent != NULL && !LIST_ENTRY_IS_DETACHED(&child->siblings))
        detach_child(child->parent, child);
    if (child->pgroup != NULL)
        pgroup_remove_thread(child);
    if (child->session != NULL)
        (void)session_remove_thread(child->session, child);
    if (child->thread_group != NULL)
        (void)thread_group_remove(child);
    proctab_proc_remove(child);
    pid_wunlock();

    __free_pid();
    thread_destroy(child);
}

// Entry wrapper for forked user threads.
// This is called as the entry point from context switch.
static void forkret_entry(struct context *prev) {
    assert(THREAD_USER_SPACE(current),
           "kernel thread %d tries to return to user space", current->pid);
    assert(prev != NULL, "forkret_entry: prev context is NULL");

    // Finish the context switch first - this releases the rq lock
    context_switch_finish(thread_from_context(prev), current, 0);
    mycpu()->noff = 0; // in a new thread, noff should be 0
    intr_on();
    // Note quiescent state for RCU - context switch is a quiescent state.
    // Callback processing is now handled by per-CPU RCU kthreads.
    rcu_check_callbacks();

    // Now safe to do the rest without holding scheduler locks
    smp_mb();

    usertrapret();
}

// Create a new thread, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
// Caller must always provide valid clone_args.
int thread_clone(struct clone_args *args) {
    struct thread *ret_ptr;
    struct thread *p = current;

    if (args == NULL) {
        return -EINVAL;
    }

    if (!THREAD_USER_SPACE(p)) {
        return -EINVAL;
    }

    // CLONE_THREAD implies CLONE_PARENT
    // CLONE_VM, CLONE_SIGHAND are required in this case
    // (POSIX requirement + Linux behavior: "like CLONE_PARENT").
    if (args->flags & CLONE_THREAD) {
        if ((args->flags & (CLONE_VM | CLONE_SIGHAND)) !=
            (CLONE_VM | CLONE_SIGHAND)) {
            return -EINVAL;
        }
        args->flags |= CLONE_PARENT;
    }

    // When CLONE_VM is specified without CLONE_VFORK, a stack must be
    // provided. entry=0 is valid (Linux behavior: child returns from syscall
    // and the userspace wrapper arranges to call the thread function).
    if ((args->flags & CLONE_VM) && !(args->flags & CLONE_VFORK) &&
        (args->stack == 0)) {
        return -EINVAL;
    }

    // When stack is specified and stack_size is non-zero, require a usable
    // range. Linux clone3 callers such as glibc may pass a non-page-multiple
    // stack_size; arch_clone_child_regs() aligns the computed stack top.
    if (args->stack != 0 && args->stack_size != 0 &&
        args->stack_size < USERSTACK_MINSZ) {
        return -EINVAL;
    }

    if (args->flags & CLONE_PIDFD) {
        if (args->flags & CLONE_DETACHED)
            return -EINVAL;
        if (args->pidfd == 0)
            return -EFAULT;
    }

    // Too many processes are already running
    if (__alloc_pid() < 0) {
        return -EAGAIN;
    }

    // Allocate thread.
    ret_ptr = thread_create(forkret_entry, 0, 0, p->kstack_order);
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
            thread_destroy(ret_ptr);
            ret_ptr = ERR_CAST(new_vm);
            goto out;
        }
    }
    ret_ptr->vm = new_vm;

    // Clone VFS cwd and root inode references
    struct fs_struct *fs_clone = vfs_struct_clone(p->fs, args->flags);
    if (IS_ERR_OR_NULL(fs_clone)) {
        thread_destroy(ret_ptr);
        ret_ptr = ERR_CAST(fs_clone);
        goto out;
    }
    ret_ptr->fs = fs_clone;

    // Clone VFS file descriptor table - must be done after releasing parent
    // lock because vfs_fdup may call cdev_dup which needs a mutex
    struct vfs_fdtable *new_fdtable =
        vfs_fdtable_clone(p->fdtable, args->flags);
    if (IS_ERR_OR_NULL(new_fdtable)) {
        thread_destroy(ret_ptr);
        ret_ptr = ERR_CAST(new_fdtable);
        goto out;
    }
    vfs_fdtable_put(ret_ptr->fdtable);
    ret_ptr->fdtable = new_fdtable;

    // copy the process's signal actions.
    if (p->sigacts) {
        ret_ptr->sigacts = sigacts_dup(p->sigacts, args->flags);
        if (ret_ptr->sigacts == NULL) {
            thread_destroy(ret_ptr);
            ret_ptr = INIT_ERR_PTR(-ENOMEM);
            goto out;
        }
    }

    // Copy per-thread signal mask from parent
    sigpending_clone(&ret_ptr->signal, &p->signal, args->flags, args->esignal);
    ret_ptr->clone_flags = args->flags;
    ret_ptr->linux_sched_policy = p->linux_sched_policy;
    ret_ptr->linux_sched_priority = p->linux_sched_priority;
    rseq_clear_thread(ret_ptr);

    // copy saved user registers.
    *(ret_ptr->trapframe) = *(p->trapframe);

    // Copy FPU state if the parent has used FP.
    if (THREAD_FPU_USED(p) && p->fpu_state != NULL) {
        ret_ptr->fpu_state = kvmalloc(sizeof(struct fpu_state));
        if (ret_ptr->fpu_state != NULL) {
            // If parent owns the hardware FPU, force-save first.
            if (mycpu()->fpu_owner_tid == p->pid &&
                p->fpu_seq == mycpu()->fpu_seq) {
#ifdef __riscv
                unsigned long s = r_sstatus();
                s &= ~SSTATUS_FS_MASK;
                s |= SSTATUS_FS_DIRTY;
                w_sstatus(s);
#else
                asm volatile("clts");
#endif
                fpu_save_state(p->fpu_state);
            }
            memmove(ret_ptr->fpu_state, p->fpu_state,
                    sizeof(struct fpu_state));
            THREAD_SET_FPU_USED(ret_ptr);
        }
    }

    // Apply architecture-specific child register adjustments:
    // sets entry point, stack pointer, return value = 0, and TLS.
    arch_clone_child_regs(ret_ptr->trapframe, args->flags,
                          args->entry, args->stack, args->stack_size,
                          args->tls);

    // CLONE_CHILD_CLEARTID: store the address to zero+futex_wake on thread exit
    if (args->flags & CLONE_CHILD_CLEARTID) {
        ret_ptr->clear_child_tid = args->ctid;
    }

    // VFS cwd and rooti already cloned above
    safestrcpy(ret_ptr->name, p->name, sizeof(p->name));

    tcb_lock(ret_ptr);
    THREAD_SET_USER_SPACE(ret_ptr);
    __thread_state_set(ret_ptr, THREAD_UNINTERRUPTIBLE);

    // Initialize the child's scheduling entity with parent's info
    rq_task_fork(ret_ptr->sched_entity);

    // For vfork, set up the parent-child relationship so child can wake parent
    if (args->flags & CLONE_VFORK) {
        ret_ptr->vfork_parent = p;
        // Set parent state BEFORE waking child to avoid race:
        // child might exit before parent goes to sleep
        __thread_state_set(p, THREAD_UNINTERRUPTIBLE);
    } else {
        ret_ptr->vfork_parent = NULL;
    }

    tcb_unlock(ret_ptr);

    // Attach to parent and add to pid table before waking up the child.
    // proctab_proc_add assigns the actual PID number.
    // thread_group_add requires pid_wlock, so handle CLONE_THREAD here too.
    pid_wlock();

    // Determine the parent. CLONE_PARENT (implied by CLONE_THREAD):
    // child shares the caller's parent. Otherwise: caller is the parent.
    struct thread *real_parent = (args->flags & CLONE_PARENT) ? p->parent : p;

    if (args->flags & CLONE_THREAD) {
        // CLONE_THREAD: not waitable, so not added to any children list.
        // Just record the parent pointer for getppid().
        ret_ptr->parent = real_parent;
    } else {
        // Waitable child: add to parent's children list.
        attach_child(real_parent, ret_ptr);
    }
    proctab_proc_add(ret_ptr);

    // Set up thread group membership under pid_wlock.
    if (args->flags & CLONE_THREAD) {
        // CLONE_THREAD: join the parent's thread group.
        // The child shares the parent's TGID.
        assert(p->thread_group != NULL,
               "clone: parent has no thread_group for CLONE_THREAD");
        thread_group_add(p->thread_group, ret_ptr);
        ret_ptr->tgid = p->tgid;
    } else {
        // Not CLONE_THREAD: create a new thread group for the child.
        int tg_ret = thread_group_alloc(ret_ptr);
        assert(tg_ret == 0, "clone: thread_group_alloc failed");
        __atomic_store_n(&ret_ptr->thread_group->acct.mm_rss_pages,
                         vm_resident_pages(new_vm), __ATOMIC_RELAXED);

        // Inherit rlimits from the parent; acct counters start at zero.
        memmove(ret_ptr->thread_group->rlim, p->thread_group->rlim,
                sizeof(struct rlimit) * RLIMIT_NLIMITS);

        // Inherit process credentials from the parent.
        ret_ptr->thread_group->uid    = p->thread_group->uid;
        ret_ptr->thread_group->gid    = p->thread_group->gid;
        ret_ptr->thread_group->euid   = p->thread_group->euid;
        ret_ptr->thread_group->egid   = p->thread_group->egid;
        ret_ptr->thread_group->suid   = p->thread_group->suid;
        ret_ptr->thread_group->sgid   = p->thread_group->sgid;
        ret_ptr->thread_group->fsuid  = p->thread_group->fsuid;
        ret_ptr->thread_group->fsgid  = p->thread_group->fsgid;
        ret_ptr->thread_group->ngroups = p->thread_group->ngroups;
        memmove(ret_ptr->thread_group->groups, p->thread_group->groups,
                sizeof(uint32) * p->thread_group->ngroups);
        ret_ptr->thread_group->umask  = p->thread_group->umask;
        __atomic_store_n(&ret_ptr->thread_group->dumpable,
                         __atomic_load_n(&p->thread_group->dumpable,
                                         __ATOMIC_SEQ_CST),
                         __ATOMIC_SEQ_CST);
        __atomic_store_n(&ret_ptr->thread_group->no_new_privs,
                         __atomic_load_n(&p->thread_group->no_new_privs,
                                         __ATOMIC_SEQ_CST),
                         __ATOMIC_SEQ_CST);
        __atomic_store_n(&ret_ptr->thread_group->timer_slack_ns,
                         __atomic_load_n(&p->thread_group->timer_slack_ns,
                                         __ATOMIC_SEQ_CST),
                         __ATOMIC_SEQ_CST);
        __atomic_store_n(&ret_ptr->thread_group->oom_score_adj,
                         __atomic_load_n(&p->thread_group->oom_score_adj,
                                         __ATOMIC_SEQ_CST),
                         __ATOMIC_SEQ_CST);
        uint32 chrome_roles =
            __atomic_load_n(&p->thread_group->chrome_trace_roles,
                            __ATOMIC_SEQ_CST);
        if (chrome_lifecycle_thread_match(p))
            chrome_roles |= TG_CHROME_TRACE_CHILD_PROCESS;
        __atomic_store_n(&ret_ptr->thread_group->chrome_trace_roles,
                         chrome_roles, __ATOMIC_SEQ_CST);

        // Inherit dynamic linker / executable metadata so that
        // /proc/<pid>/exe and GDB's shared-library queries work
        // immediately after fork (before exec replaces them).
        ret_ptr->thread_group->interp_base = p->thread_group->interp_base;
        ret_ptr->thread_group->interp_ld   = p->thread_group->interp_ld;
        safestrcpy(ret_ptr->thread_group->interp_path,
                   p->thread_group->interp_path,
                   sizeof(ret_ptr->thread_group->interp_path));
        safestrcpy(ret_ptr->thread_group->exec_path,
                   p->thread_group->exec_path,
                   sizeof(ret_ptr->thread_group->exec_path));
        (void)thread_group_exec_snapshot_clone_locked(ret_ptr->thread_group,
                                                      p->thread_group);

        // Account the fork on the parent's counters
        ACCT_INC(p->thread_group, sched_forks);
    }

    // Initialize process group and session membership.
    // The child joins the parent's pgroup and session.
    {
        struct pgroup *pg = p->pgroup;
        struct session *s = p->session;
        assert(pg != NULL, "clone: parent has no pgroup");
        assert(s != NULL, "clone: parent has no session");

        pgroup_add_thread(pg, ret_ptr);
        session_add_thread(s, ret_ptr);

        // If this is a new thread group (not CLONE_THREAD), also add
        // the TG to the parent's pgroup.
        if (!(args->flags & CLONE_THREAD)) {
            pgroup_add_tg(pg, ret_ptr->thread_group);
        }
    }

    pid_wunlock();

    if (args->flags & CLONE_PIDFD) {
        int pidfd_ret = pidfd_install_for_thread(ret_ptr, args->pidfd);
        if (pidfd_ret < 0) {
            clone_destroy_unstarted_child(ret_ptr);
            return pidfd_ret;
        }
    }

    // CLONE_CHILD_SETTID: write child's TID into the ctid address in child's
    // memory. Done after PID assignment so we have the actual TID.
    if (args->flags & CLONE_CHILD_SETTID) {
        int tid = ret_ptr->pid;
        if (vm_copyout(ret_ptr->vm, args->ctid, (char *)&tid, sizeof(tid)) <
            0) {
            // Non-fatal: child was already created, just skip the write
        }
    }

    // CLONE_PARENT_SETTID: write child's TID into the ptid address in parent's
    // memory.
    if (args->flags & CLONE_PARENT_SETTID) {
        int tid = ret_ptr->pid;
        if (vm_copyout(p->vm, args->ptid, (char *)&tid, sizeof(tid)) < 0) {
            // Non-fatal
        }
    }

    if (chrome_lifecycle_trace_enabled() &&
        (!(args->flags & CLONE_THREAD) ||
         chrome_thread_lifecycle_trace_enabled()) &&
        (chrome_lifecycle_trace_match(p) ||
         chrome_lifecycle_trace_match(ret_ptr))) {
        uint32 child_roles = ret_ptr->thread_group ?
            __atomic_load_n(&ret_ptr->thread_group->chrome_trace_roles,
                            __ATOMIC_RELAXED) : 0;
        uint32 parent_roles = chrome_lifecycle_roles(p);
        printf("chrome-lifecycle: clone parent_pid=%d parent_tgid=%d "
               "parent_name='%s' child_pid=%d child_tgid=%d child_name='%s' "
               "flags=0x%lx thread=%d process=%d vfork=%d vm=%d "
               "now_ticks=%lu parent_pid_seq=%lu child_pid_seq=%lu "
               "parent_roles=0x%x child_roles=0x%x parent_exec='%s' "
               "child_exec='%s' esignal=%lu\n",
               p->pid, p->tgid, p->name, ret_ptr->pid, ret_ptr->tgid,
               ret_ptr->name, args->flags, !!(args->flags & CLONE_THREAD),
               !(args->flags & CLONE_THREAD), !!(args->flags & CLONE_VFORK),
               !!(args->flags & CLONE_VM), r_time(), p->pid_seq,
               ret_ptr->pid_seq, parent_roles, child_roles,
               p->thread_group ? p->thread_group->exec_path : "",
               ret_ptr->thread_group ? ret_ptr->thread_group->exec_path : "",
               ret_ptr->signal.esignal);
    }

    chrome_clone_dump_asset_state(p, ret_ptr, args->flags);
    chrome_clone_dump_fdtable_state(p, ret_ptr, args->flags);

    // Wake up the new child thread
    // Note: pi_lock no longer needed - rq_lock serializes wakeups
    scheduler_wakeup(ret_ptr);

    /* kqueue: notify EVFILT_PROC watchers of fork */
    kqueue_proc_notify(p, NOTE_FORK, ret_ptr->pid);

    // For vfork, parent blocks until child exits or execs
    if (args->flags & CLONE_VFORK) {
        scheduler_yield();
        // When we return here, child has called exec() or exit()
    }

out:
    if (IS_ERR(ret_ptr)) {
        __free_pid(); // Release the reserved PID slot
        printf("thread_clone: failed for '%s' (pid %d), flags=0x%lx err=%ld\n",
               p->name, p->pid, args->flags, PTR_ERR(ret_ptr));
        return PTR_ERR(ret_ptr);
    } else if (ret_ptr == NULL) {
        __free_pid(); // Release the reserved PID slot
        return -ENOMEM;
    }
    return ret_ptr->pid;
}
