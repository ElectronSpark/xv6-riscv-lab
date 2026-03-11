/*
 * ext4fs superblock and mount operations
 *
 * Handles mounting an ext2/3/4 filesystem via lwext4's low-level APIs,
 * bridging into xv6's VFS subsystem.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "cmdline.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "dev/blkdev.h"
#include <mm/vm.h>
#include "dev/buf.h"
#include "dev/blkdev.h"
#include "vfs/fs.h"
#include <mm/slab.h>
#include "kernel/vfs/vfs_private.h"
#include "ext4fs_private.h"

#include <ext4_errno.h>
#include <ext4_super.h>
#include <ext4_blockdev.h>
#include <ext4_bcache.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_debug.h>

/* ──────────────────────────────────────────────────────────────────────────── */
/* Slab caches                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

slab_cache_t ext4fs_inode_cache;
slab_cache_t ext4fs_sb_cache;

static int ext4fs_init_caches(void)
{
    int r;

    r = slab_cache_init(&ext4fs_inode_cache, "ext4fs_inode",
                        sizeof(struct ext4fs_inode),
                        SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    if (r != 0)
        return r;

    r = slab_cache_init(&ext4fs_sb_cache, "ext4fs_sb",
                        sizeof(struct ext4fs_superblock),
                        SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    if (r != 0)
        return r;

    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Fill VFS inode from an ext4 inode reference                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

void ext4fs_fill_vfs_inode(struct vfs_inode *vi, struct ext4_inode_ref *ref,
                           struct ext4_sblock *sb)
{
    struct ext4_inode *raw = ref->inode;

    vi->ino     = ref->index;
    vi->mode    = ext4_imode_to_vfs_mode(ext4_inode_get_mode(sb, raw));
    vi->n_links = ext4_inode_get_links_cnt(raw);
    vi->size    = (loff_t)ext4_inode_get_size(sb, raw);
    vi->uid     = ext4_inode_get_uid(raw);
    vi->gid     = ext4_inode_get_gid(raw);
    vi->ops     = &ext4fs_inode_ops;

    /* Timestamps */
    vi->atime   = ext4_inode_get_access_time(raw);
    vi->mtime   = ext4_inode_get_modif_time(raw);
    vi->ctime   = ext4_inode_get_change_inode_time(raw);

    /* Block count (in 512-byte units as stored on-disk) */
    vi->n_blocks = ext4_inode_get_blocks_count(sb, raw);

    /* Populate device number for block/char device nodes */
    if (S_ISCHR(vi->mode))
        vi->cdev = (dev_t)ext4_inode_get_dev(raw);
    else if (S_ISBLK(vi->mode))
        vi->bdev = (dev_t)ext4_inode_get_dev(raw);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Superblock ops: alloc_inode / get_inode                                     */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Allocate a new on-disk inode via ext4_fs_alloc_inode.
 *
 * VFS calls this without specifying the file type.  We default to
 * EXT4_DE_REG_FILE — the caller (create / mkdir / mknod) immediately
 * overwrites the mode field.  This only affects Orlov group-selection
 * heuristics, which is a minor performance issue, not a correctness one.
 */
struct vfs_inode *ext4fs_alloc_inode(struct vfs_superblock *sb)
{
    if (sb == NULL)
        return ERR_PTR(-EINVAL);

    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_alloc_inode(fs, &ref, EXT4_DE_REG_FILE);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Allocate VFS in-memory wrapper */
    struct ext4fs_inode *ei = slab_alloc(&ext4fs_inode_cache);
    if (ei == NULL) {
        ext4_fs_free_inode(&ref);
        ext4_fs_put_inode_ref(&ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-ENOMEM);
    }
    memset(ei, 0, sizeof(*ei));
    ext4fs_inode_map_cache_init(ei);

    ei->vfs_inode.ino       = ref.index;
    ei->vfs_inode.ops       = &ext4fs_inode_ops;
    ei->vfs_inode.ref_count = 1;

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);
    return &ei->vfs_inode;
}

/*
 * Read an existing inode from disk and return a VFS inode.
 */
