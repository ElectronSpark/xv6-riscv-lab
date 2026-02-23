/**
 * @file vm.c
 * @brief Shared virtual memory management.
 *
 * Architecture-independent process VM subsystem.  Contains:
 *  - Generic page-table walking primitives (walk / walkaddr / mappages)
 *    parameterised by PAGETABLE_LEVELS (3 for RISC-V Sv39, 4 for x86-64).
 *  - VMA allocator & lifetime (slab-backed vma_t / vm_t pools).
 *  - Process address-space management (vm_init / vm_copy / vm_destroy).
 *  - Demand paging, COW handling (vma_validate).
 *  - Safe user copies (vm_copyin / vm_copyout / vm_copyinstr).
 *  - POSIX-style mmap / munmap / mprotect / mremap / msync / mincore / madvise.
 *  - Heap & stack growth helpers.
 *  - Thread-stack allocation for pthreads.
 *
 * Architecture-specific pieces live in kernel/arch/<arch>/mm/vm.c:
 *  - Kernel page-table setup (kvmmake / kvm_build).
 *  - arch_vm_init / arch_vm_init_hart.
 *  - uvmcreate / freewalk / uvmfree (page-table structure details).
 *  - TLB shootdown helpers (vm_remote_sfence / vm_remote_fence_i).
 *  - Trampoline / trapframe mapping (arch_vm_setup_trampoline / _teardown).
 *  - vm_cpu_online / vm_cpu_offline / vm_get_cpumask.
 *  - dump_pagetable (level-count dependent).
 */

#include "types.h"
#include "param.h"
#include "riscv.h"       /* PTE_*, PA2PTE, PTE2PA, PX, PGSIZE, PAGETABLE_LEVELS */
#include "defs.h"
#include "arch/vm.h"
#include <mm/vm.h>
#include <mm/memlayout.h>
#include <mm/page.h>
#include <mm/slab.h>
#include <mm/pcache.h>
#include <smp/percpu.h>
#include <smp/atomic.h>
#include "list.h"
#include "lock/spinlock.h"
#include "lock/rwsem.h"
#include "rbtree.h"
#include "string.h"
#include "printf.h"
#include "proc/thread.h"
#include "errno.h"
#include <vfs/file.h>
#include <vfs/fs.h>
#include <dev/bio.h>

/* ========================================================================== */
/*  Slab pools                                                                */
/* ========================================================================== */

static slab_cache_t __vma_pool = {0};
static slab_cache_t __vm_pool = {0};

static void __vm_destroy(vm_t *vm);

static void __vma_pool_init(void) {
    slab_cache_init(&__vma_pool, "vm area", sizeof(vma_t),
                    SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
}

static void __vm_pool_init(void) {
    slab_cache_init(&__vm_pool, "vm", sizeof(vm_t),
                    SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
}

void vm_slab_init(void) {
    __vma_pool_init();
    __vm_pool_init();
}

/* ========================================================================== */
/*  Page-table page allocator                                                 */
/* ========================================================================== */

pde_t *pgtab_alloc(void) {
    void *pa = page_alloc(0, PAGE_TYPE_PGTABLE);
    if (pa) {
        memset(pa, 0, PGSIZE);
    }
    return (pde_t *)pa;
}

void pgtab_free(void *pa) { page_free(pa, 0); }

/* ========================================================================== */
/*  Generic page-table walking (parameterised by PAGETABLE_LEVELS)            */
/* ========================================================================== */

/*
 * Walk the multi-level page table and return a pointer to the leaf PTE
 * for virtual address @va.  If @alloc is set, intermediate page-table
 * pages are allocated as needed.
 *
 * The number of levels is determined by PAGETABLE_LEVELS:
 *   RISC-V Sv39 = 3 (levels 2 -> 1 -> 0)
 *   x86-64      = 4 (levels 3 -> 2 -> 1 -> 0)
 */
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc, pte_t **retl2,
            pte_t **retl1)
{
    assert(va < MAXVA, "walk: va out of range");
    assert(pagetable != NULL, "walk: pagetable is null");

    pte_t *ret_pte[PAGETABLE_LEVELS];
    for (int i = 0; i < PAGETABLE_LEVELS; i++)
        ret_pte[i] = NULL;

    for (int level = PAGETABLE_LEVELS - 1; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        ret_pte[level] = pte;
        assert(pte != NULL, "walk: pte is null");
        if (*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc || (pagetable = (pde_t *)pgtab_alloc()) == 0)
                return NULL;
            memset(pagetable, 0, PGSIZE);
            *pte = PA2PTE(pagetable) | PTE_V;
        }
    }

    /* Return arch-specific intermediate PTE pointers when requested.
     * On Sv39 level 2 is the root, level 1 is the middle.
     * On x86-64 these indices are shifted but callers always pass NULL. */
    if (retl2 && PAGETABLE_LEVELS > 2)
        *retl2 = ret_pte[PAGETABLE_LEVELS > 3 ? 3 : 2];
    if (retl1 && PAGETABLE_LEVELS > 1)
        *retl1 = ret_pte[PAGETABLE_LEVELS > 3 ? 2 : 1];

    return &pagetable[PX(0, va)];
}

/*
 * Look up a user virtual address and return its physical address,
 * or 0 if not mapped or not user-accessible.
 */
uint64 walkaddr(pagetable_t pagetable, uint64 va)
{
    pte_t *pte;
    uint64 pa;

    if (va >= MAXVA)
        return 0;

    pte = walk(pagetable, va, 0, NULL, NULL);
    if (pte == 0)
        return 0;
    if ((*pte & PTE_V) == 0)
        return 0;
    if ((*pte & PTE_U) == 0)
        return 0;
    pa = PTE2PA(*pte);
    return pa;
}

/*
 * Add a mapping to a kernel page table.
 * Only used when booting; does not flush TLB or enable paging.
 */
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
    if (mappages(kpgtbl, va, sz, pa, perm) != 0)
        panic("kvmmap");
}

/*
 * Create PTEs for virtual addresses starting at @va that refer to
 * physical addresses starting at @pa.  Both @va and @size MUST be
 * page-aligned.
 * Returns 0 on success, negative errno if a page-table page could not
 * be allocated.
 */
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
             int perm)
{
    uint64 a, last;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("mappages: va not aligned, va %p", (void *)va);
    if ((size % PGSIZE) != 0)
        panic("mappages: size not aligned, va %p, size %p", (void *)va,
              (void *)size);
    if (size == 0)
        panic("mappages: size zero, va %p", (void *)va);

    a = va;
    last = va + size - PGSIZE;
    for (;;) {
        if ((pte = walk(pagetable, a, 1, NULL, NULL)) == 0)
            return -ENOMEM;
        if (*pte & PTE_V)
            panic("mappages: remap, va=%p pa=%p existing_pte=%p (pa=%p flags=%p)",
                  (void *)a, (void *)pa, (void *)*pte,
                  (void *)PTE2PA(*pte), (void *)PTE_FLAGS(*pte));
        *pte = PA2PTE(pa) | perm | PTE_V | PTE_A | PTE_D;
        if (a == last)
            break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

/*
 * Remove @npages of mappings starting from @va.  @va must be page-aligned.
 * The mappings must exist.  If @do_free, the physical pages are freed.
 */
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
    uint64 a;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
        if ((pte = walk(pagetable, a, 0, NULL, NULL)) == 0)
            panic("uvmunmap: walk");
        if ((*pte & PTE_V) == 0)
            panic("uvmunmap: not mapped, va=%p, pa=%p, flags: %llx", (void *)a,
                  (void *)PTE2PA(*pte), (unsigned long long)PTE_FLAGS(*pte));
        if (PTE_FLAGS(*pte) == PTE_V)
            panic("uvmunmap: not a leaf");
        uint64 pa = PTE2PA(*pte);
        *pte = 0;
        if (do_free) {
            pgtab_free((void *)pa);
        }
    }
}

/* ========================================================================== */
/*  PTE ↔ VMA flag conversion                                                */
/* ========================================================================== */

