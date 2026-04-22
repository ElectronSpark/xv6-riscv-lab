/**
 * tmpfs file operations
 *
 * This file implements the VFS file operations for tmpfs regular files.
 *
 * LOCKING DESIGN: DRIVER-MANAGED INODE LOCKS
 * ==========================================
 * VFS file operations (vfs_fileread, vfs_filewrite, etc.) do NOT acquire
 * the inode lock before calling into the driver. Instead, each driver
 * callback is responsible for acquiring the inode lock when needed.
 *
 * For tmpfs, we acquire the inode lock to protect size and data access.
 * Unlike xv6fs, tmpfs doesn't have transactions, so the locking is simpler.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include <smp/atomic.h>
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "lock/rwsem.h"
#include "lock/completion.h"
#include "proc/thread.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include <mm/vm_types.h>
#include <mm/pgtable.h>
#include <mm/pcache.h>
#include <mm/page.h>
#include "xarray.h"
#include <mm/folio.h>
#include "tmpfs_private.h"
#include "vfs/uio.h"

/******************************************************************************
 * tmpfs pcache operations
 *
 * For tmpfs (backendless file system), the pcache IS the backing store.
 * - read_page: Zero-fill the page (for holes/first access)
 * - write_page: No-op (data stays in memory, no disk to persist to)
 *****************************************************************************/

/* Convert block size to 512-byte units for pcache */
#define PCACHE_BLKS_PER_PAGE (PGSIZE / 512)

static int tmpfs_pcache_read_page(struct pcache *pcache, page_t *page) {
    struct pcache_node *pcnode = page->pcache.pcache_node;
    // Zero-fill the page - for tmpfs, unwritten data is zeros
    memset(pcnode->data, 0, PGSIZE);
    return 0;
}

static int tmpfs_pcache_write_page(struct pcache *pcache, page_t *page) {
    // No-op for tmpfs - data stays in memory, nothing to persist
    (void)pcache;
    (void)page;
    return 0;
}

static int tmpfs_pcache_read_folio(struct pcache *pcache, folio_t *folio) {
    (void)pcache;
    page_t *page = &folio->page;
    struct pcache_node *pcnode = page->pcache.pcache_node;
    /* Zero-fill the entire folio (may be multi-page). */
    memset(pcnode->data, 0, pcnode->size);
    return 0;
}

static int tmpfs_pcache_write_folio(struct pcache *pcache, folio_t *folio) {
    /* No-op for tmpfs — data stays in memory, nothing to persist. */
    (void)pcache;
    (void)folio;
    return 0;
}

static struct pcache_ops tmpfs_pcache_ops = {
    .read_page = tmpfs_pcache_read_page,
    .write_page = tmpfs_pcache_write_page,
    .read_folio = tmpfs_pcache_read_folio,
    .write_folio = tmpfs_pcache_write_folio,
};

/*
 * Initialize the embedded per-inode pcache (i_data) for tmpfs.
 * Call once for every regular-file inode after deciding to use pcache.
 */
void tmpfs_inode_pcache_init(struct vfs_inode *inode) {
    struct pcache *pc = &inode->i_data;
    memset(pc, 0, sizeof(*pc));
    pc->ops = &tmpfs_pcache_ops;
    /* blk_count in 512-byte units, rounded up to page boundary */
    pc->blk_count = (TMPFS_MAX_FILE_SIZE / 512 + PCACHE_BLKS_PER_PAGE - 1) &
                    ~(uint64)(PCACHE_BLKS_PER_PAGE - 1);

    int ret = pcache_init(pc);
    if (ret != 0)
        return; /* proceed without pcache */

    /* tmpfs has no backing store — evicting pages loses data permanently.
     * Disable max_pages so pcache_get_page never enters the eviction loop.
     * (max_pages == 0 ⇒ the `if (pcache->max_pages > 0)` guard is false.) */
    pc->max_pages = 0;

    /* pcache_init resets private_data, so set it after init */
    pc->private_data = inode;
}

/*
 * Teardown the per-inode pcache for tmpfs.
 * Call when destroying a regular file inode.
 */
void tmpfs_inode_pcache_teardown(struct vfs_inode *inode) {
    struct pcache *pc = &inode->i_data;
    if (pc->active) {
        pcache_teardown(pc);
    }
}

// Forward declarations
static ssize_t __tmpfs_file_read(struct vfs_file *file, char *buf, size_t count,
                                 bool user);
