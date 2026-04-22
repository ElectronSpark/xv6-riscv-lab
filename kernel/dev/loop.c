/*
 * loop — loopback block device.
 *
 * Presents a regular file (or a region of it) as a block device.
 * All I/O is routed through the backing file's per-inode page cache
 * (inode->i_data) so that reads/writes are automatically cached and
 * share pages with any userspace mmap of the same file.
 *
 * Pre-registers /dev/loop0 .. /dev/loop7 at boot time (inactive).
 * loop_setup() attaches a file and optionally runs partition discovery.
 * loop_clear() detaches the file and removes discovered partitions.
 */

#include <types.h>
#include <param.h>
#include "riscv.h"
#include "defs.h"
#include "string.h"
#include "printf.h"
#include <mm/page.h>
#include <mm/pcache.h>
#include <dev/bio.h>
#include <dev/blkdev.h>
#include <dev/loop.h>
#include <dev/gendisk.h>
#include <vfs/vfs_types.h>
#include <vfs/file.h>
#include <vfs/fs.h>
#include <vfs/stat.h>
#include <vfs/fcntl.h>
#include <lock/spinlock.h>
#include <errno.h>

/* ------------------------------------------------------------------------ */
/* Constants                                                                 */
/* ------------------------------------------------------------------------ */

#define LOOP_MAJOR  7          /* block-device major number for loop devs */
#define LOOP_BLOCK_SHIFT 0     /* 2^0 * 512 = 512 bytes per hw sector     */

/* Number of 512-byte sectors per page cache page */
#define SECTORS_PER_PAGE (PGSIZE / 512)

/* ------------------------------------------------------------------------ */
/* Static pool of loop devices                                               */
/* ------------------------------------------------------------------------ */

static struct loop_dev __loop_devs[NLOOP];

/* ------------------------------------------------------------------------ */
/* forward declarations                                                      */
/* ------------------------------------------------------------------------ */

static int __loop_open(blkdev_t *bdev);
static int __loop_release(blkdev_t *bdev);
static int __loop_submit_bio(blkdev_t *bdev, struct bio *bio);

static blkdev_ops_t __loop_ops = {
    .open       = __loop_open,
    .release    = __loop_release,
    .submit_bio = __loop_submit_bio,
};

/* ------------------------------------------------------------------------ */
/* blkdev_ops callbacks                                                      */
/* ------------------------------------------------------------------------ */

static int __loop_open(blkdev_t *bdev) {
    (void)bdev;
    return 0;
}

static int __loop_release(blkdev_t *bdev) {
    (void)bdev;
    return 0;
}

/*
 * __loop_submit_bio — service a BIO against the backing file's page cache.
 *
 * For each bio_vec we:
 *   1. Compute the corresponding file byte offset.
 *   2. Map that into the backing inode's i_data pcache (512-byte block units).
 *   3. For reads:  pcache_read_page → memcpy from pcache → done.
 *      For writes: pcache_get_page → memcpy into pcache → mark dirty.
 *   4. Complete the bio synchronously.
 */