uint64 vma2pte_flags(uint64 flags)
{
    uint64 pte_flags = 0;
    if (flags & PROT_READ)
        pte_flags |= PTE_R;
    if (flags & PROT_WRITE)
        pte_flags |= PTE_W;
    if (flags & PROT_EXEC)
        pte_flags |= PTE_X;
    if (flags & VMA_FLAG_USER)
        pte_flags |= PTE_U;
    return pte_flags;
}

uint64 pte2vma_flags(uint64 pte_flags)
{
    uint64 flags = 0;
    if (pte_flags & PTE_R)
        flags |= PROT_READ;
    if (pte_flags & PTE_W)
        flags |= PROT_WRITE;
    if (pte_flags & PTE_X)
        flags |= PROT_EXEC;
    if (pte_flags & PTE_U)
        flags |= VMA_FLAG_USER;
    return flags;
}

/* ========================================================================== */
/*  Debugging helpers                                                         */
/* ========================================================================== */

int vm_dump_flags(uint64 flags, char *buf, size_t buf_size)
{
    if (buf == NULL)
        return -EINVAL;
    if (buf_size < 5)
        return -ERANGE;
    size_t len = 0;
    buf[len++] = (flags & PROT_READ)    ? 'R' : ' ';
    buf[len++] = (flags & PROT_WRITE)   ? 'W' : ' ';
    buf[len++] = (flags & PROT_EXEC)    ? 'X' : ' ';
    buf[len++] = (flags & VMA_FLAG_USER)? 'U' : ' ';
    buf[len] = '\0';
    return len;
}

void dump_vm(vm_t *vm)
{
    if (vm == NULL)
        return;
    printf("VM dump:\n");
    printf("Pagetable: %p\n", vm->pagetable);
    printf("VMAs:\n");
    vma_t *vma, *tmp;
    list_foreach_node_safe(&vm->vm_list, vma, tmp, list_entry) {
        char flags_buf[10] = {0};
        vm_dump_flags(vma->flags, flags_buf, sizeof(flags_buf));
        printf("VMA: start=%lx, end=%lx, flags=%s, file=%p, pgoff=%lx\n",
               vma->start, vma->end, flags_buf, vma->file, vma->pgoff);
    }
}

/* ========================================================================== */
/*  VMA allocator helpers (slab-backed)                                       */
/* ========================================================================== */

#define VMA_FREED_MAGIC 0xDEAD0BADDEAD0BADULL

static vma_t *__vma_alloc(vm_t *vm)
{
    vma_t *vma = slab_alloc(&__vma_pool);
    if (vma == NULL)
        return NULL;
    memset(vma, 0, sizeof(vma_t));
    rb_node_init(&vma->rb_entry);
    list_entry_init(&vma->list_entry);
    list_entry_init(&vma->free_list_entry);
    vma->vm = vm;
    return vma;
}

static void __vma_free(vma_t *vma)
{
    if (vma) {
        if (vma->start == VMA_FREED_MAGIC && vma->end == VMA_FREED_MAGIC) {
            printf("__vma_free: DOUBLE FREE detected for vma=%p\n", vma);
            return;
        }
        if (vma->vm == NULL) {
            printf("__vma_free: vma->vm is NULL for vma=%p (possible "
                   "double-free or corruption)\n", vma);
            return;
        }
        vma->vm = NULL;
        vma->start = VMA_FREED_MAGIC;
        vma->end = VMA_FREED_MAGIC;
        slab_free((void *)vma);
    }
}

/* Free the pages/PTEs in a VMA and mark it as free (PROT_NONE). */
static void __vma_set_free(vma_t *vma)
{
    if (vma == NULL || vma->vm == NULL)
        return;
    if (vma->flags == PROT_NONE)
        return;

    assert((vma->start & (PGSIZE - 1)) == 0,
           "__vma_set_free: vma start not aligned");

    if (vma->vm->pagetable != NULL) {
        pagetable_t pagetable = vma->vm->pagetable;
        for (uint64 a = vma->start; a < vma->end; a += PGSIZE) {
            pte_t *pte = walk(pagetable, a, 0, NULL, NULL);
            if (pte == 0)
                continue;
            if (PTE_FLAGS(*pte) == PTE_V)
                panic("__vma_set_free: not a leaf");
            if ((*pte & PTE_V) == 0)
                continue;
            uint64 pa = PTE2PA(*pte);
            *pte = 0;
            vm_remote_sfence(vma->vm);
            page_ref_dec((void *)pa);
        }
    }

    vma->flags = PROT_NONE;
    if (vma->file != NULL)
        vfs_fput(vma->file);
    vma->file = NULL;
    vma->pgoff = 0;
    assert(LIST_NODE_IS_DETACHED(vma, free_list_entry),
           "__vma_set_free: vma already in free list");
}

static int __vma_copy(vma_t *dst, vma_t *src)
{
    if (dst == NULL || src == NULL)
        return -EINVAL;
    if (src->vm == NULL || dst->vm == NULL)
        return -EINVAL;
    if (VMA_SIZE(src) != VMA_SIZE(dst))
        return -EINVAL;
    if ((src->flags & VMA_FLAG_PROT_MASK) !=
        (dst->flags & VMA_FLAG_PROT_MASK))
        return -EINVAL;

    dst->flags = src->flags;
    dst->pgoff = src->pgoff;
    if (src->file != NULL) {
        dst->file = vfs_fdup(src->file);
        if (dst->file == NULL)
            return -EBADF;
    } else {
        dst->file = NULL;
    }

    if (src->flags != PROT_NONE) {
        pagetable_t pgtb_src = src->vm->pagetable;
        pagetable_t pgtb_dst = dst->vm->pagetable;
        int is_shared = (src->flags & VMA_FLAG_SHARED) != 0;
        for (uint64 a = src->start; a < src->end; a += PGSIZE) {
            pte_t *src_pte = walk(pgtb_src, a, 0, NULL, NULL);
            if (src_pte == NULL || *src_pte == 0)
                continue;
            if (PTE_FLAGS(*src_pte) == PTE_V)
                panic("__vma_copy: not a leaf");
            if ((PTE_FLAGS(*src_pte) & PTE_V) == 0)
                continue;
            pte_t *new_pte = walk(pgtb_dst, a, 1, NULL, NULL);
            if (new_pte == NULL) {
                __vma_set_free(dst);
                return -ENOMEM;
            }
            if (is_shared) {
                *new_pte = *src_pte;
            } else {
                *src_pte |= PTE_RSW_w;
                *src_pte &= ~PTE_W;
                *new_pte = *src_pte;
            }
            uint64 pa = PTE2PA(*src_pte);
            assert(page_ref_inc((void *)pa) > 0,
                   "__vma_copy: page refcnt should be greater than 0");
        }
        vm_remote_sfence(src->vm);
    }
    return 0;
}

/* ========================================================================== */
/*  RB-tree helpers for VMA tree                                              */
/* ========================================================================== */

static int __vma_cmp(uint64 a, uint64 b)
{
    if (a == b) return 0;
    if (a < b) return -1;
    return 1;
}

static uint64 __cma_get_key(struct rb_node *node)
{
    vma_t *vma = container_of(node, vma_t, rb_entry);
    return vma->start;
}

static struct rb_root_opts __vm_tree_opts = {
    .keys_cmp_fun = __vma_cmp,
    .get_key_fun = __cma_get_key,
};

/* ========================================================================== */
/*  VM lifecycle                                                              */
/* ========================================================================== */

/* Magic for detecting double-destroy of VM. */
#define VM_DESTROYED_MAGIC ((pagetable_t)0xDEAD0BADULL)

