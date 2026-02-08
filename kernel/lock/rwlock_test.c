#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "lock/rwlock.h"
#include "lock/mutex_types.h"
#include "proc/thread.h"
#include "proc/tq.h"
#include "proc/sched.h"

/*
 * RWLock test suite (no artificial busy delays).
 * 1. Multiple readers can hold the lock concurrently.
 * 2. Writer waits until all readers release.
 * 3. Writers are mutually exclusive.
 * 4. Data consistency under mixed reader/writer stress.
 */

static rwlock_t test_lock;

/* Shared instrumentation */
static volatile int active_readers = 0;
static volatile int max_active_readers = 0;
static volatile int active_writers = 0;
static volatile int error_flag = 0;

/* Test 1 variables */
static volatile int t1_target_readers = 0;
static volatile int t1_done_readers = 0;
static volatile int t1_started_readers = 0;   // number of readers that acquired the lock
static volatile int t1_release_readers = 0;    // barrier flag

/* Test 2 variables */
static volatile int t2_target_readers = 0;
static volatile int t2_done_readers = 0;
static volatile int t2_writer_acquired = 0;

/* Test 3 variables */
static volatile int t3_done_writers = 0;

/* Test 4 variables and dataset */
#define T4_DATA_LEN 32
#define T4_WRITER_ITERS 150
#define T4_WRITER_THREADS 2
#define T4_READER_THREADS 6
struct t4_dataset {
  int version;
  int len;
  int checksum;
  int data[T4_DATA_LEN];
};
static struct t4_dataset t4_ds;
static volatile int t4_writers_done = 0;
static volatile int t4_reader_done = 0;
static volatile int t4_start_barrier = 0;
static volatile int t4_error_logs = 0;
static mutex_t t4_start_lock;   // sleeplock barrier for Test4

static volatile int integrity_log_count = 0;

static void record_integrity_failure(const char *label, const char *reason, long v1, long v2) {
  if (__sync_add_and_fetch(&integrity_log_count, 1) <= 8) {
    printf("[rwlock][integrity][%s] %s (v1=%ld v2=%ld)\n", label, reason, v1, v2);
  }
  error_flag = 1;
}

static void check_rwlock_integrity(const char *label) {
  int read_waiters = tq_size(&test_lock.read_queue);
  int write_waiters = tq_size(&test_lock.write_queue);
  if (read_waiters < 0 || write_waiters < 0) {
    record_integrity_failure(label, "negative waiter count", read_waiters, write_waiters);
    return;
  }

  if (test_lock.readers < 0) {
    record_integrity_failure(label, "negative readers", test_lock.readers, 0);
  }

  if (test_lock.readers > 0 && test_lock.holder_pid != -1) {
    record_integrity_failure(label, "reader-writer overlap", test_lock.readers,
                             (long)test_lock.holder_pid);
  }

  if (test_lock.holder_pid == -1 && write_waiters < 0) {
    record_integrity_failure(label, "invalid write waiters", write_waiters, 0);
  }

  if (test_lock.holder_pid != -1 && test_lock.readers != 0) {
    record_integrity_failure(label, "writer with readers", test_lock.readers,
                             (long)test_lock.holder_pid);
  }

  if (test_lock.read_queue.lock != &test_lock.lock) {
    record_integrity_failure(label, "read queue lock mismatch",
                             (long)(uint64)test_lock.read_queue.lock,
                             (long)(uint64)&test_lock.lock);
  }

  if (test_lock.write_queue.lock != &test_lock.lock) {
    record_integrity_failure(label, "write queue lock mismatch",
                             (long)(uint64)test_lock.write_queue.lock,
                             (long)(uint64)&test_lock.lock);
  }
}

/* Reader for Test 1 */
static void t1_reader(uint64 a1, uint64 a2) {
  if(rwlock_acquire_read(&test_lock) != 0) { error_flag = 1; return; }
  check_rwlock_integrity("T1 reader acquired");
  int ar = __sync_add_and_fetch(&active_readers, 1);
  // atomic max update
  int prev;
  while(ar > (prev = max_active_readers)) {
    if(__sync_val_compare_and_swap(&max_active_readers, prev, ar) == prev)
      break;
  }
  __sync_add_and_fetch(&t1_started_readers, 1);
  // Wait until main test signals release so all readers hold lock concurrently.
  while(!t1_release_readers) {
   scheduler_yield();
  }
  __sync_add_and_fetch(&active_readers, -1);
  rwlock_release(&test_lock);
  check_rwlock_integrity("T1 reader released");
  __sync_add_and_fetch(&t1_done_readers, 1);
}

/* Reader for Test 2 */
static void t2_reader(uint64 a1, uint64 a2) {
  if(rwlock_acquire_read(&test_lock) != 0) { error_flag = 1; return; }
  check_rwlock_integrity("T2 reader acquired");
  __sync_add_and_fetch(&active_readers, 1);
  // Simulate work by yielding a few times while holding read lock
  for(int i=0;i<5;i++)scheduler_yield();
  __sync_add_and_fetch(&active_readers, -1);
  rwlock_release(&test_lock);
  check_rwlock_integrity("T2 reader released");
  __sync_add_and_fetch(&t2_done_readers, 1);
}

