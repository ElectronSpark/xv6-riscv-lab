// Device Table Stress Tests
//
// This file contains stress tests for the RCU-protected device table.
// Tests verify concurrent access, registration/unregistration, and RCU
// grace period handling.
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
#include "timer.h"
#include "dev.h"
#include "errno.h"
#include "page.h"

// Test configuration
#define TEST_MAJOR_BASE     100     // Starting major number for tests
#define TEST_ITERATIONS     50      // Number of iterations for stress tests
#define NUM_READER_THREADS  4       // Number of concurrent reader threads
#define NUM_WRITER_THREADS  2       // Number of concurrent writer threads

// Test statistics
static _Atomic int test_reads_completed = 0;
static _Atomic int test_writes_completed = 0;
static _Atomic int test_errors = 0;
static _Atomic int readers_running = 0;
static _Atomic int writers_running = 0;
static _Atomic int test_stop_flag = 0;

// Test device storage - statically allocated to avoid allocation issues
#define MAX_TEST_DEVICES 16
static device_t test_devices[MAX_TEST_DEVICES];
static _Atomic int test_device_registered[MAX_TEST_DEVICES] = {0};

// ============================================================================
// Test Device Callbacks
// ============================================================================

static int test_dev_open(device_t *dev) {
    (void)dev;
    return 0;
}

static int test_dev_release(device_t *dev) {
    (void)dev;
    return 0;
}

// Initialize a test device
static void init_test_device(device_t *dev, int major, int minor) {
    dev->major = major;
    dev->minor = minor;
    dev->type = DEV_TYPE_CHAR;
    dev->ops.open = test_dev_open;
    dev->ops.release = test_dev_release;
}

// ============================================================================
// Test 1: Basic Registration and Lookup
// ============================================================================

static void test_basic_registration(void) {
    printf("TEST: Basic Device Registration and Lookup\n");
    
    device_t *dev = &test_devices[0];
    init_test_device(dev, TEST_MAJOR_BASE, 1);
    
    // Register the device
    int ret = device_register(dev);
    assert(ret == 0, "device_register should succeed");
    
    // Look up the device
    device_t *found = device_get(TEST_MAJOR_BASE, 1);
    assert(!IS_ERR(found), "device_get should succeed");
    assert(found == dev, "device_get should return the registered device");
    
    // Release the reference
    device_put(found);
    
    // Unregister the device
    ret = device_unregister(dev);
    assert(ret == 0, "device_unregister should succeed");
    
    // Wait for RCU grace period
    synchronize_rcu();
    
    // Device should no longer be found
    found = device_get(TEST_MAJOR_BASE, 1);
    assert(IS_ERR(found), "device_get should fail after unregister");
    
    printf("  PASS: Basic registration and lookup works correctly\n");
}

// ============================================================================
// Test 2: Concurrent Readers
// ============================================================================

static device_t *reader_test_device = NULL;

static void reader_thread_fn(uint64 id, uint64 iterations) {
    __atomic_fetch_add(&readers_running, 1, __ATOMIC_SEQ_CST);
    printf("  Reader %d starting (%d iterations)\n", (int)id, (int)iterations);
    
    for (uint64 i = 0; i < iterations && !__atomic_load_n(&test_stop_flag, __ATOMIC_SEQ_CST); i++) {
        // Look up the device using RCU-protected path
        device_t *dev = device_get(TEST_MAJOR_BASE + 1, 1);
        if (!IS_ERR(dev)) {
            // Successfully got a reference - verify and release
            if (dev != reader_test_device) {
                __atomic_fetch_add(&test_errors, 1, __ATOMIC_SEQ_CST);
            }
            device_put(dev);
            __atomic_fetch_add(&test_reads_completed, 1, __ATOMIC_SEQ_CST);
        }
        
        // Small delay to allow other threads to run
        if (i % 10 == 0) {
            yield();
        }
    }
    
    printf("  Reader %d completed\n", (int)id);
    __atomic_fetch_sub(&readers_running, 1, __ATOMIC_SEQ_CST);
}