static ssize_t __tmpfs_file_write(struct vfs_file *file, const char *buf,
                                  size_t count, bool user);
static loff_t __tmpfs_file_llseek(struct vfs_file *file, loff_t offset,
                                  int whence);

static void *__tmpfs_file_fault(struct vfs_file *file, struct vma *vma,
                               uint64 va);
static int __tmpfs_file_writepage(struct vfs_file *file, loff_t offset,
                                 const void *data, size_t len);
static ssize_t __tmpfs_file_readv(struct vfs_file *file, struct iov_iter *iter,
                                  bool user);
static ssize_t __tmpfs_file_writev(struct vfs_file *file, struct iov_iter *iter,
                                   bool user);

struct vfs_file_ops tmpfs_file_ops = {
    .read = __tmpfs_file_read,
    .write = __tmpfs_file_write,
    .llseek = __tmpfs_file_llseek,
    .release = NULL,
    .fsync = NULL,
    .fault = __tmpfs_file_fault,
    .writepage = __tmpfs_file_writepage,
    .readv = __tmpfs_file_readv,
    .writev = __tmpfs_file_writev,
};

/*
 * Refcount-free pcache lookup for tmpfs.
 *
 * For tmpfs, pages are never evicted (max_pages=0) and the caller holds
 * a file reference keeping the inode/pcache alive.  So we can skip the
 * atomic refcount inc/dec entirely and just grab the pcache_node pointer
 * via xa_load.  Only returns a hit when the page is uptodate and idle.
 *
 * Performance note: the standard pcache_get_page path involves:
 *   1. RCU read lock + xa_load
 *   2. atomic_inc_not_zero on refcount
 *   3. pcache spinlock for LRU management (skipped for tmpfs)
 *   4. atomic_dec on put_page
 * This lookup eliminates steps 2–4.  Combined with the fast write/read
 * loops below, this avoids ALL pcache overhead for cached pages
 * (the common case for overwrites and re-reads).
 */
static inline struct pcache_node *__tmpfs_pcache_lookup(struct pcache *pc,
                                                        uint64 blkno_512) {
    uint64 base = blkno_512 & ~(uint64)(PCACHE_BLKS_PER_PAGE - 1);
    uint64 idx = base / PCACHE_BLKS_PER_PAGE;
    struct pcache_node *pcn = (struct pcache_node *)xa_load(&pc->page_map, idx);
    if (pcn != NULL && !xa_is_internal(pcn) && pcn->uptodate &&
        !pcn->io_in_progress)
        return pcn;
    return NULL;
}

/**
 * __tmpfs_copyin_user - fast copy from user VA to kernel buffer
 *
 * Bypasses vma_validate (which walks page tables for demand faulting)
 * and batches page table walks by scanning L0 PTEs directly.
 * Returns 0 on success, -EFAULT if any page is not mapped (caller
 * should fall back to vm_copyin).
 *
 * Performance note: the standard vm_copyin path was ~80% of tmpfs write
 * time.  It calls vma_validate per page (which does a full walk() +
 * spinlock for demand-fault checks), then walkaddr does a second full
 * walk() per page.  This function:
 *   - Does a single walk() to get the L0 PTE pointer, then scans
 *     consecutive L0 PTEs for physically contiguous runs.
 *   - Issues one large memmove per contiguous PA run (up to 2 MB for
 *     hugepages, or N*4KB for consecutive small pages).
 *   - Falls back to vm_copyin on unmapped pages (demand fault needed).
 *
 * Impact: copy phase became 15.5x faster, overall tmpfs write throughput
 * improved from 45 MB/s to 124–137 MB/s (2.9x).
 */
