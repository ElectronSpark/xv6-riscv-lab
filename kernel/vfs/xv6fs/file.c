/*
 * xv6fs file operations
 *
 * LOCKING DESIGN: DRIVER-MANAGED INODE LOCKS
 * ==========================================
 * VFS file operations (vfs_fileread, vfs_filewrite, etc.) do NOT acquire
 * the inode lock before calling into the driver. Instead, each driver
 * callback is responsible for acquiring the inode lock when needed.
 *
 * This design is necessary because:
 * 1. xv6fs_file_write needs to acquire a transaction (begin_op) BEFORE
 *    locking the inode, to match VFS lock ordering: transaction → superblock →
 * inode
 * 2. If VFS held the inode lock when calling write, and write called begin_op,
 *    it would cause deadlock with other paths that do begin_op → ilock.
 *
 * LOCK ORDERING:
 * - xv6fs_file_write: begin_op → vfs_ilock → work → vfs_iunlock → end_op
 * - xv6fs_file_read: vfs_ilock → read → vfs_iunlock (no transaction needed)
 * - xv6fs_file_llseek: vfs_ilock → read size → vfs_iunlock (for SEEK_END)
 * - xv6fs_file_stat: vfs_ilock → read fields → vfs_iunlock
 *
 * The VFS file lock (per-file mutex) still serializes concurrent operations
 * on the same file descriptor and protects the file position.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include <mm/vm.h>
#include "dev/buf.h"
#include "vfs/fs.h"
#include "vfs/stat.h"
#include "vfs/fcntl.h"
#include "signal.h"
#include "../vfs_private.h"
#include <mm/page.h>
#include "xv6fs_private.h"
#include "vfs/uio.h"
#include <mm/pcache.h>
#include <mm/folio.h>
#include "dev/bio.h"
#include "dev/blkdev.h"
#include "proc/tq.h"

/*
 * Blocks-per-page constants for pcache ↔ xv6fs block address translation.
 * One pcache page (4KB) covers BSIZE_PER_PAGE xv6fs blocks (BSIZE=1024).
 * Block addresses from bmap are in BSIZE units; pcache uses 512-byte units.
 */
#define BSIZE_PER_PAGE (PGSIZE / BSIZE) /* 4 */
#define BLK512_PER_BSIZE (BSIZE / 512)  /* 2 */

/* Number of folios to prefetch ahead for sequential reads. */
#define XV6FS_READAHEAD_FOLIOS 4

/* Max folios to batch-read in one pass (stack arrays must stay reasonable). */
#define XV6FS_READ_BATCH 32

/* Max bios across a batch (non-fragmented: 1 bio per folio; fragmented: more) */
#define XV6FS_READ_MAX_BIOS (XV6FS_READ_BATCH * 4)

/******************************************************************************
 * File read — batch I/O path
 *
 * Instead of reading one folio at a time (synchronous bio per folio), this
 * function allocates a batch of folios, submits all their bios at once, then
 * awaits all completions before copying data.  This lets the virtio ring
 * process many requests concurrently, massively improving throughput.
 ******************************************************************************/

