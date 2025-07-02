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

#define NPROC_HASH_BUCKETS 31

struct cpu cpus[NCPU];

struct proc proc[NPROC];
struct {
  struct {
    hlist_t procs;
    hlist_bucket_t buckets[NPROC_HASH_BUCKETS];
  };
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

  struct proc *existing = hlist_put(&proc_table.procs, p);

  assert(existing != p, "Failed to add process with pid %d", p->pid);
  assert(existing == NULL, "Process with pid %d already exists", p->pid);
  return 0;
}

extern void forkret(void);
STATIC void freeproc_locked(struct proc *p);
STATIC void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    assert(pa != 0, "kalloc failed for proc stack");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W | PTE_RSW_w);
  }
}

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
  list_entry_init(&p->siblings);
  list_entry_init(&p->children);
  hlist_entry_init(&p->proctab_entry);
  proc_queue_entry_init(&p->queue_entry);
  memset(p->name, 0, sizeof(p->name));
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  __proctab_init();
  spin_init(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
    __pcb_init(p);
    spin_init(&p->lock, "proc");
    p->kstack = KSTACK((int) (p - proc));
  }
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
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
STATIC struct proc*
allocproc(void)
{
  struct proc *p;
  struct proc* ret_proc = NULL;

  __proctab_lock();
  for(p = proc; p < &proc[NPROC]; p++) {
    spin_acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    }
    spin_release(&p->lock);
  }

  goto ret;

found:
  p->pid = __alloc_pid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc_locked(p);
    spin_release(&p->lock);
    goto ret;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc_locked(p);
    spin_release(&p->lock);
    goto ret;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  if (__proctab_add(p) == 0) {
    ret_proc = p;
    goto ret;
  } else {
    ret_proc = NULL;
    freeproc_locked(p);
    spin_release(&p->lock);
    goto ret;
  }

ret:
  __proctab_unlock();
  return ret_proc;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
STATIC void
freeproc_locked(struct proc *p)
{
  assert(spin_holding(&p->lock), "freeproc called without p->lock held");
  struct proc *existing = hlist_pop(&proc_table.procs, p);
  assert(existing == NULL || existing == p, "freeproc called with a different proc");
  if(p->trapframe)
    kfree((void*)p->trapframe);
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  __pcb_init(p);
}

STATIC void
freeproc(struct proc *p)
{
  __proctab_assert_unlocked();
  assert(p != NULL, "freeproc called with NULL proc");
  __proctab_lock();
  freeproc_locked(p);
  __proctab_unlock();
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
  __proctab_lock();
  __proctab_set_initproc(p);
  __proctab_unlock();
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  spin_release(&p->lock);
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

  spin_acquire(&wait_lock);
  np->parent = p;
  spin_release(&wait_lock);

  spin_acquire(&np->lock);
  np->state = RUNNABLE;
  spin_release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
// Caller must not hold p->lock.
void
reparent(struct proc *p)
{
  struct proc *pp;
  bool found = false;

  __proctab_assert_unlocked();
  __proctab_lock();
  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p)
      found = true;
  }
  __proctab_unlock();

  if (found)
    wakeup(__proctab_get_initproc());
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  assert(p != proc_table.initproc, "init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  spin_acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  spin_acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  spin_release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  spin_acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        spin_acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            spin_release(&pp->lock);
            spin_release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          spin_release(&pp->lock);
          spin_release(&wait_lock);
          return pid;
        }
        spin_release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      spin_release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

static struct proc*
__switch_to(struct context *current, struct proc *p)
{
  if (current == NULL) {
    panic("__switch_to: current context is null");
  }

  if (p == NULL) {
    panic("__switch_to: target process is null");
  }

  if (p->state != RUNNING) {
    panic("__switch_to: target process not running");
  }

  // Save the old process's context.
  uint64 prev = __swtch_context(current, &p->context, 0);

  // Return the old process.
  return (struct proc *)prev;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      spin_acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        p = __switch_to(&c->context, p);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        if (p->state == RUNNING) {
          panic("scheduler: process still running");
        }
        c->proc = 0;
        found = 1;
      }
      spin_release(&p->lock);
    }
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      intr_on();
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  assert(spin_holding(&p->lock), "sched called without p->lock held");
  assert(mycpu()->noff == 1, "sched locks");
  assert(p->state != RUNNING, "sched running");
  assert(!intr_get(), "sched interruptible");

  intena = mycpu()->intena;
  __swtch_context(&p->context, &mycpu()->context, (uint64)p);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  spin_acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  spin_release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  STATIC int first = 1;

  // Still holding p->lock from scheduler.
  spin_release(&myproc()->lock);

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
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to spin_release lk.

  spin_acquire(&p->lock);  //DOC: sleeplock1
  spin_release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  spin_release(&p->lock);
  spin_acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p = NULL;
  hlist_entry_t *pos, *tmp;
  hlist_bucket_t *bucket;
  int idx;

  __proctab_assert_unlocked();
  __proctab_lock();
  hlist_foreach_bucket(&proc_table.procs, idx, bucket)                        \
    if (!LIST_IS_EMPTY(bucket))                                     \
    list_foreach_node_safe(bucket, pos, tmp, list_entry)
  {
    p = __proctab_hash_get_node(pos);
    if(p != myproc()){
      spin_acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      spin_release(&p->lock);
    }
  }
  __proctab_unlock();
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
  if(p->state == SLEEPING){
    // Wake process from sleep().
    p->state = RUNNABLE;
  }

  spin_release(&p->lock);


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

int
killed(struct proc *p)
{
  int k;
  
  spin_acquire(&p->lock);
  k = p->killed;
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
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
