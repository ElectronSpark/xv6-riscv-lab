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
#include "cmdline.h"
#include <vfs/file.h>
#include <vfs/fs.h>
#include <dev/bio.h>

/* Fault-around is now controlled by USER_FAULT_AROUND_PAGES in
 * the arch trap handler.  The batch install uses va_end directly. */
#define VM_MMAP_EAGER_PAGES 64

static int webkit_mremap_trace_enabled(void)
{
    static int cached = -1;
    char value[16];

    if (cached < 0) {
        cached = (cmdline_get_param("webkit_mremap_trace", value,
                                    sizeof(value)) == 0 &&
                  value[0] != '\0' && value[0] != '0');
    }
    return cached;
}

static int webkit_mremap_trace_process(void)
{
    const char *name = current != NULL ? current->name : "";

    return strncmp(name, "WebKit", 6) == 0 ||
           strncmp(name, "MiniBrowser", 11) == 0;
}

#define WEBKIT_MREMAP_TRACE(fmt, ...)                                         \
    do {                                                                      \
        if (trace_mremap)                                                     \
            printf("[%lu] webkit-mremap: pid=%d name=%s " fmt "\n",          \
                   r_time(), current != NULL ? current->pid : -1,             \
                   current != NULL ? current->name : "?", ##__VA_ARGS__);     \
    } while (0)

/*
 * Default compound order for anonymous folio allocation during demand faults.
 * Anonymous memory always uses order 0 (single 4 KB page).  2 MB huge pages
 * are reserved for file-backed (MAP_PRIVATE) mappings where the data is
 * copied from the page cache into a freshly allocated anonymous folio.
 *
 * DESIGN NOTE (2MB hugepage support):
 *   - Hugepages are ONLY used for file-backed MAP_PRIVATE VMAs.
 *   - Anonymous VMAs (heap, stack, mmap(MAP_ANON)) always use 4 KB pages.
 *   - MAP_SHARED file VMAs always use 4 KB pages (hugepages would break
 *     write visibility between processes sharing the same mapping).
 *   - File hugepages are installed as PAGE_TYPE_ANON folios with rmap,
 *     allowing standard COW on fork (write-protect both, copy 2 MB on fault).
 *   - The pcache (page cache) is NOT bypassed — data is read from pcache
 *     into the 2 MB folio, so pcache eviction/writeback is unaffected.
 *   - __vma_set_free, __vma_copy (fork COW), and __vma_validate_pte_rxw
 *     all handle hugepage PTEs via pte_is_hugepage() checks.
 */
#define VM_ANON_FOLIO_ORDER 0

/* ========================================================================== */
/*  Slab pools                                                                */
/* ========================================================================== */

static slab_cache_t __vma_pool = {0};
static slab_cache_t __vm_pool = {0};

static void __vm_destroy(vm_t *vm);
static int __vm_unmap_range_locked(vm_t *vm, uint64 start, uint64 end);

static void atomic_sub_floor_u64(_Atomic uint64 *ptr, uint64 delta)
{
    uint64 old;

    if (delta == 0)
        return;

    old = __atomic_load_n(ptr, __ATOMIC_RELAXED);
    while (old != 0) {
        uint64 next = old > delta ? old - delta : 0;
        if (__atomic_compare_exchange_n(ptr, &old, next, 1,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return;
    }
}

uint64 vm_resident_pages(vm_t *vm)
{
    if (vm == NULL)
        return 0;
    return __atomic_load_n(&vm->resident_pages, __ATOMIC_RELAXED);
}

static void vm_account_resident_add(vma_t *vma, uint64 pages)
{
    if (vma == NULL || vma->vm == NULL || pages == 0)
        return;
    if ((vma->flags & VMA_FLAG_USER) == 0)
        return;

    __atomic_fetch_add(&vma->vm->resident_pages, pages, __ATOMIC_RELAXED);
    if (current != NULL && current->vm == vma->vm &&
        current->thread_group != NULL) {
        __atomic_fetch_add(&current->thread_group->acct.mm_rss_pages,
                           pages, __ATOMIC_RELAXED);
    }
}

static void vm_account_resident_sub(vma_t *vma, uint64 pages)
{
    if (vma == NULL || vma->vm == NULL || pages == 0)
        return;
    if ((vma->flags & VMA_FLAG_USER) == 0)
        return;

    atomic_sub_floor_u64(&vma->vm->resident_pages, pages);
    if (current != NULL && current->vm == vma->vm &&
        current->thread_group != NULL) {
        atomic_sub_floor_u64(&current->thread_group->acct.mm_rss_pages,
                             pages);
    }
}

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
/*  ASID / PCID allocator                                                     */
/* ========================================================================== */

#define ASID_KERNEL 0
#define ASID_FIRST  1

static uint16      asid_max;          /* 0 = feature not available */
static spinlock_t   asid_lock;
static uint16      asid_next = ASID_FIRST;
static uint16      asid_gen;

void vm_asid_init(uint16 max_asid)
{
    asid_max  = max_asid;
    asid_next = ASID_FIRST;
    asid_gen  = 0;
    spin_init(&asid_lock, "asid");
    printf("vm_asid_init: max ASID = %d\n", max_asid);
}

uint16 vm_asid_max(void) { return asid_max; }
uint16 vm_asid_gen(void) { return asid_gen; }

static uint16 vm_alloc_asid(uint16 *gen_out)
{
    if (asid_max == 0) {
        *gen_out = 0;
        return 0;   /* feature not available — fall back to ASID 0 */
    }

    int irq = spin_lock_irqsave(&asid_lock);
    uint16 asid = asid_next++;
    uint16 gen  = asid_gen;

    if (asid > asid_max) {
        /* ASID space exhausted — start a new generation.
         * Per-CPU code will do a full TLB flush when it notices the
         * generation mismatch. */
        gen = ++asid_gen;
        asid_next = ASID_FIRST + 1;
        asid = ASID_FIRST;
    }
    spin_unlock_irqrestore(&asid_lock, irq);

    *gen_out = gen;
    return asid;
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

static void __vma_clear_range(vma_t *vma, uint64 start, uint64 end,
                              int allow_file_writeback)
{
    if (vma == NULL || vma->vm == NULL)
        return;

    if ((start & (PGSIZE - 1)) != 0 || (end & (PGSIZE - 1)) != 0 ||
        start >= end) {
        printf("__vma_set_free: BAD vma=%p start=0x%lx end=0x%lx "
               "clear=[0x%lx-0x%lx) "
               "flags=0x%lx file=%p pgoff=0x%lx vm=%p\n",
               vma, vma->start, vma->end, start, end, vma->flags,
               vma->file, vma->pgoff, vma->vm);
        if (vma->start == VMA_FREED_MAGIC && vma->end == VMA_FREED_MAGIC) {
            printf("  -> VMA already freed (FREED_MAGIC sentinel) — skipping\n");
            return;
        }
        panic("__vma_set_free: vma start not aligned");
    }

    /* Check if this is a shared file mapping that needs writeback. */
    int shared_file_wb =
        allow_file_writeback &&
        (vma->file != NULL) && (vma->flags & VMA_FLAG_SHARED) &&
        (vma->flags & VMA_FLAG_FILE);
    int is_anon_vma = (vma->file == NULL);
    int is_pfnmap = (vma->flags & VMA_FLAG_PFNMAP) != 0;
    int tlb_needs_flush = 0;
    uint64 pages_freed = 0;
    uint64 anon_pages = 0;
    uint64 resident_pages_cleared = 0;

    /*
     * Two-phase page release: Phase 1 clears PTEs and collects pages
     * into a deferred array.  Phase 2 (after TLB flush) releases refs
     * in batch, grouping consecutive pcache folio refs together.
     */
#define VMA_FREE_DEFER_MAX 256
    struct {
        uint64 pa;
        page_t *pg;
    } defer[VMA_FREE_DEFER_MAX];
    int ndefer = 0;

    /* Flush helper: release all deferred pages. */
#define FLUSH_DEFERRED() do {                                       \
    page_t *anon_batch[VMA_FREE_DEFER_MAX];                         \
    int anon_count = 0;                                             \
    if (is_anon_vma) {                                              \
        for (int __d = 0; __d < ndefer; __d++) {                    \
            page_t *__pg = defer[__d].pg;                           \
            __sync_fetch_and_sub(&__pg->anon.mapcount, 1);          \
            anon_batch[__d] = __pg;                                 \
        }                                                           \
        anon_count = ndefer;                                        \
        anon_pages += ndefer;                                       \
    } else {                                                        \
    struct pcache *batch_pc = NULL;                                  \
    folio_t *batch_folio  = NULL;                                    \
    int batch_count = 0;                                             \
    for (int __d = 0; __d < ndefer; __d++) {                        \
        page_t *__pg = defer[__d].pg;                               \
        page_t *__pc_head = page_pcache_head(__pg);                 \
        if (__pc_head != NULL) {                                    \
            folio_t *__f = page_folio(__pc_head);                   \
            if (__f == batch_folio) {                                \
                batch_count++;                                      \
            } else {                                                \
                if (batch_folio != NULL)                             \
                    pcache_put_folio_refs(batch_pc, batch_folio,     \
                                         batch_count);              \
                batch_pc = __pc_head->pcache.pcache;                \
                batch_folio = __f;                                   \
                batch_count = 1;                                    \
            }                                                       \
        } else {                                                    \
            if (batch_folio != NULL) {                               \
                pcache_put_folio_refs(batch_pc, batch_folio,         \
                                     batch_count);                  \
                batch_folio = NULL; batch_count = 0;                \
            }                                                       \
            if (__pg != NULL && PAGE_IS_TYPE(__pg, PAGE_TYPE_ANON))  \
                page_remove_rmap(__pg);                              \
            anon_batch[anon_count++] = __pg;                        \
            anon_pages++;                                           \
        }                                                           \
    }                                                               \
    if (batch_folio != NULL)                                        \
        pcache_put_folio_refs(batch_pc, batch_folio, batch_count);  \
    }                                                               \
    if (anon_count > 0)                                             \
        page_free_anon_batch(anon_batch, anon_count);               \
    ndefer = 0;                                                     \
} while (0)

    if (vma->vm->pagetable != NULL) {
        pagetable_t pagetable = vma->vm->pagetable;
        pte_t *l0 = NULL;
        uint64 l0_rgn = ~0ULL;
        for (uint64 a = start; a < end; a += PGSIZE) {
            pte_t *pte;
            uint64 rgn = a >> PXSHIFT(1);
            if (rgn == l0_rgn && l0 != NULL) {
                pte = &l0[PX(0, a)];
            } else {
                pte = walk(pagetable, a, 0, NULL, NULL);
                l0_rgn = rgn;
                if (pte == 0) {
                    /* No page table for this 2MB region — skip ahead. */
                    l0 = NULL;
                    uint64 next = ((a >> PXSHIFT(1)) + 1) << PXSHIFT(1);
                    if (next > a + PGSIZE)
                        a = next - PGSIZE; /* loop increment adds PGSIZE */
                    continue;
                }
                /* If walk() returned a 2MB hugepage PTE, handle it
                 * specially: free the entire 2MB page and skip ahead.
                 * Do NOT cache l0 — there is no level-0 table.
                 *
                 * NOTE: hugepages here are always PAGE_TYPE_ANON folios
                 * (copied from file data), so page_remove_rmap +
                 * page_ref_dec releases the folio correctly via the
                 * buddy allocator's compound-page freeing path.
                 * No pcache ref to release — data was copied out. */
                if (pte_is_hugepage(pte)) {
                    if (pte_present(pte)) {
                        uint64 pa = pte_pa(pte);
                        pte_clear(pte);
                        tlb_needs_flush = 1;
                        if (!is_pfnmap)
                            resident_pages_cleared += HUGEPAGE_SIZE / PGSIZE;
                        defer[ndefer].pa = pa;
                        defer[ndefer].pg = __pa_to_page(pa);
                        ndefer++;
                        if (ndefer == VMA_FREE_DEFER_MAX)
                            FLUSH_DEFERRED();
                    }
                    l0 = NULL;
                    /* Advance to end of this 2MB region. */
                    uint64 next = (a & HUGEPAGE_MASK) + HUGEPAGE_SIZE;
                    if (next > a + PGSIZE)
                        a = next - PGSIZE;
                    continue;
                }
                l0 = pte - PX(0, a);
            }
            if (pte_nonleaf(pte))
                panic("__vma_set_free: not a leaf");
            if (!pte_present(pte))
                continue;
            uint64 pa = pte_pa(pte);
            int was_dirty = pte_dirty(pte);
            pte_clear(pte);
            tlb_needs_flush = 1;
            pages_freed++;
            if (!is_pfnmap)
                resident_pages_cleared++;

            /* Write dirty pages back for MAP_SHARED file mappings. */
            if (shared_file_wb && was_dirty)
                __vma_writeback_dirty_page(vma, a, pa);

            if (is_pfnmap)
                continue;

            /* Collect page for deferred release. */
            defer[ndefer].pa = pa;
            defer[ndefer].pg = __pa_to_page(pa);
            ndefer++;
            if (ndefer == VMA_FREE_DEFER_MAX)
                FLUSH_DEFERRED();
        }
        if (tlb_needs_flush)
            vm_remote_sfence_range(vma->vm, start, end - start);
        /* Phase 2: release all deferred pages after TLB flush. */
        FLUSH_DEFERRED();
    }
    vm_account_resident_sub(vma, resident_pages_cleared);
#undef FLUSH_DEFERRED
#undef VMA_FREE_DEFER_MAX
    g_vm_munmap_pages_freed += pages_freed;
    g_vm_munmap_anon_pages += anon_pages;
}

static void __vma_set_free(vma_t *vma)
{
    if (vma == NULL || vma->vm == NULL)
        return;

    __vma_clear_range(vma, vma->start, vma->end, 1);

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
    if (src->flags != dst->flags)
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
    int wipe_on_fork = (src->flags & VMA_FLAG_WIPEONFORK) != 0;
    int src_tlb_flush_needed = 0;

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
        if (wipe_on_fork)
            goto skip_pte_copy;
        for (uint64 a = src->start; a < src->end; a += PGSIZE) {
            pte_t *src_pte = walk(pgtb_src, a, 0, NULL, NULL);
            if (src_pte == NULL || *src_pte == 0)
                continue;

            /* ---- 2 MB hugepage PTE at level 1 ----
             * File-backed MAP_PRIVATE hugepages are PAGE_TYPE_ANON.
             * Fork COW: write-protect in both parent and child,
             * bump refcount.  On write fault, __vma_validate_pte_rxw
             * copies the entire 2 MB folio (no splitting). */
            if (pte_is_hugepage(src_pte)) {
                if (!pte_present(src_pte))
                    goto hugepage_skip;
                pte_t *dst_pmd = walk_pmd(pgtb_dst, a, 1);
                if (dst_pmd == NULL) {
                    __vma_set_free(dst);
                    return -ENOMEM;
                }
                if (*dst_pmd != 0) {
                    __vma_set_free(dst);
                    return -ENOMEM;
                }
                if (is_shared) {
                    *dst_pmd = *src_pte;
                } else {
                    if (pte_write(src_pte)) {
                        pte_wrprotect(src_pte);
                        src_tlb_flush_needed = 1;
                    }
                    *dst_pmd = *src_pte;
                }
                uint64 pa = pte_pa(src_pte);
                assert(page_ref_inc((void *)pa) > 0,
                       "__vma_copy: hugepage refcnt should be > 0");
                vm_account_resident_add(dst, HUGEPAGE_SIZE / PGSIZE);
                page_t *pg = __pa_to_page(pa);
                if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON) &&
                    !is_shared) {
                    if (page_mapcount(pg) == 0)
                        page_add_anon_rmap(pg, src, a);
                    page_add_anon_rmap(pg, dst, a);
                }
hugepage_skip:
                /* Advance to end of this 2 MB region. */
                {
                    uint64 next = (a & HUGEPAGE_MASK) + HUGEPAGE_SIZE;
                    if (next > a + PGSIZE)
                        a = next - PGSIZE;
                }
                continue;
            }

            /* ---- regular 4 KB PTE ---- */
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
                if (pte_write(src_pte)) {
                    pte_wrprotect(src_pte);
                    src_tlb_flush_needed = 1;
                }
                *new_pte = *src_pte;
            }
            uint64 pa = pte_pa(src_pte);
            assert(page_ref_inc((void *)pa) > 0,
                   "__vma_copy: page refcnt should be greater than 0");
            vm_account_resident_add(dst, 1);
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
        if (src_tlb_flush_needed)
            vm_remote_sfence_range(src->vm, src->start, VMA_SIZE(src));
skip_pte_copy:
        ;
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
        __mt_erase_vma(vm, vma);
        /* Roll back: restore VMA fields and re-store old range. */
        vma->start = old_start;
        vma->end = old_end;
        __mt_store_vma(vm, vma); /* best-effort restore */
    }
    return ret;
}

