/*
 * ext4fs VFS driver — private header
 *
 * Bridges lwext4 library into xv6's VFS subsystem.
 * Uses lwext4's low-level ext4_fs_* / ext4_dir_* / ext4_inode_* APIs directly,
 * bypassing lwext4's own mountpoint system.
 */
#ifndef KERNEL_VFS_EXT4FS_PRIVATE_H
#define KERNEL_VFS_EXT4FS_PRIVATE_H

#include "types.h"
#include "vfs/vfs_types.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include <mm/slab.h>
#include <mm/pcache.h>
#include "dev/blkdev.h"

/* lwext4 headers */
#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_blockdev.h>
#include <ext4_bcache.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_dir.h>
#include <ext4_dir_idx.h>
#include <ext4_super.h>

/* ──────────────────────────────────────────────────────────────────────────── */
/* Per-mount superblock structure                                              */
/* ──────────────────────────────────────────────────────────────────────────── */

struct ext4fs_superblock {
    struct vfs_superblock vfs_sb;           /* MUST be first (container_of)  */
    struct ext4_blockdev bdev;              /* lwext4 block device           */
    struct ext4_blockdev_iface bdev_iface;  /* blockdev interface vtable     */
    uint8_t  ph_bbuf[512];                 /* physical block bounce buffer  */
    struct ext4_bcache bcache;             /* lwext4 block cache            */
    struct ext4_fs ext4fs;                 /* lwext4 filesystem state       */
    blkdev_t *xv6_blkdev;                 /* xv6 block device reference    */
    mutex_t lock;                          /* protects ext4fs operations    */
};

#define ext4fs_sb_dev(esb)                                                     \
    mkdev((esb)->xv6_blkdev->dev.major, (esb)->xv6_blkdev->dev.minor)

/* ──────────────────────────────────────────────────────────────────────────── */
/* Per-inode structure                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

struct ext4fs_inode {
    struct vfs_inode vfs_inode;  /* MUST be first (container_of) */
    struct {
        spinlock_t lock;
        uint64 lblk_start;
        uint64 pblk_start;
        uint32 len;
        uint8 valid;
        uint8 hole;
    } map_cache;
};

static inline struct ext4fs_inode *ext4fs_inode_from_vfs(struct vfs_inode *vi)
{
    if (vi == NULL)
        return NULL;
    return container_of(vi, struct ext4fs_inode, vfs_inode);
}

static inline void ext4fs_inode_map_cache_init(struct ext4fs_inode *ei)
{
    if (ei == NULL)
        return;
    spin_init(&ei->map_cache.lock, "ext4_map_cache");
    ei->map_cache.lblk_start = 0;
    ei->map_cache.pblk_start = 0;
    ei->map_cache.len = 0;
    ei->map_cache.valid = 0;
    ei->map_cache.hole = 0;
}

