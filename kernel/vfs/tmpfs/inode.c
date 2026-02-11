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
#include <mm/vm.h>
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include "tmpfs_private.h"

// Initialize a tmpfs inode as a symlink with embedded target
static void
__tmpfs_make_symlink_target_embedded(struct tmpfs_inode *tmpfs_inode,
                                     const char *target, size_t len) {
    memmove(tmpfs_inode->sym.data, target, len);
    if (len < TMPFS_INODE_EMBEDDED_DATA_LEN)
        memset(tmpfs_inode->sym.data + len, 0,
               TMPFS_INODE_EMBEDDED_DATA_LEN - len);
    tmpfs_inode->vfs_inode.size = len;
    tmpfs_inode->vfs_inode.mode = S_IFLNK | 0777;
}

// Initialize a tmpfs inode as a symlink with allocated target
static int __tmpfs_make_symlink_target(struct tmpfs_inode *tmpfs_inode,
                                       const char *target, size_t len) {
    char *allocated = strndup(target, len);
    if (allocated == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    tmpfs_inode->sym.symlink_target = allocated;
    tmpfs_inode->vfs_inode.size = len;
    tmpfs_inode->vfs_inode.mode = S_IFLNK | 0777;
    return 0;
}

// Initialize a tmpfs inode as a regular file
static void __tmpfs_make_regfile(struct tmpfs_inode *tmpfs_inode) {
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->embedded = true;
    tmpfs_inode->vfs_inode.mode = S_IFREG | 0644;
    memset(&tmpfs_inode->file, 0, sizeof(tmpfs_inode->file));
}

/******************************************************************************
 * Tmpfs Dir Entry helpers
 ******************************************************************************/
static struct tmpfs_dentry *__tmpfs_alloc_dentry(size_t name_len) {
    struct tmpfs_dentry *dentry = kmm_alloc(sizeof(*dentry) + name_len + 1);
    if (dentry == NULL) {
        return NULL;
    }
    memset(dentry, 0, sizeof(*dentry) + name_len + 1);
    dentry->name_len = name_len;
    dentry->name = dentry->__name_start;
    hlist_entry_init(&dentry->hash_entry);
    return dentry;
}

static void __tmpfs_free_dentry(struct tmpfs_dentry *dentry) {
    if (dentry != NULL) {
        kmm_free(dentry);
    }
}

// Allocate and copy a tmpfs directory entry name
static int __tmpfs_dentry_name_copy(const char *name, size_t name_len,
                                    struct tmpfs_dentry **ret) {
    struct tmpfs_dentry *dentry = __tmpfs_alloc_dentry(name_len);
    if (dentry == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    memmove(dentry->name, name, name_len);
    dentry->name[name_len] = '\0';
    *ret = dentry;
    return 0;
}
/******************************************************************************
 * Tmpfs directory hash list functions
 *****************************************************************************/
static ht_hash_t __tmpfs_dir_hash_func(void *data) {
    struct tmpfs_dentry *dentry = (struct tmpfs_dentry *)data;
    return hlist_hash_str(dentry->name, dentry->name_len);
}

static int __tmpfs_dir_name_cmp_func(hlist_t *hlist, void *node, void *key) {
    (void)hlist;
    struct tmpfs_dentry *dentry_node = (struct tmpfs_dentry *)node;
    struct tmpfs_dentry *dentry_key = (struct tmpfs_dentry *)key;
    size_t min_len = dentry_node->name_len < dentry_key->name_len
                         ? dentry_node->name_len
                         : dentry_key->name_len;
    int cmp = strncmp(dentry_node->name, dentry_key->name, min_len);
    if (cmp != 0) {
        return cmp;
    }
    // If the prefixes are equal, compare lengths
    if (dentry_node->name_len > dentry_key->name_len) {
        return 1;
    } else if (dentry_node->name_len < dentry_key->name_len) {
        return -1;
    } else {
        return 0;
    }
}

static void *__tmpfs_dir_get_node_func(hlist_entry_t *entry) {
    if (entry == NULL) {
        return NULL;
    }
    return container_of(entry, struct tmpfs_dentry, hash_entry);
}

static hlist_entry_t *__tmpfs_dir_get_entry_func(void *node) {
    if (node == NULL) {
        return NULL;
    }
    struct tmpfs_dentry *dentry = (struct tmpfs_dentry *)node;
    return &dentry->hash_entry;
}

static struct hlist_func_struct __tmpfs_dir_hlist_funcs = {
    .hash = __tmpfs_dir_hash_func,
    .cmp_node = __tmpfs_dir_name_cmp_func,
    .get_node = __tmpfs_dir_get_node_func,
    .get_entry = __tmpfs_dir_get_entry_func,
};

// Initialize a tmpfs inode as a directory
void tmpfs_make_directory(struct tmpfs_inode *tmpfs_inode) {
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->vfs_inode.mode = S_IFDIR | 0755;
    int ret = hlist_init(&tmpfs_inode->dir.children, TMPFS_HASH_BUCKETS,
                         &__tmpfs_dir_hlist_funcs);
    assert(ret == 0,
           "Failed to initialize tmpfs directory children hash list, errno=%d",
           ret);
}

void tmpfs_make_cdev(struct tmpfs_inode *tmpfs_inode, dev_t cdev) {
    tmpfs_inode->vfs_inode.mode = S_IFCHR | 0644;
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->vfs_inode.cdev = cdev;
}

void tmpfs_make_bdev(struct tmpfs_inode *tmpfs_inode, dev_t bdev) {
    tmpfs_inode->vfs_inode.mode = S_IFBLK | 0644;
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->vfs_inode.bdev = bdev;
}

// Lookup a child inode by name in a tmpfs directory inode
static struct tmpfs_dentry *
__tmpfs_dir_lookup_by_name(struct tmpfs_inode *inode, const char *name,
                           size_t name_len) {
    struct tmpfs_dentry tmp = {.name = (char *)name, .name_len = name_len};
    return hlist_get(&inode->dir.children, &tmp);
}

// Allocate a dentry and link a target inode into a tmpfs directory with the
// given name WIll increase the link count of the target inode
static int __tmpfs_do_link(struct tmpfs_inode *dir,
                           struct tmpfs_dentry *dentry) {
    struct tmpfs_dentry *ret = hlist_put(&dir->dir.children, dentry, false);
    if (ret == dentry) {
        dentry->inode->vfs_inode.n_links++;
        return 0;
    }
    if (ret != NULL) {
        return -EEXIST; // Entry already exists
    }
    dentry->parent = dir;
    dentry->sb = dir->vfs_inode.sb;
    return 0;
}

// Unlink a dentry from its parent tmpfs directory
// Will decrease the link count of the target inode
static void __tmpfs_do_unlink(struct tmpfs_dentry *dentry) {
    struct tmpfs_dentry *popped =
        hlist_pop(&dentry->parent->dir.children, dentry);
    assert(popped == dentry, "Tmpfs unlink: popped dentry does not match");
}

// Allocate and link a new inode in the given tmpfs directory
// Caller should hold the dir inode lock
// Will not release the lock of the new inode, caller should do it
static struct tmpfs_inode *
__tmpfs_alloc_link_inode(struct tmpfs_inode *dir, mode_t mode,
                         struct tmpfs_dentry **ret_dentry, const char *name,
                         size_t name_len) {
    struct vfs_inode *vfs_inode = NULL;
    struct tmpfs_inode *tmpfs_inode = NULL;
    struct tmpfs_dentry *dentry = NULL;
    int ret = __tmpfs_dentry_name_copy(name, name_len, &dentry);
    if (ret != 0) {
        return ERR_PTR(ret);
    }
    ret = __tmpfs_do_link(dir, dentry);
    if (ret != 0) {
        __tmpfs_free_dentry(dentry);
        return ERR_PTR(ret);
    }
    vfs_inode = vfs_alloc_inode(dir->vfs_inode.sb);
    if (IS_ERR(vfs_inode)) {
        ret = PTR_ERR(vfs_inode);
        vfs_inode = NULL;
        __tmpfs_do_unlink(dentry);
        __tmpfs_free_dentry(dentry);
        return ERR_PTR(ret);
    }
    tmpfs_inode = container_of(vfs_inode, struct tmpfs_inode, vfs_inode);
    dentry->inode = tmpfs_inode;
    vfs_inode->mode = mode;
    vfs_inode->n_links = 1;
    // vfs_ilock(&tmpfs_inode->vfs_inode);
    // Backendless inodes are kept alive by n_links > 0, so refcount of 1
    // suffices
    tmpfs_inode->vfs_inode.n_links = 1;
    if (ret_dentry != NULL) {
        *ret_dentry = dentry;
    }
    return tmpfs_inode;
}

void tmpfs_free_symlink_target(struct tmpfs_inode *tmpfs_inode) {
    if (tmpfs_inode->vfs_inode.size >= TMPFS_INODE_EMBEDDED_DATA_LEN &&
        tmpfs_inode->sym.symlink_target != NULL) {
        kmm_free(tmpfs_inode->sym.symlink_target);
        tmpfs_inode->sym.symlink_target = NULL;
        tmpfs_inode->vfs_inode.size = 0;
    }
}

/******************************************************************************
 * tmpfs inode callbacks
 *****************************************************************************/

int __tmpfs_lookup(struct vfs_inode *dir, struct vfs_dentry *dentry,
                   const char *name, size_t name_len) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dir, struct tmpfs_inode, vfs_inode);
    int ret = 0;

    // VFS handles "." and ".." for process root and local root.
    // Driver only sees ".." for ordinary (non-root) directories.
    if (name_len == 2 && strncmp(name, "..", 2) == 0) {
        dentry->sb = dir->sb;
        dentry->name = strndup(name, name_len);
        if (dentry->name == NULL) {
            return -ENOMEM;
        }
        dentry->name_len = 2;
        dentry->ino = dir->parent->ino;
        dentry->cookies = VFS_DENTRY_COOKIE_PARENT;
        return 0;
    }

    struct tmpfs_dentry *child_dentry =
        __tmpfs_dir_lookup_by_name(tmpfs_dir, name, name_len);
    if (child_dentry == NULL) {
        return -ENOENT; // Not found
    }
    dentry->ino = child_dentry->inode->vfs_inode.ino;
    dentry->sb = dir->sb;
    dentry->parent = dir;
    dentry->name = strndup(name, name_len);
    if (dentry->name == NULL) {
        return -ENOMEM;
    }
    dentry->name_len = name_len;
    dentry->cookies = (uint64)child_dentry;
    return ret;
}

