// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "proc_queue.h"
#include "sched.h"

#define __mutex_set_holder(lk, p) \
    __atomic_store_n(&lk->holder, p, __ATOMIC_SEQ_CST)
#define __mutex_holder(lk) \
    __atomic_load_n(&lk->holder, __ATOMIC_SEQ_CST)
#define __mutex_try_set_holder(lk, p) ({           \
  struct proc *old = NULL;                              \
  __atomic_compare_exchange_n(&lk->holder, &old, p, 1,  \
                              __ATOMIC_SEQ_CST,         \
                              __ATOMIC_SEQ_CST);        \
})

static int __do_wakeup(mutex_t *lk) {
  if (LIST_IS_EMPTY(&lk->wait_queue.head)) {
    __mutex_set_holder(lk, NULL); // No process holds the lock
    assert(proc_queue_size(&lk->wait_queue) == 0,
           "mutex_unlock: wait queue is not empty");
    return 0; // Nothing to release
  }
  return proc_queue_wakeup(&lk->wait_queue, 0, &lk->holder);
}

void
mutex_init(mutex_t *lk, char *name)
{
  spin_init(&lk->lk, "sleep lock");
  proc_queue_init(&lk->wait_queue, "sleep lock wait queue", &lk->lk);
  lk->name = name;
  __mutex_set_holder(lk, NULL);
}

int
mutex_lock(mutex_t *lk)
{
  // If the lock is not held, acquire it and return success.
  if (__mutex_try_set_holder(lk, myproc())) {
    return 0;
  }

  // Slow path
  spin_acquire(&lk->lk);
  assert(__mutex_holder(lk) != myproc(), 
         "mutex_lock: deadlock detected, process already holds the lock");
  
  while (__mutex_holder(lk) != myproc()) {
    int ret = proc_queue_wait(&lk->wait_queue, &lk->lk);
    if (ret != 0) {
      // If proc_queue_wait returns an error, we need to release the lock
      // and return the error code.
      if (proc_queue_size(&lk->wait_queue) > 0) {
        assert(__do_wakeup(lk) == 0, "mutex_unlock: failed to wake up processes");
      } else if (__mutex_holder(lk) == myproc()) {
        __mutex_set_holder(lk, NULL);
      }
      spin_release(&lk->lk);
      return ret;
    }
  }
  __mutex_set_holder(lk, myproc());
  spin_release(&lk->lk);
  
  return 0;
}

// @TODO: signal handling
void
mutex_unlock(mutex_t *lk)
{
  // First put all process from the wait queue to a temporary queue,
  // so that we can detach them from the wait queue, and then wake them up.
  // This is to avoid deadlocks, as we cannot hold the lock while waking up processes
  // from the wait queue.
  spin_acquire(&lk->lk);
  int ret = __do_wakeup(lk);
  assert(ret == 0, "mutex_unlock: failed to wake up processes");
  spin_release(&lk->lk);
}

int
holding_mutex(mutex_t *lk)
{
  return __atomic_load_n(&lk->holder, __ATOMIC_SEQ_CST) == myproc();
}