static int vma_tree_entry_valid(vm_t *vm, void *entry)
{
    vma_t *vma;
    uint64 ptr = (uint64)entry;
    pte_t *pte;

    if (entry == NULL)
        return 1;

    /*
     * Maple tree values in vm_mt must always be vma_t pointers.  If a user
     * virtual address leaks into a slot, do not dereference it while printing
     * diagnostics; that turns a recoverable VMA-tree inconsistency into a
     * kernel page fault.
     */
    if (kernel_vm != NULL && kernel_vm->pagetable != NULL) {
        pte = walk(kernel_vm->pagetable, ptr, 0, NULL, NULL);
        if (pte == NULL || !pte_present(pte))
            return 0;
        pte = walk(kernel_vm->pagetable, ptr + sizeof(vma_t) - 1,
                   0, NULL, NULL);
        if (pte == NULL || !pte_present(pte))
            return 0;
    } else if (!vm->is_kernel && ptr >= vm->vm_bottom && ptr < vm->vm_top) {
        return 0;
    }

    vma = (vma_t *)entry;
    if (vma->vm != vm)
        return 0;
    if (vma->start == VMA_FREED_MAGIC && vma->end == VMA_FREED_MAGIC)
        return 0;
    if ((vma->start & (PGSIZE - 1)) != 0 || vma->start >= vma->end)
        return 0;
    if (vma->start < vm->vm_bottom || vma->end > vm->vm_top)
        return 0;

    return 1;
}

struct vm_pte_accounting {
    vm_t *vm;
    int total_leafs;
    int missing_vma;
    int invalid_vma;
    int printed;
};

static void vm_count_orphan_leaf(uint64 va, uint64 size, pte_t pte, void *arg)
{
    struct vm_pte_accounting *acct = arg;
    uint64 check = va;
    void *entry;

    if (va >= TRAPFRAME && va < TRAPFRAME + (cpu_possible_count() * PGSIZE))
        return;

    acct->total_leafs++;
    entry = mt_find(&acct->vm->vm_mt, &check, va + size - 1);
    if (entry == NULL) {
        acct->missing_vma++;
        if (acct->printed < 8) {
            printf("vm_destroy: orphan mapped leaf va=[0x%lx-0x%lx) "
                   "pte=0x%lx pid=%d %s\n",
                   va, va + size, pte,
                   current ? current->pid : -1,
                   current ? current->name : "?");
            acct->printed++;
        }
        return;
    }

    if (!vma_tree_entry_valid(acct->vm, entry) ||
        ((vma_t *)entry)->start > va || ((vma_t *)entry)->end < va + size) {
        acct->invalid_vma++;
        if (acct->printed < 8) {
            printf("vm_destroy: mapped leaf has bad VMA va=[0x%lx-0x%lx) "
                   "pte=0x%lx entry=%p pid=%d %s\n",
                   va, va + size, pte, entry,
                   current ? current->pid : -1,
                   current ? current->name : "?");
            acct->printed++;
        }
    }
}

static void vm_report_pte_accounting(vm_t *vm)
{
    if (vm == NULL || vm->pagetable == NULL)
        return;

    struct vm_pte_accounting acct = {
        .vm = vm,
    };
    uvm_visit_present_leafs(vm->pagetable, vm_count_orphan_leaf, &acct);
    if (acct.missing_vma != 0 || acct.invalid_vma != 0) {
        printf("vm_destroy: PTE/VMA mismatch pid=%d %s leafs=%d "
               "missing_vma=%d invalid_vma=%d\n",
               current ? current->pid : -1,
               current ? current->name : "?",
               acct.total_leafs, acct.missing_vma, acct.invalid_vma);
    }
}

static void *vm_tree_next_slot(vm_t *vm, uint64 *cursor,
                               uint64 *slot_start, uint64 *slot_last)
{
    if (vm == NULL || cursor == NULL)
        return NULL;

    while (*cursor < vm->vm_top) {
        MA_STATE(mas, &vm->vm_mt, *cursor, *cursor);
        void *entry = mas_walk(&mas);

        if (mas.node == NULL)
            return NULL;

        uint64 next;
        if (mas.max >= vm->vm_top - 1 || mas.max == MAPLE_MAX)
            next = vm->vm_top;
        else
            next = mas.max + 1;

        if (entry != NULL) {
            if (slot_start != NULL)
                *slot_start = mas.min;
            if (slot_last != NULL)
                *slot_last = mas.max;
            *cursor = next;
            return entry;
        }

        if (next <= *cursor)
            return NULL;
        *cursor = next;
    }

    return NULL;
}

static int vm_tree_contains_vma(vm_t *vm, vma_t *needle)
{
    if (vm == NULL || needle == NULL)
        return 0;

    uint64 cursor = vm->vm_bottom;
    void *entry;
    while ((entry = vm_tree_next_slot(vm, &cursor, NULL, NULL)) != NULL) {
        if (entry == needle)
            return 1;
    }
    return 0;
}

