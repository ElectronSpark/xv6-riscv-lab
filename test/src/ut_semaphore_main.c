#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>

#include "types.h"
#include "errno.h"
#include "list.h"
#include "semaphore.h"

// Fake runtime observers for host testing
typedef struct {
    int spin_init_count;
    struct spinlock *last_spin_init;
    const char *last_spin_name;
    int spin_acquire_count;
    struct spinlock *last_spin_acquire;
    int spin_release_count;
    struct spinlock *last_spin_release;

    int queue_init_count;
    proc_queue_t *last_queue_init;
    const char *last_queue_name;
    struct spinlock *last_queue_lock;

    int queue_wait_count;
    proc_queue_t *last_queue_wait;
    struct spinlock *last_wait_lock;

    int queue_wakeup_count;
    proc_queue_t *last_queue_wakeup;
    int last_wakeup_errno;
    uint64 last_wakeup_rdata;

    int wait_return;
    int wakeup_return;

    sem_t *wait_sem;
    int simulate_post_increment;
} fake_runtime_t;

static fake_runtime_t g_runtime;

static inline int sem_value_load(sem_t *sem) {
    return __atomic_load_n(&sem->value, __ATOMIC_SEQ_CST);
}

static inline void sem_value_store(sem_t *sem, int value) {
    __atomic_store_n(&sem->value, value, __ATOMIC_SEQ_CST);
}

// ---- Stubs for kernel primitives -----------------------------------------

void spin_init(struct spinlock *lk, char *name) {
    g_runtime.spin_init_count++;
    g_runtime.last_spin_init = lk;
    g_runtime.last_spin_name = name;
    if (lk != NULL) {
        lk->name = name;
        lk->locked = 0;
        lk->cpu = NULL;
    }
}

void spin_acquire(struct spinlock *lk) {
    g_runtime.spin_acquire_count++;
    g_runtime.last_spin_acquire = lk;
    if (lk != NULL) {
        __atomic_store_n(&lk->locked, 1, __ATOMIC_SEQ_CST);
    }
}

void spin_release(struct spinlock *lk) {
    g_runtime.spin_release_count++;
    g_runtime.last_spin_release = lk;
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

void proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock) {
    g_runtime.queue_init_count++;
    g_runtime.last_queue_init = q;
    g_runtime.last_queue_name = name;
    g_runtime.last_queue_lock = lock;
    if (q != NULL) {
        list_entry_init(&q->head);
        q->counter = 0;
        q->name = name;
        q->lock = lock;
        q->flags = 0;
    }
}

int proc_queue_wait(proc_queue_t *q, struct spinlock *lock, uint64 *rdata) {
    (void)rdata;
    g_runtime.queue_wait_count++;
    g_runtime.last_queue_wait = q;
    g_runtime.last_wait_lock = lock;
    if (g_runtime.simulate_post_increment && g_runtime.wait_sem != NULL) {
        __atomic_add_fetch(&g_runtime.wait_sem->value, 1, __ATOMIC_SEQ_CST);
    }
    return g_runtime.wait_return;
}

int proc_queue_wakeup(proc_queue_t *q, int error_no, uint64 rdata, struct proc **retp) {
    (void)retp;
    g_runtime.queue_wakeup_count++;
    g_runtime.last_queue_wakeup = q;
    g_runtime.last_wakeup_errno = error_no;
    g_runtime.last_wakeup_rdata = rdata;
    return g_runtime.wakeup_return;
}

// ---- Test helpers --------------------------------------------------------

static int test_setup(void **state) {
    (void)state;
    memset(&g_runtime, 0, sizeof(g_runtime));
    return 0;
}

static void assert_spin_locked_counts(int acquire, int release) {
    assert_int_equal(g_runtime.spin_acquire_count, acquire);
    assert_int_equal(g_runtime.spin_release_count, release);
}

// ---- Tests: sem_init -----------------------------------------------------

static void test_sem_init_rejects_null(void **state) {
    (void)state;
    assert_int_equal(sem_init(NULL, "test", 1), -EINVAL);
}

static void test_sem_init_rejects_negative_initial_value(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", -1), -EINVAL);
}

static void test_sem_init_defaults_name_and_initialises_lock(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, NULL, 2), 0);
    assert_string_equal(sem.name, "unnamed");
    assert_int_equal(sem_value_load(&sem), 2);
    assert_int_equal(g_runtime.spin_init_count, 1);
    assert_ptr_equal(g_runtime.last_spin_init, &sem.lk);
    assert_string_equal(g_runtime.last_spin_name, "semaphore spinlock");
    assert_int_equal(g_runtime.queue_init_count, 1);
    assert_ptr_equal(g_runtime.last_queue_init, &sem.wait_queue);
    assert_ptr_equal(sem.wait_queue.lock, &sem.lk);
}

// ---- Tests: sem_wait -----------------------------------------------------

static void test_sem_wait_rejects_null(void **state) {
    (void)state;
    assert_int_equal(sem_wait(NULL), -EINVAL);
}

static void test_sem_wait_fast_path_consumes_token(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 1), 0);
    g_runtime.wait_return = 0;

    assert_int_equal(sem_wait(&sem), 0);
    assert_spin_locked_counts(1, 1);
    assert_int_equal(g_runtime.queue_wait_count, 0);
    assert_int_equal(sem_value_load(&sem), 0);
}

