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
#include <dev/dev.h>

/* ------------------------------------------------------------------ */
/*  Global device-node registry                                       */
/* ------------------------------------------------------------------ */

static spinlock_t __devtmpfs_lock = SPINLOCK_INITIALIZED("devtmpfs");
static list_node_t __devtmpfs_nodes = {0}; /* list of devtmpfs_node */
static bool __devtmpfs_initialized = false;

/* The devtmpfs filesystem type pointer, saved so we can iterate all
 * mounted instances via fs_type->superblocks. */
static struct vfs_fs_type *__devtmpfs_fs_type = NULL;

/* The mounted devtmpfs superblock.  Set by devtmpfs_mount, cleared by
 * devtmpfs_umount_free.  Protected by __devtmpfs_lock. */
static struct vfs_superblock *__devtmpfs_sb = NULL;

static void __devtmpfs_ensure_init(void) {
    if (!__devtmpfs_initialized) {
        list_entry_init(&__devtmpfs_nodes);
        __devtmpfs_initialized = true;
    }
}

/* ------------------------------------------------------------------ */
/*  Mount-point-agnostic helpers (use root inode, not "/dev/")        */
/* ------------------------------------------------------------------ */

/*
 * __devtmpfs_get_root — return the devtmpfs root inode (no extra ref).
 * Caller must hold __devtmpfs_lock OR otherwise guarantee __devtmpfs_sb
 * stays alive.  Returns NULL if devtmpfs is not mounted.
 */
static struct vfs_inode *__devtmpfs_get_root(void) {
    if (__devtmpfs_sb == NULL)
        return NULL;
    return __devtmpfs_sb->root_inode;
}

/*
 * __devtmpfs_walk_parent — given a relative path like "pts/0", walk from
 * root_inode through intermediate directories, creating them if needed.
 * Returns the parent directory inode (with an extra iref) and writes the
 * leaf name into *leaf / *leaf_len.
 *
 * On failure returns NULL.
 */
static struct vfs_inode *__devtmpfs_walk_parent(
    struct vfs_inode *root, const char *name, size_t name_len,
    const char **leaf, size_t *leaf_len) {

    /* Find the last '/' — everything before it is a directory path */
    int last_slash = -1;
    for (int i = (int)name_len - 1; i >= 0; i--) {
        if (name[i] == '/') {
            last_slash = i;
            break;
        }
    }

    struct vfs_inode *dir = root;
    vfs_idup(dir); /* take a ref so caller can always iput the result */

    if (last_slash > 0) {
        /* Walk / create intermediate directory components */
        int start = 0;
        while (start < last_slash) {
            int end = start;
            while (end < last_slash && name[end] != '/')
                end++;
            if (end == start) {
                start++;
                continue;
            }

            /* Try to look up this component */
            struct vfs_dentry dentry = {0};
            int ret = vfs_ilookup(dir, &dentry, name + start,
                                  (size_t)(end - start));
            if (ret == 0 && dentry.ino != 0) {
                /* Found — get the inode */
                struct vfs_inode *child =
                    dir->sb->ops->get_inode(dir->sb, dentry.ino);
                vfs_iput(dir);
                if (IS_ERR_OR_NULL(child))
                    return NULL;
                dir = child;
            } else {
                /* Not found — create the directory */
                struct vfs_inode *sub =
                    vfs_mkdir(dir, 0755, name + start,
                              (size_t)(end - start));
                vfs_iput(dir);
                if (IS_ERR_OR_NULL(sub))
                    return NULL;
                dir = sub;
            }
            start = end + 1;
        }
    }

    /* Leaf is the part after the last '/' (or the whole name) */
    if (last_slash >= 0) {
        *leaf = name + last_slash + 1;
        *leaf_len = name_len - (size_t)(last_slash + 1);
    } else {
        *leaf = name;
        *leaf_len = name_len;
    }
    return dir; /* caller must vfs_iput() */
}

/*
 * __devtmpfs_mknod_relative — create a device node under the devtmpfs
 * root inode using the relative name (e.g. "console", "pts/0").
 * Creates intermediate directories as needed.
 */
static int __devtmpfs_mknod_relative(struct vfs_inode *root,
                                     const char *name, size_t name_len,
                                     mode_t mode, dev_t dev) {
    const char *leaf;
    size_t leaf_len;
    struct vfs_inode *parent =
        __devtmpfs_walk_parent(root, name, name_len, &leaf, &leaf_len);
    if (parent == NULL)
        return -ENOENT;

    struct vfs_inode *inode = vfs_mknod(parent, mode, dev, leaf, leaf_len);
    int ret = 0;
    if (IS_ERR(inode)) {
        ret = (int)PTR_ERR(inode);
        if (ret == -EEXIST)
            ret = 0; /* idempotent */
    } else {
        vfs_iput(inode);
    }
    vfs_iput(parent);
    return ret;
}

/*
 * __devtmpfs_unlink_relative — remove a device node from under the
 * devtmpfs root inode using the relative name (e.g. "pts/0").
 */
