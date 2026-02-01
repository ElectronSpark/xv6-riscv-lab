# VFS Lazy Unmount Design with Orphan Inode Support

This document describes the complete design for filesystem unmounting in the xv6 VFS layer, including orphan inode handling for both in-memory (tmpfs) and on-disk filesystems.

## Table of Contents

1. [Overview](#overview)
2. [New State Flags](#new-state-flags)
3. [Orphan Inode Support](#orphan-inode-support)
4. [Lazy Unmount Flow](#lazy-unmount-flow)
5. [Modified vfs_iput](#modified-vfs_iput)
6. [On-Disk Orphan Handling](#on-disk-orphan-handling)
7. [API Reference](#api-reference)
8. [Thread Safety](#thread-safety)
9. [State Invariants](#state-invariants)

---

## Overview

The unmount subsystem supports two modes:

1. **Strict Unmount**: Fails with `-EBUSY` if any references exist
2. **Lazy Unmount**: Detaches filesystem immediately, allows orphan inodes to be cleaned up when their last reference drops

Key design goals:
- No resource leaks (memory or disk blocks)
- Crash recovery for on-disk filesystems
- Graceful handling of open files during unmount
- Thread-safe operations with proper locking order

---

## New State Flags

### Superblock State Flags

```c
struct vfs_superblock {
    // Core flags
    uint64 valid: 1;        // Superblock is usable
    uint64 initialized: 1;  // Setup complete
    uint64 dirty: 1;        // Has unsaved changes
    uint64 backendless: 1;  // In-memory only (e.g., tmpfs)
    
    // Unmount/sync state flags
    uint64 syncing: 1;      // Currently syncing to backend
    uint64 unmounting: 1;   // Unmount initiated, blocking new operations
    uint64 attached: 1;     // Attached to mount tree (0 = detached/lazy unmounted)
    
    // Orphan tracking
    int orphan_count;       // Number of orphan inodes (n_links=0, ref>0)
    list_node_t orphan_list; // List of orphan inodes
};
```

### Inode Orphan State

```c
struct vfs_inode {
    // Orphan tracking
    uint64 orphan: 1;          // On orphan list (n_links=0, ref>0)
    uint64 destroying: 1;      // destroy_inode in progress
    uint64 delay_put: 1;       // Delay freeing inode (used by some filesystems)
    list_entry_t orphan_entry; // Entry in sb->orphan_list
};
```

### State Transition Diagram

```
                    ┌─────────────────────────────────────────────────────────┐
                    │                   SUPERBLOCK LIFECYCLE                   │
                    └─────────────────────────────────────────────────────────┘

    ┌──────────┐      mount()       ┌──────────┐
    │  ALLOC   │ ─────────────────► │  VALID   │◄────────────────────┐
    │          │                    │          │                      │
    └──────────┘                    └────┬─────┘                      │
                                         │                            │
                                         │ umount()                   │ sync complete
                                         ▼                            │
                               ┌──────────────────┐                   │
                               │   UNMOUNTING     │                   │
                               │  (blocking new   │                   │
                               │   operations)    │                   │
                               └────────┬─────────┘                   │
                                        │                             │
                          ┌─────────────┴──────────────┐              │
                          │                            │              │
                          ▼                            ▼              │
                  ┌───────────────┐          ┌─────────────────┐      │
                  │ STRICT UMOUNT │          │  LAZY UMOUNT    │      │
                  │  (if no refs) │          │ (always works)  │      │
                  └───────┬───────┘          └────────┬────────┘      │
                          │                           │               │
                          │                           ▼               │
                          │                  ┌─────────────────┐      │
                          │                  │   NOT ATTACHED  │──────┘
                          │                  │ (from mount     │  (if syncing
                          │                  │  tree, orphans  │   needed)
                          │                  │  may exist)     │
                          │                  └────────┬────────┘
                          │                           │
                          │                           │ all refs dropped
                          │                           ▼
                          │                  ┌─────────────────┐
                          └─────────────────►│     FREED       │
                                             │                 │
                                             └─────────────────┘
```

---

## Orphan Inode Support

### What is an Orphan Inode?

An **orphan inode** is an inode that:
- Has `n_links == 0` (no directory entries point to it)
- Has `ref_count > 0` (still held by open file descriptors or other references)

Orphan inodes are created when:
1. A file is unlinked while still open
2. A directory is removed while a process has it as cwd
3. Lazy unmount detaches inodes that are still referenced

### Orphan Lifecycle

```
                    Normal Inode
                    (n_links > 0, ref >= 0)
                           │
         ┌─────────────────┼─────────────────┐
         │                 │                 │
         ▼                 ▼                 ▼
    unlink() with     rmdir() with     lazy_unmount()
    open handles      open handles     detaches all
         │                 │                 │
         └─────────────────┼─────────────────┘
                           │
                           ▼
              ┌─────────────────────────┐
              │      ORPHAN INODE       │
              │  • n_links = 0          │
              │  • ref_count > 0        │
              │  • orphan = 1           │
              │  • On sb->orphan_list   │
              │  • For disk fs: on-disk │
              │    orphan journal       │
              └───────────┬─────────────┘
                          │
                          │ last vfs_iput() / vfs_fileclose()
                          ▼
              ┌─────────────────────────┐
              │   ORPHAN CLEANUP        │
              │  • destroy_inode()      │
              │  • Free all blocks      │
              │  • Remove from orphan   │
              │    list (mem + disk)    │
              │  • free_inode()         │
              └───────────┬─────────────┘
                          │
                          ▼
              ┌─────────────────────────┐
              │        FREED            │
              └─────────────────────────┘
```

### Creating Orphans

When `vfs_unlink()` or `vfs_rmdir()` decrements `n_links` to 0 but `ref_count > 0`:

```c
// In vfs_unlink, after dir->ops->unlink() succeeds:
if (target->n_links == 0 && target->ref_count > 0) {
    vfs_make_orphan(target);
}
```

### Orphan Cleanup

Cleanup happens automatically in `vfs_iput()` when the last reference drops.

---

## Mountpoint Reference Counting

When a filesystem is mounted on a directory inode, the VFS holds an extra reference on that mountpoint inode to prevent it from being freed while a filesystem is mounted on it.

### Mount Reference Lifecycle

```
Mount:
  __vfs_turn_mountpoint(mountpoint)
    → vfs_idup(mountpoint)  // Add reference (skip for vfs_root_inode)
    → mountpoint->mount = 1

Unmount:
  __vfs_clear_mountpoint(mountpoint)  // Under locks
  ... release locks ...
  vfs_iput(mountpoint)  // Release reference (skip for vfs_root_inode)
```

### Special Case: vfs_root_inode

The global `vfs_root_inode` is a special static inode that serves as the root of the entire VFS mount tree. It has no superblock (`sb == NULL`) and does not participate in reference counting. When checking whether to call `vfs_idup`/`vfs_iput` on a mountpoint, the code skips these calls for `vfs_root_inode`.

---

## Lazy Unmount Flow

### Phase 1: Validation

```
Locks: mount_mutex, parent_sb wlock, mountpoint inode lock

• Check mount_count == 0 (no child mounts)
• If child mounts exist: return -EBUSY
• Set sb->unmounting = 1 to block new operations
```

### Phase 2: Detach from Mount Tree

```
• Clear mountpoint->mnt_sb and mountpoint->mnt_rooti
• Clear mountpoint->mount flag
• Decrement parent sb mount_count
• Set child sb->mountpoint = NULL
• Set child sb->parent_sb = NULL
• Set sb->attached = 0
• Release mountpoint inode lock
• Release parent_sb wlock
• Release mount_mutex
• vfs_iput(mountpoint)  // Release mount reference (skip for vfs_root_inode)
```

### Phase 3: Sync and Orphan Processing

```
Locks: child_sb wlock

For backend filesystems:
  • Set sb->syncing = 1
  • Call sync_fs(sb, 1) to flush all data
  • Set sb->syncing = 0
  • If sync fails: log error, continue (data loss accepted)

For all inodes in sb->inodes hash:
  • If inode->ref_count > 0 && inode != root_inode:
    - Set inode->orphan = 1
    - Add to sb->orphan_list
    - Increment sb->orphan_count
    - For backend fs: call add_orphan(sb, inode) to persist
  
  • Else if inode->ref_count == 0:
    - Call destroy_inode() if needed
    - Remove from hash table
    - Call free_inode()
```

### Phase 4: Check for Immediate Cleanup

```
• If orphan_count == 0:
  - Detach superblock from fs_type
  - Release child_sb wlock
  - Call fs_type->ops->free(sb)
  - Return 0 (unmount complete)

• Else (orphans exist):
  - Set sb->valid = 0 (prevent new lookups)
  - Release child_sb wlock
  - Return 0 (cleanup deferred to vfs_iput)
```

---

## Modified vfs_iput

The `vfs_iput()` function is modified to handle orphan cleanup and deferred superblock freeing:

```c
void vfs_iput(struct vfs_inode *inode) {
    struct vfs_superblock *sb = inode->sb;
    
retry:
    // Fast path: refcount > 1, just decrement
    if (atomic_dec_unless(&inode->ref_count, 1)) {
        return;
    }
    
    if (sb == NULL) {
        goto out_free;
    }
    
    vfs_superblock_wlock(sb);
    vfs_ilock(inode);
    
    // Retry check after acquiring locks
    if (atomic_dec_unless(&inode->ref_count, 1)) {
        vfs_iunlock(inode);
        vfs_superblock_unlock(sb);
        return;
    }
    
    // BACKENDLESS HANDLING:
    // Keep alive if: backendless AND attached AND (has links OR is mountpoint)
    if (sb->backendless && sb->attached && (inode->n_links > 0 || inode->mount)) {
        atomic_dec(&inode->ref_count);
        assert(inode->ref_count >= 0, "vfs_iput: refcount underflow");
        vfs_iunlock(inode);
        vfs_superblock_unlock(sb);
        return;
    }
    
    // ORPHAN CLEANUP:
    if (inode->orphan) {
        list_node_detach(inode, orphan_entry);
        sb->orphan_count--;
        
        // For backend fs: remove from on-disk orphan journal
        if (sb->ops->remove_orphan) {
            sb->ops->remove_orphan(sb, inode);
        }
    }
    
    // DESTROY INODE DATA:
    if (inode->n_links == 0 && inode->ops->destroy_inode) {
        inode->ops->destroy_inode(inode);
        inode->valid = 0;
        inode->dirty = 0;
    }
    
    // REMOVE FROM CACHE:
    vfs_remove_inode(sb, inode);
    vfs_iunlock(inode);
    
    // CHECK FOR DEFERRED SUPERBLOCK CLEANUP:
    bool should_free_sb = (!sb->attached && sb->orphan_count == 0);
    
    vfs_superblock_unlock(sb);
    
out_free:
    inode->ops->free_inode(inode);
    
    // FINAL SUPERBLOCK CLEANUP:
    if (should_free_sb) {
        __vfs_final_unmount_cleanup(sb);
    }
}
```

---

## On-Disk Orphan Handling

For filesystems with persistent storage, orphan inodes must be tracked on disk to enable crash recovery.

### On-Disk Orphan Entry Structure

```c
struct orphan_entry {
    uint64 ino;           // Inode number
    uint64 size;          // File size at orphan time
    uint32 block_count;   // Number of blocks to free
    uint32 flags;         // Type info (file, dir, symlink)
};
```

### Superblock Ops for Orphan Management

```c
struct vfs_superblock_ops {
    // Existing operations...
    
    // NEW: Orphan management
    int (*add_orphan)(struct vfs_superblock *sb, struct vfs_inode *inode);
    int (*remove_orphan)(struct vfs_superblock *sb, struct vfs_inode *inode);
    int (*recover_orphans)(struct vfs_superblock *sb);
};
```

### Crash Recovery Flow

At mount time, before the filesystem becomes usable:

```
mount()
   │
   ▼
┌─────────────────────────────┐
│ Initialize superblock       │
└─────────────┬───────────────┘
              │
              ▼
┌─────────────────────────────┐
│ Call recover_orphans(sb)    │
│                             │
│ For each orphan entry:      │
│  • Load inode from disk     │
│  • Truncate to 0 (free      │
│    all data blocks)         │
│  • Free the inode itself    │
│  • Remove from orphan list  │
└─────────────┬───────────────┘
              │
              ▼
┌─────────────────────────────┐
│ Mark superblock as valid    │
│ Continue normal mount       │
└─────────────────────────────┘
```

### Orphan Journal Location Options

1. **Reserved inode**: Use a special inode (e.g., inode 11) as the orphan list head
2. **Superblock field**: Store head pointer in superblock
3. **Linked list in inodes**: Chain orphan inodes via a reserved field

---

## API Reference

### Unmount Functions

```c
// Strict unmount: fails if filesystem is busy
// Returns: 0 on success, -EBUSY if references exist, other negative errno on error
int vfs_unmount(struct vfs_inode *mountpoint);

// Lazy unmount: detaches immediately, defers cleanup
// Returns: 0 on success, -EBUSY if child mounts exist, other negative errno on error
int vfs_unmount_lazy(struct vfs_inode *mountpoint);
```

### Orphan Management (Internal)

```c
// Mark an inode as orphan (called by VFS during unlink/rmdir)
// Caller must hold: sb wlock, inode lock
// Returns: 0 on success, negative errno on error
int vfs_make_orphan(struct vfs_inode *inode);
```

### State Check Helpers

```c
// Check if superblock is usable for new operations
// Returns: 0 if usable, -EINVAL/-ESHUTDOWN/-ENOENT otherwise
static inline int vfs_sb_check_usable(struct vfs_superblock *sb) {
    if (sb == NULL) return -EINVAL;
    if (!sb->valid) return -EINVAL;
    if (sb->unmounting) return -ESHUTDOWN;
    if (!sb->attached) return -ENOENT;
    return 0;
}

static inline bool vfs_sb_is_attached(struct vfs_superblock *sb) {
    return sb->attached;
}

static inline bool vfs_sb_is_syncing(struct vfs_superblock *sb) {
    return sb->syncing;
}

static inline bool vfs_sb_is_unmounting(struct vfs_superblock *sb) {
    return sb->unmounting;
}
```

---

## Thread Safety

### Locking Order

Always acquire locks in this order to prevent deadlock:

1. `mount_mutex` (global)
2. Parent superblock `wlock`
3. Child superblock `wlock`
4. Parent directory inode `mutex`
5. Child inode `mutex`

### Lock Requirements by Operation

| Operation | Locks Required |
|-----------|----------------|
| New inode lookup | sb rlock + inode lock |
| Create file/dir | sb wlock + parent dir lock |
| Unlink creating orphan | sb wlock + dir lock + target lock |
| vfs_iput (normal) | sb wlock + inode lock (if cleanup needed) |
| vfs_iput (orphan cleanup) | sb wlock + inode lock |
| vfs_unmount_lazy | mount_mutex + parent sb wlock + child sb wlock |
| Final sb cleanup | mount_mutex + sb wlock |

### Atomic Operations

The following fields are accessed atomically:

- `inode->ref_count` - use `atomic_dec_unless()`, `atomic_dec()`, `atomic_inc()`
- `sb->orphan_count` - protected by sb wlock

---

## State Invariants

### Valid State Combinations

| valid | unmounting | attached | syncing | Meaning |
|-------|------------|----------|---------|---------|
| 1 | 0 | 1 | 0 | Normal operation |
| 1 | 0 | 1 | 1 | Sync in progress (normal) |
| 1 | 1 | 1 | 0 | Unmount starting |
| 1 | 1 | 1 | 1 | Unmount sync phase |
| 0 | 1 | 0 | 0 | Lazy unmount complete, waiting for orphans |
| 0 | 1 | 0 | 1 | Invalid (should not occur) |

### Inode Invariants

- `inode->orphan == 1` implies `inode->n_links == 0`
- `inode->orphan == 1` implies inode is on `sb->orphan_list`
- `sb->orphan_count` equals length of `sb->orphan_list`
- Mountpoint inode: `inode->mount == 1` implies `inode->ref_count > 0`

### Superblock Invariants

- `sb->attached == 0` implies `sb->mountpoint == NULL`
- `sb->attached == 0` implies `sb->parent_sb == NULL`
- If `sb->orphan_count == 0` and `sb->attached == 0`, superblock will be freed

---

## Error Handling

### Sync Failures During Unmount

If `sync_fs()` fails during unmount:
1. Log warning message
2. Continue with unmount (data loss accepted)
3. Set error flag on superblock (optional, for user-space to query)

Rationale: Once unmount is requested, it must succeed. Users can run `sync` before unmount if data integrity is critical.

### Orphan Journal Failures

If `add_orphan()` fails during unlink:
1. Log warning message
2. Continue with unlink
3. Risk: blocks may leak on crash (acceptable trade-off)

If `remove_orphan()` fails during iput:
1. Log warning message
2. Continue with cleanup
3. Next mount will re-process and clean the orphan

---

## Implementation Checklist

- [x] Add `syncing`, `unmounting`, `attached` flags to `vfs_superblock`
- [x] Add `orphan_count`, `orphan_list` to `vfs_superblock`
- [x] Add `orphan`, `orphan_entry` to `vfs_inode`
- [x] Implement `vfs_make_orphan()`
- [x] Modify `vfs_iput()` for orphan/attached handling
- [x] Implement `vfs_unmount_lazy()`
- [x] Implement `__vfs_final_unmount_cleanup()`
- [x] Add `add_orphan`, `remove_orphan`, `recover_orphans` to `vfs_superblock_ops`
- [x] Modify `vfs_unlink()` and `vfs_rmdir()` to create orphans
- [x] Add usability checks (`vfs_sb_check_usable()`) to inode operations
- [x] Implement orphan stubs in xv6fs (persistent journal TODO)
- [x] Add unmount smoketest (tmpfs strict/lazy unmount passes all tests)

---

## References

- Linux VFS: `Documentation/filesystems/vfs.rst`
- Linux orphan handling: `fs/ext4/orphan.c`
- POSIX unlink semantics: IEEE Std 1003.1