static void test_concurrent_readers(void) {
    printf("TEST: Concurrent Readers\n");
    
    // Reset counters
    __atomic_store_n(&test_reads_completed, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&test_errors, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&readers_running, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&test_stop_flag, 0, __ATOMIC_SEQ_CST);
    
    // Register a device for readers to look up
    reader_test_device = &test_devices[1];
    init_test_device(reader_test_device, TEST_MAJOR_BASE + 1, 1);
    int ret = device_register(reader_test_device);
    assert(ret == 0, "device_register should succeed");
    
    // Start reader threads
    struct proc *readers[NUM_READER_THREADS];
    for (int i = 0; i < NUM_READER_THREADS; i++) {
        ret = kernel_proc_create("dev_reader", &readers[i], 
                                  (int (*)(uint64, uint64))reader_thread_fn, 
                                  i, TEST_ITERATIONS, KERNEL_STACK_ORDER);
        assert(ret > 0, "Failed to create reader thread");
        wakeup_proc(readers[i]);
    }
    
    // Wait for readers to complete
    printf("  Waiting for readers to complete...\n");
    int wait_count = 0;
    while (__atomic_load_n(&readers_running, __ATOMIC_SEQ_CST) > 0 && wait_count < 10000) {
        yield();
        wait_count++;
    }
    
    // Stop any remaining readers
    __atomic_store_n(&test_stop_flag, 1, __ATOMIC_SEQ_CST);
    yield();
    
    // Cleanup
    device_unregister(reader_test_device);
    synchronize_rcu();
    reader_test_device = NULL;
    
    int reads = __atomic_load_n(&test_reads_completed, __ATOMIC_SEQ_CST);
    int errors = __atomic_load_n(&test_errors, __ATOMIC_SEQ_CST);
    
    printf("  Completed %d reads with %d errors\n", reads, errors);
    assert(errors == 0, "No errors should occur during concurrent reads");
    assert(reads > 0, "Some reads should have completed");
    
    printf("  PASS: Concurrent readers completed successfully\n");
}

// ============================================================================
// Test 3: Registration/Unregistration Stress
// ============================================================================

static void test_register_unregister_stress(void) {
    printf("TEST: Registration/Unregistration Stress\n");
    
    int success_count = 0;
    int fail_count = 0;
    
    for (int iter = 0; iter < TEST_ITERATIONS; iter++) {
        int dev_idx = 2 + (iter % 4);  // Use devices 2-5
        device_t *dev = &test_devices[dev_idx];
        
        // Check if already registered
        if (__atomic_load_n(&test_device_registered[dev_idx], __ATOMIC_SEQ_CST)) {
            // Unregister it
            int ret = device_unregister(dev);
            if (ret == 0) {
                __atomic_store_n(&test_device_registered[dev_idx], 0, __ATOMIC_SEQ_CST);
                success_count++;
            }
            // Wait for grace period before re-registering
            synchronize_rcu();
        } else {
            // Register it
            init_test_device(dev, TEST_MAJOR_BASE + 2, dev_idx);
            int ret = device_register(dev);
            if (ret == 0) {
                __atomic_store_n(&test_device_registered[dev_idx], 1, __ATOMIC_SEQ_CST);
                success_count++;
            } else if (ret == -EBUSY) {
                // Device already registered - this is okay in stress test
                fail_count++;
            } else {
                printf("  Unexpected error: %d\n", ret);
                fail_count++;
            }
        }
        
        if (iter % 10 == 0) {
            yield();
        }
    }
    
    // Cleanup: unregister all test devices
    for (int i = 2; i < 6; i++) {
        if (__atomic_load_n(&test_device_registered[i], __ATOMIC_SEQ_CST)) {
            device_unregister(&test_devices[i]);
            __atomic_store_n(&test_device_registered[i], 0, __ATOMIC_SEQ_CST);
        }
    }
    synchronize_rcu();
    
    printf("  Completed %d successful operations, %d expected failures\n", success_count, fail_count);
    assert(success_count > 0, "Some operations should have succeeded");
    
    printf("  PASS: Registration/unregistration stress completed\n");
}

// ============================================================================
// Test 4: Concurrent Readers and Writers
// ============================================================================

static _Atomic int rw_test_major = 0;