struct vfs_inode *ext4fs_get_inode(struct vfs_superblock *sb, uint64 ino)
{
    if (sb == NULL || ino == 0)
        return ERR_PTR(-EINVAL);

    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Check the inode is allocated (links > 0 or mode != 0) */
    uint32_t mode = ext4_inode_get_mode(&fs->sb, ref.inode);
    if (mode == 0) {
        ext4_fs_put_inode_ref(&ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-ENOENT);
    }

    struct ext4fs_inode *ei = slab_alloc(&ext4fs_inode_cache);
    if (ei == NULL) {
        ext4_fs_put_inode_ref(&ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-ENOMEM);
    }
    memset(ei, 0, sizeof(*ei));
    ext4fs_inode_map_cache_init(ei);

    ei->vfs_inode.ref_count = 1;
    ext4fs_fill_vfs_inode(&ei->vfs_inode, &ref, &fs->sb);

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);
    return &ei->vfs_inode;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Superblock ops: sync / unmount / orphan stubs                               */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_sync_fs(struct vfs_superblock *sb, int wait)
{
    if (sb == NULL)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);

    ext4fs_lock(esb);
    /* Flush the ext4 block cache to disk */
    int r = ext4_block_cache_flush(&esb->bdev);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    /* Write the superblock if dirty */
    if (esb->ext4fs.sb.magic == EXT4_SUPERBLOCK_MAGIC) {
        r = ext4_sb_write(&esb->bdev, &esb->ext4fs.sb);
        if (r != EOK) {
            ext4fs_unlock(esb);
            return -r;
        }
    }
    ext4fs_unlock(esb);

    /* Issue device-level flush to ensure data reaches stable storage */
    int flush_r = blkdev_flush(esb->xv6_blkdev);
    if (flush_r != 0)
        return flush_r;

    sb->dirty = 0;
    return 0;
}

static void ext4fs_unmount_begin(struct vfs_superblock *sb)
{
    ext4fs_sync_fs(sb, 1);
}

/*
 * Orphan list management.
 *
 * The ext4 on-disk orphan list is a singly-linked list threaded through the
 * inode deletion-time field (i_dtime).  The head is stored in the superblock's
 * last_orphan field.
 *
 * An inode is added to the orphan list when it is unlinked but still open
 * (n_links == 0, ref_count > 0).  On clean unmount, the list should be empty.
 * On crash recovery, recover_orphans truncates and frees leaked inodes.
 */
static int ext4fs_add_orphan(struct vfs_superblock *sb,
                             struct vfs_inode *inode)
{
    if (sb == NULL || inode == NULL)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    /* Thread into the list: set this inode's dtime to the old list head */
    uint32_t old_head = to_le32(fs->sb.last_orphan);
    ext4_inode_set_del_time(ref.inode, old_head);
    ref.dirty = true;
    ext4_fs_put_inode_ref(&ref);

    /* Update superblock's list head to this inode */
    fs->sb.last_orphan = to_le32((uint32_t)inode->ino);

    ext4fs_unlock(esb);
    sb->dirty = 1;
    return 0;
}

static int ext4fs_remove_orphan(struct vfs_superblock *sb,
                                struct vfs_inode *inode)
{
    if (sb == NULL || inode == NULL)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t target_ino = (uint32_t)inode->ino;

    ext4fs_lock(esb);

    uint32_t head = to_le32(fs->sb.last_orphan);
    if (head == 0) {
        ext4fs_unlock(esb);
        return 0; /* list empty, nothing to remove */
    }

    /* If the target is the head of the list */
    if (head == target_ino) {
        struct ext4_inode_ref ref;
        int r = ext4_fs_get_inode_ref(fs, target_ino, &ref);
        if (r != EOK) {
            ext4fs_unlock(esb);
            return -r;
        }
        uint32_t next = ext4_inode_get_del_time(ref.inode);
        ext4_inode_set_del_time(ref.inode, 0);
        ref.dirty = true;
        ext4_fs_put_inode_ref(&ref);

        fs->sb.last_orphan = to_le32(next);
        ext4fs_unlock(esb);
        sb->dirty = 1;
        return 0;
    }

