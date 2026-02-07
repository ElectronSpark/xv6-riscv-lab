#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H

#include "hlist_type.h"
#include "vfs/vfs_types.h"

#define VFS_DENTRY_COOKIE_END ((int64)0)
#define VFS_DENTRY_COOKIE_SELF ((int64)1)
#define VFS_DENTRY_COOKIE_PARENT ((int64)2)

#define TMPFS_HASH_BUCKETS 15

// Maximum file size supported by tmpfs (1 GB).
// All non-embedded file data is stored in the per-inode pcache (i_data),
// which allocates pages on demand.
#define TMPFS_MAX_FILE_SIZE ((uint64)1 * 1024 * 1024 * 1024)

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
            hlist_t children; // hash list of child inodes (for directories)
            hlist_bucket_t children_buckets[TMPFS_HASH_BUCKETS];
        } dir;
        union {
            // The target path is stored in the data[] field if the length
            // is shorter than TMPFS_INODE_EMBEDDED_DATA_LEN. Otherwise,
            // it is allocated separately and pointed to by symlink_target.
            char *symlink_target; // target path (for symlinks)
            char data[0];        // file data (for regular files)
        } sym;
        union {
            // Non-embedded file data is stored in the per-inode pcache
            // (vfs_inode.i_data). Only the embedded data[] overlay is used
            // directly from the tmpfs_inode when embedded == true.
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

#define TMPFS_INODE_EMBEDDED_DATA_LEN   \
    (sizeof(struct tmpfs_inode) - offsetof(struct tmpfs_inode, sym.data))

// Get the block index within the file for a given position
#define TMPFS_IBLOCK(pos) ((pos) >> PAGE_SHIFT)
// Get the offset within a block for a given position
#define TMPFS_IBLOCK_OFFSET(pos) ((pos) & PAGE_MASK)

struct vfs_inode *tmpfs_alloc_inode(struct vfs_superblock *sb);
// Set the symlink target for a tmpfs inode
// Will free any existing target if present
// WIll allocate memory if target_len >= TMPFS_INODE_EMBEDDED_DATA_LEN
// Copies the target string from `target` to the inode
void tmpfs_set_symlink_target(struct tmpfs_inode *tmpfs_inode,
                              const char *target, size_t target_len);
void tmpfs_free_inode(struct vfs_inode *inode);
// Free symlink target path string if allocated
// Do nothing if the target is embedded
// Will assume tmpfs_inode is symlink type and not NULL
void tmpfs_free_symlink_target(struct tmpfs_inode *tmpfs_inode);
void tmpfs_make_directory(struct tmpfs_inode *tmpfs_inode);
int __tmpfs_truncate(struct vfs_inode *inode, loff_t new_size);

// Migrate from embedded data to pcache-based storage
int __tmpfs_migrate_to_allocated_blocks(struct tmpfs_inode *tmpfs_inode);

// File operations
extern struct vfs_file_ops tmpfs_file_ops;
int tmpfs_open(struct vfs_inode *inode, struct vfs_file *file, int f_flags);

// pcache operations for tmpfs regular files
void tmpfs_inode_pcache_init(struct vfs_inode *inode);
void tmpfs_inode_pcache_teardown(struct vfs_inode *inode);

// Shrink all tmpfs slab caches to release unused memory
void tmpfs_shrink_caches(void);

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H