static int __tmpfs_copyin_user(void *dst, uint64 srcva, uint64 len) {
    vm_t *vm = current->vm;
    pagetable_t pgtable = vm->pagetable;

    vm_rlock(vm);

    vma_t *vma = NULL;
    uint64 vma_end = 0;

    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(srcva);

        /* Re-lookup VMA when needed. */
        if (vma == NULL || va0 < vma->start || va0 >= vma_end) {
            vma = vm_find_area(vm, va0);
            if (vma == NULL ||
                (vma->flags & (VMA_FLAG_USER | PROT_READ)) !=
                    (VMA_FLAG_USER | PROT_READ)) {
                vm_runlock(vm);
                return vm_copyin(vm, dst, srcva, len);
            }
            vma_end = vma->end;
        }

        /* Walk the page table once — returns L0 PTE (or L1 for hugepage). */
        pte_t *pte = walk(pgtable, va0, 0, NULL, NULL);
        if (pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_U)) {
            vm_runlock(vm);
            return vm_copyin(vm, dst, srcva, len);
        }

        uint64 pa0 = PTE2PA(*pte);
        uint64 page_off = srcva - va0;
        uint64 contig;

        if (pte_is_hugepage(pte)) {
            /* Hugepage: up to 2MB contiguous. */
            pa0 += va0 & (HUGEPAGE_SIZE - 1);
            contig = HUGEPAGE_SIZE - (srcva & (HUGEPAGE_SIZE - 1));
        } else {
            /* Regular 4KB page — scan consecutive L0 PTEs. */
            contig = PGSIZE - page_off;
            uint64 prev_pa_end = pa0 + PGSIZE;
            int l0_idx = PX(0, va0);
            int scan = 1;
            /* Don't scan past VMA boundary. */
            uint64 scan_limit = vma_end - srcva;
            if (scan_limit > len)
                scan_limit = len;

            while (contig < scan_limit && l0_idx + scan < 512) {
                pte_t next = pte[scan];
                if (!(next & PTE_V) || !(next & PTE_U))
                    break;
                uint64 next_pa = PTE2PA(next);
                if (next_pa != prev_pa_end)
                    break;
                contig += PGSIZE;
                prev_pa_end = next_pa + PGSIZE;
                scan++;
            }
        }

        if (contig > len)
            contig = len;

        memmove(dst, (void *)((uint64)PA2VA(pa0) + page_off), contig);

        dst = (char *)dst + contig;
        srcva += contig;
        len -= contig;
    }

    vm_runlock(vm);
    return 0;
}

/**
 * __tmpfs_copyout_user - fast copy from kernel buffer to user VA
 *
 * Same optimization as __tmpfs_copyin_user but for the read direction.
 * Scans L0 PTEs for contiguous PA runs and issues large memmoves.
 * Falls back to vm_copyout for unmapped pages.
 *
 * Impact: read throughput improved from 135 MB/s to 362–407 MB/s (2.9x).
 */
static int __tmpfs_copyout_user(uint64 dstva, const void *src, uint64 len) {
    vm_t *vm = current->vm;
    pagetable_t pgtable = vm->pagetable;

    vm_rlock(vm);

    vma_t *vma = NULL;
    uint64 vma_end = 0;

    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(dstva);

        if (vma == NULL || va0 < vma->start || va0 >= vma_end) {
            vma = vm_find_area(vm, va0);
            if (vma == NULL ||
                (vma->flags & (VMA_FLAG_USER | PROT_WRITE)) !=
                    (VMA_FLAG_USER | PROT_WRITE)) {
                vm_runlock(vm);
                return vm_copyout(vm, dstva, src, len);
            }
            vma_end = vma->end;
        }

        pte_t *pte = walk(pgtable, va0, 0, NULL, NULL);
        if (pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_U)) {
            vm_runlock(vm);
            return vm_copyout(vm, dstva, src, len);
        }

        uint64 pa0 = PTE2PA(*pte);
        uint64 page_off = dstva - va0;
        uint64 contig;

        if (pte_is_hugepage(pte)) {
            pa0 += va0 & (HUGEPAGE_SIZE - 1);
            contig = HUGEPAGE_SIZE - (dstva & (HUGEPAGE_SIZE - 1));
        } else {
            contig = PGSIZE - page_off;
            uint64 prev_pa_end = pa0 + PGSIZE;
            int l0_idx = PX(0, va0);
            int scan = 1;
            uint64 scan_limit = vma_end - dstva;
            if (scan_limit > len)
                scan_limit = len;

            while (contig < scan_limit && l0_idx + scan < 512) {
                pte_t next = pte[scan];
                if (!(next & PTE_V) || !(next & PTE_U))
                    break;
                uint64 next_pa = PTE2PA(next);
                if (next_pa != prev_pa_end)
                    break;
                contig += PGSIZE;
                prev_pa_end = next_pa + PGSIZE;
                scan++;
            }
        }

        if (contig > len)
            contig = len;

        memmove((void *)((uint64)PA2VA(pa0) + page_off), src, contig);

        src = (const char *)src + contig;
        dstva += contig;
        len -= contig;
    }

    vm_runlock(vm);
    return 0;
}