    /* Walk the list to find the predecessor */
    uint32_t prev_ino = head;
    while (prev_ino != 0) {
        struct ext4_inode_ref prev_ref;
        int r = ext4_fs_get_inode_ref(fs, prev_ino, &prev_ref);
        if (r != EOK)
            break;

        uint32_t next_ino = ext4_inode_get_del_time(prev_ref.inode);
        if (next_ino == target_ino) {
            /* Found the predecessor — relink around the target */
            struct ext4_inode_ref target_ref;
            r = ext4_fs_get_inode_ref(fs, target_ino, &target_ref);
            if (r == EOK) {
                uint32_t after = ext4_inode_get_del_time(target_ref.inode);
                ext4_inode_set_del_time(target_ref.inode, 0);
                target_ref.dirty = true;
                ext4_fs_put_inode_ref(&target_ref);

                ext4_inode_set_del_time(prev_ref.inode, after);
                prev_ref.dirty = true;
            }
            ext4_fs_put_inode_ref(&prev_ref);
            break;
        }
        ext4_fs_put_inode_ref(&prev_ref);
        prev_ino = next_ino;
    }

    ext4fs_unlock(esb);
    sb->dirty = 1;
    return 0;
}

static int ext4fs_recover_orphans(struct vfs_superblock *sb)
{
    if (sb == NULL)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    uint32_t ino = to_le32(fs->sb.last_orphan);
    int recovered = 0;

    while (ino != 0) {
        struct ext4_inode_ref ref;
        int r = ext4_fs_get_inode_ref(fs, ino, &ref);
        if (r != EOK)
            break;

        uint32_t next_ino = ext4_inode_get_del_time(ref.inode);
        uint32_t links = ext4_inode_get_links_cnt(ref.inode);

        if (links == 0) {
            /* Inode was unlinked — truncate and free */
            ext4_fs_truncate_inode(&ref, 0);
            ext4_fs_free_inode(&ref);
            recovered++;
        }

        ext4_inode_set_del_time(ref.inode, 0);
        ref.dirty = true;
        ext4_fs_put_inode_ref(&ref);

        ino = next_ino;
    }

    /* Clear the orphan list head */
    fs->sb.last_orphan = to_le32(0);
    ext4fs_unlock(esb);

    if (recovered > 0) {
        printf("ext4fs: recovered %d orphaned inode(s)\n", recovered);
        sb->dirty = 1;
    }

    return 0;
}

/*
 * No journal → transactions are no-ops.  The ext4 block cache is flushed
 * synchronously and VFS serialisation (superblock wlock) provides the
 * ordering guarantees.
 */
static int ext4fs_begin_transaction(struct vfs_superblock *sb)
{
    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);
    /* Enable write-back mode for batch of metadata changes */
    ext4fs_lock(esb);
    ext4_block_cache_write_back(&esb->bdev, 1);
    ext4fs_unlock(esb);
    return 0;
}

static int ext4fs_end_transaction(struct vfs_superblock *sb)
{
    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);
    /* Disable write-back mode → triggers flush of dirty blocks */
    ext4fs_lock(esb);
    ext4_block_cache_write_back(&esb->bdev, 0);
    ext4fs_unlock(esb);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* VFS operations tables                                                       */
/* ──────────────────────────────────────────────────────────────────────────── */

struct vfs_superblock_ops ext4fs_superblock_ops = {
    .alloc_inode      = ext4fs_alloc_inode,
    .get_inode        = ext4fs_get_inode,
    .sync_fs          = ext4fs_sync_fs,
    .unmount_begin    = ext4fs_unmount_begin,
    .add_orphan       = ext4fs_add_orphan,
    .remove_orphan    = ext4fs_remove_orphan,
    .recover_orphans  = ext4fs_recover_orphans,
    .begin_transaction = ext4fs_begin_transaction,
    .end_transaction  = ext4fs_end_transaction,
};