/* Writer for Test 2 */
static void t2_writer(uint64 a1, uint64 a2) {
  if(rwlock_acquire_write(&test_lock) != 0) { error_flag = 1; return; }
  check_rwlock_integrity("T2 writer acquired");
  if(active_readers != 0) {
    printf("[rwlock][T2] writer saw active_readers=%d (expected 0)\n", active_readers);
    error_flag = 1;
  }
  active_writers = 1;
  t2_writer_acquired = 1;
  for(int i=0;i<5;i++)scheduler_yield();
  active_writers = 0;
  rwlock_release(&test_lock);
  check_rwlock_integrity("T2 writer released");
}

/* Writer for Test 3 */
static void t3_writer(uint64 a1, uint64 a2) {
  if(rwlock_acquire_write(&test_lock) != 0) { error_flag = 1; return; }
  check_rwlock_integrity("T3 writer acquired");
  if(active_writers != 0) {
    printf("[rwlock][T3] mutual exclusion violated (active_writers=%d)\n", active_writers);
    error_flag = 1;
  }
  active_writers = 1;
 scheduler_yield();scheduler_yield();scheduler_yield();
  active_writers = 0;
  rwlock_release(&test_lock);
  check_rwlock_integrity("T3 writer released");
  __sync_add_and_fetch(&t3_done_writers, 1);
}

/* Writer for Test 4 */
static void t4_writer(uint64 a1, uint64 a2) {
  // Wait on sleeplock barrier: master holds it initially; writers block in mutex_lock
  if(mutex_lock(&t4_start_lock) != 0) return; // will hold barrier for first writer only
  mutex_unlock(&t4_start_lock); // Immediately release so everyone can proceed after barrier opens
  for(int iter=0; iter < T4_WRITER_ITERS; iter++) {
    if(rwlock_acquire_write(&test_lock) != 0) { error_flag = 1; return; }
    check_rwlock_integrity("T4 writer acquired");
    int new_version = t4_ds.version + 1;
    t4_ds.version = new_version;
    t4_ds.len = T4_DATA_LEN;
    int sum = 0;
    for(int i=0;i<T4_DATA_LEN;i++) {
      int val = (new_version << 16) ^ (i * 0x9e37);
      t4_ds.data[i] = val;
      sum += val;
    }
    t4_ds.checksum = sum;
    rwlock_release(&test_lock);
    check_rwlock_integrity("T4 writer released");
   scheduler_yield(); // allow readers to interleave
  }
  __sync_add_and_fetch(&t4_writers_done, 1);
}

/* Reader for Test 4 */
static void t4_reader(uint64 a1, uint64 a2) {
  if(mutex_lock(&t4_start_lock) != 0) return; // barrier
  mutex_unlock(&t4_start_lock);
  for(;;) {
    if(rwlock_acquire_read(&test_lock) != 0) { error_flag = 1; return; }
    check_rwlock_integrity("T4 reader acquired");
    int version = t4_ds.version;
    int len = t4_ds.len;
    int checksum = t4_ds.checksum;
    if(len != T4_DATA_LEN) {
      if(__sync_add_and_fetch(&t4_error_logs,1) <= 10)
        printf("[rwlock][T4] len mismatch %d\n", len);
      error_flag = 1;
    } else if(version > 0) {
      int sum = 0;
      for(int i=0;i<len;i++) {
        int expected = (version << 16) ^ (i * 0x9e37);
        int got = t4_ds.data[i];
        if(got != expected) {
          if(__sync_add_and_fetch(&t4_error_logs,1) <= 10)
            printf("[rwlock][T4] data[%d]=%x expected %x (ver=%d)\n", i, got, expected, version);
          error_flag = 1;
          break;
        }
        sum += got;
      }
      if(sum != checksum) {
        if(__sync_add_and_fetch(&t4_error_logs,1) <= 10)
          printf("[rwlock][T4] checksum mismatch sum=%x stored=%x ver=%d\n", sum, checksum, version);
        error_flag = 1;
      }
    }
    rwlock_release(&test_lock);
    check_rwlock_integrity("T4 reader released");
    if(t4_writers_done >= T4_WRITER_THREADS) break;
   scheduler_yield();
  }
  __sync_add_and_fetch(&t4_reader_done, 1);
}

static int wait_for(volatile int *ptr, int expected, int spin_loops) {
  while(spin_loops-- > 0) {
    if(*ptr == expected) return 0;
   scheduler_yield();
  }
  return -1;
}

