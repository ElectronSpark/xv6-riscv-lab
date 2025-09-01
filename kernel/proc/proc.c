#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "list.h"
#include "hlist.h"
#include "proc_queue.h"
#include "sched.h"
#include "slab.h"
#include "page.h"
#include "signal.h"
#include "vm.h"

#define NPROC_HASH_BUCKETS 31

struct cpu cpus[NCPU];

// Lock order for proc:
// 1. proc table lock
// 2. parent pcb lock
// 3. target pcb lock
// 4. children pcb lock

struct {
  struct {
    hlist_t procs;
    hlist_bucket_t buckets[NPROC_HASH_BUCKETS];
  };
  list_node_t procs_list; // List of all processes, for dumping
  struct proc *initproc;
  int nextpid;
  struct spinlock pid_lock;
} proc_table;

/* Hash table callback functions for proc table */

static ht_hash_t __proctab_hash(void *node)
{
  struct proc *p = (struct proc *)node;
  return hlist_hash_int(p->pid);
}

static int __proctab_hash_cmp(hlist_t* ht, void *node1, void *node2)
{
  struct proc *p1 = (struct proc *)node1;
  struct proc *p2 = (struct proc *)node2;
  return p1->pid - p2->pid;
}

static hlist_entry_t *__proctab_hash_get_entry(void *node)
{
  struct proc *p = (struct proc *)node;
  return &p->proctab_entry;
}

static void *__proctab_hash_get_node(hlist_entry_t *entry)
{
  return (void *)container_of(entry, struct proc, proctab_entry);
}

// initialize the proc table and pid_lock.
static void __proctab_init(void)
{
  hlist_func_t funcs = {
    .hash = __proctab_hash,
    .get_node = __proctab_hash_get_node,
    .get_entry = __proctab_hash_get_entry,
    .cmp_node = __proctab_hash_cmp,
  };
  hlist_init(&proc_table.procs, NPROC_HASH_BUCKETS, &funcs);
  spin_init(&proc_table.pid_lock, "pid_lock");
  list_entry_init(&proc_table.procs_list);
  proc_table.initproc = NULL;
  proc_table.nextpid = 1;
}

/* Lock and unlock proc table */

static void 
__proctab_lock(void)
{
  spin_acquire(&proc_table.pid_lock);
}

static void 
__proctab_unlock(void)
{
  spin_release(&proc_table.pid_lock);
}

// panic if proc_table is not locked.
static void 
__proctab_assert_locked(void)
{
  assert(spin_holding(&proc_table.pid_lock), "proc_table not locked");
}

// panic if proc_table is locked.
static void 
__proctab_assert_unlocked(void)
{
  assert(!spin_holding(&proc_table.pid_lock), "proc_table locked");
}


/* The following will assert that the process table is locked */
static void __proctab_set_initproc(struct proc *p)
{
  __proctab_assert_locked();

  assert(p != NULL, "NULL initproc");
  assert(proc_table.initproc == NULL, "initproc already set");
  proc_table.initproc = p;
}

// get the init process.
// This function won't check locking state
static struct proc *__proctab_get_initproc(void)
{
  assert(proc_table.initproc != NULL, "initproc not set");
  return proc_table.initproc;
}

// get a PCB by pid.
static struct proc *__proctab_get_pid_proc(int pid)
{
  __proctab_assert_locked();

  struct proc dummy = { .pid = pid };
  struct proc *p = hlist_get(&proc_table.procs, &dummy);
  return p;
}

// allocate a new pid.
static int __alloc_pid(void)
{
  __proctab_assert_locked();

  while (__proctab_get_pid_proc(proc_table.nextpid) != NULL) {
    proc_table.nextpid++;
  }
  int pid = proc_table.nextpid;
  proc_table.nextpid++;
  return pid;
}

static int __proctab_add(struct proc *p)
{
  __proctab_assert_locked();
  assert(p != NULL, "NULL proc passed to __proctab_add");
  assert(LIST_ENTRY_IS_DETACHED(&p->dmp_list_entry),
         "Process %d is already in the dump list", p->pid);

  struct proc *existing = hlist_put(&proc_table.procs, p);

  assert(existing != p, "Failed to add process with pid %d", p->pid);
  assert(existing == NULL, "Process with pid %d already exists", p->pid);
  // Add to the global list of processes for dumping.
  list_entry_push_back(&proc_table.procs_list, &p->dmp_list_entry);
  return 0;
}

