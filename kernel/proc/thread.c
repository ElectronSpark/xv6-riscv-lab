#include "proc/thread.h"
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

#define NR_THREAD_HASH_BUCKETS 31

// Lock order:
// 1. pid_lock (rwlock) — protects parent-child hierarchy and proc table
// 2. individual thread lock (tcb_lock) — protects thread-local state

extern char trampoline[];     // trampoline.S
extern char sig_trampoline[]; // sig_trampoline.S

// Initialize a thread structure and set it to UNUSED state.
// Its spinlock and kstack will not be initialized here
static void __pcb_init(struct thread *p, struct vfs_fdtable *fdtable) {
    __thread_state_set(p, THREAD_UNUSED);
    sigpending_init(p);
    sigstack_init(&p->signal.sig_stack);
    list_entry_init(&p->sched_entry);
    list_entry_init(&p->dmp_list_entry);
    list_entry_init(&p->siblings);
    list_entry_init(&p->children);
    hlist_entry_init(&p->proctab_entry);
    spin_init(&p->lock, "thread");
    p->fs = NULL;
    p->fdtable = fdtable;
    if (p->sched_entity != NULL) {
        memset(p->sched_entity, 0, sizeof(*(p->sched_entity)));
        sched_entity_init(p->sched_entity, p);
    }
}