vm_t *vm_init(void)
{
    vm_t *vm = slab_alloc(&__vm_pool);
    if (vm == NULL)
        return NULL;
    memset(vm, 0, sizeof(vm_t));
    rb_root_init(&vm->vm_tree, &__vm_tree_opts);
    list_entry_init(&vm->vm_list);
    list_entry_init(&vm->vm_free_list);

    vma_t *vma = __vma_alloc(vm);
    if (vma == NULL) {
        __vm_destroy(vm);
        return NULL;
    }

    /* Initial free VMA covering the entire user address space. */
    vma->start = UVMBOTTOM;
    vma->end = UVMTOP;
    rb_insert_color(&vm->vm_tree, &vma->rb_entry);
    list_node_push(&vm->vm_free_list, vma, free_list_entry);
    list_node_push(&vm->vm_list, vma, list_entry);

    vm->pagetable = uvmcreate();
    if (vm->pagetable == NULL) {
        __vm_destroy(vm);
        return NULL;
    }

    if (arch_vm_setup_trampoline(vm) != 0) {
        __vm_destroy(vm);
        return NULL;
    }

    spin_init(&vm->spinlock, "vm_pgtable_lock");
    rwsem_init(&vm->rw_lock, RWLOCK_PRIO_READ, "vm_rw_lock");
    vm->refcount = 1;
    return vm;
}

void vm_dup(vm_t *vm) { atomic_inc(&vm->refcount); }

void vm_put(vm_t *vm)
{
    if (vm == NULL)
        return;
    if (!atomic_dec_unless(&vm->refcount, 1))
        __vm_destroy(vm);
}

static void __vm_destroy(vm_t *vm)
{
    if (vm == NULL)
        return;
    if (vm->pagetable == VM_DESTROYED_MAGIC) {
        printf("__vm_destroy: DOUBLE DESTROY detected for vm=%p\n", vm);
        return;
    }

    vma_t *vma, *tmp;
    list_foreach_node_safe(&vm->vm_list, vma, tmp, list_entry) {
        if (vma->vm != vm) {
            printf("__vm_destroy: vma->vm mismatch! vma=%p vma->vm=%p "
                   "expected=%p\n", vma, vma->vm, vm);
            continue;
        }
        if (vma->start == VMA_FREED_MAGIC || vma->end == VMA_FREED_MAGIC) {
            printf("__vm_destroy: vma already freed! vma=%p\n", vma);
            continue;
        }
        __vma_set_free(vma);
        __vma_free(vma);
    }

    list_entry_init(&vm->vm_list);
    list_entry_init(&vm->vm_free_list);
    rb_root_init(&vm->vm_tree, &__vm_tree_opts);

    arch_vm_teardown_trampoline(vm);

    if (vm->pagetable != NULL)
        uvmfree(vm->pagetable, 0);

    vm->pagetable = VM_DESTROYED_MAGIC;
    slab_free((void *)vm);
}

vm_t *vm_copy(vm_t *src)
{
    if (src == NULL)
        return ERR_PTR(-EINVAL);
    vm_t *dst = vm_init();
    if (dst == NULL)
        return ERR_PTR(-ENOMEM);
    vm_rlock(src);
    vm_wlock(dst);

    vma_t *vma, *tmp;
    list_foreach_node_safe(&src->vm_list, vma, tmp, list_entry) {
        if (vma->flags == PROT_NONE)
            continue;
        vma_t *new_vma = vma_alloc(dst, vma->start, VMA_SIZE(vma), vma->flags);
        if (new_vma == NULL) {
            vm_runlock(src);
            vm_wunlock(dst);
            vm_put(dst);
            return ERR_PTR(-ENOMEM);
        }
        if (vma == src->stack) {
            dst->stack = new_vma;
            dst->stack_size = src->stack_size;
        } else if (vma == src->heap) {
            dst->heap = new_vma;
            dst->heap_size = src->heap_size;
            dst->heap_reserve_end = src->heap_reserve_end;
        }
        if (vma->flags != PROT_NONE && __vma_copy(new_vma, vma) != 0) {
            vm_runlock(src);
            vm_wunlock(dst);
            vm_put(dst);
            return ERR_PTR(-ENOMEM);
        }
    }

    vm_runlock(src);
    vm_wunlock(dst);
    return dst;
}

/* ========================================================================== */
/*  VM locking                                                                */
/* ========================================================================== */

void vm_rlock(vm_t *vm)          { rwsem_acquire_read(&vm->rw_lock); }
void vm_runlock(vm_t *vm)        { rwsem_release(&vm->rw_lock); }
void vm_wlock(vm_t *vm)          { rwsem_acquire_write(&vm->rw_lock); }
void vm_wunlock(vm_t *vm)        { rwsem_release(&vm->rw_lock); }
void vm_pgtable_lock(vm_t *vm)   { spin_lock(&vm->spinlock); }
void vm_pgtable_unlock(vm_t *vm) { spin_unlock(&vm->spinlock); }

/* ========================================================================== */
/*  VMA tree operations                                                       */
/* ========================================================================== */

static inline vma_t *__get_vma_left(vma_t *vma)
{
    if (vma == NULL || vma->vm == NULL)
        return NULL;
    return rb_prev_entry_safe(vma, rb_entry);
}

static inline vma_t *__get_vma_right(vma_t *vma)
{
    if (vma == NULL || vma->vm == NULL)
        return NULL;
    return rb_next_entry_safe(vma, rb_entry);
}

vma_t *vm_find_area(vm_t *vm, uint64 va)
{
    if (va >= UVMTOP || va < UVMBOTTOM)
        return NULL;
    struct rb_node *node = rb_find_key_rdown(&vm->vm_tree, va);
    if (node != NULL) {
        vma_t *vma = container_of(node, vma_t, rb_entry);
        assert(VMA_IN_RANGE(vma, va),
               "vm_find_area: va %lx not in range [%lx, %lx)", va, vma->start,
               vma->end);
        return vma;
    }
    return NULL;
}

vma_t *vma_split(vma_t *vma, uint64 va)
{
    if (vma == NULL || vma->vm == NULL)
        return NULL;
    if (va < vma->start || va >= vma->end)
        return NULL;
    if (va == vma->start)
        return vma;

    vma_t *new_vma = __vma_alloc(vma->vm);
    if (new_vma == NULL)
        return NULL;

    new_vma->start = va;
    assert(rb_insert_color(&vma->vm->vm_tree, &new_vma->rb_entry) ==
               &new_vma->rb_entry,
           "vma_split: rb_insert_color failed");

    new_vma->end = vma->end;
    new_vma->flags = vma->flags;
    if (vma->file != NULL) {
        new_vma->file = vfs_fdup(vma->file);
        new_vma->pgoff = vma->pgoff + (va - vma->start);
    } else {
        new_vma->file = NULL;
        new_vma->pgoff = 0;
    }

    vma->end = va;

    list_node_insert(vma, new_vma, list_entry);
    if (vma->flags == PROT_NONE)
        list_node_insert(vma, new_vma, free_list_entry);

    return new_vma;
}

vma_t *vma_merge(vma_t *vma1, vma_t *vma2)
{
    if (vma1 == NULL || vma2 == NULL || vma1->vm != vma2->vm)
        return NULL;
    if (!VMA_ADJACENT(vma1, vma2))
        return NULL;
    if ((vma1->flags & VMA_FLAG_PROT_MASK) !=
        (vma2->flags & VMA_FLAG_PROT_MASK))
        return NULL;
    if (vma1->start > vma2->start) {
        vma_t *tmp = vma1;
        vma1 = vma2;
        vma2 = tmp;
    }
    if (vma1->file != vma2->file)
        return NULL;
    if (vma1->file != NULL &&
        (vma2->pgoff - vma1->pgoff) != (vma2->start - vma1->start))
        return NULL;

    vma1->end = vma2->end;
    assert(rb_delete_node_color(&vma2->vm->vm_tree, &vma2->rb_entry) ==
               &vma2->rb_entry,
           "vma_merge: rb_delete_node_color failed");
    list_node_detach(vma2, list_entry);
    list_node_detach(vma2, free_list_entry);
    if (vma2->file != NULL) {
        vfs_fput(vma2->file);
        vma2->file = NULL;
    }
    __vma_free(vma2);
    return vma1;
}

