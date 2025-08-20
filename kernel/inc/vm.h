#ifndef __KERNEL_VM_H
#define __KERNEL_VM_H
#include "vm_types.h"

#define VMA_SIZE(__vma) ((__vma)->end - (__vma)->start)
#define VMA_PG_ALIGNED(__vma) \
    (((__vma)->start & PAGE_MASK) == (__vma)->start && \
     ((__vma)->end & PAGE_MASK) == (__vma)->end)
#define VMA_IN_RANGE(__vma, __va) \
    ((__vma)->start <= (__va) && (__va) < (__vma)->end)
#define VMA_OVERLAP(__vma1, __vma2) \
    ((__vma1)->end > (__vma2)->start && \
     (__vma2)->end > (__vma1)->start)
#define VM_ADJACENT(__vma1, __vma2) \
    ((__vma1)->end == (__vma2)->start || \
     (__vma2)->end == (__vma1)->start)

vm_t *vm_init(uint64 trapframe);
vm_t *vm_dup(vm_t *src, uint64 trapframe);
void vm_destroy(vm_t *vm);
vma_t *va_alloc(vm_t *vm, uint64 va, uint64 size, uint64 flags);
int vm_growstack(vm_t *vm, int change_size);
int vm_growheap(vm_t *vm, int change_size);
int vm_createheap(vm_t *vm, uint64 va, uint64 size);
int vm_createstack(vm_t *vm, uint64 stack_top, uint64 size);
int vm_try_growstack(vm_t *vm, uint64 va);
int va_free(vma_t *vma);
vma_t *vm_find_area(vm_t *vm, uint64 va);
vma_t *vma_split(vma_t *vma, uint64 va);
vma_t *vma_merge(vma_t *vma1, vma_t *vma2);
int vma_validate(vma_t *vma, uint64 va, uint64 size, uint64 flags);
int vm_copyout(vm_t *vm, uint64 dstva, void *src, uint64 len);
int vm_copyin(vm_t *vm, void *dst, uint64 srcva, uint64 len);
int vm_copyinstr(vm_t *vm, char *dst, uint64 srcva, uint64 max);

uint64 vm2pte_flags(uint64 flags);
uint64 pte2vm_flags(uint64 pte_flags);

// int vma_mprotect(vm_t *vma, uint64 flags);
int vma_mmap(vm_t *vm, uint64 start, size_t size, uint64 flags, struct file *file, uint64 pgoff, void *pa);
int vma_munmap(vm_t *vm, uint64 start, size_t size);
// int vma_mremap(vm_t *vm, uint64 old_start, uint64 old_end, uint64 new_start, uint64 new_end);
// int vma_msync(vm_t *vm, uint64 start, uint64 end);
// int vma_mincore(vm_t *vm, uint64 start, uint64 end, unsigned char *vec);
// int vma_madvise(vm_t *vm, uint64 start, uint64 end, int advice);


#endif // __KERNEL_VM_H