/* ──────────────────────────────────────────────────────────────────────────── */
/* Mount / Free (fs_type_ops)                                                  */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Mount an ext2/3/4 filesystem from the given block device.
 *
 * Flow:
 *   1. Acquire blkdev_t from device inode
 *   2. Allocate ext4fs_superblock from slab
 *   3. Set up ext4_blockdev adapter → xv6 bread/bwrite
 *   4. ext4_block_init  (opens physical device)
 *   5. ext4_bcache_init_dynamic (allocate block cache)
 *   6. ext4_block_bind_bcache + set logical block size
 *   7. ext4_fs_init  (reads & validates ext4 superblock)
 *   8. Load root inode (ino 2)
 *   9. Fill VFS superblock and return
 */
static int ext4fs_mount(struct vfs_inode *mountpoint, struct vfs_inode *device,
                        int flags, const char *data,
                        struct vfs_superblock **ret_sb)
{
    if (mountpoint == NULL || ret_sb == NULL)
        return -EINVAL;

    /* ── Get block device ── */
    dev_t dev_num;
    if (device != NULL && S_ISBLK(device->mode))
        dev_num = device->bdev;
    else
        return -EINVAL;

    blkdev_t *blkdev = blkdev_get(major(dev_num), minor(dev_num));
    if (IS_ERR(blkdev))
        return PTR_ERR(blkdev);

    /* ── Allocate ext4fs superblock ── */
    struct ext4fs_superblock *esb = slab_alloc(&ext4fs_sb_cache);
    if (esb == NULL) {
        blkdev_put(blkdev);
        return -ENOMEM;
    }
    memset(esb, 0, sizeof(*esb));
    esb->xv6_blkdev = blkdev;
    mutex_init(&esb->lock, "ext4fs");

    /* ── Set up the blockdev adapter ── */
    ext4fs_blockdev_setup(esb);

    /* ── Initialise lwext4 block layer ── */
    int r = ext4_block_init(&esb->bdev);
    if (r != EOK) {
        printf("ext4fs: ext4_block_init failed: %d\n", r);
        goto fail_blkdev;
    }

    /* ── Allocate ext4 block cache ── */
    uint32_t cache_cnt  = CONFIG_BLOCK_DEV_CACHE_SIZE;
    /* Initial item size = physical block; will be resized after sb read */
    r = ext4_bcache_init_dynamic(&esb->bcache, cache_cnt,
                                 esb->bdev_iface.ph_bsize);
    if (r != EOK) {
        printf("ext4fs: ext4_bcache_init_dynamic failed: %d\n", r);
        goto fail_block_fini;
    }

    r = ext4_block_bind_bcache(&esb->bdev, &esb->bcache);
    if (r != EOK) {
        printf("ext4fs: ext4_block_bind_bcache failed: %d\n", r);
        goto fail_bcache;
    }

    /* ── Read & validate ext4 superblock ── */
    r = ext4_fs_init(&esb->ext4fs, &esb->bdev, !!(flags & 1) /* read_only */);
    if (r != EOK) {
        printf("ext4fs: ext4_fs_init failed: %d\n", r);
        goto fail_bcache;
    }

    /* Now we know the logical block size — resize the bcache */
    uint32_t lb_size = ext4_sb_get_block_size(&esb->ext4fs.sb);
    ext4_block_set_lb_size(&esb->bdev, lb_size);

    /* Re-init bcache with the correct logical block size if different */
    if (lb_size != esb->bdev_iface.ph_bsize) {
        ext4_bcache_cleanup(&esb->bcache);
        ext4_bcache_fini_dynamic(&esb->bcache);
        r = ext4_bcache_init_dynamic(&esb->bcache, cache_cnt, lb_size);
        if (r != EOK) {
            printf("ext4fs: bcache re-init failed: %d\n", r);
            goto fail_fs_fini;
        }
        r = ext4_block_bind_bcache(&esb->bdev, &esb->bcache);
        if (r != EOK) {
            printf("ext4fs: bcache re-bind failed: %d\n", r);
            goto fail_bcache_fini;
        }
    }

    /* ── Fill VFS superblock ── */
    esb->vfs_sb.block_size   = lb_size;
    esb->vfs_sb.total_blocks = ext4_sb_get_blocks_cnt(&esb->ext4fs.sb);
    esb->vfs_sb.backendless  = 0; /* backed by disk */
    esb->vfs_sb.ops          = &ext4fs_superblock_ops;
    esb->vfs_sb.fs_data      = esb;