ssize_t xv6fs_file_read(struct vfs_file *file, char *buf, size_t count,
                        bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb =
        container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode))
        return -EINVAL;
    if (!pc->active) {
        printf("xv6fs_file_read: pcache not active ino=%lu\n", inode->ino);
        return -EIO;
    }

    vfs_ilock(inode);
    loff_t isize = inode->size;
    vfs_iunlock(inode);

    loff_t pos = file->f_pos;
    if (pos >= isize) {
        printf("xv6fs_file_read: EOF ino=%lu pos=%ld isize=%ld\n",
               inode->ino, (long)pos, (long)isize);
        return 0;
    }
    size_t orig_count = count;
    if (pos + count > isize)
        count = isize - pos;

    size_t bytes_read = 0;

    while (bytes_read < count) {
        /* ── Phase 1: allocate a batch of folios ── */
        page_t *pages[XV6FS_READ_BATCH];
        int n_pages = 0;
        loff_t scan_pos = pos;

        while (n_pages < XV6FS_READ_BATCH && scan_pos < isize &&
               (size_t)(scan_pos - pos) < (count - bytes_read)) {
            uint bn = scan_pos / BSIZE;
            uint64 blkno_512 = (uint64)bn * BLK512_PER_BSIZE;
            page_t *page = pcache_get_page(pc, blkno_512);
            if (page == NULL)
                break;
            pages[n_pages++] = page;
            struct pcache_node *pcn = page->pcache.pcache_node;
            /* Advance scan_pos to the byte past this folio */
            scan_pos = (uint64)(pcn->blkno + pcn->size / 512) * 512;
        }

        if (n_pages == 0) {
            if (bytes_read > 0)
                return bytes_read;
            printf("xv6fs_file_read: pcache_get_page failed ino=%lu "
                   "pos=%lld isize=%lld blk_count=%lu active=%d\n",
                   inode->ino, (long long)pos, (long long)isize,
                   pc->blk_count, pc->active);
            return signal_pending(current) ? -EINTR : -EIO;
        }

        /* ── Phase 2: submit bios for all non-uptodate folios ── */
        struct bio *bios[XV6FS_READ_MAX_BIOS];
        int n_bios = 0;
        /* Track which pages we submitted I/O for (bitmask via array) */
        uint8 did_io[XV6FS_READ_BATCH] = {0};

        /* Collect non-uptodate folios for merged cross-folio I/O. */
        folio_t *io_folios[XV6FS_READ_BATCH];
        int n_io_folios = 0;

        for (int pi = 0; pi < n_pages; pi++) {
            struct pcache_node *pcn = pages[pi]->pcache.pcache_node;
            if (pcn->uptodate)
                continue;
            if (pcn->io_in_progress) {
                /* Another reader is loading this folio; we'll wait in phase 3
                 * via pcache_read_page fallback. */
                continue;
            }
            /* Mark I/O in progress so concurrent readers wait */
            pcn->io_in_progress = 1;
            did_io[pi] = 1;
            io_folios[n_io_folios++] = page_folio(pages[pi]);
        }

        /* Submit merged bios — contiguous folios share one bio with
         * scatter-gather, reducing virtio request count from N to ~1. */
        if (n_io_folios > 0)
            xv6fs_submit_merged_folio_reads(ip, xv6_sb,
                                            io_folios, n_io_folios,
                                            bios, XV6FS_READ_MAX_BIOS,
                                            &n_bios);

        /* Kick the device once after all batch bios are queued */
        if (n_bios > 0)
            blkdev_kick(xv6_sb->blkdev);

        /* ── Phase 3: await all submitted bios ── */
        for (int i = 0; i < n_bios; i++) {
            bio_await(bios[i]);
            bio_release(bios[i]);
        }

        /* Mark batch-read folios as uptodate and wake any waiters */
        for (int pi = 0; pi < n_pages; pi++) {
            if (!did_io[pi])
                continue;
            struct pcache_node *pcn = pages[pi]->pcache.pcache_node;
            pcn->uptodate = 1;
            pcn->dirty = 0;
            pcn->io_in_progress = 0;
            tq_wakeup_all(&pcn->io_waiters, 0, 0);
        }

        /* For folios that were io_in_progress from another reader, wait now */
        for (int pi = 0; pi < n_pages; pi++) {
            struct pcache_node *pcn = pages[pi]->pcache.pcache_node;
            if (!pcn->uptodate) {
                int ret = pcache_read_page(pc, pages[pi]);
                if (ret != 0) {
                    /* Release remaining pages and return */
                    for (int j = pi; j < n_pages; j++)
                        pcache_put_page(pc, pages[j]);
                    if (bytes_read > 0)
                        return bytes_read;
                    return ret;
                }
            }
        }

        /* ── Phase 4: copy data to user buffer ── */
        for (int pi = 0; pi < n_pages && bytes_read < count; pi++) {
            struct pcache_node *pcn = pages[pi]->pcache.pcache_node;
            uint64 folio_start_byte = (uint64)pcn->blkno * 512;
            uint64 folio_off = (uint64)pos - folio_start_byte;
            size_t avail = pcn->size - (size_t)folio_off;
            size_t n = count - bytes_read;
            if (n > avail)
                n = avail;
            if (pos + (loff_t)n > isize)
                n = (size_t)(isize - pos);

            char *data = (char *)pcn->data + folio_off;
            if (user) {
                int co_ret = vm_copyout(current->vm, (uint64)(buf + bytes_read),
                               data, n);
                if (co_ret < 0) {
                    printf("xv6fs_file_read: vm_copyout failed=%d "
                           "ino=%lu pos=%ld n=%lu data=%p\n",
                           co_ret, inode->ino, (long)pos, n, data);
                    for (int j = pi; j < n_pages; j++)
                        pcache_put_page(pc, pages[j]);
                    if (bytes_read == 0)
                        return -EFAULT;
                    return bytes_read;
                }
            } else {
                memmove(buf + bytes_read, data, n);
            }
            pcache_put_page(pc, pages[pi]);
            bytes_read += n;
            pos += n;
        }
    }

    if (bytes_read != orig_count && pos == file->f_pos) {
        printf("xv6fs_file_read: SHORT ino=%lu got=%lu want=%lu pos=%ld isize=%ld\n",
               inode->ino, bytes_read, orig_count, (long)pos, (long)isize);
    }
    return bytes_read;
}