vma_t *vma_alloc(vm_t *vm, uint64 va, uint64 size, uint64 flags)
{
    assert(current == NULL || rwsem_is_write_holding(&vm->rw_lock),
           "vma_alloc: vm rwsem must be write-held");
    if (vm == NULL)
        return NULL;
    if (size == 0 || (size & (PGSIZE - 1)) != 0)
        return NULL;
    if ((va & (PGSIZE - 1)) != 0)
        return NULL;
    if ((flags & VMA_FLAG_PROT_MASK) == 0)
        return NULL;

    vma_t *free_area = NULL;
    if (va == 0) {
        vma_t *tmp = NULL;
        list_foreach_node_inv_safe(&vm->vm_free_list, free_area, tmp,
                                   free_list_entry) {
            if (VMA_SIZE(free_area) >= size)
                break;
        }
    } else {
        free_area = vm_find_area(vm, va);
    }

    if (free_area == NULL)
        return NULL;
    if (free_area->flags != PROT_NONE)
        return NULL;

    uint64 va_end = 0;
    if (va == 0) {
        if (VMA_SIZE(free_area) < size)
            return NULL;
        va = free_area->start;
    } else {
        if (free_area->end - va < size)
            return NULL;
    }
    va_end = va + size;

    vma_t *vma2 = NULL;
    vma_t *vma3 = NULL;
    vma_t *original_left = NULL;

    if (va > free_area->start) {
        original_left = free_area;
        vma2 = vma_split(free_area, va);
        if (vma2 == NULL)
            return NULL;
    } else {
        vma2 = free_area;
    }

    if (va_end < vma2->end) {
        vma3 = vma_split(vma2, va_end);
        if (vma3 == NULL) {
            if (original_left != NULL)
                vma_merge(original_left, vma2);
            return NULL;
        }
    }

    list_node_detach(vma2, free_list_entry);
    vma2->flags = flags;
    return vma2;
}

int vma_free(vm_t *vm, vma_t *vma)
{
    assert(rwsem_is_write_holding(&vm->rw_lock),
           "vma_free: vm rwsem must be write-held");
    if (vma == NULL || vma->vm != vm)
        return -EINVAL;
    if (vma->flags == PROT_NONE)
        return -EINVAL;

    vma_t *left = __get_vma_left(vma);
    vma_t *right = __get_vma_right(vma);

    if (left == NULL && right == NULL)
        return -EINVAL;

    __vma_set_free(vma);
    list_node_push_back(&vm->vm_free_list, vma, free_list_entry);
    if (left != NULL && left->flags == PROT_NONE)
        vma = vma_merge(left, vma);
    if (right != NULL && right->flags == PROT_NONE)
        vma_merge(vma, right);

    return 0;
}

/* ========================================================================== */
/*  Demand paging / COW                                                       */
/* ========================================================================== */

static void *__vma_fault_file_page(vma_t *vma, uint64 va)
{
    struct vfs_file *file = vma->file;
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return NULL;
    struct pcache *pc = &inode->i_data;

    uint64 file_off = vma->pgoff + (va - vma->start);

    void *pa = page_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
    if (pa == NULL)
        return NULL;

    if (file_off >= (uint64)inode->size) {
        memset(pa, 0, PGSIZE);
        return pa;
    }

    uint64 bytes_to_read = PGSIZE;
    if (file_off + PGSIZE > (uint64)inode->size)
        bytes_to_read = (uint64)inode->size - file_off;

    uint64 blkno_512 = file_off / BLK_SIZE;
    page_t *pcpage = pcache_get_page(pc, blkno_512);
    if (pcpage == NULL) {
        page_free(pa, 0);
        return NULL;
    }
    int ret = pcache_read_page(pc, pcpage);
    if (ret != 0) {
        pcache_put_page(pc, pcpage);
        page_free(pa, 0);
        return NULL;
    }

    struct pcache_node *pcn = pcpage->pcache.pcache_node;
    memmove(pa, pcn->data, bytes_to_read);

    if (bytes_to_read < PGSIZE)
        memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);

    pcache_put_page(pc, pcpage);
    return pa;
}

static int __vma_validate_pte_rxw(vma_t *vma, pte_t *pte)
{
    pte_t pte_val = *pte;

    if ((pte_val & (PTE_W | PTE_A | PTE_D)) == (PTE_W | PTE_A | PTE_D))
        return 0;

    pte_t flags = PTE_FLAGS(pte_val);
    void *addr = (void *)PTE2PA(pte_val);
    void *pa = NULL;

    if (pte_val == 0) {
        pa = page_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
        if (pa == NULL)
            return -ENOMEM;
        memset(pa, 0, PGSIZE);
    } else if (pte_val & PTE_V) {
        if (pte_val & PTE_RSW_w) {
            pa = page_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
            if (pa == NULL)
                return -ENOMEM;
            memmove(pa, addr, PGSIZE);
            flags &= ~PTE_RSW_w;
            assert(page_ref_dec(addr) >= 0,
                   "vma_validate_pte_w: page_ref_dec failed for addr %p", addr);
        } else {
            pa = addr;
        }
    } else {
        return -EFAULT;
    }

    flags |= PTE_V | PTE_W | PTE_A | PTE_D;
    if (vma->flags & PROT_READ)
        flags |= PTE_R;
    if (vma->flags & PROT_EXEC)
        flags |= PTE_X;
    if (vma->flags & VMA_FLAG_USER)
        flags |= PTE_U;
    *pte = PA2PTE(pa) | flags;

    sfence_vma();
    return 0;
}

static int __vma_validate_pte_rx(vma_t *vma, pte_t *pte)
{
    pte_t pte_val = *pte;
    void *pa = (void *)PTE2PA(pte_val);
    pte_t flags = PTE_FLAGS(pte_val);

    if (pte_val == 0) {
        pa = page_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
        if (pa == NULL)
            return -ENOMEM;
        memset(pa, 0, PGSIZE);
        if (vma->flags & PROT_WRITE)
            flags |= PTE_W;
    } else if (!(pte_val & PTE_V)) {
        return -EFAULT;
    }

    if (vma->flags & PROT_READ)
        flags |= PTE_R;
    if (vma->flags & PROT_EXEC)
        flags |= PTE_X;
    if (vma->flags & VMA_FLAG_USER)
        flags |= PTE_U;

    flags |= PTE_V | PTE_A | PTE_D;
    *pte = PA2PTE(pa) | flags;

    sfence_vma();
    return 0;
}

static int __vma_validate_pte(vma_t *vma, pte_t *pte, uint64 flags)
{
    bool pte_user = (*pte & PTE_U) != 0;
    bool vma_user = (flags & VMA_FLAG_USER) != 0;

    if (*pte != 0 && (pte_user ^ vma_user))
        return -EACCES;

    if ((flags & PROT_WRITE) && __vma_validate_pte_rxw(vma, pte) != 0)
        return -EFAULT;
    else if ((flags & (PROT_READ | PROT_EXEC)) &&
             __vma_validate_pte_rx(vma, pte) != 0)
        return -EFAULT;
    return 0;
}

