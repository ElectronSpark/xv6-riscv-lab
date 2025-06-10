#include "ut_slab.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

// Only declare passthrough flags for slab functions
// The page allocator flags are already defined in ut_page_wraps.c
bool __wrap_slab_alloc_passthrough = false;
bool __wrap_slab_free_passthrough = false;
bool __wrap_slab_cache_init_passthrough = false;
bool __wrap_slab_cache_create_passthrough = false;
bool __wrap_slab_cache_destroy_passthrough = false;
bool __wrap_slab_cache_shrink_passthrough = false;

// Implement the slab wrapper functions
void *__wrap_slab_alloc(slab_cache_t *cache) {
    if (__wrap_slab_alloc_passthrough) {
        return __real_slab_alloc(cache);
    }
    return mock_ptr_type(void *);
}

void __wrap_slab_free(void *obj) {
    if (__wrap_slab_free_passthrough) {
        __real_slab_free(obj);
    } else {
        check_expected_ptr(obj);
        // We don't need to return a value for void functions
        function_called();
    }
}

int __wrap_slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, uint64 flags) {
    if (__wrap_slab_cache_init_passthrough) {
        return __real_slab_cache_init(cache, name, obj_size, flags);
    }
    check_expected_ptr(cache);
    check_expected_ptr(name);
    check_expected(obj_size);
    check_expected(flags);
    return mock_type(int);
}

slab_cache_t *__wrap_slab_cache_create(char *name, size_t obj_size, uint64 flags) {
    if (__wrap_slab_cache_create_passthrough) {
        return __real_slab_cache_create(name, obj_size, flags);
    }
    check_expected_ptr(name);
    check_expected(obj_size);
    check_expected(flags);
    return mock_ptr_type(slab_cache_t *);
}

int __wrap_slab_cache_destroy(slab_cache_t *cache) {
    if (__wrap_slab_cache_destroy_passthrough) {
        return __real_slab_cache_destroy(cache);
    }
    check_expected_ptr(cache);
    return mock_type(int);
}

int __wrap_slab_cache_shrink(slab_cache_t *cache, int nums) {
    if (__wrap_slab_cache_shrink_passthrough) {
        return __real_slab_cache_shrink(cache, nums);
    }
    check_expected_ptr(cache);
    check_expected(nums);
    return mock_type(int);
}

/**
 * Enable passthrough for all slab wrapper functions
 * This makes all slab wrapper functions call their real counterparts
 */
void ut_slab_wrappers_enable_passthrough(void) {
    __wrap_slab_alloc_passthrough = true;
    __wrap_slab_free_passthrough = true;
    __wrap_slab_cache_init_passthrough = true;
    __wrap_slab_cache_create_passthrough = true;
    __wrap_slab_cache_destroy_passthrough = true;
    __wrap_slab_cache_shrink_passthrough = true;
}

/**
 * Disable passthrough for all slab wrapper functions
 * This makes all slab wrapper functions use mocks
 */
void ut_slab_wrappers_disable_passthrough(void) {
    __wrap_slab_alloc_passthrough = false;
    __wrap_slab_free_passthrough = false;
    __wrap_slab_cache_init_passthrough = false;
    __wrap_slab_cache_create_passthrough = false;
    __wrap_slab_cache_destroy_passthrough = false;
    __wrap_slab_cache_shrink_passthrough = false;
}

/**
 * Enable passthrough for slab memory operations
 * This enables slab_alloc and slab_free
 */
void ut_slab_memory_enable_passthrough(void) {
    __wrap_slab_alloc_passthrough = true;
    __wrap_slab_free_passthrough = true;
}

/**
 * Disable passthrough for slab memory operations
 */
void ut_slab_memory_disable_passthrough(void) {
    __wrap_slab_alloc_passthrough = false;
    __wrap_slab_free_passthrough = false;
}

/**
 * Enable passthrough for slab cache management functions
 * This enables slab_cache_init, slab_cache_create, slab_cache_destroy, and slab_cache_shrink
 */
void ut_slab_cache_enable_passthrough(void) {
    __wrap_slab_cache_init_passthrough = true;
    __wrap_slab_cache_create_passthrough = true;
    __wrap_slab_cache_destroy_passthrough = true;
    __wrap_slab_cache_shrink_passthrough = true;
}

/**
 * Disable passthrough for slab cache management functions
 */
void ut_slab_cache_disable_passthrough(void) {
    __wrap_slab_cache_init_passthrough = false;
    __wrap_slab_cache_create_passthrough = false;
    __wrap_slab_cache_destroy_passthrough = false;
    __wrap_slab_cache_shrink_passthrough = false;
}