// VFS synthesizes "." at index 0 and ".." for process/local roots at index 1.
// Driver handles ".." for ordinary dirs (index 1) and all children (index > 2).
int __tmpfs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter,
                     struct vfs_dentry *dentry) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_dentry *current = NULL;
    char *name = NULL;

    if (iter->index == 1) {
        // VFS passes ".." to driver only for non-root directories
        if (dir->parent == NULL) {
            return -ENOENT; // No parent (should not happen for non-root)
        }
        vfs_release_dentry(dentry); // Release any previous name
        dentry->name = strndup("..", 2);
        if (dentry->name == NULL) {
            return -ENOMEM;
        }
        dentry->name_len = 2;
        dentry->cookies = VFS_DENTRY_COOKIE_PARENT;
        dentry->ino = dir->parent->ino;
        return 0;
    }

    // index > 2: iterate over directory children
    if (dentry->cookies == VFS_DENTRY_COOKIE_END ||
        dentry->cookies == VFS_DENTRY_COOKIE_PARENT) {
        current = HLIST_FIRST_NODE(&tmpfs_dir->dir.children,
                                   struct tmpfs_dentry, hash_entry);
    } else {
        current = (struct tmpfs_dentry *)dentry->cookies;
        current =
            HLIST_NEXT_NODE(&tmpfs_dir->dir.children, current, hash_entry);
    }

    if (current == NULL) {
        // End of directory
        vfs_release_dentry(dentry); // Release any previous name
        dentry->name = NULL;
        dentry->cookies = VFS_DENTRY_COOKIE_END;
        return 0;
    }

    name = strndup(current->name, current->name_len);
    if (name == NULL) {
        return -ENOMEM;
    }
    vfs_release_dentry(dentry); // Release any previous name
    dentry->name = name;
    dentry->name_len = current->name_len;
    dentry->ino = current->inode->vfs_inode.ino;
    dentry->cookies = (uint64)current;
    return 0;
}