/******************************************************************************
 * File write
 *
 * Data goes through the per-inode pcache: user bytes are copied into pcache
 * pages which are marked dirty.  The pcache flusher thread writes dirty pages
 * back to disk via bio (data=writeback semantics).
 *
 * Metadata (block allocation, inode size) still goes through the log for
 * crash consistency.  Each transaction chunk covers bmap + iupdate only;
 * data blocks are NOT logged.
 ******************************************************************************/

ssize_t xv6fs_file_write(struct vfs_file *file, const char *buf, size_t count,
                         bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb =
        container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }

    if (!pc->active) {
        return -EIO;
    }

    loff_t pos = file->f_pos;
    loff_t end_pos = pos + count;

    // Check file size limit
    if (end_pos > XV6FS_MAXFILE * BSIZE) {
        return -EFBIG;
    }

    // Write in chunks to avoid exceeding log transaction size.
    // Only metadata (bmap allocations + iupdate) goes through the log now;
    // data blocks are written by the pcache flusher via bio.
    //
    // Metadata budget per chunk of N data blocks (sequential write):
    //   ceil(N/NINDIRECT) indirect blocks  (NINDIRECT = BSIZE/4 = 256)
    //   + ceil(N/(BSIZE*8)) bitmap blocks  (1 per 8192 data blocks)
    //   + ~4 fixed (inode, dbl-indirect root, spare)
    // This must be <= MAXOPBLOCKS (80).
    //
    // For 8MB = 8192 blocks: 32 indirect + 1 bitmap + 4 = 37 ≤ 80.  Safe.
    int max = 8 * 1024 * 1024; /* 8 MB per transaction chunk */
    size_t bytes_written = 0;

    while (bytes_written < count) {
        size_t n = count - bytes_written;
        if (n > max) {
            n = max;
        }

        // Acquire transaction BEFORE inode lock to match VFS locking order:
        // transaction → superblock → inode
        // VFS releases inode lock before calling this function to avoid
        // deadlock.
        int begin_ret = xv6fs_begin_op(xv6_sb);
        if (begin_ret != 0) {
            if (bytes_written == 0)
                return begin_ret;
            goto done;
        }

        // Now acquire inode lock to protect inode metadata during write.
        // The file reference guarantees the inode remains allocated.
        vfs_ilock(inode);

        size_t chunk_written = 0;
        while (chunk_written < n) {
            /*
             * Folio-aware write: map the current position to a pcache
             * folio, compute how much data fits in this folio, allocate
             * all the underlying xv6fs blocks, then copy the data in one
             * shot.  This reduces pcache_get/put overhead from N calls per
             * folio to just one.
             */
            uint bn = pos / BSIZE;
            uint64 blkno_512 = (uint64)bn * BLK512_PER_BSIZE;

            page_t *page = pcache_get_page(pc, blkno_512);
            if (page == NULL) {
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if (bytes_written > 0)
                    goto done;
                return signal_pending(current) ? -EINTR : -EIO;
            }

            struct pcache_node *pcn = page->pcache.pcache_node;
            uint64 folio_start_byte = (uint64)pcn->blkno * 512;
            uint64 folio_off = (uint64)pos - folio_start_byte;
            size_t avail = pcn->size - (size_t)folio_off;
            size_t chunk = avail;
            if (chunk > n - chunk_written)
                chunk = n - chunk_written;

            /* Ensure all xv6fs blocks covered by this chunk are allocated. */
            uint first_bn = pos / BSIZE;
            uint last_bn = (pos + chunk - 1) / BSIZE;
            int bmap_ok = 1;
            for (uint b = first_bn; b <= last_bn; b++) {
                uint addr = xv6fs_bmap(ip, b);
                if (addr == 0) {
                    bmap_ok = 0;
                    break;
                }
            }
            if (!bmap_ok) {
                pcache_put_page(pc, page);
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if (bytes_written == 0)
                    return -ENOSPC;
                goto done;
            }

            /*
             * Skip the disk read when we are about to overwrite the entire
             * pcache folio.  pcache_prepare_write_page zero-fills and marks
             * the folio up-to-date without I/O.
             */
            int ret;
            bool full_folio = (folio_off == 0 && chunk >= pcn->size);
            if (full_folio) {
                ret = pcache_prepare_write_page(pc, page);
            } else {
                ret = pcache_read_page(pc, page);
            }
            if (ret != 0) {
                pcache_put_page(pc, page);
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if (bytes_written > 0)
                    goto done;
                return (ret == -EINTR) ? -EINTR : -EIO;
            }

            char *data = (char *)pcn->data + folio_off;

            if (user) {
                if (vm_copyin(current->vm, data,
                              (uint64)(buf + bytes_written + chunk_written),
                              chunk) < 0) {
                    pcache_put_page(pc, page);
                    vfs_iunlock(inode);
                    xv6fs_end_op(xv6_sb);
                    if (bytes_written == 0)
                        return -EFAULT;
                    goto done;
                }
            } else {
                memmove(data, buf + bytes_written + chunk_written, chunk);
            }
            pcache_mark_page_dirty(pc, page);
            pcache_put_page(pc, page);

            chunk_written += chunk;
            pos += chunk;
        }

        // Update size if we extended the file
        if (pos > inode->size) {
            inode->size = pos;
        }
        xv6fs_iupdate(ip);

        // Release inode lock before ending transaction
        vfs_iunlock(inode);
        xv6fs_end_op(xv6_sb);

        bytes_written += chunk_written;
    }

