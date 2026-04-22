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

uint64 vm_cpu_online(vm_t *vm, int cpu, struct thread *p);
void vm_cpu_offline(vm_t *vm, int cpu);
cpumask_t vm_get_cpumask(vm_t *vm);
void vm_remote_sfence(vm_t *vm);
void vm_remote_sfence_page(vm_t *vm, uint64 va);
void vm_remote_sfence_range(vm_t *vm, uint64 start, uint64 size);
void vm_remote_fence_i(vm_t *vm);
void vm_rlock(vm_t *vm);
void vm_runlock(vm_t *vm);
void vm_wlock(vm_t *vm);
void vm_wunlock(vm_t *vm);
int  vm_is_wlocked(vm_t *vm);
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
void dump_vm(vm_t *vm);
vma_t *vma_split(vma_t *vma, uint64 va);
vma_t *vma_merge(vma_t *vma1, vma_t *vma2);
int vma_validate(vma_t *vma, uint64 va, uint64 size, uint64 flags);
int vm_copyout(vm_t *vm, uint64 dstva, const void *src, uint64 len);
int vm_copyin(vm_t *vm, void *dst, uint64 srcva, uint64 len);
int vm_copyinstr(vm_t *vm, char *dst, uint64 srcva, uint64 max);
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);

/*
 * Page-table walking, mapping, and PTE flag conversion are declared in
 * <mm/pgtable.h>.   Include that header instead of duplicating decls here.
 */

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

/* ========================================================================== */
/*  Kernel VM management                                                      */
/* ========================================================================== */
/*
 * The kernel VM is a singleton vm_t shared by all CPUs and kernel threads.
 * It tracks the kernel address space using the same VMA / RB-tree machinery
 * as user VMs, but:
 *   - Uses the boot-time kernel_pagetable (no new page table is created).
 *   - Pages are always eagerly allocated and mapped — no lazy allocation,
 *     no COW, no demand paging.
 *   - The linear-mapped area and trampoline area remain intact.
 *   - All kernel threads share this single vm_t (p->vm = kernel_vm).
 */

/* Global kernel VM instance (set during boot by kernel_vm_init). */
extern vm_t *kernel_vm;

/* Initialise the kernel VM singleton.  Must be called after arch_vm_init()
 * and vm_slab_init() so that the slab pools and kernel_pagetable exist. */
void kernel_vm_init(void);

/* Register a pre-existing identity-mapped kernel region as a VMA.
 * Called during boot to record text, rodata, data, BSS, physical-RAM
 * and device-MMIO mappings that were already installed by kvmmake/kvm_build.
 * @start  virtual (== physical) start address  (page-aligned)
 * @size   region size in bytes  (page-aligned, > 0)
 * @flags  PROT_READ | PROT_WRITE | PROT_EXEC as appropriate
 * Returns 0 on success, negative errno on failure.
 */
int kvm_register_region(uint64 start, uint64 size, uint64 flags);

/* Allocate @size bytes of kernel virtual memory backed by freshly allocated
 * physical pages.  The pages are identity-mapped into kernel_pagetable.
 * @addr  requested VA (0 = let the allocator choose)
 * @size  bytes to allocate (rounded up to PGSIZE)
 * @flags PROT_READ | PROT_WRITE | PROT_EXEC (VMA_FLAG_KERNEL is added
 *        internally)
 * Returns the base VA on success, 0 on failure.
 */
uint64 kvm_mmap(uint64 addr, size_t size, uint64 flags);

/* Release a previously kvm_mmap'd region.
 * Unmaps the pages from kernel_pagetable and frees the physical pages.
 * Returns 0 on success, negative errno on failure.
 */
int kvm_munmap(uint64 addr, size_t size);

/* Convenience wrappers: allocate / free @npages contiguous kernel pages. */
void *kvm_alloc(size_t npages);
void  kvm_free(void *addr, size_t npages);

/* Check whether @addr was allocated from the kernel VM (kvm_alloc/kvm_mmap)
 * rather than the slab allocator (kmm_alloc). */
int is_kvm_addr(const void *addr);

/* Allocate @size bytes of kernel memory.
 * Small allocations (<= PAGE_SIZE) are served from the slab allocator
 * (kmm_alloc).  Larger allocations are backed by kernel-VM pages
 * (kvm_alloc).  Returns NULL on failure. */
void *kvmalloc(size_t size);

/* Free memory obtained from kvmalloc().  The allocator that originally
 * provided the memory is determined automatically from the pointer. */
void  kvfree(void *ptr);

/* Dump the kernel VM's VMA list (debugging). */
void dump_kernel_vm(void);

#endif // __KERNEL_VM_H
