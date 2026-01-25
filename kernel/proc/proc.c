#include "proc/proc.h"
#include "defs.h"
#include "hlist.h"
#include "list.h"
#include "memlayout.h"
#include "page.h"
#include "param.h"
#include "printf.h"
#include "proc/proc_queue.h"
#include "proc/rq.h"
#include "proc/sched.h"
#include "proc_private.h"
#include "rcu.h"
#include "riscv.h"
#include "signal.h"
#include "slab.h"
#include "spinlock.h"
#include "string.h"
#include "types.h"
#include "vfs/file.h"
#include "vfs/fs.h"
#include "vm.h"
#include "errno.h"

#define NPROC_HASH_BUCKETS 31

// Lock order for proc:
// 1. proc table lock
// 2. parent pcb lock
// 3. target pcb lock
// 4. children pcb lock

static void forkret(void);
static void forkret_entry(struct context *prev);
STATIC void freeproc(struct proc *p);

extern char trampoline[];     // trampoline.S
extern char sig_trampoline[]; // sig_trampoline.S

// Initialize a proc structure and set it to UNUSED state.
// Its spinlock and kstack will not be initialized here
static void __pcb_init(struct proc *p, struct vfs_fdtable *fdtable) {
    __proc_set_pstate(p, PSTATE_UNUSED);
    sigpending_init(p);
    sigstack_init(&p->sig_stack);
    list_entry_init(&p->sched_entry);
    list_entry_init(&p->dmp_list_entry);
    list_entry_init(&p->siblings);
    list_entry_init(&p->children);
    hlist_entry_init(&p->proctab_entry);
    spin_init(&p->lock, "proc");
    p->fs = NULL;
    p->fdtable = fdtable;
    if (p->sched_entity != NULL) {
        memset(p->sched_entity, 0, sizeof(*(p->sched_entity)));
        sched_entity_init(p->sched_entity, p);
    }
}

// Arrange proc, utrapframe, proc_fs, and vfs_fdtable on the kernel stack.
// Memory layout (from high to low addresses):
//   - struct proc (at top of stack)
//   - struct utrapframe (below proc, with 16-byte gap)
//   - struct vfs_fdtable (below proc_fs)
//   - kernel stack pointer (aligned, with 16-byte gap)
// Returns the initialized proc structure.
#define KSTACK_ARRANGE_FLAGS_TF 0x1 // place utrapframe
#define KSTACK_ARRANGE_FLAGS_ALL (KSTACK_ARRANGE_FLAGS_TF)
static struct proc *__kstack_arrange(void *kstack, size_t kstack_size,
                                     uint64 flags) {
    // Place PCB at the top of the kernel stack
    struct proc *p =
        (struct proc *)(kstack + kstack_size - sizeof(struct proc));
    uint64 next_addr = (uint64)p;

    struct utrapframe *trapframe = NULL;
    struct vfs_fdtable *fdtable = NULL;

    if (flags & KSTACK_ARRANGE_FLAGS_TF) {
        // Place utrapframe below struct proc (matching original layout)
        // Original: p->ksp = ((uint64)p - sizeof(struct utrapframe) - 16);
        //           p->trapframe = (void *)p->ksp;
        next_addr = (uint64)p - sizeof(struct utrapframe) - 16;
        next_addr &= ~0x7UL; // align to 8 bytes
        trapframe = (struct utrapframe *)next_addr;
    }

    // Allocate space for sched_entity
    next_addr = next_addr - sizeof(struct sched_entity);
    next_addr &= ~CACHELINE_MASK; // align to cache line size
    p->sched_entity = (struct sched_entity *)next_addr;

    // Initialize the proc structure
    __pcb_init(p, fdtable);

    // Set trapframe pointer
    p->trapframe = trapframe;

    // Set kernel stack pointer below the last allocated structure
    uint64 ksp = next_addr - 16;
    ksp &= ~0x7UL; // align to 8 bytes
    p->ksp = ksp;

    return p;
}

void proc_lock(struct proc *p) {
    assert(p != NULL, "proc_lock: proc is NULL");
    spin_acquire(&p->lock);
}