done:
    return bytes_written;
}

/******************************************************************************
 * File seek
 ******************************************************************************/

loff_t xv6fs_file_llseek(struct vfs_file *file, loff_t offset, int whence) {
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
 * File fsync - flush a range of dirty pcache pages to disk
 ******************************************************************************/

int xv6fs_file_fsync(struct vfs_file *file, loff_t start, loff_t len) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct pcache *pc;
    (void)start; /* TODO: implement range-based flush */
    (void)len;

    if (inode == NULL)
        return 0;

    pc = &inode->i_data;
    if (!pc->active)
        return 0;

    return pcache_flush(pc);
}

/******************************************************************************
 * File fflush - flush all dirty pcache pages to disk
 ******************************************************************************/

int xv6fs_file_fflush(struct vfs_file *file) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct pcache *pc;

    if (inode == NULL)
        return 0;

    pc = &inode->i_data;
    if (!pc->active)
        return 0;

    int ret = pcache_flush(pc);
    if (ret != 0)
        printf("xv6fs_file_fflush: pcache_flush=%d ino=%lu dirty=%ld\n",
               ret, inode->ino, (long)pc->dirty_count);
    return ret;
}

/******************************************************************************
 * Vectored read (readv)
 *
 * Delegates to the generic pcache_readv() helper which handles the
 * page-at-a-time scatter loop, batched page releases, and RWF_NOWAIT.
 * The inode lock is taken only briefly to snapshot the file size.
 ******************************************************************************/

