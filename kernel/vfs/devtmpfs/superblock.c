/*
 * devtmpfs/superblock.c - devtmpfs filesystem type and superblock operations
 *
 * devtmpfs reuses the tmpfs inode/dentry infrastructure but registers as
 * a separate filesystem type.  On mount it auto-populates /dev with the
 * device nodes that have been registered so far via devtmpfs_create_node().
 *
 * Unlike Linux's devtmpfs which uses a kernel thread to serialise node
 * creation, this implementation directly creates/removes nodes under the
 * global devtmpfs spinlock.  This is sufficient for xv6 where device
 * registration happens during early boot on a single hart.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
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
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include <mm/page.h>
#include "../tmpfs/tmpfs_private.h"
#include "devtmpfs_private.h"
#include "printf.h"

/* ------------------------------------------------------------------ */
/*  Global device-node registry                                       */
/* ------------------------------------------------------------------ */

static spinlock_t __devtmpfs_lock = SPINLOCK_INITIALIZED("devtmpfs");
static list_node_t __devtmpfs_nodes = {0}; /* list of devtmpfs_node */
static bool __devtmpfs_initialized = false;

/* The devtmpfs filesystem type pointer, saved so we can iterate all
 * mounted instances via fs_type->superblocks. */
static struct vfs_fs_type *__devtmpfs_fs_type = NULL;

static void __devtmpfs_ensure_init(void) {
    if (!__devtmpfs_initialized) {
        list_entry_init(&__devtmpfs_nodes);
        __devtmpfs_initialized = true;
    }
}

/* ------------------------------------------------------------------ */
/*  Populate a mounted devtmpfs with registered device nodes          */
/* ------------------------------------------------------------------ */

/*
 * Called AFTER vfs_mount_path("/dev") succeeds, so the superblock and
 * all inodes are fully VFS-initialised.  Uses VFS-level vfs_mknod /
 * vfs_mkdir which handle their own locking.
 */