void proc_unlock(struct proc *p) {
    assert(p != NULL, "proc_unlock: proc is NULL");
    spin_release(&p->lock);
}

void proc_assert_holding(struct proc *p) {
    assert(p != NULL, "proc_assert_holding: proc is NULL");
    assert(spin_holding(&p->lock), "proc_assert_holding: proc lock not held");
}

// initialize the proc table.
void procinit(void) { __proctab_init(); }

// attach a newly forked process to the current process as its child.
// This function is called by fork() to set up the parent-child relationship.
// caller must hold the lock of its process (the parent) and the lock of the new
// process (the child).
void attach_child(struct proc *parent, struct proc *child) {
    assert(parent != NULL, "attach_child: parent is NULL");
    assert(child != NULL, "attach_child: child is NULL");
    assert(child != __proctab_get_initproc(),
           "attach_child: child is init process");
    proc_assert_holding(parent);
    proc_assert_holding(child);
    assert(LIST_ENTRY_IS_DETACHED(&child->siblings),
           "attach_child: child is attached to a parent");
    assert(child->parent == NULL, "attach_child: child has a parent");

    // Attach the child to the parent.
    child->parent = parent;
    list_entry_push(&parent->children, &child->siblings);
    parent->children_count++;
}

void detach_child(struct proc *parent, struct proc *child) {
    assert(parent != NULL, "detach_child: parent is NULL");
    assert(child != NULL, "detach_child: child is NULL");
    proc_assert_holding(parent);
    proc_assert_holding(child);
    assert(parent->children_count > 0, "detach_child: parent has no children");
    assert(!LIST_IS_EMPTY(&child->siblings),
           "detach_child: child is not a sibling of parent");
    assert(!LIST_ENTRY_IS_DETACHED(&child->siblings),
           "detach_child: child is already detached");
    assert(child->parent == parent,
           "detach_child: child is not a child of parent");

    // Detach the child from the parent.
    list_entry_detach(&child->siblings);
    parent->children_count--;
    child->parent = NULL;

    assert(parent->children_count > 0 || LIST_IS_EMPTY(&parent->children),
           "detach_child: parent has no children after detaching child");
}

// allocate and initialize a new proc structure.
// The newly created process will be a kernel process, which means it will not
// have user space environment set up. and return without p->lock held. If there
// are no free procs, or a memory allocation fails, return NULL. Signal actions
// will not be initialized here.
STATIC struct proc *allocproc(void *entry, uint64 arg1, uint64 arg2,
                              int kstack_order) {
    struct proc *p = NULL;
    void *kstack = NULL;

    if (kstack_order < 0 || kstack_order > PAGE_BUDDY_MAX_ORDER) {
        return NULL; // Invalid kernel stack order
    }

    int pid = __alloc_pid();
    if (pid < 0) {
        return NULL; // Failed to allocate PID
    }

    // Allocate a kernel stack page.
    kstack = page_alloc(kstack_order, PAGE_TYPE_ANON);
    if (kstack == NULL) {
        __free_pid(pid); // Release the allocated PID
        return NULL;
    }
    size_t kstack_size = (1UL << (PAGE_SHIFT + kstack_order));
    memset(kstack + kstack_size - PAGE_SIZE, 0, PAGE_SIZE);

    // Arrange proc, utrapframe, fs_struct, and vfs_fdtable on the kernel stack
    p = __kstack_arrange(kstack, kstack_size, KSTACK_ARRANGE_FLAGS_ALL);

    // Set up new context to start executing at forkret,
    // which returns to user space.
    p->kstack_order = kstack_order;
    p->kstack = (uint64)kstack;
    memset(&p->sched_entity->context, 0, sizeof(p->sched_entity->context));
    p->sched_entity->context.ra = (uint64)entry;
    p->sched_entity->context.sp = p->ksp;
    p->sched_entity->context.s0 = 0;
    p->kentry = (uint64)entry;
    p->arg[0] = arg1;
    p->arg[1] = arg2;

    sched_entity_init(p->sched_entity, p);

    p->pid = pid;
    proctab_proc_add(p);
    return p;
}