static void vm_clear_vma_slot_range(vm_t *vm, vma_t *vma,
                                    uint64 slot_start, uint64 slot_end)
{
    if (vm == NULL || vma == NULL || slot_start >= slot_end)
        return;

    if (slot_start < vm->vm_bottom)
        slot_start = vm->vm_bottom;
    if (slot_end > vm->vm_top)
        slot_end = vm->vm_top;
    if (slot_start >= slot_end)
        return;

    /*
     * The maple slot range is the range that actually points at this VMA.
     * It can differ from vma->start/end after a failed partial range update.
     * Clear the PTEs for the slot itself so stale disjoint slots cannot leave
     * mapped pages behind, but only do file writeback when the slot is still
     * inside the VMA's current file-offset domain.
     */
    int allow_writeback =
        slot_start >= vma->start && slot_end <= vma->end;
    __vma_clear_range(vma, slot_start, slot_end, allow_writeback);
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

    uint16 gen;
    vm->asid = vm_alloc_asid(&gen);
    vm->asid_gen = gen;

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
     * Tear down by maple slot range, not only by vma->start/end.  A failed
     * partial mtree_store_range can leave the same VMA pointer in stale
     * disjoint slots.  Those stale slots still describe page-table ranges
     * that must be cleared before freewalk() runs.
     */

    /*
     * No CPU can be running this address space any more (the last
     * reference was just dropped), so zero the cpumask to suppress all
     * TLB IPIs during the VMA teardown loop below.
     */
    smp_store_release(&vm->cpumask, 0);

    vm_report_pte_accounting(vm);

    while (1) {
        uint64 cursor = vm->vm_bottom;
        uint64 slot_start;
        uint64 slot_last;
        void *entry = vm_tree_next_slot(vm, &cursor, &slot_start, &slot_last);
        if (entry == NULL)
            break;

        uint64 slot_end;
        if (slot_last >= vm->vm_top - 1)
            slot_end = vm->vm_top;
        else
            slot_end = slot_last + 1;

        if (!vma_tree_entry_valid(vm, entry)) {
            mtree_store_range(&vm->vm_mt, slot_start, slot_last, NULL);
            continue;
        }

        vma_t *vma = (vma_t *)entry;
        vm_clear_vma_slot_range(vm, vma, slot_start, slot_end);

        mtree_store_range(&vm->vm_mt, slot_start, slot_last, NULL);
        if (!vm_tree_contains_vma(vm, vma)) {
            __vma_set_free(vma);
            __vma_free(vma);
        }
    }

    vm_report_pte_accounting(vm);

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
    void *mt_entry;
    vma_t *last_vma = NULL;  /* duplicate detection for partial mtree_store_range */
    uint64 cursor = src->vm_bottom;
    while ((mt_entry = vm_tree_next_slot(src, &cursor, NULL, NULL)) != NULL) {
        vma_t *vma = (vma_t *)mt_entry;

        /* Skip duplicate: a prior partial mtree_store_range can leave
         * the same VMA pointer in two disjoint maple tree ranges.
         * Also skip freed VMAs (sentinel from __vma_free). */
        if (vma == last_vma)
            continue;
        if (vma->start == VMA_FREED_MAGIC && vma->end == VMA_FREED_MAGIC)
            continue;
        last_vma = vma;
        if (vma->flags & VMA_FLAG_DONTFORK)
            continue;

        vma_t *new_vma = vma_alloc(dst, vma->start, VMA_SIZE(vma), vma->flags);
        if (new_vma == NULL) {
            /* If vma_alloc failed because the range already exists in dst,
             * this is a non-consecutive duplicate from a partial
             * mtree_store_range — skip it rather than failing.
             * Safety check: verify the existing entry covers the EXACT
             * same range.  A partial mtree_store_range from a DIFFERENT
             * VMA could have spilled into this range; in that case the
             * current VMA has NOT been copied and we must not skip it. */
            uint64 check = vma->start;
            void *existing = mt_find(&dst->vm_mt, &check,
                                     vma->start + VMA_SIZE(vma) - 1);
            if (existing != NULL) {
                vma_t *ev = (vma_t *)existing;
                if (ev->start == vma->start && ev->end == vma->end)
                    continue;
            }
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

void vm_rlock(vm_t *vm) __acquires(vm)
{
#ifdef __CHECKER__
    __acquire_context(vm);
#else
    if (vm->is_kernel)
        spin_lock(&vm->spinlock);
    else
        rwsem_acquire_read(&vm->rw_lock);
#endif
}
void vm_runlock(vm_t *vm) __releases(vm)
{
#ifdef __CHECKER__
    __release_context(vm);
#else
    if (vm->is_kernel)
        spin_unlock(&vm->spinlock);
    else
        rwsem_release(&vm->rw_lock);
#endif
}
void vm_wlock(vm_t *vm) __acquires(vm)
{
#ifdef __CHECKER__
    __acquire_context(vm);
#else
    if (vm->is_kernel)
        spin_lock(&vm->spinlock);
    else
        rwsem_acquire_write(&vm->rw_lock);
#endif
}
void vm_wunlock(vm_t *vm) __releases(vm)
{
#ifdef __CHECKER__
    __release_context(vm);
#else
    if (vm->is_kernel)
        spin_unlock(&vm->spinlock);
    else
        rwsem_release(&vm->rw_lock);
#endif
}
int vm_is_wlocked(vm_t *vm) {
    if (vm->is_kernel)
        return spin_holding(&vm->spinlock);
    return rwsem_is_write_holding(&vm->rw_lock);
}
void vm_pgtable_lock(vm_t *vm) __acquires(vm)
{
#ifdef __CHECKER__
    __acquire_context(vm);
#else
    spin_lock(&vm->spinlock);
#endif
}
void vm_pgtable_unlock(vm_t *vm) __releases(vm)
{
#ifdef __CHECKER__
    __release_context(vm);
#else
    spin_unlock(&vm->spinlock);
#endif
}

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
        if (!vma_tree_entry_valid(vm, vma)) {
            printf("vm_find_area: invalid maple entry=%p for va=0x%lx\n",
                   vma, va);
            return NULL;
        }
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
    if ((va & (PGSIZE - 1)) != 0) {
        printf("vma_split: FAIL unaligned va=0x%lx vma=[0x%lx-0x%lx]\n",
               va, vma->start, vma->end);
        return NULL;
    }
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

    if (vma->anon_vma != NULL && anon_vma_fork(new_vma, vma) != 0) {
        if (new_vma->file != NULL)
            vfs_fput(new_vma->file);
        __vma_free(new_vma);
        return NULL;
    }

    uint64 old_start = vma->start;
    uint64 old_end = vma->end;
    vma->end = va;

    /* Update the maple tree: shrink old VMA, insert new VMA.
     * Both stores must succeed; otherwise roll back. */
    if (__mt_update_vma(vma->vm, vma, old_start, old_end) != 0) {
        if (new_vma->file != NULL)
            vfs_fput(new_vma->file);
        __vma_free(new_vma);
        return NULL;
    }
    if (__mt_store_vma(vma->vm, new_vma) != 0) {
        /* Restore old VMA range and re-store it. */
        __mt_erase_vma(vma->vm, new_vma);
        vma->end = old_end;
        __mt_update_vma(vma->vm, vma, old_start, va); /* best-effort restore */
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
    if (vma1->flags != vma2->flags)
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
            if (!vma_tree_entry_valid(vm, overlap)) {
                printf("vma_alloc: invalid maple overlap entry=%p "
                       "range=[0x%lx-0x%lx]\n",
                       overlap, va, va + size);
                return NULL;
            }
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
        __mt_erase_vma(vm, vma_new);
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
 * VMA around @fault_va.  For 2MB huge pages the folio base must be
 * naturally aligned (2MB boundary), fit entirely within the VMA, and the
 * VMA must be a MAP_PRIVATE file-backed mapping (not anonymous, not shared).
 *
 * Returns HUGEPAGE_ORDER or 0.
 */
static unsigned int __folio_order_for_vma(vma_t *vma, uint64 fault_va)
{
    /*
     * File-backed hugepages disabled: the 2 MB copy from pcache into an
     * anonymous folio is extremely expensive under QEMU TCG (770 ms for
     * a Python launch).  The regular batch-install path maps pcache pages
     * directly (zero-copy) and is much faster despite 4 KB TLB entries.
     */
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
    /* Anonymous memory always uses single 4 KB pages.
     * NOTE: hugepages are handled separately in vma_validate() for
     * file-backed VMAs via __vma_fault_file_hugepage(). */
    folio_t *folio = folio_alloc(0, PAGE_TYPE_ANON | GFP_HIGHMEM);
    if (folio == NULL)
        return NULL;

    uint64 folio_pa = folio_address(folio);
    memset((void *)folio_pa, 0, PGSIZE);

    pte_t *pte = walk(vma->vm->pagetable, fault_va, 1, NULL, NULL);
    if (pte == NULL) {
        page_ref_dec((void *)folio_pa);
        return NULL;
    }
    if (*pte != 0) {
        /* Race: another core already populated this PTE. */
        page_ref_dec((void *)folio_pa);
        return (void *)pte_pa(pte);
    }
    *pte = mk_pte(folio_pa, pte_flags);
    folio_add_anon_rmap(folio, vma, fault_va);
    vm_account_resident_add(vma, 1);
    /* TLB flush deferred to vma_validate end (single batch flush). */
    return (void *)folio_pa;
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

/**
 * __vma_fault_file_hugepage - try to satisfy a file-backed page fault with
 * a 2 MB hugepage.
 *
 * Allocates a 2 MB anonymous folio, reads 512 consecutive pages of file
 * data from the page cache into it, and returns the folio's physical
 * address.  The caller installs the 2 MB PTE and sets up rmap.
 *
 * Only suitable for MAP_PRIVATE VMAs — the data is copied, so MAP_SHARED
 * semantics (write visibility between processes) would be lost.
 *
 * Requirements checked by caller:
 *   - vma is file-backed, MAP_PRIVATE
 *   - 2 MB-aligned range [hp_base, hp_base+2MB) fits within the VMA
 *   - file is large enough
 *
 * @vma:     the faulting VMA
 * @hp_base: 2 MB-aligned virtual address (start of the huge page region)
 *
 * Returns the physical address of the 2 MB folio, or NULL on failure.
 * On success the returned folio has refcount 1 (caller's).
 */
static void *__vma_fault_file_hugepage(vma_t *vma, uint64 hp_base)
{
    struct vfs_file *file = vma->file;
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return NULL;
    struct pcache *pc = &inode->i_data;

    /* File offset corresponding to the 2 MB base VA. */
    uint64 file_off_base = vma->pgoff + (hp_base - vma->start);
    uint64 file_off_end  = file_off_base + HUGEPAGE_SIZE;

    /* All 512 pages must be fully covered by file data. */
    if (file_off_end > (uint64)inode->size)
        return NULL;

    /* Warm up pcache folios for the entire 2 MB region via the
     * filesystem's prefault callback (batched BIO per folio). */
    if (file->ops != NULL && file->ops->prefault != NULL)
        (void)file->ops->prefault(file, vma, hp_base,
                                  hp_base + HUGEPAGE_SIZE);

    folio_t *folio = folio_alloc(HUGEPAGE_ORDER, PAGE_TYPE_ANON | GFP_HIGHMEM);
    if (folio == NULL)
        return NULL;
    uint64 folio_pa = folio_address(folio);

    /* Copy data from pcache into the 2 MB folio, stepping by pcache
     * folio (typically 16 pages = 64 KB) instead of per page. */
    for (uint64 off = 0; off < HUGEPAGE_SIZE; ) {
        uint64 file_off  = file_off_base + off;
        uint64 blkno_512 = file_off / BLK_SIZE;

        folio_t *pcfolio = pcache_get_folio(pc, blkno_512);
        if (pcfolio == NULL)
            goto fail;
        int ret = pcache_read_folio(pc, pcfolio);
        if (ret != 0) {
            pcache_put_folio(pc, pcfolio);
            goto fail;
        }

        page_t *pcpage = &pcfolio->page;
        struct pcache_node *pcn = pcpage->pcache.pcache_node;
        if (pcn == NULL || pcn->data == NULL) {
            pcache_put_folio(pc, pcfolio);
            goto fail;
        }

        /* Copy as much as this pcache folio covers within our range. */
        uint64 pcn_base = (uint64)pcn->blkno * BLK_SIZE;
        uint64 pcn_size = (uint64)pcn->page_count * PGSIZE;
        uint64 src_off  = file_off - pcn_base;
        uint64 copy_len = pcn_size - src_off;
        if (off + copy_len > HUGEPAGE_SIZE)
            copy_len = HUGEPAGE_SIZE - off;
        memmove((void *)(folio_pa + off),
                (char *)pcn->data + src_off, copy_len);
        pcache_put_folio(pc, pcfolio);
        off += copy_len;
    }

    return (void *)folio_pa;

fail:
    page_ref_dec((void *)folio_pa);
    return NULL;
}

static int __vma_validate_pte_rxw(vma_t *vma, pte_t *pte, uint64 fault_va)
{
    if (pte_write_ready(pte))
        return 0;

    /*
     * ---- 2 MB hugepage write-fault fast paths ----
     *
     * Hugepages are file-backed MAP_PRIVATE folios (PAGE_TYPE_ANON):
     *  - Installed read-only in vma_validate()'s hugepage path.
     *  - On first write: single mapper → re-grant write in-place.
     *  - After fork (COW): mapcount > 1 → copy entire 2 MB folio.
     *
     * CAVEAT: COW always copies the full 2 MB — no splitting to 4 KB.
     * If memory is tight and folio_alloc(HUGEPAGE_ORDER) fails, the
     * write fault returns -ENOMEM.  Future work could split the
     * hugepage into 512 x 4 KB pages and COW only the faulting page.
     */
    if (pte_present(pte) && pte_is_hugepage(pte)) {
        /* Writable but not dirty — just set dirty bit. */
        if (pte_write(pte) && !pte_dirty(pte)) {
            pte_mkdirty(pte);
            /* TLB flush deferred */
            return 0;
        }
        uint64 hp_pa = pte_pa(pte);
        page_t *pg = __pa_to_page(hp_pa);
        if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON)) {
            if (page_mapcount(pg) <= 1) {
                /* Single mapper — re-grant write on the 2 MB PTE. */
                *pte = mk_pte_huge(hp_pa, vma->flags);
                /* TLB flush deferred */
                return 0;
            }
            /* COW: multiple mappers — copy the entire 2 MB folio.
             * The new folio inherits PAGE_TYPE_ANON and gets its own
             * rmap entry so future forks/unmaps track it correctly. */
            if (anon_vma_prepare(vma) != 0)
                return -ENOMEM;
            folio_t *old_folio = page_folio(pg);
            folio_t *new_folio =
                folio_alloc(HUGEPAGE_ORDER, PAGE_TYPE_ANON | GFP_HIGHMEM);
            if (new_folio == NULL)
                return -ENOMEM;
            uint64 new_pa = folio_address(new_folio);
            memmove((void *)new_pa, (void *)hp_pa, HUGEPAGE_SIZE);
            folio_remove_rmap(old_folio);
            folio_add_anon_rmap(new_folio, vma,
                                fault_va & HUGEPAGE_MASK);
            *pte = mk_pte_huge(new_pa, vma->flags);
            page_ref_dec((void *)hp_pa);
            /* TLB flush deferred */
            return 0;
        }
        /* Non-anonymous hugepage — just re-grant write. */
        *pte = mk_pte_huge(hp_pa, vma->flags);
        /* TLB flush deferred */
        return 0;
    }

    /* ---- existing 4 KB paths below ---- */

    if (pte_present(pte)) {
        page_t *pg = __pa_to_page(pte_pa(pte));
        page_t *pc_head = page_pcache_head(pg);

        /*
         * MAP_SHARED pcache pages are installed clean and write-protected
         * so the first write faults through here on every architecture.
         * Mark the backing cache dirty, then re-enable writes in-place
         * instead of taking the MAP_PRIVATE COW path below.
         */
        if (pc_head != NULL && (vma->flags & VMA_FLAG_SHARED)) {
            pcache_mark_page_dirty(pc_head->pcache.pcache, pc_head);
            *pte = mk_pte(pte_pa(pte), vma->flags);
            /* TLB flush deferred */
            return 0;
        }
    }

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
        /* TLB flush deferred */
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
                    if (mapped_pa >= folio_pa && mapped_pa < folio_pa_end) {
                        *p = mk_pte(mapped_pa, vma->flags);
                        /* TLB flush deferred */
                    }
                }
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
    /* TLB flush deferred */
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
        /* TLB flush deferred */
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
        int is_huge = pte_is_hugepage(pte);
        if (is_huge) {
            uint64 pa = pte_pa(pte);
            *pte = mk_pte_huge(pa, mflags);
        } else {
            pte_modify(pte, mflags);
        }
        /* Keep pcache pages clean unless the PTE was already dirty. */
        page_t *pg = __pa_to_page(pte_pa(pte));
        if (!is_huge && page_is_pcache(pg) && !old_dirty)
            pte_mkclean(pte);
    }

    /* TLB flush deferred */
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

static int vma_file_hugepage_collapse_enabled(void)
{
    /*
     * The file-backed PMD collapse path frees and replaces L0 page-table pages.
     * Under SMP WebKit/GStreamer workloads it has exposed stale upper-level PTEs
     * during concurrent faults and process teardown. Keep the normal 4 KB
     * pcache-backed mappings until the collapse path has full shootdown/refcount
     * protection for page-table pages.
     */
    return 0;
}

int vma_validate(vma_t *vma, uint64 va, uint64 size, uint64 flags)
{
    uint64 validate_start = r_time();
    g_vm_vma_validate_calls += 1;

    if (flags == PROT_NONE)
        return -EINVAL;
    if (vma == NULL || vma->vm == NULL || vma->vm->pagetable == NULL)
        return -EINVAL;
    if (flags & ~VMA_FLAG_PROT_MASK)
        return -EINVAL;
    if ((flags & PROT_EXEC) && (flags & PROT_READ) == 0)
        return -EINVAL;

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
     * while a spinlock is held.  Always prepare for ALL VMAs because:
     *  - Anonymous VMAs need it for demand-zero folio rmap.
     *  - File-backed MAP_PRIVATE VMAs need it for 2 MB hugepage rmap
     *    (hugepages are PAGE_TYPE_ANON folios backed by file data).
     *  - Writable file VMAs need it for COW on pcache pages.
     * Previously this was conditional on (file == NULL || PROT_WRITE);
     * now unconditional so read-only file hugepages can set up rmap. */
    if (vma->anon_vma == NULL) {
        if (anon_vma_prepare(vma) != 0)
            return -ENOMEM;
    }

    vm_pgtable_lock(vma->vm);

    /*
     * L0 page table cache: avoid redundant 4-level page table walks
     * for consecutive pages within the same 2 MB region.  Invalidated
     * whenever we drop vm_pgtable_lock (batch install, hugepage alloc).
     */
    pte_t *__l0_base = NULL;
    uint64 __l0_rgn = ~0ULL;

    uint64 hp_skip_until = 0;
    for (uint64 i = va; i < va_end; i += PGSIZE) {
        /* File-backed VMA: may need to drop spinlock for I/O */
        if (vma->file != NULL) {

            /*
             * ---- 2 MB hugepage attempt for MAP_PRIVATE files ----
             *
             * Allocate the 2 MB folio FIRST (outside lock), so that
             * a failed allocation never damages the existing L0 table.
             * Only collapse L0 entries after the folio is ready.
             *
             * hp_skip_until prevents re-attempting a 2 MB region that
             * already failed in this validate pass.
             */
            if (vma_file_hugepage_collapse_enabled() &&
                i >= hp_skip_until &&
                __folio_order_for_vma(vma, i) >= HUGEPAGE_ORDER) {
                uint64 hp_base = i & HUGEPAGE_MASK;
                pte_t *pmd = walk_pmd(vma->vm->pagetable, hp_base, 1);
                if (pmd != NULL && pte_is_hugepage(pmd)) {
                    /* Already a 2 MB hugepage — skip range. */
                    i = hp_base + HUGEPAGE_SIZE - PGSIZE;
                    continue;
                }
                if (pmd != NULL) {
                    /* Drop lock to allocate 2 MB folio (may do I/O). */
                    vm_pgtable_unlock(vma->vm);
                    void *hp_pa =
                        __vma_fault_file_hugepage(vma, hp_base);
                    vm_pgtable_lock(vma->vm);
                    __l0_rgn = ~0ULL; /* invalidate after lock drop */

                    if (hp_pa == NULL) {
                        hp_skip_until = hp_base + HUGEPAGE_SIZE;
                        goto hp_per_page;
                    }

                    /* Re-check PMD after re-acquiring lock. */
                    pmd = walk_pmd(vma->vm->pagetable, hp_base, 1);
                    if (pmd == NULL || pte_is_hugepage(pmd)) {
                        page_ref_dec(hp_pa);
                        if (pmd != NULL) {
                            i = hp_base + HUGEPAGE_SIZE - PGSIZE;
                            continue;
                        }
                        goto hp_per_page;
                    }

                    if (*pmd != 0) {
                        /* L0 table exists — collapse under lock. */
                        pagetable_t l0_tbl = (pagetable_t)PTE2PA(*pmd);
                        uint64 deferred_pa[512];
                        int nd = 0;
                        for (int k = 0; k < 512; k++) {
                            if (l0_tbl[k] != 0) {
                                pte_t *lp = &l0_tbl[k];
                                if (pte_present(lp))
                                    deferred_pa[nd++] = pte_pa(lp);
                                l0_tbl[k] = 0;
                            }
                        }
                        uint64 l0_pa = PTE2PA(*pmd);
                        pte_clear(pmd);
                        sfence_vma();
                        pgtab_free((void *)l0_pa);
                        if (nd > 0)
                            vm_account_resident_sub(vma, (uint64)nd);

                        /* Install hugepage under lock. */
                        folio_t *hp_folio =
                            page_folio(__pa_to_page((uint64)hp_pa));
                        uint64 hp_flags = vma->flags & ~PROT_WRITE;
                        *pmd = mk_pte_huge((uint64)hp_pa, hp_flags);
                        folio_add_anon_rmap(hp_folio, vma, hp_base);
                        vm_account_resident_add(vma,
                                                HUGEPAGE_SIZE / PGSIZE);

                        /* Release deferred pages outside lock. */
                        vm_pgtable_unlock(vma->vm);
                        for (int d = 0; d < nd; d++) {
                            page_t *pg = __pa_to_page(deferred_pa[d]);
                            page_t *pc_head = page_pcache_head(pg);
                            if (pc_head != NULL)
                                pcache_put_folio(
                                    pc_head->pcache.pcache,
                                    page_folio(pc_head));
                            else if (PAGE_IS_TYPE(pg, PAGE_TYPE_ANON)) {
                                page_remove_rmap(pg);
                                page_ref_dec((void *)deferred_pa[d]);
                            }
                        }
                        vm_pgtable_lock(vma->vm);
                        __l0_rgn = ~0ULL;
                        i = hp_base + HUGEPAGE_SIZE - PGSIZE;
                        continue;
                    }

                    /* PMD is empty — install directly. */
                    folio_t *hp_folio =
                        page_folio(__pa_to_page((uint64)hp_pa));
                    uint64 hp_flags = vma->flags & ~PROT_WRITE;
                    *pmd = mk_pte_huge((uint64)hp_pa, hp_flags);
                    folio_add_anon_rmap(hp_folio, vma, hp_base);
                    vm_account_resident_add(vma, HUGEPAGE_SIZE / PGSIZE);
                    /* TLB flush deferred */
                    i = hp_base + HUGEPAGE_SIZE - PGSIZE;
                    continue;
                }
            }
hp_per_page:
            ;
            /* Fast L0-cached PTE lookup: avoid 4-level walk for
             * consecutive pages in the same 2 MB region. */
            pte_t *pte;
            uint64 __rgn = i >> PXSHIFT(1);
            if (__rgn == __l0_rgn) {
                pte = &__l0_base[PX(0, i)];
            } else {
                pte = walk(vma->vm->pagetable, i, 1, NULL, NULL);
                if (pte == NULL) {
                    vm_pgtable_unlock(vma->vm);
                    return -ENOMEM;
                }
                __l0_base = pte - PX(0, i);
                __l0_rgn = __rgn;
            }
            if (*pte != 0) {
                /* PTE present.  For read/exec faults, skip this page
                 * and scan ahead in the L0 table to find the next
                 * unmapped PTE, avoiding per-page loop overhead. */
                if (!(flags & PROT_WRITE)) {
                    int idx0 = PX(0, i);
                    int end0 = 512;
                    /* Don't scan past va_end within this L0 region. */
                    uint64 rgn_end = (__l0_rgn + 1) << PXSHIFT(1);
                    if (va_end < rgn_end) {
                        int e = PX(0, va_end - PGSIZE) + 1;
                        if (e < end0)
                            end0 = e;
                    }
                    int k = idx0 + 1;
                    while (k < end0 && __l0_base[k] != 0)
                        k++;
                    if (k < end0) {
                        /* Found unmapped PTE at index k — jump to it.
                         * Subtract PGSIZE because the for-loop adds it. */
                        i = (__l0_rgn << PXSHIFT(1)) | ((uint64)k << PGSHIFT);
                        i -= PGSIZE;
                    } else {
                        /* All remaining PTEs in this L0 region are present.
                         * Skip to end of region. */
                        i = rgn_end - PGSIZE;
                        if (i >= va_end)
                            i = va_end - PGSIZE;
                    }
                    continue;
                }
                goto __do_pte_check;
            }
            {
                __l0_rgn = ~0ULL; /* invalidate: dropping lock */
                vm_pgtable_unlock(vma->vm);

                uint64 fault_end = va_end;
                if (fault_end > vma->end)
                    fault_end = vma->end;

                /*
                 * Folio-based batch PTE installation.
                 *
                 * After prefault loaded pcache folios, try to install
                 * PTEs for all pages in each folio under a single lock
                 * hold, using one pcache_get_page per folio instead of
                 * one per page.  Fall through to per-page fault for any
                 * pages that can't be batch-installed (partial pages,
                 * pcache miss, etc.).
                 */
                struct vfs_inode *__fi = vfs_inode_deref(&vma->file->inode);
                struct pcache *__pc = NULL;
                if (__fi != NULL) {
                    __pc = &__fi->i_data;
                    if (!__pc->active)
                        __pc = NULL;
                }

                uint64 fault_va = i;
                while (__pc != NULL && fault_va < fault_end) {
                    uint64 file_off = vma->pgoff + (fault_va - vma->start);
                    uint64 inode_sz = (uint64)READ_ONCE(__fi->size);
                    if (file_off >= inode_sz)
                        break;

                    uint64 blkno_512 = file_off / 512ULL;
                    page_t *pcpage = pcache_get_page(__pc, blkno_512);
                    if (pcpage == NULL) {
                        /* Lazy prefault: call on each cache miss for the
                         * remaining range.  Warm cache never enters here. */
                        if (vma->file->ops != NULL &&
                            vma->file->ops->prefault != NULL)
                            (void)vma->file->ops->prefault(vma->file, vma,
                                                           fault_va, fault_end);
                        pcpage = pcache_get_page(__pc, blkno_512);
                    }
                    if (pcpage == NULL)
                        break;

                    if (pcache_read_page(__pc, pcpage) != 0) {
                        pcache_put_page(__pc, pcpage);
                        break;
                    }

                    struct pcache_node *pcn = pcpage->pcache.pcache_node;
                    if (pcn == NULL || pcn->data == NULL) {
                        pcache_put_page(__pc, pcpage);
                        break;
                    }

                    uint64 folio_base = (uint64)pcn->blkno * 512ULL;
                    unsigned long nr = pcn->page_count;
                    uint64 folio_data_end = folio_base + nr * PGSIZE;

                    /* Collect installable full pages from this folio. */
                    int nc = 0;
                    uint64 iv[FOLIO_MAX_ORDER_NR_PAGES];
                    void  *ip[FOLIO_MAX_ORDER_NR_PAGES];

                    for (uint64 v = fault_va; v < fault_end; v += PGSIZE) {
                        uint64 fo = vma->pgoff + (v - vma->start);
                        if (fo < folio_base || fo >= folio_data_end)
                            break;
                        if (fo + PGSIZE > inode_sz)
                            break; /* partial page — per-page fallback */
                        iv[nc] = v;
                        ip[nc] = (char *)pcn->data + (fo - folio_base);
                        nc++;
                    }

                    if (nc == 0) {
                        pcache_put_page(__pc, pcpage);
                        break; /* can't batch — fall through */
                    }

                    /* Add nc refs under a single lock hold. */
                    __page_ref_add(pcpage, nc);

                    vm_pgtable_lock(vma->vm);
                    pte_t *fp = walk(vma->vm->pagetable, iv[0], 1, NULL, NULL);
                    if (fp == NULL) {
                        vm_pgtable_unlock(vma->vm);
                        __page_ref_sub(pcpage, nc);
                        pcache_put_page(__pc, pcpage);
                        return -ENOMEM;
                    }
                    pte_t *l0 = fp - PX(0, iv[0]);
                    uint64 l0_rgn = iv[0] >> PXSHIFT(1);

                    int batch_faults = 0;
                    int unused_refs = 0;
                    for (int j = 0; j < nc; j++) {
                        pte_t *p;
                        if ((iv[j] >> PXSHIFT(1)) == l0_rgn) {
                            p = &l0[PX(0, iv[j])];
                        } else {
                            p = walk(vma->vm->pagetable, iv[j], 1, NULL, NULL);
                            if (p == NULL) {
                                unused_refs += nc - j;
                                nc = j;
                                break;
                            }
                            l0 = p - PX(0, iv[j]);
                            l0_rgn = iv[j] >> PXSHIFT(1);
                        }

                        if (*p != 0) {
                            /* Already mapped — release the PTE ref. */
                            unused_refs++;
                        } else {
                            *p = mk_pte((uint64)ip[j],
                                        vma->flags & ~PROT_WRITE);
                            pte_mkclean(p);
                            batch_faults++;
                        }
                    }
                    /* Release unused PTE refs in batch. */
                    if (unused_refs > 0)
                        __page_ref_sub(pcpage, unused_refs);
                    if (batch_faults > 0) {
                        g_vm_file_faults += batch_faults;
                        vm_account_resident_add(vma, (uint64)batch_faults);
                    }
                    /* No TLB flush needed: we're installing PTEs for
                     * previously-unmapped addresses (PTE was 0).
                     * Non-present entries are not cached in TLB. */
                    vm_pgtable_unlock(vma->vm);

                    pcache_put_page(__pc, pcpage);

                    /* Advance past the batch-installed pages. */
                    fault_va = iv[nc - 1] + PGSIZE;
                }

                /* Per-page fallback for pages the batch couldn't handle. */
                for (; fault_va < fault_end; fault_va += PGSIZE) {
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
                    g_vm_file_faults += 1;
                    if (vma->file->ops != NULL && vma->file->ops->fault != NULL)
                        pa = vma->file->ops->fault(vma->file, vma, fault_va);
                    else
                        pa = __vma_fault_file_page(vma, fault_va);
                    if (pa == NULL)
                        return -ENOMEM;

                    page_t *fault_pg = __pa_to_page((uint64)pa);
                    page_t *fault_pc_head = page_pcache_head(fault_pg);
                    int is_pfnmap = (vma->flags & VMA_FLAG_PFNMAP) != 0;
                    int is_pcache = !is_pfnmap && (fault_pc_head != NULL);

                    vm_pgtable_lock(vma->vm);
                    pte = walk(vma->vm->pagetable, fault_va, 1, NULL, NULL);
                    if (pte == NULL) {
                        vm_pgtable_unlock(vma->vm);
                        if (is_pcache)
                            pcache_put_folio(fault_pc_head->pcache.pcache,
                                             page_folio(fault_pc_head));
                        else if (!is_pfnmap && vma->file->ops != NULL &&
                                 vma->file->ops->fault != NULL)
                            page_ref_dec(pa);
                        else if (!is_pfnmap)
                            page_free(pa, 0);
                        return -ENOMEM;
                    }
                    if (*pte != 0) {
                        if (is_pcache)
                            pcache_put_folio(fault_pc_head->pcache.pcache,
                                             page_folio(fault_pc_head));
                        else if (!is_pfnmap && vma->file->ops != NULL &&
                                 vma->file->ops->fault != NULL)
                            page_ref_dec(pa);
                        else if (!is_pfnmap)
                            page_free(pa, 0);
                    } else {
                        uint64 pte_flags = vma->flags;
                        if (is_pcache)
                            pte_flags &= ~PROT_WRITE;
                        *pte = mk_pte((uint64)pa, pte_flags);
                        if (is_pcache)
                            pte_mkclean(pte);
                        vm_account_resident_add(vma, 1);
                        /* No TLB flush: PTE was 0, non-present entries
                         * are not cached in TLB. */
                    }
                    vm_pgtable_unlock(vma->vm);
                }
                vm_pgtable_lock(vma->vm);
                __l0_rgn = ~0ULL; /* L0 cache stale after lock re-acquire */
                i = fault_end - PGSIZE;
                continue;
            }
__do_pte_check:
            {
                int pte_ret = __vma_validate_pte(vma, pte, flags, i);
                if (pte_ret != 0) {
                    vm_pgtable_unlock(vma->vm);
                    return -EFAULT;
                }
            }
            continue;
        }

        /* Anonymous VMA path — use L0 cache */
        pte_t *pte;
        uint64 __rgn = i >> PXSHIFT(1);
        if (__rgn == __l0_rgn) {
            pte = &__l0_base[PX(0, i)];
        } else {
            pte = walk(vma->vm->pagetable, i, 1, NULL, NULL);
            if (pte == NULL) {
                vm_pgtable_unlock(vma->vm);
                return -ENOMEM;
            }
            __l0_base = pte - PX(0, i);
            __l0_rgn = __rgn;
        }
        if (__vma_validate_pte(vma, pte, flags, i) != 0) {
            vm_pgtable_unlock(vma->vm);
            return -EFAULT;
        }
    }

    /* Single bulk TLB flush: replaces per-page INVLPG calls in the
     * validate PTE helpers.  Safe because we hold vm_pgtable_lock,
     * so no user-space access can occur between PTE modifications
     * and this flush. */
    sfence_vma();
    vm_pgtable_unlock(vma->vm);
    g_vm_vma_validate_ticks += r_time() - validate_start;
    return 0;
}

/* ========================================================================== */
/*  User memory accessors                                                     */
/* ========================================================================== */

uint64 g_vm_copyout_fast_hits = 0;
uint64 g_vm_copyout_fast_bytes = 0;

static bool vm_copyout_fast_src(const void *src, uint64 len,
                                const void **src_safe)
{
    uint64 start = (uint64)src;
    uint64 end = start + len;

    if (end < start)
        return false;

    if (start >= PAGE_OFFSET) {
        uint64 pa_start = start - PAGE_OFFSET;
        uint64 pa_end = end - PAGE_OFFSET;
        if (pa_start >= KERNBASE && pa_end <= PHYSTOP) {
            *src_safe = src;
            return true;
        }
        return false;
    }

    /*
     * The fast path runs while the process page table is loaded.  Only the
     * higher-half direct map is guaranteed there; low-half kernel-VM mappings
     * from kvmalloc() are only present in the kernel page table and must use
     * the slow path.
     */
    if (start >= KERNBASE && end <= PHYSTOP) {
        *src_safe = (const void *)(start + PAGE_OFFSET);
        return true;
    }

    return false;
}

int vm_copyout(vm_t *vm, uint64 dstva, const void *src, uint64 len)
{
    uint64 copy_start = r_time();
    g_vm_copyout_calls += 1;
    g_vm_copyout_bytes += len;

    uint64 n, va0, pa0;
    uint64 validated_end = 0;
    vma_t *vma = NULL;
    pte_t *pte;
    int ret = 0;

    vm_rlock(vm);

    /*
     * Fast path: switch CR3 to the user page table and copy directly
     * into user VAs instead of per-page software walks.  Processes the
     * copy in per-VMA chunks (the buffer may span multiple VMAs).
     *
     * Interrupts are disabled while the user CR3 is loaded because the
     * trap handler unconditionally switches CR3 to the kernel page table
     * on entry and does NOT restore it for kernel-mode traps.
     *
     * IMPORTANT (discovered during -O0 → -O2 migration):
     * All destination pages must have valid PTEs before we enter the
     * interrupt-disabled CR3-switched section.  If a page is not yet
     * mapped (demand-paging), a page fault would fire with interrupts
     * off.  The trap handler switches CR3 to the kernel page table but
     * does NOT restore the user CR3 when returning from a kernel-mode
     * fault, so the faulting memmove resumes under the kernel CR3 where
     * the user VA is unmapped — causing an infinite page-fault loop
     * (system hang).  At -O0 this never triggered because the slower
     * code path always let vma_validate() install pages before the PTE
     * walk checked them; at -O2, compiler reordering and inlining could
     * expose windows where pages weren't yet resident.
     *
     * The fix: after vma_validate(), explicitly walk the page table to
     * confirm every page in the chunk has PTE_V set.  If any is missing,
     * we fall through to the slow path which handles faults safely with
     * interrupts enabled.
     *
     * Performance (ext4, QEMU virtio-blk, 16 MB sequential I/O):
     *   -O0 baseline:  WRITE ~50 MB/s,  READ ~32 MB/s
     *   -O2 + fast path + PTE check: WRITE ~135 MB/s, READ ~91 MB/s
     */
#ifdef __x86_64__
    if (len > 0 && current != NULL && current->vm == vm &&
        !vm->is_kernel && dstva < UVMTOP && dstva + len <= UVMTOP) {
        uint64 fdst = dstva;
        const void *fsrc = src;
        uint64 flen = len;
        bool fast_ok = true;

        while (flen > 0) {
            uint64 sp = PGROUNDDOWN(fdst);
            vma_t *fvma = vm_find_area(vm, sp);
            if (fvma == NULL) {
                fast_ok = false; break;
            }

            /* Clamp chunk to this VMA. */
            uint64 chunk = flen;
            if (fdst + chunk > fvma->end)
                chunk = fvma->end - fdst;
            uint64 ep = PGROUNDUP(fdst + chunk);
            if (ep > fvma->end)
                ep = fvma->end;

            if (vma_validate(fvma, sp, ep - sp,
                             VMA_FLAG_USER | PROT_WRITE) != 0) {
                fast_ok = false;
                break;
            }

            /* Verify all user pages are resident before switching CR3;
             * a page fault with interrupts disabled would deadlock
             * because the trap handler switches to the kernel CR3
             * but does not restore the user CR3 on return to the
             * faulting instruction. */
            for (uint64 pg = sp; pg < ep; pg += PGSIZE) {
                pte_t *ppte = walk(vm->pagetable, pg, 0, NULL, NULL);
                if (ppte == NULL || !(*ppte & PTE_V)) {
                    fast_ok = false;
                    break;
                }
            }
            if (!fast_ok) break;

            /* Convert physical/direct-map sources for access under user CR3. */
            const void *src_safe;
            if (!vm_copyout_fast_src(fsrc, chunk, &src_safe)) {
                fast_ok = false;
                break;
            }

            /*
             * The fast path copies while running on the target user CR3.  The
             * destination check above is not enough: kernel stack/direct-map
             * source bytes must also be mapped in that page table, otherwise a
             * small copyout (for example waitpid status) can fault with
             * interrupts disabled.  Check only the shared PML4 slots here:
             * the direct map uses static/huge lower-level tables that walk()
             * intentionally diagnoses when reached from a user page table.
             */
            uint64 sstart = (uint64)src_safe;
            uint64 send = sstart + chunk;
            if (send < sstart) {
                fast_ok = false;
                break;
            }
            if (sstart >= PAGE_OFFSET) {
                uint64 span = 1ULL << PXSHIFT(PAGETABLE_LEVELS - 1);
                uint64 pg = sstart;
                while (pg < send) {
                    pte_t *spte = &vm->pagetable[PX(PAGETABLE_LEVELS - 1, pg)];
                    if ((*spte & PTE_V) == 0) {
                        fast_ok = false;
                        break;
                    }
                    uint64 next = (pg & ~(span - 1)) + span;
                    if (next <= pg || next > send)
                        next = send;
                    pg = next;
                }
                if (!fast_ok)
                    break;
            }

            int was = intr_off_save();
            uint64 kcr3 = r_satp();
            uint64 kcr3_pt = kcr3 & ~CR3_NOFLUSH & ~CR3_PCID_MASK;
            uint64 ucr3_pt = (uint64)vm->pagetable;

            if (kcr3_pt != ucr3_pt) {
                uint64 ucr3;
                if (vm_asid_max() > 0)
                    ucr3 = MAKE_SATP_PCID(ucr3_pt, vm->asid, 0);
                else
                    ucr3 = ucr3_pt;
                w_satp(ucr3);
                memmove((void *)fdst, src_safe, chunk);
                w_satp(kcr3);
            } else {
                memmove((void *)fdst, src_safe, chunk);
            }
            intr_restore(was);

            fdst += chunk;
            fsrc += chunk;
            flen -= chunk;
        }

        if (fast_ok) {
            g_vm_copyout_fast_hits++;
            g_vm_copyout_ticks += r_time() - copy_start;
            vm_runlock(vm);
            return 0;
        }
        /* Fast path failed partway — fall through to slow path
         * for the remaining bytes. */
        dstva = fdst;
        src = fsrc;
        len = flen;
    }
#endif /* __x86_64__ */

    /* Slow path: per-page walk + coalesced memmove (multi-VMA, stack
     * growth, or validation failure on fast path). */
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
        if (pte_is_hugepage(pte))
            pa0 += va0 & (HUGEPAGE_SIZE - 1);
        n = PGSIZE - (dstva - va0);
        if (dstva + n > validated_end)
            n = validated_end - dstva;
        if (n > len)
            n = len;

        /*
         * Coalesce: if subsequent pages are physically contiguous,
         * merge them into a single memmove to avoid per-page overhead.
         */
        uint64 contig = n;
        uint64 prev_pa_end = pa0 + PGSIZE;
        while (contig < len) {
            uint64 next_va = PGROUNDDOWN(dstva + contig);
            if (next_va <= va0 || next_va >= validated_end)
                break;
            pte_t *next_pte = walk(vm->pagetable, next_va, 0, NULL, NULL);
            if (next_pte == NULL || !(*next_pte & PTE_V))
                break;
            uint64 next_pa = pte_pa(next_pte);
            if (next_pa != prev_pa_end)
                break;
            uint64 add = PGSIZE;
            if (contig + add > len)
                add = len - contig;
            if (dstva + contig + add > validated_end)
                add = validated_end - (dstva + contig);
            if (add == 0)
                break;
            contig += add;
            prev_pa_end = next_pa + PGSIZE;
        }
        memmove((void *)(pa0 + (dstva - va0)), src, contig);

        len -= contig;
        src += contig;
        dstva += contig;
    }
out:
    g_vm_copyout_ticks += r_time() - copy_start;
    vm_runlock(vm);
    return ret;
}

int vm_copyin(vm_t *vm, void *dst, uint64 srcva, uint64 len)
{
    uint64 copy_start = r_time();
    g_vm_copyin_calls += 1;
    g_vm_copyin_bytes += len;

    uint64 n, va0, pa0;
    uint64 validated_end = 0;
    vma_t *vma = NULL;
    int ret = 0;
    vm_rlock(vm);

    /*
     * Fast path: single-VMA range — switch to user CR3 and copy
     * directly from user VA instead of per-page software walks.
     *
     * NOTE: Currently disabled (the leading "0 &&").  Before enabling,
     * this needs the same PTE-residency pre-check that vm_copyout()'s
     * fast path has — walk every source page after vma_validate() and
     * bail to the slow path if any PTE is missing.  Without that guard,
     * a demand-paged source page would cause an infinite page-fault loop
     * under the user CR3 with interrupts disabled (see the detailed
     * comment in vm_copyout() above for the full explanation).
     *
     * It should also be converted to a multi-VMA loop like vm_copyout()
     * for buffers that span VMA boundaries.
     */
#ifdef __x86_64__
    if (0 && len > 0 && current != NULL && current->vm == vm &&
        !vm->is_kernel && srcva < UVMTOP && srcva + len <= UVMTOP) {
        uint64 sp = PGROUNDDOWN(srcva);
        uint64 ep = PGROUNDUP(srcva + len);
        vma_t *fvma = vm_find_area(vm, sp);
        if (fvma != NULL && ep <= fvma->end &&
            vma_validate(fvma, sp, ep - sp,
                         VMA_FLAG_USER | PROT_READ) == 0) {
            void *dst_safe = ((uint64)dst < PAGE_OFFSET)
                ? (void *)((uint64)dst + PAGE_OFFSET) : dst;

            int was = intr_off_save();
            uint64 kcr3 = r_satp();
            uint64 kcr3_pt = kcr3 & ~CR3_NOFLUSH & ~CR3_PCID_MASK;
            uint64 ucr3_pt = (uint64)vm->pagetable;

            if (kcr3_pt != ucr3_pt) {
                uint64 ucr3;
                if (vm_asid_max() > 0)
                    ucr3 = MAKE_SATP_PCID(ucr3_pt, vm->asid, 1);
                else
                    ucr3 = ucr3_pt;
                w_satp(ucr3);
                memmove(dst_safe, (const void *)srcva, len);
                w_satp(kcr3);
            } else {
                memmove(dst_safe, (const void *)srcva, len);
            }
            intr_restore(was);

            g_vm_copyin_ticks += r_time() - copy_start;
            vm_runlock(vm);
            return 0;
        }
    }
#endif /* __x86_64__ */

    /* Slow path: per-page walk + coalesced memmove. */
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

        /*
         * Coalesce: if subsequent pages are physically contiguous,
         * merge them into a single memmove to avoid per-page overhead.
         */
        uint64 contig = n;
        uint64 prev_pa_end = pa0 + PGSIZE;
        while (contig < len) {
            uint64 next_va = PGROUNDDOWN(srcva + contig);
            if (next_va <= va0)
                break;
            if (next_va >= validated_end)
                break;
            uint64 next_pa = walkaddr(vm->pagetable, next_va);
            if (next_pa == 0)
                break;
            if (next_pa != prev_pa_end)
                break;
            uint64 add = PGSIZE;
            if (contig + add > len)
                add = len - contig;
            if (srcva + contig + add > validated_end)
                add = validated_end - (srcva + contig);
            if (add == 0)
                break;
            contig += add;
            prev_pa_end = next_pa + PGSIZE;
        }
        memmove(dst, (void *)((uint64)PA2VA(pa0) + (srcva - va0)), contig);

        len -= contig;
        dst += contig;
        srcva += contig;
    }
out:
    g_vm_copyin_ticks += r_time() - copy_start;
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

        char *p = (char *)((uint64)PA2VA(pa0) + (srcva - va0));
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

static int vm_normalize_user_range(uint64 *addr, size_t *size)
{
    uint64 start;
    uint64 end;

    if (addr == NULL || size == NULL || *size == 0)
        return -EINVAL;
    if (*size > (size_t)(~0ULL - *addr))
        return -EINVAL;

    start = PGROUNDDOWN(*addr);
    end = PGROUNDUP(*addr + *size);
    if (end <= start)
        return -EINVAL;

    *addr = start;
    *size = (size_t)(end - start);
    return 0;
}

int vm_mmap_region_locked(vm_t *vm, uint64 start, size_t size, uint64 flags,
                          struct vfs_file *file, uint64 pgoff, void *pa)
{
    if (vm == NULL || vm->pagetable == NULL)
        return -EINVAL;

    if (size == 0 || size > (size_t)(~0ULL - start))
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
        /*
         * pa is already a physical address (from kalloc/page_alloc).
         * Do NOT apply VA2PA() here — that would subtract PAGE_OFFSET
         * from an already-physical address, producing a bogus PA that
         * exceeds MAXPHYADDR and triggers a reserved-bit #PF (errcode
         * 0xc) when the CPU walks the page table.
         * Callers: exec.c boundary page (kalloc), shm.c (kalloc).
         */
        if (mappages(vm->pagetable, vma->start, size, (uint64)pa, pte_flags) !=
            0) {
            assert(vma_free(vm, vma) == 0,
                   "vm_mmap_region_locked: failed to free vma");
            return -ENOMEM;
        }
        vm_account_resident_add(vma, size / PGSIZE);
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

    if (vm_normalize_user_range(&addr, &size) != 0) {
        ret = -EINVAL;
        goto out;
    }

    if (addr < vm->vm_bottom || (addr + size) > vm->vm_top) {
        ret = -ENOMEM;
        goto out;
    }
    if (size == 0) {
        ret = 0;
        goto out;
    }

    uint64 end = addr + size;
    uint64 cur = addr;
    int flush_needed = 0;
    uint64 prot_bits = PROT_READ | PROT_WRITE | PROT_EXEC;

    while (cur < end) {
        vma_t *vma = vm_find_area(vm, cur);
        if (vma == NULL || cur < vma->start) {
            ret = -ENOMEM;
            goto out;
        }

        if (cur > vma->start) {
            vma = vma_split(vma, cur);
            if (vma == NULL) {
                ret = -ENOMEM;
                goto out;
            }
        }

        uint64 seg_end = vma->end < end ? vma->end : end;
        if (seg_end < vma->end) {
            if (vma_split(vma, seg_end) == NULL) {
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
        for (uint64 va = cur; va < seg_end; va += PGSIZE) {
            pte_t *pte = walk(vm->pagetable, va, 0, NULL, NULL);
            if (pte != NULL && pte_present(pte)) {
                /* Handle hugepage: modify the 2MB PTE and skip ahead. */
                if (pte_is_hugepage(pte)) {
                    uint64 mflags = effective_flags;
                    if (mflags & PROT_WRITE) {
                        uint64 pa = pte_pa(pte);
                        page_t *pg = __pa_to_page(pa);
                        if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON) &&
                            page_mapcount(pg) > 1)
                            mflags &= ~PROT_WRITE;
                    }
                    uint64 pa = pte_pa(pte);
                    *pte = mk_pte_huge(pa, mflags);
                    uint64 next = (va & HUGEPAGE_MASK) + HUGEPAGE_SIZE;
                    if (next > va + PGSIZE)
                        va = next - PGSIZE;
                    continue;
                }
                uint64 mflags = effective_flags;
                /* rmap-based COW: if page has mapcount > 1 and we're trying
                 * to grant write, suppress write — the write-fault handler
                 * will resolve it via COW when the page is actually written.
                 * All pcache pages stay write-protected in the PTE so the
                 * first write faults and we can either COW (MAP_PRIVATE) or
                 * mark the backing cache dirty (MAP_SHARED) explicitly. */
                if (mflags & PROT_WRITE) {
                    uint64 pa = pte_pa(pte);
                    page_t *pg = __pa_to_page(pa);
                    if (page_is_pcache(pg)) {
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

        if ((old_flags ^ new_flags) & prot_bits)
            flush_needed = 1;
        cur = seg_end;
    }

    if (flush_needed)
        vm_remote_sfence_range(vm, addr, size);

    ret = 0;
out:
    vm_wunlock(vm);
    return ret;
}

uint64 vm_mremap(vm_t *vm, uint64 old_addr, size_t old_size, size_t new_size,
                 int flags, uint64 new_addr)
{
    uint64 ret = (uint64)-EINVAL;
    uint64 trace_arg_old_addr = old_addr;
    size_t trace_arg_old_size = old_size;
    size_t trace_arg_new_size = new_size;
    uint64 trace_arg_new_addr = new_addr;
    int trace_arg_flags = flags;
    int trace_mremap = webkit_mremap_trace_enabled() &&
                       webkit_mremap_trace_process();
    int source_split = 0;
    uint64 new_location = 0;
    uint64 new_location_end = 0;
    vma_t *new_vma = NULL;
    vm_wlock(vm);

    WEBKIT_MREMAP_TRACE("enter old=0x%lx old_size=0x%lx new_size=0x%lx "
                        "flags=0x%x new_addr=0x%lx",
                        trace_arg_old_addr, trace_arg_old_size,
                        trace_arg_new_size, trace_arg_flags,
                        trace_arg_new_addr);

    if (vm == NULL || vm->pagetable == NULL) {
        WEBKIT_MREMAP_TRACE("fail stage=vm ret=%ld", (int64)ret);
        goto out;
    }

    if (vm_normalize_user_range(&old_addr, &old_size) != 0) {
        WEBKIT_MREMAP_TRACE("fail stage=normalize old=0x%lx old_size=0x%lx "
                            "ret=%ld",
                            trace_arg_old_addr, trace_arg_old_size,
                            (int64)ret);
        goto out;
    }
    if (new_size > ((size_t)-1) - (PGSIZE - 1)) {
        WEBKIT_MREMAP_TRACE("fail stage=new-size-overflow new_size=0x%lx "
                            "ret=%ld",
                            trace_arg_new_size, (int64)ret);
        goto out;
    }
    new_size = PGROUNDUP(new_size);

    if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED)) {
        WEBKIT_MREMAP_TRACE("fail stage=flags flags=0x%x ret=%ld",
                            flags, (int64)ret);
        goto out;
    }
    if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE)) {
        WEBKIT_MREMAP_TRACE("fail stage=fixed-without-maymove flags=0x%x "
                            "ret=%ld",
                            flags, (int64)ret);
        goto out;
    }
    if ((flags & MREMAP_FIXED) && (new_addr & (PGSIZE - 1)) != 0) {
        WEBKIT_MREMAP_TRACE("fail stage=fixed-unaligned new_addr=0x%lx "
                            "ret=%ld",
                            new_addr, (int64)ret);
        goto out;
    }

    if (old_size > (vm->vm_top - vm->vm_bottom) ||
        new_size > (vm->vm_top - vm->vm_bottom)) {
        WEBKIT_MREMAP_TRACE("fail stage=size-bounds old_size=0x%lx "
                            "new_size=0x%lx vm=[0x%lx-0x%lx] ret=%ld",
                            old_size, new_size, vm->vm_bottom, vm->vm_top,
                            (int64)ret);
        goto out;
    }
    if (old_addr + old_size < old_addr) {
        WEBKIT_MREMAP_TRACE("fail stage=old-overflow old=0x%lx "
                            "old_size=0x%lx ret=%ld",
                            old_addr, old_size, (int64)ret);
        goto out;
    }
    if (old_addr < vm->vm_bottom || (old_addr + old_size) > vm->vm_top) {
        WEBKIT_MREMAP_TRACE("fail stage=old-out-of-vm old=[0x%lx-0x%lx] "
                            "vm=[0x%lx-0x%lx] ret=%ld",
                            old_addr, old_addr + old_size, vm->vm_bottom,
                            vm->vm_top, (int64)ret);
        goto out;
    }

    uint64 old_end = old_addr + old_size;
    vma_t *vma = vm_find_area(vm, old_addr);
    if (vma == NULL || old_addr < vma->start || old_end > vma->end) {
        WEBKIT_MREMAP_TRACE("fail stage=source old=[0x%lx-0x%lx] "
                            "source=%p source_range=[0x%lx-0x%lx] "
                            "source_flags=0x%lx ret=%ld",
                            old_addr, old_end, vma,
                            vma != NULL ? vma->start : 0,
                            vma != NULL ? vma->end : 0,
                            vma != NULL ? vma->flags : 0, (int64)ret);
        goto out;
    }

    if (new_size > old_size && !(flags & MREMAP_MAYMOVE) &&
        !(flags & MREMAP_FIXED)) {
        uint64 requested_end = old_addr + new_size;

        if (requested_end < old_addr) {
            WEBKIT_MREMAP_TRACE("fail stage=request-overflow old=0x%lx "
                                "new_size=0x%lx ret=%ld",
                                old_addr, new_size, (int64)ret);
            goto out;
        }

        uint64 expand_start = old_end;
        uint64 expand_size = requested_end - expand_start;
        uint64 check = expand_start;
        void *overlap =
            mt_find(&vm->vm_mt, &check, expand_start + expand_size - 1);

        if (overlap != NULL) {
            ret = (uint64)-ENOMEM;
            vma_t *blocker = (vma_t *)overlap;
            WEBKIT_MREMAP_TRACE("fail stage=expand-blocked-preflight "
                                "old=[0x%lx-0x%lx] want=[0x%lx-0x%lx] "
                                "source=[0x%lx-0x%lx] source_flags=0x%lx "
                                "blocker=%p blocker_range=[0x%lx-0x%lx] "
                                "blocker_flags=0x%lx ret=%ld",
                                old_addr, old_end, old_end, requested_end,
                                vma->start,
                                vma->end, vma->flags, blocker,
                                blocker->start, blocker->end, blocker->flags,
                                (int64)ret);
            goto out;
        }
    }

    if (old_addr > vma->start) {
        vma = vma_split(vma, old_addr);
        if (vma == NULL) {
            ret = (uint64)-ENOMEM;
            WEBKIT_MREMAP_TRACE("fail stage=split-start old=0x%lx ret=%ld",
                                old_addr, (int64)ret);
            goto out;
        }
        source_split = 1;
    }
    if (old_end < vma->end) {
        if (vma_split(vma, old_end) == NULL) {
            ret = (uint64)-ENOMEM;
            WEBKIT_MREMAP_TRACE("fail stage=split-end old_end=0x%lx ret=%ld",
                                old_end, (int64)ret);
            goto out;
        }
        source_split = 1;
    }

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
        if (tail == NULL) {
            ret = (uint64)-ENOMEM;
            goto out;
        }
        if (vma_free(vm, tail) != 0)
            goto out;
        vm_remote_sfence_range(vm, shrink_start, old_size - new_size);
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

    if (!(flags & MREMAP_MAYMOVE)) {
        ret = (uint64)-ENOMEM;
        vma_t *blocker = overlap != NULL ? (vma_t *)overlap : NULL;
        WEBKIT_MREMAP_TRACE("fail stage=expand-blocked old=[0x%lx-0x%lx] "
                            "want=[0x%lx-0x%lx] source_flags=0x%lx "
                            "blocker=%p blocker_range=[0x%lx-0x%lx] "
                            "blocker_flags=0x%lx ret=%ld",
                            old_addr, old_addr + old_size, expand_start,
                            expand_start + expand_size, vma->flags, blocker,
                            blocker != NULL ? blocker->start : 0,
                            blocker != NULL ? blocker->end : 0,
                            blocker != NULL ? blocker->flags : 0,
                            (int64)ret);
        goto out;
    }

    if (flags & MREMAP_FIXED) {
        new_location = new_addr;
        new_location_end = new_location + new_size;
        if (new_location_end < new_location)
            goto out;
        if (new_location < vm->vm_bottom || new_location_end > vm->vm_top)
            goto out;
        if (!(new_location_end <= old_addr ||
              new_location >= old_addr + old_size))
            goto out;
        if (__vm_unmap_range_locked(vm, new_location, new_location_end) != 0)
            goto out;
    } else {
        new_location = vm_find_free_range(vm, new_size, 0);
        if (new_location == 0) {
            ret = (uint64)-ENOMEM;
            goto out;
        }
        new_location_end = new_location + new_size;
    }

    new_vma = vma_alloc(vm, new_location, new_size, vma->flags);
    if (new_vma == NULL) {
        ret = (uint64)-ENOMEM;
        goto out;
    }

    if (vma->file != NULL) {
        new_vma->file = vfs_fdup(vma->file);
        if (new_vma->file == NULL) {
            ret = (uint64)-ENOMEM;
            goto err_new_vma;
        }
        new_vma->pgoff = vma->pgoff;
    }

    if (vma->anon_vma != NULL && anon_vma_fork(new_vma, vma) != 0) {
        ret = (uint64)-ENOMEM;
        goto err_new_vma;
    }

    for (uint64 offset = 0; offset < old_size;) {
        pte_t *old_pte = walk(vm->pagetable, old_addr + offset, 0, NULL, NULL);
        if (old_pte != NULL && pte_present(old_pte)) {
            uint64 pa = pte_pa(old_pte);

            if (pte_is_hugepage(old_pte)) {
                if (((new_location + offset) & (HUGEPAGE_SIZE - 1)) != 0)
                    goto err_new_vma;
                /* 2MB hugepage: copy PMD entry to new location. */
                pte_t *new_pmd =
                    walk_pmd(vm->pagetable, new_location + offset, 1);
                if (new_pmd == NULL) {
                    ret = (uint64)-ENOMEM;
                    goto err_new_vma;
                }
                if (*new_pmd != 0)
                    goto err_new_vma;
                *new_pmd = *old_pte;
                if (page_ref_inc((void *)pa) <= 0) {
                    ret = (uint64)-ENOMEM;
                    goto err_new_vma;
                }
                page_t *pg = __pa_to_page(pa);
                if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON))
                    page_add_anon_rmap(pg, new_vma, new_location + offset);
                offset += HUGEPAGE_SIZE;
                continue;
            }

            pte_t *new_pte =
                walk(vm->pagetable, new_location + offset, 1, NULL, NULL);
            if (new_pte == NULL) {
                ret = (uint64)-ENOMEM;
                goto err_new_vma;
            }
            if (*new_pte != 0)
                goto err_new_vma;

            /* Copy the mapping first and hold an extra page reference.
             * The old VMA remains intact until the new one is fully built,
             * so failures can unwind by freeing the new VMA only. */
            *new_pte = *old_pte;
            if (page_ref_inc((void *)pa) <= 0) {
                ret = (uint64)-ENOMEM;
                goto err_new_vma;
            }
            page_t *pg = __pa_to_page(pa);
            /* Only update rmap for HEAD pages (PAGE_TYPE_ANON).
             * Tail pages are skipped — their union stores head_page. */
            if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_ANON)) {
                page_add_anon_rmap(pg, new_vma, new_location + offset);
            }
        }
        offset += PGSIZE;
    }

    if (vma == vm->heap)
        vm->heap = new_vma;
    if (vma == vm->stack)
        vm->stack = new_vma;

    vma_free(vm, vma);
    vm_remote_sfence_range(vm, old_addr, old_size);

    if (new_vma->flags & PROT_EXEC)
        vm_remote_fence_i(vm);

    ret = new_location;
    goto out;