int vma_validate(vma_t *vma, uint64 va, uint64 size, uint64 flags)
{
    if (flags == PROT_NONE)
        return -EINVAL;
    if (vma == NULL || vma->vm == NULL || vma->vm->pagetable == NULL)
        return -EINVAL;
    if (flags & ~VMA_FLAG_PROT_MASK)
        return -EINVAL;
    if ((flags & PROT_EXEC)) {
        if ((flags & PROT_READ) == 0)
            return -EINVAL;
        if ((flags & PROT_WRITE) != 0 && (flags & VMA_FLAG_USER) != 0)
            return -EACCES;
    }

    uint64 va_end = va + size;
    va = PGROUNDDOWN(va);
    if (size == 0)
        va_end = vma->end;
    else
        va_end = PGROUNDUP(va_end);
    if (va < vma->start || va_end > vma->end)
        return -EFAULT;
    if ((flags & vma->flags) != flags)
        return -EACCES;

    vm_pgtable_lock(vma->vm);
    smp_mb();

    for (uint64 i = va; i < va_end; i += PGSIZE) {
        /* File-backed VMA: may need to drop spinlock for I/O */
        if (vma->file != NULL) {
            pte_t *pte = walk(vma->vm->pagetable, i, 1, NULL, NULL);
            if (pte == NULL) {
                vm_pgtable_unlock(vma->vm);
                return -ENOMEM;
            }
            if (*pte == 0) {
                vm_pgtable_unlock(vma->vm);

                void *pa;
                if (vma->file->ops != NULL && vma->file->ops->fault != NULL)
                    pa = vma->file->ops->fault(vma->file, vma, i);
                else
                    pa = __vma_fault_file_page(vma, i);
                if (pa == NULL)
                    return -ENOMEM;

                vm_pgtable_lock(vma->vm);
                pte = walk(vma->vm->pagetable, i, 1, NULL, NULL);
                if (pte == NULL) {
                    vm_pgtable_unlock(vma->vm);
                    page_free(pa, 0);
                    return -ENOMEM;
                }
                if (*pte != 0) {
                    page_free(pa, 0);
                } else {
                    pte_t pte_flags = 0;
                    if (vma->flags & PROT_READ)
                        pte_flags |= PTE_R;
                    if (vma->flags & PROT_WRITE)
                        pte_flags |= PTE_W;
                    if (vma->flags & PROT_EXEC)
                        pte_flags |= PTE_X;
                    if (vma->flags & VMA_FLAG_USER)
                        pte_flags |= PTE_U;
                    pte_flags |= PTE_V | PTE_A | PTE_D;
                    *pte = PA2PTE(pa) | pte_flags;
                    sfence_vma();
                }
                continue;
            }
            if (__vma_validate_pte(vma, pte, flags) != 0) {
                vm_pgtable_unlock(vma->vm);
                return -EFAULT;
            }
            continue;
        }

        /* Anonymous VMA path */
        pte_t *pte = walk(vma->vm->pagetable, i, 1, NULL, NULL);
        if (pte == NULL) {
            vm_pgtable_unlock(vma->vm);
            return -ENOMEM;
        }
        if (__vma_validate_pte(vma, pte, flags) != 0) {
            vm_pgtable_unlock(vma->vm);
            return -EFAULT;
        }
    }

    vm_pgtable_unlock(vma->vm);
    return 0;
}

/* ========================================================================== */
/*  User memory accessors                                                     */
/* ========================================================================== */

int vm_copyout(vm_t *vm, uint64 dstva, const void *src, uint64 len)
{
    uint64 n, va0, pa0;
    pte_t *pte;
    int ret = 0;

    vm_rlock(vm);
    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA) {
            ret = -EFAULT;
            goto out;
        }
        vma_t *vma = vm_find_area(vm, va0);
        if (vma == NULL ||
            vma_validate(vma, va0, PGSIZE, VMA_FLAG_USER | PROT_WRITE) != 0) {
            if (va0 >= USTACK_MAX_BOTTOM && va0 < USTACKTOP) {
                vm_runlock(vm);
                if (vm_try_growstack(vm, va0) == 0) {
                    vm_rlock(vm);
                    vma = vm_find_area(vm, va0);
                    if (vma != NULL &&
                        vma_validate(vma, va0, PGSIZE,
                                     VMA_FLAG_USER | PROT_WRITE) == 0)
                        goto copyout_ok;
                }
                return -EFAULT;
            }
            ret = -EFAULT;
            goto out;
        }
copyout_ok:
        pte = walk(vm->pagetable, va0, 0, NULL, NULL);
        assert(pte != NULL, "vma_copyout: pte should not be null");

        pa0 = PTE2PA(*pte);
        n = PGSIZE - (dstva - va0);
        if (n > len)
            n = len;
        memmove((void *)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
out:
    vm_runlock(vm);
    return ret;
}

int vm_copyin(vm_t *vm, void *dst, uint64 srcva, uint64 len)
{
    uint64 n, va0, pa0;
    int ret = 0;
    vm_rlock(vm);

    while (len > 0) {
        va0 = PGROUNDDOWN(srcva);
        vma_t *vma = vm_find_area(vm, va0);
        if (vma == NULL ||
            vma_validate(vma, va0, PGSIZE, VMA_FLAG_USER | PROT_READ) != 0) {
            if (va0 >= USTACK_MAX_BOTTOM && va0 < USTACKTOP) {
                vm_runlock(vm);
                if (vm_try_growstack(vm, va0) == 0) {
                    vm_rlock(vm);
                    vma = vm_find_area(vm, va0);
                    if (vma != NULL &&
                        vma_validate(vma, va0, PGSIZE,
                                     VMA_FLAG_USER | PROT_READ) == 0)
                        goto copyin_ok;
                }
                return -EFAULT;
            }
            ret = -EFAULT;
            goto out;
        }
copyin_ok:
        pa0 = walkaddr(vm->pagetable, va0);
        if (pa0 == 0) {
            ret = -EFAULT;
            goto out;
        }
        n = PGSIZE - (srcva - va0);
        if (n > len)
            n = len;
        memmove(dst, (void *)(pa0 + (srcva - va0)), n);

        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
out:
    vm_runlock(vm);
    return ret;
}

int vm_copyinstr(vm_t *vm, char *dst, uint64 srcva, uint64 max)
{
    uint64 n, va0, pa0;
    int got_null = 0;
    int ret = 0;

    vm_rlock(vm);
    while (got_null == 0 && max > 0) {
        va0 = PGROUNDDOWN(srcva);
        vma_t *vma = vm_find_area(vm, va0);
        if (vma == NULL ||
            vma_validate(vma, va0, PGSIZE, VMA_FLAG_USER | PROT_READ) != 0) {
            ret = -EFAULT;
            goto out;
        }
        pa0 = walkaddr(vm->pagetable, va0);
        if (pa0 == 0) {
            ret = -EFAULT;
            goto out;
        }
        n = PGSIZE - (srcva - va0);
        if (n > max)
            n = max;

        char *p = (char *)(pa0 + (srcva - va0));
        while (n > 0) {
            if (*p == '\0') {
                *dst = '\0';
                got_null = 1;
                break;
            } else {
                *dst = *p;
            }
            --n;
            --max;
            p++;
            dst++;
        }

        srcva = va0 + PGSIZE;
    }
    if (!got_null)
        ret = -ENAMETOOLONG;
out:
    vm_runlock(vm);
    return ret;
}

int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
    struct thread *p = current;
    if (user_dst)
        return vm_copyout(p->vm, dst, src, len);
    else {
        memmove((char *)dst, src, len);
        return 0;
    }
}

int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
    struct thread *p = current;
    if (user_src)
        return vm_copyin(p->vm, dst, src, len);
    else {
        memmove(dst, (char *)src, len);
        return 0;
    }
}

/* ========================================================================== */
/*  Heap & stack management                                                   */
/* ========================================================================== */

int vm_createheap(vm_t *vm, uint64 va, uint64 size)
{
    assert(current == NULL || rwsem_is_write_holding(&vm->rw_lock),
           "vm_createheap: vm rwsem must be write-held");
    size = PGROUNDUP(size);
    if ((va & (PGSIZE - 1)) != 0)
        return -EINVAL;
    if (va >= UVMTOP || va + size > UVMTOP)
        return -EINVAL;
    vma_t *vma = vma_alloc(vm, va, size,
                  PROT_READ | PROT_WRITE | VMA_FLAG_USER | VMA_FLAG_GROWSUP);
    if (vma == NULL)
        return -ENOMEM;
    vm->heap = vma;
    vm->heap_size = size;
    return 0;
}

int vm_createstack(vm_t *vm, uint64 stack_top, uint64 size)
{
    assert(current == NULL || rwsem_is_write_holding(&vm->rw_lock),
           "vm_createstack: vm rwsem must be write-held");
    size = PGROUNDUP(size);
    if ((stack_top & (PGSIZE - 1)) != 0)
        return -EINVAL;
    if (stack_top < size || stack_top > UVMTOP)
        return -EINVAL;
    vma_t *vma = vma_alloc(vm, stack_top - size, size,
                  PROT_READ | PROT_WRITE | VMA_FLAG_USER | VMA_FLAG_GROWSDOWN);
    if (vma == NULL)
        return -ENOMEM;
    vm->stack = vma;
    vm->stack_size = size;
    return 0;
}

