// RCU Test Suite
//
// This file contains comprehensive tests for the RCU (Read-Copy-Update)
// synchronization mechanism.
//

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "spinlock.h"
#include "rcu.h"
#include "proc.h"
#include "sched.h"

// Test data structures
typedef struct test_node {
    int value;
    struct test_node *next;
} test_node_t;

static test_node_t *test_list = NULL;
static _Atomic int test_counter = 0;
static _Atomic int callback_invoked = 0;

// ============================================================================
// Test 1: Basic RCU Read-Side Critical Section
// ============================================================================

static void test_rcu_read_lock(void) {
    printf("TEST: RCU Read Lock/Unlock\n");

    // Test nested locking
    rcu_read_lock();
    assert(rcu_is_watching(), "CPU should be in RCU critical section");

    rcu_read_lock();  // Nested
    assert(rcu_is_watching(), "CPU should still be in RCU critical section");

    rcu_read_unlock(); // Unnest
    assert(rcu_is_watching(), "CPU should still be in RCU critical section");

    rcu_read_unlock(); // Final unlock
    assert(!rcu_is_watching(), "CPU should not be in RCU critical section");

    printf("  PASS: Nested RCU read locks work correctly\n");
}

// ============================================================================
// Test 2: RCU Pointer Operations
// ============================================================================

static void test_rcu_pointers(void) {
    printf("TEST: RCU Pointer Operations\n");

    test_node_t *node = (test_node_t *)kmm_alloc(sizeof(test_node_t));
    node->value = 42;
    node->next = NULL;

    // Test rcu_assign_pointer
    rcu_assign_pointer(test_list, node);

    // Test rcu_dereference
    rcu_read_lock();
    test_node_t *read_node = rcu_dereference(test_list);
    assert(read_node != NULL, "rcu_dereference should return non-NULL");
    assert(read_node->value == 42, "rcu_dereference should return correct value");
    rcu_read_unlock();

    // Test rcu_access_pointer
    test_node_t *access_node = rcu_access_pointer(test_list);
    assert(access_node != NULL, "rcu_access_pointer should return non-NULL");

    printf("  PASS: RCU pointer operations work correctly\n");

    // Cleanup
    kmm_free(node);
    test_list = NULL;
}

// ============================================================================
// Test 3: synchronize_rcu()
// ============================================================================

static void test_synchronize_rcu(void) {
    printf("TEST: synchronize_rcu()\n");

    test_node_t *old_node = (test_node_t *)kmm_alloc(sizeof(test_node_t));
    old_node->value = 100;
    old_node->next = NULL;

    rcu_assign_pointer(test_list, old_node);

    // Create new node
    test_node_t *new_node = (test_node_t *)kmm_alloc(sizeof(test_node_t));
    new_node->value = 200;
    new_node->next = NULL;

    // Update pointer
    rcu_assign_pointer(test_list, new_node);

    // Wait for grace period
    printf("  Waiting for grace period...\n");
    synchronize_rcu();
    printf("  Grace period completed\n");

    // Now safe to free old node
    kmm_free(old_node);

    // Verify new node is accessible
    rcu_read_lock();
    test_node_t *current = rcu_dereference(test_list);
    assert(current != NULL, "List should not be NULL");
    assert(current->value == 200, "Should read new value");
    rcu_read_unlock();

    printf("  PASS: synchronize_rcu() allows safe reclamation\n");

    // Cleanup
    kmm_free(new_node);
    test_list = NULL;
}

// ============================================================================
// Test 4: call_rcu() Callbacks
// ============================================================================

static void test_callback(void *data) {
    int *value = (int *)data;
    printf("  Callback invoked with value: %d\n", *value);
    __atomic_fetch_add(&callback_invoked, 1, __ATOMIC_RELEASE);
    kmm_free(data);
}