static void run_test1(void) {
  printf("[rwlock][T1] multiple readers... ");
  struct thread *np = NULL;
  t1_target_readers = 4;
  t1_done_readers = 0;
  t1_started_readers = 0;
  t1_release_readers = 0;
  active_readers = 0; max_active_readers = 0; error_flag = 0;
  for(int i=0;i<t1_target_readers;i++) {
    if(kthread_create("run_test1", &np, t1_reader, 0, 0, KERNEL_STACK_ORDER) < 0)
      error_flag = 1;
    else
      wakeup(np);
  }
  if(wait_for(&t1_started_readers, t1_target_readers, 50000) != 0)
    error_flag = 1;
  t1_release_readers = 1;
  __sync_synchronize();
  if(wait_for(&t1_done_readers, t1_target_readers, 50000) != 0)
    error_flag = 1;
  if(max_active_readers != t1_target_readers) {
    printf("(observed max=%d started=%d expected=%d) ", max_active_readers, t1_started_readers, t1_target_readers);
    error_flag = 1;
  }
  check_rwlock_integrity("T1 final");
  printf(error_flag?"FAIL\n":"OK\n");
}

static void run_test2(void) {
  printf("[rwlock][T2] writer waits for readers... ");
  struct thread *np = NULL;
  t2_target_readers = 3;
  t2_done_readers = 0;
  t2_writer_acquired = 0;
  active_readers = 0; active_writers = 0; error_flag = 0;
  for(int i=0;i<t2_target_readers;i++) {
    if(kthread_create("run_test2", &np, t2_reader, 0, 0, KERNEL_STACK_ORDER) < 0)
      error_flag = 1;
    else
      wakeup(np);
  }
  // Wait until all readers finished
  if(wait_for(&t2_done_readers, t2_target_readers, 80000) != 0)
    error_flag = 1;
  if(kthread_create("run_test2", &np, t2_writer, 0, 0, KERNEL_STACK_ORDER) < 0)
    error_flag = 1;
  else
    wakeup(np);
  if(wait_for(&t2_writer_acquired, 1, 40000) != 0)
    error_flag = 1;
  if(active_readers != 0) error_flag = 1;
  check_rwlock_integrity("T2 final");
  printf(error_flag?"FAIL\n":"OK\n");
}

static void run_test3(void) {
  printf("[rwlock][T3] mutual exclusion for writers... ");
  struct thread *np = NULL;
  t3_done_writers = 0; active_writers = 0; error_flag = 0;
  if(kthread_create("run_test3", &np, t3_writer, 0, 0, KERNEL_STACK_ORDER) < 0)
    error_flag = 1;
  else
    wakeup(np);
  if(kthread_create("run_test3", &np, t3_writer, 0, 0, KERNEL_STACK_ORDER) < 0)
    error_flag = 1;
  else
    wakeup(np);
  if(wait_for(&t3_done_writers, 2, 80000) != 0) error_flag = 1;
  check_rwlock_integrity("T3 final");
  printf(error_flag?"FAIL\n":"OK\n");
}

static void run_test4(void) {
  printf("[rwlock][T4] data consistency under stress... ");
  struct thread *np = NULL;
  error_flag = 0;
  t4_ds.version = 0;
  t4_ds.len = T4_DATA_LEN;
  t4_ds.checksum = 0;
  for(int i=0;i<T4_DATA_LEN;i++) t4_ds.data[i] = 0;
  t4_writers_done = 0;
  t4_reader_done = 0;
  t4_start_barrier = 0;
  t4_error_logs = 0;
  mutex_init(&t4_start_lock, "t4start");
  // Acquire barrier sleeplock so spawned threads block when they try to acquire
  if(mutex_lock(&t4_start_lock) != 0) error_flag = 1;
  for(int i=0;i<T4_WRITER_THREADS;i++)
    if(kthread_create("run_test4", &np, t4_writer, 0, 0, KERNEL_STACK_ORDER) < 0)
      error_flag = 1;
    else
      wakeup(np);
  for(int i=0;i<T4_READER_THREADS;i++)
    if(kthread_create("run_test4", &np, t4_reader, 0, 0, KERNEL_STACK_ORDER) < 0)
      error_flag = 1;
    else
      wakeup(np);
  // Release barrier
  mutex_unlock(&t4_start_lock);
  if(wait_for(&t4_writers_done, T4_WRITER_THREADS, 400000) != 0) error_flag = 1;
  if(wait_for(&t4_reader_done, T4_READER_THREADS, 400000) != 0) error_flag = 1;
  check_rwlock_integrity("T4 final");
  printf(error_flag?"FAIL\n":"OK\n");
}

static void rwlock_test_master(uint64 a1, uint64 a2) {
  for (int i = 0; i < 10000; i++) {
   scheduler_yield();
  }
  printf("[rwlock] starting simple rwlock tests\n");
  if(rwlock_init(&test_lock, 0, "rwlock-test") != 0) {
    printf("[rwlock] init failed\n");
    return;
  }
  check_rwlock_integrity("init");
  run_test1();
  run_test2();
  run_test3();
  run_test4();
  printf("[rwlock] tests finished\n");
}

void rwlock_launch_tests(void) {
  struct thread *np = NULL;
  if(kthread_create("rwlock_test_master", &np, rwlock_test_master, 0, 0, KERNEL_STACK_ORDER) < 0) {
    printf("[rwlock] cannot create test master thread\n");
  } else {
    wakeup(np);
  }
}