static ssize_t __tmpfs_file_read(struct vfs_file *file, char *buf, size_t count,
                                 bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }

    loff_t pos = file->f_pos;

    /*
     * Fast path for non-embedded files with an active pcache: read
     * inode->size without the inode lock.  On 64-bit this is a
     * naturally-atomic load.  The file-level lock serializes reads on
     * the same FD, and writes that update inode->size do so atomically
     * under the inode lock.  A concurrent write extending the file may
     * cause us to see a slightly stale size, which POSIX allows
     * (we'd just read fewer bytes this time).
     */
    if (!ti->embedded && pc->active) {
        loff_t size = inode->size;
        if (pos >= size)
            return 0;
        if (pos + count > size)
            count = size - pos;
        goto do_pcache_read;
    }

    // Slow path: acquire inode lock for embedded data or inactive pcache.
    vfs_ilock(inode);

    if (pos >= inode->size) {
        vfs_iunlock(inode);
        return 0; // EOF
    }
    if (pos + count > inode->size) {
        count = inode->size - pos;
    }

    // Handle embedded data
    if (ti->embedded) {
        if (pos + count > TMPFS_INODE_EMBEDDED_DATA_LEN) {
            // This shouldn't happen - embedded files are limited in size
            count = TMPFS_INODE_EMBEDDED_DATA_LEN - pos;
        }
        if (user) {
            if (vm_copyout(current->vm, (uint64)buf, ti->file.data + pos,
                           count) < 0) {
                vfs_iunlock(inode);
                return -EFAULT;
            }
        } else {
            memmove(buf, ti->file.data + pos, count);
        }
        vfs_iunlock(inode);
        return count;
    }

    // pcache-based read — release inode lock early since pcache has its own
    // concurrency control and tmpfs pages cannot be evicted (max_pages==0).
    {
        int is_active = pc->active;
        vfs_iunlock(inode);
        if (!is_active)
            return -EIO;
    }

do_pcache_read:;

    size_t bytes_read = 0;
    while (bytes_read < count) {
        uint64 blkno_512 = (pos / PGSIZE) * PCACHE_BLKS_PER_PAGE;
        struct pcache_node *pcn = __tmpfs_pcache_lookup(pc, blkno_512);
        page_t *page = NULL;

        if (pcn == NULL) {
            /* Slow path: page not yet allocated or not uptodate. */
            page = pcache_get_page(pc, blkno_512);
            if (page == NULL) {
                if (bytes_read == 0)
                    return -EIO;
                return bytes_read;
            }
            int ret = pcache_read_page(pc, page);
            if (ret != 0) {
                pcache_put_page(pc, page);
                if (bytes_read == 0)
                    return -EIO;
                return bytes_read;
            }
            pcn = page->pcache.pcache_node;
        }

        uint64 folio_start = (uint64)pcn->blkno * 512;
        uint64 folio_off = (uint64)pos - folio_start;
        size_t chunk = pcn->size - (size_t)folio_off;
        if (chunk > count - bytes_read)
            chunk = count - bytes_read;
        char *data = (char *)pcn->data + folio_off;

        if (user) {
            if (__tmpfs_copyout_user((uint64)(buf + bytes_read), data,
                                     chunk) < 0) {
                if (page)
                    pcache_put_page(pc, page);
                if (bytes_read == 0)
                    return -EFAULT;
                return bytes_read;
            }
        } else {
            memmove(buf + bytes_read, data, chunk);
        }
        if (page)
            pcache_put_page(pc, page);

        bytes_read += chunk;
        pos += chunk;
    }

    return bytes_read;
}