static ssize_t xv6fs_file_readv(struct vfs_file *file, struct iov_iter *iter,
                                bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode))
        return -EINVAL;
    if (!pc->active)
        return -EIO;

    /* Snapshot inode size under lock */
    vfs_ilock(inode);
    loff_t isize = inode->size;
    vfs_iunlock(inode);

    loff_t pos = file->f_pos;

    ssize_t ret = pcache_readv(pc, iter, &pos, isize, user);

    if (ret > 0)
        file->f_pos = pos;
    return ret;
}

/******************************************************************************
 * Vectored write (writev)
 *
 * Batch segments within transaction chunks, reducing begin_op/end_op
 * overhead compared to the per-segment fallback path.
 ******************************************************************************/

static ssize_t xv6fs_file_writev(struct vfs_file *file, struct iov_iter *iter,
                                 bool user)
{
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb =
        container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode))
        return -EINVAL;
    if (!pc->active)
        return -EIO;

    /*
     * RWF_NOWAIT: xv6fs writes require transactions (begin_op) and
     * on-disk block allocation (bmap), both of which can block.
     * Non-blocking writes are not supported — return -EAGAIN so the
     * caller can retry without RWF_NOWAIT.
     */
    if (iter->flags & RWF_NOWAIT)
        return -EAGAIN;

    loff_t pos = file->f_pos;

    /* Check file size limit */
    if (pos + (loff_t)iter->count > XV6FS_MAXFILE * BSIZE)
        return -EFBIG;

    int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
    ssize_t bytes_written = 0;

    while (iter->nr_segs > 0 && iter->count > 0) {
        /* Determine how much we can write in this transaction chunk */
        size_t chunk_budget = (size_t)max;

        int begin_ret = xv6fs_begin_op(xv6_sb);
        if (begin_ret != 0) {
            if (bytes_written == 0)
                return begin_ret;
            goto done;
        }

        vfs_ilock(inode);

        size_t chunk_written = 0;

        /* Consume iov_iter segments within this transaction */
        while (iter->nr_segs > 0 && iter->count > 0 &&
               chunk_written < chunk_budget) {
            size_t seg_len = iter->iov->iov_len - iter->iov_off;
            if (seg_len == 0) {
                iov_iter_advance(iter, 0);
                continue;
            }
            uint64 base = iter->iov->iov_base + iter->iov_off;

            /* Limit to remaining transaction budget */
            if (seg_len > chunk_budget - chunk_written)
                seg_len = chunk_budget - chunk_written;

            size_t seg_written = 0;
            while (seg_written < seg_len) {
                uint bn    = pos / BSIZE;
                uint off   = pos % BSIZE;
                uint chunk = BSIZE - off;
                if (chunk > seg_len - seg_written)
                    chunk = seg_len - seg_written;

                uint addr = xv6fs_bmap(ip, bn);
                if (addr == 0) {
                    vfs_iunlock(inode);
                    xv6fs_end_op(xv6_sb);
                    if (bytes_written == 0)
                        return -ENOSPC;
                    goto done;
                }

                uint64 blkno_512 = (uint64)bn * BLK512_PER_BSIZE;
                page_t *page = pcache_get_page(pc, blkno_512);
                if (page == NULL) {
                    vfs_iunlock(inode);
                    xv6fs_end_op(xv6_sb);
                    if (bytes_written > 0)
                        goto done;
                    return signal_pending(current) ? -EINTR : -EIO;
                }

                int ret = pcache_read_page(pc, page);
                if (ret != 0) {
                    pcache_put_page(pc, page);
                    vfs_iunlock(inode);
                    xv6fs_end_op(xv6_sb);
                    if (bytes_written > 0)
                        goto done;
                    return (ret == -EINTR) ? -EINTR : -EIO;
                }

                struct pcache_node *pcn = page->pcache.pcache_node;
                uint64 folio_off = (uint64)blkno_512 * 512 -
                                   (uint64)pcn->blkno * 512 + off;
                char *data = (char *)pcn->data + folio_off;

                if (user) {
                    if (vm_copyin(current->vm, data,
                                  (uint64)(base + seg_written), chunk) < 0) {
                        pcache_put_page(pc, page);
                        vfs_iunlock(inode);
                        xv6fs_end_op(xv6_sb);
                        if (bytes_written == 0)
                            return -EFAULT;
                        goto done;
                    }
                } else {
                    memmove(data, (const char *)(base + seg_written), chunk);
                }
                pcache_mark_page_dirty(pc, page);
                pcache_put_page(pc, page);

                seg_written += chunk;
                pos += chunk;
            }

            chunk_written += seg_written;
            bytes_written += seg_written;
            iov_iter_advance(iter, seg_written);
            if (seg_written < seg_len)
                break; /* short write */
        }

        /* Update inode size if extended */
        if (pos > inode->size)
            inode->size = pos;
        xv6fs_iupdate(ip);

        vfs_iunlock(inode);
        xv6fs_end_op(xv6_sb);
    }

