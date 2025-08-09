// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "proc_queue.h"
#include "sched.h"

#define __sleep_lock_set_holder(lk, p) \
    __atomic_store_n(&lk->holder, p, __ATOMIC_SEQ_CST)
#define __sleep_lock_holder(lk) \
    __atomic_load_n(&lk->holder, __ATOMIC_SEQ_CST)
#define __sleep_lock_try_set_holder(lk, p) ({           \
  struct proc *old = NULL;                              \
  __atomic_compare_exchange_n(&lk->holder, &old, p, 1,  \
                              __ATOMIC_SEQ_CST,         \
                              __ATOMIC_SEQ_CST);        \
})

void
initsleeplock(struct sleeplock *lk, char *name)
{
  spin_init(&lk->lk, "sleep lock");
  proc_queue_init(&lk->wait_queue, "sleep lock wait queue", &lk->lk);
  lk->name = name;
  __sleep_lock_set_holder(lk, NULL);
}

int
acquiresleep(struct sleeplock *lk)
{
  proc_node_t waiter = { 0 };
  int ret = -1;

  // If the lock is not held, acquire it and return success.
  if (__sleep_lock_try_set_holder(lk, myproc())) {
    return 0;
  }

  // Slow path
  spin_acquire(&lk->lk);
  assert(__sleep_lock_holder(lk) != myproc(), 
         "acquiresleep: deadlock detected, process already holds the lock");
  proc_node_init(&waiter);
  proc_lock(myproc());
  // @TODO: handle signals
  if (proc_queue_push(&lk->wait_queue, &waiter) != 0) {
    ret = -1;
    proc_unlock(myproc());
    goto done; // Error: failed to push to wait queue
  }
  for (;;) {
    __proc_set_pstate(myproc(), PSTATE_UNINTERRUPTIBLE);
    scheduler_sleep(&lk->lk);
    if (__sleep_lock_holder(lk) == myproc()) {
      assert(proc_queue_remove(&lk->wait_queue, &waiter) == 0,
             "acquiresleep: failed to remove from wait queue");
      ret = 0;
      break; // Success: lock acquired
    }
  }
  proc_unlock(myproc());
  
done:
  spin_release(&lk->lk);
  return ret;
}

// @TODO: signal handling
void
releasesleep(struct sleeplock *lk)
{
  // First put all process from the wait queue to a temporary queue,
  // so that we can detach them from the wait queue, and then wake them up.
  // This is to avoid deadlocks, as we cannot hold the lock while waking up processes
  // from the wait queue.
  spin_acquire(&lk->lk);
  if (LIST_IS_EMPTY(&lk->wait_queue.head)) {
    __sleep_lock_set_holder(lk, NULL); // No process holds the lock
    assert(proc_queue_size(&lk->wait_queue) == 0,
           "releasesleep: wait queue is not empty");
    spin_release(&lk->lk);
    return; // Nothing to release
  }
  proc_node_t *first_waiter = NULL;
  assert(proc_queue_first(&lk->wait_queue, &first_waiter) == 0,
         "releasesleep: failed to get first process from wait queue");
  if (first_waiter == NULL) {
    __sleep_lock_set_holder(lk, NULL); // No process holds the lock
    assert(proc_queue_size(&lk->wait_queue) == 0,
           "releasesleep: wait queue is not empty");
  } else {
    struct proc *next = proc_node_get_proc(first_waiter);
    assert(next != NULL, "releasesleep: first waiter is NULL");
    __sleep_lock_set_holder(lk, next);
    proc_lock(next); // Lock the process that will hold the lock
    sched_lock();
    scheduler_wakeup(next); // Wake up the first process in the wait queue
    sched_unlock();
    proc_unlock(next); // Unlock the process that will hold the lock
  }
  spin_release(&lk->lk);
}

int
holdingsleep(struct sleeplock *lk)
{
  // int r;
  
  // spin_acquire(&lk->lk);
  // r = lk->locked && (lk->holder == myproc());
  // spin_release(&lk->lk);
  // return r;
  return __atomic_load_n(&lk->holder, __ATOMIC_SEQ_CST) == myproc();
}