int proctab_get_pid_proc(int pid, struct proc **pp) {
  if (!pp) {
    return -1; // Invalid argument
  }
  __proctab_lock();
  struct proc *p = __proctab_get_pid_proc(pid);
  __proctab_unlock();
  *pp = p;
  return 0;
}

extern void forkret(void);
STATIC void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S
extern char sig_trampoline[]; // sig_trampoline.S

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
// @TODO:
// void
// proc_mapkstack(pagetable_t kpgtbl, void *kstack)
// {
//   struct proc *p;

//   for(p = proc; p < &proc[NPROC]; p++) {
//     char *pa = kalloc();
//     assert(pa != 0, "kalloc failed for proc stack");
//     uint64 va = KSTACK((int) (p - proc));
//     kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W | PTE_RSW_w);
//   }
// }

// @TODO: proc_unmapkstack

// Initialize a proc structure and set it to UNUSED state.
// Its spinlock and kstack will not be initialized here
static void
__pcb_init(struct proc *p)
{
  memset(p, 0, sizeof(*p));
  __proc_set_pstate(p, PSTATE_UNUSED);
  sigpending_init(p);
  sigstack_init(&p->sig_stack);
  list_entry_init(&p->sched_entry);
  list_entry_init(&p->dmp_list_entry);
  list_entry_init(&p->siblings);
  list_entry_init(&p->children);
  hlist_entry_init(&p->proctab_entry);
  spin_init(&p->lock, "proc");
}

void
proc_lock(struct proc *p)
{
  assert(p != NULL, "proc_lock: proc is NULL");
  spin_acquire(&p->lock);
}

void
proc_unlock(struct proc *p)
{
  assert(p != NULL, "proc_unlock: proc is NULL");
  spin_release(&p->lock);
}

void
proc_assert_holding(struct proc *p)
{
  assert(p != NULL, "proc_assert_holding: proc is NULL");
  assert(spin_holding(&p->lock), "proc_assert_holding: proc lock not held");
}

// initialize the proc table.
void
procinit(void)
{
  __proctab_init();
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// attach a newly forked process to the current process as its child.
// This function is called by fork() to set up the parent-child relationship.
// caller must hold the lock of its process (the parent) and the lock of the new process (the child).
void
attach_child(struct proc *parent, struct proc *child)
{
  assert(parent != NULL, "attach_child: parent is NULL");
  assert(child != NULL, "attach_child: child is NULL");
  assert(child != __proctab_get_initproc(), "attach_child: child is init process");
  proc_assert_holding(parent);
  proc_assert_holding(child);
  assert(LIST_ENTRY_IS_DETACHED(&child->siblings), "attach_child: child is attached to a parent");
  assert(child->parent == NULL, "attach_child: child has a parent");

  // Attach the child to the parent.
  child->parent = parent;
  list_entry_push(&parent->children, &child->siblings);
  parent->children_count++;
}

void
detach_child(struct proc *parent, struct proc *child)
{
  assert(parent != NULL, "detach_child: parent is NULL");
  assert(child != NULL, "detach_child: child is NULL");
  proc_assert_holding(parent);
  proc_assert_holding(child);
  assert(parent->children_count > 0, "detach_child: parent has no children");
  assert(!LIST_IS_EMPTY(&child->siblings), "detach_child: child is not a sibling of parent");
  assert(!LIST_ENTRY_IS_DETACHED(&child->siblings), "detach_child: child is already detached");
  assert(child->parent == parent, "detach_child: child is not a child of parent");

  // Detach the child from the parent.
  list_entry_detach(&child->siblings);
  parent->children_count--;
  child->parent = NULL;

  assert(parent-> children_count > 0 || LIST_IS_EMPTY(&parent->children),
         "detach_child: parent has no children after detaching child");
}

// allocate and initialize a new proc structure.
// The newly created process will be a kernel process, which means it will not have
// user space environment set up.
// and return without p->lock held.
// If there are no free procs, or a memory allocation fails, return NULL.
// Signal actions will not be initialized here.
STATIC struct proc*
allocproc(void *entry, uint64 arg1, uint64 arg2, int kstack_order)
{
  struct proc *p = NULL;
  void *kstack = NULL;

  if (kstack_order < 0 || kstack_order > PAGE_BUDDY_MAX_ORDER) {
    return NULL; // Invalid kernel stack order
  }

  __proctab_assert_unlocked();

  // Allocate a kernel stack page.
  kstack = page_alloc(kstack_order, PAGE_FLAG_ANON);
  if(kstack == NULL){
    return NULL;
  }
  size_t kstack_size = (1UL << (PAGE_SHIFT + kstack_order));
  memset(kstack, 0, kstack_size);
  // Place PCB at the top of the kernel stack
  p = (struct proc *)(kstack  + kstack_size - sizeof(struct proc));
  __pcb_init(p);

  // Set up new context to start executing at forkret,
  // which returns to user space.
  p->kstack_order = kstack_order;
  p->kstack = (uint64)kstack;
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)entry;
  p->ksp = ((uint64)p - sizeof(struct trapframe) - 16);
  p->ksp &= ~0x7UL; // align to 8 bytes
  p->trapframe = (void *)p->ksp;
  p->ksp -= 16;
  p->ksp &= ~0x7UL; // align to 8 bytes
  p->context.sp = p->ksp;
  p->kentry = (uint64)entry;
  p->arg[0] = arg1;
  p->arg[1] = arg2;

  __proctab_lock();
  p->pid = __alloc_pid();
  if (__proctab_add(p) != 0) {
    __proctab_unlock();
    freeproc(p);
    return NULL;
  }

  __proctab_unlock();
  return p;
}

