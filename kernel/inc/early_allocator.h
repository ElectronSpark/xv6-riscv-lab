// Simple allocation functions before the full kernel memory allocator is initialized.
// Memory allocated by early_alloc() is not freeable.
// Early allocator will only be used by the init hart before the full kernel memory allocator is ready. 
#ifndef __KERNEL_EARLY_ALLOCATOR_H
#define __KERNEL_EARLY_ALLOCATOR_H

#include "compiler.h"
#include "types.h"

void early_allocator_init(void *pa_start, void *pa_end);
void *early_alloc(size_t size);
void *early_alloc_align(size_t size, size_t align);
void *early_alloc_end_ptr(void);

#endif        /* __KERNEL_EARLY_ALLOCATOR_H */
