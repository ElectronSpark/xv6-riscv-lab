#ifndef __UT_SLAB_H__
#define __UT_SLAB_H__

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "cmocka.h"

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "slab.h"
#include "slab_private.h"
#include "ut_page.h"

#define SLAB_CACHE_COUNT 8

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

// Mock function declarations to prevent implicit function declaration warnings
void *__wrap_slab_alloc(slab_cache_t *cache);
void __wrap_slab_free(void *obj);
int __wrap_slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, uint64 flags);
slab_cache_t *__wrap_slab_cache_create(char *name, size_t obj_size, uint64 flags);
int __wrap_slab_cache_destroy(slab_cache_t *cache);
int __wrap_slab_cache_shrink(slab_cache_t *cache, int nums);

// Function declarations for real functions that are aliases to the wrappers
void *__real_slab_alloc(slab_cache_t *cache);
void __real_slab_free(void *obj);
int __real_slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, uint64 flags);
slab_cache_t *__real_slab_cache_create(char *name, size_t obj_size, uint64 flags);
int __real_slab_cache_destroy(slab_cache_t *cache);
int __real_slab_cache_shrink(slab_cache_t *cache, int nums);

// Passthrough flags for slab functions
extern bool __wrap_slab_alloc_passthrough;
extern bool __wrap_slab_free_passthrough;
extern bool __wrap_slab_cache_init_passthrough;
extern bool __wrap_slab_cache_create_passthrough;
extern bool __wrap_slab_cache_destroy_passthrough;
extern bool __wrap_slab_cache_shrink_passthrough;

// Functions to enable/disable slab wrapper passthroughs
void ut_slab_wrappers_enable_passthrough(void);
void ut_slab_wrappers_disable_passthrough(void);

// Functions for more granular control of slab wrapper passthroughs
void ut_slab_memory_enable_passthrough(void);
void ut_slab_memory_disable_passthrough(void);
void ut_slab_cache_enable_passthrough(void);
void ut_slab_cache_disable_passthrough(void);

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

#endif // __UT_SLAB_H__