int devtmpfs_post_mount_populate(void) {
    struct devtmpfs_node *node;
    struct devtmpfs_node *tmp;

    spin_lock(&__devtmpfs_lock);
    list_foreach_node_safe(&__devtmpfs_nodes, node, tmp, list_entry) {
        /* Snapshot fields so we can release the lock */
        const char *n = node->name;
        size_t nl = node->name_len;
        mode_t m = node->mode;
        dev_t d = node->dev;
        spin_unlock(&__devtmpfs_lock);

        /* Build "/dev/<name>" full path for namei helpers */
        char fullpath[128];
        const char *prefix = "/dev/";
        size_t plen = 5; /* strlen("/dev/") */
        if (plen + nl + 1 > sizeof(fullpath)) {
            printf("devtmpfs: name '%s' too long\n", n);
            spin_lock(&__devtmpfs_lock);
            continue;
        }
        memmove(fullpath, prefix, plen);
        memmove(fullpath + plen, n, nl);
        fullpath[plen + nl] = '\0';
        int pathlen = (int)(plen + nl);

        /* If there are intermediate directories (e.g. "pts/0"),
         * create each component.  Find the last '/'. */
        for (int i = pathlen - 1; i > 4 /* past "/dev/" */; i--) {
            if (fullpath[i] == '/') {
                /* Need to ensure parent directory exists.
                 * E.g. for "/dev/pts/0", ensure "/dev/pts". */
                char dirpath[128];
                memmove(dirpath, fullpath, i);
                dirpath[i] = '\0';

                struct vfs_inode *parent_dir =
                    vfs_namei(dirpath, i);
                if (IS_ERR_OR_NULL(parent_dir)) {
                    /* Parent of dir doesn't exist either — just use
                     * a simple split to create the leaf dir under
                     * /dev.  (We only support 1-level subdirs.) */
                    struct vfs_inode *dev_root =
                        vfs_namei("/dev", 4);
                    if (!IS_ERR_OR_NULL(dev_root)) {
                        /* Extract the directory component name */
                        const char *dname = dirpath + 5; /* skip "/dev/" */
                        size_t dname_len = i - 5;
                        struct vfs_inode *sub =
                            vfs_mkdir(dev_root, 0755, dname, dname_len);
                        if (!IS_ERR(sub))
                            vfs_iput(sub);
                        vfs_iput(dev_root);
                    }
                } else {
                    /* Parent dir already exists */
                    vfs_iput(parent_dir);
                }
                break; /* only handle the deepest split */
            }
        }

        /* Now create the device node.  Use vfs_nameiparent to get the
         * parent directory and leaf name. */
        char leaf[64];
        struct vfs_inode *parent =
            vfs_nameiparent(fullpath, pathlen, leaf, sizeof(leaf));
        if (IS_ERR_OR_NULL(parent)) {
            printf("devtmpfs: cannot find parent for '%s'\n", n);
            spin_lock(&__devtmpfs_lock);
            continue;
        }

        struct vfs_inode *inode =
            vfs_mknod(parent, m, d, leaf, strlen(leaf));
        if (IS_ERR(inode)) {
            if (PTR_ERR(inode) != -EEXIST)
                printf("devtmpfs: mknod '%s' failed: %ld\n",
                       n, PTR_ERR(inode));
        } else {
            vfs_iput(inode);
        }
        vfs_iput(parent);

        spin_lock(&__devtmpfs_lock);
    }
    spin_unlock(&__devtmpfs_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Superblock ops — delegate to tmpfs                                */
/* ------------------------------------------------------------------ */

/* Forward declarations from tmpfs that we reuse */
extern struct vfs_inode *tmpfs_get_inode(struct vfs_superblock *sb, uint64 ino);
extern int tmpfs_sync_fs(struct vfs_superblock *sb, int wait);
extern void tmpfs_unmount_begin(struct vfs_superblock *sb);

struct vfs_superblock_ops devtmpfs_superblock_ops = {
    .alloc_inode = tmpfs_alloc_inode,
    .get_inode = tmpfs_get_inode,
    .sync_fs = tmpfs_sync_fs,
    .unmount_begin = tmpfs_unmount_begin,
};

/* ------------------------------------------------------------------ */
/*  Filesystem type mount/free                                        */
/* ------------------------------------------------------------------ */

static int devtmpfs_mount(struct vfs_inode *mountpoint,
                          struct vfs_inode *device, int flags,
                          const char *data,
                          struct vfs_superblock **ret_sb) {
    if (mountpoint == NULL || ret_sb == NULL)
        return -EINVAL;
    if (device != NULL)
        return -EINVAL; /* devtmpfs is backendless */

    struct tmpfs_superblock *sb = tmpfs_alloc_superblock();
    if (sb == NULL)
        return -ENOMEM;

    /*
     * Initialise next_ino to 1 BEFORE allocating the root inode.
     * tmpfs_alloc_inode calls __tmpfs_ino_alloc which returns next_ino++,
     * so it would return 0 (and fail with ENOENT) if we leave it at the
     * memset-zero default.  The root inode gets ino 1, and we bump
     * next_ino to 2 afterwards for subsequent allocations.
     */
    sb->private_data.next_ino = 1;

    struct tmpfs_inode *root_inode;
    /* Use tmpfs_alloc_inode to get a proper inode from the shared cache */
    struct vfs_inode *vi = tmpfs_alloc_inode(&sb->vfs_sb);
    if (IS_ERR(vi)) {
        tmpfs_free(&sb->vfs_sb);
        return PTR_ERR(vi);
    }
    root_inode = container_of(vi, struct tmpfs_inode, vfs_inode);
    tmpfs_make_directory(root_inode);
    root_inode->vfs_inode.n_links = 2;

    sb->vfs_sb.block_size = PAGE_SIZE;
    sb->vfs_sb.root_inode = &root_inode->vfs_inode;
    sb->vfs_sb.backendless = 1;
    sb->vfs_sb.ops = &devtmpfs_superblock_ops;

    *ret_sb = &sb->vfs_sb;

    /* NOTE: Do NOT populate here.  The superblock / root inode are not
     * yet initialised by VFS (rwsem, mutex, inode hash, sb->valid are
     * all set up by __vfs_init_superblock_structure / __vfs_init_sb_rooti
     * which run AFTER the mount callback returns).  Population is done
     * by devtmpfs_post_mount_populate() called from vfs_init(). */

    return 0;
}

static void devtmpfs_umount_free(struct vfs_superblock *sb) {
    tmpfs_free(sb);
}

struct vfs_fs_type_ops devtmpfs_fs_type_ops = {
    .mount = devtmpfs_mount,
    .free = devtmpfs_umount_free,
};

/* ------------------------------------------------------------------ */
/*  Public API: create / remove device nodes across all instances     */
/* ------------------------------------------------------------------ */

int devtmpfs_create_node(const char *name, mode_t mode, dev_t dev) {
    if (name == NULL)
        return -EINVAL;

    size_t name_len = strlen(name);
    if (name_len == 0)
        return -EINVAL;

    /* Allocate a registry entry (node struct + name string in one block) */
    struct devtmpfs_node *node = kmm_alloc(sizeof(*node) + name_len + 1);
    if (node == NULL)
        return -ENOMEM;

    char *name_copy = (char *)(node + 1);
    memmove(name_copy, name, name_len);
    name_copy[name_len] = '\0';
    node->name = name_copy;
    node->name_len = name_len;
    node->mode = mode;
    node->dev = dev;
    list_entry_init(&node->list_entry);

    spin_lock(&__devtmpfs_lock);
    list_entry_push_back(&__devtmpfs_nodes, &node->list_entry);
    spin_unlock(&__devtmpfs_lock);

    /*
     * If devtmpfs is already mounted at /dev, create the node live.
     * This is needed for devices registered after boot (e.g. PTYs).
     * Guard: during early boot (devtmpfs_init), no root filesystem
     * is mounted yet, so vfs_namei would panic in vfs_curroot().
     */
    if (current == NULL || current->fs == NULL ||
        vfs_inode_deref(&current->fs->rooti) == NULL)
        return 0;

    struct vfs_inode *dev_root = vfs_namei("/dev", 4);
    if (!IS_ERR_OR_NULL(dev_root)) {
        /* Build full path /dev/<name> */
        char fullpath[128];
        const char *prefix = "/dev/";
        size_t plen = 5;
        if (plen + name_len + 1 <= sizeof(fullpath)) {
            memmove(fullpath, prefix, plen);
            memmove(fullpath + plen, name, name_len);
            fullpath[plen + name_len] = '\0';
            int pathlen = (int)(plen + name_len);

            /* Create intermediate directories if needed */
            for (int i = pathlen - 1; i > 4; i--) {
                if (fullpath[i] == '/') {
                    char dircomp[64];
                    size_t dlen = i - 5; /* skip "/dev/" */
                    if (dlen > 0 && dlen < sizeof(dircomp)) {
                        memmove(dircomp, fullpath + 5, dlen);
                        dircomp[dlen] = '\0';
                        struct vfs_inode *sub =
                            vfs_mkdir(dev_root, 0755, dircomp, dlen);
                        if (!IS_ERR(sub))
                            vfs_iput(sub);
                        /* Ignore EEXIST */
                    }
                    break;
                }
            }

            /* Use vfs_nameiparent + vfs_mknod for the leaf */
            char leaf[64];
            struct vfs_inode *parent =
                vfs_nameiparent(fullpath, pathlen, leaf, sizeof(leaf));
            if (!IS_ERR_OR_NULL(parent)) {
                struct vfs_inode *inode =
                    vfs_mknod(parent, mode, dev, leaf, strlen(leaf));
                if (!IS_ERR(inode))
                    vfs_iput(inode);
                /* Ignore EEXIST for idempotency */
                vfs_iput(parent);
            }
        }
        vfs_iput(dev_root);
    }

    return 0;
}

int devtmpfs_remove_node(const char *name) {
    if (name == NULL)
        return -EINVAL;

    size_t name_len = strlen(name);
    if (name_len == 0)
        return -EINVAL;

    /* Remove from the registry list */
    struct devtmpfs_node *node = NULL;
    struct devtmpfs_node *tmp = NULL;
    int found = 0;

    spin_lock(&__devtmpfs_lock);
    list_foreach_node_safe(&__devtmpfs_nodes, node, tmp, list_entry) {
        if (node->name_len == name_len &&
            strncmp(node->name, name, name_len) == 0) {
            list_node_detach(node, list_entry);
            found = 1;
            break;
        }
    }
    spin_unlock(&__devtmpfs_lock);

    if (found)
        kmm_free(node);

    /*
     * Also unlink the live filesystem node if devtmpfs is mounted.
     * We do this regardless of whether we found a registry entry,
     * because the node could have been created live and already
     * removed from the registry.
     */
    if (current != NULL && current->fs != NULL &&
        vfs_inode_deref(&current->fs->rooti) != NULL) {
        char fullpath[128];
        const char *prefix = "/dev/";
        size_t plen = 5;
        if (plen + name_len + 1 <= sizeof(fullpath)) {
            memmove(fullpath, prefix, plen);
            memmove(fullpath + plen, name, name_len);
            fullpath[plen + name_len] = '\0';
            int pathlen = (int)(plen + name_len);

            char leaf[64];
            struct vfs_inode *parent =
                vfs_nameiparent(fullpath, pathlen, leaf, sizeof(leaf));
            if (!IS_ERR_OR_NULL(parent)) {
                vfs_unlink(parent, leaf, strlen(leaf));
                vfs_iput(parent);
            }
        }
    }

    return found ? 0 : -ENOENT;
}

/* ------------------------------------------------------------------ */
/*  Initialisation                                                    */
/* ------------------------------------------------------------------ */

void devtmpfs_init(void) {
    __devtmpfs_ensure_init();

    /* Register the devtmpfs filesystem type */
    struct vfs_fs_type *fs_type = vfs_fs_type_allocate();
    assert(fs_type != NULL, "devtmpfs_init: vfs_fs_type_allocate failed");
    fs_type->name = "devtmpfs";
    fs_type->ops = &devtmpfs_fs_type_ops;
    __devtmpfs_fs_type = fs_type;

    vfs_mount_lock();
    int ret = vfs_register_fs_type(fs_type);
    assert(ret == 0,
           "devtmpfs_init: vfs_register_fs_type failed, errno=%d", ret);
    vfs_mount_unlock();

    printf("devtmpfs: filesystem type registered\n");

    /* ---- Character devices ---- */
    devtmpfs_create_node("console", S_IFCHR | 0666,
                         mkdev(CONSOLE_MAJOR, CONSOLE_MINOR));
    devtmpfs_create_node("null", S_IFCHR | 0666,
                         mkdev(NULL_MAJOR, NULL_MINOR));
    devtmpfs_create_node("random", S_IFCHR | 0666,
                         mkdev(RANDOM_MAJOR, RANDOM_MINOR));
    devtmpfs_create_node("zero", S_IFCHR | 0666,
                         mkdev(ZERO_MAJOR, ZERO_MINOR));
    devtmpfs_create_node("tty", S_IFCHR | 0666,
                         mkdev(TTY_DEV_MAJOR, TTY_DEV_MINOR));
    devtmpfs_create_node("ptmx", S_IFCHR | 0666,
                         mkdev(PTMX_MAJOR, PTMX_MINOR));

    /* ---- Block devices ---- */
    devtmpfs_create_node("disk0", S_IFBLK | 0600, mkdev(2, 1));
    devtmpfs_create_node("disk1", S_IFBLK | 0600, mkdev(2, 2));
    devtmpfs_create_node("ramdisk", S_IFBLK | 0600, mkdev(3, 1));

    printf("devtmpfs: pre-registered %d built-in device nodes\n", 9);
}