err_new_vma:
    if (new_vma != NULL)
        vma_free(vm, new_vma);
out:
    if ((int64)ret < 0 && source_split && vma != NULL)
        vma = __vma_try_merge_neighbors(vma);
    WEBKIT_MREMAP_TRACE("return ret=%ld arg_old=0x%lx arg_old_size=0x%lx "
                        "arg_new_size=0x%lx arg_flags=0x%x arg_new_addr=0x%lx",
                        (int64)ret, trace_arg_old_addr, trace_arg_old_size,
                        trace_arg_new_size, trace_arg_flags,
                        trace_arg_new_addr);
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

    if (vm_normalize_user_range(&addr, &size) != 0) {
        ret = -EINVAL;
        goto out;
    }

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
            uint64 file_off = vma->pgoff + (addr - vma->start);
            if (file->ops != NULL && file->ops->fsync != NULL)
                ret = file->ops->fsync(file, (loff_t)file_off,
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

static int __vm_madvise_update_flags(vm_t *vm, uint64 addr, size_t size,
                                     uint64 set_flags, uint64 clear_flags)
{
    uint64 end = addr + size;
    uint64 cur = addr;

    while (cur < end) {
        vma_t *vma = vm_find_area(vm, cur);
        if (vma == NULL || cur < vma->start)
            return -ENOMEM;
        if (cur > vma->start) {
            vma = vma_split(vma, cur);
            if (vma == NULL)
                return -ENOMEM;
        }

        uint64 seg_end = vma->end < end ? vma->end : end;
        if (seg_end < vma->end && vma_split(vma, seg_end) == NULL)
            return -ENOMEM;

        vma->flags &= ~clear_flags;
        vma->flags |= set_flags;
        vma = __vma_try_merge_neighbors(vma);
        cur = seg_end;
    }

    return 0;
}

static int __vm_madvise_check_mapped(vm_t *vm, uint64 addr, size_t size)
{
    uint64 end = addr + size;
    uint64 cur = addr;

    while (cur < end) {
        vma_t *vma = vm_find_area(vm, cur);
        if (vma == NULL || cur < vma->start)
            return -ENOMEM;
        cur = vma->end < end ? vma->end : end;
    }

    return 0;
}

static int __vm_madvise_populate(vm_t *vm, uint64 addr, size_t size,
                                 uint64 access)
{
    uint64 end = addr + size;
    uint64 cur = addr;

    while (cur < end) {
        vma_t *vma = vm_find_area(vm, cur);
        if (vma == NULL || cur < vma->start)
            return -ENOMEM;
        uint64 seg_end = vma->end < end ? vma->end : end;
        int ret = vma_validate(vma, cur, seg_end - cur,
                               VMA_FLAG_USER | access);
        if (ret != 0)
            return ret;
        cur = seg_end;
    }

    return 0;
}

static int __vm_madvise_dontneed(vm_t *vm, uint64 addr, size_t size)
{
    uint64 end = addr + size;
    uint64 cur = addr;

    while (cur < end) {
        vma_t *vma = vm_find_area(vm, cur);
        if (vma == NULL || cur < vma->start)
            return -ENOMEM;
        uint64 seg_end = vma->end < end ? vma->end : end;
        int shared_file_wb =
            (vma->file != NULL) && (vma->flags & VMA_FLAG_SHARED) &&
            (vma->flags & VMA_FLAG_FILE);

        vm_pgtable_lock(vm);
        for (uint64 va = cur; va < seg_end; va += PGSIZE) {
            pte_t *pte = walk(vm->pagetable, va, 0, NULL, NULL);
            if (pte != NULL && pte_present(pte)) {
                uint64 pa = pte_pa(pte);
                if (pa != 0) {
                    int was_dirty = pte_dirty(pte);
                    pte_clear(pte);

                    if (shared_file_wb && was_dirty) {
                        vm_pgtable_unlock(vm);
                        __vma_writeback_dirty_page(vma, va, pa);
                        vm_pgtable_lock(vm);
                    }

                    page_t *pg = __pa_to_page(pa);
                    page_t *pc_head = page_pcache_head(pg);
                    if (pc_head != NULL) {
                        folio_t *folio = page_folio(pc_head);
                        pcache_put_folio(pc_head->pcache.pcache, folio);
                    } else {
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
        cur = seg_end;
    }

    vm_remote_sfence_range(vm, addr, size);
    return 0;
}

int vm_madvise(vm_t *vm, uint64 addr, size_t size, int advice)
{
    int ret = 0;

    if (vm == NULL || vm->pagetable == NULL) {
        return -EINVAL;
    }

    vm_wlock(vm);

    if (vm_normalize_user_range(&addr, &size) != 0) {
        ret = -EINVAL;
        goto out;
    }

    if (addr < vm->vm_bottom || (addr + size) > vm->vm_top) {
        ret = -ENOMEM;
        goto out;
    }
    ret = __vm_madvise_check_mapped(vm, addr, size);
    if (ret != 0)
        goto out;

    switch (advice) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
    case MADV_COLD:
    case MADV_COLLAPSE:
        ret = 0;
        break;

    case MADV_DONTNEED:
    case MADV_DONTNEED_LOCKED:
    case MADV_PAGEOUT:
        ret = __vm_madvise_dontneed(vm, addr, size);
        break;

    case MADV_FREE:
        ret = __vm_madvise_dontneed(vm, addr, size);
        break;

    case MADV_DONTFORK:
        ret = __vm_madvise_update_flags(vm, addr, size, VMA_FLAG_DONTFORK, 0);
        break;

    case MADV_DOFORK:
        ret = __vm_madvise_update_flags(vm, addr, size, 0, VMA_FLAG_DONTFORK);
        break;

    case MADV_DONTDUMP:
        ret = __vm_madvise_update_flags(vm, addr, size, VMA_FLAG_DONTDUMP, 0);
        break;

    case MADV_DODUMP:
        ret = __vm_madvise_update_flags(vm, addr, size, 0, VMA_FLAG_DONTDUMP);
        break;

    case MADV_WIPEONFORK:
        ret = __vm_madvise_update_flags(vm, addr, size,
                                        VMA_FLAG_WIPEONFORK,
                                        VMA_FLAG_DONTFORK);
        break;

    case MADV_KEEPONFORK:
        ret = __vm_madvise_update_flags(vm, addr, size, 0,
                                        VMA_FLAG_WIPEONFORK);
        break;

    case MADV_MERGEABLE:
        ret = __vm_madvise_update_flags(vm, addr, size, VMA_FLAG_MERGEABLE, 0);
        break;

    case MADV_UNMERGEABLE:
        ret = __vm_madvise_update_flags(vm, addr, size, 0, VMA_FLAG_MERGEABLE);
        break;

    case MADV_HUGEPAGE:
        ret = __vm_madvise_update_flags(vm, addr, size, VMA_FLAG_HUGEPAGE,
                                        VMA_FLAG_NOHUGEPAGE);
        break;

    case MADV_NOHUGEPAGE:
        ret = __vm_madvise_update_flags(vm, addr, size, VMA_FLAG_NOHUGEPAGE,
                                        VMA_FLAG_HUGEPAGE);
        break;

    case MADV_POPULATE_READ:
        ret = __vm_madvise_populate(vm, addr, size, PROT_READ);
        break;

    case MADV_POPULATE_WRITE:
        ret = __vm_madvise_populate(vm, addr, size, PROT_WRITE);
        break;

    case MADV_REMOVE:
        ret = -ENOSYS;
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
    uint64 heap_end = vm->heap ? vm->heap->end : vm->vm_bottom;
    uint64 reserve_end = PGROUNDUP(vm->heap_reserve_end);
    uint64 effective_bottom = (reserve_end > heap_end) ? reserve_end : heap_end;
    effective_bottom = PGROUNDUP(effective_bottom);

    /* Reserve the entire potential stack growth region.  The stack can grow
     * down to USTACK_MAX_BOTTOM, so mmap allocations must stay below that
     * to avoid blocking future stack expansion. */
    uint64 search_top = USTACK_MAX_BOTTOM;
    uint64 search_bottom = effective_bottom;

    if (search_top <= search_bottom + size) {
        printf("vm_find_free_range: FAIL bounds check size=0x%lx "
               "search_bottom=0x%lx search_top=0x%lx "
               "stack_bottom=0x%lx heap_end=0x%lx reserve_end=0x%lx\n",
               size, search_bottom, search_top,
               stack_bottom, heap_end, reserve_end);
        return 0;
    }

    /*
     * mmap(addr, ..., !MAP_FIXED) treats addr as a placement hint.  Several
     * allocators and shared-memory users choose adjacent hints so a later
     * mremap(..., flags=0) can grow in place.  Ignoring the hint entirely
     * scatters mappings and turns those legal growth attempts into avoidable
     * ENOMEM failures.
     */
    if (hint != 0 && hint <= (uint64)-1 - (PGSIZE - 1)) {
        uint64 hstart = PGROUNDUP(hint);
        uint64 hend = hstart + size;

        if (hend > hstart && hstart >= search_bottom && hend <= search_top) {
            uint64 idx = hstart;
            void *conflict = mt_find(&vm->vm_mt, &idx, hend - 1);
            if (conflict == NULL)
                return hstart;
        }
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
        if (!vma_tree_entry_valid(vm, conflict)) {
            printf("vm_find_free_range: invalid maple conflict entry=%p "
                   "addr=0x%lx size=0x%lx search=[0x%lx,0x%lx]\n",
                   conflict, mas.index, size, search_bottom, search_top - 1);
            return 0;
        }
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
            if (!vma_tree_entry_valid(vm, dump_entry)) {
                printf("  VMA[%d]: INVALID entry=%p idx=0x%lx\n",
                       count, dump_entry, dump_idx);
                count++;
                continue;
            }
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
        vma_t *tail = NULL;
        int is_heap = 0;
        int is_stack = 0;
        int split_left = 0;
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
        is_heap = (vma == vm->heap);
        is_stack = (vma == vm->stack);
        if (vma->start < start) {
            vma_t *right = vma_split(vma, start);
            if (right == NULL)
                return -ENOMEM;
            split_left = 1;
            vma = right;
        }
        if (vma->end > end) {
            tail = vma_split(vma, end);
            if (tail == NULL)
                return -ENOMEM;
        }
        uint64 next = vma->end;
        if (is_heap && !split_left)
            vm->heap = tail;
        if (is_stack && !split_left)
            vm->stack = tail;
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
    if ((flags & MAP_FIXED) && (flags & MAP_FIXED_NOREPLACE))
        return (uint64)-EINVAL;

    if (flags & MAP_ANONYMOUS) {
        file = NULL;
    } else if (fd != -1) {
        if (offset & (PGSIZE - 1))
            return (uint64)-EINVAL;
        file = vfs_fdtable_get_file(current->fdtable, fd);
        if (file == NULL)
            return (uint64)-EBADF;
        struct vfs_inode *inode = vfs_inode_deref(&file->inode);
        if ((inode == NULL || !S_ISREG(inode->mode)) &&
            (file->ops == NULL || file->ops->fault == NULL)) {
            vfs_fput(file);
            return (uint64)-ENODEV;
        }
    } else if (!(flags & MAP_ANONYMOUS)) {
        return (uint64)-EBADF;
    }

    size_t map_length = PGROUNDUP(length);

    uint64 vm_flags =
        VMA_FLAG_USER | (prot & (PROT_READ | PROT_WRITE | PROT_EXEC));
    if (file != NULL)
        vm_flags |= VMA_FLAG_FILE;
    if (flags & MAP_SHARED)
        vm_flags |= VMA_FLAG_SHARED;

    vm_wlock(vm);

    uint64 map_addr;
    if ((flags & MAP_FIXED_NOREPLACE) != 0) {
        if (addr == 0 || (addr & (PGSIZE - 1))) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-EINVAL;
        }
        map_addr = addr;
        uint64 map_end = map_addr + map_length;
        if (map_end <= map_addr || map_addr < vm->vm_bottom ||
            map_end > vm->vm_top) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-ENOMEM;
        }
        uint64 check = map_addr;
        if (mt_find(&vm->vm_mt, &check, map_end - 1) != NULL) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-EEXIST;
        }
    } else if (addr == 0 || (flags & MAP_FIXED) == 0) {
        map_addr = vm_find_free_range(vm, map_length, addr);
        if (map_addr == 0) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-ENOMEM;
        }
    } else {
        if (addr & (PGSIZE - 1)) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-EINVAL;
        }
        map_addr = addr;
        uint64 map_end = map_addr + map_length;
        if (__vm_unmap_range_locked(vm, map_addr, map_end) != 0) {
            vm_wunlock(vm);
            if (file != NULL)
                vfs_fput(file);
            return (uint64)-EINVAL;
        }
    }

    vma_t *vma = vma_alloc(vm, map_addr, map_length, vm_flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        if (file != NULL)
            vfs_fput(file);
        return (uint64)-ENOMEM;
    }

    if (file != NULL) {
        vma->file = file;
        vma->pgoff = offset;
        if (file->ops != NULL && file->ops->mmap != NULL) {
            int mmap_ret = file->ops->mmap(file, vma);
            if (mmap_ret != 0) {
                vma_free(vm, vma);
                vm_wunlock(vm);
                return (uint64)mmap_ret;
            }
        }
    }

    vma = __vma_try_merge_neighbors(vma);

    if (file != NULL && !(vm_flags & PROT_WRITE) &&
        (vm_flags & (PROT_READ | PROT_EXEC))) {
        uint64 eager_flags = VMA_FLAG_USER | (vm_flags & (PROT_READ | PROT_EXEC));
        uint64 eager_len = map_length;
        uint64 eager_max = VM_MMAP_EAGER_PAGES * PGSIZE;

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

    if (vm_normalize_user_range(&addr, &length) != 0)
        return -EINVAL;

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
    vm->asid = ASID_KERNEL;
    vm->asid_gen = 0;

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
    vma_t *existing = vm_find_area(vm, start);
    if (existing != NULL && existing->start <= start &&
        existing->end >= start + size) {
        vm_wunlock(vm);
        return 0;
    }
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
    for (uint64 a = addr; a < addr + size; a += PGSIZE)
        sfence_vma_page(a);
    vm_remote_sfence_range(kernel_vm, addr, size);
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
 * come from the slab/buddy allocator; everything else is from the kernel VM.
 * Check both the identity-mapped range (PA, used on x86 and RISC-V) and the
 * higher-half range (PA2VA, for any higher-half kernel pointers). */
int is_kvm_addr(const void *addr)
{
    uint64 a = (uint64)addr;
    /* Identity-mapped range (VA == PA) */
    if (a >= KERNBASE && a < PHYSTOP)
        return 0;
    /* Higher-half mapped range */
    if (a >= (uint64)PA2VA(KERNBASE) && a < (uint64)PA2VA(PHYSTOP))
        return 0;
    return 1;
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
