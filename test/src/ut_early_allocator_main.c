/*
 * Unit tests for early_allocator.c
 * Tests buddy-style allocation, chunk splitting, and alignment handling
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <cmocka.h>

#define ON_HOST_OS 1
#define HOST_TEST 1

// Block problematic kernel headers before including early_allocator.c
#define __KERNEL_PRINTF_H  // Block kernel printf.h
#define __KERNEL_DEFS_H    // Block kernel defs.h

// Mock types needed by early_allocator.c
typedef uint64_t uint64;
typedef unsigned long size_t;

// Mock RISC-V page size
#define PGSIZE 4096
#define PAGE_SIZE PGSIZE

// Mock panic function
void panic(const char *s) {
    fprintf(stderr, "PANIC: %s\n", s);
    fail_msg("Panic called: %s", s);
}

// Mock assert - redefine to use cmocka's assert
#undef assert
#define assert(condition, ...) assert_true(condition)

// Include necessary kernel headers for list operations
#include "list_type.h"
#include "list.h"

// Include the early allocator implementation
#include "../../kernel/mm/early_allocator.c"

// Test memory pool
#define TEST_MEMORY_SIZE (1024 * 1024)  // 1MB
static char test_memory[TEST_MEMORY_SIZE];

/*
 * Setup function - initialize the allocator with test memory
 */
static int setup_allocator(void **state) {
    early_allocator_init(test_memory, test_memory + TEST_MEMORY_SIZE);
    *state = test_memory;
    return 0;
}

/*
 * Teardown function - reset allocator state
 */
static int teardown_allocator(void **state) {
    memset(test_memory, 0, TEST_MEMORY_SIZE);
    memset(&earalloc_params, 0, sizeof(earalloc_params));
    return 0;
}

/*
 * Test: Basic initialization
 */
static void test_init(void **state) {
    (void)state;
    
    // Initialize allocator
    early_allocator_init(test_memory, test_memory + TEST_MEMORY_SIZE);
    
    // Check that current pointer is set and aligned
    assert_non_null(earalloc_params.current);
    assert_true((uint64)earalloc_params.current >= (uint64)test_memory);
    assert_true((uint64)earalloc_params.current < (uint64)(test_memory + TEST_MEMORY_SIZE));
    assert_int_equal((uint64)earalloc_params.current & (EARLYALLOC_SMALLEST_CHUNK - 1), 0);
    
    // Check that end pointer is set
    assert_ptr_equal(earalloc_params.end, test_memory + TEST_MEMORY_SIZE);
    
    // Check that free lists are initialized
    for (int i = 0; i < EARLYALLOC_ORDERS; i++) {
        assert_true(LIST_IS_EMPTY(&earalloc_params.free_lists[i]));
    }
}

/*
 * Test: Small allocation from advancing pointer
 */
static void test_small_alloc_basic(void **state) {
    void *ptr1, *ptr2, *ptr3;
    
    // Allocate 64 bytes (will be rounded to 64)
    ptr1 = early_alloc(64);
    assert_non_null(ptr1);
    assert_int_equal((uint64)ptr1 & 63, 0);  // Should be 64-byte aligned
    
    // Allocate 128 bytes (will be rounded to 128)
    ptr2 = early_alloc(128);
    assert_non_null(ptr2);
    assert_int_equal((uint64)ptr2 & 127, 0);  // Should be 128-byte aligned
    
    // Allocate 32 bytes (minimum)
    ptr3 = early_alloc(32);
    assert_non_null(ptr3);
    assert_int_equal((uint64)ptr3 & 31, 0);  // Should be 32-byte aligned
    
    // Verify allocations don't overlap
    assert_true((uint64)ptr1 + 64 <= (uint64)ptr2 || (uint64)ptr2 + 128 <= (uint64)ptr1);
    assert_true((uint64)ptr1 + 64 <= (uint64)ptr3 || (uint64)ptr3 + 32 <= (uint64)ptr1);
    assert_true((uint64)ptr2 + 128 <= (uint64)ptr3 || (uint64)ptr3 + 32 <= (uint64)ptr2);
}

/*
 * Test: Chunk splitting from free lists
 */