done:
    file->f_pos = pos;
    return bytes_written;
}

/******************************************************************************
 * VFS file operations structure
 ******************************************************************************/

static void *xv6fs_file_fault(struct vfs_file *file, struct vma *vma,
                              uint64 va);
static int xv6fs_file_writepage(struct vfs_file *file, loff_t offset,
                                const void *data, size_t len);

struct vfs_file_ops xv6fs_file_ops = {
    .read = xv6fs_file_read,
    .write = xv6fs_file_write,
    .llseek = xv6fs_file_llseek,
    .release = NULL,
    .fsync = xv6fs_file_fsync,
    .fflush = xv6fs_file_fflush,
    .fault = xv6fs_file_fault,
    .writepage = xv6fs_file_writepage,
    .readv = xv6fs_file_readv,
    .writev = xv6fs_file_writev,
};

/******************************************************************************
 * Page fault handler for file-backed mmap
 *
 * Allocates a fresh anonymous page and populates it with file data read
 * from the per-inode pcache.
 *
 * Because the faulting offset is always page-aligned (both vma->pgoff and
 * va are page-aligned), the logical block number (bn = file_off / BSIZE) is
 * always a multiple of BSIZE_PER_PAGE.  This means the pcache page starts
 * at offset 0 within the data, and we can copy a full PGSIZE (or less for
 * the last partial page) directly from pcn->data.
 *
 * The inode lock is held while reading size and pcache data to prevent
 * races with concurrent truncate or write.
 *
 * @file:  the open file backing the mapping
 * @vma:   the VMA that was faulted in
 * @va:    faulting virtual address (page-aligned)
 * Returns: physical address of the populated page, or NULL on failure.
 ******************************************************************************/