static ssize_t __tmpfs_file_write(struct vfs_file *file, const char *buf,
                                  size_t count, bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }

    loff_t pos = file->f_pos;
    loff_t end_pos = pos + count;

    /*
     * Fast path for non-embedded files with an active pcache: skip the
     * inode lock entirely.  The file-level lock serializes writes on the
     * same FD.  We only need the inode lock for embedded-data handling
     * and the size-limit check (TMPFS_MAX_FILE_SIZE is large enough that
     * the check can be performed lock-free).
     */
    if (!ti->embedded && pc->active) {
        if (end_pos > TMPFS_MAX_FILE_SIZE)
            return -EFBIG;
        goto do_pcache_write;
    }

    // Acquire inode lock to protect size and data.
    // The file reference guarantees the inode remains allocated.
    vfs_ilock(inode);

    // Check for file size limits
    if (end_pos > TMPFS_MAX_FILE_SIZE) {
        vfs_iunlock(inode);
        return -EFBIG;
    }

    // Handle embedded data
    if (ti->embedded) {
        if (end_pos <= TMPFS_INODE_EMBEDDED_DATA_LEN) {
            // Still fits in embedded storage
            if (user) {
                if (vm_copyin(current->vm, ti->file.data + pos, (uint64)buf,
                              count) < 0) {
                    vfs_iunlock(inode);
                    return -EFAULT;
                }
            } else {
                memmove(ti->file.data + pos, buf, count);
            }
            if (end_pos > inode->size) {
                inode->size = end_pos;
            }
            vfs_iunlock(inode);
            return count;
        }
        // Need to migrate to pcache storage
        int ret = __tmpfs_migrate_to_allocated_blocks(ti);
        if (ret != 0) {
            vfs_iunlock(inode);
            return ret;
        }
    }

    // pcache-based write — release inode lock early since pcache has its own
    // concurrency control and tmpfs pages cannot be evicted (max_pages==0).
    // The file-level lock in vfs_filewrite already serializes writes on the
    // same file descriptor.
    {
        int is_active = pc->active;
        vfs_iunlock(inode);
        if (!is_active)
            return -EIO;
    }

do_pcache_write:;

    size_t bytes_written = 0;
    while (bytes_written < count) {
        uint64 blkno_512 = (pos / PGSIZE) * PCACHE_BLKS_PER_PAGE;

        /*
         * Fast path: if the page is already cached and uptodate (overwrite),
         * skip ALL pcache overhead — no refcount, no locks, just direct
         * memory access.  Safe because tmpfs pages are never evicted
         * (max_pages=0) and our file reference keeps the inode alive.
         */
        struct pcache_node *pcn = __tmpfs_pcache_lookup(pc, blkno_512);
        page_t *page = NULL;
        int need_commit = 0;

        if (pcn == NULL) {
            /* Slow path: page not cached or not uptodate — allocate. */
            page = pcache_get_page(pc, blkno_512);
            if (page == NULL) {
                if (bytes_written == 0)
                    return -ENOMEM;
                goto done;
            }
            pcn = page->pcache.pcache_node;
        }

        uint64 folio_start = (uint64)pcn->blkno * 512;
        uint64 folio_off = (uint64)pos - folio_start;
        size_t chunk = pcn->size - (size_t)folio_off;
        if (chunk > count - bytes_written)
            chunk = count - bytes_written;

        if (page != NULL) {
            /* Slow path: need to prepare the page for writing.
             *
             * Use pcache_begin_full_page_write for both full-folio and
             * partial-folio writes.  For partial writes to new folios (the
             * begin call returns 1), zero-fill only the gaps rather than
             * the entire folio.  This halves the memset cost when the folio
             * is larger than the write (e.g. 128 KB folio, 64 KB write).
             *
             * Safe because unwritten parts of new folios are invisible to
             * readers: inode->size hasn't been updated past the written
             * range yet, and __tmpfs_file_read clamps reads to inode->size.
             */
            bool full_folio = (folio_off == 0 && chunk >= pcn->size);
            int ret = pcache_begin_full_page_write(pc, page);
            if (ret < 0) {
                pcache_put_page(pc, page);
                if (bytes_written == 0)
                    return ret;
                goto done;
            }
            need_commit = ret;
            if (need_commit && !full_folio) {
                /* New folio, partial write: zero-fill the gaps. */
                if (folio_off > 0)
                    memset(pcn->data, 0, folio_off);
                size_t end = folio_off + chunk;
                if (end < pcn->size)
                    memset((char *)pcn->data + end, 0,
                           pcn->size - end);
            }
        }

        char *data = (char *)pcn->data + folio_off;

        if (user) {
            if (__tmpfs_copyin_user(data, (uint64)(buf + bytes_written),
                                    chunk) < 0) {
                if (need_commit)
                    pcache_end_full_page_write(pc, page, false);
                if (page)
                    pcache_put_page(pc, page);
                if (bytes_written == 0)
                    return -EFAULT;
                goto done;
            }
        } else {
            memmove(data, buf + bytes_written, chunk);
        }

        if (need_commit)
            pcache_end_full_page_write(pc, page, true);
        if (page)
            pcache_put_page(pc, page);

        bytes_written += chunk;
        pos += chunk;
    }

    // Update size if we extended the file (re-acquire inode lock briefly)
    if (pos > inode->size) {
        vfs_ilock(inode);
        if (pos > inode->size)
            inode->size = pos;
        vfs_iunlock(inode);
    }