static void test_chunk_splitting(void **state) {
    void *ptr1, *ptr2, *ptr3;
    int initial_chunks = 0;
    
    // Count initial chunks in free lists
    for (int i = 0; i < EARLYALLOC_ORDERS; i++) {
        list_node_t *entry;
        list_foreach_entry(&earalloc_params.free_lists[i], entry) {
            initial_chunks++;
        }
    }
    
    // First allocate 256 bytes
    ptr1 = early_alloc(256);
    assert_non_null(ptr1);
    
    // Create a gap that will be freed as chunks
    uint64 gap_start = EARLYALLOC_ALIGN((uint64)earalloc_params.current, 64);
    uint64 gap_end = gap_start + 2048;  // Larger gap
    
    // Free the gap to create chunks in free lists
    __free_region_to_chunks(gap_start, gap_end);
    
    // Count chunks after freeing - should have more
    int after_free_chunks = 0;
    for (int i = 0; i < EARLYALLOC_ORDERS; i++) {
        list_node_t *entry;
        list_foreach_entry(&earalloc_params.free_lists[i], entry) {
            after_free_chunks++;
        }
    }
    
    // Should have created some chunks
    assert_true(after_free_chunks > initial_chunks);
    
    // Now allocate 64 bytes - should come from free lists
    ptr2 = early_alloc(64);
    assert_non_null(ptr2);
    
    // Allocate another 64 bytes
    ptr3 = early_alloc(64);
    assert_non_null(ptr3);
    
    // Should not overlap
    assert_true((uint64)ptr2 + 64 <= (uint64)ptr3 || (uint64)ptr3 + 64 <= (uint64)ptr2);
}

/*
 * Test: Large allocation with custom alignment
 */
static void test_large_alloc_alignment(void **state) {
    void *ptr1, *ptr2;
    
    // Allocate large object (128KB > 64KB max chunk)
    ptr1 = early_alloc_align(128 * 1024, PAGE_SIZE);
    assert_non_null(ptr1);
    assert_int_equal((uint64)ptr1 & (PAGE_SIZE - 1), 0);  // Page aligned
    
    // Allocate another with different alignment
    ptr2 = early_alloc_align(256 * 1024, 8192);
    assert_non_null(ptr2);
    assert_int_equal((uint64)ptr2 & (8192 - 1), 0);  // 8KB aligned
    
    // Should not overlap
    assert_true((uint64)ptr1 + 128 * 1024 <= (uint64)ptr2);
}

/*
 * Test: Small allocations ignore user alignment
 */
static void test_small_alloc_ignores_user_alignment(void **state) {
    void *ptr;
    
    // Request 128 bytes with page alignment (should be ignored)
    ptr = early_alloc_align(128, PAGE_SIZE);
    assert_non_null(ptr);
    
    // Should be aligned to 128 (chunk size), not PAGE_SIZE
    assert_int_equal((uint64)ptr & 127, 0);
    
    // May or may not be page aligned (depends on where current pointer is)
    // But the size allocated is 128, not more
}

/*
 * Test: Alignment gap recycling
 */
static void test_alignment_gap_recycling(void **state) {
    void *ptr1, *ptr2;
    
    // Allocate small amount to move current pointer to unaligned position
    ptr1 = early_alloc(100);
    assert_non_null(ptr1);
    
    // Now allocate large object with page alignment
    // This should create gaps that get recycled
    ptr2 = early_alloc_align(128 * 1024, PAGE_SIZE);
    assert_non_null(ptr2);
    assert_int_equal((uint64)ptr2 & (PAGE_SIZE - 1), 0);
    
    // Check that gap was recycled to free lists
    int chunks_found = 0;
    for (int i = 0; i < EARLYALLOC_ORDERS; i++) {
        if (!LIST_IS_EMPTY(&earalloc_params.free_lists[i])) {
            chunks_found++;
        }
    }
    
    // Should have at least some chunks from the gap
    assert_true(chunks_found > 0);
}

/*
 * Test: End pointer tracking
 */
static void test_end_ptr_tracking(void **state) {
    void *start_ptr, *end_ptr;
    void *alloc1, *alloc2;
    
    start_ptr = early_alloc_end_ptr();
    assert_non_null(start_ptr);
    
    // Make some allocations
    alloc1 = early_alloc(1024);
    assert_non_null(alloc1);
    
    alloc2 = early_alloc(2048);
    assert_non_null(alloc2);
    
    // End pointer should have advanced
    end_ptr = early_alloc_end_ptr();
    assert_true((uint64)end_ptr > (uint64)start_ptr);
    
    // End pointer should be past both allocations
    assert_true((uint64)end_ptr >= (uint64)alloc1 + 1024);
    assert_true((uint64)end_ptr >= (uint64)alloc2 + 2048);
}

/*
 * Test: Multiple small allocations from free lists
 */