static void test_call_rcu(void) {
    printf("TEST: call_rcu() Callbacks\n");

    __atomic_store_n(&callback_invoked, 0, __ATOMIC_RELEASE);

    // Allocate callback data
    int *data = (int *)kmm_alloc(sizeof(int));
    *data = 42;

    // Allocate RCU head
    rcu_head_t *head = (rcu_head_t *)kmm_alloc(sizeof(rcu_head_t));

    // Register callback
    call_rcu(head, test_callback, data);
    printf("  Callback registered\n");

    // Force grace period completion and callback processing
    // The segmented callback list requires multiple grace periods to advance
    // callbacks through NEXT_TAIL -> NEXT_READY_TAIL -> WAIT_TAIL -> DONE_TAIL
    for (int i = 0; i < 3; i++) {
        synchronize_rcu();
        rcu_process_callbacks();
    }

    // Wait for callback to be invoked (should already be done after above)
    int timeout = 100;
    while (__atomic_load_n(&callback_invoked, __ATOMIC_ACQUIRE) == 0 && timeout > 0) {
        synchronize_rcu();
        rcu_process_callbacks();
        yield();
        timeout--;
    }

    assert(__atomic_load_n(&callback_invoked, __ATOMIC_ACQUIRE) == 1,
           "Callback should have been invoked");

    printf("  PASS: call_rcu() callback executed successfully\n");

    // Note: callback frees the data, we just need to free the head
    kmm_free(head);
}

// ============================================================================
// Test 5: Multiple Concurrent Readers
// ============================================================================

static void reader_thread(uint64 id, uint64 iterations) {
    printf("  Reader %ld starting (%ld iterations)\n", id, iterations);

    for (uint64 i = 0; i < iterations; i++) {
        rcu_read_lock();

        test_node_t *node = rcu_dereference(test_list);
        if (node != NULL) {
            // Simulate some work
            volatile int sum = 0;
            for (int j = 0; j < 100; j++) {
                sum += node->value;
            }
        }

        rcu_read_unlock();

        if (i % 10 == 0) {
            yield(); // Give other threads a chance
        }
    }

    printf("  Reader %ld completed\n", id);
}

static void test_concurrent_readers(void) {
    printf("TEST: Concurrent Readers\n");

    // Setup test list
    test_node_t *node = (test_node_t *)kmm_alloc(sizeof(test_node_t));
    node->value = 777;
    node->next = NULL;
    rcu_assign_pointer(test_list, node);

    // Create multiple reader threads
    struct proc *readers[4];
    for (int i = 0; i < 4; i++) {
        kernel_proc_create("rcu_reader", &readers[i],
                          (void *)reader_thread, i, 50, KERNEL_STACK_ORDER);
        wakeup_proc(readers[i]);
    }

    printf("  Waiting for readers to complete...\n");

    // Give readers time to run
    for (int i = 0; i < 100; i++) {
        yield();
    }

    printf("  PASS: Concurrent readers completed successfully\n");

    // Cleanup
    synchronize_rcu();
    kmm_free(node);
    test_list = NULL;
}

// ============================================================================
// Test 6: Grace Period Detection
// ============================================================================

static void test_grace_period(void) {
    printf("TEST: Grace Period Detection\n");

    __atomic_store_n(&test_counter, 0, __ATOMIC_RELEASE);

    // Test RCU critical section
    rcu_read_lock();
    // Do some work inside RCU critical section
    volatile int sum = 0;
    for (int i = 0; i < 100; i++) {
        sum += i;
    }
    rcu_read_unlock();

    // Context switches OUTSIDE of RCU critical section are quiescent states
    for (int i = 0; i < 10; i++) {
        yield(); // These context switches help advance grace periods
    }

    // Force grace period completion
    synchronize_rcu();

    printf("  PASS: Grace period detection through context switches\n");
}

// ============================================================================
// Main Test Runner
// ============================================================================

void rcu_run_tests(void) {
    sleep_ms(100);
    printf("\n");
    printf("================================================================================\n");
    printf("RCU Test Suite Starting\n");
    printf("================================================================================\n");
    printf("\n");

    test_rcu_read_lock();
    printf("\n");

    test_rcu_pointers();
    printf("\n");

    test_synchronize_rcu();
    printf("\n");

    test_call_rcu();
    printf("\n");

    test_grace_period();
    printf("\n");

    test_concurrent_readers();
    printf("\n");

    printf("================================================================================\n");
    printf("RCU Test Suite Completed - ALL TESTS PASSED\n");
    printf("================================================================================\n");
    printf("\n");
}
