#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
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
    memcpy(tmpfs_inode->sym.data, target, len);
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
    memcpy(allocated, target, len);
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
 * Tmpfs directory hash list functions
 *****************************************************************************/
static void __tmpfs_dir_hash_func(void *data) {
    struct tmpfs_inode *inode = (struct tmpfs_inode *)data;
    return hlist_hash_string(inode->name, inode->name_len);
}

static int __tmpfs_dir_name_cmp_func(hlist_t *hlist, void *node, void *key) {
    (void)hlist;
    struct tmpfs_inode *inode_node = (struct tmpfs_inode *)node;
    struct tmpfs_inode *inode_key = (struct tmpfs_inode *)key;
    size_t min_len = inode_node->name_len < inode_key->name_len ? 
                     inode_node->name_len : inode_key->name_len;
    int cmp = strncmp(inode_node->name, inode_key->name, min_len);
    if (cmp != 0) {
        return cmp;
    }
    // If the prefixes are equal, compare lengths
    if (inode_node->name_len > inode_key->name_len) {
        return 1;
    } else if (inode_node->name_len < inode_key->name_len) {
        return -1;
    } else {
        return 0;
    }
}

static void *__tmpfs_dir_get_node_func(hlist_entry_t *entry) {
    if (entry == NULL) {
        return NULL;
    }
    return container_of(entry, struct tmpfs_inode, hash_entry);
}

static hlist_entry_t *__tmpfs_dir_get_entry_func(void *node) {
    if (node == NULL) {
        return NULL;
    }
    struct tmpfs_inode *inode = (struct tmpfs_inode *)node;
    return &inode->hash_entry;
}

static struct hlist_func_struct __tmpfs_dir_hlist_funcs = {
    .hash = __tmpfs_dir_hash_func,
    .cmp_node = __tmpfs_dir_name_cmp_func,
    .get_node = __tmpfs_dir_get_node_func,
    .get_entry = __tmpfs_dir_get_entry_func,
};

// Initialize a tmpfs inode as a directory
static void __tmpfs_make_directory(struct tmpfs_inode *tmpfs_inode, 
                                   struct tmpfs_inode *parent) {
    tmpfs_inode->vfs_inode.size = 0;
    tmpfs_inode->vfs_inode.type = VFS_I_TYPE_DIR;
    int ret = hlist_init(&tmpfs_inode->dir.children,
                         TMPFS_HASH_BUCKETS,
                         &__tmpfs_dir_hlist_funcs);
    assert(ret == 0, "Failed to initialize tmpfs directory children hash list, errno=%d", ret);
}

// Lookup a child inode by name in a tmpfs directory inode
static struct tmpfs_inode *__tmpfs_dir_lookup_by_name(struct vfs_inode *inode, 
                                                      const char *name, 
                                                      size_t name_len) {
    struct tmpfs_inode tmp = {
        .name = (char *)name,
        .name_len = name_len
    };
    struct tmpfs_inode *tmpfs_inode = (struct tmpfs_inode *)inode;
    return hlist_get(&tmpfs_inode->dir.children, &tmp);
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
                  const char *name, size_t name_len) {
    struct tmpfs_inode *tmpfs_dir = (struct tmpfs_inode *)dir;
    if (name_len == 1 && strncmp(name, ".", 1) == 0) {
        dentry->ino = dir->ino;
        dentry->sb = dir->sb;
        dentry->name = kmm_alloc(2);
        memcpy(dentry->name, ".", 2);
        dentry->name_len = 1;
        dentry->cookies = (uint64)tmpfs_dir;
        return 0;
    }

    if (name_len == 2 && strncmp(name, "..", 2) == 0) {
        dentry->sb = dir->sb;
        dentry->name = kmm_alloc(3);
        memcpy(dentry->name, "..", 3);
        dentry->name_len = 2;
        if (tmpfs_dir->dir.parent == NULL) {
            dentry->ino = dir->ino;
            dentry->cookies = (uint64)tmpfs_dir;
        } else {
            dentry->ino = tmpfs_dir->dir.parent->vfs_inode.ino;
            dentry->cookies = (uint64)tmpfs_dir->dir.parent;
        }
        return 0;
    }
    
    struct tmpfs_inode *child = __tmpfs_dir_lookup_by_name(dir, name, name_len);
    if (child == NULL) {
        return -ENOENT; // Not found
    }
    dentry->ino = child->vfs_inode.ino;
    dentry->sb = dir->sb;
    dentry->name = kmm_alloc(name_len + 1);
    memcpy(dentry->name, name, name_len);
    dentry->name[name_len] = '\0';
    dentry->name_len = name_len;
    dentry->cookies = (uint64)child;
    return 0;
}

int __tmpfs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter);
int __tmpfs_readlink(struct vfs_inode *inode, char *buf, size_t buflen, bool user);
int __tmpfs_create(struct vfs_inode *dir, struct vfs_dentry *dentry, uint32 mode);
int __tmpfs_link(struct vfs_dentry *old, struct vfs_inode *dir, struct vfs_dentry *new);
int __tmpfs_unlink(struct vfs_inode *dir, struct vfs_dentry *dentry);
int __tmpfs_mkdir(struct vfs_inode *dir, struct vfs_dentry *dentry, uint32 mode);
int __tmpfs_rmdir(struct vfs_inode *dir, struct vfs_dentry *dentry);
int __tmpfs_mknod(struct vfs_inode *dir, struct vfs_dentry *dentry, uint32 mode, uint32 dev);
int __tmpfs_move(struct vfs_inode *old_dir, struct vfs_dentry *old_dentry,
            struct vfs_inode *new_dir, struct vfs_dentry *new_dentry);
int __tmpfs_symlink(struct vfs_inode *dir, struct vfs_dentry *dentry,
                const char *target, bool user);
int __tmpfs_truncate(struct vfs_inode *inode, uint64 new_size);
void __tmpfs_destroy_inode(struct vfs_inode *inode);
void __tmpfs_free_inode(struct vfs_inode *inode);
int __tmpfs_dirty_inode(struct vfs_inode *inode);

int __tmpfs_sync_inode(struct vfs_inode *inode) {
    // tmpfs is an in-memory filesystem, so syncing is a no-op
    return 0;
}