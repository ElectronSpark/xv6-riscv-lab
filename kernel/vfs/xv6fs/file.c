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

/*
 * Blocks-per-page constants for pcache ↔ xv6fs block address translation.
 * One pcache page (4KB) covers BSIZE_PER_PAGE xv6fs blocks (BSIZE=1024).
 * Block addresses from bmap are in BSIZE units; pcache uses 512-byte units.
 */
#define BSIZE_PER_PAGE (PGSIZE / BSIZE) /* 4 */
#define BLK512_PER_BSIZE (BSIZE / 512)  /* 2 */

/******************************************************************************
 * File read
 ******************************************************************************/

ssize_t xv6fs_file_read(struct vfs_file *file, char *buf, size_t count,
                        bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }

    if (!pc->active) {
        return -EIO;
    }

    // Snapshot inode size under lock, then release.  The pcache and page
    // reference keep the data safe; holding the inode mutex across the
    // entire I/O loop would serialise concurrent readers on the same file.
    // If a concurrent truncate shrinks the file after our snapshot, we may
    // read stale (but consistent) cached data — matching Linux VFS semantics.
    vfs_ilock(inode);
    loff_t isize = inode->size;
    vfs_iunlock(inode);

    loff_t pos = file->f_pos;
    if (pos >= isize) {
        return 0; // EOF
    }
    if (pos + count > isize) {
        count = isize - pos;
    }

    size_t bytes_read = 0;
    while (bytes_read < count) {
        uint bn = pos / BSIZE;
        uint off = pos % BSIZE;
        uint n = BSIZE - off;
        if (n > count - bytes_read) {
            n = count - bytes_read;
        }

        // Per-inode pcache path (keyed by logical file offset).
        // The read_page callback handles bmap + bio internally, including
        // zero-filling of sparse blocks, so no bmap call is needed here.
        uint64 blkno_512 = (uint64)bn * BLK512_PER_BSIZE;
        page_t *page = pcache_get_page(pc, blkno_512);
        if (page == NULL) {
            if (bytes_read > 0)
                return bytes_read;
            return signal_pending(current) ? -EINTR : -EIO;
        }
        int ret = pcache_read_page(pc, page);
        if (ret != 0) {
            pcache_put_page(pc, page);
            if (bytes_read > 0)
                return bytes_read;
            return (ret == -EINTR) ? -EINTR : -EIO;
        }
        struct pcache_node *pcn = page->pcache.pcache_node;
        uint page_off = (bn % BSIZE_PER_PAGE) * BSIZE + off;
        char *data = (char *)pcn->data + page_off;
        if (user) {
            if (vm_copyout(current->vm, (uint64)(buf + bytes_read), data, n) <
                0) {
                pcache_put_page(pc, page);
                if (bytes_read == 0)
                    return -EFAULT;
                return bytes_read;
            }
        } else {
            memmove(buf + bytes_read, data, n);
        }
        pcache_put_page(pc, page);

        bytes_read += n;
        pos += n;
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
    int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
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
            uint bn = pos / BSIZE;
            uint off = pos % BSIZE;
            uint chunk = BSIZE - off;
            if (chunk > n - chunk_written) {
                chunk = n - chunk_written;
            }

            // Ensure the block is allocated (may log indirect-block changes)
            uint addr = xv6fs_bmap(ip, bn);
            if (addr == 0) {
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if (bytes_written == 0) {
                    return -ENOSPC;
                }
                goto done;
            }

            // Write data through per-inode pcache
            uint64 blkno_512 = (uint64)bn * BLK512_PER_BSIZE;
            page_t *page = pcache_get_page(pc, blkno_512);
            if (page == NULL) {
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if (bytes_written > 0)
                    goto done;
                return signal_pending(current) ? -EINTR : -EIO;
            }

            /*
             * Skip the disk read when we are about to overwrite the entire
             * pcache page (4 KB).  pcache_prepare_write_page zero-fills and
             * marks the page up-to-date without I/O, so the subsequent
             * BSIZE-chunk iterations fill every byte before the page is
             * released.  Falls back to a normal read when the page is
             * already cached or has I/O in progress.
             */
            int ret;
            bool full_page = (bn % BSIZE_PER_PAGE == 0 && off == 0 &&
                              (n - chunk_written) >= PGSIZE);
            if (full_page) {
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

            struct pcache_node *pcn = page->pcache.pcache_node;
            uint page_off = (bn % BSIZE_PER_PAGE) * BSIZE + off;
            char *data = (char *)pcn->data + page_off;

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

    return pcache_flush(pc);
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
                uint page_off = (bn % BSIZE_PER_PAGE) * BSIZE + off;
                char *data = (char *)pcn->data + page_off;

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

    /*
     * Zero-copy path: if the entire page is covered by file data, return
     * the pcache page PA directly.  The pcache_get_page ref keeps the
     * page pinned (refcount >= 2, off LRU).  The caller (vma_validate)
     * will detect PAGE_TYPE_PCACHE and handle teardown via pcache_put_page.
     */
    if (bytes_to_read == PGSIZE) {
        vfs_iunlock(inode);
        return pcn->data; /* zero-copy: return pcache page PA */
    }

    /* Partial page: copy + zero-fill tail. */
    void *pa = page_alloc(0, PAGE_TYPE_ANON);
    if (pa == NULL) {
        pcache_put_page(pc, pcpage);
        vfs_iunlock(inode);
        return NULL;
    }

    memmove(pa, pcn->data, bytes_to_read);
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
     * Zero-copy check: if the data pointer IS a pcache page, just mark
     * it dirty — its content is already the authoritative copy.
     */
    page_t *pg = __pa_to_page((uint64)data);
    if (pg != NULL && PAGE_IS_TYPE(pg, PAGE_TYPE_PCACHE)) {
        pcache_mark_page_dirty(pc, pg);
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
    memmove(pcn->data, data, len);
    if (len < PGSIZE)
        memset((char *)pcn->data + len, 0, PGSIZE - len);

    pcache_mark_page_dirty(pc, pcpage);
    pcache_put_page(pc, pcpage);
    return 0;
}
