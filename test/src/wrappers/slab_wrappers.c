/*
 * Slab allocator wrappers for unit tests
 * Provides mock slab allocation
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include <mm/slab.h>

// Global flags for simulating failures
static bool g_test_fail_slab_alloc = false;

void pcache_test_fail_next_slab_alloc(void)
{
    g_test_fail_slab_alloc = true;
}

int __wrap_slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, uint64 flags)
{
    if (cache == NULL) {
        return -1;
    }
    
    cache->name = name;
    cache->obj_size = obj_size;
    cache->flags = flags;
    // Note: The actual slab_cache_struct doesn't have these fields in all versions,
    // so we just initialize what we can
    
    return 0;
}

slab_cache_t *__wrap_slab_cache_create(char *name, size_t obj_size, uint64 flags)
{
    slab_cache_t *cache = calloc(1, sizeof(slab_cache_t));
    if (cache == NULL) {
        return NULL;
    }
    
    if (__wrap_slab_cache_init(cache, name, obj_size, flags) != 0) {
        free(cache);
        return NULL;
    }
    
    return cache;
}

int __wrap_slab_cache_destroy(slab_cache_t *cache)
{
    if (cache != NULL) {
        free(cache);
    }
    return 0;
}

int __wrap_slab_cache_shrink(slab_cache_t *cache, int nums)
{
    (void)cache;
    (void)nums;
    return 0;
}

void *__wrap_slab_alloc(slab_cache_t *cache)
{
    if (cache == NULL) {
        return NULL;
    }
    
    if (g_test_fail_slab_alloc) {
        g_test_fail_slab_alloc = false;
        return NULL;
    }
    
    return calloc(1, cache->obj_size);
}

void __wrap_slab_free(void *obj)
{
    if (obj != NULL) {
        free(obj);
    }
}