static void *xv6fs_file_fault(struct vfs_file *file, struct vma *vma,
                              uint64 va) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return NULL;
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

    uint64 bytes_to_read = PGSIZE;
    if (file_off + PGSIZE > (uint64)inode->size)
        bytes_to_read = (uint64)inode->size - file_off;

    if (!pc->active) {
        vfs_iunlock(inode);
        return NULL;
    }

    // Convert file offset to xv6fs pcache key (512-byte units).
    // Since file_off is page-aligned, bn is a multiple of BSIZE_PER_PAGE,
    // so the target data begins at offset 0 within the pcache page.
    uint bn = file_off / BSIZE;
    uint64 blkno_512 = (uint64)bn * BLK512_PER_BSIZE;

    page_t *pcpage = pcache_get_page(pc, blkno_512);
    if (pcpage == NULL) {
        vfs_iunlock(inode);
        return NULL;
    }
    int ret = pcache_read_page(pc, pcpage);
    if (ret != 0) {
        pcache_put_page(pc, pcpage);
        vfs_iunlock(inode);
        return NULL;
    }

    struct pcache_node *pcn = pcpage->pcache.pcache_node;
    /* Compute sub-page offset within multi-page folio. */
    uint64 folio_byte_off = blkno_512 * BLK_SIZE -
                            (uint64)pcn->blkno * BLK_SIZE;

    /*
     * Zero-copy path: if the entire page is covered by file data, return
     * the pcache page PA directly.  The pcache_get_page ref keeps the
     * page pinned (refcount >= 2, off LRU).  The caller (vma_validate)
     * will detect PAGE_TYPE_PCACHE and handle teardown via pcache_put_page.
     */
    if (bytes_to_read == PGSIZE) {
        vfs_iunlock(inode);
        return (char *)pcn->data + folio_byte_off;
    }

    /* Partial page: copy + zero-fill tail. */
    void *pa = page_alloc(0, PAGE_TYPE_ANON);
    if (pa == NULL) {
        pcache_put_page(pc, pcpage);
        vfs_iunlock(inode);
        return NULL;
    }

    memmove(pa, (char *)pcn->data + folio_byte_off, bytes_to_read);
    memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);

    pcache_put_page(pc, pcpage);
    vfs_iunlock(inode);
    return pa;
}

/******************************************************************************
 * File writepage — write a single page of data back to xv6fs via pcache
 *
 * Called by the VM layer when tearing down or syncing a dirty MAP_SHARED
 * mapping.  Two cases:
 *
 *   1. Zero-copy page:  data == pcn->data for some pcache node.  The page
 *      is already in the pcache, so we just mark it dirty.  The pcache
 *      flusher will write it to disk later (or fsync forces it).
 *
 *   2. Anon-copy page:  data points to an anonymous page (partial last
 *      page, or COW copy).  We must write the content into the pcache so
 *      it reaches the block device.
 *
 * @file:    the open file for the mapping
 * @offset:  byte offset in the file (page-aligned)
 * @data:    physical address of the page data
 * @len:     number of valid bytes (may be < PGSIZE at end-of-file)
 * Returns:  0 on success, negative errno on failure
 ******************************************************************************/
static int xv6fs_file_writepage(struct vfs_file *file, loff_t offset,
                                const void *data, size_t len) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL)
        return -EIO;

    struct pcache *pc = &inode->i_data;
    if (!pc->active)
        return -EIO;

    /*
     * Zero-copy check: if the data pointer IS a pcache page (or a
     * tail page of a pcache folio), just mark it dirty — its content
     * is already the authoritative copy.
     */
    page_t *pg = __pa_to_page((uint64)data);
    page_t *pc_head = page_pcache_head(pg);
    if (pc_head != NULL) {
        pcache_mark_page_dirty(pc, pc_head);
        return 0;
    }

    /*
     * Non-pcache data (anon partial page): look up the pcache page for
     * this file offset, copy the data in, and mark dirty.
     */
    uint bn = (uint)(offset / BSIZE);
    uint64 blkno_512 = (uint64)bn * BLK512_PER_BSIZE;

    page_t *pcpage = pcache_get_page(pc, blkno_512);
    if (pcpage == NULL)
        return -EIO;

    int ret = pcache_read_page(pc, pcpage);
    if (ret != 0) {
        pcache_put_page(pc, pcpage);
        return -EIO;
    }

    struct pcache_node *pcn = pcpage->pcache.pcache_node;
    /* Compute the sub-page offset for multi-page folios. */
    uint64 folio_byte_off = blkno_512 * BLK_SIZE - (uint64)pcn->blkno * BLK_SIZE;
    memmove((char *)pcn->data + folio_byte_off, data, len);
    if (len < PGSIZE)
        memset((char *)pcn->data + folio_byte_off + len, 0, PGSIZE - len);

    pcache_mark_page_dirty(pc, pcpage);
    pcache_put_page(pc, pcpage);
    return 0;
}