done:
    return bytes_written;
}

static loff_t __tmpfs_file_llseek(struct vfs_file *file, loff_t offset,
                                  int whence) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    loff_t new_pos;

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        // Need to lock inode to safely read size
        vfs_ilock(inode);
        new_pos = inode->size + offset;
        vfs_iunlock(inode);
        break;
    default:
        return -EINVAL;
    }

    if (new_pos < 0) {
        return -EINVAL;
    }

    return new_pos;
}

/******************************************************************************
 * Vectored read (readv)
 *
 * Takes the inode lock once and iterates all segments, avoiding per-segment
 * lock overhead.  Handles both embedded and pcache-backed data.
 ******************************************************************************/

static ssize_t __tmpfs_file_readv(struct vfs_file *file, struct iov_iter *iter,
                                  bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode))
        return -EINVAL;

    vfs_ilock(inode);

    loff_t pos = file->f_pos;
    if (pos >= inode->size) {
        vfs_iunlock(inode);
        return 0;
    }

    ssize_t bytes_read = 0;

    /* Embedded data path */
    if (ti->embedded) {
        while (iter->nr_segs > 0 && iter->count > 0 && pos < inode->size) {
            size_t seg_len = iter->iov->iov_len - iter->iov_off;
            if (seg_len == 0) { iov_iter_advance(iter, 0); continue; }
            uint64 base = iter->iov->iov_base + iter->iov_off;

            if (pos + (loff_t)seg_len > inode->size)
                seg_len = (size_t)(inode->size - pos);
            if (pos + (loff_t)seg_len > TMPFS_INODE_EMBEDDED_DATA_LEN)
                seg_len = TMPFS_INODE_EMBEDDED_DATA_LEN - pos;

            if (user) {
                if (vm_copyout(current->vm, (uint64)base,
                               ti->file.data + pos, seg_len) < 0) {
                    vfs_iunlock(inode);
                    return bytes_read > 0 ? bytes_read : -EFAULT;
                }
            } else {
                memmove((char *)base, ti->file.data + pos, seg_len);
            }
            bytes_read += seg_len;
            pos += seg_len;
            iov_iter_advance(iter, seg_len);
        }
        vfs_iunlock(inode);
        file->f_pos = pos;
        return bytes_read;
    }

    /* pcache path — delegate to generic vectorized helper */
    if (!pc->active) {
        vfs_iunlock(inode);
        return -EIO;
    }

    loff_t isize = inode->size;
    vfs_iunlock(inode);

    ssize_t ret = pcache_readv(pc, iter, &pos, isize, user);

    bytes_read = ret;
    file->f_pos = pos;
    return bytes_read;
}

/******************************************************************************
 * Vectored write (writev)
 *
 * Takes the inode lock once and writes all segments, reducing overhead.
 ******************************************************************************/

static ssize_t __tmpfs_file_writev(struct vfs_file *file, struct iov_iter *iter,
                                   bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode))
        return -EINVAL;

    vfs_ilock(inode);

    loff_t pos = file->f_pos;
    loff_t end_pos = pos + (loff_t)iter->count;

    if (end_pos > TMPFS_MAX_FILE_SIZE) {
        vfs_iunlock(inode);
        return -EFBIG;
    }

    ssize_t bytes_written = 0;

    /* Embedded path — try to stay embedded if possible */
    if (ti->embedded) {
        if (end_pos <= TMPFS_INODE_EMBEDDED_DATA_LEN) {
            while (iter->nr_segs > 0 && iter->count > 0) {
                size_t seg_len = iter->iov->iov_len - iter->iov_off;
                if (seg_len == 0) { iov_iter_advance(iter, 0); continue; }
                uint64 base = iter->iov->iov_base + iter->iov_off;

                if (user) {
                    if (vm_copyin(current->vm, ti->file.data + pos,
                                  (uint64)base, seg_len) < 0) {
                        vfs_iunlock(inode);
                        if (bytes_written == 0) return -EFAULT;
                        goto done;
                    }
                } else {
                    memmove(ti->file.data + pos, (const char *)base, seg_len);
                }
                bytes_written += seg_len;
                pos += seg_len;
                iov_iter_advance(iter, seg_len);
            }
            if (pos > inode->size)
                inode->size = pos;
            vfs_iunlock(inode);
            file->f_pos = pos;
            return bytes_written;
        }
        /* Migrate to pcache storage */
        int ret = __tmpfs_migrate_to_allocated_blocks(ti);
        if (ret != 0) {
            vfs_iunlock(inode);
            return ret;
        }
    }

    /* pcache path — delegate to generic vectorized helper */
    if (!pc->active) {
        vfs_iunlock(inode);
        return -EIO;
    }

    /* Release inode lock before entering pcache loop — pcache_writev will
     * handle page-level locking internally.  We re-acquire afterwards to
     * update inode size if necessary. */
    vfs_iunlock(inode);

    ssize_t ret = pcache_writev(pc, iter, &pos, user);

    if (ret > 0) {
        bytes_written += ret;
        /* Re-acquire lock to update size atomically */
        vfs_ilock(inode);
        if (pos > inode->size)
            inode->size = pos;
        vfs_iunlock(inode);
    } else if (ret < 0 && bytes_written == 0) {
        bytes_written = ret;
    }

