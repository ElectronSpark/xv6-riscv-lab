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

void
initsleeplock(struct sleeplock *lk, char *name)
{
  spin_init(&lk->lk, "sleep lock");
  proc_queue_init(&lk->wait_queue, "sleep lock wait queue", &lk->lk);
  lk->name = name;
  lk->locked = 0;
  lk->holder = NULL;
}

int
acquiresleep(struct sleeplock *lk)
{
  proc_node_t waiter = { 0 };
  int ret = -1;
  proc_node_init(&waiter);
  proc_lock(myproc());
  spin_acquire(&lk->lk);

  // If the lock is not held, acquire it and return success.
  if (lk->locked == 0) {
    lk->locked = 1;
    lk->holder = myproc();
    ret = 0;
    goto done; // Success: lock acquired
  }

  // Slow path
  // @TODO: handle signal
  if (proc_queue_push(&lk->wait_queue, &waiter) != 0) {
    ret = -1;
    goto done; // Error: failed to push to wait queue
  }
  for (;;) {
    __proc_set_pstate(myproc(), PSTATE_UNINTERRUPTIBLE);
    scheduler_sleep(&lk->lk);
    if (lk->holder == myproc()) {
      assert(proc_queue_remove(&lk->wait_queue, &waiter) == 0,
             "acquiresleep: failed to remove from wait queue");
      ret = 0;
      goto done; // Success: lock acquired
    }
  }
  
done:
  spin_release(&lk->lk);
  proc_unlock(myproc());
  return ret;
}

void
releasesleep(struct sleeplock *lk)
{
  // First put all process from the wait queue to a temporary queue,
  // so that we can detach them from the wait queue, and then wake them up.
  // This is to avoid deadlocks, as we cannot hold the lock while waking up processes
  // from the wait queue.
  spin_acquire(&lk->lk);
  if (LIST_IS_EMPTY(&lk->wait_queue.head)) {
    lk->locked = 0; // Lock is released
    lk->holder = NULL; // No process holds the lock
    assert(proc_queue_size(&lk->wait_queue) == 0,
           "releasesleep: wait queue is not empty");
    spin_release(&lk->lk);
    return; // Nothing to release
  }
  proc_node_t *first_waiter = NULL;
  assert(proc_queue_first(&lk->wait_queue, &first_waiter) == 0,
         "releasesleep: failed to get first process from wait queue");
  if (first_waiter == NULL) {
    lk->holder = NULL;
    lk->locked = 0; // Lock is released
    assert(proc_queue_size(&lk->wait_queue) == 0,
           "releasesleep: wait queue is not empty");
    spin_release(&lk->lk);
  } else {
    struct proc *next = proc_node_get_proc(first_waiter);
    assert(next != NULL, "releasesleep: first waiter is NULL");
    lk->holder = next;
    spin_release(&lk->lk); 
    proc_lock(next); // Lock the process that will hold the lock
    sched_lock();
    scheduler_wakeup(next); // Wake up the first process in the wait queue
    sched_unlock();
    proc_unlock(next); // Unlock the process that will hold the lock
  }
}

int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  spin_acquire(&lk->lk);
  r = lk->locked && (lk->holder == myproc());
  spin_release(&lk->lk);
  return r;
}



