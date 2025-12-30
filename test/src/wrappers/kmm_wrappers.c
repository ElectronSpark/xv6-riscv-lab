/*
 * Kernel memory management wrappers for unit tests
 * Provides mock kmm_alloc/free
 */

#include <stddef.h>
#include <stdlib.h>
#include "types.h"

void *__wrap_kmm_alloc(size_t size)
{
    return malloc(size);
}

void __wrap_kmm_free(void *ptr)
{
    free(ptr);
}
