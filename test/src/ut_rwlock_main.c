#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "host_test_stubs.h"
#include "types.h"
#include "rwlock.h"
#include "proc.h"
#include "proc_queue.h"
#include "spinlock.h"
#include "sched.h"
#include "list.h"

typedef struct {
    int spin_init_count;
    int spin_acquire_count;
    int spin_release_count;

    int wait_calls;
    int wake_reader_calls;
    int wake_writer_calls;

    proc_queue_t *last_wait_queue;
    proc_queue_t *last_wake_queue;

    int wait_return;
    int wakeup_return;
    int wakeup_all_return;

    struct proc *next_wakeup_proc;
} fake_runtime_t;

static fake_runtime_t g_runtime;
static struct proc g_self_proc;
static struct proc g_wait_proc;
static struct cpu g_cpu_stub;

void spin_init(struct spinlock *lk, char *name) {
    g_runtime.spin_init_count++;
    if (lk != NULL) {
        lk->name = name;
        lk->locked = 0;
        lk->cpu = NULL;
    }
}

void spin_acquire(struct spinlock *lk) {
    g_runtime.spin_acquire_count++;
    if (lk != NULL) {
        __atomic_store_n(&lk->locked, 1, __ATOMIC_SEQ_CST);
    }
}

void spin_release(struct spinlock *lk) {
    g_runtime.spin_release_count++;
    if (lk != NULL) {
        __atomic_store_n(&lk->locked, 0, __ATOMIC_SEQ_CST);
    }
}

int spin_holding(struct spinlock *lk) {
    (void)lk;
    return 0;
}

void push_off(void) {}
void pop_off(void) {}

struct cpu *mycpu(void) {
    return &g_cpu_stub;
}

struct proc *myproc(void) {
    return &g_self_proc;
}

void proc_lock(struct proc *p) {
    (void)p;
}

void proc_unlock(struct proc *p) {
    (void)p;
}

void proc_assert_holding(struct proc *p) {
    (void)p;
}

void sched_lock(void) {}
void sched_unlock(void) {}
void scheduler_wakeup(struct proc *p) { (void)p; }
void scheduler_sleep(struct spinlock *lk, enum procstate state) {
    (void)lk;
    (void)state;
}

void proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock) {
    if (q == NULL) {
        return;
    }
    list_entry_init(&q->head);
    q->counter = 0;
    q->name = name;
    q->lock = lock;
    q->flags = 0;
}

int proc_queue_size(proc_queue_t *q) {
    if (q == NULL) {
        return -1;
    }
    return q->counter;
}

int proc_queue_wait(proc_queue_t *q, struct spinlock *lock, uint64 *rdata) {
    (void)lock;
    (void)rdata;
    g_runtime.wait_calls++;
    g_runtime.last_wait_queue = q;
    if (q != NULL) {
        q->counter++;
    }
    return g_runtime.wait_return;
}

int proc_queue_wakeup(proc_queue_t *q, int error_no, uint64 rdata, struct proc **retp) {
    (void)error_no;
    (void)rdata;
    g_runtime.wake_writer_calls++;
    g_runtime.last_wake_queue = q;
    if (q != NULL && q->counter > 0) {
        q->counter--;
    }
    if (retp != NULL) {
        if (g_runtime.next_wakeup_proc != NULL) {
            *retp = g_runtime.next_wakeup_proc;
        } else {
            *retp = &g_wait_proc;
        }
    }
    return g_runtime.wakeup_return;
}

int proc_queue_wakeup_all(proc_queue_t *q, int error_no, uint64 rdata) {
    (void)error_no;
    (void)rdata;
    g_runtime.wake_reader_calls++;
    if (q != NULL) {
        q->counter = 0;
    }
    return g_runtime.wakeup_all_return;
}

