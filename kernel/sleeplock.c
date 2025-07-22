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
  lk->pid = 0;
}

void
acquiresleep(struct sleeplock *lk)
{
  spin_acquire(&myproc()->lock);
  spin_acquire(&lk->lk);
  while (lk->locked) {
    scheduler_sleep(&lk->wait_queue, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  spin_release(&lk->lk);
  spin_release(&myproc()->lock);
}

void
releasesleep(struct sleeplock *lk)
{
  // First put all process from the wait queue to a temporary queue,
  // so that we can detach them from the wait queue, and then wake them up.
  // This is to avoid deadlocks, as we cannot hold the lock while waking up processes
  // from the wait queue.
  proc_queue_t tmp_queue = { 0 };
  proc_queue_init(&tmp_queue, "tmp_queue", NULL);
  spin_acquire(&lk->lk);
  proc_queue_bulk_move(&tmp_queue, &lk->wait_queue);
  lk->locked = 0;
  lk->pid = 0;
  spin_release(&lk->lk);

  // Now we can wake up all processes from the temporary queue.
  // We can do this without holding the lock related to the temporary queue, 
  // because the temporary queue is invisible to other processes.
  struct proc *tmp = NULL;
  struct proc *pos = NULL;
  proc_queue_foreach_unlocked(&tmp_queue, pos, tmp) {
    spin_acquire(&pos->lock);
    proc_queue_remove(&tmp_queue, pos);
    assert(pos->state == SLEEPING, "Process must be SLEEPING to wake up");
    
    sched_lock();
    scheduler_wakeup(pos);
    sched_unlock();
    spin_release(&pos->lock);
  }
}

void
releasesleep_one(struct sleeplock *lk)
{
  if (!lk->locked) {
    return; // Lock is not held, nothing to release
  }
  struct proc *poc = NULL;
  spin_acquire(&lk->lk);
  if (proc_queue_size(&lk->wait_queue) <= 0) {
    spin_release(&lk->lk);
    return;
  }
  assert(proc_queue_pop(&lk->wait_queue, &poc) == 0, "Failed to pop process from wait queue");
  spin_release(&lk->lk);
  assert(poc != NULL, "releasesleep_one: failed to pop from wait queue");
  spin_acquire(&poc->lock);
  sched_lock();
  scheduler_wakeup(poc);
  sched_unlock();
  spin_release(&poc->lock);
}

int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  spin_acquire(&lk->lk);
  r = lk->locked && (lk->pid == myproc()->pid);
  spin_release(&lk->lk);
  return r;
}