// Arrange thread, utrapframe, thread_fs, and vfs_fdtable on the kernel stack.
// Memory layout (from high to low addresses):
//   - struct thread (at top of stack)
//   - struct utrapframe (below thread, with 16-byte gap)
//   - struct vfs_fdtable (below thread_fs)
//   - kernel stack pointer (aligned, with 16-byte gap)
// Returns the initialized thread structure.
#define KSTACK_ARRANGE_FLAGS_TF 0x1 // place utrapframe
#define KSTACK_ARRANGE_FLAGS_ALL (KSTACK_ARRANGE_FLAGS_TF)
static struct thread *__kstack_arrange(void *kstack, size_t kstack_size,
                                       uint64 flags) {
    // Place PCB at the top of the kernel stack
    struct thread *p =
        (struct thread *)(kstack + kstack_size - sizeof(struct thread));
    uint64 next_addr = (uint64)p;

    struct utrapframe *trapframe = NULL;
    struct vfs_fdtable *fdtable = NULL;

    if (flags & KSTACK_ARRANGE_FLAGS_TF) {
        // Place utrapframe below struct thread (matching original layout)
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

    // Initialize the thread structure
    __pcb_init(p, fdtable);

    // Set trapframe pointer
    p->trapframe = trapframe;

    // Set kernel stack pointer below the last allocated structure
    uint64 ksp = next_addr - 16;
    ksp &= ~0x7UL; // align to 8 bytes
    p->ksp = ksp;

    return p;
}

void tcb_lock(struct thread *p) {
    assert(p != NULL, "tcb_lock: thread is NULL");
    spin_lock(&p->lock);
}

void tcb_unlock(struct thread *p) {
    assert(p != NULL, "tcb_unlock: thread is NULL");
    spin_unlock(&p->lock);
}

void proc_assert_holding(struct thread *p) {
    assert(p != NULL, "proc_assert_holding: thread is NULL");
    assert(spin_holding(&p->lock), "proc_assert_holding: thread lock not held");
}

// initialize the proc table.
void thread_init(void) { __proctab_init(); }

// Attach a newly forked thread to the given parent as its child.
// This function is called by fork/clone to set up the parent-child
// relationship. Caller must hold pid_wlock.
void attach_child(struct thread *parent, struct thread *child) {
    assert(parent != NULL, "attach_child: parent is NULL");
    assert(child != NULL, "attach_child: child is NULL");
    assert(child != __proctab_get_initproc(),
           "attach_child: child is init process");
    pid_assert_wholding(); // Must hold pid lock to modify parent-child
                           // relationship
    assert(LIST_ENTRY_IS_DETACHED(&child->siblings),
           "attach_child: child is attached to a parent");
    assert(child->parent == NULL, "attach_child: child has a parent");

    // Attach the child to the parent.
    child->parent = parent;
    list_entry_push(&parent->children, &child->siblings);
    parent->children_count++;
}

void detach_child(struct thread *parent, struct thread *child) {
    assert(parent != NULL, "detach_child: parent is NULL");
    assert(child != NULL, "detach_child: child is NULL");
    pid_assert_wholding(); // Must hold pid lock to modify parent-child
                           // relationship
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

// Allocate and initialize a bare thread structure.
// The newly created thread will be a kernel thread (no user space set up),
// with pid set to -1. The caller is responsible for allocating a PID,
// assigning it, and adding the thread to the proc table under pid_wlock.
// Signal actions will not be initialized here.
// Return ERR_PTR on failure.
struct thread *thread_create(void *entry, uint64 arg1, uint64 arg2,
                             int kstack_order) {
    struct thread *p = NULL;
    void *kstack = NULL;

    if (kstack_order < 0 || kstack_order > PAGE_BUDDY_MAX_ORDER) {
        return ERR_PTR(-EINVAL); // Invalid kernel stack order
    }

    // Allocate a kernel stack page.
    kstack = page_alloc(kstack_order, PAGE_TYPE_ANON);
    if (kstack == NULL) {
        return ERR_PTR(-ENOMEM);
    }
    size_t kstack_size = (1UL << (PAGE_SHIFT + kstack_order));
    memset(kstack + kstack_size - PAGE_SIZE, 0, PAGE_SIZE);

    // Arrange thread, utrapframe, fs_struct, and vfs_fdtable on the kernel
    // stack
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
    p->pid = -1;

    sched_entity_init(p->sched_entity, p);

    return p;
}

static void __kthread_entry(struct context *prev) {
    assert(prev != NULL, "kthread_entry: prev context is NULL");
    context_switch_finish(thread_from_context(prev), current, 0);
    mycpu()->noff = 0; // in a new thread, noff should be 0
    intr_on();
    // Note quiescent state for RCU - context switch is a quiescent state.
    // Callback processing is now handled by per-CPU RCU kthreads.
    rcu_check_callbacks();

    // Set up the kernel stack and context for the new thread.
    int (*entry)(uint64, uint64) = (void *)current->kentry;
    int ret = entry(current->arg[0], current->arg[1]);
    exit(ret);
}

// Create a new kernel thread which runs the given entry function.
// The newly created thread starts in UNINTERRUPTIBLE state.
// It is attached to the init process as its child.
// Returns the new thread's PID on success, or -1 on failure.
int kthread_create(const char *name, struct thread **retp, void *entry,
                   uint64 arg1, uint64 arg2, int stack_order) {
    rcu_read_lock();
    struct thread *initproc = __proctab_get_initproc();
    assert(initproc != NULL, "kthread_create: initproc is NULL");

    // Reserve a PID slot (lock-free)
    if (__alloc_pid() < 0) {
        rcu_read_unlock();
        *retp = NULL;
        return -1; // No available PID slots
    }

    struct thread *p = thread_create(entry, arg1, arg2, stack_order);
    if (IS_ERR_OR_NULL(p)) {
        *retp = NULL;
        __free_pid(); // Release the reserved PID slot
        rcu_read_unlock();
        return -1; // Allocation failed
    }

    // Clone fs_struct from initproc so kernel thread has valid cwd/root
    struct fs_struct *fs_clone = NULL;
    if (initproc->fs != NULL) {
        fs_clone = vfs_struct_clone(initproc->fs, 0);
        if (IS_ERR_OR_NULL(fs_clone)) {
            __free_pid(); // Release the reserved PID slot
            thread_destroy(p);
            rcu_read_unlock();
            *retp = NULL;
            return -1; // Failed to clone fs_struct
        }
    }

    // Set up the context BEFORE making the thread visible to scheduler
    p->sched_entity->context.ra = (uint64)__kthread_entry;
    p->kentry = (uint64)entry;
    p->arg[0] = arg1;
    p->arg[1] = arg2;
    p->fs = fs_clone;
    safestrcpy(p->name, name ? name : "kthread", sizeof(p->name));
    __thread_state_set(p, THREAD_UNINTERRUPTIBLE);

    // proctab_proc_add assigns the actual PID number
    pid_wlock();
    attach_child(initproc, p);
    proctab_proc_add(p);
    pid_wunlock();

    rcu_read_unlock();
    if (retp != NULL) {
        *retp = p;
    }

    return p->pid;
}

// Initialize the current context as an idle process.
// This function is called during CPU initialization.
// Idle processes will never be added to the scheduler's ready queue,
// and it will be scheduled only when there are no other running threads.
// Idle processes will also not be added to process table
// in entry.S:
//   # with a KERNEL_STACK_SIZE-byte stack per CPU.
void idle_thread_init(void) {
    struct thread *p = NULL;
    void *kstack = NULL;

    // Allocate a kernel stack page.
    size_t kstack_size = KERNEL_STACK_SIZE;
    kstack = (void *)(r_sp() & (~(kstack_size - 1)));

    // Arrange thread on the kernel stack (idle process doesn't need
    // trapframe/fs/fdtable)
    assert((PAGE_SIZE << KERNEL_STACK_ORDER) == kstack_size,
           "idle_thread_init: invalid KERNEL_STACK_ORDER");
    p = __kstack_arrange(kstack, kstack_size, 0);
    assert(p != NULL, "idle_thread_init: failed to arrange kstack");

    // Set up new context to start executing at forkret,
    // which returns to user space.
    p->kstack_order = KERNEL_STACK_ORDER;
    p->kstack = (uint64)kstack;
    strncpy(p->name, "idle", sizeof(p->name));
    __thread_state_set(p, THREAD_RUNNING);
    mycpu()->proc = p;
    mycpu()->idle_thread = p;

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
    rq_enqueue_task(idle_rq, p->sched_entity);
    rq_unlock_current();
    // Idle process is currently on CPU
    smp_store_release(&p->sched_entity->on_cpu, 1);

    printf("CPU %ld idle process initialized at kstack 0x%lx\n", cpuid(),
           (uint64)kstack);
}

// RCU callback to free thread kernel stack after grace period
// IMPORTANT: Read all needed values from thread BEFORE calling page_free,
// since page_free will free the memory containing the thread structure.
static void thread_destroy_rcu_callback(void *data) {
    struct thread *p = (struct thread *)data;
    // Copy kstack info to local variables BEFORE freeing
    uint64 kstack_addr = p->kstack;
    int kstack_order = p->kstack_order;
    // Now free the kernel stack - thread structure is gone after this, never
    // access p again
    page_free((void *)kstack_addr, kstack_order);
}

// free a thread structure and the data hanging from it,
// including user pages.
// Caller must make sure the thread is detached from the process table and won't
// be scheduled anymore before calling this function.
void thread_destroy(struct thread *p) {
    assert(p != NULL, "thread_destroy called with NULL thread");
    assert(!THREAD_AWOKEN(p), "thread_destroy called with a runnable thread");
    assert(!THREAD_SLEEPING(p), "thread_destroy called with a sleeping thread");
    assert(p->kstack_order >= 0 && p->kstack_order <= PAGE_BUDDY_MAX_ORDER,
           "thread_destroy: invalid kstack_order %d", p->kstack_order);

    if (p->sigacts != NULL) {
        sigacts_put(p->sigacts);
        p->sigacts = NULL;
    }

    if (p->vm != NULL) {
        vm_put(p->vm);
        p->vm = NULL;
    }

    if (p->fdtable != NULL) {
        vfs_fdtable_put(p->fdtable);
        p->fdtable = NULL;
    }

    if (p->fs != NULL) {
        vfs_struct_put(p->fs);
        p->fs = NULL;
    }

    // Purge any remaining pending signals (e.g., SIGKILL) before destroy
    // assertions.
    sigpending_empty(p, 0);
    sigpending_destroy(p);

    // Defer freeing of the kernel stack until after the RCU grace period.
    // This ensures all RCU readers have finished accessing the thread
    // structure.
    call_rcu(&p->rcu_head, thread_destroy_rcu_callback, p);
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
    context_switch_finish(thread_from_context(prev), current, 0);
    mycpu()->noff = 0; // in a new thread, noff should be 0
    intr_on();

    // Now do post-init work without holding any scheduler locks
    start_kernel_post_init();

    // Return to user space via forkret
    smp_mb();
    usertrapret();
}

// Set up first user thread.
void userinit(void) {
    struct thread *p;

    assert(__alloc_pid() == 0, "userinit: __alloc_pid failed");

    p = thread_create(init_entry, 0, 0, KERNEL_STACK_ORDER);
    assert(!IS_ERR_OR_NULL(p), "userinit: thread_create failed");

    // proctab_proc_add assigns the actual PID number
    pid_wlock();
    proctab_proc_add(p);
    pid_wunlock();

    printf("Init process kernel stack size order: %d\n", p->kstack_order);

    // Allocate pagetable for the thread.
    vm_t *vm = vm_init();
    assert(!IS_ERR_OR_NULL(vm), "userinit: vm_init failed");
    p->vm = vm;

    // // printf user pagetable
    // printf("\nuser pagetable after thread_create:\n");
    // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
    // printf("\n");

    __proctab_set_initproc(p);

    // allocate one user page and copy initcode's instructions
    // and data into it.
    uint64 ustack_top = USTACKTOP;
    printf("user stack top at 0x%lx\n", ustack_top);
    tcb_lock(p);
    uint64 flags = VM_FLAG_EXEC | VM_FLAG_READ | VM_FLAG_USERMAP;
    assert(sizeof(initcode) <= PGSIZE, "userinit: initcode too large");
    void *initcode_page = page_alloc(0, PAGE_TYPE_ANON);
    assert(initcode_page != NULL, "userinit: page_alloc failed for initcode");
    memset(initcode_page, 0, PGSIZE);
    memmove(initcode_page, initcode, sizeof(initcode));
    assert(vma_mmap(p->vm, UVMBOTTOM, PGSIZE, flags, NULL, 0, initcode_page) ==
               0,
           "userinit: vma_mmap failed");
    // current hasn't been set yet, so we can call createstack without holding
    // the vm lock
    assert(vm_createstack(p->vm, ustack_top, USERSTACK * PGSIZE) == 0,
           "userinit: vm_createstack failed");

    // allocate signal actions for the thread
    p->sigacts = sigacts_init();
    assert(p->sigacts != NULL, "userinit: sigacts_init failed");

    // printf("\nuser pagetable after uvmfirst:\n");
    // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
    // printf("\n");

    // prepare for the very first "return" from kernel to user.
    p->trapframe->trapframe.sepc = UVMBOTTOM; // user program counter
    p->trapframe->trapframe.sp = USTACKTOP;   // user stack pointer

    safestrcpy(p->name, "initcode", sizeof(p->name));

    THREAD_SET_USER_SPACE(p);

    tcb_unlock(p);

    // Set init process scheduling attributes using the new sched_attr API
    struct sched_attr attr;
    sched_attr_init(&attr);
    // Use default priority and allow running on all CPUs
    sched_setattr(p->sched_entity, &attr);

    // Don't forget to wake up the thread.
    // Note: pi_lock no longer needed - rq_lock serializes wakeups
    __thread_state_set(p, THREAD_UNINTERRUPTIBLE);
    scheduler_wakeup(p);
}

/*
 * install_user_root - Initialize thread filesystem state
 *
 * Sets up the initial current working directory for the init process.
 * Uses VFS interfaces instead of legacy namei/idup:
 *   - vfs_namei() to look up "/" path
 *   - vfs_inode_get_ref() to set p->fs->cwd
 *   - vfs_iput() to release lookup reference
 *
 * The thread struct now uses p->fs->cwd (vfs_inode_ref) instead of
 * the legacy p->cwd (struct inode*).
 */
void install_user_root(void) {
    struct thread *p = current;

    // Use VFS to look up the root directory
    struct vfs_inode *root_inode = vfs_namei("/", 1);
    if (root_inode == NULL) {
        panic("install_user_root: cannot find root directory");
    }

    assert(p->fs != NULL, "install_user_root: thread fs_struct is NULL");

    tcb_lock(p);
    THREAD_SET_USER_SPACE(p);
    tcb_unlock(p);

    // Get reference to root inode BEFORE acquiring spinlock
    // (vfs_inode_get_ref may acquire the inode mutex internally)
    struct vfs_inode_ref cwd_ref;
    int ret = vfs_inode_get_ref(root_inode, &cwd_ref);
    if (ret < 0) {
        panic("install_user_root: failed to get ref to root inode");
    }

    // Set the VFS cwd to root (only assignment under spinlock)
    vfs_struct_lock(p->fs);
    p->fs->cwd = cwd_ref;
    vfs_struct_unlock(p->fs);

    // Release the lookup reference (cwd now holds its own ref)
    vfs_iput(root_inode);
}