    /* ── Load root inode (always ino 2 in ext4) ── */
    struct vfs_inode *root_inode =
        ext4fs_get_inode(&esb->vfs_sb, EXT4_ROOT_INO);
    if (IS_ERR_OR_NULL(root_inode)) {
        printf("ext4fs: failed to load root inode\n");
        r = root_inode ? (int)PTR_ERR(root_inode) : -ENOMEM;
        goto fail_bcache_fini;
    }

    esb->vfs_sb.root_inode = root_inode;
    *ret_sb = &esb->vfs_sb;

    /* ── Recover orphaned inodes from a previous crash ── */
    if (to_le32(esb->ext4fs.sb.last_orphan) != 0)
        ext4fs_recover_orphans(&esb->vfs_sb);

    printf("ext4fs: mounted (block_size=%u, blocks=%lu)\n",
           lb_size, (unsigned long)esb->vfs_sb.total_blocks);
    return 0;

    /* ── Error paths ── */
fail_bcache_fini:
    ext4_bcache_cleanup(&esb->bcache);
    ext4_bcache_fini_dynamic(&esb->bcache);
fail_fs_fini:
    ext4_fs_fini(&esb->ext4fs);
    goto fail_blkdev;
fail_bcache:
    ext4_bcache_cleanup(&esb->bcache);
    ext4_bcache_fini_dynamic(&esb->bcache);
fail_block_fini:
    ext4_block_fini(&esb->bdev);
fail_blkdev:
    blkdev_put(blkdev);
    slab_free(esb);
    return r < 0 ? r : -r;
}

static void ext4fs_free(struct vfs_superblock *sb)
{
    struct ext4fs_superblock *esb = ext4fs_get_esb(sb);

    /* Flush and finalize */
    ext4_block_cache_flush(&esb->bdev);
    ext4_fs_fini(&esb->ext4fs);
    ext4_bcache_cleanup(&esb->bcache);
    ext4_bcache_fini_dynamic(&esb->bcache);
    ext4_block_fini(&esb->bdev);

    if (esb->xv6_blkdev != NULL)
        blkdev_put(esb->xv6_blkdev);

    slab_free(esb);
}

struct vfs_fs_type_ops ext4fs_fs_type_ops = {
    .mount = ext4fs_mount,
    .free  = ext4fs_free,
};

/* ──────────────────────────────────────────────────────────────────────────── */
/* Filesystem type registration                                                */
/* ──────────────────────────────────────────────────────────────────────────── */

void ext4fs_init(void)
{
    /* Initialise slab caches */
    int r = ext4fs_init_caches();
    assert(r == 0, "ext4fs_init: slab cache init failed, errno=%d", r);

    /* Register the filesystem type with VFS */
    struct vfs_fs_type *fs_type = vfs_fs_type_allocate();
    assert(fs_type != NULL, "ext4fs_init: vfs_fs_type_allocate failed");

    fs_type->name = "ext4";
    fs_type->ops  = &ext4fs_fs_type_ops;

    vfs_mount_lock();
    r = vfs_register_fs_type(fs_type);
    assert(r == 0, "ext4fs_init: vfs_register_fs_type failed, errno=%d", r);
    vfs_mount_unlock();

    /* Suppress noisy lwext4 debug output by default */
    ext4_dmask_set(DEBUG_ALL);
    ext4_dmask_clr(DEBUG_ALL);

    printf("ext4fs: filesystem type registered\n");
}

/**
 * Mount ext4 at /root and chroot into it.
 * Requires: tmpfs already mounted as initial root (vfs_root_inode.mnt_rooti
 * set), and devtmpfs already mounted at /dev on the tmpfs root.
 *
 * Root device selection order:
 *   1. boot command line "root=" parameter (from bootloader / QEMU -append)
 *   2. ramdisk (major 3) if available
 *   3. fallback to ROOTDEV (virtio disk, major 2)
 *
 * After mounting the real root at /root:
 *   - Ensures /root/dev exists
 *   - Moves devtmpfs from /dev to /root/dev
 *   - Chroots into /root
 */