static void __kernel_proc_entry(struct context *prev) {
    assert(prev != NULL, "kernel_proc_entry: prev context is NULL");
    context_switch_finish(proc_from_context(prev), myproc());

    // Set up the kernel stack and context for the new process.
    int (*entry)(uint64, uint64) = (void *)myproc()->kentry;
    int ret = entry(myproc()->arg[0], myproc()->arg[1]);
    exit(ret);
}

// create a new kernel process, which runs the function entry.
// The newly created functions are sleeping.
// Kernel thread will be attached to the init process as its child.
int kernel_proc_create(const char *name, struct proc **retp, void *entry,
                       uint64 arg1, uint64 arg2, int stack_order) {
    struct proc *p = allocproc(entry, arg1, arg2, stack_order);
    if (p == NULL) {
        *retp = NULL;
        return -1; // Allocation failed
    }
    struct proc *initproc = __proctab_get_initproc();
    assert(initproc != NULL, "kernel_proc_create: initproc is NULL");

    // Clone fs_struct from initproc so kernel process has valid cwd/root
    struct fs_struct *fs_clone = NULL;
    if (initproc->fs != NULL) {
        fs_clone = vfs_struct_clone(initproc->fs, 0);
        if (IS_ERR_OR_NULL(fs_clone)) {
            freeproc(p);
            *retp = NULL;
            return -1; // Failed to clone fs_struct
        }
    }

    // Set up the context BEFORE making the process visible to scheduler
    p->sched_entity->context.ra = (uint64)__kernel_proc_entry;
    p->kentry = (uint64)entry;
    p->arg[0] = arg1;
    p->arg[1] = arg2;

    proc_lock(initproc);
    proc_lock(p);
    p->fs = fs_clone;
    attach_child(initproc, p);
    proc_unlock(initproc);
    // Newly allocated process is a kernel process
    assert(!PROC_USER_SPACE(p),
           "kernel_proc_create: new proc is a user process");
    safestrcpy(p->name, name ? name : "kproc", sizeof(p->name));
    __proc_set_pstate(p, PSTATE_UNINTERRUPTIBLE);
    if (retp != NULL) {
        *retp = p;
    }

    proc_unlock(p);
    return p->pid;
}

// Initialize the current context as an idle process.
// This function is called during CPU initialization.
// Idle processes will never be added to the scheduler's ready queue,
// and it will be scheduled only when there are no other running processes.
// Idle processes will also not be added to process table
// in entry.S:
//   # with a KERNEL_STACK_SIZE-byte stack per CPU.
void idle_proc_init(void) {
    struct proc *p = NULL;
    void *kstack = NULL;

    // Allocate a kernel stack page.
    size_t kstack_size = KERNEL_STACK_SIZE;
    kstack = (void *)(r_sp() & (~(kstack_size - 1)));

    // Arrange proc on the kernel stack (idle proc doesn't need
    // trapframe/fs/fdtable)
    assert((PAGE_SIZE << KERNEL_STACK_ORDER) == kstack_size,
           "idle_proc_init: invalid KERNEL_STACK_ORDER");
    p = __kstack_arrange(kstack, kstack_size, 0);
    assert(p != NULL, "idle_proc_init: failed to arrange kstack");

    // Set up new context to start executing at forkret,
    // which returns to user space.
    p->kstack_order = KERNEL_STACK_ORDER;
    p->kstack = (uint64)kstack;
    strncpy(p->name, "idle", sizeof(p->name));
    __proc_set_pstate(p, PSTATE_RUNNING);
    mycpu()->proc = p;
    mycpu()->idle_proc = p;

    // Mark this CPU as active in the rq subsystem
    rq_cpu_activate(cpuid());

    // Set idle process scheduling attributes using the new sched_attr API
    struct sched_attr attr;
    sched_attr_init(&attr);
    attr.priority = IDLE_PRIORITY;          // Lowest priority for idle process
    attr.affinity_mask = (1ULL << cpuid()); // Pin to current CPU
    sched_setattr(p->sched_entity, &attr);

    rq_lock_current();
    struct rq *idle_rq = GET_RQ_FOR_CURRENT(IDLE_MAJOR_PRIORITY);
    // Set on_rq before enqueue (matches the pattern in __do_scheduler_wakeup)
    smp_store_release(&p->sched_entity->on_rq, 1);
    rq_enqueue_task(idle_rq, p->sched_entity);
    rq_unlock_current();
    // Idle process is currently on CPU
    smp_store_release(&p->sched_entity->on_cpu, 1);

    printf("CPU %ld idle process initialized at kstack 0x%lx\n", cpuid(),
           (uint64)kstack);
}