static inline void ext4fs_inode_map_cache_invalidate(struct vfs_inode *vi)
{
    struct ext4fs_inode *ei = ext4fs_inode_from_vfs(vi);
    if (ei == NULL)
        return;
    ei->map_cache.valid = 0;
    ei->map_cache.len = 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Operations tables                                                           */
/* ──────────────────────────────────────────────────────────────────────────── */

extern struct vfs_fs_type_ops     ext4fs_fs_type_ops;
extern struct vfs_superblock_ops  ext4fs_superblock_ops;
extern struct vfs_inode_ops       ext4fs_inode_ops;
extern struct vfs_file_ops        ext4fs_file_ops;

/* ──────────────────────────────────────────────────────────────────────────── */
/* Slab caches                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

extern slab_cache_t ext4fs_inode_cache;
extern slab_cache_t ext4fs_sb_cache;

/* ──────────────────────────────────────────────────────────────────────────── */
/* Helpers                                                                     */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Convert ext4 inode mode to VFS mode_t */
static inline mode_t ext4_imode_to_vfs_mode(uint32_t imode)
{
    mode_t mode = 0;
    /* file type */
    switch (imode & 0xF000) {
    case EXT4_INODE_MODE_FILE:      mode = S_IFREG; break;
    case EXT4_INODE_MODE_DIRECTORY: mode = S_IFDIR; break;
    case EXT4_INODE_MODE_CHARDEV:   mode = S_IFCHR; break;
    case EXT4_INODE_MODE_BLOCKDEV:  mode = S_IFBLK; break;
    case EXT4_INODE_MODE_FIFO:      mode = S_IFIFO; break;
    case EXT4_INODE_MODE_SOCKET:    mode = S_IFSOCK; break;
    case EXT4_INODE_MODE_SOFTLINK:  mode = S_IFLNK; break;
    default: mode = S_IFREG; break;
    }
    /* permission bits */
    mode |= (imode & 0xFFF);
    return mode;
}

/* Convert VFS mode_t to ext4 inode mode */
static inline uint32_t vfs_mode_to_ext4_imode(mode_t mode)
{
    uint32_t imode = mode & 0xFFF; /* permission bits */
    if (S_ISDIR(mode))       imode |= EXT4_INODE_MODE_DIRECTORY;
    else if (S_ISCHR(mode))  imode |= EXT4_INODE_MODE_CHARDEV;
    else if (S_ISBLK(mode))  imode |= EXT4_INODE_MODE_BLOCKDEV;
    else if (S_ISFIFO(mode)) imode |= EXT4_INODE_MODE_FIFO;
    else if (S_ISSOCK(mode)) imode |= EXT4_INODE_MODE_SOCKET;
    else if (S_ISLNK(mode))  imode |= EXT4_INODE_MODE_SOFTLINK;
    else                     imode |= EXT4_INODE_MODE_FILE;
    return imode;
}

/* Convert VFS mode to ext4 dir entry filetype */
static inline int vfs_mode_to_ext4_filetype(mode_t mode)
{
    if (S_ISDIR(mode))       return EXT4_DE_DIR;
    if (S_ISCHR(mode))       return EXT4_DE_CHRDEV;
    if (S_ISBLK(mode))       return EXT4_DE_BLKDEV;
    if (S_ISFIFO(mode))      return EXT4_DE_FIFO;
    if (S_ISSOCK(mode))      return EXT4_DE_SOCK;
    if (S_ISLNK(mode))       return EXT4_DE_SYMLINK;
    return EXT4_DE_REG_FILE;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Public init / mount                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

void ext4fs_init(void);
void ext4fs_mount_root(void);
void ext4fs_inode_pcache_init(struct vfs_inode *inode);

/*
 * Filesystem-wide lock.  Serialises all lwext4 operations on this mount.
 * Must be held around every sequence of ext4_block_get / ext4_bcache / etc.
 * calls.  This replaces the per-operation EXT4_MP_LOCK used in vanilla lwext4.
 *
 * Lock ordering:  VFS inode lock  →  ext4fs_lock  →  xv6 buffer cache
 */
static inline void ext4fs_lock(struct ext4fs_superblock *esb)   { mutex_lock(&esb->lock); }
static inline void ext4fs_unlock(struct ext4fs_superblock *esb) { mutex_unlock(&esb->lock); }

/* Set up the lwext4 blockdev interface for a mounted filesystem */
void ext4fs_blockdev_setup(struct ext4fs_superblock *esb);

/* Fill a VFS inode from an ext4 inode reference */
void ext4fs_fill_vfs_inode(struct vfs_inode *vi, struct ext4_inode_ref *ref,
                           struct ext4_sblock *sb);

/* Get an ext4_fs pointer from a VFS superblock */
static inline struct ext4_fs *ext4fs_get_fs(struct vfs_superblock *sb)
{
    struct ext4fs_superblock *esb =
        container_of(sb, struct ext4fs_superblock, vfs_sb);
    return &esb->ext4fs;
}

/* Get the ext4fs_superblock from a VFS superblock */
static inline struct ext4fs_superblock *ext4fs_get_esb(struct vfs_superblock *sb)
{
    return container_of(sb, struct ext4fs_superblock, vfs_sb);
}

#endif /* KERNEL_VFS_EXT4FS_PRIVATE_H */