static void __kernel_proc_entry(void) {
  // Still holding p->lock from scheduler.
  sched_unlock(); // Release the scheduler lock.
  proc_unlock(myproc());
  intr_on();
  // Set up the kernel stack and context for the new process.
  int (*entry)(uint64, uint64) = (void*)myproc()->kentry;
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
  proc_lock(initproc);
  proc_lock(p);
  attach_child(initproc, p);
  proc_unlock(initproc);

  p->context.ra = (uint64)__kernel_proc_entry;
  p->kentry = (uint64)entry;
  p->arg[0] = arg1;
  p->arg[1] = arg2;
  // Newly allocated process is a kernel process
  assert(!PROC_USER_SPACE(p), "kernel_proc_create: new proc is a user process");
  safestrcpy(p->name, name ? name : "kproc", sizeof(p->name));
  __proc_set_pstate(p, PSTATE_UNINTERRUPTIBLE);
  if (retp != NULL) {
    *retp = p;
  }

  proc_unlock(p);
  return p->pid;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must not be held.
STATIC void
freeproc(struct proc *p)
{
  assert(p != NULL, "freeproc called with NULL proc");
  assert(!PROC_AWOKEN(p), "freeproc called with a runnable proc");
  assert(!PROC_SLEEPING(p), "freeproc called with a sleeping proc");

  __proctab_lock();
  proc_lock(p);
  assert(p->kstack_order >= 0 && p->kstack_order <= PAGE_BUDDY_MAX_ORDER,
         "freeproc: invalid kstack_order %d", p->kstack_order);
  struct proc *existing = hlist_pop(&proc_table.procs, p);
  // Remove from the global list of processes for dumping.
  list_entry_detach(&p->dmp_list_entry);
  __proctab_unlock();

  assert(existing == NULL || existing == p, "freeproc called with a different proc");
  if(p->sigacts)
    sigacts_free(p->sigacts);
  if (p->vm != NULL) {
    proc_freepagetable(p);
  }
  // Purge any remaining pending signals (e.g., SIGKILL) before destroy assertions.
  sigpending_empty(p, 0);
  sigpending_destroy(p);

  page_free((void *)p->kstack, p->kstack_order);
  pop_off();  // PCB has been freed, so no need to keep noff counter increased
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
int
proc_pagetable(struct proc *p)
{
  // Create a new VM structure for the process.
  p->vm = vm_init((uint64)p->trapframe);
  if (p->vm == NULL) {
    return -1; // Failed to initialize VM
  }
  return 0;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(struct proc *p)
{
  vm_destroy(p->vm);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x10, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc(forkret, 0, 0, KERNEL_STACK_ORDER);
  assert(p != NULL, "userinit: allocproc failed");
  printf("Init process kernel stack size order: %d\n", p->kstack_order);

  // Allocate pagetable for the process.
  assert(proc_pagetable(p) == 0, "userinit: proc_pagetable failed");

  // // printf user pagetable
  // printf("\nuser pagetable after allocproc:\n");
  // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
  // printf("\n");

  __proctab_lock();
  __proctab_set_initproc(p);
  __proctab_unlock();
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uint64 ustack_top = USTACKTOP;
  printf("user stack top at 0x%lx\n", ustack_top);
  proc_lock(p);
  uint64 flags = VM_FLAG_EXEC | VM_FLAG_READ | VM_FLAG_USERMAP;
  assert(sizeof(initcode) <= PGSIZE, "userinit: initcode too large");
  void *initcode_page = page_alloc(0, PAGE_FLAG_ANON);
  assert(initcode_page != NULL, "userinit: page_alloc failed for initcode");
  memset(initcode_page, 0, PGSIZE);
  memmove(initcode_page, initcode, sizeof(initcode));
  assert(vma_mmap(p->vm, UVMBOTTOM, PGSIZE, flags, NULL, 0, initcode_page) == 0,
         "userinit: vma_mmap failed");
  assert(vm_createstack(p->vm, ustack_top, USERSTACK * PGSIZE) == 0,
         "userinit: vm_createstack failed");

  // allocate signal actions for the process
  p->sigacts = sigacts_init();
  assert(p->sigacts != NULL, "userinit: sigacts_init failed");

  // printf("\nuser pagetable after uvmfirst:\n");
  // dump_pagetable(p->vm.pagetable, 2, 0, 0, 0, false);
  // printf("\n");

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = UVMBOTTOM;      // user program counter
  p->trapframe->sp = USTACKTOP;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  PROC_SET_USER_SPACE(p);
  __proc_set_pstate(p, PSTATE_UNINTERRUPTIBLE);

  sched_lock();
  scheduler_wakeup(p);
  sched_unlock();
  proc_unlock(p);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  struct proc *p = myproc();

  return vm_growheap(p->vm, n);
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  if (!PROC_USER_SPACE(p)) {
    return -1;
  }

  // Allocate process.
  if((np = allocproc(forkret, 0, 0, p->kstack_order)) == 0){
    return -1;
  }

  proc_lock(p);
  proc_lock(np);

  // Copy user memory from parent to child.
  if((np->vm = vm_dup(p->vm, (uint64)np->trapframe)) == NULL){
    // @TODO: need to make sure no one would access np before freeing it.
    proc_unlock(np);
    proc_unlock(p);
    freeproc(np);
    return -1;
  }

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // copy the process's signal actions.
  if (p->sigacts) {
    np->sigacts = sigacts_dup(p->sigacts);
    if (np->sigacts == NULL) {
      proc_unlock(np);
      proc_unlock(p);
      freeproc(np);
      return -1;
    }
  }

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  attach_child(p, np);
  PROC_SET_USER_SPACE(np);
  __proc_set_pstate(np, PSTATE_UNINTERRUPTIBLE);
  proc_unlock(p);
  
  sched_lock();
  scheduler_wakeup(np);
  sched_unlock();
  proc_unlock(np);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must not hold p->lock.
void
reparent(struct proc *p)
{
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
// The caller need to hold p->lock, and not to hold the scheduler lock.
// This is to ensure that its parent can be scheduled after it becomes zombie
// and not to wake up before it becomes zombie.
static void
__exit_yield(int status)
{
  struct proc *p = myproc();
  proc_lock(p);
  p->xstate = status;
  __proc_set_pstate(p, PSTATE_ZOMBIE);
  sched_lock();
  scheduler_yield(NULL);
  sched_unlock();
  proc_unlock(p);
  panic("exit: __exit_yield should not return");
}


// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();
  struct inode *cwd = NULL;

  proc_lock(p);
  assert(p != proc_table.initproc, "init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      p->ofile[fd] = 0;
      assert((uint64)f < PHYSTOP, "exit: file pointer out of bounds, pid: %d, fd: %d(*%p), f: %p", p->pid, fd, f, &p->ofile[fd]);
      // release p->lock before fileclose because fileclose may sleep
      proc_unlock(p);
      fileclose(f);
      proc_lock(p); // reacquire p->lock after fileclose
    }
  }
  cwd = p->cwd;
  p->cwd = 0;
  proc_unlock(p);

  if (cwd != NULL) {
    begin_op();
    iput(cwd);
    end_op();
  }

  // Give any children to init.
  reparent(p);

  __exit_yield(status);

  // Jump into the scheduler, never to return.
  yield();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  int pid;
  struct proc *p = myproc();
  struct proc *child, *tmp;

  proc_lock(p);
  for(;;){
    // Scan through table looking for exited children.
    list_foreach_node_safe(&p->children, child, tmp, siblings) {
      // make sure the child isn't still in exit() or swtch().
      proc_lock(child);
      if(PROC_ZOMBIE(child)) {
        // Found one.
        pid = child->pid;
        if(addr != 0 && vm_copyout(p->vm, addr, (char *)&child->xstate,
                                    sizeof(child->xstate)) < 0) {
          proc_unlock(child);
          pid = -1;
          goto ret;
        }
        pid = child->pid;
        detach_child(p, child);
        proc_unlock(child);
        freeproc(child);
        goto ret;
      }
      proc_unlock(child);
    }

    // No point waiting if we don't have any children.
    if(p->children_count == 0 || signal_terminated(p)){
      pid = -1;
      goto ret;
    }
    
    // Wait for a child to exit.
    __proc_set_pstate(myproc(), PSTATE_INTERRUPTIBLE);
    scheduler_sleep(NULL);  //DOC: wait-sleep
  }

ret:
  proc_unlock(p);
  return pid;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  proc_lock(myproc());
  sched_lock();
  scheduler_yield(NULL);
  sched_unlock();
  proc_unlock(myproc());
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  STATIC int first = 1;

  assert(PROC_USER_SPACE(myproc()), "kernel process %d tries to return to user space", myproc()->pid);
  // The scheduler will disable interrupts to assure the atomicity of
  // the scheduler operations. For processes that gave up CPU by calling yield(),
  //   yield() would restore the previous interruption state when switched back. 
  // But at here, we need to enable interrupts for the first time.
  
  // Still holding p->lock from scheduler.
  sched_unlock(); // Release the scheduler lock.
  proc_unlock(myproc());
  intr_on();

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.

#ifdef RWAD_WRITE_TEST
    // forward decl for rwlock tests
    void rwlock_launch_tests(void);
    // launch rwlock tests
    rwlock_launch_tests();
#endif
  }
  
  // printf("forkret: process %d is running\n", myproc()->pid);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  usertrapret();
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid, int signum)
{
  ksiginfo_t info = { 0 };
  info.signo = signum;
  info.sender = myproc();
  info.info.si_pid = myproc()->pid;

  return signal_send(pid, &info);
}

int
killed(struct proc *p)
{
  int k;
  
  proc_lock(p);
  k = signal_terminated(p);
  proc_unlock(p);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return vm_copyout(p->vm, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return vm_copyin(p->vm, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  struct proc *p;
  const char *state;
  int _panic_state = panic_state();
  int idx;
  hlist_bucket_t *bucket;
  hlist_entry_t *pos_entry, *tmp;

  printf("Process List:\n");
  if (!_panic_state)
  {
    __proctab_lock();
  }

  // list_foreach_node_safe(&proc_table.procs_list, p, tmp, dmp_list_entry) {
  hlist_foreach_entry(&proc_table.procs, idx, bucket, pos_entry, tmp) {
    p = __proctab_hash_get_node(pos_entry);
    proc_lock(p);
    enum procstate pstate = __proc_get_pstate(p);
    int pid = p->pid;
    char name[sizeof(p->name)];
    safestrcpy(name, p->name, sizeof(name));
    proc_unlock(p);

    if (__proc_get_pstate(p) == PSTATE_UNUSED)
      continue;

    state = procstate_to_str(pstate);
    printf("%d %s%s [%s] %s", pid, state, 
            PROC_STOPPED(p) ? " (stopped)" : "", 
            PROC_USER_SPACE(p) ? "U":"K", name);
    printf("\n");
  }

  if (!_panic_state)
  {
    __proctab_unlock();
  }
}

uint64 sys_dumpproc(void)
{
  // This function is called from the dumpproc user program.
  // It dumps the process table to the console.
  procdump();
  return 0;
}