static void rw_reader_thread_fn(uint64 id, uint64 iterations) {
    __atomic_fetch_add(&readers_running, 1, __ATOMIC_SEQ_CST);
    
    for (uint64 i = 0; i < iterations && !__atomic_load_n(&test_stop_flag, __ATOMIC_SEQ_CST); i++) {
        int major = __atomic_load_n(&rw_test_major, __ATOMIC_SEQ_CST);
        if (major > 0) {
            device_t *dev = device_get(major, 1);
            if (!IS_ERR(dev)) {
                device_put(dev);
                __atomic_fetch_add(&test_reads_completed, 1, __ATOMIC_SEQ_CST);
            }
        }
        
        if (i % 5 == 0) {
            yield();
        }
    }
    
    __atomic_fetch_sub(&readers_running, 1, __ATOMIC_SEQ_CST);
}

static void rw_writer_thread_fn(uint64 id, uint64 iterations) {
    __atomic_fetch_add(&writers_running, 1, __ATOMIC_SEQ_CST);
    
    int dev_idx = 6 + (int)id;  // Use devices 6-7
    device_t *dev = &test_devices[dev_idx];
    int my_major = TEST_MAJOR_BASE + 10 + (int)id;
    
    for (uint64 i = 0; i < iterations && !__atomic_load_n(&test_stop_flag, __ATOMIC_SEQ_CST); i++) {
        // Register
        init_test_device(dev, my_major, 1);
        int ret = device_register(dev);
        if (ret == 0) {
            __atomic_store_n(&rw_test_major, my_major, __ATOMIC_SEQ_CST);
            __atomic_fetch_add(&test_writes_completed, 1, __ATOMIC_SEQ_CST);
            
            // Let readers access it
            for (int j = 0; j < 5; j++) {
                yield();
            }
            
            // Unregister
            __atomic_store_n(&rw_test_major, 0, __ATOMIC_SEQ_CST);
            device_unregister(dev);
            synchronize_rcu();
            __atomic_fetch_add(&test_writes_completed, 1, __ATOMIC_SEQ_CST);
        }
        
        yield();
    }
    
    __atomic_fetch_sub(&writers_running, 1, __ATOMIC_SEQ_CST);
}

static void test_concurrent_readers_writers(void) {
    printf("TEST: Concurrent Readers and Writers\n");
    
    // Reset counters
    __atomic_store_n(&test_reads_completed, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&test_writes_completed, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&test_errors, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&readers_running, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&writers_running, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&test_stop_flag, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rw_test_major, 0, __ATOMIC_SEQ_CST);
    
    // Start reader threads
    struct proc *readers[NUM_READER_THREADS];
    for (int i = 0; i < NUM_READER_THREADS; i++) {
        int ret = kernel_proc_create("dev_rw_reader", &readers[i], 
                                      (int (*)(uint64, uint64))rw_reader_thread_fn, 
                                      i, TEST_ITERATIONS * 2, KERNEL_STACK_ORDER);
        assert(ret > 0, "Failed to create reader thread");
        wakeup_proc(readers[i]);
    }
    
    // Start writer threads
    struct proc *writers[NUM_WRITER_THREADS];
    for (int i = 0; i < NUM_WRITER_THREADS; i++) {
        int ret = kernel_proc_create("dev_rw_writer", &writers[i], 
                                      (int (*)(uint64, uint64))rw_writer_thread_fn, 
                                      i, TEST_ITERATIONS / 2, KERNEL_STACK_ORDER);
        assert(ret > 0, "Failed to create writer thread");
        wakeup_proc(writers[i]);
    }
    
    // Wait for completion
    printf("  Waiting for readers and writers to complete...\n");
    int wait_count = 0;
    while ((__atomic_load_n(&readers_running, __ATOMIC_SEQ_CST) > 0 ||
            __atomic_load_n(&writers_running, __ATOMIC_SEQ_CST) > 0) && 
           wait_count < 20000) {
        yield();
        wait_count++;
    }
    
    // Stop any remaining threads
    __atomic_store_n(&test_stop_flag, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 10; i++) {
        yield();
    }
    
    int reads = __atomic_load_n(&test_reads_completed, __ATOMIC_SEQ_CST);
    int writes = __atomic_load_n(&test_writes_completed, __ATOMIC_SEQ_CST);
    
    printf("  Completed %d reads and %d writes\n", reads, writes);
    assert(writes > 0, "Some writes should have completed");
    
    printf("  PASS: Concurrent readers and writers completed successfully\n");
}

