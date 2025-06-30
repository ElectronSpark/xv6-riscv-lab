// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void
initsleeplock(struct sleeplock *lk, char *name)
{
  spin_init(&lk->lk, "sleep lock");
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}

void
acquiresleep(struct sleeplock *lk)
{
  spin_acquire(&lk->lk);
  while (lk->locked) {
    sleep(lk, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  spin_release(&lk->lk);
}

void
releasesleep(struct sleeplock *lk)
{
  spin_acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  spin_release(&lk->lk);
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



