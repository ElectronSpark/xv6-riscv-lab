#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "semaphore.h"
#include "proc_queue.h"
#include "sched.h"
#include "mutex_types.h"
#include "errno.h"

/*
 * Semaphore runtime test suite.
 * 1. Waiters block until tokens are posted.
 * 2. Try-wait succeeds while tokens remain and fails with EAGAIN when empty.
 * 3. Overflow protection rejects posts beyond SEM_VALUE_MAX.
 * 4. Producer/consumer stress validates ordering and wakeups under contention.
 */

static volatile int sem_error_flag = 0;

#define SEM_TEST_WAITERS 4
static sem_t sem_t1;
static volatile int sem_t1_wait_requests = 0;
static volatile int sem_t1_acquired = 0;

#define SEM_T4_BUFFER_CAP 16
#define SEM_T4_TOTAL_ITEMS 512
#define SEM_T4_PRODUCERS 3
#define SEM_T4_CONSUMERS 4
static sem_t sem_t4_empty;
static sem_t sem_t4_full;
static mutex_t sem_t4_lock; // protects ring buffer indices
static int sem_t4_buffer[SEM_T4_BUFFER_CAP];
static volatile int sem_t4_head = 0;
static volatile int sem_t4_tail = 0;
static volatile int sem_t4_produce_cursor = 0;
static volatile int sem_t4_items_consumed = 0;
static volatile int sem_t4_producers_done = 0;
static volatile int sem_t4_consumers_done = 0;
static int sem_t4_seen[SEM_T4_TOTAL_ITEMS];
static volatile int sem_t4_log_budget = 0;

static int sem_wait_for(volatile int *ptr, int expected, int spin_loops) {
  while(spin_loops-- > 0) {
    if(__atomic_load_n(ptr, __ATOMIC_SEQ_CST) == expected)
      return 0;
    yield();
  }
  if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8) {
    printf("[sem][diag] wait_for timed out target=%p value=%d expected=%d\n",
           ptr, __atomic_load_n(ptr, __ATOMIC_SEQ_CST), expected);
  }
  return -1;
}

static void sem_test1_waiter(uint64 a1, uint64 a2) {
  (void)a1; (void)a2;
  __sync_add_and_fetch(&sem_t1_wait_requests, 1);
  int ret = sem_wait(&sem_t1);
  if(ret != 0) {
    sem_error_flag = 1;
    return;
  }
  __sync_add_and_fetch(&sem_t1_acquired, 1);
}

static void sem_run_test1(void) {
  printf("[sem][T1] waiters block until posted tokens... ");
  sem_error_flag = 0;
  sem_t1_wait_requests = 0;
  sem_t1_acquired = 0;

  if(sem_init(&sem_t1, "sem-test1", 0) != 0)
    sem_error_flag = 1;

  struct proc *np = NULL;
  for(int i = 0; i < SEM_TEST_WAITERS; i++) {
    if(kernel_proc_create("sem_t1", &np, sem_test1_waiter, 0, 0, KERNEL_STACK_ORDER) < 0) {
      sem_error_flag = 1;
    } else {
      wakeup_proc(np);
    }
  }

  if(sem_wait_for(&sem_t1_wait_requests, SEM_TEST_WAITERS, 50000) != 0)
    sem_error_flag = 1;

  int value = 0;
  if(sem_getvalue(&sem_t1, &value) != 0)
    sem_error_flag = 1;
  else if(value != -SEM_TEST_WAITERS)
    sem_error_flag = 1;

  for(int i = 0; i < SEM_TEST_WAITERS; i++) {
    if(sem_post(&sem_t1) != 0)
      sem_error_flag = 1;
  }

  if(sem_wait_for(&sem_t1_acquired, SEM_TEST_WAITERS, 50000) != 0)
    sem_error_flag = 1;

  if(sem_getvalue(&sem_t1, &value) != 0)
    sem_error_flag = 1;
  else if(value != 0)
    sem_error_flag = 1;

  printf(sem_error_flag ? "FAIL\n" : "OK\n");
}

