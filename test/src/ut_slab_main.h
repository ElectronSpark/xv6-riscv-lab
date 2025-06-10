#ifndef __UT_SLAB_MAIN_H__
#define __UT_SLAB_MAIN_H__

#include <stdint.h>
#include <stdbool.h>
#include "slab.h"
#include "slab_private.h"
#include "ut_slab_wraps.h"

// Structure to store slab allocator state
typedef struct {
    uint64 slab_counts[SLAB_CACHE_COUNT]; // Count of slabs in each cache
    uint64 obj_active[SLAB_CACHE_COUNT];  // Count of active objects in each cache
    uint64 obj_total[SLAB_CACHE_COUNT];   // Total objects capacity in each cache
    bool skip;                          // Skip state validation if true
} slab_state_t;

// Test setup and teardown functions
int test_slab_setup(void **state);
int test_slab_teardown(void **state);

// Test functions
void test_print_slab_cache_stat(void **state);
void test_slab_cache_create_destroy(void **state);
void test_slab_alloc_free(void **state);
void test_slab_sizes_and_flags(void **state);
void test_slab_cache_shrink(void **state);
void test_multiple_slab_caches(void **state);
void test_slab_alloc_free_pattern(void **state);
void test_slab_edge_cases(void **state);
void test_slab_large_objects(void **state);
void test_slab_stress(void **state);
void test_slab_passthrough_demonstration(void **state);

#endif // __UT_SLAB_MAIN_H__