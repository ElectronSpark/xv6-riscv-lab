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
  proc_queue_init(&lk->wait_queue, "sleep lock wait queue");
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}

void
acquiresleep(struct sleeplock *lk)
{
  spin_acquire(&lk->lk);
  while (lk->locked) {
    scheduler_sleep(&lk->wait_queue, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  spin_release(&lk->lk);
}

void
releasesleep(struct sleeplock *lk)
{
  proc_queue_t tmp_queue = { 0 };
  proc_queue_init(&tmp_queue, "tmp_queue");
  spin_acquire(&lk->lk);
  proc_queue_bulk_move(&tmp_queue, &lk->wait_queue);
  lk->locked = 0;
  lk->pid = 0;
  spin_release(&lk->lk);
  struct proc *tmp = NULL;
  struct proc *pos = NULL;
  proc_queue_foreach_unlocked(&tmp_queue, pos, tmp) {
    proc_queue_remove(&tmp_queue, pos);
    assert(pos->state == SLEEPING, "Process must be SLEEPING to wake up");
    spin_acquire(&pos->lock);
    scheduler_wakeup(pos);
    spin_release(&pos->lock);
  }
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