int vm_growstack(vm_t *vm, int64 change_size)
{
    assert(current == NULL || rwsem_is_write_holding(&vm->rw_lock),
           "vm_growstack: vm rwsem must be write-held");
    if (vm == NULL || vm->pagetable == NULL)
        return -EINVAL;
    if (vm->stack == NULL || vm->stack_size < PGSIZE)
        return -EINVAL;
    if ((vm->stack->flags & VMA_FLAG_GROWSDOWN) == 0)
        return -EINVAL;
    if (change_size == 0)
        return 0;

    if (change_size < 0 && -change_size > vm->stack_size - PGSIZE)
        return -EINVAL;
    else if ((uint64)change_size > (MAXUSTACK << PGSHIFT) - vm->stack_size)
        return -ENOMEM;

    uint64 new_size = vm->stack_size + change_size;
    if (new_size < PGSIZE || new_size > (MAXUSTACK << PGSHIFT))
        return -EINVAL;

    int64 delta = PGROUNDUP(new_size) - PGROUNDUP(vm->stack_size);
    if (delta == 0) {
        vm->stack_size = new_size;
        return 0;
    }

    vma_t *left = __get_vma_left(vm->stack);
    uint64 new_start = vm->stack->start - delta;

    if (delta < 0) {
        vma_t *splitted = vm->stack;
        vma_t *right = vma_split(vm->stack, new_start);
        if (right == NULL)
            return -ENOMEM;
        vm->stack = right;
        __vma_set_free(splitted);
        if (left != NULL && left->flags == PROT_NONE)
            vma_merge(splitted, left);
    } else {
        if (left == NULL || left->flags != PROT_NONE)
            return -ENOMEM;
        if (VMA_SIZE(left) < delta)
            return -ENOMEM;
        vma_t *grows = vma_split(left, new_start);
        if (grows == NULL)
            return -ENOMEM;
        list_entry_detach(&grows->free_list_entry);
        grows->flags = vm->stack->flags;
        vma_t *new_stack = vma_merge(grows, vm->stack);
        vm->stack = new_stack;
    }
    vm->stack_size = new_size;
    return 0;
}

int vm_try_growstack(vm_t *vm, uint64 va)
{
    int ret = 0;
    vm_wlock(vm);
    if (vm == NULL || vm->pagetable == NULL) {
        ret = -EINVAL;
        goto out;
    }
    if (va < USTACK_MAX_BOTTOM || va >= USTACKTOP) {
        ret = 0;
        goto out;
    }
    if (vm->stack == NULL) {
        ret = -EINVAL;
        goto out;
    }
    if (vm->stack->start <= va) {
        ret = 0;
        goto out;
    }

    uint64 ustack_bottom_after =
        vm->stack->start - (USERSTACK_GROWTH << PAGE_SHIFT);
    if (ustack_bottom_after < USTACK_MAX_BOTTOM)
        ustack_bottom_after = USTACK_MAX_BOTTOM;
    if (ustack_bottom_after > va) {
        ret = -EFAULT;
        goto out;
    }

    uint64 needed = vm->stack->start - PGROUNDDOWN(va);
    uint64 growth = PGROUNDUP(needed);
    uint64 growth_unit = USERSTACK_GROWTH << PAGE_SHIFT;
    growth = ((growth + growth_unit - 1) / growth_unit) * growth_unit;

    ret = vm_growstack(vm, growth);
out:
    vm_wunlock(vm);
    return ret;
}

int vm_growheap(vm_t *vm, int64 change_size)
{
    int ret = 0;
    vm_wlock(vm);
    if (vm == NULL || vm->pagetable == NULL) {
        ret = -EINVAL;
        goto ret;
    }
    if (vm->heap == NULL || vm->heap_size < PGSIZE) {
        ret = -EINVAL;
        goto ret;
    }
    if ((vm->heap->flags & VMA_FLAG_GROWSUP) == 0) {
        ret = -EINVAL;
        goto ret;
    }
    if (change_size == 0) {
        ret = 0;
        goto ret;
    }

    if (change_size < 0) {
        if (-change_size > vm->heap_size - PGSIZE) {
            ret = -EINVAL;
            goto ret;
        }
    } else if (change_size > UHEAP_MAX_TOP - vm->heap->end) {
        ret = -ENOMEM;
        goto ret;
    }

    uint64 new_size = vm->heap_size + change_size;
    int64 delta = PGROUNDUP(new_size) - VMA_SIZE(vm->heap);
    if (delta == 0) {
        vm->heap_size = new_size;
        ret = 0;
        goto ret;
    }
    uint64 new_end = vm->heap->end + delta;
    vma_t *right = __get_vma_right(vm->heap);

    if (delta < 0) {
        vma_t *splitted = vma_split(vm->heap, new_end);
        if (splitted == NULL) {
            ret = -ENOMEM;
            goto ret;
        }
        __vma_set_free(splitted);
        if (right != NULL && right->flags == PROT_NONE)
            vma_merge(splitted, right);
    } else {
        if (right == NULL || right->flags != PROT_NONE) {
            printf("vm_growheap: FAIL no free right neighbor for heap "
                   "[0x%lx,0x%lx) right=%p right_flags=0x%lx delta=%ld\n",
                   vm->heap->start, vm->heap->end,
                   right, right ? right->flags : 0xdead, delta);
            ret = -ENOMEM;
            goto ret;
        }
        if (VMA_SIZE(right) < delta) {
            ret = -ENOMEM;
            goto ret;
        }
        if (vma_split(right, new_end) == NULL) {
            ret = -ENOMEM;
            goto ret;
        }
        list_entry_detach(&right->free_list_entry);
        right->flags = vm->heap->flags;
        vma_t *new_heap = vma_merge(right, vm->heap);
        vm->heap = new_heap;
    }
    vm->heap_size = new_size;
ret:
    vm_wunlock(vm);
    return ret;
}

/* ========================================================================== */
/*  mmap / munmap / mprotect / mremap / msync / mincore / madvise             */
/* ========================================================================== */

int vm_mmap_region_locked(vm_t *vm, uint64 start, size_t size, uint64 flags,
                          struct vfs_file *file, uint64 pgoff, void *pa)
{
    if (vm == NULL || vm->pagetable == NULL)
        return -EINVAL;

    uint64 va_end = PGROUNDUP(start + size);
    start = PGROUNDDOWN(start);
    if (va_end <= start || start < UVMBOTTOM || va_end > UVMTOP)
        return -EINVAL;
    size = va_end - start;

    vma_t *vma = vma_alloc(vm, start, size, flags);
    if (vma == NULL)
        return -ENOMEM;

    if (file != NULL) {
        vma->file = vfs_fdup(file);
        if (vma->file == NULL) {
            vma_free(vm, vma);
            return -EBADF;
        }
        vma->pgoff = pgoff;
    }

    if (pa != NULL) {
        pte_t pte_flags = vma2pte_flags(flags);
        if (mappages(vm->pagetable, vma->start, size, (uint64)pa, pte_flags) !=
            0) {
            assert(vma_free(vm, vma) == 0,
                   "vm_mmap_region_locked: failed to free vma");
            return -ENOMEM;
        }
    }
    return 0;
}

int vm_mmap_region(vm_t *vm, uint64 start, size_t size, uint64 flags,
                   struct vfs_file *file, uint64 pgoff, void *pa)
{
    int ret = 0;
    if (current != NULL)
        vm_wlock(vm);
    ret = vm_mmap_region_locked(vm, start, size, flags, file, pgoff, pa);
    if (current != NULL)
        vm_wunlock(vm);
    return ret;
}

int vm_munmap_region(vm_t *vm, uint64 start, size_t size)
{
    int ret = 0;
    vm_wlock(vm);
    if (vm == NULL || vm->pagetable == NULL) {
        ret = -EINVAL;
        goto out;
    }
    if (start < UVMBOTTOM || (start + size) > UVMTOP) {
        ret = -EINVAL;
        goto out;
    }
    if ((size & (PGSIZE - 1)) != 0 || (start & (PGSIZE - 1)) != 0) {
        ret = -EINVAL;
        goto out;
    }
    if (size == 0) {
        ret = 0;
        goto out;
    }

    vma_t *vma = vm_find_area(vm, start);
    if (vma == NULL || vma->start != start || vma->end < start + size) {
        ret = -EINVAL;
        goto out;
    }
    if (vma_free(vm, vma) != 0) {
        ret = -EINVAL;
        goto out;
    }
    ret = 0;
out:
    vm_wunlock(vm);
    return ret;
}