// ============================================================================
// Test 5: RCU Grace Period with Device Unregistration
// ============================================================================

static _Atomic int gp_test_device_freed = 0;

static void test_rcu_grace_period(void) {
    printf("TEST: RCU Grace Period with Device Unregistration\n");
    
    __atomic_store_n(&gp_test_device_freed, 0, __ATOMIC_SEQ_CST);
    
    // Register a device
    device_t *dev = &test_devices[8];
    init_test_device(dev, TEST_MAJOR_BASE + 20, 1);
    int ret = device_register(dev);
    assert(ret == 0, "device_register should succeed");
    
    // Get a reference while in RCU critical section
    rcu_read_lock();
    device_t *found = device_get(TEST_MAJOR_BASE + 20, 1);
    assert(!IS_ERR(found), "device_get should succeed");
    
    // Unregister the device (but we still hold a reference)
    device_unregister(dev);
    
    // Device should still be accessible via our reference
    assert(found->major == TEST_MAJOR_BASE + 20, "Device should still be valid");
    
    rcu_read_unlock();
    
    // Release our reference
    device_put(found);
    
    // Wait for grace period
    synchronize_rcu();
    
    // Now device should be fully cleaned up
    found = device_get(TEST_MAJOR_BASE + 20, 1);
    assert(IS_ERR(found), "device_get should fail after unregister and grace period");
    
    printf("  PASS: RCU grace period correctly protects device access\n");
}

// ============================================================================
// Test 6: Rapid Registration/Unregistration (Same Slot)
// ============================================================================

static void test_rapid_reuse(void) {
    printf("TEST: Rapid Registration/Unregistration (Same Slot)\n");
    
    int success_count = 0;
    device_t *dev = &test_devices[9];
    
    for (int iter = 0; iter < TEST_ITERATIONS / 2; iter++) {
        // Register
        init_test_device(dev, TEST_MAJOR_BASE + 30, 1);
        int ret = device_register(dev);
        if (ret == 0) {
            success_count++;
            
            // Verify lookup
            device_t *found = device_get(TEST_MAJOR_BASE + 30, 1);
            if (!IS_ERR(found)) {
                device_put(found);
            }
            
            // Unregister
            device_unregister(dev);
            
            // Wait for grace period before re-registering
            synchronize_rcu();
            success_count++;
        }
        
        yield();
    }
    
    printf("  Completed %d successful operations\n", success_count);
    assert(success_count >= TEST_ITERATIONS / 2, "Most operations should succeed");
    
    printf("  PASS: Rapid reuse of device slots works correctly\n");
}

// ============================================================================
// Main Test Entry Point
// ============================================================================

void dev_table_test(void) {
    printf("\n");
    printf("================================================================================\n");
    printf("Device Table Stress Test Suite Starting\n");
    printf("================================================================================\n");
    printf("  Configuration:\n");
    printf("    - Reader threads: %d\n", NUM_READER_THREADS);
    printf("    - Writer threads: %d\n", NUM_WRITER_THREADS);
    printf("    - Iterations per test: %d\n", TEST_ITERATIONS);
    printf("================================================================================\n");
    printf("\n");
    
    // Initialize test devices array
    for (int i = 0; i < MAX_TEST_DEVICES; i++) {
        __atomic_store_n(&test_device_registered[i], 0, __ATOMIC_SEQ_CST);
    }
    
    // Run tests
    test_basic_registration();
    printf("\n");
    
    test_concurrent_readers();
    printf("\n");
    
    test_register_unregister_stress();
    printf("\n");
    
    test_concurrent_readers_writers();
    printf("\n");
    
    test_rcu_grace_period();
    printf("\n");
    
    test_rapid_reuse();
    printf("\n");
    
    printf("================================================================================\n");
    printf("Device Table Stress Test Suite Completed - ALL TESTS PASSED\n");
    printf("================================================================================\n");
    printf("\n");
}
