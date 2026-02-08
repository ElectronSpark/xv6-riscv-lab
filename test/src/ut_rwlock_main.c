#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "host_test_stubs.h"
#include "types.h"
#include "rwsem.h"
#include "proc/thread.h"
#include "proc/tq.h"
#include "spinlock.h"
#include "proc/sched.h"
#include "list.h"
#include "wrapper_tracking.h"

typedef struct {
    spinlock_tracking_t spinlock;
    tq_tracking_t tq;
    proc_tracking_t proc;
    
    int wait_calls;
    
    tq_t *last_wait_queue;
    tq_t *last_wake_queue;
} fake_runtime_t;

static fake_runtime_t g_runtime;
static struct thread g_self_proc;
static struct thread g_wait_proc;

static void expect_integrity(const rwsem_t *lock, const char *label) {
    assert_non_null(lock);
    if (lock->readers < 0) {
        fail_msg("%s: readers negative (%d)", label, lock->readers);
    }
    if (lock->readers > 0 && lock->holder_pid != -1) {
        fail_msg("%s: reader/writer overlap (readers=%d)", label, lock->readers);
    }
    if (lock->holder_pid != -1 && lock->readers != 0) {
        fail_msg("%s: holder present with readers=%d", label, lock->readers);
    }
    if (lock->read_queue.lock != &lock->lock) {
        fail_msg("%s: read queue lock mismatch", label);
    }
    if (lock->write_queue.lock != &lock->lock) {
        fail_msg("%s: write queue lock mismatch", label);
    }
    assert_true(tq_size((tq_t *)&lock->read_queue) >= 0);
    assert_true(tq_size((tq_t *)&lock->write_queue) >= 0);
}

static int test_setup(void **state) {
    (void)state;
    memset(&g_runtime, 0, sizeof(g_runtime));
    memset(&g_self_proc, 0, sizeof(g_self_proc));
    memset(&g_wait_proc, 0, sizeof(g_wait_proc));
    g_self_proc.pid = 1;  // Set distinct PIDs for test procs
    g_wait_proc.pid = 2;
    g_runtime.tq.wait_return = 0;
    g_runtime.tq.wakeup_return = 0;
    g_runtime.tq.wakeup_all_return = 0;
    g_runtime.tq.next_wakeup = &g_wait_proc;
    
    // Set up proc tracking so current returns g_self_proc
    g_runtime.proc.current_proc = &g_self_proc;
    
    // Enable tracking
    wrapper_tracking_enable_spinlock(&g_runtime.spinlock);
    wrapper_tracking_enable_tq(&g_runtime.tq);
    wrapper_tracking_enable_proc(&g_runtime.proc);
    
    return 0;
}

static void test_rwsem_init_integrity(void **state) {
    (void)state;
    rwsem_t lock;
    assert_int_equal(rwsem_init(&lock, 0, "ut"), 0);
    expect_integrity(&lock, "after init");
    assert_int_equal(lock.readers, 0);
    assert_int_equal(lock.holder_pid, -1);
}

static void test_rwsem_read_acquire_release_integrity(void **state) {
    (void)state;
    rwsem_t lock;
    assert_int_equal(rwsem_init(&lock, 0, "ut"), 0);
    assert_int_equal(rwsem_acquire_read(&lock), 0);
    assert_int_equal(lock.readers, 1);
    assert_int_equal(lock.holder_pid, -1);
    expect_integrity(&lock, "after read acquire");
    rwsem_release(&lock);
    assert_int_equal(lock.readers, 0);
    expect_integrity(&lock, "after read release");
}

static void test_rwsem_write_acquire_release_integrity(void **state) {
    (void)state;
    rwsem_t lock;
    assert_int_equal(rwsem_init(&lock, 0, "ut"), 0);
    assert_int_equal(rwsem_acquire_write(&lock), 0);
    assert_int_equal(lock.holder_pid, g_self_proc.pid);
    expect_integrity(&lock, "after write acquire");
    rwsem_release(&lock);
    assert_true(lock.holder_pid == -1 || lock.holder_pid == g_wait_proc.pid);
    expect_integrity(&lock, "after write release");
}

static void test_rwsem_release_wakes_writer_integrity(void **state) {
    (void)state;
    rwsem_t lock;
    assert_int_equal(rwsem_init(&lock, RWLOCK_PRIO_WRITE, "ut"), 0);
    assert_int_equal(rwsem_acquire_write(&lock), 0);
    lock.write_queue.counter = 1;
    g_runtime.tq.next_wakeup = &g_wait_proc;
    rwsem_release(&lock);
    assert_int_equal(tq_size(&lock.write_queue), 0);
    assert_int_equal(lock.holder_pid, g_wait_proc.pid);
    expect_integrity(&lock, "writer wake");
    assert_int_equal(g_runtime.tq.queue_wakeup_count, 1);
}

static void test_rwsem_release_wakes_readers_integrity(void **state) {
    (void)state;
    rwsem_t lock;
    assert_int_equal(rwsem_init(&lock, 0, "ut"), 0);
    assert_int_equal(rwsem_acquire_write(&lock), 0);
    lock.read_queue.counter = 3;
    rwsem_release(&lock);
    assert_int_equal(tq_size(&lock.read_queue), 0);
    expect_integrity(&lock, "reader wake");
    assert_int_equal(g_runtime.tq.queue_wakeup_all_count, 1);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_rwsem_init_integrity, test_setup),
        cmocka_unit_test_setup(test_rwsem_read_acquire_release_integrity, test_setup),
        cmocka_unit_test_setup(test_rwsem_write_acquire_release_integrity, test_setup),
        cmocka_unit_test_setup(test_rwsem_release_wakes_writer_integrity, test_setup),
        cmocka_unit_test_setup(test_rwsem_release_wakes_readers_integrity, test_setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