static void sem_run_test2(void) {
  printf("[sem][T2] trywait semantics... ");
  sem_error_flag = 0;
  sem_t sem_local;
  if(sem_init(&sem_local, "sem-test2", 2) != 0) {
    sem_error_flag = 1;
  } else {
    if(sem_trywait(&sem_local) != 0)
      sem_error_flag = 1;
    if(sem_trywait(&sem_local) != 0)
      sem_error_flag = 1;
    int ret = sem_trywait(&sem_local);
    if(ret != -EAGAIN)
      sem_error_flag = 1;
    int value = -1;
    if(sem_getvalue(&sem_local, &value) != 0)
      sem_error_flag = 1;
    else if(value != 0)
      sem_error_flag = 1;
  }
  printf(sem_error_flag ? "FAIL\n" : "OK\n");
}

static void sem_run_test3(void) {
  printf("[sem][T3] overflow guard... ");
  sem_error_flag = 0;
  sem_t sem_local;
  if(sem_init(&sem_local, "sem-test3", SEM_VALUE_MAX) != 0) {
    sem_error_flag = 1;
  } else {
    if(sem_post(&sem_local) != -EOVERFLOW)
      sem_error_flag = 1;
    int value = 0;
    if(sem_getvalue(&sem_local, &value) != 0)
      sem_error_flag = 1;
    else if(value != SEM_VALUE_MAX)
      sem_error_flag = 1;
  }
  printf(sem_error_flag ? "FAIL\n" : "OK\n");
}

static void sem_t4_producer(uint64 a1, uint64 a2) {
  (void)a1; (void)a2;
  for(;;) {
    int ticket = __sync_fetch_and_add(&sem_t4_produce_cursor, 1);
    if(ticket >= SEM_T4_TOTAL_ITEMS)
      break;
    if(sem_wait(&sem_t4_empty) != 0) {
      if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8)
        printf("[sem][T4][prod] sem_wait(empty) failed\n");
      sem_error_flag = 1;
      return;
    }
    if(mutex_lock(&sem_t4_lock) != 0) {
      if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8)
        printf("[sem][T4][prod] mutex_lock failed\n");
      sem_error_flag = 1;
      return;
    }
    int head = sem_t4_head;
    sem_t4_buffer[head] = ticket;
    sem_t4_head = (head + 1) % SEM_T4_BUFFER_CAP;
    mutex_unlock(&sem_t4_lock);
    if(sem_post(&sem_t4_full) != 0) {
      if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8)
        printf("[sem][T4][prod] sem_post(full) failed\n");
      sem_error_flag = 1;
      return;
    }
    yield();
  }
  __sync_add_and_fetch(&sem_t4_producers_done, 1);
}

static void sem_t4_consumer(uint64 a1, uint64 a2) {
  (void)a1; (void)a2;
  for(;;) {
    if(__atomic_load_n(&sem_t4_items_consumed, __ATOMIC_SEQ_CST) >= SEM_T4_TOTAL_ITEMS &&
       __atomic_load_n(&sem_t4_producers_done, __ATOMIC_SEQ_CST) >= SEM_T4_PRODUCERS)
      break;

    if(sem_wait(&sem_t4_full) != 0) {
      if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8)
        printf("[sem][T4][cons] sem_wait(full) failed\n");
      sem_error_flag = 1;
      return;
    }

    if(mutex_lock(&sem_t4_lock) != 0) {
      if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8)
        printf("[sem][T4][cons] mutex_lock failed\n");
      sem_error_flag = 1;
      return;
    }

    if(sem_t4_items_consumed >= SEM_T4_TOTAL_ITEMS) {
      mutex_unlock(&sem_t4_lock);
      if(sem_post(&sem_t4_full) != 0)
        sem_error_flag = 1;
      break;
    }

    int tail = sem_t4_tail;
    int value = sem_t4_buffer[tail];
    sem_t4_tail = (tail + 1) % SEM_T4_BUFFER_CAP;
    mutex_unlock(&sem_t4_lock);

    if(value < 0 || value >= SEM_T4_TOTAL_ITEMS)
      sem_error_flag = 1;
    else if(__sync_lock_test_and_set(&sem_t4_seen[value], 1) != 0)
      sem_error_flag = 1;

    if(sem_error_flag && __sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8) {
      if(value < 0 || value >= SEM_T4_TOTAL_ITEMS) {
        printf("[sem][T4][cons] out-of-range value=%d tail=%d head=%d\n",
               value, tail, __atomic_load_n(&sem_t4_head, __ATOMIC_SEQ_CST));
      } else {
        printf("[sem][T4][cons] duplicate value=%d tail=%d\n", value, tail);
      }
    }

    int consumed = __sync_add_and_fetch(&sem_t4_items_consumed, 1);

    if(sem_post(&sem_t4_empty) != 0) {
      if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8)
        printf("[sem][T4][cons] sem_post(empty) failed\n");
      sem_error_flag = 1;
      return;
    }

    if(consumed >= SEM_T4_TOTAL_ITEMS &&
       __atomic_load_n(&sem_t4_producers_done, __ATOMIC_SEQ_CST) >= SEM_T4_PRODUCERS)
      break;

    yield();
  }
  __sync_add_and_fetch(&sem_t4_consumers_done, 1);
}

