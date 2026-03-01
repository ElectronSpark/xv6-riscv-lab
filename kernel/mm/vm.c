/**
 * @file vm.c
 * @brief Shared virtual memory management.
 *
 * Architecture-independent process VM subsystem.  Contains:
 *  - VMA allocator & lifetime (slab-backed vma_t / vm_t pools).
 *  - Process address-space management (vm_init / vm_copy / vm_destroy).
 *  - Demand paging, COW handling (vma_validate).
 *  - Safe user copies (vm_copyin / vm_copyout / vm_copyinstr).
 *  - POSIX-style mmap / munmap / mprotect / mremap / msync / mincore / madvise.
 *  - Heap & stack growth helpers.
 *  - Thread-stack allocation for pthreads.
 *
 * Page-table walking, mapping, and raw PTE flag manipulation live in
 * kernel/mm/pgtable.c behind the abstract API declared in <mm/pgtable.h>.
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
#include "defs.h"
#include "arch/vm.h"
#include <mm/pgtable.h>
#include <mm/vm.h>
#include <mm/memlayout.h>
#include <mm/page.h>
#include <mm/slab.h>
#include <mm/pcache.h>
#include <mm/rmap.h>
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
    rmap_init();
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
    list_entry_init(&vma->anon_vma_chain);
    vma->anon_vma = NULL;
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
        anon_vma_unlink(vma);
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
            if (pte_nonleaf(pte))
                panic("__vma_set_free: not a leaf");
            if (!pte_present(pte))
                continue;
            uint64 pa = pte_pa(pte);
            pte_clear(pte);
            vm_remote_sfence(vma->vm);
            page_t *pg = __pa_to_page(pa);
            if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON))
                page_remove_rmap(pg);
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

    int is_shared = (src->flags & VMA_FLAG_SHARED) != 0;

    /* For non-shared (COW) VMAs, ensure the parent has an anon_vma before
     * we enter the PTE loop.  This is required so that:
     *  (a) page_add_anon_rmap can record the anon_vma on each page, and
     *  (b) the anon_vma_fork call below links the child to the parent's
     *      anon_vma chain (which requires src->anon_vma != NULL).
     * Must be done outside any spinlock — anon_vma_prepare acquires rwsem. */
    if (!is_shared && src->anon_vma == NULL) {
        int ret = anon_vma_prepare(src);
        if (ret != 0)
            return ret;
    }

    if (src->flags != PROT_NONE) {
        pagetable_t pgtb_src = src->vm->pagetable;
        pagetable_t pgtb_dst = dst->vm->pagetable;
        for (uint64 a = src->start; a < src->end; a += PGSIZE) {
            pte_t *src_pte = walk(pgtb_src, a, 0, NULL, NULL);
            if (src_pte == NULL || *src_pte == 0)
                continue;
            if (pte_nonleaf(src_pte))
                panic("__vma_copy: not a leaf");
            if (!pte_present(src_pte))
                continue;
            pte_t *new_pte = walk(pgtb_dst, a, 1, NULL, NULL);
            if (new_pte == NULL) {
                __vma_set_free(dst);
                return -ENOMEM;
            }
            if (is_shared) {
                *new_pte = *src_pte;
            } else {
                /* COW: clear write on both, no PTE_RSW_w needed —
                 * COW is detected via mapcount > 1 at fault time. */
                pte_wrprotect(src_pte);
                *new_pte = *src_pte;
            }
            uint64 pa = pte_pa(src_pte);
            assert(page_ref_inc((void *)pa) > 0,
                   "__vma_copy: page refcnt should be greater than 0");
            page_t *pg = __pa_to_page(pa);
            if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON) && !is_shared) {
                /* Bump mapcount for BOTH parent and child mappings.
                 * The parent's pages (e.g. from exec→mappages) may
                 * never have had page_add_anon_rmap called, so their
                 * mapcount could still be 0.  We must account for both
                 * so that the COW check (mapcount > 1) fires correctly
                 * when either process writes. */
                if (page_mapcount(pg) == 0)
                    page_add_anon_rmap(pg, src, a);
                page_add_anon_rmap(pg, dst, a);
            }
        }
        vm_remote_sfence(src->vm);
    }

    /* Set up anon_vma chain for the child VMA. */
    if (src->anon_vma != NULL && !is_shared) {
        int ret = anon_vma_fork(dst, src);
        if (ret != 0) {
            __vma_set_free(dst);
            return ret;
        }
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
    vm->vm_bottom = UVMBOTTOM;
    vm->vm_top = UVMTOP;
    vm->is_kernel = 0;
    return vm;
}