done:
    file->f_pos = pos;
    return bytes_written;
}

// Open callback for tmpfs inodes
// Sets up file operations based on inode type
int tmpfs_open(struct vfs_inode *inode, struct vfs_file *file, int f_flags) {
    if (inode == NULL || file == NULL) {
        return -EINVAL;
    }

    if (S_ISREG(inode->mode)) {
        file->ops = &tmpfs_file_ops;
        return 0;
    }

    if (S_ISDIR(inode->mode)) {
        // Directories don't need special file ops - they use dir_iter
        file->ops = &tmpfs_file_ops;
        return 0;
    }

    if (S_ISLNK(inode->mode)) {
        /*
         * Allow opening symlinks with O_NOFOLLOW flag.
         * POSIX requires that symlinks can be opened with O_NOFOLLOW to allow
         * fstat() on the symlink itself (not its target). This is needed by
         * programs like ls and symlinktest that want to stat symlink info.
         */
        file->ops = &tmpfs_file_ops;
        return 0;
    }

    // Character/block devices and pipes are handled by VFS core
    // They should not reach here as vfs_fileopen handles them
    if (S_ISCHR(inode->mode) || S_ISBLK(inode->mode) || S_ISFIFO(inode->mode)) {
        return -EINVAL; // Should be handled by VFS
    }

    return -ENOSYS;
}

/******************************************************************************
 * Page fault handler for file-backed mmap
 *
 * For the pcache path (normal files), returns the pcache page directly
 * (zero-copy).  The caller (vma_validate) detects pcache pages via
 * page_pcache_head(), installs a write-protected PTE, and on unmap
 * __vma_set_free releases the pcache reference via pcache_put_folio_refs.
 *
 * This eliminates both the anonymous page allocation and the data copy
 * that the old implementation performed on every fault.  For tmpfs, the
 * pcache folio is zero-filled on creation (tmpfs_pcache_read_folio), so
 * even partial pages at EOF have correct zero tails.
 *
 * Embedded data (small files inline in the inode) still requires a copy
 * because the data lives inside the inode struct, not in page-allocator
 * pages.
 *
 * @file:  the open file backing the mapping
 * @vma:   the VMA that was faulted in
 * @va:    faulting virtual address (page-aligned)
 * Returns: physical address of the page, or NULL on failure.
 ******************************************************************************/