static void test_multiple_small_from_freelist(void **state) {
    void *ptrs[10];
    
    // Create initial gap with chunks
    void *old_current = earalloc_params.current;
    earalloc_params.current = (void *)((uint64)old_current + 4096);
    __free_region_to_chunks((uint64)old_current, (uint64)earalloc_params.current);
    
    // Allocate multiple 64-byte blocks
    for (int i = 0; i < 10; i++) {
        ptrs[i] = early_alloc(64);
        assert_non_null(ptrs[i]);
        assert_int_equal((uint64)ptrs[i] & 63, 0);
    }
    
    // Verify all are unique
    for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j < 10; j++) {
            assert_ptr_not_equal(ptrs[i], ptrs[j]);
        }
    }
}

/*
 * Test: Size rounding to power of 2
 */
static void test_size_rounding(void **state) {
    void *ptr1, *ptr2, *ptr3;
    
    // Allocate 100 bytes (should round to 128)
    ptr1 = early_alloc(100);
    assert_non_null(ptr1);
    assert_int_equal((uint64)ptr1 & 127, 0);  // 128-byte aligned
    
    // Allocate 200 bytes (should round to 256)
    ptr2 = early_alloc(200);
    assert_non_null(ptr2);
    assert_int_equal((uint64)ptr2 & 255, 0);  // 256-byte aligned
    
    // Allocate 10 bytes (should round to 32, minimum)
    ptr3 = early_alloc(10);
    assert_non_null(ptr3);
    assert_int_equal((uint64)ptr3 & 31, 0);  // 32-byte aligned
}

/*
 * Test: Zero size allocation
 */
static void test_zero_size(void **state) {
    void *ptr;
    
    ptr = early_alloc(0);
    assert_null(ptr);
}

/*
 * Test: Chunk magic numbers
 */
static void test_chunk_magic(void **state) {
    // Create a gap and free it as chunks
    void *old_current = earalloc_params.current;
    earalloc_params.current = (void *)((uint64)old_current + 1024);
    __free_region_to_chunks((uint64)old_current, (uint64)earalloc_params.current);
    
    // Check that chunks have proper magic numbers
    for (int i = 0; i < EARLYALLOC_ORDERS; i++) {
        if (!LIST_IS_EMPTY(&earalloc_params.free_lists[i])) {
            list_node_t *entry = LIST_FIRST_ENTRY(&earalloc_params.free_lists[i]);
            struct earalloc_chunk *chunk = container_of(entry, struct earalloc_chunk, list_entry);
            assert_int_equal(chunk->magic, EARLYALLOC_CHUNK_MAGIC);
        }
    }
}

/*
 * Test: Stress test - many allocations
 */
static void test_stress_many_allocations(void **state) {
    void *ptrs[100];
    int sizes[] = {32, 64, 128, 256, 512, 1024, 2048};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    
    // Allocate many blocks of various sizes
    for (int i = 0; i < 100; i++) {
        int size = sizes[i % num_sizes];
        ptrs[i] = early_alloc(size);
        assert_non_null(ptrs[i]);
        
        // Verify alignment
        int order = __size_to_order(size);
        size_t actual_size = 1UL << order;
        assert_int_equal((uint64)ptrs[i] & (actual_size - 1), 0);
    }
    
    // Verify current pointer hasn't gone past end
    assert_true((uint64)earalloc_params.current <= (uint64)earalloc_params.end);
}

/*
 * Test: Alignment to chunk size for small objects
 */
static void test_chunk_alignment_verification(void **state) {
    // Create chunks and verify they're aligned to their sizes
    void *old_current = earalloc_params.current;
    earalloc_params.current = (void *)((uint64)old_current + 8192);
    __free_region_to_chunks((uint64)old_current, (uint64)earalloc_params.current);
    
    // Check all chunks in free lists are properly aligned
    for (int i = 0; i < EARLYALLOC_ORDERS; i++) {
        list_node_t *entry;
        list_foreach_entry(&earalloc_params.free_lists[i], entry) {
            struct earalloc_chunk *chunk = container_of(entry, struct earalloc_chunk, list_entry);
            
            // Verify alignment
            assert_int_equal((uint64)chunk & (chunk->size - 1), 0);
            
            // Verify size is power of 2
            assert_int_equal(chunk->size & (chunk->size - 1), 0);
        }
    }
}

/*
 * Main test runner
 */
int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_init),
        cmocka_unit_test_setup_teardown(test_small_alloc_basic, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_chunk_splitting, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_large_alloc_alignment, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_small_alloc_ignores_user_alignment, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_alignment_gap_recycling, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_end_ptr_tracking, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_multiple_small_from_freelist, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_size_rounding, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_zero_size, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_chunk_magic, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_stress_many_allocations, setup_allocator, teardown_allocator),
        cmocka_unit_test_setup_teardown(test_chunk_alignment_verification, setup_allocator, teardown_allocator),
    };
    
    return cmocka_run_group_tests(tests, NULL, NULL);
}
