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
#include <mm/folio.h>
#include <smp/percpu.h>
#include <smp/atomic.h>
#include "list.h"
#include "lock/spinlock.h"
#include "lock/rwsem.h"
#include "maple_tree.h"
#include "string.h"
#include "printf.h"
#include "proc/thread.h"
#include "errno.h"
#include "kstats.h"
#include <vfs/file.h>
#include <vfs/fs.h>
#include <dev/bio.h>

#define VM_FILE_FAULT_AROUND_PAGES 16
#define VM_MMAP_EAGER_PAGES 32

/*
 * Default compound order for anonymous folio allocation during demand faults.
 * Order 2 = 4 pages = 16 KB.  The allocator falls back to smaller orders
 * (down to 0) when the VMA is too small or memory is tight.
 */
#define VM_ANON_FOLIO_ORDER 0

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
    maple_tree_init();
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
    uint64 idx = 0;
    void *entry;
    mt_for_each(&vm->vm_mt, entry, idx, MAPLE_MAX) {
        vma_t *vma = (vma_t *)entry;
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

/**
 * __vma_set_free() - Release all pages/PTEs owned by a VMA.
 *
 * Walks the VMA's page-table range, clears PTEs, and drops page
 * references.  Releases any held file reference and resets metadata.
 * Does NOT remove the VMA from the maple tree — callers must do that.
 */

/**
 * __vma_writeback_dirty_page - Write a dirty anonymous page back to its file.
 *
 * For MAP_SHARED file-backed VMAs whose fault handler returns anonymous
 * pages (e.g. ext4), dirty pages must be explicitly flushed to the
 * underlying file on munmap/exit.
 *
 * Uses the filesystem's writepage callback, which takes an explicit
 * file offset and does not touch file->f_pos.  This avoids races with
 * concurrent read/write syscalls on the same file descriptor.
 */
static void __vma_writeback_dirty_page(vma_t *vma, uint64 va, uint64 pa)
{
    struct vfs_file *file = vma->file;
    if (file->ops == NULL || file->ops->writepage == NULL)
        return;

    uint64 file_offset = vma->pgoff + (va - vma->start);

    /* Don't write beyond the current file size. */
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return;
    if (file_offset >= (uint64)inode->size)
        return;

    uint64 write_len = PGSIZE;
    if (file_offset + PGSIZE > (uint64)inode->size)
        write_len = (uint64)inode->size - file_offset;

    file->ops->writepage(file, (loff_t)file_offset, (const void *)pa,
                         write_len);
}

static void __vma_set_free(vma_t *vma)
{
    if (vma == NULL || vma->vm == NULL)
        return;

    if ((vma->start & (PGSIZE - 1)) != 0) {
        printf("__vma_set_free: BAD vma=%p start=0x%lx end=0x%lx "
               "flags=0x%lx file=%p pgoff=0x%lx vm=%p\n",
               vma, vma->start, vma->end, vma->flags,
               vma->file, vma->pgoff, vma->vm);
        if (vma->start == VMA_FREED_MAGIC && vma->end == VMA_FREED_MAGIC) {
            printf("  -> VMA already freed (FREED_MAGIC sentinel) — skipping\n");
            return;
        }
        panic("__vma_set_free: vma start not aligned");
    }

    /* Check if this is a shared file mapping that needs writeback. */
    int shared_file_wb =
        (vma->file != NULL) && (vma->flags & VMA_FLAG_SHARED) &&
        (vma->flags & VMA_FLAG_FILE);
    int tlb_needs_flush = 0;

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
            int was_dirty = pte_dirty(pte);
            pte_clear(pte);
            tlb_needs_flush = 1;

            /* Write dirty pages back for MAP_SHARED file mappings. */
            if (shared_file_wb && was_dirty)
                __vma_writeback_dirty_page(vma, a, pa);

            /* Release the page. */
            page_t *pg = __pa_to_page(pa);
            page_t *pc_head = page_pcache_head(pg);
            if (pc_head != NULL) {
                folio_t *folio = page_folio(pc_head);
                pcache_put_folio(pc_head->pcache.pcache, folio);
            } else {
                /* Only decrement mapcount for HEAD pages (PAGE_TYPE_ANON).
                 * Tail pages share the union with head_page — never touch
                 * anon.mapcount on them.  Each folio has exactly one head
                 * PTE so mapcount stays balanced (one dec per teardown). */
                if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON))
                    page_remove_rmap(pg);
                page_ref_dec((void *)pa);
            }
        }
        if (tlb_needs_flush)
            vm_remote_sfence(vma->vm);
    }

    if (vma->file != NULL)
        vfs_fput(vma->file);
    vma->file = NULL;
    vma->pgoff = 0;
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

    {
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
            /* Only bump mapcount for HEAD pages (PAGE_TYPE_ANON).
             * Tail pages must be skipped — their anon union stores
             * head_page, not mapcount.  Each folio has exactly one
             * head PTE, so mapcount stays balanced (one inc per fork
             * per folio). */
            if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON) && !is_shared) {
                /* The parent's folios (e.g. from exec→mappages) may
                 * never have had rmap set up, so mapcount could be 0.
                 * Bump for both parent and child so that COW check
                 * (mapcount > 1) fires correctly. */
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
/*  Maple tree helpers for VMA tree                                           */
/* ========================================================================== */

/**
 * __mt_store_vma() - Insert or replace a VMA in the maple tree.
 *
 * Stores @vma for the range [vma->start, vma->end - 1] (inclusive).
 * Returns 0 on success, negative errno on error.
 */
static inline int __mt_store_vma(vm_t *vm, vma_t *vma)
{
    int ret = mtree_store_range(&vm->vm_mt, vma->start, vma->end - 1, vma);
    return ret;
}

/**
 * __mt_erase_vma() - Remove a VMA from the maple tree.
 *
 * Erases (sets to NULL) the range [vma->start, vma->end - 1].
 */
static inline void __mt_erase_vma(vm_t *vm, vma_t *vma)
{
    /* Store NULL over the VMA's range to erase it. */
    mtree_store_range(&vm->vm_mt, vma->start, vma->end - 1, NULL);
}

/**
 * __mt_update_vma() - Atomically re-store a VMA whose range has changed.
 *
 * When a VMA's start/end is modified in-place (e.g. stack grow or merge),
 * the maple tree must be updated.  A plain __mt_store_vma can leave the
 * old range partially in the tree if the new range crosses a leaf boundary
 * and mtree_store_range fails to allocate a new internal node.  This leaves
 * the SAME VMA pointer in two disjoint ranges — a use-after-free when the
 * VM is later destroyed.
 *
 * This helper erases the old range first, then stores the new range.
 * On failure, the VMA and the tree are restored to the old state.
 *
 * @vm:        The VM owning the VMA.
 * @vma:       The VMA whose start/end have ALREADY been modified.
 * @old_start: The previous start address (before the caller modified vma).
 * @old_end:   The previous end address (before the caller modified vma).
 *
 * Returns 0 on success, negative errno on failure (VMA restored).
 */
static int __mt_update_vma(vm_t *vm, vma_t *vma, uint64 old_start,
                           uint64 old_end)
{
    /* Erase old range first (stores NULL, cannot fail for subset). */
    mtree_store_range(&vm->vm_mt, old_start, old_end - 1, NULL);

    /* Store VMA at its new range. */
    int ret = __mt_store_vma(vm, vma);
    if (ret != 0) {
        /* Roll back: restore VMA fields and re-store old range. */
        vma->start = old_start;
        vma->end = old_end;
        __mt_store_vma(vm, vma); /* best-effort restore */
    }
    return ret;
}

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
    mt_init(&vm->vm_mt);

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
        return;
    }

    /*
     * Single-pass destruction, Linux-style.
     *
     * Iterate the maple tree, release pages/PTEs and free each VMA
     * inline.  mtree_destroy() only frees internal tree nodes — it
     * never dereferences the entry pointers stored in leaf slots —
     * so it is safe to slab_free the VMAs during iteration.
     *
     * Duplicate detection: a prior partial mtree_store_range can leave
     * the same VMA pointer in two disjoint ranges.  After __vma_free
     * sets FREED_MAGIC, the second encounter is caught cheaply.
     */
    uint64 idx = 0;
    void *entry;
    mt_for_each(&vm->vm_mt, entry, idx, MAPLE_MAX) {
        vma_t *vma = (vma_t *)entry;

        /* Skip already-freed duplicates (FREED_MAGIC sentinel). */
        if (vma->start == VMA_FREED_MAGIC && vma->end == VMA_FREED_MAGIC)
            continue;

        __vma_set_free(vma);
        __vma_free(vma);
    }

    mtree_destroy(&vm->vm_mt);

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
    uint64 idx = 0;
    void *mt_entry;
    mt_for_each(&src->vm_mt, mt_entry, idx, MAPLE_MAX) {
        vma_t *vma = (vma_t *)mt_entry;
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
        if (__vma_copy(new_vma, vma) != 0) {
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
    return (vma_t *)mt_prev(&vma->vm->vm_mt, vma->start, 0);
}

static inline vma_t *__get_vma_right(vma_t *vma)
{
    if (vma == NULL || vma->vm == NULL)
        return NULL;
    return (vma_t *)mt_next(&vma->vm->vm_mt, vma->end - 1, MAPLE_MAX);
}

static vma_t *__vma_try_merge_neighbors(vma_t *vma)
{
    if (vma == NULL)
        return NULL;

    vma_t *left = __get_vma_left(vma);
    if (left != NULL) {
        vma_t *merged = vma_merge(left, vma);
        if (merged != NULL)
            vma = merged;
    }

    vma_t *right = __get_vma_right(vma);
    if (right != NULL) {
        vma_t *merged = vma_merge(vma, right);
        if (merged != NULL)
            vma = merged;
    }

    return vma;
}

vma_t *vm_find_area(vm_t *vm, uint64 va)
{
    if (va >= vm->vm_top || va < vm->vm_bottom)
        return NULL;
    vma_t *vma = (vma_t *)mtree_load(&vm->vm_mt, va);
    if (vma != NULL) {
        assert(VMA_IN_RANGE(vma, va),
               "vm_find_area: va %lx not in range [%lx, %lx)", va, vma->start,
               vma->end);
    }
    return vma;
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
    new_vma->end = vma->end;
    new_vma->flags = vma->flags;
    if (vma->file != NULL) {
        new_vma->file = vfs_fdup(vma->file);
        new_vma->pgoff = vma->pgoff + (va - vma->start);
    } else {
        new_vma->file = NULL;
        new_vma->pgoff = 0;
    }

    uint64 old_end = vma->end;
    vma->end = va;

    /* Update the maple tree: shrink old VMA, insert new VMA.
     * Both stores must succeed; otherwise roll back. */
    if (__mt_store_vma(vma->vm, vma) != 0) {
        vma->end = old_end;
        if (new_vma->file != NULL)
            vfs_fput(new_vma->file);
        __vma_free(new_vma);
        return NULL;
    }
    if (__mt_store_vma(vma->vm, new_vma) != 0) {
        /* Restore old VMA range and re-store it. */
        vma->end = old_end;
        __mt_store_vma(vma->vm, vma); /* best-effort restore */
        if (new_vma->file != NULL)
            vfs_fput(new_vma->file);
        __vma_free(new_vma);
        return NULL;
    }

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

    /* Erase vma2 from the tree first. */
    __mt_erase_vma(vma1->vm, vma2);

    /* Extend vma1 to cover vma2's range.
     * Use __mt_update_vma so a partial mtree_store_range cannot
     * duplicate vma1 across two leaf nodes. */
    uint64 old_end = vma1->end;
    vma1->end = vma2->end;

    if (__mt_update_vma(vma1->vm, vma1, vma1->start, old_end) != 0) {
        /* Roll back: restore vma1 and re-insert vma2. */
        vma1->end = old_end;
        __mt_store_vma(vma1->vm, vma1); /* best-effort */
        __mt_store_vma(vma1->vm, vma2); /* best-effort */
        return NULL;
    }

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
    if (vm == NULL) {
        printf("vma_alloc: FAIL vm==NULL\n");
        return NULL;
    }
    if (size == 0 || (size & (PGSIZE - 1)) != 0) {
        printf("vma_alloc: FAIL bad size=0x%lx\n", size);
        return NULL;
    }
    if (va != 0 && (va & (PGSIZE - 1)) != 0) {
        printf("vma_alloc: FAIL unaligned va=0x%lx\n", va);
        return NULL;
    }
    if ((flags & VMA_FLAG_PROT_MASK) == 0) {
        printf("vma_alloc: FAIL flags=0x%lx prot_mask=0\n", flags);
        return NULL;
    }

    if (va == 0) {
        /* Use maple tree gap search to find free space (low to high). */
        MA_STATE(mas, &vm->vm_mt, 0, 0);
        int rc = mas_empty_area(&mas, vm->vm_bottom, vm->vm_top - 1, size);
        if (rc != 0) {
            printf("vma_alloc: FAIL mas_empty_area rc=%d "
                   "bottom=0x%lx top=0x%lx size=0x%lx\n",
                   rc, vm->vm_bottom, vm->vm_top - 1, size);
            return NULL;
        }
        va = mas.index;
    } else {
        /* Fixed address: verify the range [va, va+size) is free (no
         * overlapping entries in the maple tree). */
        uint64 check = va;
        void *overlap = mt_find(&vm->vm_mt, &check, va + size - 1);
        if (overlap != NULL) {
            printf("vma_alloc: FAIL overlap at va=0x%lx size=0x%lx "
                   "overlap_vma=[0x%lx-0x%lx]\n",
                   va, size,
                   ((vma_t *)overlap)->start, ((vma_t *)overlap)->end);
            return NULL;
        }
    }

    uint64 va_end = va + size;
    if (va_end <= va || va < vm->vm_bottom || va_end > vm->vm_top) {
        printf("vma_alloc: FAIL bounds va=0x%lx va_end=0x%lx "
               "bottom=0x%lx top=0x%lx\n",
               va, va_end, vm->vm_bottom, vm->vm_top);
        return NULL;
    }

    vma_t *vma_new = __vma_alloc(vm);
    if (vma_new == NULL) {
        printf("vma_alloc: FAIL __vma_alloc returned NULL\n");
        return NULL;
    }

    vma_new->start = va;
    vma_new->end = va_end;
    vma_new->flags = flags;

    if (__mt_store_vma(vm, vma_new) != 0) {
        printf("vma_alloc: FAIL __mt_store_vma\n");
        __vma_free(vma_new);
        return NULL;
    }

    return vma_new;
}

int vma_free(vm_t *vm, vma_t *vma)
{
    assert(vm_is_wlocked(vm),
           "vma_free: vm must be write-locked");
    if (vma == NULL || vma->vm != vm)
        return -EINVAL;

    __vma_set_free(vma);
    __mt_erase_vma(vm, vma);
    __vma_free(vma);

    return 0;
}

/* ========================================================================== */
/*  Demand paging / COW                                                       */
/* ========================================================================== */

/*
 * __folio_order_for_vma - compute the largest folio order that fits in the
 * VMA around @fault_va.  The folio base address must be naturally aligned,
 * so we clamp both by VMA boundaries and by alignment.
 *
 * Returns an order in [0, VM_ANON_FOLIO_ORDER].
 */
static unsigned int __folio_order_for_vma(vma_t *vma, uint64 fault_va)
{
    for (int order = VM_ANON_FOLIO_ORDER; order > 0; order--) {
        uint64 folio_size = (uint64)PGSIZE << order;
        uint64 folio_base = fault_va & ~(folio_size - 1);
        uint64 folio_end = folio_base + folio_size;
        if (folio_base >= vma->start && folio_end <= vma->end)
            return (unsigned int)order;
    }
    return 0;
}

/*
 * __vma_map_anon_folio - allocate an anonymous folio and install PTEs for
 * every page in it.  The caller must hold vm_pgtable_lock.
 *
 * @vma:       the faulting VMA
 * @fault_va:  the page-aligned virtual address that caused the fault
 * @pte_flags: permission bits to set on every PTE
 *
 * Only pages whose PTEs are still zero are populated; races with
 * concurrent faults on the same folio range are handled gracefully by
 * skipping already-populated PTEs.
 *
 * Returns the physical address of the page corresponding to @fault_va,
 * or NULL on failure.  The folio's rmap is set up for the head page.
 */
static void *__vma_map_anon_folio(vma_t *vma, uint64 fault_va,
                                  uint64 pte_flags)
{
    unsigned int order = __folio_order_for_vma(vma, fault_va);

    /* Try the chosen order; fall back to smaller orders on OOM. */
    folio_t *folio = NULL;
    while (order > 0) {
        folio = folio_alloc(order, PAGE_TYPE_ANON | GFP_HIGHMEM);
        if (folio != NULL)
            break;
        order--;
    }
    if (folio == NULL) {
        folio = folio_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
        if (folio == NULL)
            return NULL;
    }

    uint64 folio_size = (uint64)PGSIZE << order;
    uint64 folio_base_va = fault_va & ~(folio_size - 1);
    uint64 folio_pa = folio_address(folio);

    /* Zero the entire folio. */
    memset((void *)folio_pa, 0, folio_size);

    /* Map every page of the folio and set up per-page rmap. */
    unsigned long nr_pages = folio_nr_pages(folio);
    void *fault_page_pa = NULL;
    for (unsigned long i = 0; i < nr_pages; i++) {
        uint64 va = folio_base_va + i * PGSIZE;
        uint64 pa = folio_pa + i * PGSIZE;
        if (va == fault_va)
            fault_page_pa = (void *)pa;

        pte_t *pte = walk(vma->vm->pagetable, va, 1, NULL, NULL);
        if (pte == NULL)
            continue; /* best-effort: we still own the folio */
        if (*pte != 0)
            continue; /* another thread (or fault-around) already mapped it */
        *pte = mk_pte(pa, pte_flags);
    }

    /* Record the mapping in the folio's head page.  Rmap is tracked
     * per-folio (on the head page) — never on tail pages, whose union
     * stores head_page and would be corrupted by writing mapcount. */
    folio_add_anon_rmap(folio, vma, fault_va);

    /* For order > 0 folios mapped to multiple PTEs we bump refcount so
     * that each PTE's eventual page_ref_dec still leaves the folio
     * alive until the last PTE is torn down.  The folio starts with
     * refcount 1 from folio_alloc.  We add (nr_pages - 1) refs. */
    for (unsigned long i = 1; i < nr_pages; i++)
        folio_get(folio);

    sfence_vma();
    return fault_page_pa;
}

static void *__vma_fault_file_page(vma_t *vma, uint64 va)
{
    struct vfs_file *file = vma->file;
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return NULL;
    struct pcache *pc = &inode->i_data;

    uint64 file_off = vma->pgoff + (va - vma->start);

    /*
     * Beyond EOF — return a zero anonymous page (no pcache mapping).
     */
    if (file_off >= (uint64)inode->size) {
        folio_t *folio = folio_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
        if (folio == NULL)
            return NULL;
        memset((void *)folio_address(folio), 0, PGSIZE);
        return (void *)folio_address(folio);
    }

    uint64 bytes_to_read = PGSIZE;
    if (file_off + PGSIZE > (uint64)inode->size)
        bytes_to_read = (uint64)inode->size - file_off;

    uint64 blkno_512 = file_off / BLK_SIZE;
    folio_t *pcfolio = pcache_get_folio(pc, blkno_512);
    if (pcfolio == NULL)
        return NULL;
    int ret = pcache_read_folio(pc, pcfolio);
    if (ret != 0) {
        pcache_put_folio(pc, pcfolio);
        return NULL;
    }

    /*
     * Zero-copy path: if the page is fully covered by file data, map the
     * pcache page directly into the user address space.  The pcache_get_folio
     * ref (refcount >= 2) keeps the page pinned in the cache (off LRU).
     * We do NOT call pcache_put_folio here — the user PTE now owns that ref.
     * __vma_set_free / munmap will call pcache_put_folio when the mapping
     * is torn down.
     *
     * Partial pages (last page of a file that doesn't fill PGSIZE) must
     * still be copied so the tail is zero-filled.
     */
    /* Compute the byte offset of the faulting page within the folio.
     * For multi-page folios the PTE must point to the correct sub-page,
     * not always the head. */
    page_t *pcpage = &pcfolio->page;
    struct pcache_node *pcn = pcpage->pcache.pcache_node;
    uint64 folio_byte_off = (uint64)blkno_512 * BLK_SIZE -
                            (uint64)pcn->blkno * BLK_SIZE;

    if (bytes_to_read == PGSIZE) {
        return (char *)pcn->data + folio_byte_off;
    }

    /* Partial page: fall back to copy so tail bytes are zeroed. */
    folio_t *anon_folio = folio_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
    if (anon_folio == NULL) {
        pcache_put_folio(pc, pcfolio);
        return NULL;
    }

    void *pa = (void *)folio_address(anon_folio);
    memmove(pa, (char *)pcn->data + folio_byte_off, bytes_to_read);
    memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);

    pcache_put_folio(pc, pcfolio);
    return pa;
}