ssize_t __tmpfs_readlink(struct vfs_inode *inode, char *buf, size_t buflen) {
    struct tmpfs_inode *tmpfs_inode = (struct tmpfs_inode *)inode;
    size_t link_len = inode->size;
    if (link_len + 1 > buflen) {
        return -ENAMETOOLONG; // Buffer too small
    }
    if (link_len < TMPFS_INODE_EMBEDDED_DATA_LEN) {
        memmove(buf, tmpfs_inode->sym.data, link_len);
    } else {
        memmove(buf, tmpfs_inode->sym.symlink_target, link_len);
    }
    buf[link_len] = '\0'; // Null-terminate the string
    return (int)link_len;
}

struct vfs_inode *__tmpfs_create(struct vfs_inode *dir, mode_t mode,
                                 const char *name, size_t name_len) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_inode =
        __tmpfs_alloc_link_inode(tmpfs_dir, mode, NULL, name, name_len);
    if (IS_ERR(tmpfs_inode)) {
        return ERR_PTR(PTR_ERR(tmpfs_inode));
    }
    __tmpfs_make_regfile(tmpfs_inode);
    vfs_iunlock(&tmpfs_inode->vfs_inode);
    return &tmpfs_inode->vfs_inode;
}

int __tmpfs_unlink(struct vfs_dentry *dentry, struct vfs_inode *target) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dentry->parent, struct tmpfs_inode, vfs_inode);
    struct tmpfs_dentry *tmpfs_dentry = NULL;
    // We need to lookup the dentry again to get the tmpfs_dentry
    tmpfs_dentry =
        __tmpfs_dir_lookup_by_name(tmpfs_dir, dentry->name, dentry->name_len);
    if (tmpfs_dentry == NULL) {
        return -ENOENT; // Entry not found
    }
    if (&tmpfs_dentry->inode->vfs_inode != target) {
        return -EINVAL; // Target inode does not match
    }

    // Remove directory entry - this makes the file inaccessible by name
    // even if it's still open (Unix semantics)
    target->n_links--;
    __tmpfs_do_unlink(tmpfs_dentry);
    __tmpfs_free_dentry(tmpfs_dentry);

    assert(target->n_links >= 0, "Tmpfs unlink: negative link count");
    // VFS layer will call vfs_iput on target after we return
    return 0;
}

