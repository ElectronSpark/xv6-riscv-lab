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
#include <mm/pcache.h>
#include <mm/page.h>
#include "tmpfs_private.h"

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

static struct pcache_ops tmpfs_pcache_ops = {
    .read_page = tmpfs_pcache_read_page,
    .write_page = tmpfs_pcache_write_page,
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
static int __tmpfs_file_stat(struct vfs_file *file, struct stat *stat);

static void *__tmpfs_file_fault(struct vfs_file *file, struct vma *vma,
                               uint64 va);

struct vfs_file_ops tmpfs_file_ops = {
    .read = __tmpfs_file_read,
    .write = __tmpfs_file_write,
    .llseek = __tmpfs_file_llseek,
    .release = NULL,
    .fsync = NULL,
    .stat = __tmpfs_file_stat,
    .fault = __tmpfs_file_fault,
};

static ssize_t __tmpfs_file_read(struct vfs_file *file, char *buf, size_t count,
                                 bool user) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);
    struct pcache *pc = &inode->i_data;

    if (!S_ISREG(inode->mode)) {
        return -EINVAL;
    }

    // Acquire inode lock to safely read size and data.
    // The file reference guarantees the inode remains allocated.
    vfs_ilock(inode);

    loff_t pos = file->f_pos;
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

    // pcache-based read
    if (!pc->active) {
        vfs_iunlock(inode);
        return -EIO;
    }

    size_t bytes_read = 0;
    while (bytes_read < count) {
        size_t block_idx = TMPFS_IBLOCK(pos);
        size_t block_off = TMPFS_IBLOCK_OFFSET(pos);
        size_t chunk = PAGE_SIZE - block_off;
        if (chunk > count - bytes_read) {
            chunk = count - bytes_read;
        }

        // Get page from pcache (blkno in 512-byte units)
        uint64 blkno_512 = (uint64)block_idx * PCACHE_BLKS_PER_PAGE;
        page_t *page = pcache_get_page(pc, blkno_512);
        if (page == NULL) {
            vfs_iunlock(inode);
            if (bytes_read == 0)
                return -EIO;
            return bytes_read;
        }
        int ret = pcache_read_page(pc, page);
        if (ret != 0) {
            pcache_put_page(pc, page);
            vfs_iunlock(inode);
            if (bytes_read == 0)
                return -EIO;
            return bytes_read;
        }
        struct pcache_node *pcn = page->pcache.pcache_node;
        char *data = (char *)pcn->data + block_off;

        if (user) {
            if (vm_copyout(current->vm, (uint64)(buf + bytes_read), data,
                           chunk) < 0) {
                pcache_put_page(pc, page);
                vfs_iunlock(inode);
                if (bytes_read == 0)
                    return -EFAULT;
                return bytes_read;
            }
        } else {
            memmove(buf + bytes_read, data, chunk);
        }
        pcache_put_page(pc, page);

        bytes_read += chunk;
        pos += chunk;
    }

    vfs_iunlock(inode);
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

    // Acquire inode lock to protect size and data.
    // The file reference guarantees the inode remains allocated.
    vfs_ilock(inode);

    loff_t pos = file->f_pos;
    loff_t end_pos = pos + count;

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

    // pcache-based write
    if (!pc->active) {
        vfs_iunlock(inode);
        return -EIO;
    }

    size_t bytes_written = 0;
    while (bytes_written < count) {
        size_t block_idx = TMPFS_IBLOCK(pos);
        size_t block_off = TMPFS_IBLOCK_OFFSET(pos);
        size_t chunk = PAGE_SIZE - block_off;
        if (chunk > count - bytes_written) {
            chunk = count - bytes_written;
        }

        // Get page from pcache (blkno in 512-byte units)
        uint64 blkno_512 = (uint64)block_idx * PCACHE_BLKS_PER_PAGE;
        page_t *page = pcache_get_page(pc, blkno_512);
        if (page == NULL) {
            vfs_iunlock(inode);
            if (bytes_written == 0)
                return -ENOMEM;
            goto done;
        }
        int ret = pcache_read_page(pc, page);
        if (ret != 0) {
            pcache_put_page(pc, page);
            vfs_iunlock(inode);
            if (bytes_written == 0)
                return ret;
            goto done;
        }
        struct pcache_node *pcn = page->pcache.pcache_node;
        char *data = (char *)pcn->data + block_off;

        if (user) {
            if (vm_copyin(current->vm, data, (uint64)(buf + bytes_written),
                          chunk) < 0) {
                pcache_put_page(pc, page);
                vfs_iunlock(inode);
                if (bytes_written == 0)
                    return -EFAULT;
                goto done;
            }
        } else {
            memmove(data, buf + bytes_written, chunk);
        }
        pcache_mark_page_dirty(pc, page);
        pcache_put_page(pc, page);

        bytes_written += chunk;
        pos += chunk;
    }

    // Update size if we extended the file
    if (pos > inode->size) {
        inode->size = pos;
    }

    vfs_iunlock(inode);
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