static void test_sem_wait_blocks_and_resumes_via_post(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 0), 0);
    g_runtime.wait_return = 0;
    g_runtime.simulate_post_increment = 1;
    g_runtime.wait_sem = &sem;

    assert_int_equal(sem_wait(&sem), 0);
    assert_int_equal(g_runtime.queue_wait_count, 1);
    assert_ptr_equal(g_runtime.last_queue_wait, &sem.wait_queue);
    assert_ptr_equal(g_runtime.last_wait_lock, &sem.lk);
    assert_int_equal(sem_value_load(&sem), 0);
    assert_int_equal(g_runtime.queue_wakeup_count, 0);
}

static void test_sem_wait_interrupt_restores_count_and_wakes_another(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 0), 0);
    g_runtime.wait_return = -EINTR;
    g_runtime.simulate_post_increment = 0;
    g_runtime.wakeup_return = 0;

    assert_int_equal(sem_wait(&sem), -EINTR);
    assert_int_equal(g_runtime.queue_wait_count, 1);
    assert_int_equal(g_runtime.queue_wakeup_count, 1);
    assert_int_equal(g_runtime.last_wakeup_errno, 0);
    assert_int_equal(sem_value_load(&sem), 0);
}

static void test_sem_wait_detects_underflow(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 0), 0);
    sem_value_store(&sem, -SEM_VALUE_MAX);

    assert_int_equal(sem_wait(&sem), -EOVERFLOW);
    assert_int_equal(g_runtime.queue_wait_count, 0);
    assert_int_equal(sem_value_load(&sem), -SEM_VALUE_MAX);
}

// ---- Tests: sem_trywait --------------------------------------------------

static void test_sem_trywait_rejects_null(void **state) {
    (void)state;
    assert_int_equal(sem_trywait(NULL), -EINVAL);
}

static void test_sem_trywait_success(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 3), 0);
    assert_int_equal(sem_trywait(&sem), 0);
    assert_int_equal(sem_value_load(&sem), 2);
}

static void test_sem_trywait_eagain_when_empty(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 0), 0);
    assert_int_equal(sem_trywait(&sem), -EAGAIN);
    assert_int_equal(sem_value_load(&sem), 0);
}

// ---- Tests: sem_post -----------------------------------------------------

static void test_sem_post_rejects_null(void **state) {
    (void)state;
    assert_int_equal(sem_post(NULL), -EINVAL);
}

static void test_sem_post_increments_without_wakeup_when_positive(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 1), 0);
    g_runtime.wakeup_return = 0;

    assert_int_equal(sem_post(&sem), 0);
    assert_int_equal(sem_value_load(&sem), 2);
    assert_int_equal(g_runtime.queue_wakeup_count, 0);
}

static void test_sem_post_wakes_waiter_when_count_non_positive(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 0), 0);
    sem_value_store(&sem, -1);
    g_runtime.wakeup_return = 0;

    assert_int_equal(sem_post(&sem), 0);
    assert_int_equal(g_runtime.queue_wakeup_count, 1);
    assert_ptr_equal(g_runtime.last_queue_wakeup, &sem.wait_queue);
    assert_int_equal(sem_value_load(&sem), 0);
}

static void test_sem_post_rejects_overflow(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", SEM_VALUE_MAX), 0);

    assert_int_equal(sem_post(&sem), -EOVERFLOW);
    assert_int_equal(g_runtime.queue_wakeup_count, 0);
    assert_int_equal(sem_value_load(&sem), SEM_VALUE_MAX);
}

// ---- Tests: sem_getvalue -------------------------------------------------

static void test_sem_getvalue_rejects_nulls(void **state) {
    (void)state;
    sem_t sem;
    int value;
    assert_int_equal(sem_getvalue(NULL, &value), -EINVAL);
    assert_int_equal(sem_init(&sem, "s", 4), 0);
    assert_int_equal(sem_getvalue(&sem, NULL), -EINVAL);
}

static void test_sem_getvalue_reports_current_value(void **state) {
    (void)state;
    sem_t sem;
    assert_int_equal(sem_init(&sem, "s", 7), 0);
    int value = 0;
    assert_int_equal(sem_getvalue(&sem, &value), 0);
    assert_int_equal(value, 7);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_sem_init_rejects_null, test_setup),
        cmocka_unit_test_setup(test_sem_init_rejects_negative_initial_value, test_setup),
        cmocka_unit_test_setup(test_sem_init_defaults_name_and_initialises_lock, test_setup),

        cmocka_unit_test_setup(test_sem_wait_rejects_null, test_setup),
        cmocka_unit_test_setup(test_sem_wait_fast_path_consumes_token, test_setup),
        cmocka_unit_test_setup(test_sem_wait_blocks_and_resumes_via_post, test_setup),
        cmocka_unit_test_setup(test_sem_wait_interrupt_restores_count_and_wakes_another, test_setup),
        cmocka_unit_test_setup(test_sem_wait_detects_underflow, test_setup),

        cmocka_unit_test_setup(test_sem_trywait_rejects_null, test_setup),
        cmocka_unit_test_setup(test_sem_trywait_success, test_setup),
        cmocka_unit_test_setup(test_sem_trywait_eagain_when_empty, test_setup),

        cmocka_unit_test_setup(test_sem_post_rejects_null, test_setup),
        cmocka_unit_test_setup(test_sem_post_increments_without_wakeup_when_positive, test_setup),
        cmocka_unit_test_setup(test_sem_post_wakes_waiter_when_count_non_positive, test_setup),
        cmocka_unit_test_setup(test_sem_post_rejects_overflow, test_setup),

        cmocka_unit_test_setup(test_sem_getvalue_rejects_nulls, test_setup),
        cmocka_unit_test_setup(test_sem_getvalue_reports_current_value, test_setup),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