int __tmpfs_link(struct vfs_inode *target, struct vfs_inode *dir,
                 const char *name, size_t name_len) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_target =
        container_of(target, struct tmpfs_inode, vfs_inode);
    ;
    struct tmpfs_dentry *new_entry = NULL;
    int ret = 0;
    target->n_links++;

    ret = __tmpfs_dentry_name_copy(name, name_len, &new_entry);
    if (ret != 0) {
        target->n_links--;
        return ret;
    }

    new_entry->inode = tmpfs_target;
    ret = __tmpfs_do_link(tmpfs_dir, new_entry);
    if (ret != 0) {
        target->n_links--;
        __tmpfs_free_dentry(new_entry);
    }

    return ret;
}

struct vfs_inode *__tmpfs_mkdir(struct vfs_inode *dir, mode_t mode,
                                const char *name, size_t name_len) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_inode =
        __tmpfs_alloc_link_inode(tmpfs_dir, mode, NULL, name, name_len);
    if (IS_ERR(tmpfs_inode)) {
        return ERR_PTR(PTR_ERR(tmpfs_inode));
    }
    tmpfs_make_directory(tmpfs_inode);
    // Directory has n_links=2 for "." and ".." entries
    tmpfs_inode->vfs_inode.n_links = 2;
    // Increment parent's n_links for this subdir's ".." entry
    dir->n_links++;
    vfs_iunlock(&tmpfs_inode->vfs_inode);
    return &tmpfs_inode->vfs_inode;
}