int vm_mprotect(vm_t *vm, uint64 addr, size_t size, int prot)
{
    int ret = 0;
    vm_wlock(vm);

    if (vm == NULL || vm->pagetable == NULL) {
        ret = -EINVAL;
        goto out;
    }

    addr = PGROUNDDOWN(addr);
    size = PGROUNDUP(size);

    if (addr < UVMBOTTOM || (addr + size) > UVMTOP) {
        ret = -ENOMEM;
        goto out;
    }
    if (size == 0) {
        ret = 0;
        goto out;
    }

    vma_t *vma = vm_find_area(vm, addr);
    if (vma == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    if (addr < vma->start || (addr + size) > vma->end) {
        ret = -ENOMEM;
        goto out;
    }

    if (addr > vma->start) {
        vma = vma_split(vma, addr);
        if (vma == NULL) {
            ret = -ENOMEM;
            goto out;
        }
    }

    uint64 end = addr + size;
    if (end < vma->end) {
        if (vma_split(vma, end) == NULL) {
            ret = -ENOMEM;
            goto out;
        }
    }

    uint64 old_flags = vma->flags;
    uint64 new_flags = (vma->flags & ~PROT_MASK);
    new_flags |= prot & PROT_MASK;

    uint64 pte_flags = vma2pte_flags(new_flags);

    vm_pgtable_lock(vm);
    for (uint64 va = addr; va < addr + size; va += PGSIZE) {
        pte_t *pte = walk(vm->pagetable, va, 0, NULL, NULL);
        if (pte != NULL && (*pte & PTE_V)) {
            uint64 pa = PTE2PA(*pte);
            *pte = PA2PTE(pa) | pte_flags | PTE_V | PTE_A | PTE_D;
        }
    }
    vm_pgtable_unlock(vm);

    vma->flags = new_flags;

    uint64 prot_bits = PROT_READ | PROT_WRITE | PROT_EXEC;
    if ((old_flags & prot_bits) & ~(new_flags & prot_bits))
        vm_remote_sfence(vm);

    ret = 0;
out:
    vm_wunlock(vm);
    return ret;
}

uint64 vm_mremap(vm_t *vm, uint64 old_addr, size_t old_size, size_t new_size,
                 int flags, uint64 new_addr)
{
    uint64 ret = (uint64)-1;
    vm_wlock(vm);

    if (vm == NULL || vm->pagetable == NULL)
        goto out;

    old_addr = PGROUNDDOWN(old_addr);
    old_size = PGROUNDUP(old_size);
    new_size = PGROUNDUP(new_size);

    if (old_size > (UVMTOP - UVMBOTTOM) || new_size > (UVMTOP - UVMBOTTOM))
        goto out;
    if (old_addr + old_size < old_addr)
        goto out;
    if (old_addr < UVMBOTTOM || (old_addr + old_size) > UVMTOP)
        goto out;

    vma_t *vma = vm_find_area(vm, old_addr);
    if (vma == NULL || vma->start != old_addr ||
        vma->end != old_addr + old_size)
        goto out;

    if (new_size == 0) {
        if (vma_free(vm, vma) != 0)
            goto out;
        ret = old_addr;
        goto out;
    }
    if (new_size == old_size && !(flags & MREMAP_FIXED)) {
        ret = old_addr;
        goto out;
    }
    if (new_size < old_size) {
        uint64 shrink_start = old_addr + new_size;
        vma_t *tail = vma_split(vma, shrink_start);
        if (tail == NULL)
            goto out;
        if (vma_free(vm, tail) != 0)
            goto out;
        vm_remote_sfence(vm);
        ret = old_addr;
        goto out;
    }

    /* Expanding */
    uint64 expand_size = new_size - old_size;
    uint64 expand_start = old_addr + old_size;

    vma_t *next_vma = vm_find_area(vm, expand_start);
    if (next_vma == NULL || next_vma->start >= expand_start + expand_size) {
        vma->end = old_addr + new_size;
        ret = old_addr;
        goto out;
    }

    if (!(flags & MREMAP_MAYMOVE))
        goto out;

    uint64 new_location = vm_find_free_range(vm, new_size, 0);
    if (new_location == 0)
        goto out;

    vma_t *new_vma = vma_alloc(vm, new_location, new_size, vma->flags);
    if (new_vma == NULL)
        goto out;

    for (uint64 offset = 0; offset < old_size; offset += PGSIZE) {
        pte_t *old_pte = walk(vm->pagetable, old_addr + offset, 0, NULL, NULL);
        if (old_pte != NULL && (*old_pte & PTE_V)) {
            uint64 pa = PTE2PA(*old_pte);
            uint64 pte_flags =
                *old_pte & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);

            pte_t *new_pte =
                walk(vm->pagetable, new_location + offset, 1, NULL, NULL);
            if (new_pte == NULL)
                goto out;
            *new_pte = PA2PTE(pa) | pte_flags | PTE_V;
            page_ref_inc((void *)pa);
            *old_pte = 0;
        }
    }

    vma_free(vm, vma);
    vm_remote_sfence(vm);

    if (new_vma->flags & PROT_EXEC)
        vm_remote_fence_i(vm);

    ret = new_location;
out:
    vm_wunlock(vm);
    return ret;
}