static void *__tmpfs_file_fault(struct vfs_file *file, struct vma *vma,
                                uint64 va) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return NULL;
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    struct pcache *pc = &inode->i_data;

    // file_off is always page-aligned (both pgoff and va are page-aligned)
    uint64 file_off = vma->pgoff + (va - vma->start);

    vfs_ilock(inode);

    // Entirely beyond EOF — return a zero page
    if (file_off >= (uint64)inode->size) {
        vfs_iunlock(inode);
        void *pa = page_alloc(0, PAGE_TYPE_ANON);
        if (pa == NULL)
            return NULL;
        memset(pa, 0, PGSIZE);
        return pa;
    }

    /* ---- embedded data path (small files inline in the inode) ---- */
    if (ti->embedded) {
        uint64 bytes_to_read = PGSIZE;
        if (file_off + PGSIZE > (uint64)inode->size)
            bytes_to_read = (uint64)inode->size - file_off;

        void *pa = page_alloc(0, PAGE_TYPE_ANON);
        if (pa == NULL) {
            vfs_iunlock(inode);
            return NULL;
        }
        if (file_off < TMPFS_INODE_EMBEDDED_DATA_LEN) {
            uint64 avail = TMPFS_INODE_EMBEDDED_DATA_LEN - file_off;
            if (bytes_to_read > avail)
                bytes_to_read = avail;
            memmove(pa, ti->file.data + file_off, bytes_to_read);
        } else {
            bytes_to_read = 0;
        }
        vfs_iunlock(inode);
        if (bytes_to_read < PGSIZE)
            memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);
        return pa;
    }

    /* ---- pcache path: zero-copy ---- */
    if (!pc->active) {
        vfs_iunlock(inode);
        return NULL;
    }
    vfs_iunlock(inode);

    uint64 block_idx = TMPFS_IBLOCK(file_off);
    uint64 blkno_512 = block_idx * PCACHE_BLKS_PER_PAGE;

    page_t *pcpage = pcache_get_page(pc, blkno_512);
    if (pcpage == NULL)
        return NULL;
    int ret = pcache_read_page(pc, pcpage);
    if (ret != 0) {
        pcache_put_page(pc, pcpage);
        return NULL;
    }

    struct pcache_node *pcn = pcpage->pcache.pcache_node;
    /* Compute sub-page offset within multi-page folio. */
    uint64 folio_byte_off = (uint64)blkno_512 * 512ULL -
                            (uint64)pcn->blkno * 512ULL;

    /*
     * Zero-copy: return the pcache data pointer directly.
     *
     * The reference from pcache_get_page() is transferred to the PTE —
     * do NOT call pcache_put_page().  The caller (vma_validate) detects
     * this as a pcache page via page_pcache_head() and installs a
     * write-protected, clean PTE.  On munmap / exit, __vma_set_free
     * calls pcache_put_folio_refs() to release the reference.
     *
     * For tmpfs, unwritten regions within a folio are zero (read_folio
     * zero-fills the entire folio), so this is correct even for partial
     * pages at EOF — no need to copy + zero-fill the tail.
     */
    return (char *)pcn->data + folio_byte_off;
}

/******************************************************************************
 * File writepage — write a single page of data back to tmpfs via pcache
 *
 * Called by the VM layer when tearing down or syncing a dirty MAP_SHARED
 * mapping.  For tmpfs the pcache IS the backing store (no disk), so we
 * copy the data into the pcache page to make it visible to subsequent
 * reads and mappings.
 *
 * Two data paths:
 *   1. Embedded data (small files inline in inode): copy into ti->file.data.
 *   2. Pcache path: look up the pcache page and memcpy into it.
 *
 * @file:    the open file for the mapping
 * @offset:  byte offset in the file (page-aligned)
 * @data:    physical address of the page data
 * @len:     number of valid bytes (may be < PGSIZE at end-of-file)
 * Returns:  0 on success, negative errno on failure
 ******************************************************************************/
static int __tmpfs_file_writepage(struct vfs_file *file, loff_t offset,
                                 const void *data, size_t len)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return -EIO;
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    struct pcache *pc = &inode->i_data;

    /* ---- embedded data path ---- */
    if (ti->embedded) {
        if ((uint64)offset < TMPFS_INODE_EMBEDDED_DATA_LEN) {
            uint64 avail = TMPFS_INODE_EMBEDDED_DATA_LEN - (uint64)offset;
            size_t copy_len = len < avail ? len : avail;
            memmove(ti->file.data + offset, data, copy_len);
        }
        return 0;
    }

    /* ---- pcache path ---- */
    if (!pc->active)
        return -EIO;

    uint64 block_idx = TMPFS_IBLOCK((uint64)offset);
    uint64 blkno_512 = block_idx * PCACHE_BLKS_PER_PAGE;

    page_t *pcpage = pcache_get_page(pc, blkno_512);
    if (pcpage == NULL)
        return -EIO;

    int ret = pcache_read_page(pc, pcpage);
    if (ret != 0) {
        pcache_put_page(pc, pcpage);
        return -EIO;
    }

    struct pcache_node *pcn = pcpage->pcache.pcache_node;
    uint64 folio_off = (uint64)blkno_512 * 512 -
                       (uint64)pcn->blkno * 512;
    memmove((char *)pcn->data + folio_off, data, len);
    if (len < PGSIZE)
        memset((char *)pcn->data + folio_off + len, 0, PGSIZE - len);

    pcache_mark_page_dirty(pc, pcpage);
    pcache_put_page(pc, pcpage);
    return 0;
}