void vm_dup(vm_t *vm) { atomic_inc(&vm->refcount); }

void vm_put(vm_t *vm)
{
    if (vm == NULL)
        return;
    /* The kernel VM singleton must never be destroyed. */
    if (vm->is_kernel)
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

void vm_rlock(vm_t *vm) {
    if (vm->is_kernel)
        spin_lock(&vm->spinlock);
    else
        rwsem_acquire_read(&vm->rw_lock);
}
void vm_runlock(vm_t *vm) {
    if (vm->is_kernel)
        spin_unlock(&vm->spinlock);
    else
        rwsem_release(&vm->rw_lock);
}
void vm_wlock(vm_t *vm) {
    if (vm->is_kernel)
        spin_lock(&vm->spinlock);
    else
        rwsem_acquire_write(&vm->rw_lock);
}
void vm_wunlock(vm_t *vm) {
    if (vm->is_kernel)
        spin_unlock(&vm->spinlock);
    else
        rwsem_release(&vm->rw_lock);
}
int vm_is_wlocked(vm_t *vm) {
    if (vm->is_kernel)
        return spin_holding(&vm->spinlock);
    return rwsem_is_write_holding(&vm->rw_lock);
}
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
    if (va >= vm->vm_top || va < vm->vm_bottom)
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
    assert(current == NULL || vm_is_wlocked(vm),
           "vma_alloc: vm must be write-locked");
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
    assert(vm_is_wlocked(vm),
           "vma_free: vm must be write-locked");
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

static int __vma_validate_pte_rxw(vma_t *vma, pte_t *pte, uint64 fault_va)
{
    if (pte_write_ready(pte))
        return 0;

    void *addr = (void *)pte_pa(pte);
    void *pa = NULL;

    if (*pte == 0) {
        /* Demand-zero: allocate a fresh anonymous page. */
        if (anon_vma_prepare(vma) != 0)
            return -ENOMEM;
        pa = page_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
        if (pa == NULL)
            return -ENOMEM;
        memset(pa, 0, PGSIZE);
        page_t *pg = __pa_to_page((uint64)pa);
        page_add_anon_rmap(pg, vma, fault_va);
    } else if (pte_present(pte)) {
        /* Page present but not writable — check COW via mapcount. */
        page_t *old_page = __pa_to_page((uint64)addr);
        if (old_page != NULL && PAGE_IS_TYPE(old_page, PAGE_TYPE_ANON) &&
            page_mapcount(old_page) > 1) {
            /* COW: multiple mappers — must copy. */
            if (anon_vma_prepare(vma) != 0)
                return -ENOMEM;
            pa = page_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
            if (pa == NULL)
                return -ENOMEM;
            memmove(pa, addr, PGSIZE);
            page_remove_rmap(old_page);
            assert(page_ref_dec(addr) >= 0,
                   "vma_validate_pte_w: page_ref_dec failed for addr %p", addr);
            page_t *new_page = __pa_to_page((uint64)pa);
            page_add_anon_rmap(new_page, vma, fault_va);
        } else if (old_page != NULL && PAGE_IS_TYPE(old_page, PAGE_TYPE_ANON) &&
                   page_mapcount(old_page) == 1) {
            /* Single mapper fast path — just re-grant write, no copy. */
            pa = addr;
        } else {
            /* Non-anonymous or unmapped — just re-grant write. */
            pa = addr;
        }
    } else {
        return -EFAULT;
    }

    *pte = mk_pte((uint64)pa, vma->flags);
    sfence_vma_page(fault_va);
    return 0;
}

static int __vma_validate_pte_rx(vma_t *vma, pte_t *pte, uint64 fault_va)
{
    void *pa;

    if (*pte == 0) {
        /* Demand-zero: allocate with full VMA permissions. */
        if (anon_vma_prepare(vma) != 0)
            return -ENOMEM;
        pa = page_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
        if (pa == NULL)
            return -ENOMEM;
        memset(pa, 0, PGSIZE);
        page_t *pg = __pa_to_page((uint64)pa);
        page_add_anon_rmap(pg, vma, fault_va);
        *pte = mk_pte((uint64)pa, vma->flags);
    } else if (!pte_present(pte)) {
        return -EFAULT;
    } else {
        /* Present page, read/exec fault — rebuild PTE with VMA flags
         * but preserve the existing write-permission state (don't
         * re-grant write on a COW-shared page). */
        uint64 mflags = vma->flags;
        if (!pte_write(pte))
            mflags &= ~PROT_WRITE;
        pte_modify(pte, mflags);
    }

    sfence_vma_page(fault_va);
    return 0;
}

static int __vma_validate_pte(vma_t *vma, pte_t *pte, uint64 flags, uint64 fault_va)
{
    bool is_pte_user = pte_user(pte) != 0;
    bool is_vma_user = (flags & VMA_FLAG_USER) != 0;

    if (*pte != 0 && (is_pte_user ^ is_vma_user))
        return -EACCES;

    if ((flags & PROT_WRITE) && __vma_validate_pte_rxw(vma, pte, fault_va) != 0)
        return -EFAULT;
    else if ((flags & (PROT_READ | PROT_EXEC)) &&
             __vma_validate_pte_rx(vma, pte, fault_va) != 0)
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

    /* Pre-allocate anon_vma outside spinlock — rwsem cannot be acquired
     * while a spinlock is held.  For anonymous VMAs (demand-zero) and
     * writable file-backed VMAs (potential COW), prepare the anon_vma now
     * so that __vma_validate_pte_rxw / _rx find it already set (no-op). */
    if (vma->anon_vma == NULL && (vma->file == NULL || (flags & PROT_WRITE))) {
        if (anon_vma_prepare(vma) != 0)
            return -ENOMEM;
    }

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
                    *pte = mk_pte((uint64)pa, vma->flags);
                    sfence_vma_page(i);
                }
                continue;
            }
            if (__vma_validate_pte(vma, pte, flags, i) != 0) {
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
        if (__vma_validate_pte(vma, pte, flags, i) != 0) {
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

        pa0 = pte_pa(pte);
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
    assert(current == NULL || vm_is_wlocked(vm),
           "vm_createheap: vm must be write-locked");
    size = PGROUNDUP(size);
    if ((va & (PGSIZE - 1)) != 0)
        return -EINVAL;
    if (va >= vm->vm_top || va + size > vm->vm_top)
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
    assert(current == NULL || vm_is_wlocked(vm),
           "vm_createstack: vm must be write-locked");
    size = PGROUNDUP(size);
    if ((stack_top & (PGSIZE - 1)) != 0)
        return -EINVAL;
    if (stack_top < size || stack_top > vm->vm_top)
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
    assert(current == NULL || vm_is_wlocked(vm),
           "vm_growstack: vm must be write-locked");
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
    if (va_end <= start || start < vm->vm_bottom || va_end > vm->vm_top)
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
    if (start < vm->vm_bottom || (start + size) > vm->vm_top) {
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

    if (addr < vm->vm_bottom || (addr + size) > vm->vm_top) {
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

    /*
     * Compute effective VMA-level flags for PTE construction.
     * On x86, PTE_R == PTE_V (Present), so a Present+User page is always
     * readable.  For PROT_NONE, strip VMA_FLAG_USER so user-mode access
     * faults (supervisor-only page).  The PTE remains valid so that
     * __vma_set_free / __vma_copy still handle it correctly.
     */
    uint64 effective_flags = new_flags;
    if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0)
        effective_flags &= ~VMA_FLAG_USER;

    vm_pgtable_lock(vm);
    for (uint64 va = addr; va < addr + size; va += PGSIZE) {
        pte_t *pte = walk(vm->pagetable, va, 0, NULL, NULL);
        if (pte != NULL && pte_present(pte)) {
            uint64 mflags = effective_flags;
            /* rmap-based COW: if page has mapcount > 1 and we're trying
             * to grant write, suppress write — the write-fault handler
             * will resolve it via COW when the page is actually written. */
            if (mflags & PROT_WRITE) {
                uint64 pa = pte_pa(pte);
                page_t *pg = __pa_to_page(pa);
                if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON) &&
                    page_mapcount(pg) > 1) {
                    mflags &= ~PROT_WRITE;
                }
            }
            pte_modify(pte, mflags);
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

    if (old_size > (vm->vm_top - vm->vm_bottom) || new_size > (vm->vm_top - vm->vm_bottom))
        goto out;
    if (old_addr + old_size < old_addr)
        goto out;
    if (old_addr < vm->vm_bottom || (old_addr + old_size) > vm->vm_top)
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
        if (old_pte != NULL && pte_present(old_pte)) {
            uint64 pa = pte_pa(old_pte);

            pte_t *new_pte =
                walk(vm->pagetable, new_location + offset, 1, NULL, NULL);
            if (new_pte == NULL)
                goto out;
            /* Move the PTE: copy full value, then clear old. */
            *new_pte = *old_pte;
            page_ref_inc((void *)pa);
            /* Transfer rmap: add mapping for new VMA before old is freed. */
            page_t *pg = __pa_to_page(pa);
            if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON))
                page_add_anon_rmap(pg, new_vma, new_location + offset);
            pte_clear(old_pte);
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

    if (addr < vm->vm_bottom || (addr + size) > vm->vm_top) {
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

    if (addr < vm->vm_bottom || (addr + size) > vm->vm_top) {
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
        if (pte != NULL && pte_present(pte))
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

    if (addr < vm->vm_bottom || (addr + size) > vm->vm_top) {
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
            if (pte != NULL && pte_present(pte)) {
                uint64 pa = pte_pa(pte);
                if (pa != 0) {
                    page_t *pg = __pa_to_page(pa);
                    if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON))
                        page_remove_rmap(pg);
                    page_ref_dec((void *)pa);
                }
                pte_clear(pte);
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

    uint64 stack_bottom = vm->stack ? vm->stack->start : vm->vm_top - PGSIZE;
    uint64 heap_end = vm->heap ? (vm->heap->start + vm->heap_size) : vm->vm_bottom;
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
    if (length > (vm->vm_top - vm->vm_bottom))
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

/* ========================================================================== */
/*  Kernel VM singleton                                                       */
/* ========================================================================== */
/*
 * The kernel VM is a single vm_t shared by all CPUs and kernel threads.
 * It uses the boot-time kernel_pagetable and tracks kernel-land VMAs with
 * the same RB-tree / list infrastructure as user VMs.
 *
 * Key differences from user VMs:
 *   - Pages are ALWAYS eagerly allocated and mapped — no lazy faults, no COW.
 *   - The kernel VM is never destroyed (refcount is never decremented to 0).
 *   - Linear-mapped areas and the trampoline area remain intact; they are
 *     registered as fixed VMAs during boot so the VMA tree is aware of them.
 */

vm_t *kernel_vm = NULL;

/*
 * Static storage for the kernel vm_t so we can initialise it before the
 * slab allocator is ready to serve dynamic allocations.
 */
static vm_t __kernel_vm_storage;

void kernel_vm_init(void)
{
    extern pagetable_t kernel_pagetable;

    vm_t *vm = &__kernel_vm_storage;
    memset(vm, 0, sizeof(vm_t));

    rb_root_init(&vm->vm_tree, &__vm_tree_opts);
    list_entry_init(&vm->vm_list);
    list_entry_init(&vm->vm_free_list);

    /* Initial free VMA covering the kernel virtual address space. */
    vma_t *vma = __vma_alloc(vm);
    if (vma == NULL)
        panic("kernel_vm_init: cannot allocate initial VMA");

    vma->start = KVMBASE;
    vma->end = KVMTOP;
    rb_insert_color(&vm->vm_tree, &vma->rb_entry);
    list_node_push(&vm->vm_free_list, vma, free_list_entry);
    list_node_push(&vm->vm_list, vma, list_entry);

    vm->pagetable = kernel_pagetable;
    vm->trapframe_pte = NULL;     /* no per-process trapframes */
    vm->stack = NULL;
    vm->heap = NULL;

    spin_init(&vm->spinlock, "kernel_vm_pgtable_lock");
    rwsem_init(&vm->rw_lock, RWLOCK_PRIO_READ, "kernel_vm_rw_lock");
    vm->refcount = 1;             /* immortal */
    vm->vm_bottom = KVMBASE;
    vm->vm_top = KVMTOP;
    vm->is_kernel = 1;

    kernel_vm = vm;
    printf("kernel_vm_init: kernel VM at %p [%lx, %lx)\n",
           vm, vm->vm_bottom, vm->vm_top);
}

int kvm_register_region(uint64 start, uint64 size, uint64 flags)
{
    vm_t *vm = kernel_vm;
    if (vm == NULL)
        return -EINVAL;

    start = PGROUNDDOWN(start);
    size = PGROUNDUP(size);
    if (size == 0)
        return -EINVAL;

    /* Clamp to the kernel VM address range. */
    if (start < vm->vm_bottom)
        start = vm->vm_bottom;
    uint64 end = start + size;
    if (end > vm->vm_top)
        end = vm->vm_top;
    size = end - start;
    if (size == 0)
        return 0; /* entirely outside range, nothing to register */

    flags |= VMA_FLAG_KERNEL;
    flags &= ~VMA_FLAG_USER;

    vm_wlock(vm);
    vma_t *vma = vma_alloc(vm, start, size, flags);
    vm_wunlock(vm);

    if (vma == NULL) {
        printf("kvm_register_region: failed to register [%lx, %lx)\n",
               start, start + size);
        return -ENOMEM;
    }
    return 0;
}

uint64 kvm_mmap(uint64 addr, size_t size, uint64 flags)
{
    vm_t *vm = kernel_vm;
    if (vm == NULL)
        return 0;

    size = PGROUNDUP(size);
    if (size == 0)
        return 0;

    flags |= VMA_FLAG_KERNEL;
    flags &= ~VMA_FLAG_USER;

    vm_wlock(vm);

    /* Find or place the VMA. */
    uint64 map_addr;
    if (addr == 0) {
        map_addr = vm_find_free_range(vm, size, 0);
        if (map_addr == 0) {
            vm_wunlock(vm);
            return 0;
        }
    } else {
        map_addr = PGROUNDDOWN(addr);
    }

    vma_t *vma = vma_alloc(vm, map_addr, size, flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        return 0;
    }

    /* Eagerly allocate and map every page — no lazy allocation, no COW. */
    for (uint64 a = map_addr; a < map_addr + size; a += PGSIZE) {
        void *pa = page_alloc(0, PAGE_TYPE_ANON);
        if (pa == NULL) {
            /* Roll back: unmap and free pages we already mapped. */
            for (uint64 b = map_addr; b < a; b += PGSIZE) {
                pte_t *pte = walk(vm->pagetable, b, 0, NULL, NULL);
                if (pte && pte_present(pte)) {
                    uint64 old_pa = pte_pa(pte);
                    pte_clear(pte);
                    page_free((void *)old_pa, 0);
                }
            }
            vma_free(vm, vma);
            vm_wunlock(vm);
            return 0;
        }
        memset(pa, 0, PGSIZE);
        pte_t pte_flags = vma2pte_flags(flags);
        if (mappages(vm->pagetable, a, PGSIZE, (uint64)pa, pte_flags) != 0) {
            page_free(pa, 0);
            /* Roll back everything mapped so far. */
            for (uint64 b = map_addr; b < a; b += PGSIZE) {
                pte_t *pte = walk(vm->pagetable, b, 0, NULL, NULL);
                if (pte && pte_present(pte)) {
                    uint64 old_pa = pte_pa(pte);
                    pte_clear(pte);
                    page_free((void *)old_pa, 0);
                }
            }
            vma_free(vm, vma);
            vm_wunlock(vm);
            return 0;
        }
    }

    vm_wunlock(vm);
    return map_addr;
}

int kvm_munmap(uint64 addr, size_t size)
{
    vm_t *vm = kernel_vm;
    if (vm == NULL)
        return -EINVAL;

    addr = PGROUNDDOWN(addr);
    size = PGROUNDUP(size);
    if (size == 0)
        return -EINVAL;

    vm_wlock(vm);

    vma_t *vma = vm_find_area(vm, addr);
    if (vma == NULL || vma->start != addr || vma->end != addr + size) {
        vm_wunlock(vm);
        return -EINVAL;
    }

    /* Free the physical pages and clear PTEs. */
    for (uint64 a = addr; a < addr + size; a += PGSIZE) {
        pte_t *pte = walk(vm->pagetable, a, 0, NULL, NULL);
        if (pte == NULL || !pte_present(pte))
            continue;
        uint64 pa = pte_pa(pte);
        pte_clear(pte);
        page_free((void *)pa, 0);
    }

    vma_free(vm, vma);
    vm_wunlock(vm);

    /* Flush TLB on local CPU and send IPI to remote CPUs in cpumask. */
    arch_tlb_flush();
    vm_remote_sfence(kernel_vm);
    return 0;
}

void *kvm_alloc(size_t npages)
{
    if (npages == 0)
        return NULL;
    uint64 va = kvm_mmap(0, npages * PGSIZE, PROT_READ | PROT_WRITE);
    if (va == 0)
        return NULL;
    return (void *)va;
}

void kvm_free(void *addr, size_t npages)
{
    if (addr == NULL || npages == 0)
        return;
    kvm_munmap((uint64)addr, npages * PGSIZE);
}

/* ------------------------------------------------------------------ */
/*  kvmalloc / kvfree                                                  */
/* ------------------------------------------------------------------ */

/* Addresses in the identity-mapped physical-memory range [KERNBASE, PHYSTOP)
 * come from the slab/buddy allocator; everything else is from the kernel VM. */
int is_kvm_addr(const void *addr)
{
    uint64 a = (uint64)addr;
    return a < KERNBASE || a >= PHYSTOP;
}

void *kvmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* Small allocation: use the slab allocator. */
    if (size <= SLAB_OBJ_MAX_SIZE) {
        void *p = kmm_alloc(size);
        if (p != NULL)
            return p;
        /* slab OOM — fall through to page-granular path */
    }

    /* Large (or slab-failed) allocation: round up to pages. */
    size_t npages = (size + PGSIZE - 1) / PGSIZE;
    return kvm_alloc(npages);
}

void kvfree(void *ptr)
{
    if (ptr == NULL)
        return;

    if (!is_kvm_addr(ptr)) {
        /* Address is in the identity-mapped physical-memory range
         * → it was allocated by kmm_alloc (slab). */
        kmm_free(ptr);
        return;
    }

    /* Address is outside identity-mapped RAM → allocated by kvm_alloc.
     * Look up the covering VMA to determine the size. */
    vm_t *vm = kernel_vm;
    if (vm == NULL) {
        printf("kvfree: kernel_vm not initialised\n");
        return;
    }

    vm_wlock(vm);
    vma_t *vma = vm_find_area(vm, (uint64)ptr);
    if (vma == NULL || vma->start != (uint64)ptr) {
        vm_wunlock(vm);
        printf("kvfree: no VMA for %p\n", ptr);
        return;
    }
    size_t size = vma->end - vma->start;
    vm_wunlock(vm);

    kvm_munmap((uint64)ptr, size);
}

void dump_kernel_vm(void)
{
    if (kernel_vm == NULL) {
        printf("kernel_vm: not initialized\n");
        return;
    }
    printf("=== Kernel VM dump ===\n");
    dump_vm(kernel_vm);
    printf("=== End kernel VM dump ===\n");
}
