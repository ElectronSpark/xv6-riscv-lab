/*
 * procfs/superblock.c - procfs superblock, slab cache, mount, and eviction
 *
 * Locking hierarchy:
 *   pid_lock (rcu)  →  sb wlock  →  inode mutex
 *
 * procfs is a singleton: only one instance is ever mounted at /proc.
 * All dentries are virtual; the inode number encodes both the entry type
 * and the owning tgid, so the filesystem is fully stateless except for
 * the inode hash kept by the VFS.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "lock/mutex_types.h"
#include "lock/rwsem.h"
#include "lock/rcu.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include "proc/thread.h"
#include "printf.h"
#include "procfs_private.h"

/* ------------------------------------------------------------------ */
/*  Module globals                                                     */
/* ------------------------------------------------------------------ */

static slab_cache_t __procfs_inode_cache = {0};
static slab_cache_t __procfs_sb_cache    = {0};

/* Singleton superblock pointer – set on first mount, cleared on unmount */
static struct vfs_superblock *__procfs_sb = NULL;

/* Wrapped vfs_superblock for slab allocation */
struct procfs_superblock {
    struct vfs_superblock vfs_sb; /* must be first */
};

/* ------------------------------------------------------------------ */
/*  Slab helpers                                                       */
/* ------------------------------------------------------------------ */