static int __loop_submit_bio(blkdev_t *bdev, struct bio *bio) {
    struct loop_dev *lo = container_of(bdev, struct loop_dev, blkdev);

    spin_lock(&lo->lock);
    struct vfs_file *file = lo->backing_file;
    if (file == NULL) {
        spin_unlock(&lo->lock);
        bio->error = -ENXIO;
        bio_complete(bio);
        return -ENXIO;
    }
    spin_unlock(&lo->lock);

    struct vfs_inode *inode = file->inode.inode;
    if (inode == NULL) {
        bio->error = -ENXIO;
        bio_complete(bio);
        return -ENXIO;
    }
    struct pcache *pc = &inode->i_data;

    bool is_write = bio->rw;

    /* Walk each segment of the scatter-gather list */
    struct bio_iter iter;
    struct bio_vec bvec;
    bio_for_each_segment(&bvec, bio, &iter) {
        /*
         * iter.blkno is the bio-level block number (in hardware sectors).
         * Convert to a byte offset within the loop device, then add the
         * file_offset to get the absolute byte position in the backing file.
         */
        uint64 dev_byte_off = (uint64)iter.blkno * 512;
        uint64 file_byte_off = lo->file_offset + dev_byte_off;

        /*
         * Process this bvec, which may span multiple pcache pages if
         * bvec.len > PGSIZE.  In practice, bvec.len ≤ PGSIZE (one page),
         * but handle the general case.
         */
        uint16 remaining = bvec.len;
        uint16 bvec_off  = bvec.offset;    /* offset within the bio page */

        while (remaining > 0) {
            /* pcache block number in 512-byte units */
            uint64 pc_blkno = file_byte_off / 512;
            /* Align to page boundary for pcache lookup */
            uint64 pc_page_blkno = (pc_blkno / SECTORS_PER_PAGE)
                                   * SECTORS_PER_PAGE;
            /* Offset within the pcache page */
            uint16 page_off = (uint16)(file_byte_off % PGSIZE);
            /* How much we can transfer in this pcache page */
            uint16 chunk = PGSIZE - page_off;
            if (chunk > remaining)
                chunk = remaining;

            /* Get (or allocate) the pcache page for this file region */
            page_t *pc_page = pcache_get_page(pc, pc_page_blkno);
            if (pc_page == NULL) {
                bio->error = -ENOMEM;
                bio_complete(bio);
                return -ENOMEM;
            }

            struct pcache_node *pcn = pc_page->pcache.pcache_node;
            if (pcn == NULL) {
                pcache_put_page(pc, pc_page);
                bio->error = -EIO;
                bio_complete(bio);
                return -EIO;
            }

            /* Ensure the page data is up-to-date */
            if (!pcn->uptodate) {
                int ret = pcache_read_page(pc, pc_page);
                if (ret != 0) {
                    pcache_put_page(pc, pc_page);
                    bio->error = ret;
                    bio_complete(bio);
                    return ret;
                }
            }

            /* Data pointer inside the pcache page */
            char *pc_data = (char *)pcn->data + page_off;
            /* Data pointer inside the bio page */
            char *bio_data = (char *)__page_to_pa(bvec.bv_page) + bvec_off;

            if (is_write) {
                memmove(pc_data, bio_data, chunk);
                pcache_mark_page_dirty(pc, pc_page);
            } else {
                memmove(bio_data, pc_data, chunk);
            }

            pcache_put_page(pc, pc_page);

            remaining    -= chunk;
            bvec_off     += chunk;
            file_byte_off += chunk;
        }
    }

    /* Synchronous completion — all data has been transferred */
    bio->error = 0;
    bio_complete(bio);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Helper: format loop device name manually (no snprintf in kernel)          */
/* ------------------------------------------------------------------------ */
static void __format_loop_name(char *buf, size_t bufsz, int num) {
    /* "loop" prefix */
    const char *prefix = "loop";
    size_t pos = 0;
    while (*prefix && pos < bufsz - 1)
        buf[pos++] = *prefix++;

    /* Decimal number */
    char numstr[8];
    int nlen = 0;
    if (num == 0) {
        numstr[nlen++] = '0';
    } else {
        int n = num;
        while (n > 0 && nlen < 7) {
            numstr[nlen++] = '0' + (n % 10);
            n /= 10;
        }
    }
    for (int i = nlen - 1; i >= 0 && pos < bufsz - 1; i--)
        buf[pos++] = numstr[i];
    buf[pos] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void loop_init(void) {
    for (int i = 0; i < NLOOP; i++) {
        struct loop_dev *lo = &__loop_devs[i];
        memset(lo, 0, sizeof(*lo));
        lo->loop_num = i;
        spin_init(&lo->lock, "loop");

        /* Format device name */
        __format_loop_name(lo->devname, sizeof(lo->devname), i);

        /* Set up the blkdev — starts inactive (backing_file == NULL) */
        lo->blkdev.dev.major   = LOOP_MAJOR;
        lo->blkdev.dev.minor   = i + 1;    /* minor 1..NLOOP */
        lo->blkdev.dev.devname = lo->devname;
        lo->blkdev.dev.devmode = S_IFBLK | 0600;
        lo->blkdev.readable    = 1;
        lo->blkdev.writable    = 1;
        lo->blkdev.block_shift = LOOP_BLOCK_SHIFT;
        lo->blkdev.ops         = __loop_ops;

        int ret = blkdev_register(&lo->blkdev);
        if (ret != 0)
            printf("loop: failed to register /dev/%s: %d\n", lo->devname, ret);
        else
            printf("loop: registered /dev/%s (major=%d, minor=%d)\n",
                   lo->devname, LOOP_MAJOR, i + 1);
    }
}

int loop_setup(int loop_num, struct vfs_file *file, uint64 offset) {
    if (loop_num < 0 || loop_num >= NLOOP)
        return -EINVAL;
    if (file == NULL)
        return -EINVAL;

    struct loop_dev *lo = &__loop_devs[loop_num];

    /* Validate that the backing file is a regular file with a pcache */
    struct vfs_inode *inode = file->inode.inode;
    if (inode == NULL || !S_ISREG(inode->mode))
        return -EINVAL;

    spin_lock(&lo->lock);
    if (lo->backing_file != NULL) {
        spin_unlock(&lo->lock);
        return -EBUSY;
    }

    /* Dup the file reference so the loop device holds its own ref */
    struct vfs_file *dup = vfs_fdup(file);
    if (dup == NULL) {
        spin_unlock(&lo->lock);
        return -ENOMEM;
    }

    lo->backing_file = dup;
    lo->file_offset  = offset;
    lo->size_bytes   = (uint64)inode->size - offset;
    spin_unlock(&lo->lock);

    printf("loop: /dev/%s attached (size=%ld bytes, offset=%ld)\n",
           lo->devname, lo->size_bytes, offset);

    /* Attempt partition discovery */
    lo->gd = gendisk_probe(&lo->blkdev);
    /* gd may be NULL if no partitions found — that's fine */

    return 0;
}

int loop_clear(int loop_num) {
    if (loop_num < 0 || loop_num >= NLOOP)
        return -EINVAL;

    struct loop_dev *lo = &__loop_devs[loop_num];

    /* Remove any discovered partitions first */
    if (lo->gd != NULL) {
        gendisk_remove(lo->gd);
        lo->gd = NULL;
    }

    spin_lock(&lo->lock);
    struct vfs_file *old_file = lo->backing_file;
    lo->backing_file = NULL;
    lo->file_offset  = 0;
    lo->size_bytes   = 0;
    spin_unlock(&lo->lock);

    if (old_file != NULL) {
        vfs_fput(old_file);
        printf("loop: /dev/%s detached\n", lo->devname);
    }

    return 0;
}

int loop_is_free(int loop_num) {
    if (loop_num < 0 || loop_num >= NLOOP)
        return 0;
    struct loop_dev *lo = &__loop_devs[loop_num];
    spin_lock(&lo->lock);
    int free = (lo->backing_file == NULL);
    spin_unlock(&lo->lock);
    return free;
}

blkdev_t *loop_get_blkdev(int loop_num) {
    if (loop_num < 0 || loop_num >= NLOOP)
        return NULL;
    return &__loop_devs[loop_num].blkdev;
}

/* ------------------------------------------------------------------------ */
/* sys_losetup — syscall for loop device management                          */
/*                                                                            */
/* arg0 = subcommand:                                                         */
/*   0 = setup: attach file (arg2=path) to loop device (arg1=loop_num)       */
/*   1 = clear: detach loop device (arg1=loop_num)                            */
/*   2 = status: print all loop device status                                 */
/* arg1 = loop device number (for setup/clear)                                */
/* arg2 = file path string (for setup only)                                   */
/* ------------------------------------------------------------------------ */
uint64 sys_losetup(void) {
    int cmd, loop_num;
    argint(0, &cmd);
    argint(1, &loop_num);

    switch (cmd) {
    case 0: {
        /* Setup: attach a file to a loop device */
        char filepath[MAXPATH];
        int n = argstr(2, filepath, MAXPATH);
        if (n < 0)
            return -EFAULT;

        /* Resolve the file */
        struct vfs_inode *inode = vfs_namei(filepath, n);
        if (IS_ERR_OR_NULL(inode))
            return IS_ERR(inode) ? PTR_ERR(inode) : -ENOENT;
        if (!S_ISREG(inode->mode)) {
            vfs_iput(inode);
            return -EINVAL;
        }

        struct vfs_file *file = vfs_fileopen(inode, O_RDWR);
        if (IS_ERR_OR_NULL(file)) {
            file = vfs_fileopen(inode, O_RDONLY);
        }
        vfs_iput(inode);

        if (IS_ERR_OR_NULL(file))
            return IS_ERR(file) ? PTR_ERR(file) : -EIO;

        int ret = loop_setup(loop_num, file, 0);
        vfs_fput(file);
        return ret;
    }
    case 1: {
        /* Clear: detach loop device */
        return loop_clear(loop_num);
    }
    case 2: {
        /* Status: print all loop devices */
        printf("LOOP  STATUS    SIZE        BACKING FILE\n");
        for (int i = 0; i < NLOOP; i++) {
            struct loop_dev *lo = &__loop_devs[i];
            spin_lock(&lo->lock);
            if (lo->backing_file != NULL) {
                printf("%-5s active    %-10ld  (attached)\n",
                       lo->devname, lo->size_bytes);
            } else {
                printf("%-5s free\n", lo->devname);
            }
            spin_unlock(&lo->lock);
        }
        return 0;
    }
    default:
        return -EINVAL;
    }
}