int vm_msync(vm_t *vm, uint64 addr, size_t size, int flags)
{
    int ret = 0;
    vm_rlock(vm);

    if (vm == NULL || vm->pagetable == NULL) {
        ret = -EINVAL;
        goto out;
    }

    addr = PGROUNDDOWN(addr);
    size = PGROUNDUP(size);

    if (addr < UVMBOTTOM || (addr + size) > UVMTOP) {
        ret = -ENOMEM;
        goto out;
    }

    vma_t *vma = vm_find_area(vm, addr);
    if (vma == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    if (addr < vma->start || (addr + size) > vma->end) {
        ret = -ENOMEM;
        goto out;
    }

    if (vma->file != NULL)
        __sync_synchronize();

    ret = 0;
out:
    vm_runlock(vm);
    return ret;
}

int vm_mincore(vm_t *vm, uint64 addr, size_t size, unsigned char *vec)
{
    int ret = 0;
    vm_rlock(vm);

    if (vm == NULL || vm->pagetable == NULL || vec == NULL) {
        ret = -EINVAL;
        goto out;
    }
    if ((addr & (PGSIZE - 1)) != 0) {
        ret = -EINVAL;
        goto out;
    }

    size = PGROUNDUP(size);

    if (addr < UVMBOTTOM || (addr + size) > UVMTOP) {
        ret = -ENOMEM;
        goto out;
    }

    vma_t *vma = vm_find_area(vm, addr);
    if (vma == NULL) {
        ret = -ENOMEM;
        goto out;
    }

    size_t num_pages = size / PGSIZE;
    vm_pgtable_lock(vm);
    for (size_t i = 0; i < num_pages; i++) {
        uint64 va = addr + (i * PGSIZE);
        vec[i] = 0;
        if (va < vma->start || va >= vma->end) {
            vma = vm_find_area(vm, va);
            if (vma == NULL)
                continue;
        }
        pte_t *pte = walk(vm->pagetable, va, 0, NULL, NULL);
        if (pte != NULL && (*pte & PTE_V))
            vec[i] = 1;
    }
    vm_pgtable_unlock(vm);

    ret = 0;
out:
    vm_runlock(vm);
    return ret;
}

int vm_madvise(vm_t *vm, uint64 addr, size_t size, int advice)
{
    int ret = 0;
    vm_wlock(vm);

    if (vm == NULL || vm->pagetable == NULL) {
        ret = -EINVAL;
        goto out;
    }

    addr = PGROUNDDOWN(addr);
    size = PGROUNDUP(size);

    if (addr < UVMBOTTOM || (addr + size) > UVMTOP) {
        ret = -ENOMEM;
        goto out;
    }

    vma_t *vma = vm_find_area(vm, addr);
    if (vma == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    if (addr < vma->start || (addr + size) > vma->end) {
        ret = -ENOMEM;
        goto out;
    }

    switch (advice) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
        ret = 0;
        break;

    case MADV_DONTNEED:
        vm_pgtable_lock(vm);
        for (uint64 va = addr; va < addr + size; va += PGSIZE) {
            pte_t *pte = walk(vm->pagetable, va, 0, NULL, NULL);
            if (pte != NULL && (*pte & PTE_V)) {
                uint64 pa = PTE2PA(*pte);
                if (pa != 0)
                    page_ref_dec((void *)pa);
                *pte = 0;
            }
        }
        vm_pgtable_unlock(vm);
        vm_remote_sfence(vm);
        ret = 0;
        break;

    default:
        ret = -EINVAL;
        break;
    }

out:
    vm_wunlock(vm);
    return ret;
}

/* ========================================================================== */
/*  Thread stack support (pthreads)                                           */
/* ========================================================================== */

uint64 vm_find_free_range(vm_t *vm, size_t size, uint64 hint)
{
    if (vm == NULL || size == 0)
        return 0;

    size = PGROUNDUP(size);

    uint64 stack_bottom = vm->stack ? vm->stack->start : UVMTOP - PGSIZE;
    uint64 heap_end = vm->heap ? (vm->heap->start + vm->heap_size) : UVMBOTTOM;
    uint64 reserve_end = vm->heap_reserve_end;
    uint64 effective_bottom = (reserve_end > heap_end) ? reserve_end : heap_end;

    uint64 search_top = stack_bottom - (16 * PGSIZE);
    uint64 search_bottom = effective_bottom;

    if (search_top <= search_bottom + size)
        return 0;

    vma_t *free_area, *tmp;
    list_foreach_node_inv_safe(&vm->vm_free_list, free_area, tmp,
                               free_list_entry) {
        if (free_area->flags != PROT_NONE)
            continue;

        uint64 usable_start = free_area->start;
        uint64 usable_end = free_area->end;

        if (usable_start < search_bottom)
            usable_start = search_bottom;
        if (usable_end > search_top)
            usable_end = search_top;

        if (usable_end > usable_start && usable_end - usable_start >= size) {
            uint64 result = PGROUNDDOWN(usable_end - size);
            if (result >= usable_start)
                return result;
        }
    }
    return 0;
}

int vm_alloc_thread_stack(vm_t *vm, size_t stack_size, uint64 *stack_top_out)
{
    if (vm == NULL || stack_top_out == NULL)
        return -EINVAL;

    if (stack_size < USERSTACK_MINSZ)
        stack_size = USERSTACK_MINSZ;
    stack_size = PGROUNDUP(stack_size);

    size_t total_size = stack_size + PGSIZE;

    vm_wlock(vm);

    uint64 stack_base = vm_find_free_range(vm, total_size, 0);
    if (stack_base == 0) {
        vm_wunlock(vm);
        return -ENOMEM;
    }

    uint64 guard_page = stack_base;
    vma_t *guard_vma = vma_alloc(vm, guard_page, PGSIZE, VMA_FLAG_GROWSDOWN);
    if (guard_vma == NULL) {
        vm_wunlock(vm);
        return -ENOMEM;
    }

    uint64 usable_stack_base = guard_page + PGSIZE;
    vma_t *stack_vma =
        vma_alloc(vm, usable_stack_base, stack_size,
                  VMA_FLAG_USER | PROT_READ | PROT_WRITE | VMA_FLAG_GROWSDOWN);
    if (stack_vma == NULL) {
        vma_free(vm, guard_vma);
        vm_wunlock(vm);
        return -ENOMEM;
    }

    uint64 stack_top = usable_stack_base + stack_size;
    vm_wunlock(vm);

    *stack_top_out = stack_top;
    return 0;
}

int vm_free_thread_stack(vm_t *vm, uint64 stack_top, size_t stack_size)
{
    if (vm == NULL || stack_top == 0)
        return -EINVAL;

    stack_size = PGROUNDUP(stack_size);
    if (stack_size < USERSTACK_MINSZ)
        stack_size = USERSTACK_MINSZ;

    uint64 usable_stack_base = stack_top - stack_size;
    uint64 guard_page = usable_stack_base - PGSIZE;

    vm_wlock(vm);

    vma_t *stack_vma = vm_find_area(vm, usable_stack_base);
    if (stack_vma != NULL)
        vma_free(vm, stack_vma);

    vma_t *guard_vma = vm_find_area(vm, guard_page);
    if (guard_vma != NULL)
        vma_free(vm, guard_vma);

    vm_wunlock(vm);
    return 0;
}

/* ========================================================================== */
/*  vm_mmap / vm_munmap wrappers                                              */
/* ========================================================================== */

static int __vm_unmap_range_locked(vm_t *vm, uint64 start, uint64 end)
{
    while (start < end) {
        vma_t *vma = vm_find_area(vm, start);
        if (vma == NULL)
            break;
        if (vma->flags == PROT_NONE) {
            start = vma->end;
            continue;
        }
        if (vma->start < start) {
            vma_t *right = vma_split(vma, start);
            if (right == NULL)
                return -ENOMEM;
            vma = right;
        }
        if (vma->end > end) {
            if (vma_split(vma, end) == NULL)
                return -ENOMEM;
        }
        uint64 next = vma->end;
        if (vma == vm->heap)
            vm->heap = NULL;
        if (vma == vm->stack)
            vm->stack = NULL;
        vma_free(vm, vma);
        start = next;
    }
    return 0;
}

uint64 vm_mmap(vm_t *vm, uint64 addr, size_t length, int prot, int flags,
               int fd, uint64 offset)
{
    struct vfs_file *file = NULL;

    if (vm == NULL || length == 0)
        return (uint64)-1;
    if (length > (UVMTOP - UVMBOTTOM))
        return (uint64)-1;
    if (length > ((size_t)-1) - (PGSIZE - 1))
        return (uint64)-1;

    if (!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED))
        return (uint64)-1;
    if ((flags & MAP_PRIVATE) && (flags & MAP_SHARED))
        return (uint64)-1;

    if (fd != -1) {
        if (flags & MAP_ANONYMOUS)
            return (uint64)-1;
        if (offset & (PGSIZE - 1))
            return (uint64)-1;
        file = vfs_fdtable_get_file(current->fdtable, fd);
        if (file == NULL)
            return (uint64)-1;
        struct vfs_inode *inode = vfs_inode_deref(&file->inode);
        if (inode == NULL || !S_ISREG(inode->mode)) {
            vfs_fput(file);
            return (uint64)-1;
        }
    } else if (!(flags & MAP_ANONYMOUS)) {
        return (uint64)-1;
    }

    length = PGROUNDUP(length);

    uint64 vm_flags =
        VMA_FLAG_USER | (prot & (PROT_READ | PROT_WRITE | PROT_EXEC));
    if (file != NULL)
        vm_flags |= VMA_FLAG_FILE;
    if (flags & MAP_SHARED)
        vm_flags |= VMA_FLAG_SHARED;

    vm_wlock(vm);

    uint64 map_addr;
    if (addr == 0 || (flags & MAP_FIXED) == 0) {
        map_addr = vm_find_free_range(vm, length, addr);
        if (map_addr == 0) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-1;
        }
    } else {
        map_addr = PGROUNDDOWN(addr);
        uint64 map_end = map_addr + length;
        if (__vm_unmap_range_locked(vm, map_addr, map_end) != 0) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-1;
        }
    }

    vma_t *vma = vma_alloc(vm, map_addr, length, vm_flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        if (file != NULL)
            vfs_fput(file);
        return (uint64)-1;
    }

    if (file != NULL) {
        vma->file = file;
        vma->pgoff = offset;
    }

    vm_wunlock(vm);
    return map_addr;
}

int vm_munmap(vm_t *vm, uint64 addr, size_t length)
{
    return vm_munmap_region(vm, addr, length);
}