static int __procfs_init_caches(void) {
    int ret = slab_cache_init(&__procfs_inode_cache,
                              "procfs_inode_cache",
                              sizeof(struct procfs_inode),
                              SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    if (ret != 0)
        return ret;
    ret = slab_cache_init(&__procfs_sb_cache,
                          "procfs_sb_cache",
                          sizeof(struct procfs_superblock),
                          SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  alloc_inode / free_inode callbacks                                */
/* ------------------------------------------------------------------ */

/*
 * procfs_alloc_inode - allocate a blank procfs inode from slab.
 *
 * This is called by the VFS when creating a NEW inode (e.g., vfs_create).
 * For read-only procfs this should never be reached in practice, but VFS
 * requires the callback to be non-NULL.
 */
struct vfs_inode *procfs_alloc_inode(struct vfs_superblock *sb) {
    (void)sb;
    struct procfs_inode *pi = slab_alloc(&__procfs_inode_cache);
    if (pi == NULL)
        return ERR_PTR(-ENOMEM);
    memset(pi, 0, sizeof(*pi));
    pi->vfs_inode.ops = &procfs_inode_ops;
    return &pi->vfs_inode;
}

/*
 * procfs_free_inode - return a procfs inode to the slab cache.
 *
 * Called by the VFS when the last reference drops and the inode is to
 * be freed.  No on-disk resources to release.
 */
void procfs_free_inode(struct vfs_inode *inode) {
    slab_free(procfs_i(inode));
}

/* ------------------------------------------------------------------ */
/*  get_inode callback – decode inode number to entry type + fields   */
/* ------------------------------------------------------------------ */

/*
 * procfs_get_inode - load a procfs inode from its encoded inode number.
 *
 * This is the primary factory function.  VFS holds the superblock write
 * lock on entry; __vfs_inode_init() and vfs_add_inode() are called by
 * the VFS after we return – the driver must NOT call them.
 *
 * Returns a freshly allocated, pre-filled procfs_inode on success, or
 * ERR_PTR on failure.  ref_count is set to 1 as required by the VFS.
 */
struct vfs_inode *procfs_get_inode(struct vfs_superblock *sb, uint64 ino) {
    if (sb == NULL)
        return ERR_PTR(-EINVAL);

    struct procfs_inode *pi = slab_alloc(&__procfs_inode_cache);
    if (pi == NULL)
        return ERR_PTR(-ENOMEM);
    memset(pi, 0, sizeof(*pi));

    /* Fields common to every inode */
    pi->vfs_inode.ino       = ino;
    pi->vfs_inode.ops       = &procfs_inode_ops;
    /* NOTE: do NOT set pi->vfs_inode.sb here — vfs_add_inode sets it */
    pi->vfs_inode.ref_count = 1;
    pi->vfs_inode.uid       = 0;
    pi->vfs_inode.gid       = 0;

    /* ---- Static top-level entries ---- */

    if (ino == PROCFS_INO_ROOT) {
        pi->type                 = PROC_ROOT;
        pi->vfs_inode.mode       = S_IFDIR | 0555;
        pi->vfs_inode.n_links    = 2;
        /* root inode's parent points to itself (VFS convention) */
        pi->vfs_inode.parent     = &pi->vfs_inode;
        return &pi->vfs_inode;
    }

    if (ino == PROCFS_INO_SELF) {
        pi->type              = PROC_SELF;
        pi->vfs_inode.mode    = S_IFLNK | 0777;
        pi->vfs_inode.n_links = 1;
        pi->vfs_inode.size    = 20; /* conservative upper bound */
        return &pi->vfs_inode;
    }

    if (ino == PROCFS_INO_MEMINFO) {
        pi->type              = PROC_MEMINFO;
        pi->vfs_inode.mode    = S_IFREG | 0444;
        pi->vfs_inode.n_links = 1;
        pi->vfs_inode.size    = 4096;
        return &pi->vfs_inode;
    }

    if (ino == PROCFS_INO_CPUINFO) {
        pi->type              = PROC_CPUINFO;
        pi->vfs_inode.mode    = S_IFREG | 0444;
        pi->vfs_inode.n_links = 1;
        pi->vfs_inode.size    = 4096;
        return &pi->vfs_inode;
    }

    if (ino == PROCFS_INO_CRASHES) {
        pi->type              = PROC_CRASHES;
        pi->vfs_inode.mode    = S_IFREG | 0444;
        pi->vfs_inode.n_links = 1;
        pi->vfs_inode.size    = 4096;
        return &pi->vfs_inode;
    }

    if (ino == PROCFS_INO_CMDLINE) {
        pi->type              = PROC_CMDLINE;
        pi->vfs_inode.mode    = S_IFREG | 0444;
        pi->vfs_inode.n_links = 1;
        pi->vfs_inode.size    = 512;
        return &pi->vfs_inode;
    }

    /* ---- Per-process entries ---- */

    if (ino >= PROCFS_PID_BASE && ino < PROCFS_FD_BASE) {
        uint64 offset = ino - PROCFS_PID_BASE;
        int    tgid   = (int)(offset / 10);
        int    slot   = (int)(offset % 10);

        if (tgid <= 0 || slot > 5) {
            slab_free(pi);
            return ERR_PTR(-ENOENT);
        }

        /* Validate process existence under RCU */
        rcu_read_lock();
        struct thread *p = NULL;
        get_pid_thread(tgid, &p);
        rcu_read_unlock();

        if (p == NULL || p->tgid != tgid || p->pid != p->tgid) {
            slab_free(pi);
            return ERR_PTR(-ENOENT);
        }

        pi->pid = tgid;

        switch (slot) {
        case 0: /* /proc/<tgid>/ */
            pi->type              = PROC_PID_DIR;
            pi->vfs_inode.mode    = S_IFDIR | 0555;
            pi->vfs_inode.n_links = 2;
            break;
        case 1: /* /proc/<tgid>/status */
            pi->type              = PROC_STATUS;
            pi->vfs_inode.mode    = S_IFREG | 0444;
            pi->vfs_inode.n_links = 1;
            pi->vfs_inode.size    = 4096;
            break;
        case 2: /* /proc/<tgid>/maps */
            pi->type              = PROC_MAPS;
            pi->vfs_inode.mode    = S_IFREG | 0444;
            pi->vfs_inode.n_links = 1;
            pi->vfs_inode.size    = 4096;
            break;
        case 3: /* /proc/<tgid>/exe */
            pi->type              = PROC_EXE;
            pi->vfs_inode.mode    = S_IFLNK | 0777;
            pi->vfs_inode.n_links = 1;
            pi->vfs_inode.size    = 128;
            break;
        case 4: /* /proc/<tgid>/fd/ */
            pi->type              = PROC_FDDIR;
            pi->vfs_inode.mode    = S_IFDIR | 0555;
            pi->vfs_inode.n_links = 2;
            break;
        case 5: /* /proc/<tgid>/resources */
            pi->type              = PROC_RESOURCES;
            pi->vfs_inode.mode    = S_IFREG | 0444;
            pi->vfs_inode.n_links = 1;
            pi->vfs_inode.size    = 4096;
            break;
        default:
            slab_free(pi);
            return ERR_PTR(-ENOENT);
        }
        return &pi->vfs_inode;
    }

    /* ---- fd symlink entries ---- */

    if (ino >= PROCFS_FD_BASE) {
        uint64 offset = ino - PROCFS_FD_BASE;
        int    tgid   = (int)(offset / 1000);
        int    fd     = (int)(offset % 1000);

        if (tgid <= 0 || fd < 0 || fd >= NOFILE) {
            slab_free(pi);
            return ERR_PTR(-ENOENT);
        }

        /* Validate process existence */
        rcu_read_lock();
        struct thread *p = NULL;
        get_pid_thread(tgid, &p);
        rcu_read_unlock();

        if (p == NULL || p->tgid != tgid || p->pid != p->tgid) {
            slab_free(pi);
            return ERR_PTR(-ENOENT);
        }

        pi->type              = PROC_FD_ENTRY;
        pi->pid               = tgid;
        pi->fd                = fd;
        pi->vfs_inode.mode    = S_IFLNK | 0777;
        pi->vfs_inode.n_links = 1;
        pi->vfs_inode.size    = 256;
        return &pi->vfs_inode;
    }

    slab_free(pi);
    return ERR_PTR(-ENOENT);
}

/* ------------------------------------------------------------------ */
/*  sync_fs / unmount_begin                                           */
/* ------------------------------------------------------------------ */

int procfs_sync_fs(struct vfs_superblock *sb, int wait) {
    (void)sb;
    (void)wait;
    return 0;
}

/*
 * procfs_unmount_begin - evict all unreferenced procfs inodes.
 *
 * Locking: caller holds sb write lock.
 */
void procfs_unmount_begin(struct vfs_superblock *sb) {
    if (sb == NULL)
        return;

    struct vfs_inode *rooti = sb->root_inode;
    struct vfs_inode *inode, *tmp_inode;

    hlist_foreach_node_safe(&sb->inodes, inode, tmp_inode, hash_entry) {
        if (inode == rooti)
            continue;
        if (inode->ref_count > 0)
            continue;

        vfs_ilock(inode);
        if (inode->ref_count > 0) {
            vfs_iunlock(inode);
            continue;
        }
        if (inode->ops && inode->ops->destroy_inode)
            inode->ops->destroy_inode(inode);
        inode->valid = 0;
        vfs_remove_inode(sb, inode);
        vfs_iunlock(inode);
        inode->ops->free_inode(inode);
    }
}

struct vfs_superblock_ops procfs_sb_ops = {
    .alloc_inode    = procfs_alloc_inode,
    .get_inode      = procfs_get_inode,
    .sync_fs        = procfs_sync_fs,
    .unmount_begin  = procfs_unmount_begin,
};

/* ------------------------------------------------------------------ */
/*  procfs_evict_pid – invalidate all cached inodes for a tgid        */
/* ------------------------------------------------------------------ */

/*
 * procfs_evict_pid - remove all procfs inodes belonging to @tgid.
 *
 * Called from exit.c after the process has been removed from the pid
 * table.  Unreferenced inodes are freed immediately; inodes still held
 * open are invalidated (n_links = 0) so they will be freed on the last
 * vfs_iput().
 */
void procfs_evict_pid(int tgid) {
    if (__procfs_sb == NULL)
        return;

    struct vfs_superblock *sb = __procfs_sb;

    vfs_superblock_wlock(sb);

    struct vfs_inode *inode, *tmp_inode;
    hlist_foreach_node_safe(&sb->inodes, inode, tmp_inode, hash_entry) {
        struct procfs_inode *pi = procfs_i(inode);
        if (pi->pid != tgid)
            continue;

        if (inode->ref_count > 0) {
            /* Still open: mark dead so VFS frees on last release */
            vfs_ilock(inode);
            inode->n_links = 0;
            vfs_iunlock(inode);
            continue;
        }

        vfs_ilock(inode);
        if (inode->ref_count > 0) {
            inode->n_links = 0;
            vfs_iunlock(inode);
            continue;
        }
        if (inode->ops && inode->ops->destroy_inode)
            inode->ops->destroy_inode(inode);
        inode->valid = 0;
        vfs_remove_inode(sb, inode);
        vfs_iunlock(inode);
        inode->ops->free_inode(inode);
    }

    vfs_superblock_unlock(sb);
}

/* ------------------------------------------------------------------ */
/*  mount / free                                                      */
/* ------------------------------------------------------------------ */

static int procfs_mount(struct vfs_inode *mountpoint, struct vfs_inode *device,
                        int flags, const char *data,
                        struct vfs_superblock **ret_sb) {
    (void)flags;
    (void)data;

    if (mountpoint == NULL || ret_sb == NULL)
        return -EINVAL;
    if (device != NULL)
        return -EINVAL; /* procfs does not use a block device */

    /* Allocate the superblock wrapper */
    struct procfs_superblock *psb = slab_alloc(&__procfs_sb_cache);
    if (psb == NULL)
        return -ENOMEM;
    memset(psb, 0, sizeof(*psb));

    struct vfs_superblock *sb = &psb->vfs_sb;
    sb->backendless = 1;
    sb->block_size  = 512;
    sb->ops         = &procfs_sb_ops;

    /* Create and install the root inode directly (same pattern as tmpfs) */
    struct procfs_inode *rooti = slab_alloc(&__procfs_inode_cache);
    if (rooti == NULL) {
        slab_free(psb);
        return -ENOMEM;
    }
    memset(rooti, 0, sizeof(*rooti));
    rooti->type                     = PROC_ROOT;
    rooti->vfs_inode.ino            = PROCFS_INO_ROOT;
    rooti->vfs_inode.mode           = S_IFDIR | 0555;
    rooti->vfs_inode.n_links        = 2;
    rooti->vfs_inode.ops            = &procfs_inode_ops;
    rooti->vfs_inode.ref_count      = 1;
    /* NOTE: do NOT set .sb here — vfs_add_inode sets it */
    rooti->vfs_inode.parent         = &rooti->vfs_inode; /* root → self */

    sb->root_inode = &rooti->vfs_inode;

    /* Remember the singleton */
    __procfs_sb = sb;

    *ret_sb = sb;
    return 0;
}

static void procfs_free(struct vfs_superblock *sb) {
    __procfs_sb = NULL;
    struct procfs_superblock *psb =
        container_of(sb, struct procfs_superblock, vfs_sb);
    slab_free(psb);
}

struct vfs_fs_type_ops procfs_fs_type_ops = {
    .mount = procfs_mount,
    .free  = procfs_free,
};

/* ------------------------------------------------------------------ */
/*  procfs_init – called once from vfs_init()                        */
/* ------------------------------------------------------------------ */

void procfs_init(void) {
    int ret = __procfs_init_caches();
    assert(ret == 0, "procfs_init: cache init failed, errno=%d", ret);

    struct vfs_fs_type *fs_type = vfs_fs_type_allocate();
    assert(fs_type != NULL, "procfs_init: vfs_fs_type_allocate failed");

    fs_type->name = "procfs";
    fs_type->ops  = &procfs_fs_type_ops;

    vfs_mount_lock();
    ret = vfs_register_fs_type(fs_type);
    assert(ret == 0, "procfs_init: vfs_register_fs_type failed, errno=%d", ret);
    vfs_mount_unlock();

    printf("procfs: initialized\n");
}