void ext4fs_mount_root(void)
{
    struct vfs_inode *tmpfs_root = vfs_root_inode.mnt_rooti;
    if (tmpfs_root == NULL) {
        printf("ext4fs: no root filesystem to mount onto\n");
        return;
    }

    /* Create /root directory in tmpfs root */
    struct vfs_inode *root_dir = vfs_mkdir(tmpfs_root, 0755, "root", 4);
    if (IS_ERR_OR_NULL(root_dir)) {
        printf("ext4fs: failed to create /root directory\n");
        return;
    }

    /* Select root device:
     * 1. Try boot command line root= parameter
     * 2. Prefer ramdisk if available
     * 3. Fall back to compiled-in ROOTDEV */
    dev_t root_dev = cmdline_get_root_dev();
    if (root_dev != 0) {
        /* Validate that the specified device exists */
        blkdev_t *bdev = blkdev_get(major(root_dev), minor(root_dev));
        if (bdev == NULL || IS_ERR(bdev)) {
            printf("ext4fs: cmdline root device (%d,%d) not found, "
                   "falling back\n", major(root_dev), minor(root_dev));
            root_dev = 0;
        } else {
            blkdev_put(bdev);
        }
    }
    if (root_dev == 0) {
        blkdev_t *ramdisk = blkdev_get(major(RAMDISK_DEV), minor(RAMDISK_DEV));
        if (ramdisk != NULL && !IS_ERR(ramdisk)) {
            root_dev = RAMDISK_DEV;
            blkdev_put(ramdisk);
        } else {
            root_dev = ROOTDEV;
        }
    }

    /* Create a block device inode for root device */
    struct vfs_inode *dev_inode =
        vfs_mknod(tmpfs_root, S_IFBLK | 0600, root_dev, "rootdev", 7);
    if (IS_ERR_OR_NULL(dev_inode)) {
        printf("ext4fs: failed to create device inode, errno=%ld\n",
               dev_inode ? PTR_ERR(dev_inode) : -ENOMEM);
        vfs_iput(root_dir);
        return;
    }

    /* Mount ext4 at /root */
    vfs_mount_lock();
    vfs_superblock_wlock(root_dir->sb);
    vfs_ilock(root_dir);
    int ret = vfs_mount("ext4", root_dir, dev_inode, 0, NULL);
    if (ret == 0) {
        vfs_iunlock(root_dir);
        vfs_superblock_unlock(root_dir->sb);
    }
    vfs_mount_unlock();

    vfs_iput(dev_inode);

    if (ret == 0) {
        printf("ext4fs: mounted at /root\n");

        struct vfs_inode *ext4_root = root_dir->mnt_rooti;
        if (ext4_root != NULL) {
            /* Ensure /root/dev exists for moving devtmpfs */
            struct vfs_inode *new_dev = vfs_namei("/root/dev", 9);
            if (IS_ERR_OR_NULL(new_dev)) {
                new_dev = vfs_mkdir(ext4_root, 0755, "dev", 3);
                if (IS_ERR_OR_NULL(new_dev)) {
                    printf("ext4fs: failed to create /root/dev\n");
                } else {
                    vfs_iput(new_dev);
                }
            } else {
                vfs_iput(new_dev);
            }

            /* Move devtmpfs from /dev (on tmpfs) to /root/dev (on ext4) */
            ret = vfs_move_mount_path("/dev", 4, "/root/dev", 9);
            if (ret == 0) {
                printf("ext4fs: moved devtmpfs /dev -> /root/dev\n");
            } else {
                printf("ext4fs: failed to move /dev -> /root/dev, "
                       "errno=%d\n", ret);
            }

            /* Chroot into the ext4 root */
            ret = vfs_chroot(ext4_root);
            if (ret == 0) {
                printf("ext4fs: chroot to /root successful\n");
            } else {
                printf("ext4fs: chroot to /root failed, errno=%d\n", ret);
            }
        }
    } else {
        printf("ext4fs: failed to mount at /root, errno=%d\n", ret);
    }
    vfs_iput(root_dir);
}
