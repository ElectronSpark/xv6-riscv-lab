#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "proc/workqueue.h"

extern void pcache_test_run_pending_work(void);
extern void pcache_test_fail_next_queue_work(void);

static int g_workqueue_ctor_calls;
static int g_workqueue_dtor_calls;
static int g_work_run_calls;
static int g_work_fault_calls;

static void test_reset_globals(void) {
    g_workqueue_ctor_calls = 0;
    g_workqueue_dtor_calls = 0;
    g_work_run_calls = 0;
    g_work_fault_calls = 0;
}

static void test_workqueue_ctor_cb(struct workqueue *wq) {
    (void)wq;
    g_workqueue_ctor_calls++;
}

static void test_workqueue_dtor_cb(struct workqueue *wq) {
    (void)wq;
    g_workqueue_dtor_calls++;
}

static void test_work_cb(struct work_struct *work) {
    (void)work;
    g_work_run_calls++;
}

static void test_work_fault_cb(struct work_struct *work) {
    (void)work;
    g_work_fault_calls++;
}

static void test_create_with_callbacks_invokes_ctor(void **state) {
    (void)state;
    test_reset_globals();

    struct workqueue_callbacks callbacks = {
        .workqueue_ctor = test_workqueue_ctor_cb,
        .workqueue_dtor = test_workqueue_dtor_cb,
    };

    struct workqueue *wq =
        workqueue_create_with_callbacks("ut_wq", 1, &callbacks);
    assert_non_null(wq);
    assert_int_equal(g_workqueue_ctor_calls, 1);
    assert_int_equal(g_workqueue_dtor_calls, 0);
}

static void test_kill_invokes_dtor_once(void **state) {
    (void)state;
    test_reset_globals();

    struct workqueue_callbacks callbacks = {
        .workqueue_ctor = test_workqueue_ctor_cb,
        .workqueue_dtor = test_workqueue_dtor_cb,
    };

    struct workqueue *wq =
        workqueue_create_with_callbacks("ut_wq_kill", 1, &callbacks);
    assert_non_null(wq);

    assert_int_equal(workqueue_kill(wq), 0);
    assert_int_equal(workqueue_kill(wq), 0);
    assert_int_equal(g_workqueue_dtor_calls, 1);
    assert_int_equal(wq->active, 0);
}

static void test_init_and_create_work_struct_ex_fields(void **state) {
    (void)state;
    test_reset_globals();

    struct work_struct stack_work;
    init_work_struct_ex(&stack_work, test_work_cb, test_work_fault_cb, 0x1234,
                        WORK_STRUCT_FLAG_RUN_ON_DRAIN |
                            WORK_STRUCT_FLAG_FREE_AFTER_RUN);
    assert_ptr_equal(stack_work.func, test_work_cb);
    assert_ptr_equal(stack_work.fault, test_work_fault_cb);
    assert_int_equal(stack_work.data, 0x1234);
    assert_int_equal(stack_work.flags,
                     WORK_STRUCT_FLAG_RUN_ON_DRAIN |
                         WORK_STRUCT_FLAG_FREE_AFTER_RUN);

    struct work_struct *heap_work =
        create_work_struct_ex(test_work_cb, test_work_fault_cb, 0x5678,
                              WORK_STRUCT_FLAG_RUN_ON_DRAIN);
    assert_non_null(heap_work);
    assert_ptr_equal(heap_work->func, test_work_cb);
    assert_ptr_equal(heap_work->fault, test_work_fault_cb);
    assert_int_equal(heap_work->data, 0x5678);
    assert_int_equal(heap_work->flags, WORK_STRUCT_FLAG_RUN_ON_DRAIN);
    free_work_struct(heap_work);
}

static void test_queue_work_and_run_pending(void **state) {
    (void)state;
    test_reset_globals();

    struct workqueue *wq = workqueue_create("ut_queue", 1);
    assert_non_null(wq);

    struct work_struct *work = create_work_struct(test_work_cb, 0);
    assert_non_null(work);

    assert_true(queue_work(wq, work));
    assert_int_equal(g_work_run_calls, 0);
    pcache_test_run_pending_work();
    assert_int_equal(g_work_run_calls, 1);

    free_work_struct(work);
}

static void test_queue_work_failure_injection(void **state) {
    (void)state;
    test_reset_globals();

    struct workqueue *wq = workqueue_create("ut_queue_fail", 1);
    assert_non_null(wq);

    struct work_struct *work = create_work_struct(test_work_cb, 0);
    assert_non_null(work);

    pcache_test_fail_next_queue_work();
    assert_false(queue_work(wq, work));
    assert_int_equal(g_work_run_calls, 0);

    free_work_struct(work);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_create_with_callbacks_invokes_ctor),
        cmocka_unit_test(test_kill_invokes_dtor_once),
        cmocka_unit_test(test_init_and_create_work_struct_ex_fields),
        cmocka_unit_test(test_queue_work_and_run_pending),
        cmocka_unit_test(test_queue_work_failure_injection),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
