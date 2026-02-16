/**
 * @file stdlib.h
 * @brief Minimal stdlib.h shim for lwext4 in xv6 kernel space.
 *
 * Shadows the C library stdlib.h so lwext4 uses kernel allocators.
 */
#ifndef _EXT4_COMPAT_STDLIB_H
#define _EXT4_COMPAT_STDLIB_H

#include "types.h"
#include "string.h"

/* Forward-declare kernel memory allocators (defined in kernel/mm/kalloc.c).
 * We avoid #include "defs.h" because it pulls in riscv/VM types that
 * conflict with lwext4's freestanding compilation. */
void *kmm_alloc(size_t size);
void kmm_free(void *ptr);

/* malloc / calloc / realloc / free — lwext4 uses these for bcache,
 * directory index splitting, and extent manipulation. */
static inline void *malloc(size_t size)
{
    return kmm_alloc(size);
}

static inline void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = kmm_alloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

static inline void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmm_alloc(size);
    if (size == 0) {
        kmm_free(ptr);
        return 0;
    }
    void *new_ptr = kmm_alloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, size);
        kmm_free(ptr);
    }
    return new_ptr;
}

static inline void free(void *ptr)
{
    if (ptr)
        kmm_free(ptr);
}

/* Simple insertion-sort qsort — ext4_dir_idx uses it for small arrays
 * of directory hash entries during htree splits. */
static inline void qsort(void *base, size_t nmemb, size_t size,
                          int (*compar)(const void *, const void *))
{
    char *arr = (char *)base;
    char tmp[64]; /* ext4 dx_sort_entry is 8 bytes — well within 64 */
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0; j--) {
            if (compar(arr + j * size, arr + (j - 1) * size) < 0) {
                memcpy(tmp, arr + j * size, size);
                memcpy(arr + j * size, arr + (j - 1) * size, size);
                memcpy(arr + (j - 1) * size, tmp, size);
            } else {
                break;
            }
        }
    }
}

#endif /* _EXT4_COMPAT_STDLIB_H */