// RCU callback to free proc kernel stack after grace period
// IMPORTANT: Read all needed values from proc BEFORE calling page_free,
// since page_free will free the memory containing the proc structure.
static void freeproc_rcu_callback(void *data) {
    struct proc *p = (struct proc *)data;
    // Copy kstack info to local variables BEFORE freeing
    uint64 kstack_addr = p->kstack;
    int kstack_order = p->kstack_order;
    // Now free the kernel stack - proc structure is gone after this, never
    // access p again
    page_free((void *)kstack_addr, kstack_order);
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must not be held on entry.
STATIC void freeproc(struct proc *p) {
    assert(p != NULL, "freeproc called with NULL proc");
    proc_lock(p);
    assert(!PROC_AWOKEN(p), "freeproc called with a runnable proc");
    assert(!PROC_SLEEPING(p), "freeproc called with a sleeping proc");
    assert(p->kstack_order >= 0 && p->kstack_order <= PAGE_BUDDY_MAX_ORDER,
           "freeproc: invalid kstack_order %d", p->kstack_order);

    if (p->sigacts)
        sigacts_free(p->sigacts);
    if (p->vm != NULL) {
        proc_freepagetable(p);
    }
    // Purge any remaining pending signals (e.g., SIGKILL) before destroy
    // assertions.
    sigpending_empty(p, 0);
    sigpending_destroy(p);

    // Remove from process table (requires proc lock to be held)
    proctab_proc_remove(p);

    proc_unlock(p);

    __free_pid(p->pid); // Mark one PID is freed

    // Defer freeing of the kernel stack until after the RCU grace period.
    // This ensures all RCU readers have finished accessing the proc structure.
    call_rcu(&p->rcu_head, freeproc_rcu_callback, p);
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
int proc_pagetable(struct proc *p) {
    // Create a new VM structure for the process.
    p->vm = vm_init((uint64)p->trapframe);
    if (p->vm == NULL) {
        return -1; // Failed to initialize VM
    }
    return 0;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(struct proc *p) {
    vm_put(p->vm);
    p->vm = NULL;
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02, 0x97,
                    0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02, 0x93, 0x08,
                    0x70, 0x00, 0x73, 0x00, 0x00, 0x00, 0x93, 0x08, 0x20,
                    0x00, 0x73, 0x00, 0x00, 0x00, 0xef, 0xf0, 0x9f, 0xff,
                    0x2f, 0x69, 0x6e, 0x69, 0x74, 0x00, 0x00, 0x24, 0x10,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void init_entry(struct context *prev) {
    // When we arrive here from context switch, we hold the rq lock.
    // Finish the context switch first to release the rq lock properly.
    context_switch_finish(proc_from_context(prev), myproc());

    // Now do post-init work without holding any scheduler locks
    start_kernel_post_init();

    // Return to user space via forkret
    forkret();
}

// Set up first user process.
void userinit(void) {
    struct proc *p;

    p = allocproc(init_entry, 0, 0, KERNEL_STACK_ORDER);
    assert(p != NULL, "userinit: allocproc failed");
    printf("Init process kernel stack size order: %d\n", p->kstack_order);

    // Allocate pagetable for the process.
    assert(proc_pagetable(p) == 0, "userinit: proc_pagetable failed");

    // // printf user pagetable
    // printf("\nuser pagetable after allocproc:\n");
    // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
    // printf("\n");

    __proctab_set_initproc(p);

    // allocate one user page and copy initcode's instructions
    // and data into it.
    uint64 ustack_top = USTACKTOP;
    printf("user stack top at 0x%lx\n", ustack_top);
    proc_lock(p);
    uint64 flags = VM_FLAG_EXEC | VM_FLAG_READ | VM_FLAG_USERMAP;
    assert(sizeof(initcode) <= PGSIZE, "userinit: initcode too large");
    void *initcode_page = page_alloc(0, PAGE_TYPE_ANON);
    assert(initcode_page != NULL, "userinit: page_alloc failed for initcode");
    memset(initcode_page, 0, PGSIZE);
    memmove(initcode_page, initcode, sizeof(initcode));
    assert(vma_mmap(p->vm, UVMBOTTOM, PGSIZE, flags, NULL, 0, initcode_page) ==
               0,
           "userinit: vma_mmap failed");
    // myproc() hasn't been set yet, so we can call createstack without holding
    // the vm lock
    assert(vm_createstack(p->vm, ustack_top, USERSTACK * PGSIZE) == 0,
           "userinit: vm_createstack failed");

    // allocate signal actions for the process
    p->sigacts = sigacts_init();
    assert(p->sigacts != NULL, "userinit: sigacts_init failed");

    // printf("\nuser pagetable after uvmfirst:\n");
    // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
    // printf("\n");

    // prepare for the very first "return" from kernel to user.
    p->trapframe->trapframe.sepc = UVMBOTTOM; // user program counter
    p->trapframe->trapframe.sp = USTACKTOP;   // user stack pointer

    safestrcpy(p->name, "initcode", sizeof(p->name));

    PROC_SET_USER_SPACE(p);

    proc_unlock(p);

    // Set init process scheduling attributes using the new sched_attr API
    struct sched_attr attr;
    sched_attr_init(&attr);
    // Use default priority and allow running on all CPUs
    sched_setattr(p->sched_entity, &attr);

    // Don't forget to wake up the process.
    spin_acquire(&p->sched_entity->pi_lock);
    __proc_set_pstate(p, PSTATE_UNINTERRUPTIBLE);
    scheduler_wakeup(p);
    spin_release(&p->sched_entity->pi_lock);
}

/*
 * install_user_root - Initialize process filesystem state
 *
 * Sets up the initial current working directory for the init process.
 * Uses VFS interfaces instead of legacy namei/idup:
 *   - vfs_namei() to look up "/" path
 *   - vfs_inode_get_ref() to set p->fs->cwd
 *   - vfs_iput() to release lookup reference
 *
 * The process struct now uses p->fs->cwd (vfs_inode_ref) instead of
 * the legacy p->cwd (struct inode*).
 */
void install_user_root(void) {
    struct proc *p = myproc();

    // Use VFS to look up the root directory
    struct vfs_inode *root_inode = vfs_namei("/", 1);
    if (root_inode == NULL) {
        panic("install_user_root: cannot find root directory");
    }

    assert(p->fs != NULL, "install_user_root: process fs_struct is NULL");

    proc_lock(p);
    PROC_SET_USER_SPACE(p);
    proc_unlock(p);

    // Set the VFS cwd to root
    vfs_struct_lock(p->fs);
    vfs_inode_get_ref(root_inode, &p->fs->cwd);
    vfs_struct_unlock(p->fs);

    // Release the lookup reference (cwd now holds its own ref)
    vfs_iput(root_inode);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n) {
    struct proc *p = myproc();

    return vm_growheap(p->vm, n);
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void) {
    struct proc *np;
    struct proc *p = myproc();

    if (!PROC_USER_SPACE(p)) {
        return -1;
    }

    // Allocate process.
    if ((np = allocproc(forkret_entry, 0, 0, p->kstack_order)) == 0) {
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

    spin_acquire(&np->sched_entity->pi_lock);
    scheduler_wakeup(np);
    spin_release(&np->sched_entity->pi_lock);

    return np->pid;
}

// Pass p's abandoned children to init.
// Caller must not hold p->lock.
void reparent(struct proc *p) {
    struct proc *initproc = __proctab_get_initproc();
    struct proc *child, *tmp;
    bool found = false;

    assert(initproc != NULL, "reparent: initproc is NULL");
    assert(p != initproc, "reparent: p is init process");

    proc_lock(initproc);
    proc_lock(p);

    list_foreach_node_safe(&p->children, child, tmp, siblings) {
        // make sure the child isn't still in exit() or swtch().
        proc_lock(child);
        detach_child(p, child);
        attach_child(initproc, child);
        proc_unlock(child);
        found = true;
    }

    proc_unlock(p);
    proc_unlock(initproc);

    if (found)
        wakeup_interruptible(initproc);
}

// Yield the CPU after the process becomes zombie.
// The caller need to hold p->lock, and not to hold the rq_lock.
// This is to ensure that its parent can be scheduled after it becomes zombie
// and not to wake up before it becomes zombie.
static void __exit_yield(int status) {
    struct proc *p = myproc();
    proc_lock(p);
    p->xstate = status;
    __proc_set_pstate(p, PSTATE_ZOMBIE);
    proc_unlock(p);
    scheduler_yield();
    panic("exit: __exit_yield should not return");
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status) {
    struct proc *p = myproc();

    // VFS file descriptor table cleanup (closes all VFS files)
    vfs_fdtable_put(p->fdtable);
    p->fdtable = NULL;

    assert(p != __proctab_get_initproc(), "init exiting");

    vfs_struct_put(p->fs);
    p->fs = NULL;

    // Give any children to init.
    reparent(p);

    __exit_yield(status);

    // Jump into the scheduler, never to return.
    yield();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr) {
    int pid = -1;
    int xstate = 0;
    struct proc *p = myproc();
    struct proc *child, *tmp;
    bool needs_yield = false;

    proc_lock(p);
    for (;;) {
        // Scan through table looking for exited children.
        list_foreach_node_safe(&p->children, child, tmp, siblings) {
            // make sure the child isn't still in exit() or swtch().
            proc_lock(child);
            if (PROC_ZOMBIE(child)) {
                // Make sure the zombie child has fully switched out of CPU
                if (smp_load_acquire(&child->sched_entity->on_cpu)) {
                    // Still on CPU, mark needs yields and try again later
                    needs_yield = true;
                    proc_unlock(child);
                    continue;
                }
                // Found one.
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
        if (p->children_count == 0 || signal_terminated(p)) {
            pid = -1;
            goto ret;
        }

        // Wait for a child to exit.
        if (needs_yield) {
            proc_unlock(p);
            needs_yield = false;
            yield();
            proc_lock(p);
        } else {
            scheduler_sleep(&p->lock, PSTATE_INTERRUPTIBLE); // DOC: wait-sleep
        }
    }

ret:
    proc_unlock(p);
    if (pid >= 0 && addr != 0) {
        // copy xstate to user.
        if (either_copyout(1, addr, (char *)&xstate, sizeof(xstate)) < 0) {
            return -1;
        }
    }
    return pid;
}

// Give up the CPU for one scheduling round.
void yield(void) { scheduler_yield(); }

// Entry wrapper for forked user processes.
// This is called as the entry point from context switch.
static void forkret_entry(struct context *prev) {
    assert(PROC_USER_SPACE(myproc()),
           "kernel process %d tries to return to user space", myproc()->pid);
    assert(prev != NULL, "forkret_entry: prev context is NULL");

    // Finish the context switch first - this releases the rq lock
    context_switch_finish(proc_from_context(prev), myproc());

    // Now safe to do the rest without holding scheduler locks
    forkret();
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret_entry, which calls this after context_switch_finish.
static void forkret(void) {
    // The scheduler will disable interrupts to assure the atomicity of
    // the scheduler operations. For processes that gave up CPU by calling
    // yield(),
    //   yield() would restore the previous interruption state when switched
    //   back.
    // But at here, we need to enable interrupts for the first time.
    intr_on();

    // printf("forkret: process %d is running\n", myproc()->pid);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    usertrapret();
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid, int signum) {
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = myproc();
    info.info.si_pid = myproc()->pid;

    return signal_send(pid, &info);
}

int killed(struct proc *p) {
    int k;

    proc_lock(p);
    k = signal_terminated(p);
    proc_unlock(p);
    return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len) {
    struct proc *p = myproc();
    if (user_dst) {
        return vm_copyout(p->vm, dst, src, len);
    } else {
        memmove((char *)dst, src, len);
        return 0;
    }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len) {
    struct proc *p = myproc();
    if (user_src) {
        return vm_copyin(p->vm, dst, src, len);
    } else {
        memmove(dst, (char *)src, len);
        return 0;
    }
}
