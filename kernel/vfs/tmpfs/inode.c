#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "completion.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "tmpfs_private.h"

// Initialize a tmpfs inode as a symlink with embedded target
static void __tmpfs_make_symlink_target_embedded(struct tmpfs_inode *tmpfs_inode, 
                                                 const char *target, size_t len) {
    memmove(tmpfs_inode->sym.data, target, len);
    if (len < TMPFS_SYMLINK_EMBEDDED_TARGET_LEN)
        memset(tmpfs_inode->sym.data + len, 0, TMPFS_SYMLINK_EMBEDDED_TARGET_LEN - len);
    tmpfs_inode->vfs_inode.size = len;
    tmpfs_inode->vfs_inode.type = VFS_I_TYPE_SYMLINK;
}

// Initialize a tmpfs inode as a symlink with allocated target
static int __tmpfs_make_symlink_target(struct tmpfs_inode *tmpfs_inode, 
                                       const char *target, size_t len) {
    char *allocated = kmm_alloc(len);
    if (allocated == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    memmove(allocated, target, len);
    tmpfs_inode->sym.symlink_target = allocated;
    tmpfs_inode->vfs_inode.size = len;
    tmpfs_inode->vfs_inode.type = VFS_I_TYPE_SYMLINK;
    return 0;
}

// Initialize a tmpfs inode as a regular file
static void __tmpfs_make_regfile(struct tmpfs_inode *tmpfs_inode) {
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->embedded = true;
    tmpfs_inode->vfs_inode.type = VFS_I_TYPE_FILE;
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

// Allocate and copy a name string from user or kernel space using kmm_alloc
static int __tmpfs_alloc_name_copy(const char *name, size_t name_len, bool user, char **ret) {
    char *name_copy = (char *)kmm_alloc(name_len + 1);
    if (name_copy == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    if (user) {
        int retval = vm_copyin(myproc()->vm, name_copy, (uint64)name, name_len);
        if (retval != 0) {
            kmm_free(name_copy);
            *ret = NULL;
            return -EFAULT;
        }
    } else {
        memmove(name_copy, name, name_len);
    }
    name_copy[name_len] = '\0';
    *ret = name_copy;
    return 0;
}

static void __tmpfs_free_dentry(struct tmpfs_dentry *dentry) {
    if (dentry != NULL) {
        kmm_free(dentry);
    }
}

// Allocate and copy a tmpfs directory entry name from user or kernel space
static int __tmpfs_dentry_name_copy(const char *name, size_t name_len, bool user,
                                    struct tmpfs_dentry **ret) {
    struct tmpfs_dentry *dentry = __tmpfs_alloc_dentry(name_len);
    if (dentry == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    if (user) {
        int retval = vm_copyin(myproc()->vm, dentry->name, (uint64)name, name_len);
        if (retval != 0) {
            __tmpfs_free_dentry(dentry);
            *ret = NULL;
            return -EFAULT;
        }
    } else {
        memmove(dentry->name, name, name_len);
    }
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
    size_t min_len = dentry_node->name_len < dentry_key->name_len ? 
                     dentry_node->name_len : dentry_key->name_len;
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
void tmpfs_make_directory(struct tmpfs_inode *tmpfs_inode, struct tmpfs_inode *parent) {
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->vfs_inode.type = VFS_I_TYPE_DIR;
    int ret = hlist_init(&tmpfs_inode->dir.children,
                         TMPFS_HASH_BUCKETS,
                         &__tmpfs_dir_hlist_funcs);
    assert(ret == 0, "Failed to initialize tmpfs directory children hash list, errno=%d", ret);
}

void tmpfs_make_cdev(struct tmpfs_inode *tmpfs_inode, dev_t cdev) {
    tmpfs_inode->vfs_inode.type = VFS_I_TYPE_CDEV;
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->vfs_inode.cdev = cdev;
}

void tmpfs_make_bdev(struct tmpfs_inode *tmpfs_inode, dev_t bdev) {
    tmpfs_inode->vfs_inode.type = VFS_I_TYPE_BDEV;
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->vfs_inode.bdev = bdev;
}

// Lookup a child inode by name in a tmpfs directory inode
static struct tmpfs_dentry *__tmpfs_dir_lookup_by_name(struct tmpfs_inode *inode, 
                                                      const char *name, 
                                                      size_t name_len) {
    struct tmpfs_dentry tmp = {
        .name = (char *)name,
        .name_len = name_len
    };
    return hlist_get(&inode->dir.children, &tmp);
}

// Allocate a dentry and link a target inode into a tmpfs directory with the given name
// WIll increase the link count of the target inode
static int __tmpfs_do_link(struct tmpfs_inode *dir, struct tmpfs_dentry *dentry) {
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
    struct tmpfs_dentry *popped = hlist_pop(&dentry->parent->dir.children, dentry);
    assert(popped == dentry, "Tmpfs unlink: popped dentry does not match");
}

// Allocate and link a new inode in the given tmpfs directory
// Caller should hold the dir inode lock
// Will not release the lock of the new inode, caller should do it
static int __tmpfs_alloc_link_inode(struct tmpfs_inode *dir, uint32 mode, struct tmpfs_inode **new_inode,    
                                    struct tmpfs_dentry **ret_dentry, const char *name, size_t name_len, 
                                    bool user) {
    struct tmpfs_inode *tmpfs_inode = NULL;
    struct vfs_inode *vfs_inode = NULL;
    struct tmpfs_dentry *dentry = NULL;
    int ret = 0;
    ret = __tmpfs_dentry_name_copy(name, name_len, user, &dentry);
    if (ret != 0) {
        goto done;
    }
    ret = __tmpfs_do_link(dir, dentry);
    if (ret != 0) {
        goto done;
    }
    ret = vfs_alloc_inode(dir->vfs_inode.sb, &vfs_inode);
    if (ret != 0) {
        goto done;
    }
    tmpfs_inode = container_of(vfs_inode, struct tmpfs_inode, vfs_inode);
    dentry->inode = tmpfs_inode;
    vfs_inode->mode = mode;
    vfs_inode->n_links = 1;
    ret = vfs_ilockdup(&tmpfs_inode->vfs_inode); // Increase ref count for the caller
    assert(ret == 0, "Tmpfs alloc link inode: failed to lock and dup inode, errno=%d", ret);
    tmpfs_inode->vfs_inode.n_links = 1;
    *new_inode = tmpfs_inode;
done:
    if (ret != 0) {
        if (dentry != NULL) {
            __tmpfs_free_dentry(dentry);
        }
        *new_inode = NULL;
    } else if (ret_dentry != NULL) {
        *ret_dentry = dentry;
    }
    return ret;
}

void tmpfs_free_symlink_target(struct tmpfs_inode *tmpfs_inode) {
    if (tmpfs_inode->vfs_inode.size >= TMPFS_SYMLINK_EMBEDDED_TARGET_LEN &&
        tmpfs_inode->sym.symlink_target != NULL) {
        kmm_free(tmpfs_inode->sym.symlink_target);
        tmpfs_inode->sym.symlink_target = NULL;
        tmpfs_inode->vfs_inode.size = 0;
    }
}





int __tmpfs_lookup(struct vfs_inode *dir, struct vfs_dentry *dentry, 
                   const char *name, size_t name_len, bool user) {
    struct tmpfs_inode *tmpfs_dir = container_of(dir, struct tmpfs_inode, vfs_inode);
    char *name_buf = NULL;
    int ret = 0;
    vfs_ilock(dir);
    if (user) {
        ret = __tmpfs_alloc_name_copy(name, name_len, user, &name_buf);
        if (ret != 0) {
            goto done;
        }
        name = name_buf;
    }
    if (name_len == 1 && strncmp(name, ".", 1) == 0) {
        dentry->ino = dir->ino;
        dentry->sb = dir->sb;
        dentry->name = kmm_alloc(2);
        memmove(dentry->name, ".", 2);
        dentry->name_len = 1;
        dentry->cookies = TMPFS_DENTRY_COOKIE_SELF;
        goto done;
    }

    if (name_len == 2 && strncmp(name, "..", 2) == 0) {
        dentry->sb = dir->sb;
        dentry->name = kmm_alloc(3);
        memmove(dentry->name, "..", 3);
        dentry->name_len = 2;
        if (tmpfs_dir->dir.parent == NULL) {
            dentry->ino = dir->ino;
        } else {
            dentry->ino = tmpfs_dir->dir.parent->vfs_inode.ino;
        }
        dentry->cookies = TMPFS_DENTRY_COOKIE_PARENT;
        goto done;
    }
    
    struct tmpfs_dentry *child_dentry = __tmpfs_dir_lookup_by_name(tmpfs_dir, name, name_len);
    if (child_dentry == NULL) {
        ret = -ENOENT; // Not found
        goto done;
    }
    dentry->ino = child_dentry->inode->vfs_inode.ino;
    dentry->sb = dir->sb;
    dentry->name = kmm_alloc(name_len + 1);
    memmove(dentry->name, name, name_len);
    dentry->name[name_len] = '\0';
    dentry->name_len = name_len;
    dentry->cookies = (uint64)child_dentry;

 done:
    vfs_iunlock(dir);
    if (user && name_buf != NULL) {
        kmm_free(name_buf);
    }
    return ret;
}

int __tmpfs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter) {
    return -ENOSYS; // Not implemented
}

int __tmpfs_readlink(struct vfs_inode *inode, char *buf, size_t buflen, bool user) {
    struct tmpfs_inode *tmpfs_inode = (struct tmpfs_inode *)inode;
    int ret = 0;
    size_t link_len = inode->size;
    if (link_len + 1 > buflen) {
        return -ENAMETOOLONG; // Buffer too small
    }
    if (user) {
        // For user space, we need to copy the data to user space
        proc_lock(myproc());
        vfs_ilock(inode);
        if (link_len < TMPFS_SYMLINK_EMBEDDED_TARGET_LEN) {
            ret = vm_copyout(myproc()->vm, (uint64)buf, tmpfs_inode->sym.data, link_len);
            if (ret != 0) {
                vfs_iunlock(inode);
                proc_unlock(myproc());
                return -EFAULT; // Failed to copy to user space
            }
        } else {
            ret = vm_copyout(myproc()->vm, (uint64)buf, tmpfs_inode->sym.symlink_target, link_len);
            if (ret != 0) {
                vfs_iunlock(inode);
                proc_unlock(myproc());
                return -EFAULT; // Failed to copy to user space
            }
        }
        ret = vm_copyout(myproc()->vm, (uint64)buf + link_len, "\0", 1);
        if (ret != 0) {
            vfs_iunlock(inode);
            proc_unlock(myproc());
            return -EFAULT; // Failed to copy to user space
        }
        vfs_iunlock(inode);
        proc_unlock(myproc());
        return (int)link_len;
    }
    vfs_ilock(inode);
    if (link_len < TMPFS_SYMLINK_EMBEDDED_TARGET_LEN) {
        memmove(buf, tmpfs_inode->sym.data, link_len);
    } else {
        memmove(buf, tmpfs_inode->sym.symlink_target, link_len);
    }
    vfs_iunlock(inode);
    buf[link_len] = '\0'; // Null-terminate the string
    return (int)link_len;
}

int __tmpfs_create(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_inode,
                   const char *name, size_t name_len, bool user) {
    struct tmpfs_inode *tmpfs_dir = container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_inode = NULL;
    vfs_ilock(dir);
    int ret = __tmpfs_alloc_link_inode(tmpfs_dir, mode, &tmpfs_inode, NULL, name, name_len, user);
    if (ret != 0) {
        vfs_iunlock(dir);
        *new_inode = NULL;
        return ret;
    }
    __tmpfs_make_regfile(tmpfs_inode);
    vfs_iunlock(&tmpfs_inode->vfs_inode);
    vfs_iunlock(dir);
    *new_inode = &tmpfs_inode->vfs_inode;
    return 0;
}

int __tmpfs_unlink(struct vfs_inode *dir, const char *name, size_t name_len, bool user) {
    struct tmpfs_inode *tmpfs_dir = container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_dentry *tmpfs_dentry = NULL;
    struct vfs_inode *target = NULL;
    int ret = 0;
    vfs_ilock(dir);
    tmpfs_dentry = __tmpfs_dir_lookup_by_name(tmpfs_dir, name, name_len);
    if (tmpfs_dentry == NULL) {
        ret = -ENOENT; // Entry not found
        goto done;
    }
    target = &tmpfs_dentry->inode->vfs_inode;
    vfs_ilock(target);
    if (target->type == VFS_I_TYPE_DIR) {
        vfs_iunlock(target);
        ret = -EISDIR; // Target is a directory
        goto done;
    }
    if (kobject_refcount(&target->kobj) > 1) {
        vfs_iunlock(target);
        ret = -EBUSY; // Target inode is busy
        goto done;
    }
    target->n_links--;
    __tmpfs_do_unlink(tmpfs_dentry);
    if (target->n_links > 0) {
        vfs_iunlock(target);
        ret = 0;
    } else if (target->n_links == 0) {
        vfs_iputunlock(target);
        ret = 0;
    } else {
        panic("Tmpfs unlink: negative link count");
    }
done:
    vfs_iunlock(dir);
    return ret;
}

int __tmpfs_link(struct vfs_dentry *old, struct vfs_inode *dir,
                 const char *name, size_t name_len, bool user) {
    struct tmpfs_inode *tmpfs_dir = container_of(dir, struct tmpfs_inode, vfs_inode);
    struct vfs_inode *target = NULL;
    struct tmpfs_inode *tmpfs_target = NULL;
    struct tmpfs_dentry *new_entry = NULL;
    int ret = 0;
    ret = vfs_get_dentry_inode(old, &target);
    if (ret != 0) {
        return ret;
    }
    tmpfs_target = container_of(target, struct tmpfs_inode, vfs_inode);
    vfs_ilock(target);
    target->n_links++;
    vfs_iunlock(target);

    ret = __tmpfs_dentry_name_copy(name, name_len, user, &new_entry);
    if (ret != 0) {
        vfs_ilock(target);
        target->n_links--;
        vfs_iputunlock(target);
        return ret;
    }

    new_entry->inode = tmpfs_target;
    vfs_ilock(dir);
    ret = __tmpfs_do_link(tmpfs_dir, new_entry);
    vfs_iunlock(dir);
    vfs_ilock(target);
    if (ret != 0) {
        target->n_links--;
        __tmpfs_free_dentry(new_entry);
    }
    vfs_iputunlock(target);
    
    return ret;
}

int __tmpfs_mkdir(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_dir,
                  const char *name, size_t name_len, bool user) {
    struct tmpfs_inode *tmpfs_dir = container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_inode = NULL;
    vfs_ilock(dir);
    int ret = __tmpfs_alloc_link_inode(tmpfs_dir, mode, &tmpfs_inode, NULL, name, name_len, user);
    vfs_iunlock(dir);
    if (ret != 0) {
        *new_dir = NULL;
        return ret;
    }
    tmpfs_make_directory(tmpfs_inode, tmpfs_dir);
    vfs_iunlock(&tmpfs_inode->vfs_inode);
    *new_dir = &tmpfs_inode->vfs_inode;
    return 0;
}

int __tmpfs_rmdir(struct vfs_inode *dir, const char *name, size_t name_len, bool user) {
    struct tmpfs_inode *tmpfs_dir = container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_dentry *tmpfs_dentry = NULL;
    struct vfs_inode *target = NULL;
    struct tmpfs_inode *tmpfs_target = NULL;
    int ret = 0;
    vfs_ilock(dir);
    tmpfs_dentry = __tmpfs_dir_lookup_by_name(tmpfs_dir, name, name_len);
    if (tmpfs_dentry == NULL) {
        ret = -ENOENT; // Entry not found
        goto done;
    }
    target = &tmpfs_dentry->inode->vfs_inode;
    tmpfs_target = container_of(target, struct tmpfs_inode, vfs_inode);
    vfs_ilock(target);
    if (target->type != VFS_I_TYPE_DIR) {
        vfs_iunlock(target);
        ret = -ENOTDIR; // Target is not a directory
        goto done;
    }
    if (hlist_len(&tmpfs_target->dir.children) != 0) {
        vfs_iunlock(target);
        ret = -ENOTEMPTY; // Directory not empty
        goto done;
    }
    if (kobject_refcount(&target->kobj) > 1) {
        vfs_iunlock(target);
        ret = -EBUSY; // Target inode is busy
        goto done;
    }
    assert(target->n_links == 1, "Tmpfs rmdir: directory link count is not 1");
    target->n_links--;
    __tmpfs_do_unlink(tmpfs_dentry);
    vfs_iputunlock(target);
done:
    vfs_iunlock(dir);
    return ret;
}

int __tmpfs_mknod(struct vfs_inode *dir, uint32 mode, struct vfs_inode **new_inode, 
                  dev_t dev, const char *name, size_t name_len, bool user) {
    struct tmpfs_inode *tmpfs_dir = container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_inode = NULL;
    vfs_inode_type_t type = vfs_mode_to_inode_type(mode);
    if (type != VFS_I_TYPE_CDEV && type != VFS_I_TYPE_BDEV) {
        // @TODO: Support FIFO, socket, and other special files
        return -EINVAL; // Mknod can only create character or block device files
    }
    vfs_ilock(dir);
    int ret = __tmpfs_alloc_link_inode(tmpfs_dir, mode, &tmpfs_inode, NULL, name, name_len, user);
    vfs_iunlock(dir);
    if (ret != 0) {
        *new_inode = NULL;
        return ret;
    }
    if (S_ISBLK(mode)) {
        tmpfs_make_bdev(tmpfs_inode, dev);
    } else if (S_ISCHR(mode)) {
        tmpfs_make_cdev(tmpfs_inode, dev);
    }
    vfs_iunlock(&tmpfs_inode->vfs_inode);
    *new_inode = &tmpfs_inode->vfs_inode;
    return 0;
}


int __tmpfs_move(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
                 struct vfs_inode *new_dir, const char *name, size_t name_len, bool user) {
    struct tmpfs_inode *tmpfs_old_dir = container_of(old_dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *tmpfs_new_dir = container_of(new_dir, struct tmpfs_inode, vfs_inode);
    struct vfs_inode *target = NULL;
    int ret = 0;
    struct tmpfs_dentry *tmpfs_old_dentry = NULL;
    struct tmpfs_dentry *new_entry = NULL;

    // Lookup the old dentry in the old directory
    vfs_ilock(old_dir);
    tmpfs_old_dentry = __tmpfs_dir_lookup_by_name(tmpfs_old_dir, old_dentry->name, old_dentry->name_len);
    vfs_iunlock(old_dir);
    if (tmpfs_old_dentry == NULL) {
        return -ENOENT; // Old entry not found
    }

    // Increase the link count and refcount of the old inode
    target = &tmpfs_old_dentry->inode->vfs_inode;
    ret = vfs_ilockdup(target);
    if (ret != 0) {
        return ret;
    }
    if ((ret = kobject_refcount(&target->kobj)) > 2) {
        vfs_iputunlock(target);
        printf("Tmpfs move: target inode is busy, %d\n", ret);
        return -EBUSY; // Target inode is busy
    }
    target->n_links++;
    vfs_iunlock(target);

    // Create a new dentry in the new directory
    ret = __tmpfs_dentry_name_copy(name, name_len, user, &new_entry);
    if (ret != 0) {
        goto done;
    }
    new_entry->inode = tmpfs_old_dentry->inode;
    vfs_ilock(new_dir);
    ret = __tmpfs_do_link(tmpfs_new_dir, new_entry);
    vfs_iunlock(new_dir);
    if (ret != 0) {
        goto done;
    }
    vfs_ilock(old_dir);
    __tmpfs_do_unlink(tmpfs_old_dentry);
    vfs_iunlock(old_dir);
    
done:
    vfs_ilock(target);
    target->n_links--;
    vfs_iputunlock(target);
    if (ret != 0 && new_entry != NULL) {
        __tmpfs_free_dentry(new_entry);
    }
    return ret;
}
            
int __tmpfs_symlink(struct vfs_inode *dir, struct vfs_inode **ret_inode,
                    uint32 mode, const char *name, size_t name_len,
                    const char *target, size_t target_len, bool user) {
    struct tmpfs_inode *tmpfs_dir = container_of(dir, struct tmpfs_inode, vfs_inode);
    struct tmpfs_inode *new_inode = NULL;
    struct tmpfs_dentry *dentry = NULL;
    vfs_ilock(dir);
    int ret = __tmpfs_alloc_link_inode(tmpfs_dir, mode, &new_inode, &dentry, name, name_len, user);
    if (ret != 0) {
        vfs_iunlock(dir);
        *ret_inode = NULL;
        return ret;
    }
    if (target_len < TMPFS_SYMLINK_EMBEDDED_TARGET_LEN) {
        __tmpfs_make_symlink_target_embedded(new_inode, target, target_len);
    } else {
        ret = __tmpfs_make_symlink_target(new_inode, target, target_len);
        if (ret != 0) {
            __tmpfs_do_unlink(dentry);
            vfs_iput(&new_inode->vfs_inode);
            vfs_iputunlock(&new_inode->vfs_inode);
            vfs_iunlock(dir);
            *ret_inode = NULL;
            return ret;
        }
    }
    vfs_iunlock(dir);
    vfs_iunlock(&new_inode->vfs_inode);
    *ret_inode = &new_inode->vfs_inode;
    return 0;
}

int __tmpfs_truncate(struct vfs_inode *inode, uint64 new_size) {
    return -ENOSYS; // Not implemented
}

void __tmpfs_destroy_inode(struct vfs_inode *inode) {
    // return -ENOSYS; // Not implemented
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
};
