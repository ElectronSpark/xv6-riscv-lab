#ifndef __KERNEL_VM_H
#define __KERNEL_VM_H
#include <mm/vm_types.h>

#define VMA_SIZE(__vma) ((__vma)->end - (__vma)->start)
#define VMA_PG_ALIGNED(__vma)                                                  \
    (((__vma)->start & PAGE_MASK) == (__vma)->start &&                         \
     ((__vma)->end & PAGE_MASK) == (__vma)->end)
#define VMA_IN_RANGE(__vma, __va)                                              \
    ((__vma)->start <= (__va) && (__va) < (__vma)->end)
#define VMA_OVERLAP(__vma1, __vma2)                                            \
    ((__vma1)->end > (__vma2)->start && (__vma2)->end > (__vma1)->start)
#define VMA_ADJACENT(__vma1, __vma2)                                           \
    ((__vma1)->end == (__vma2)->start || (__vma2)->end == (__vma1)->start)

uint64 vm_cpu_online(vm_t *vm, int cpu);
void vm_cpu_offline(vm_t *vm, int cpu);
cpumask_t vm_get_cpumask(vm_t *vm);
void vm_remote_sfence(vm_t *vm);
void vm_rlock(vm_t *vm);
void vm_runlock(vm_t *vm);
void vm_wlock(vm_t *vm);
void vm_wunlock(vm_t *vm);
void vm_pgtable_lock(vm_t *vm);
void vm_pgtable_unlock(vm_t *vm);
vm_t *vm_init(void);
void vm_dup(vm_t *vm);
void vm_put(vm_t *vm);
vm_t *vm_copy(vm_t *src);
void vm_destroy(vm_t *vm);
vma_t *vma_alloc(vm_t *vm, uint64 va, uint64 size, uint64 flags);
int vm_growstack(vm_t *vm, int64 change_size);
int vm_growheap(vm_t *vm, int64 change_size);
int vm_createheap(vm_t *vm, uint64 va, uint64 size);
int vm_createstack(vm_t *vm, uint64 stack_top, uint64 size);
int vm_try_growstack(vm_t *vm, uint64 va);
int vma_free(vm_t *vm, vma_t *vma);
vma_t *vm_find_area(vm_t *vm, uint64 va);
vma_t *vma_split(vma_t *vma, uint64 va);
vma_t *vma_merge(vma_t *vma1, vma_t *vma2);
int vma_validate(vma_t *vma, uint64 va, uint64 size, uint64 flags);
int vm_copyout(vm_t *vm, uint64 dstva, const void *src, uint64 len);
int vm_copyin(vm_t *vm, void *dst, uint64 srcva, uint64 len);
int vm_copyinstr(vm_t *vm, char *dst, uint64 srcva, uint64 max);
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);

uint64 vma2pte_flags(uint64 flags);
uint64 pte2vma_flags(uint64 pte_flags);

// Memory protection and mapping operations (POSIX-compatible)
int vm_mprotect(vm_t *vm, uint64 addr, size_t size, int prot);
int vm_mmap_region(vm_t *vm, uint64 start, size_t size, uint64 flags,
                   struct vfs_file *file, uint64 pgoff, void *pa);
int vm_mmap_region_locked(vm_t *vm, uint64 start, size_t size, uint64 flags,
                          struct vfs_file *file, uint64 pgoff, void *pa);
int vm_munmap_region(vm_t *vm, uint64 start, size_t size);
uint64 vm_mremap(vm_t *vm, uint64 old_addr, size_t old_size, size_t new_size,
                 int flags, uint64 new_addr);
int vm_msync(vm_t *vm, uint64 addr, size_t size, int flags);
int vm_mincore(vm_t *vm, uint64 addr, size_t size, unsigned char *vec);
int vm_madvise(vm_t *vm, uint64 addr, size_t size, int advice);

// Pthread support functions
uint64 vm_find_free_range(vm_t *vm, size_t size, uint64 hint);
int vm_alloc_thread_stack(vm_t *vm, size_t stack_size, uint64 *stack_top_out);
int vm_free_thread_stack(vm_t *vm, uint64 stack_top, size_t stack_size);
uint64 vm_mmap(vm_t *vm, uint64 addr, size_t length, int prot, int flags,
               int fd, uint64 offset);
int vm_munmap(vm_t *vm, uint64 addr, size_t length);

// Note: PROT_*, MAP_*, and MAP_FAILED are defined in vm_types.h

#endif // __KERNEL_VM_H