static int __vma_validate_pte_rxw(vma_t *vma, pte_t *pte, uint64 fault_va)
{
    if (pte_write_ready(pte))
        return 0;

    /*
     * Fast path: writable PTE that only needs the dirty bit set.
     * This happens on RISC-V implementations that trap instead of
     * auto-setting D, and for MAP_SHARED pcache pages that were
     * intentionally mapped clean (D=0) so the first write lets us
     * propagate dirty state to the page cache.
     */
    if (pte_present(pte) && pte_write(pte) && !pte_dirty(pte)) {
        page_t *pg = __pa_to_page(pte_pa(pte));
        page_t *pc_head = page_pcache_head(pg);
        if (pc_head != NULL)
            pcache_mark_page_dirty(pc_head->pcache.pcache, pc_head);
        pte_mkdirty(pte);
        sfence_vma_page(fault_va);
        return 0;
    }

    void *addr = (void *)pte_pa(pte);
    void *pa = NULL;

    if (*pte == 0) {
        /* Demand-zero: allocate a large anonymous folio and map all
         * its pages into the VMA.  __vma_map_anon_folio handles
         * fallback to order-0 when memory is tight. */
        if (anon_vma_prepare(vma) != 0)
            return -ENOMEM;
        pa = __vma_map_anon_folio(vma, fault_va, vma->flags);
        if (pa == NULL)
            return -ENOMEM;
        /* PTEs and rmap already set up by __vma_map_anon_folio. */
        return 0;
    } else if (pte_present(pte)) {
        /* Page present but not writable — check COW via mapcount. */
        page_t *old_page = __pa_to_page((uint64)addr);
        /* Resolve tail→head so we can check PAGE_TYPE_ANON and
         * mapcount on the compound head (tails use the same union
         * bytes for head_page, so their anon.mapcount is invalid). */
        page_t *cow_head = old_page;
        if (old_page != NULL && PAGE_IS_TYPE(old_page, PAGE_TYPE_TAIL)
            && old_page->tail.head_page != NULL)
            cow_head = old_page->tail.head_page;
        page_t *pc_head = page_pcache_head(old_page);
        if (pc_head != NULL) {
            /* Zero-copy pcache page: ALWAYS COW — we must never
             * modify the page cache directly.  Copy to a fresh
             * anonymous folio and release the pcache mapping ref.
             * Propagate dirty from PTE before dropping it. */
            folio_t *old_folio = page_folio(pc_head);
            if (pte_dirty(pte))
                pcache_mark_page_dirty(pc_head->pcache.pcache, pc_head);
            if (anon_vma_prepare(vma) != 0)
                return -ENOMEM;
            folio_t *new_folio = folio_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
            if (new_folio == NULL)
                return -ENOMEM;
            pa = (void *)folio_address(new_folio);
            memmove(pa, addr, PGSIZE);
            pcache_put_folio(pc_head->pcache.pcache, old_folio);
            page_add_anon_rmap((page_t *)new_folio, vma, fault_va);
        } else if (cow_head != NULL && PAGE_IS_TYPE(cow_head, PAGE_TYPE_ANON) &&
            page_mapcount(cow_head) > 1) {
            /* COW: multiple mappers — copy the entire folio at once so
             * that subsequent write faults on sibling pages of the
             * same folio can take the single-mapper fast path. */
            folio_t *old_folio = page_folio(old_page);
            unsigned int order = folio_order(old_folio);
            if (anon_vma_prepare(vma) != 0)
                return -ENOMEM;
            folio_t *new_folio = folio_alloc(order, PAGE_TYPE_ANON | GFP_HIGHMEM);
            if (new_folio == NULL) {
                /* Fall back to single-page COW. */
                new_folio = folio_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
                if (new_folio == NULL)
                    return -ENOMEM;
                pa = (void *)folio_address(new_folio);
                memmove(pa, addr, PGSIZE);
                /* Single-page COW fallback: if we are replacing the
                 * HEAD page's PTE, decrement old folio's mapcount now
                 * because teardown only decrements rmap for HEAD PTEs
                 * and this PTE will point to the new folio instead.
                 * If replacing a tail page PTE, don't touch mapcount —
                 * the head PTE still points to the old folio. */
                if (old_page == cow_head)
                    page_remove_rmap(cow_head);
                page_ref_dec(addr);
                folio_add_anon_rmap(new_folio, vma, fault_va);
            } else {
                /* Whole-folio COW: copy all pages, remap PTEs. */
                uint64 old_folio_pa = folio_address(old_folio);
                uint64 new_folio_pa = folio_address(new_folio);
                unsigned long nr = folio_nr_pages(new_folio);
                memmove((void *)new_folio_pa, (void *)old_folio_pa,
                        nr * PGSIZE);

                /* Whole-folio COW: this process is replacing ALL
                 * pages of the old folio → update mapcount once at
                 * the folio level (head page only). */
                folio_remove_rmap(old_folio);
                folio_add_anon_rmap(new_folio, vma, fault_va);

                /* Walk the VMA range to find PTEs into the old folio.
                 * Swap each to the corresponding new folio page and
                 * adjust refcounts. */
                uint64 old_pa_base = old_folio_pa;
                uint64 old_pa_end = old_folio_pa + nr * PGSIZE;
                uint64 scan_start = vma->start;
                uint64 scan_end = vma->end;

                for (uint64 va = scan_start; va < scan_end; va += PGSIZE) {
                    pte_t *p = walk(vma->vm->pagetable, va, 0, NULL, NULL);
                    if (p == NULL || *p == 0 || !pte_present(p))
                        continue;
                    uint64 mapped_pa = pte_pa(p);
                    if (mapped_pa >= old_pa_base && mapped_pa < old_pa_end) {
                        uint64 page_off = mapped_pa - old_pa_base;
                        *p = mk_pte(new_folio_pa + page_off, vma->flags);
                        /* Drop old ref for this PTE. */
                        page_ref_dec((void *)mapped_pa);
                        /* Add new ref for this PTE. */
                        if (new_folio_pa + page_off !=
                            folio_address(new_folio))
                            folio_get(new_folio);
                    }
                }
                /* The fault_va PTE needs the new folio's page. */
                uint64 fault_off = (uint64)addr - old_pa_base;
                pa = (void *)(new_folio_pa + fault_off);
            }
        } else if (cow_head != NULL && PAGE_IS_TYPE(cow_head, PAGE_TYPE_ANON) &&
                   page_mapcount(cow_head) == 1) {
            /* Single mapper fast path — re-grant write to all pages
             * in the folio so sibling pages don't re-fault. */
            folio_t *old_folio = page_folio(old_page);
            unsigned int order = folio_order(old_folio);
            if (order > 0) {
                uint64 folio_pa = folio_address(old_folio);
                uint64 folio_pa_end = folio_pa + folio_nr_pages(old_folio) * PGSIZE;
                uint64 scan_start = vma->start;
                uint64 scan_end = vma->end;
                for (uint64 va = scan_start; va < scan_end; va += PGSIZE) {
                    pte_t *p = walk(vma->vm->pagetable, va, 0, NULL, NULL);
                    if (p == NULL || *p == 0 || !pte_present(p))
                        continue;
                    uint64 mapped_pa = pte_pa(p);
                    if (mapped_pa >= folio_pa && mapped_pa < folio_pa_end)
                        *p = mk_pte(mapped_pa, vma->flags);
                }
                sfence_vma();
                return 0;
            }
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
        /* Demand-zero: allocate a large anonymous folio. */
        if (anon_vma_prepare(vma) != 0)
            return -ENOMEM;
        pa = __vma_map_anon_folio(vma, fault_va, vma->flags);
        if (pa == NULL)
            return -ENOMEM;
        /* PTEs and rmap already set up by __vma_map_anon_folio. */
        sfence_vma_page(fault_va);
        return 0;
    } else if (!pte_present(pte)) {
        return -EFAULT;
    } else {
        /* Present page, read/exec fault — rebuild PTE with VMA flags
         * but preserve the existing write-permission state (don't
         * re-grant write on a COW-shared page). */
        uint64 mflags = vma->flags;
        if (!pte_write(pte))
            mflags &= ~PROT_WRITE;
        int old_dirty = pte_dirty(pte);
        pte_modify(pte, mflags);
        /* Keep pcache pages clean unless the PTE was already dirty. */
        page_t *pg = __pa_to_page(pte_pa(pte));
        if (page_is_pcache(pg) && !old_dirty)
            pte_mkclean(pte);
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
    uint64 validate_start = r_time();
    __atomic_fetch_add(&g_vm_vma_validate_calls, 1, __ATOMIC_RELAXED);

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

                uint64 fault_end = i + VM_FILE_FAULT_AROUND_PAGES * PGSIZE;
                if (fault_end > va_end)
                    fault_end = va_end;
                if (fault_end > vma->end)
                    fault_end = vma->end;

                if (vma->file->ops != NULL &&
                    vma->file->ops->prefault != NULL)
                    (void)vma->file->ops->prefault(vma->file, vma, i,
                                                   fault_end);

                for (uint64 fault_va = i; fault_va < fault_end;
                     fault_va += PGSIZE) {
                    vm_pgtable_lock(vma->vm);
                    pte = walk(vma->vm->pagetable, fault_va, 1, NULL, NULL);
                    if (pte == NULL) {
                        vm_pgtable_unlock(vma->vm);
                        return -ENOMEM;
                    }
                    if (*pte != 0) {
                        vm_pgtable_unlock(vma->vm);
                        continue;
                    }
                    vm_pgtable_unlock(vma->vm);

                    void *pa;
                    __atomic_fetch_add(&g_vm_file_faults, 1,
                                       __ATOMIC_RELAXED);
                    if (vma->file->ops != NULL && vma->file->ops->fault != NULL)
                        pa = vma->file->ops->fault(vma->file, vma, fault_va);
                    else
                        pa = __vma_fault_file_page(vma, fault_va);
                    if (pa == NULL)
                        return -ENOMEM;

                    page_t *fault_pg = __pa_to_page((uint64)pa);
                    page_t *fault_pc_head = page_pcache_head(fault_pg);
                    int is_pcache = (fault_pc_head != NULL);

                    vm_pgtable_lock(vma->vm);
                    pte = walk(vma->vm->pagetable, fault_va, 1, NULL, NULL);
                    if (pte == NULL) {
                        vm_pgtable_unlock(vma->vm);
                        if (is_pcache)
                            pcache_put_folio(fault_pc_head->pcache.pcache,
                                             page_folio(fault_pc_head));
                        else
                            page_free(pa, 0);
                        return -ENOMEM;
                    }
                    if (*pte != 0) {
                        if (is_pcache)
                            pcache_put_folio(fault_pc_head->pcache.pcache,
                                             page_folio(fault_pc_head));
                        else
                            page_free(pa, 0);
                    } else {
                        uint64 pte_flags = vma->flags;
                        if (is_pcache && !(vma->flags & VMA_FLAG_SHARED))
                            pte_flags &= ~PROT_WRITE;
                        *pte = mk_pte((uint64)pa, pte_flags);
                        if (is_pcache)
                            pte_mkclean(pte);
                        sfence_vma_page(fault_va);
                    }
                    vm_pgtable_unlock(vma->vm);
                }
                vm_pgtable_lock(vma->vm);
                i = fault_end - PGSIZE;
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
    __atomic_fetch_add(&g_vm_vma_validate_ticks, r_time() - validate_start,
                       __ATOMIC_RELAXED);
    return 0;
}

/* ========================================================================== */
/*  User memory accessors                                                     */
/* ========================================================================== */

int vm_copyout(vm_t *vm, uint64 dstva, const void *src, uint64 len)
{
    uint64 copy_start = r_time();
    __atomic_fetch_add(&g_vm_copyout_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_vm_copyout_bytes, len, __ATOMIC_RELAXED);

    uint64 n, va0, pa0;
    uint64 validated_end = 0;
    vma_t *vma = NULL;
    pte_t *pte;
    int ret = 0;

    vm_rlock(vm);
    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= UVMTOP) {
            ret = -EFAULT;
            goto out;
        }

        if (vma == NULL || dstva >= validated_end ||
            va0 < vma->start || va0 >= vma->end) {
            vma = vm_find_area(vm, va0);
            if (vma != NULL) {
                uint64 chunk_end = vma->end;
                uint64 req_end = PGROUNDUP(dstva + len);
                if (chunk_end > req_end)
                    chunk_end = req_end;
                validated_end = chunk_end;
            }
        }

        if (vma == NULL ||
            vma_validate(vma, va0, validated_end - va0,
                         VMA_FLAG_USER | PROT_WRITE) != 0) {
            if (va0 >= USTACK_MAX_BOTTOM && va0 < USTACKTOP) {
                vm_runlock(vm);
                if (vm_try_growstack(vm, va0) == 0) {
                    vm_rlock(vm);
                    vma = vm_find_area(vm, va0);
                    if (vma != NULL) {
                        uint64 chunk_end = vma->end;
                        uint64 req_end = PGROUNDUP(dstva + len);
                        if (chunk_end > req_end)
                            chunk_end = req_end;
                        validated_end = chunk_end;
                    }
                    if (vma != NULL &&
                        vma_validate(vma, va0, validated_end - va0,
                                     VMA_FLAG_USER | PROT_WRITE) == 0)
                        goto copyout_ok;
                    vm_runlock(vm);
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
        if (dstva + n > validated_end)
            n = validated_end - dstva;
        if (n > len)
            n = len;
        memmove((void *)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
out:
    __atomic_fetch_add(&g_vm_copyout_ticks, r_time() - copy_start,
                       __ATOMIC_RELAXED);
    vm_runlock(vm);
    return ret;
}

int vm_copyin(vm_t *vm, void *dst, uint64 srcva, uint64 len)
{
    uint64 copy_start = r_time();
    __atomic_fetch_add(&g_vm_copyin_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_vm_copyin_bytes, len, __ATOMIC_RELAXED);

    uint64 n, va0, pa0;
    uint64 validated_end = 0;
    vma_t *vma = NULL;
    int ret = 0;
    vm_rlock(vm);

    while (len > 0) {
        va0 = PGROUNDDOWN(srcva);
        if (vma == NULL || srcva >= validated_end ||
            va0 < vma->start || va0 >= vma->end) {
            vma = vm_find_area(vm, va0);
            if (vma != NULL) {
                uint64 chunk_end = vma->end;
                uint64 req_end = PGROUNDUP(srcva + len);
                if (chunk_end > req_end)
                    chunk_end = req_end;
                validated_end = chunk_end;
            }
        }

        if (vma == NULL ||
            vma_validate(vma, va0, validated_end - va0,
                         VMA_FLAG_USER | PROT_READ) != 0) {
            if (va0 >= USTACK_MAX_BOTTOM && va0 < USTACKTOP) {
                vm_runlock(vm);
                if (vm_try_growstack(vm, va0) == 0) {
                    vm_rlock(vm);
                    vma = vm_find_area(vm, va0);
                    if (vma != NULL) {
                        uint64 chunk_end = vma->end;
                        uint64 req_end = PGROUNDUP(srcva + len);
                        if (chunk_end > req_end)
                            chunk_end = req_end;
                        validated_end = chunk_end;
                    }
                    if (vma != NULL &&
                        vma_validate(vma, va0, validated_end - va0,
                                     VMA_FLAG_USER | PROT_READ) == 0)
                        goto copyin_ok;
                    vm_runlock(vm);
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
        if (srcva + n > validated_end)
            n = validated_end - srcva;
        if (n > len)
            n = len;
        memmove(dst, (void *)(pa0 + (srcva - va0)), n);

        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
out:
    __atomic_fetch_add(&g_vm_copyin_ticks, r_time() - copy_start,
                       __ATOMIC_RELAXED);
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

    if (delta < 0) {
        /* Shrink: release pages from the bottom of the stack.
         * delta is negative, so new_start = stack->start + |delta|. */
        uint64 new_start = vm->stack->start - delta;
        vma_t *upper = vma_split(vm->stack, new_start);
        if (upper == NULL)
            return -ENOMEM;
        /* vm->stack is now the lower portion to free. */
        vma_t *freed = vm->stack;
        vm->stack = upper;
        __vma_set_free(freed);
        __mt_erase_vma(vm, freed);
        __vma_free(freed);
    } else {
        /* Grow: extend the stack downward into the gap.
         * Verify no VMA exists in the expansion range. */
        uint64 new_start = vm->stack->start - delta;
        uint64 check = new_start;
        void *overlap = mt_find(&vm->vm_mt, &check, vm->stack->start - 1);
        if (overlap != NULL)
            return -ENOMEM;
        /* Extend the VMA start downward and update the tree.
         * Use __mt_update_vma to erase+reinsert atomically so a
         * partial mtree_store_range cannot duplicate the VMA pointer
         * across two leaf nodes. */
        uint64 old_start = vm->stack->start;
        vm->stack->start = new_start;
        if (__mt_update_vma(vm, vm->stack, old_start, vm->stack->end) != 0)
            return -ENOMEM;
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

    if (delta < 0) {
        /* Shrink: release pages from the top of the heap. */
        vma_t *splitted = vma_split(vm->heap, new_end);
        if (splitted == NULL) {
            ret = -ENOMEM;
            goto ret;
        }
        __vma_set_free(splitted);
        __mt_erase_vma(vm, splitted);
        __vma_free(splitted);
    } else {
        /* Grow: extend heap upward into the gap.
         * Verify no VMA exists in the expansion range. */
        uint64 check = vm->heap->end;
        void *overlap = mt_find(&vm->vm_mt, &check, new_end - 1);
        if (overlap != NULL) {
            ret = -ENOMEM;
            goto ret;
        }
        /* Extend the VMA end upward and update the tree.
         * Use __mt_update_vma to erase+reinsert atomically. */
        uint64 old_end = vm->heap->end;
        vm->heap->end = new_end;
        if (__mt_update_vma(vm, vm->heap, vm->heap->start, old_end) != 0) {
            ret = -ENOMEM;
            goto ret;
        }
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
             * will resolve it via COW when the page is actually written.
             * For pcache pages in MAP_PRIVATE VMAs, suppress write to
             * force COW.  MAP_SHARED pcache pages are writable but will
             * have D cleared below for dirty tracking. */
            if (mflags & PROT_WRITE) {
                uint64 pa = pte_pa(pte);
                page_t *pg = __pa_to_page(pa);
                if (page_is_pcache(pg) &&
                    !(vma->flags & VMA_FLAG_SHARED)) {
                    mflags &= ~PROT_WRITE;
                } else if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON) &&
                    page_mapcount(pg) > 1) {
                    mflags &= ~PROT_WRITE;
                }
            }
            /*
             * Capture PTE dirty state before pte_modify overwrites
             * the flags.  If the old PTE was dirty and the page is a
             * pcache page, propagate dirty to the page cache.
             */
            int old_dirty = pte_dirty(pte);
            pte_modify(pte, mflags);
            {
                uint64 pa = pte_pa(pte);
                page_t *pg = __pa_to_page(pa);
                page_t *pc_head = page_pcache_head(pg);
                if (pc_head != NULL) {
                    if (old_dirty)
                        pcache_mark_page_dirty(pc_head->pcache.pcache, pc_head);
                    pte_mkclean(pte);
                }
            }
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

    /* Check whether the expansion range is free (no overlapping VMAs). */
    uint64 check = expand_start;
    void *overlap = mt_find(&vm->vm_mt, &check, expand_start + expand_size - 1);
    if (overlap == NULL) {
        /* Gap is free — expand in place.
         * Use __mt_update_vma to erase+reinsert atomically. */
        uint64 old_end = vma->end;
        vma->end = old_addr + new_size;
        if (__mt_update_vma(vm, vma, vma->start, old_end) != 0)
            goto out;
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
            /* Move the PTE: copy full value, then clear old.
             * This is a MOVE, not a copy — the page reference count
             * stays the same because we clear the old PTE (preventing
             * vma_free from decrementing it) and install the new PTE
             * (which will be decremented when the new VMA is freed).
             * We must NOT call page_ref_inc here — doing so would leak
             * a reference because vma_free(old) finds the PTE already
             * cleared and skips cleanup.
             * Transfer rmap: remove from old VMA, add to new. */
            *new_pte = *old_pte;
            page_t *pg = __pa_to_page(pa);
            /* Only update rmap for HEAD pages (PAGE_TYPE_ANON).
             * Tail pages are skipped — their union stores head_page. */
            if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON)) {
                page_remove_rmap(pg);
                page_add_anon_rmap(pg, new_vma, new_location + offset);
            }
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

    /*
     * For MAP_SHARED file-backed VMAs, walk PTEs in the requested range
     * and write back any dirty pages.
     *
     * pcache pages:  propagate PTE dirty → pcache node dirty (the
     *                background flusher will do the actual I/O).
     * anonymous pages:  call writepage to push data into the FS.
     *
     * After the writeback loop, call fsync for MS_SYNC so the data
     * reaches stable storage before we return.
     */
    if (vma->file != NULL && (vma->flags & VMA_FLAG_SHARED) &&
        (vma->flags & VMA_FLAG_FILE)) {
        struct vfs_file *file = vma->file;

        vm_pgtable_lock(vm);
        for (uint64 va = addr; va < addr + size; va += PGSIZE) {
            pte_t *pte = walk(vm->pagetable, va, 0, NULL, NULL);
            if (pte == NULL || !pte_present(pte))
                continue;
            if (!pte_dirty(pte))
                continue;

            uint64 pa = pte_pa(pte);

            /* Writeback dirty page — must drop the pgtable spinlock
             * because writepage may sleep (inode lock, I/O).  Clear
             * the PTE D bit so subsequent writes are tracked again. */
            pte_mkclean(pte);
            vm_pgtable_unlock(vm);
            __vma_writeback_dirty_page(vma, va, pa);
            vm_pgtable_lock(vm);
        }
        vm_pgtable_unlock(vm);
        sfence_vma();

        /* MS_SYNC: flush data to stable storage before returning. */
        if (flags & MS_SYNC) {
            if (file->ops != NULL && file->ops->fsync != NULL)
                ret = file->ops->fsync(file, (loff_t)addr,
                                       (loff_t)size);
        }
    }

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

    case MADV_DONTNEED: {
        int shared_file_wb =
            (vma->file != NULL) && (vma->flags & VMA_FLAG_SHARED) &&
            (vma->flags & VMA_FLAG_FILE);
        vm_pgtable_lock(vm);
        for (uint64 va = addr; va < addr + size; va += PGSIZE) {
            pte_t *pte = walk(vm->pagetable, va, 0, NULL, NULL);
            if (pte != NULL && pte_present(pte)) {
                uint64 pa = pte_pa(pte);
                if (pa != 0) {
                    int was_dirty = pte_dirty(pte);
                    pte_clear(pte);

                    /* Write dirty pages back for MAP_SHARED file mappings.
                     * Must drop the pgtable spinlock because the
                     * writepage callback may sleep (inode lock, I/O). */
                    if (shared_file_wb && was_dirty) {
                        vm_pgtable_unlock(vm);
                        __vma_writeback_dirty_page(vma, va, pa);
                        vm_pgtable_lock(vm);
                    }

                    /* Release the page. */
                    page_t *pg = __pa_to_page(pa);
                    page_t *pc_head = page_pcache_head(pg);
                    if (pc_head != NULL) {
                        folio_t *folio = page_folio(pc_head);
                        pcache_put_folio(pc_head->pcache.pcache, folio);
                    } else {
                        /* Only decrement rmap for HEAD pages. */
                        if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON))
                            page_remove_rmap(pg);
                        page_ref_dec((void *)pa);
                    }
                } else {
                    pte_clear(pte);
                }
            }
        }
        vm_pgtable_unlock(vm);
        vm_remote_sfence(vm);
        ret = 0;
        break;
    }

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

    if (search_top <= search_bottom + size) {
        printf("vm_find_free_range: FAIL bounds check size=0x%lx "
               "search_bottom=0x%lx search_top=0x%lx "
               "stack_bottom=0x%lx heap_end=0x%lx reserve_end=0x%lx\n",
               size, search_bottom, search_top,
               stack_bottom, heap_end, reserve_end);
        return 0;
    }

    /* Use maple tree reverse gap search to find free space (high to low). */
    MA_STATE(mas, &vm->vm_mt, 0, 0);
    int rc = mas_empty_area_rev(&mas, search_bottom, search_top - 1, size);
    if (rc != 0) {
        printf("vm_find_free_range: mas_empty_area_rev FAIL rc=%d "
               "search_bottom=0x%lx search_top=0x%lx size=0x%lx\n",
               rc, search_bottom, search_top - 1, size);
        return 0;
    }

    /* Verify: the returned address should not overlap any existing entry. */
    uint64 verify_idx = mas.index;
    void *conflict = mt_find(&vm->vm_mt, &verify_idx, mas.index + size - 1);
    if (conflict != NULL) {
        vma_t *cv = (vma_t *)conflict;
        printf("vm_find_free_range: BUG mas_empty_area_rev returned occupied "
               "addr=0x%lx size=0x%lx conflict_vma=[0x%lx-0x%lx] "
               "search=[0x%lx,0x%lx]\n",
               mas.index, size, cv->start, cv->end,
               search_bottom, search_top - 1);
        /* Dump all VMAs in the tree for diagnosis. */
        uint64 dump_idx = 0;
        void *dump_entry;
        int count = 0;
        mt_for_each(&vm->vm_mt, dump_entry, dump_idx, MAPLE_MAX) {
            vma_t *dv = (vma_t *)dump_entry;
            if (count < 30 || (dv->start <= mas.index + size && dv->end >= mas.index)) {
                printf("  VMA[%d]: [0x%lx-0x%lx] flags=0x%lx\n",
                       count, dv->start, dv->end, dv->flags);
            }
            count++;
        }
        printf("  Total VMAs: %d\n", count);
        return 0;
    }

    return mas.index;
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
        if (vma == NULL) {
            /* In a gap — find the next VMA in the range. */
            uint64 next_idx = start;
            void *next = mt_find(&vm->vm_mt, &next_idx, end - 1);
            if (next == NULL)
                break; /* no more VMAs in range */
            vma = (vma_t *)next;
            start = vma->start;
            if (start >= end)
                break;
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
        return (uint64)-EINVAL;
    if (length > (vm->vm_top - vm->vm_bottom))
        return (uint64)-ENOMEM;
    if (length > ((size_t)-1) - (PGSIZE - 1))
        return (uint64)-EINVAL;

    if (!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED))
        return (uint64)-EINVAL;
    if ((flags & MAP_PRIVATE) && (flags & MAP_SHARED))
        return (uint64)-EINVAL;

    if (fd != -1) {
        if (flags & MAP_ANONYMOUS)
            return (uint64)-EINVAL;
        if (offset & (PGSIZE - 1))
            return (uint64)-EINVAL;
        file = vfs_fdtable_get_file(current->fdtable, fd);
        if (file == NULL)
            return (uint64)-EBADF;
        struct vfs_inode *inode = vfs_inode_deref(&file->inode);
        if (inode == NULL || !S_ISREG(inode->mode)) {
            vfs_fput(file);
            return (uint64)-ENODEV;
        }
    } else if (!(flags & MAP_ANONYMOUS)) {
        return (uint64)-EBADF;
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
            return (uint64)-ENOMEM;
        }
    } else {
        map_addr = PGROUNDDOWN(addr);
        uint64 map_end = map_addr + length;
        if (__vm_unmap_range_locked(vm, map_addr, map_end) != 0) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-EINVAL;
        }
    }

    vma_t *vma = vma_alloc(vm, map_addr, length, vm_flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        if (file != NULL)
            vfs_fput(file);
        return (uint64)-ENOMEM;
    }

    if (file != NULL) {
        vma->file = file;
        vma->pgoff = offset;
    }

    vma = __vma_try_merge_neighbors(vma);

    if (file != NULL && !(vm_flags & PROT_WRITE) &&
        (vm_flags & (PROT_READ | PROT_EXEC))) {
        uint64 eager_len = length;
        uint64 eager_max = VM_MMAP_EAGER_PAGES * PGSIZE;
        uint64 eager_flags = VMA_FLAG_USER | (vm_flags & (PROT_READ | PROT_EXEC));

        if (eager_len > eager_max)
            eager_len = eager_max;
        if (eager_len != 0)
            (void)vma_validate(vma, map_addr, eager_len, eager_flags);
    }

    vm_wunlock(vm);
    return map_addr;
}

int vm_munmap(vm_t *vm, uint64 addr, size_t length)
{
    if (vm == NULL || length == 0)
        return -EINVAL;

    addr = PGROUNDDOWN(addr);
    length = PGROUNDUP(length);

    if (addr < vm->vm_bottom || (addr + length) > vm->vm_top)
        return -EINVAL;

    int ret;
    vm_wlock(vm);
    ret = __vm_unmap_range_locked(vm, addr, addr + length);
    vm_wunlock(vm);
    return ret;
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

    mt_init(&vm->vm_mt);

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
        folio_t *folio = folio_alloc(0, PAGE_TYPE_ANON);
        void *pa = folio ? (void *)folio_address(folio) : NULL;
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