int __tmpfs_rmdir(struct vfs_dentry *dentry, struct vfs_inode *target) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dentry->parent, struct tmpfs_inode, vfs_inode);
    struct tmpfs_dentry *tmpfs_dentry = NULL;
    tmpfs_dentry =
        __tmpfs_dir_lookup_by_name(tmpfs_dir, dentry->name, dentry->name_len);
    if (tmpfs_dentry == NULL) {
        return -ENOENT; // Entry not found
    }
    if (&tmpfs_dentry->inode->vfs_inode != target) {
        return -EINVAL; // Target inode does not match
    }
    // VFS core already verified directory is empty and not in use
    // Directory n_links should be 2 (for "." and "..") when empty
    assert(target->n_links == 2, "Tmpfs rmdir: directory link count is not 2");
    target->n_links -= 2; // Remove both "." and ".." links
    // Decrement parent's n_links for this subdir's ".." entry
    dentry->parent->n_links--;
    __tmpfs_do_unlink(tmpfs_dentry);
    __tmpfs_free_dentry(tmpfs_dentry);
    // VFS layer will call vfs_iput on target after we return

    return 0;
}

struct vfs_inode *__tmpfs_mknod(struct vfs_inode *dir, mode_t mode, dev_t dev,
                                const char *name, size_t name_len) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_inode = NULL;
    if (!S_ISBLK(mode) && !S_ISCHR(mode)) {
        // @TODO: Support FIFO, socket, and other special files
        return ERR_PTR(
            -EINVAL); // Mknod can only create character or block device files
    }
    tmpfs_inode =
        __tmpfs_alloc_link_inode(tmpfs_dir, mode, NULL, name, name_len);
    if (IS_ERR(tmpfs_inode)) {
        return ERR_PTR(PTR_ERR(tmpfs_inode));
    }
    if (S_ISBLK(mode)) {
        tmpfs_make_bdev(tmpfs_inode, dev);
    } else if (S_ISCHR(mode)) {
        tmpfs_make_cdev(tmpfs_inode, dev);
    }
    vfs_iunlock(&tmpfs_inode->vfs_inode);
    return &tmpfs_inode->vfs_inode;
}