static void expect_integrity(const rwlock_t *lock, const char *label) {
    assert_non_null(lock);
    if (lock->readers < 0) {
        fail_msg("%s: readers negative (%d)", label, lock->readers);
    }
    if (lock->readers > 0 && lock->holder_pid != 0) {
        fail_msg("%s: reader/writer overlap (readers=%d)", label, lock->readers);
    }
    if (lock->holder_pid != 0 && lock->readers != 0) {
        fail_msg("%s: holder present with readers=%d", label, lock->readers);
    }
    if (lock->read_queue.lock != &lock->lock) {
        fail_msg("%s: read queue lock mismatch", label);
    }
    if (lock->write_queue.lock != &lock->lock) {
        fail_msg("%s: write queue lock mismatch", label);
    }
    assert_true(proc_queue_size((proc_queue_t *)&lock->read_queue) >= 0);
    assert_true(proc_queue_size((proc_queue_t *)&lock->write_queue) >= 0);
}

static int test_setup(void **state) {
    (void)state;
    memset(&g_runtime, 0, sizeof(g_runtime));
    memset(&g_self_proc, 0, sizeof(g_self_proc));
    memset(&g_wait_proc, 0, sizeof(g_wait_proc));
    g_self_proc.pid = 1;  // Set distinct PIDs for test procs
    g_wait_proc.pid = 2;
    g_runtime.wait_return = 0;
    g_runtime.wakeup_return = 0;
    g_runtime.wakeup_all_return = 0;
    g_runtime.next_wakeup_proc = &g_wait_proc;
    return 0;
}

static void test_rwlock_init_integrity(void **state) {
    (void)state;
    rwlock_t lock;
    assert_int_equal(rwlock_init(&lock, 0, "ut"), 0);
    expect_integrity(&lock, "after init");
    assert_int_equal(lock.readers, 0);
    assert_int_equal(lock.holder_pid, 0);
}

static void test_rwlock_read_acquire_release_integrity(void **state) {
    (void)state;
    rwlock_t lock;
    assert_int_equal(rwlock_init(&lock, 0, "ut"), 0);
    assert_int_equal(rwlock_acquire_read(&lock), 0);
    assert_int_equal(lock.readers, 1);
    assert_int_equal(lock.holder_pid, 0);
    expect_integrity(&lock, "after read acquire");
    rwlock_release(&lock);
    assert_int_equal(lock.readers, 0);
    expect_integrity(&lock, "after read release");
}

static void test_rwlock_write_acquire_release_integrity(void **state) {
    (void)state;
    rwlock_t lock;
    assert_int_equal(rwlock_init(&lock, 0, "ut"), 0);
    assert_int_equal(rwlock_acquire_write(&lock), 0);
    assert_int_equal(lock.holder_pid, g_self_proc.pid);
    expect_integrity(&lock, "after write acquire");
    rwlock_release(&lock);
    assert_true(lock.holder_pid == 0 || lock.holder_pid == g_wait_proc.pid);
    expect_integrity(&lock, "after write release");
}

static void test_rwlock_release_wakes_writer_integrity(void **state) {
    (void)state;
    rwlock_t lock;
    assert_int_equal(rwlock_init(&lock, RWLOCK_PRIO_WRITE, "ut"), 0);
    assert_int_equal(rwlock_acquire_write(&lock), 0);
    lock.write_queue.counter = 1;
    g_runtime.next_wakeup_proc = &g_wait_proc;
    rwlock_release(&lock);
    assert_int_equal(proc_queue_size(&lock.write_queue), 0);
    assert_int_equal(lock.holder_pid, g_wait_proc.pid);
    expect_integrity(&lock, "writer wake");
    assert_int_equal(g_runtime.wake_writer_calls, 1);
}

static void test_rwlock_release_wakes_readers_integrity(void **state) {
    (void)state;
    rwlock_t lock;
    assert_int_equal(rwlock_init(&lock, 0, "ut"), 0);
    assert_int_equal(rwlock_acquire_write(&lock), 0);
    lock.read_queue.counter = 3;
    rwlock_release(&lock);
    assert_int_equal(proc_queue_size(&lock.read_queue), 0);
    expect_integrity(&lock, "reader wake");
    assert_int_equal(g_runtime.wake_reader_calls, 1);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_rwlock_init_integrity, test_setup),
        cmocka_unit_test_setup(test_rwlock_read_acquire_release_integrity, test_setup),
        cmocka_unit_test_setup(test_rwlock_write_acquire_release_integrity, test_setup),
        cmocka_unit_test_setup(test_rwlock_release_wakes_writer_integrity, test_setup),
        cmocka_unit_test_setup(test_rwlock_release_wakes_readers_integrity, test_setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
