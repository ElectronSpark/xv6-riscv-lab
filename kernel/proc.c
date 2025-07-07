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

#define NPROC_HASH_BUCKETS 31

struct cpu cpus[NCPU];

// Lock order for proc:
// 1. proc table lock
// 2. parent pcb lock
// 3. target pcb lock
// 4. children pcb lock

STATIC int __killed_locked(struct proc *p);

static slab_cache_t proc_cache;

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
  return 0;
}

extern void forkret(void);
STATIC void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

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
  p->state = UNUSED;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->pid = 0;
  p->parent = 0;
  p->sz = 0;
  p->pagetable = 0;
  p->trapframe = 0;
  list_entry_init(&p->dmp_list_entry);
  p->children_count = 0;
  list_entry_init(&p->siblings);
  list_entry_init(&p->children);
  hlist_entry_init(&p->proctab_entry);
  spin_init(&p->lock, "proc");
  proc_queue_entry_init(&p->queue_entry);
  memset(p->name, 0, sizeof(p->name));
}

// initialize the proc table.
void
procinit(void)
{
  // struct proc *p;
  
  slab_cache_init(&proc_cache, "PCB Pool", sizeof(struct proc), SLAB_FLAG_STATIC);
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

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return without p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
STATIC struct proc*
allocproc(void)
{
  struct proc *p = NULL;
  void *kstack = NULL;

  p = slab_alloc(&proc_cache);
  if (p == NULL) {
    return NULL; // No free proc available in the slab cache
  }

  __pcb_init(p);
  p->state = USED;

  // Allocate a trapframe page.
  struct trapframe *trapframe = 
    (struct trapframe *)page_alloc(TRAPFRAME_ORDER, PAGE_FLAG_ANON);
  if(trapframe == 0){
    freeproc(p);
    return NULL;
  }
  memset(trapframe, 0, TRAPFRAME_SIZE);
  p->trapframe = trapframe;

  // Allocate a kernel stack page.
  kstack = page_alloc(KERNEL_STACK_ORDER, PAGE_FLAG_ANON);
  if(kstack == NULL){
    freeproc(p);
    return NULL;
  }
  memset(kstack, 0, KERNEL_STACK_SIZE);
  p->kstack = (uint64)kstack;

  // Allocate pagetable for the process.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    return NULL;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + KERNEL_STACK_SIZE;
  p->context.sp -= sizeof(struct context) + 8;
  p->context.sp &= ~0x7UL; // align to 8 bytes

  __proctab_lock();
  p->pid = __alloc_pid();
  if (__proctab_add(p) != 0) {
    freeproc(p);
    __proctab_unlock();
    return NULL;
  }

  __proctab_unlock();
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
STATIC void
freeproc(struct proc *p)
{
  assert(p != NULL, "freeproc called with NULL proc");
  assert(spin_holding(&p->lock), "freeproc called without p->lock held");
  assert(p->state != RUNNING, "freeproc called with a running proc");
  assert(p->state != RUNNABLE, "freeproc called with a runnable proc");
  assert(p->state != SLEEPING, "freeproc called with a sleeping proc");

  __proctab_lock();
  struct proc *existing = hlist_pop(&proc_table.procs, p);
  // Remove from the global list of processes for dumping.
  list_entry_detach(&p->dmp_list_entry);
  __proctab_unlock();

  assert(existing == NULL || existing == p, "freeproc called with a different proc");

  if(p->trapframe)
    page_free((void*)p->trapframe, TRAPFRAME_ORDER);
  if(p->kstack)
    page_free((void*)p->kstack, KERNEL_STACK_ORDER);
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  
  slab_free(p);
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W | PTE_RSW_w) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
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
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  assert(p != NULL, "userinit: allocproc failed");

  // // printf user pagetable
  // printf("\nuser pagetable after allocproc:\n");
  // dump_pagetable(p->pagetable, 2, 0, 0, 0, false);
  // printf("\n");

  __proctab_lock();
  __proctab_set_initproc(p);
  __proctab_unlock();
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  spin_acquire(&p->lock);
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // printf("\nuser pagetable after uvmfirst:\n");
  // dump_pagetable(p->pagetable, 2, 0, 0, 0, false);
  // printf("\n");

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = PROC_INITIALIZED;

  spin_release(&p->lock);

  scheduler_wakeup(p);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W | PTE_RSW_w)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
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
  assert(spin_holding(&parent->lock), "attach_child: parent lock not held");
  assert(spin_holding(&child->lock), "attach_child: child lock not held");
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
  assert(spin_holding(&parent->lock), "detach_child: parent lock not held");
  assert(spin_holding(&child->lock), "detach_child: child lock not held");
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

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  spin_acquire(&np->lock);

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    // @TODO: need to make sure no one would access np before freeing it.
    freeproc(np);
    spin_release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  spin_release(&np->lock);

  spin_acquire(&p->lock);
  spin_acquire(&np->lock);
  attach_child(p, np);
  np->state = PROC_INITIALIZED;
  spin_release(&np->lock);
  spin_release(&p->lock);

  scheduler_wakeup(np);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must not hold p->lock.
void
reparent(struct proc *p)
{
  struct proc *pp = NULL;
  struct proc *child, *tmp;
  bool found = false;

  spin_acquire(&p->lock);
  do {
    pp = p->parent;
    spin_release(&p->lock);
    if (pp == NULL) {
      // No parent, nothing to do.
      assert(p == __proctab_get_initproc(), "reparent: no parent and not init");
      return;
    }
    // parent must be held before acquiring p->lock.
    spin_acquire(&pp->lock);
    spin_acquire(&p->lock);
  } while (p->parent != pp);  // in case p->parent changed while we were acquiring locks
  
  list_foreach_node_safe(&p->children, child, tmp, siblings) {
    // make sure the child isn't still in exit() or swtch().
    spin_acquire(&child->lock);
    detach_child(p, child);
    attach_child(pp, child);
    spin_release(&child->lock);
  }

  spin_release(&p->lock);
  spin_release(&pp->lock);

  assert(!spin_holding(&p->lock), "reparent: p->lock is still held");
  assert(!spin_holding(&pp->lock), "reparent: pp->lock is still held");
  
  if (found)
    wakeup(pp);
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();
  struct inode *cwd = NULL;

  spin_acquire(&p->lock);
  assert(p != proc_table.initproc, "init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }
  cwd = p->cwd;
  p->cwd = 0;
  spin_release(&p->lock);

  begin_op();
  iput(cwd);
  end_op();

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  spin_acquire(&p->lock);
  p->xstate = status;
  p->state = ZOMBIE;
  spin_release(&p->lock);

  assert(!spin_holding(&p->lock), "exit: p->lock is still held");

  // Jump into the scheduler, never to return.
  scheduler_yield(0);
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

  spin_acquire(&p->lock);
  for(;;){
    // Scan through table looking for exited children.
    list_foreach_node_safe(&p->children, child, tmp, siblings) {
      // make sure the child isn't still in exit() or swtch().
      spin_acquire(&child->lock);
      if(child->state == ZOMBIE){
        // Found one.
        pid = child->pid;
        if(addr != 0 && copyout(p->pagetable, addr, (char *)&child->xstate,
                                    sizeof(child->xstate)) < 0) {
          spin_release(&child->lock);
          pid = -1;
          goto ret;
        }
        detach_child(p, child);
        freeproc(child);
        // spin_release(&child->lock); // pcb has been freed
        pid = child->pid;
        goto ret;
      }
      spin_release(&child->lock);
    }

    // No point waiting if we don't have any children.
    if(p->children_count == 0 || __killed_locked(p)){
      pid = -1;
      goto ret;
    }
    
    // Wait for a child to exit.
    spin_release(&p->lock);
    sleep(p, NULL);  //DOC: wait-sleep
    spin_acquire(&p->lock);
  }

ret:
  spin_release(&p->lock);
  return pid;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  scheduler_yield(NULL);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  STATIC int first = 1;

  // The scheduler will disable interrupts to assure the atomicity of
  // the scheduler operations. For processes that gave up CPU by calling yield(),
  //   yield() would restore the previous interruption state when switched back. 
  // But at here, we need to enable interrupts for the first time.
  intr_on();

  // Still holding p->lock from scheduler.
  spin_release(&myproc()->lock);
  sched_unlock(); // Release the scheduler lock.

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically spin_release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  scheduler_sleep_on_chan(chan, lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  scheduler_wakeup_on_chan(chan);
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;
  int ret_val = 0;

  __proctab_assert_unlocked();
  __proctab_lock();
  p = __proctab_get_pid_proc(pid);
  if (p == NULL) {
    ret_val = -1;
    goto ret;
  }

  spin_acquire(&p->lock);
  assert(p->pid == pid, "kill: pid mismatch");
  p->killed = 1;
  spin_release(&p->lock);

  // @TODO: if the process is sleeping on a queue
  scheduler_wakeup_on_chan(p->chan);

ret:
  __proctab_unlock();
  return ret_val;
}

void
setkilled(struct proc *p)
{
  spin_acquire(&p->lock);
  p->killed = 1;
  spin_release(&p->lock);
}

STATIC int
__killed_locked(struct proc *p)
{
  // This function is used when we don't hold p->lock.
  // It is not safe to call this function if p->lock is held.
  assert(spin_holding(&p->lock), "killed_locked called without p->lock held");
  return p->killed;
}

int
killed(struct proc *p)
{
  int k;
  
  spin_acquire(&p->lock);
  k = __killed_locked(p);
  spin_release(&p->lock);
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
    return copyout(p->pagetable, dst, src, len);
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
    return copyin(p->pagetable, dst, src, len);
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
  STATIC char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p, *tmp;
  char *state;
  int _panic_state = panic_state();

  printf("\n");
  if (!_panic_state)
  {
    __proctab_lock();
  }

  list_foreach_node_safe(&proc_table.procs_list, p, tmp, dmp_list_entry) {
    spin_acquire(&p->lock);
    enum procstate pstate = p->state;
    int pid = p->pid;
    char name[sizeof(p->name)];
    safestrcpy(name, p->name, sizeof(name));
    spin_release(&p->lock);

    if (pstate == UNUSED)
      continue;
    if (pstate >= 0 && pstate < NELEM(states) && states[pstate])
      state = states[pstate];
    else
      state = "???";
    printf("%d %s %s", pid, state, name);
    printf("\n");
  }

  if (!_panic_state)
  {
    __proctab_unlock();
  }
}