int __tmpfs_move(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
                 struct vfs_inode *new_dir, const char *name, size_t name_len) {
    struct tmpfs_inode *tmpfs_old_dir =
        container_of(old_dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_new_dir =
        container_of(new_dir, struct tmpfs_inode, vfs_inode);
    struct vfs_inode *target = NULL;
    int ret = 0;
    struct tmpfs_dentry *tmpfs_old_dentry = NULL;
    struct tmpfs_dentry *new_entry = NULL;

    // Lookup the old dentry in the old directory
    tmpfs_old_dentry = __tmpfs_dir_lookup_by_name(
        tmpfs_old_dir, old_dentry->name, old_dentry->name_len);
    if (tmpfs_old_dentry == NULL) {
        return -ENOENT; // Old entry not found
    }

    // Increase the link count and refcount of the old inode
    target = &tmpfs_old_dentry->inode->vfs_inode;
    if ((ret = vfs_inode_refcount(target)) > 2) {
        printf("Tmpfs move: target inode is busy, %d\n", ret);
        return -EBUSY; // Target inode is busy
    }
    target->n_links++;

    // Create a new dentry in the new directory
    ret = __tmpfs_dentry_name_copy(name, name_len, &new_entry);
    if (ret != 0) {
        goto done;
    }
    new_entry->inode = tmpfs_old_dentry->inode;
    ret = __tmpfs_do_link(tmpfs_new_dir, new_entry);
    if (ret != 0) {
        goto done;
    }
    __tmpfs_do_unlink(tmpfs_old_dentry);

done:
    target->n_links--;
    if (ret != 0 && new_entry != NULL) {
        __tmpfs_free_dentry(new_entry);
    } else {
        __tmpfs_free_dentry(tmpfs_old_dentry);
    }
    return ret;
}

struct vfs_inode *__tmpfs_symlink(struct vfs_inode *dir, mode_t mode,
                                  const char *name, size_t name_len,
                                  const char *target, size_t target_len) {
    struct tmpfs_inode *tmpfs_dir =
        container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *new_inode = NULL;
    struct tmpfs_dentry *dentry = NULL;
    new_inode =
        __tmpfs_alloc_link_inode(tmpfs_dir, mode, &dentry, name, name_len);
    if (IS_ERR(new_inode)) {
        return ERR_PTR(PTR_ERR(new_inode));
    }
    if (target_len < TMPFS_INODE_EMBEDDED_DATA_LEN) {
        __tmpfs_make_symlink_target_embedded(new_inode, target, target_len);
    } else {
        int ret = __tmpfs_make_symlink_target(new_inode, target, target_len);
        if (ret != 0) {
            __tmpfs_do_unlink(dentry);
            int rm_ret = vfs_remove_inode(dir->sb, &new_inode->vfs_inode);
            assert(rm_ret == 0,
                   "Tmpfs symlink: failed to remove inode after symlink target "
                   "allocation failure, errno=%d",
                   rm_ret);
            __tmpfs_free_dentry(dentry);
            // Inode is locked and detached from superblock; directly free it
            vfs_iunlock(&new_inode->vfs_inode);
            tmpfs_free_inode(&new_inode->vfs_inode);
            return ERR_PTR(ret);
        }
    }
    vfs_iunlock(&new_inode->vfs_inode);
    return &new_inode->vfs_inode;
}

// Destroy inode data when the last reference is dropped and n_links == 0
// Called with inode locked and superblock write-locked
void __tmpfs_destroy_inode(struct vfs_inode *inode) {
    struct tmpfs_inode *ti = container_of(inode, struct tmpfs_inode, vfs_inode);

    if (S_ISREG(inode->mode)) {
        // For regular files, teardown pcache (which frees all cached pages).
        // Embedded files have no pcache, so this is a no-op for them.
        tmpfs_inode_pcache_teardown(inode);
    } else if (S_ISLNK(inode->mode)) {
        // For symlinks, free the target string if allocated externally
        tmpfs_free_symlink_target(ti);
    }
    // For directories, they must be empty before rmdir, nothing to do
    // For device nodes/pipes/sockets, no data to free
}

struct vfs_inode_ops tmpfs_inode_ops = {
    .lookup = __tmpfs_lookup,
    .dir_iter = __tmpfs_dir_iter,
    .readlink = __tmpfs_readlink,
    .create = __tmpfs_create,
    .link = __tmpfs_link,
    .unlink = __tmpfs_unlink,
    .mkdir = __tmpfs_mkdir,
    .rmdir = __tmpfs_rmdir,
    .mknod = __tmpfs_mknod,
    .move = __tmpfs_move,
    .symlink = __tmpfs_symlink,
    .truncate = __tmpfs_truncate,
    .destroy_inode = __tmpfs_destroy_inode,
    .free_inode = tmpfs_free_inode,
    .open = tmpfs_open,
};
