// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void
spin_init(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
spin_acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.
  assert(lk && !spin_holding(lk), "spin_acquire");

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  int __debug_count = 0;
  while(__atomic_test_and_set(&lk->locked, __ATOMIC_ACQUIRE) != 0) {
      __debug_count += 1;
      if (__debug_count >= 10) {
        asm volatile ("nop");
      }
    ;
  }
  
  // Record info about lock acquisition for spin_holding() and debugging.
  __atomic_store_n(&lk->cpu, mycpu(), __ATOMIC_RELAXED);
  __atomic_signal_fence(__ATOMIC_ACQUIRE);
}

// Release the lock.
void
spin_release(struct spinlock *lk)
{
  assert(lk && spin_holding(lk), "spin_release");

  
  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __atomic_store_n(&lk->cpu, 0, __ATOMIC_RELEASE);

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __atomic_clear(&lk->locked, __ATOMIC_RELEASE);
  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
spin_holding(struct spinlock *lk)
{
  if (__atomic_load_n(&lk->locked, __ATOMIC_ACQUIRE) == 0) {
    return 0; // Lock is not held
  }
  if (__atomic_load_n(&lk->cpu, __ATOMIC_ACQUIRE) != mycpu()) {
    return 0;
  }
  return 1;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  int old = intr_get();

  intr_off();
  if(mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if(intr_get())
    panic("pop_off - interruptible");
  if(c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if(c->noff == 0 && c->intena)
    intr_on();
}