static int __devtmpfs_unlink_relative(struct vfs_inode *root,
                                      const char *name, size_t name_len) {
    const char *leaf;
    size_t leaf_len;
    struct vfs_inode *parent =
        __devtmpfs_walk_parent(root, name, name_len, &leaf, &leaf_len);
    if (parent == NULL)
        return -ENOENT;

    int ret = vfs_unlink(parent, leaf, leaf_len);
    vfs_iput(parent);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Populate a mounted devtmpfs with registered device nodes          */
/* ------------------------------------------------------------------ */

/*
 * Called AFTER vfs_mount_path() succeeds, so the superblock and
 * all inodes are fully VFS-initialised.  Uses the stored __devtmpfs_sb
 * root inode so that the code is independent of the mount path.
 */
int devtmpfs_post_mount_populate(void) {
    struct vfs_inode *root = __devtmpfs_get_root();
    if (root == NULL) {
        printf("devtmpfs: post_mount_populate called but no sb\n");
        return -ENODEV;
    }

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

        int ret = __devtmpfs_mknod_relative(root, n, nl, m, d);
        if (ret != 0 && ret != -EEXIST)
            printf("devtmpfs: mknod '%s' failed: %d\n", n, ret);

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

    /* Save the superblock so create_node/remove_node can find the root
     * inode without relying on the mount path (e.g. "/dev"). */
    __devtmpfs_sb = &sb->vfs_sb;

    /* NOTE: Do NOT populate here.  The superblock / root inode are not
     * yet initialised by VFS (rwsem, mutex, inode hash, sb->valid are
     * all set up by __vfs_init_superblock_structure / __vfs_init_sb_rooti
     * which run AFTER the mount callback returns).  Population is done
     * by devtmpfs_post_mount_populate() called from vfs_init(). */

    return 0;
}

static void devtmpfs_umount_free(struct vfs_superblock *sb) {
    if (__devtmpfs_sb == sb)
        __devtmpfs_sb = NULL;
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
    __devtmpfs_ensure_init();

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
    list_entry_push_front(&__devtmpfs_nodes, &node->list_entry);
    spin_unlock(&__devtmpfs_lock);

    /*
     * If devtmpfs is already mounted, create the node live using the
     * stored superblock root inode (mount-point independent).
     * This is needed for devices registered after boot (e.g. PTYs).
     */
    struct vfs_inode *root = __devtmpfs_get_root();
    if (root != NULL)
        __devtmpfs_mknod_relative(root, name, name_len, mode, dev);

    return 0;
}

int devtmpfs_remove_node(const char *name) {
    __devtmpfs_ensure_init();

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
     * Uses the stored root inode so we don't depend on mount path.
     */
    struct vfs_inode *root = __devtmpfs_get_root();
    if (root != NULL)
        __devtmpfs_unlink_relative(root, name, name_len);

    return found ? 0 : -ENOENT;
}

/* ------------------------------------------------------------------ */
/*  Retroactive population from device table                          */
/* ------------------------------------------------------------------ */

/*
 * Callback for dev_for_each_device(): create a devtmpfs registry entry
 * for each device that has a devname set but hasn't been registered yet.
 */
static int __devtmpfs_register_one_device(device_t *dev, void *ctx) {
    (void)ctx;
    if (dev->devname == NULL)
        return 0; /* device has no devtmpfs binding — skip */

    /* Check if a node already exists in the registry (e.g. because
     * device_register already called devtmpfs_create_node after the
     * registry list was initialised).  Avoid duplicates. */
    spin_lock(&__devtmpfs_lock);
    struct devtmpfs_node *node;
    struct devtmpfs_node *tmp;
    size_t name_len = strlen(dev->devname);
    int found = 0;
    list_foreach_node_safe(&__devtmpfs_nodes, node, tmp, list_entry) {
        if (node->name_len == name_len &&
            strncmp(node->name, dev->devname, name_len) == 0) {
            found = 1;
            break;
        }
    }
    spin_unlock(&__devtmpfs_lock);

    if (!found)
        devtmpfs_create_node(dev->devname, dev->devmode,
                             mkdev(dev->major, dev->minor));
    return 0;
}

void devtmpfs_populate_devices(void) {
    __devtmpfs_ensure_init();
    int count = 0;

    /* Use the dev_for_each_device iterator to find all devices
     * that were registered before devtmpfs was initialised. */
    dev_for_each_device(__devtmpfs_register_one_device, NULL);

    /* Count how many nodes are now in the registry */
    spin_lock(&__devtmpfs_lock);
    struct devtmpfs_node *node;
    struct devtmpfs_node *tmp;
    list_foreach_node_safe(&__devtmpfs_nodes, node, tmp, list_entry)
        count++;
    spin_unlock(&__devtmpfs_lock);

    printf("devtmpfs: populated %d device nodes from device table\n", count);
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

    /*
     * Retroactively create devtmpfs entries for all devices that were
     * registered before devtmpfs became available.  device_register()
     * calls devtmpfs_create_node() which buffers entries into the
     * registry list even when devtmpfs is not yet mounted.  However,
     * for devices that were registered before this subsystem was
     * initialised (i.e. before __devtmpfs_ensure_init ran), we now
     * iterate them and add any that carry a devname.
     */
    devtmpfs_populate_devices();
}