static int __tmpfs_file_stat(struct vfs_file *file, struct stat *stat) {
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);

    // Lock inode to get consistent snapshot of inode fields.
    // The file reference guarantees the inode remains allocated.
    vfs_ilock(inode);

    memset(stat, 0, sizeof(*stat));
    stat->dev = inode->sb ? (int)(uint64)inode->sb : 0;
    stat->ino = inode->ino;
    stat->mode = inode->mode;
    stat->nlink = inode->n_links;
    stat->size = inode->size;

    vfs_iunlock(inode);
    return 0;
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
 * Allocates a fresh anonymous page and populates it with data from the
 * tmpfs file at the faulting offset.  Handles both the embedded-data path
 * (small files stored inline in the inode) and the pcache path.
 *
 * The inode lock is held while reading size/data to prevent races with
 * concurrent truncate or write.
 *
 * @file:  the open file backing the mapping
 * @vma:   the VMA that was faulted in
 * @va:    faulting virtual address (page-aligned)
 * Returns: physical address of the populated page, or NULL on failure.
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

    void *pa = page_alloc(0, PAGE_TYPE_ANON);
    if (pa == NULL)
        return NULL;

    vfs_ilock(inode);

    // Entirely beyond EOF — return a zero page
    if (file_off >= (uint64)inode->size) {
        vfs_iunlock(inode);
        memset(pa, 0, PGSIZE);
        return pa;
    }

    uint64 bytes_to_read = PGSIZE;
    if (file_off + PGSIZE > (uint64)inode->size)
        bytes_to_read = (uint64)inode->size - file_off;

    /* ---- embedded data path (small files inline in the inode) ---- */
    if (ti->embedded) {
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

    /* ---- pcache path ---- */
    if (!pc->active) {
        vfs_iunlock(inode);
        page_free(pa, 0);
        return NULL;
    }

    uint64 block_idx = TMPFS_IBLOCK(file_off);
    uint64 blkno_512 = block_idx * PCACHE_BLKS_PER_PAGE;

    page_t *pcpage = pcache_get_page(pc, blkno_512);
    if (pcpage == NULL) {
        vfs_iunlock(inode);
        page_free(pa, 0);
        return NULL;
    }
    int ret = pcache_read_page(pc, pcpage);
    if (ret != 0) {
        pcache_put_page(pc, pcpage);
        vfs_iunlock(inode);
        page_free(pa, 0);
        return NULL;
    }

    struct pcache_node *pcn = pcpage->pcache.pcache_node;
    memmove(pa, pcn->data, bytes_to_read);
    if (bytes_to_read < PGSIZE)
        memset((char *)pa + bytes_to_read, 0, PGSIZE - bytes_to_read);

    pcache_put_page(pc, pcpage);
    vfs_iunlock(inode);
    return pa;
}