static void sem_run_test4(void) {
  printf("[sem][T4] producer/consumer stress... ");
  sem_error_flag = 0;

  if(sem_init(&sem_t4_empty, "sem-empty", SEM_T4_BUFFER_CAP) != 0)
    sem_error_flag = 1;
  if(sem_init(&sem_t4_full, "sem-full", 0) != 0)
    sem_error_flag = 1;
  mutex_init(&sem_t4_lock, "sem-buffer");

  sem_t4_head = 0;
  sem_t4_tail = 0;
  sem_t4_produce_cursor = 0;
  sem_t4_items_consumed = 0;
  sem_t4_producers_done = 0;
  sem_t4_consumers_done = 0;
  sem_t4_log_budget = 0;
  for(int i = 0; i < SEM_T4_TOTAL_ITEMS; i++)
    sem_t4_seen[i] = 0;

  struct proc *np = NULL;
  for(int i = 0; i < SEM_T4_PRODUCERS; i++) {
    if(kernel_proc_create("sem_prod", &np, sem_t4_producer, 0, 0, KERNEL_STACK_ORDER) < 0)
      sem_error_flag = 1;
    else
      wakeup_proc(np);
  }
  for(int i = 0; i < SEM_T4_CONSUMERS; i++) {
    if(kernel_proc_create("sem_cons", &np, sem_t4_consumer, 0, 0, KERNEL_STACK_ORDER) < 0)
      sem_error_flag = 1;
    else
      wakeup_proc(np);
  }

  if(sem_wait_for(&sem_t4_producers_done, SEM_T4_PRODUCERS, 400000) != 0)
    sem_error_flag = 1;
  else {
    for(int i = 0; i < SEM_T4_CONSUMERS; i++) {
      if(sem_post(&sem_t4_full) != 0 && __sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8) {
        printf("[sem][T4] failed to post wake sentinel for consumers\n");
      }
    }
  }
  if(sem_wait_for(&sem_t4_consumers_done, SEM_T4_CONSUMERS, 400000) != 0)
    sem_error_flag = 1;

  if(sem_t4_items_consumed != SEM_T4_TOTAL_ITEMS) {
    if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8) {
      printf("[sem][T4] consumed=%d expected=%d\n", sem_t4_items_consumed, SEM_T4_TOTAL_ITEMS);
    }
    sem_error_flag = 1;
  }

  for(int i = 0; i < SEM_T4_TOTAL_ITEMS; i++) {
    if(sem_t4_seen[i] == 0) {
      if(__sync_add_and_fetch(&sem_t4_log_budget, 1) <= 8) {
        printf("[sem][T4] missing item %d\n", i);
      }
      sem_error_flag = 1;
    }
  }

  printf(sem_error_flag ? "FAIL\n" : "OK\n");
}

static void semaphore_test_master(uint64 a1, uint64 a2) {
  (void)a1; (void)a2;
  for(int i = 0; i < 10000; i++)
    yield();

  printf("[sem] starting semaphore tests\n");
  sem_run_test1();
  sem_run_test2();
  sem_run_test3();
  sem_run_test4();
  printf("[sem] tests finished\n");
}

void semaphore_launch_tests(void) {
  struct proc *np = NULL;
  if(kernel_proc_create("semaphore_test_master", &np, semaphore_test_master, 0, 0, KERNEL_STACK_ORDER) < 0) {
    printf("[sem] cannot create test master thread\n");
  } else {
    wakeup_proc(np);
  }
}
