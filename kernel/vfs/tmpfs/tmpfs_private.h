#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H

#include "hlist_type.h"
#include "vfs/vfs_types.h"

#define TMPFS_HASH_BUCKETS 15
#define TMPFS_INODE_DBLOCKS 32UL  // Number of direct blocks in a tmpfs inode
#define TMPFS_INODE_INDRECT_ITEMS (PAGE_SIZE / sizeof(uint64))
#define TMPFS_MAX_FILE_SIZE     \
    (((TMPFS_INODE_INDRECT_ITEMS * TMPFS_INODE_INDRECT_ITEMS * TMPFS_INODE_INDRECT_ITEMS)   \
    + (TMPFS_INODE_INDRECT_ITEMS * TMPFS_INODE_INDRECT_ITEMS)                               \
    + TMPFS_INODE_INDRECT_ITEMS + TMPFS_INODE_DBLOCKS) * PAGE_SIZE)

#define TMPFS_DENTRY_COOKIE_SELF 1
#define TMPFS_DENTRY_COOKIE_PARENT 2

extern struct vfs_inode_ops tmpfs_inode_ops;

struct tmpfs_sb_private {
    // tmpfs specific superblock data can be added here
    uint64 next_ino; // next inode number to allocate
};

struct tmpfs_superblock {
    struct vfs_superblock vfs_sb;
    struct tmpfs_sb_private private_data;
};

struct tmpfs_inode {
    struct vfs_inode vfs_inode;
    // tmpfs specific inode data can be added here
    bool embedded;
    union {
        struct {
            struct tmpfs_inode *parent;
            hlist_t children; // hash list of child inodes (for directories)
            hlist_bucket_t children_buckets[TMPFS_HASH_BUCKETS];
        } dir;
        union {
            // The target path is stored in the data[] field if the length
            // is shorter than TMPFS_SYMLINK_EMBEDDED_TARGET_LEN. Otherwise,
            // it is allocated separately and pointed to by symlink_target.
            char *symlink_target; // target path (for symlinks)
            char data[0];        // file data (for regular files)
        } sym;
        union {
            // data blocks or other file-specific data (for regular files)
            struct {
                void *direct[TMPFS_INODE_DBLOCKS];
                void **indirect;
                void ***double_indirect;
                void ****triple_indirect;
            };
            uint8 data[0];
        } file;
    };
};

struct tmpfs_dentry {
    hlist_entry_t hash_entry; // entry in tmpfs_inode.dir.children
    struct tmpfs_inode *parent;
    struct vfs_superblock *sb;
    struct tmpfs_inode *inode;
    size_t name_len;
    char *name;
    char __name_start[0];
};

#define TMPFS_SYMLINK_EMBEDDED_TARGET_LEN   \
    (sizeof(struct tmpfs_inode) - offsetof(struct tmpfs_inode, sym.data))

int tmpfs_alloc_inode(struct vfs_superblock *sb, struct vfs_inode **ret_inode);
// Set the symlink target for a tmpfs inode
// Will free any existing target if present
// WIll allocate memory if target_len >= TMPFS_SYMLINK_EMBEDDED_TARGET_LEN
// Copies the target string from `target` to the inode
void tmpfs_set_symlink_target(struct tmpfs_inode *tmpfs_inode,
                              const char *target, size_t target_len);
void tmpfs_free_inode(struct vfs_inode *inode);
// Free symlink target path string if allocated
// Do nothing if the target is embedded
// Will assume tmpfs_inode is symlink type and not NULL
void tmpfs_free_symlink_target(struct tmpfs_inode *tmpfs_inode);
void tmpfs_make_directory(struct tmpfs_inode *tmpfs_inode, struct tmpfs_inode *parent);


#endif // KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H
