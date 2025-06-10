#ifndef __UT_SLAB_WRAPS_H__
#define __UT_SLAB_WRAPS_H__

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "cmocka.h"

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "slab.h"
#include "slab_private.h"
#include "ut_page_wraps.h"

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

#define SLAB_CACHE_COUNT 8

#endif // __UT_SLAB_WRAPS_H__