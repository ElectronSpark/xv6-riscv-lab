/*
 * xv6fs inode operations
 * 
 * Handles inode operations including lookup, create, directory iteration,
 * and inode synchronization for the xv6 filesystem.
 *
 * Locking order (must acquire in this order to avoid deadlock):
 * 1. vfs_superblock rwlock (held by VFS layer for create/mkdir/unlink/etc)
 * 2. vfs_inode mutex (held by VFS layer before calling inode ops)
 * 3. log->lock spinlock (acquired by xv6fs_begin_op/end_op)
 * 4. buffer mutex (acquired by bread/brelse)
 *
 * CRITICAL: Functions like xv6fs_destroy_inode are called from vfs_iput
 * while holding superblock wlock + inode lock. These functions call
 * xv6fs_begin_op which can sleep waiting for log space. This creates
 * a potential priority inversion with file I/O operations.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "vm.h"
#include "mutex_types.h"
#include "buf.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "slab.h"
#include "xv6fs_private.h"

// Bitmap block calculation for pointer-based superblock
#define BBLOCK_PTR(b, sbp) ((b)/BPB + (sbp)->bmapstart)

/******************************************************************************
 * Block mapping
 ******************************************************************************/

// Get the disk block address for the bn-th block of inode
// If alloc is true, allocate the block if it doesn't exist
static uint __xv6fs_bmap_ind(struct xv6fs_superblock *xv6_sb, uint *entry, uint dev, uint bn) {
    uint addr;
    struct buf *bp;
    
    if (*entry == 0) {
        // Allocate indirect block
        // Find a free block
        int b, bi;
        struct buf *alloc_bp;
        addr = 0;
        
        struct superblock *disk_sb = &xv6_sb->disk_sb;
        for (b = 0; b < disk_sb->size; b += BPB) {
            alloc_bp = bread(dev, BBLOCK_PTR(b, disk_sb));
            if (alloc_bp == NULL) return 0;
            
            for (bi = 0; bi < BPB && b + bi < disk_sb->size; bi++) {
                int m = 1 << (bi % 8);
                if ((alloc_bp->data[bi/8] & m) == 0) {
                    // Found free block
                    alloc_bp->data[bi/8] |= m;
                    xv6fs_log_write(xv6_sb, alloc_bp);
                    brelse(alloc_bp);
                    addr = b + bi;
                    
                    // Zero the block
                    struct buf *zbp = bread(dev, addr);
                    memset(zbp->data, 0, BSIZE);
                    xv6fs_log_write(xv6_sb, zbp);
                    brelse(zbp);
                    
                    *entry = addr;
                    goto found_indirect;
                }
            }
            brelse(alloc_bp);
        }
        return 0; // No free blocks
    }
    
found_indirect:
    bp = bread(dev, *entry);
    if (bp == NULL) return 0;
    
    uint *a = (uint*)bp->data;
    addr = a[bn];
    
    if (addr == 0) {
        // Allocate data block
        int b, bi;
        struct buf *alloc_bp;
        
        struct superblock *disk_sb = &xv6_sb->disk_sb;
        for (b = 0; b < disk_sb->size; b += BPB) {
            alloc_bp = bread(dev, BBLOCK_PTR(b, disk_sb));
            if (alloc_bp == NULL) {
                brelse(bp);
                return 0;
            }
            
            for (bi = 0; bi < BPB && b + bi < disk_sb->size; bi++) {
                int m = 1 << (bi % 8);
                if ((alloc_bp->data[bi/8] & m) == 0) {
                    alloc_bp->data[bi/8] |= m;
                    xv6fs_log_write(xv6_sb, alloc_bp);
                    brelse(alloc_bp);
                    addr = b + bi;
                    
                    // Zero the block
                    struct buf *zbp = bread(dev, addr);
                    memset(zbp->data, 0, BSIZE);
                    xv6fs_log_write(xv6_sb, zbp);
                    brelse(zbp);
                    
                    a[bn] = addr;
                    xv6fs_log_write(xv6_sb, bp);
                    brelse(bp);
                    return addr;
                }
            }
            brelse(alloc_bp);
        }
        brelse(bp);
        return 0;
    }
    
    brelse(bp);
    return addr;
}

// Read-only bmap - returns 0 if block doesn't exist (for sparse files)
// Does not allocate any blocks
uint xv6fs_bmap_read(struct xv6fs_inode *ip, uint bn) {
    uint dev = ip->dev;
    uint addr;
    struct buf *bp;
    
    // Direct blocks
    if (bn < XV6FS_NDIRECT) {
        return ip->addrs[bn];  // May be 0 for sparse files
    }
    bn -= XV6FS_NDIRECT;
    
    // Single indirect block
    if (bn < XV6FS_NINDIRECT) {
        if (ip->addrs[XV6FS_NDIRECT] == 0) {
            return 0;
        }
        bp = bread(dev, ip->addrs[XV6FS_NDIRECT]);
        if (bp == NULL) return 0;
        addr = ((uint*)bp->data)[bn];
        brelse(bp);
        return addr;
    }
    bn -= XV6FS_NINDIRECT;
    
    // Double indirect block
    if (bn < XV6FS_NDINDIRECT) {
        if (ip->addrs[XV6FS_NDIRECT + 1] == 0) {
            return 0;
        }
        bp = bread(dev, ip->addrs[XV6FS_NDIRECT + 1]);
        if (bp == NULL) return 0;
        
        uint l1_idx = bn / XV6FS_NINDIRECT;
        uint l2_idx = bn % XV6FS_NINDIRECT;
        uint *a = (uint*)bp->data;
        
        if (a[l1_idx] == 0) {
            brelse(bp);
            return 0;
        }
        uint l1_addr = a[l1_idx];
        brelse(bp);
        
        bp = bread(dev, l1_addr);
        if (bp == NULL) return 0;
        addr = ((uint*)bp->data)[l2_idx];
        brelse(bp);
        return addr;
    }
    
    return 0;  // Block number out of range
}

uint xv6fs_bmap(struct xv6fs_inode *ip, uint bn) {
    struct xv6fs_superblock *xv6_sb = container_of(ip->vfs_inode.sb, 
                                                    struct xv6fs_superblock, vfs_sb);
    uint dev = ip->dev;
    uint addr;
    struct buf *bp;
    
    // Direct blocks
    if (bn < XV6FS_NDIRECT) {
        if ((addr = ip->addrs[bn]) == 0) {
            // Allocate new block
            int b, bi;
            struct buf *alloc_bp;
            
            struct superblock *disk_sb = &xv6_sb->disk_sb;
        for (b = 0; b < disk_sb->size; b += BPB) {
                alloc_bp = bread(dev, BBLOCK_PTR(b, disk_sb));
                if (alloc_bp == NULL) return 0;
                
                for (bi = 0; bi < BPB && b + bi < disk_sb->size; bi++) {
                    int m = 1 << (bi % 8);
                    if ((alloc_bp->data[bi/8] & m) == 0) {
                        alloc_bp->data[bi/8] |= m;
                        xv6fs_log_write(xv6_sb, alloc_bp);
                        brelse(alloc_bp);
                        addr = b + bi;
                        
                        // Zero the block
                        struct buf *zbp = bread(dev, addr);
                        memset(zbp->data, 0, BSIZE);
                        xv6fs_log_write(xv6_sb, zbp);
                        brelse(zbp);
                        
                        ip->addrs[bn] = addr;
                        return addr;
                    }
                }
                brelse(alloc_bp);
            }
            return 0;
        }
        return addr;
    }
    bn -= XV6FS_NDIRECT;
    
    // Single indirect block
    if (bn < XV6FS_NINDIRECT) {
        return __xv6fs_bmap_ind(xv6_sb, &ip->addrs[XV6FS_NDIRECT], dev, bn);
    }
    bn -= XV6FS_NINDIRECT;
    
    // Double indirect block
    if (bn < XV6FS_NDINDIRECT) {
        if (ip->addrs[XV6FS_NDIRECT + 1] == 0) {
            // Allocate double indirect block
            int b, bi;
            struct buf *alloc_bp;
            
            struct superblock *disk_sb = &xv6_sb->disk_sb;
        for (b = 0; b < disk_sb->size; b += BPB) {
                alloc_bp = bread(dev, BBLOCK_PTR(b, disk_sb));
                if (alloc_bp == NULL) return 0;
                
                for (bi = 0; bi < BPB && b + bi < disk_sb->size; bi++) {
                    int m = 1 << (bi % 8);
                    if ((alloc_bp->data[bi/8] & m) == 0) {
                        alloc_bp->data[bi/8] |= m;
                        xv6fs_log_write(xv6_sb, alloc_bp);
                        brelse(alloc_bp);
                        addr = b + bi;
                        
                        struct buf *zbp = bread(dev, addr);
                        memset(zbp->data, 0, BSIZE);
                        xv6fs_log_write(xv6_sb, zbp);
                        brelse(zbp);
                        
                        ip->addrs[XV6FS_NDIRECT + 1] = addr;
                        goto have_dindirect;
                    }
                }
                brelse(alloc_bp);
            }
            return 0;
        }
        
have_dindirect:
        bp = bread(dev, ip->addrs[XV6FS_NDIRECT + 1]);
        if (bp == NULL) return 0;
        
        uint l1_idx = bn / XV6FS_NINDIRECT;
        uint l2_idx = bn % XV6FS_NINDIRECT;
        uint *a = (uint*)bp->data;
        
        addr = __xv6fs_bmap_ind(xv6_sb, &a[l1_idx], dev, l2_idx);
        if (a[l1_idx] != 0) {
            xv6fs_log_write(xv6_sb, bp);
        }
        brelse(bp);
        return addr;
    }
    
    panic("xv6fs_bmap: block number too large");
    return 0;
}

/******************************************************************************
 * Inode update/sync
 ******************************************************************************/

void xv6fs_iupdate(struct xv6fs_inode *ip) {
    struct xv6fs_superblock *xv6_sb = container_of(ip->vfs_inode.sb,
                                                    struct xv6fs_superblock, vfs_sb);
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    
    struct buf *bp = bread(ip->dev, XV6FS_IBLOCK(ip->vfs_inode.ino, disk_sb));
    struct dinode *dip = (struct dinode*)bp->data + ip->vfs_inode.ino % IPB;
    
    dip->type = xv6fs_mode_to_type(ip->vfs_inode.mode);
    dip->major = ip->major;
    dip->minor = ip->minor;
    dip->nlink = ip->vfs_inode.n_links;
    dip->size = ip->vfs_inode.size;
    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
    
    xv6fs_log_write(xv6_sb, bp);
    brelse(bp);
}

int xv6fs_sync_inode(struct vfs_inode *inode) {
    if (inode == NULL) return -EINVAL;
    
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    
    xv6fs_begin_op(xv6_sb);
    xv6fs_iupdate(ip);
    xv6fs_end_op(xv6_sb);
    
    inode->dirty = 0;
    return 0;
}

int xv6fs_dirty_inode(struct vfs_inode *inode) {
    if (inode == NULL) return -EINVAL;
    inode->dirty = 1;
    return 0;
}

/******************************************************************************
 * Truncate
 ******************************************************************************/

// Free a block
static void __xv6fs_bfree(struct xv6fs_superblock *xv6_sb, uint dev, uint b) {
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    struct buf *bp = bread(dev, BBLOCK_PTR(b, disk_sb));
    int bi = b % BPB;
    int m = 1 << (bi % 8);
    if ((bp->data[bi/8] & m) == 0)
        panic("xv6fs_bfree: freeing free block");
    bp->data[bi/8] &= ~m;
    xv6fs_log_write(xv6_sb, bp);
    brelse(bp);
}

// Free indirect blocks starting from index 'start_idx'
// If start_idx == 0, frees the indirect block itself too
// Returns number of blocks freed
static int __xv6fs_itrunc_ind_partial(struct xv6fs_superblock *xv6_sb, 
                                       uint *entry, uint dev, uint start_idx) {
    if (*entry == 0) return 0;
    
    struct buf *bp = bread(dev, *entry);
    uint *a = (uint*)bp->data;
    int freed = 0;
    
    for (uint j = start_idx; j < XV6FS_NINDIRECT; j++) {
        if (a[j]) {
            __xv6fs_bfree(xv6_sb, dev, a[j]);
            a[j] = 0;
            freed++;
        }
    }
    
    if (freed > 0) {
        xv6fs_log_write(xv6_sb, bp);
    }
    brelse(bp);
    
    // If we freed from the beginning, free the indirect block itself
    if (start_idx == 0) {
        __xv6fs_bfree(xv6_sb, dev, *entry);
        *entry = 0;
        freed++;
    }
    
    return freed;
}

// Free indirect blocks (original full version for backwards compat)
static void __xv6fs_itrunc_ind(struct xv6fs_superblock *xv6_sb, uint *entry, uint dev) {
    __xv6fs_itrunc_ind_partial(xv6_sb, entry, dev, 0);
}

// Maximum blocks to free per transaction to stay within log limits
// Each bfree modifies the bitmap (1 write) so we can free ~MAXOPBLOCKS/2 blocks per tx
#define ITRUNC_BATCH_SIZE ((MAXOPBLOCKS - 5) / 2)

// Truncate inode in batches to handle large files
// IMPORTANT: Caller must have called xv6fs_begin_op before calling this.
// This function may call xv6fs_end_op and xv6fs_begin_op internally to commit batches.
// When this function returns, a transaction is still active (caller should call xv6fs_end_op).
void xv6fs_itrunc(struct xv6fs_inode *ip) {
    struct xv6fs_superblock *xv6_sb = container_of(ip->vfs_inode.sb,
                                                    struct xv6fs_superblock, vfs_sb);
    uint dev = ip->dev;
    int freed_this_batch = 0;
    
    // Free direct blocks
    for (int i = 0; i < XV6FS_NDIRECT; i++) {
        if (ip->addrs[i]) {
            __xv6fs_bfree(xv6_sb, dev, ip->addrs[i]);
            ip->addrs[i] = 0;
            freed_this_batch++;
            
            if (freed_this_batch >= ITRUNC_BATCH_SIZE) {
                // Commit current batch and start new transaction
                xv6fs_iupdate(ip);
                xv6fs_end_op(xv6_sb);
                xv6fs_begin_op(xv6_sb);
                freed_this_batch = 0;
            }
        }
    }
    
    // Free indirect blocks
    if (ip->addrs[XV6FS_NDIRECT]) {
        struct buf *bp = bread(dev, ip->addrs[XV6FS_NDIRECT]);
        uint *a = (uint*)bp->data;
        
        for (uint j = 0; j < XV6FS_NINDIRECT; j++) {
            if (a[j]) {
                __xv6fs_bfree(xv6_sb, dev, a[j]);
                a[j] = 0;
                freed_this_batch++;
                
                if (freed_this_batch >= ITRUNC_BATCH_SIZE) {
                    xv6fs_log_write(xv6_sb, bp);
                    brelse(bp);
                    xv6fs_iupdate(ip);
                    xv6fs_end_op(xv6_sb);
                    xv6fs_begin_op(xv6_sb);
                    freed_this_batch = 0;
                    bp = bread(dev, ip->addrs[XV6FS_NDIRECT]);
                    a = (uint*)bp->data;
                }
            }
        }
        xv6fs_log_write(xv6_sb, bp);
        brelse(bp);
        __xv6fs_bfree(xv6_sb, dev, ip->addrs[XV6FS_NDIRECT]);
        ip->addrs[XV6FS_NDIRECT] = 0;
        freed_this_batch++;
        
        if (freed_this_batch >= ITRUNC_BATCH_SIZE) {
            xv6fs_iupdate(ip);
            xv6fs_end_op(xv6_sb);
            xv6fs_begin_op(xv6_sb);
            freed_this_batch = 0;
        }
    }
    
    // Free double indirect blocks
    if (ip->addrs[XV6FS_NDIRECT + 1]) {
        struct buf *dbp = bread(dev, ip->addrs[XV6FS_NDIRECT + 1]);
        uint *da = (uint*)dbp->data;
        
        for (int j = 0; j < XV6FS_NINDIRECT; j++) {
            if (da[j]) {
                struct buf *bp = bread(dev, da[j]);
                uint *a = (uint*)bp->data;
                
                for (uint k = 0; k < XV6FS_NINDIRECT; k++) {
                    if (a[k]) {
                        __xv6fs_bfree(xv6_sb, dev, a[k]);
                        a[k] = 0;
                        freed_this_batch++;
                        
                        if (freed_this_batch >= ITRUNC_BATCH_SIZE) {
                            xv6fs_log_write(xv6_sb, bp);
                            brelse(bp);
                            xv6fs_log_write(xv6_sb, dbp);
                            brelse(dbp);
                            xv6fs_iupdate(ip);
                            xv6fs_end_op(xv6_sb);
                            xv6fs_begin_op(xv6_sb);
                            freed_this_batch = 0;
                            dbp = bread(dev, ip->addrs[XV6FS_NDIRECT + 1]);
                            da = (uint*)dbp->data;
                            bp = bread(dev, da[j]);
                            a = (uint*)bp->data;
                        }
                    }
                }
                xv6fs_log_write(xv6_sb, bp);
                brelse(bp);
                __xv6fs_bfree(xv6_sb, dev, da[j]);
                da[j] = 0;
                freed_this_batch++;
                
                if (freed_this_batch >= ITRUNC_BATCH_SIZE) {
                    xv6fs_log_write(xv6_sb, dbp);
                    brelse(dbp);
                    xv6fs_iupdate(ip);
                    xv6fs_end_op(xv6_sb);
                    xv6fs_begin_op(xv6_sb);
                    freed_this_batch = 0;
                    dbp = bread(dev, ip->addrs[XV6FS_NDIRECT + 1]);
                    da = (uint*)dbp->data;
                }
            }
        }
        xv6fs_log_write(xv6_sb, dbp);
        brelse(dbp);
        __xv6fs_bfree(xv6_sb, dev, ip->addrs[XV6FS_NDIRECT + 1]);
        ip->addrs[XV6FS_NDIRECT + 1] = 0;
    }
    
    ip->vfs_inode.size = 0;
    xv6fs_iupdate(ip);
    // Note: Transaction is still active - caller must call xv6fs_end_op
}

// Partial truncate: free blocks from 'first_block' to end of file
// This is done in small batches to stay within transaction limits
static int __xv6fs_truncate_partial(struct xv6fs_inode *ip, uint first_block) {
    struct xv6fs_superblock *xv6_sb = container_of(ip->vfs_inode.sb,
                                                    struct xv6fs_superblock, vfs_sb);
    uint dev = ip->dev;
    
    // Free direct blocks from first_block onwards
    for (uint i = first_block; i < XV6FS_NDIRECT; i++) {
        if (ip->addrs[i]) {
            __xv6fs_bfree(xv6_sb, dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }
    
    // Handle indirect blocks
    if (first_block <= XV6FS_NDIRECT) {
        // All indirect blocks need to be freed
        __xv6fs_itrunc_ind(xv6_sb, &ip->addrs[XV6FS_NDIRECT], dev);
    } else if (first_block < XV6FS_NDIRECT + XV6FS_NINDIRECT) {
        // Partial indirect block freeing
        uint ind_start = first_block - XV6FS_NDIRECT;
        __xv6fs_itrunc_ind_partial(xv6_sb, &ip->addrs[XV6FS_NDIRECT], dev, ind_start);
    }
    
    // Handle double indirect blocks
    uint dind_threshold = XV6FS_NDIRECT + XV6FS_NINDIRECT;
    if (first_block <= dind_threshold) {
        // All double indirect blocks need to be freed
        if (ip->addrs[XV6FS_NDIRECT + 1]) {
            struct buf *bp = bread(dev, ip->addrs[XV6FS_NDIRECT + 1]);
            uint *a = (uint*)bp->data;
            
            for (uint j = 0; j < XV6FS_NINDIRECT; j++) {
                if (a[j]) {
                    __xv6fs_itrunc_ind(xv6_sb, &a[j], dev);
                }
            }
            brelse(bp);
            __xv6fs_bfree(xv6_sb, dev, ip->addrs[XV6FS_NDIRECT + 1]);
            ip->addrs[XV6FS_NDIRECT + 1] = 0;
        }
    } else if (first_block < dind_threshold + XV6FS_NDINDIRECT) {
        // Partial double indirect freeing
        if (ip->addrs[XV6FS_NDIRECT + 1]) {
            uint rel_block = first_block - dind_threshold;
            uint l1_start = rel_block / XV6FS_NINDIRECT;
            uint l2_start = rel_block % XV6FS_NINDIRECT;
            
            struct buf *bp = bread(dev, ip->addrs[XV6FS_NDIRECT + 1]);
            uint *a = (uint*)bp->data;
            bool modified = false;
            
            // Handle partial first L1 entry
            if (l2_start > 0 && a[l1_start]) {
                __xv6fs_itrunc_ind_partial(xv6_sb, &a[l1_start], dev, l2_start);
                l1_start++;
            }
            
            // Free remaining L1 entries completely
            for (uint j = l1_start; j < XV6FS_NINDIRECT; j++) {
                if (a[j]) {
                    __xv6fs_itrunc_ind(xv6_sb, &a[j], dev);
                    modified = true;
                }
            }
            
            if (modified) {
                xv6fs_log_write(xv6_sb, bp);
            }
            brelse(bp);
            
            // Check if all L1 entries are now zero, free the dind block
            bp = bread(dev, ip->addrs[XV6FS_NDIRECT + 1]);
            a = (uint*)bp->data;
            bool all_zero = true;
            for (uint j = 0; j < XV6FS_NINDIRECT; j++) {
                if (a[j]) {
                    all_zero = false;
                    break;
                }
            }
            brelse(bp);
            
            if (all_zero) {
                __xv6fs_bfree(xv6_sb, dev, ip->addrs[XV6FS_NDIRECT + 1]);
                ip->addrs[XV6FS_NDIRECT + 1] = 0;
            }
        }
    }
    
    return 0;
}

int xv6fs_truncate(struct vfs_inode *inode, loff_t new_size) {
    if (inode == NULL) return -EINVAL;
    if (new_size < 0) return -EINVAL;
    
    // Check max file size (MAXFILE blocks)
    if (new_size > (loff_t)XV6FS_MAXFILE * BSIZE) {
        return -EFBIG;
    }
    
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    
    loff_t old_size = inode->size;
    
    if (new_size == old_size) {
        return 0;  // No change needed
    }
    
    if (new_size == 0) {
        // Full truncation - use the optimized path
        xv6fs_begin_op(xv6_sb);
        xv6fs_itrunc(ip);
        xv6fs_end_op(xv6_sb);
        return 0;
    }
    
    if (new_size < old_size) {
        // Shrinking file - free blocks beyond new size
        // Calculate first block to free (block containing byte at new_size)
        // If new_size is block-aligned, start from that block
        // Otherwise, keep the partial block and free from next block
        uint first_block = (new_size + BSIZE - 1) / BSIZE;
        
        xv6fs_begin_op(xv6_sb);
        __xv6fs_truncate_partial(ip, first_block);
        inode->size = new_size;
        xv6fs_iupdate(ip);
        xv6fs_end_op(xv6_sb);
        return 0;
    }
    
    // Extending file - allocate blocks as needed
    uint old_blocks = (old_size + BSIZE - 1) / BSIZE;
    uint new_blocks = (new_size + BSIZE - 1) / BSIZE;
    
    xv6fs_begin_op(xv6_sb);
    for (uint bn = old_blocks; bn < new_blocks; bn++) {
        if (xv6fs_bmap(ip, bn) == 0) {
            xv6fs_end_op(xv6_sb);
            return -ENOSPC;
        }
    }
    inode->size = new_size;
    xv6fs_iupdate(ip);
    xv6fs_end_op(xv6_sb);
    
    return 0;
}

/******************************************************************************
 * Directory operations
 ******************************************************************************/

int xv6fs_lookup(struct vfs_inode *dir, struct vfs_dentry *dentry,
                 const char *name, size_t name_len) {
    if (dir == NULL || dentry == NULL || name == NULL) {
        return -EINVAL;
    }
    
    if (!S_ISDIR(dir->mode)) {
        return -ENOTDIR;
    }
    
    struct xv6fs_inode *dp = container_of(dir, struct xv6fs_inode, vfs_inode);
    struct dirent de;
    
    // Search through directory entries
    for (uint off = 0; off < dir->size; off += sizeof(de)) {
        // Read directory entry
        uint bn = off / BSIZE;
        uint block_off = off % BSIZE;
        uint addr = xv6fs_bmap(dp, bn);
        if (addr == 0) continue;
        
        struct buf *bp = bread(dp->dev, addr);
        if (bp == NULL) continue;
        
        memmove(&de, bp->data + block_off, sizeof(de));
        brelse(bp);
        
        if (de.inum == 0) continue;
        
        // Compare names
        if (name_len == strnlen(de.name, DIRSIZ) &&
            strncmp(name, de.name, name_len) == 0) {
            // Found
            dentry->ino = de.inum;
            dentry->sb = dir->sb;
            dentry->parent = dir;
            return 0;
        }
    }
    
    return -ENOENT;
}

int xv6fs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter,
                   struct vfs_dentry *ret_dentry) {
    if (dir == NULL || iter == NULL || ret_dentry == NULL) {
        return -EINVAL;
    }
    
    if (!S_ISDIR(dir->mode)) {
        return -ENOTDIR;
    }
    
    struct xv6fs_inode *dp = container_of(dir, struct xv6fs_inode, vfs_inode);
    struct dirent de;
    char *name = NULL;
    
    // VFS handles "." via iter->index == 0.
    // VFS calls driver with iter->index == 1 for ".." on ordinary directories.
    // VFS calls driver with iter->index > 1 for regular entries.
    
    // Handle ".." entry when index == 1
    if (iter->index == 1) {
        // Look up ".." in the on-disk directory to get parent inode number
        for (uint off = 0; off < dir->size; off += sizeof(de)) {
            uint bn = off / BSIZE;
            uint block_off = off % BSIZE;
            uint addr = xv6fs_bmap(dp, bn);
            if (addr == 0) continue;
            
            struct buf *bp = bread(dp->dev, addr);
            if (bp == NULL) continue;
            
            memmove(&de, bp->data + block_off, sizeof(de));
            brelse(bp);
            
            if (de.inum == 0) continue;
            
            if (de.name[0] == '.' && de.name[1] == '.' && de.name[2] == '\0') {
                vfs_release_dentry(ret_dentry);
                ret_dentry->name = strndup("..", 2);
                if (ret_dentry->name == NULL) return -ENOMEM;
                ret_dentry->name_len = 2;
                ret_dentry->ino = de.inum;
                ret_dentry->sb = dir->sb;  // VFS doesn't set sb for index==1
                ret_dentry->cookies = 0;  // Will be reset by VFS for index > 1
                return 0;
            }
        }
        // ".." not found on disk (shouldn't happen for valid dirs)
        return -ENOENT;
    }
    
    // Handle regular entries when index > 1
    uint start_off = (uint)iter->cookies;
    
    // Iterate through directory entries starting from cookies offset
    for (uint off = start_off; off < dir->size; off += sizeof(de)) {
        uint bn = off / BSIZE;
        uint block_off = off % BSIZE;
        uint addr = xv6fs_bmap(dp, bn);
        if (addr == 0) continue;
        
        struct buf *bp = bread(dp->dev, addr);
        if (bp == NULL) continue;
        
        memmove(&de, bp->data + block_off, sizeof(de));
        brelse(bp);
        
        if (de.inum == 0) continue;
        
        // Skip . and .. as VFS handles them
        if ((de.name[0] == '.' && de.name[1] == '\0') ||
            (de.name[0] == '.' && de.name[1] == '.' && de.name[2] == '\0')) {
            continue;
        }
        
        // Allocate memory for name (freed by vfs_release_dentry)
        size_t namelen = strnlen(de.name, DIRSIZ);
        name = strndup(de.name, namelen);
        if (name == NULL) return -ENOMEM;
        
        vfs_release_dentry(ret_dentry);
        ret_dentry->ino = de.inum;
        ret_dentry->name = name;
        ret_dentry->name_len = namelen;
        ret_dentry->cookies = off + sizeof(de);  // Next offset for continuation
        return 0;
    }
    
    // End of directory - return 0 with name=NULL to signal end
    vfs_release_dentry(ret_dentry);
    ret_dentry->name = NULL;
    ret_dentry->name_len = 0;
    ret_dentry->cookies = VFS_DENTRY_COOKIE_END;
    return 0;
}

/******************************************************************************
 * Create/Unlink operations
 ******************************************************************************/

/*
 * FIX: Check if a name already exists in a directory before creating entries.
 * This prevents duplicate directory entries which caused issues like:
 * - Multiple entries with same name in ls output
 * - Filesystem corruption from overlapping entries
 * 
 * Returns the inode number if found, 0 if not found.
 * Used by xv6fs_mkdir, xv6fs_link, and xv6fs_symlink to return -EEXIST
 * when attempting to create an entry with an existing name.
 */
static uint __xv6fs_dir_name_exists(struct xv6fs_inode *dp, const char *name) {
    struct dirent de;
    for (uint off = 0; off < dp->vfs_inode.size; off += sizeof(de)) {
        uint bn = off / BSIZE;
        uint block_off = off % BSIZE;
        uint addr = xv6fs_bmap_read(dp, bn);
        if (addr == 0) continue;
        
        struct buf *bp = bread(dp->dev, addr);
        memmove(&de, bp->data + block_off, sizeof(de));
        brelse(bp);
        
        if (de.inum != 0 && strncmp(de.name, name, DIRSIZ) == 0) {
            return de.inum;
        }
    }
    return 0;
}

// Add a directory entry
static int __xv6fs_dirlink(struct xv6fs_superblock *xv6_sb, struct xv6fs_inode *dp, const char *name, uint inum) {
    struct dirent de;
    
    // Look for an empty directory slot
    uint off;
    for (off = 0; off < dp->vfs_inode.size; off += sizeof(de)) {
        uint bn = off / BSIZE;
        uint block_off = off % BSIZE;
        uint addr = xv6fs_bmap(dp, bn);
        if (addr == 0) continue;
        
        struct buf *bp = bread(dp->dev, addr);
        memmove(&de, bp->data + block_off, sizeof(de));
        brelse(bp);
        
        if (de.inum == 0) {
            goto found;
        }
    }
    
    // No empty slot, extend directory
    off = dp->vfs_inode.size;
    
found:
    strncpy(de.name, name, DIRSIZ);
    de.inum = inum;
    
    uint bn = off / BSIZE;
    uint block_off = off % BSIZE;
    uint addr = xv6fs_bmap(dp, bn);
    if (addr == 0) return -ENOSPC;
    
    struct buf *bp = bread(dp->dev, addr);
    memmove(bp->data + block_off, &de, sizeof(de));
    xv6fs_log_write(xv6_sb, bp);
    brelse(bp);
    
    if (off >= dp->vfs_inode.size) {
        dp->vfs_inode.size = off + sizeof(de);
        xv6fs_iupdate(dp);
    }
    
    return 0;
}

struct vfs_inode *xv6fs_create(struct vfs_inode *dir, mode_t mode,
                                const char *name, size_t name_len) {
    if (dir == NULL || name == NULL || name_len == 0 || name_len >= DIRSIZ) {
        return ERR_PTR(-EINVAL);
    }
    
    struct xv6fs_inode *dp = container_of(dir, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(dir->sb, struct xv6fs_superblock, vfs_sb);
    
    // Check if file already exists
    char name_buf[DIRSIZ];
    memset(name_buf, 0, DIRSIZ);
    strncpy(name_buf, name, name_len);
    
    uint existing_ino = 0;
    struct dirent de;
    for (uint off = 0; off < dir->size; off += sizeof(de)) {
        uint bn = off / BSIZE;
        uint block_off = off % BSIZE;
        uint addr = xv6fs_bmap_read(dp, bn);
        if (addr == 0) continue;
        
        struct buf *bp = bread(dp->dev, addr);
        memmove(&de, bp->data + block_off, sizeof(de));
        brelse(bp);
        
        if (de.inum != 0 && strncmp(de.name, name_buf, DIRSIZ) == 0) {
            existing_ino = de.inum;
            break;
        }
    }
    
    if (existing_ino != 0) {
        return ERR_PTR(-EEXIST);
    }
    
    // Allocate new inode through VFS layer
    struct vfs_inode *new_inode = vfs_alloc_inode(dir->sb);
    if (IS_ERR_OR_NULL(new_inode)) {
        return new_inode == NULL ? ERR_PTR(-ENOMEM) : new_inode;
    }
    // vfs_alloc_inode returns the inode locked
    
    struct xv6fs_inode *ip = container_of(new_inode, struct xv6fs_inode, vfs_inode);
    ip->dev = dp->dev;
    new_inode->mode = mode | S_IFREG;
    new_inode->n_links = 1;
    new_inode->size = 0;
    xv6fs_iupdate(ip);
    
    // Add directory entry (name_buf already prepared above)
    int ret = __xv6fs_dirlink(xv6_sb, dp, name_buf, new_inode->ino);
    if (ret != 0) {
        // TODO: Free inode on failure
        vfs_iunlock(new_inode);
        return ERR_PTR(ret);
    }
    
    vfs_idup(new_inode);
    vfs_iunlock(new_inode);  // VFS's vfs_create will re-lock it
    return new_inode;
}

struct vfs_inode *xv6fs_mkdir(struct vfs_inode *dir, mode_t mode,
                               const char *name, size_t name_len) {
    if (dir == NULL || name == NULL || name_len == 0 || name_len >= DIRSIZ) {
        return ERR_PTR(-EINVAL);
    }
    
    struct xv6fs_inode *dp = container_of(dir, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(dir->sb, struct xv6fs_superblock, vfs_sb);
    
    // Check if name already exists
    char name_buf[DIRSIZ];
    memset(name_buf, 0, DIRSIZ);
    strncpy(name_buf, name, name_len);
    
    if (__xv6fs_dir_name_exists(dp, name_buf) != 0) {
        return ERR_PTR(-EEXIST);
    }
    
    // Allocate new inode through VFS layer (handles mutex init and hash add)
    struct vfs_inode *new_inode = vfs_alloc_inode(dir->sb);
    if (IS_ERR_OR_NULL(new_inode)) {
        return new_inode == NULL ? ERR_PTR(-ENOMEM) : new_inode;
    }
    // vfs_alloc_inode returns the inode locked
    
    struct xv6fs_inode *ip = container_of(new_inode, struct xv6fs_inode, vfs_inode);
    ip->dev = dp->dev;
    new_inode->mode = mode | S_IFDIR;
    new_inode->n_links = 1;
    new_inode->size = 0;
    
    // Create . and .. entries
    if (__xv6fs_dirlink(xv6_sb, ip, ".", new_inode->ino) < 0 ||
        __xv6fs_dirlink(xv6_sb, ip, "..", dir->ino) < 0) {
        // TODO: Cleanup on failure
        vfs_iunlock(new_inode);
        return ERR_PTR(-EIO);
    }
    
    xv6fs_iupdate(ip);
    
    // Add directory entry in parent (name_buf already set earlier)
    if (__xv6fs_dirlink(xv6_sb, dp, name_buf, new_inode->ino) < 0) {
        vfs_iunlock(new_inode);
        return ERR_PTR(-EIO);
    }
    
    // Update parent's link count for ..
    dir->n_links++;
    xv6fs_iupdate(dp);
    
    vfs_idup(new_inode);
    vfs_iunlock(new_inode);  // VFS's vfs_mkdir will re-lock it
    return new_inode;
}

struct vfs_inode *xv6fs_unlink(struct vfs_inode *dir, const char *name, size_t name_len) {
    if (dir == NULL || name == NULL) {
        return ERR_PTR(-EINVAL);
    }
    
    if (name_len == 1 && name[0] == '.') {
        return ERR_PTR(-EINVAL);
    }
    if (name_len == 2 && name[0] == '.' && name[1] == '.') {
        return ERR_PTR(-EINVAL);
    }
    
    struct xv6fs_superblock *xv6_sb = container_of(dir->sb, struct xv6fs_superblock, vfs_sb);
    
    struct xv6fs_inode *dp = container_of(dir, struct xv6fs_inode, vfs_inode);
    struct dirent de;
    uint off;
    
    // Find the directory entry
    for (off = 0; off < dir->size; off += sizeof(de)) {
        uint bn = off / BSIZE;
        uint block_off = off % BSIZE;
        uint addr = xv6fs_bmap(dp, bn);
        if (addr == 0) continue;
        
        struct buf *bp = bread(dp->dev, addr);
        memmove(&de, bp->data + block_off, sizeof(de));
        brelse(bp);
        
        if (de.inum == 0) continue;
        
        if (name_len == strnlen(de.name, DIRSIZ) &&
            strncmp(name, de.name, name_len) == 0) {
            // Found - clear the entry
            uint inum = de.inum;
            memset(&de, 0, sizeof(de));
            
            bp = bread(dp->dev, addr);
            memmove(bp->data + block_off, &de, sizeof(de));
            xv6fs_log_write(xv6_sb, bp);
            brelse(bp);
            
            // Get the target inode (vfs_get_inode returns it locked)
            struct vfs_inode *target = vfs_get_inode(dir->sb, inum);
            if (IS_ERR_OR_NULL(target)) {
                return target == NULL ? ERR_PTR(-ENOMEM) : target;
            }
            
            // inode is already locked by vfs_get_inode
            target->n_links--;
            
            struct xv6fs_inode *tip = container_of(target, struct xv6fs_inode, vfs_inode);
            xv6fs_iupdate(tip);
            vfs_iunlock(target);
            
            // Return the target inode - VFS will call vfs_iput on it after
            // releasing the superblock lock. This handles both cases:
            // - n_links == 0: inode will be freed when refcount reaches 0
            // - n_links > 0: just releases the reference from vfs_get_inode
            return target;
        }
    }
    
    return ERR_PTR(-ENOENT);
}

struct vfs_inode *xv6fs_rmdir(struct vfs_inode *dir, const char *name, size_t name_len) {
    // For now, just use unlink logic
    // TODO: Check if directory is empty
    return xv6fs_unlink(dir, name, name_len);
}

int xv6fs_link(struct vfs_inode *old, struct vfs_inode *dir,
               const char *name, size_t name_len) {
    if (old == NULL || dir == NULL || name == NULL || name_len >= DIRSIZ) {
        return -EINVAL;
    }
    
    if (S_ISDIR(old->mode)) {
        return -EPERM;  // Can't hard link directories
    }
    
    struct xv6fs_inode *dp = container_of(dir, struct xv6fs_inode, vfs_inode);
    struct xv6fs_inode *ip = container_of(old, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(dir->sb, struct xv6fs_superblock, vfs_sb);
    
    // Check if name already exists
    char name_buf[DIRSIZ];
    memset(name_buf, 0, DIRSIZ);
    strncpy(name_buf, name, name_len);
    
    if (__xv6fs_dir_name_exists(dp, name_buf) != 0) {
        return -EEXIST;
    }
    
    old->n_links++;
    xv6fs_iupdate(ip);
    
    // name_buf already set earlier for EEXIST check
    int ret = __xv6fs_dirlink(xv6_sb, dp, name_buf, old->ino);
    if (ret != 0) {
        old->n_links--;
        xv6fs_iupdate(ip);
        return ret;
    }
    
    return 0;
}

/******************************************************************************
 * Symlink operations
 ******************************************************************************/

ssize_t xv6fs_readlink(struct vfs_inode *inode, char *buf, size_t buflen) {
    if (inode == NULL || buf == NULL) {
        return -EINVAL;
    }
    
    if (!S_ISLNK(inode->mode)) {
        return -EINVAL;
    }
    
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    size_t link_len = inode->size;
    
    if (link_len + 1 > buflen) {
        return -ENAMETOOLONG;
    }
    
    // Read symlink target from data blocks
    size_t bytes_read = 0;
    while (bytes_read < link_len) {
        uint bn = bytes_read / BSIZE;
        uint off = bytes_read % BSIZE;
        uint n = BSIZE - off;
        if (n > link_len - bytes_read) {
            n = link_len - bytes_read;
        }
        
        uint addr = xv6fs_bmap(ip, bn);
        if (addr == 0) {
            return -EIO;
        }
        
        struct buf *bp = bread(ip->dev, addr);
        if (bp == NULL) {
            return -EIO;
        }
        memmove(buf + bytes_read, bp->data + off, n);
        brelse(bp);
        
        bytes_read += n;
    }
    
    buf[link_len] = '\0';
    return (ssize_t)link_len;
}

struct vfs_inode *xv6fs_symlink(struct vfs_inode *dir, mode_t mode,
                                 const char *name, size_t name_len,
                                 const char *target, size_t target_len) {
    if (dir == NULL || name == NULL || name_len == 0 || name_len >= DIRSIZ ||
        target == NULL || target_len == 0) {
        return ERR_PTR(-EINVAL);
    }
    
    struct xv6fs_inode *dp = container_of(dir, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(dir->sb, struct xv6fs_superblock, vfs_sb);
    
    // Check if name already exists
    char name_buf[DIRSIZ];
    memset(name_buf, 0, DIRSIZ);
    strncpy(name_buf, name, name_len);
    
    if (__xv6fs_dir_name_exists(dp, name_buf) != 0) {
        return ERR_PTR(-EEXIST);
    }
    
    // Allocate new inode through VFS layer
    struct vfs_inode *new_inode = vfs_alloc_inode(dir->sb);
    if (IS_ERR_OR_NULL(new_inode)) {
        return new_inode == NULL ? ERR_PTR(-ENOMEM) : new_inode;
    }
    // vfs_alloc_inode returns the inode locked
    
    struct xv6fs_inode *ip = container_of(new_inode, struct xv6fs_inode, vfs_inode);
    ip->dev = dp->dev;
    new_inode->mode = S_IFLNK | 0777;
    new_inode->n_links = 1;
    new_inode->size = 0;
    
    // Write symlink target to data blocks
    size_t bytes_written = 0;
    while (bytes_written < target_len) {
        uint bn = bytes_written / BSIZE;
        uint off = bytes_written % BSIZE;
        uint n = BSIZE - off;
        if (n > target_len - bytes_written) {
            n = target_len - bytes_written;
        }
        
        uint addr = xv6fs_bmap(ip, bn);
        if (addr == 0) {
            // Failed to allocate block - cleanup
            xv6fs_itrunc(ip);
            vfs_iunlock(new_inode);
            return ERR_PTR(-ENOSPC);
        }
        
        struct buf *bp = bread(ip->dev, addr);
        if (bp == NULL) {
            xv6fs_itrunc(ip);
            vfs_iunlock(new_inode);
            return ERR_PTR(-EIO);
        }
        memmove(bp->data + off, target + bytes_written, n);
        xv6fs_log_write(xv6_sb, bp);
        brelse(bp);
        
        bytes_written += n;
    }
    
    new_inode->size = target_len;
    xv6fs_iupdate(ip);
    
    // Add directory entry (name_buf already set earlier)
    int ret = __xv6fs_dirlink(xv6_sb, dp, name_buf, new_inode->ino);
    if (ret != 0) {
        xv6fs_itrunc(ip);
        vfs_iunlock(new_inode);
        return ERR_PTR(ret);
    }
    
    vfs_idup(new_inode);
    vfs_iunlock(new_inode);
    return new_inode;
}

/******************************************************************************
 * Device file operations (mknod)
 ******************************************************************************/

struct vfs_inode *xv6fs_mknod(struct vfs_inode *dir, mode_t mode,
                               dev_t dev, const char *name, size_t name_len) {
    if (dir == NULL || name == NULL || name_len == 0 || name_len >= DIRSIZ) {
        return ERR_PTR(-EINVAL);
    }
    
    // xv6 only supports character and block devices
    if (!S_ISBLK(mode) && !S_ISCHR(mode)) {
        return ERR_PTR(-EINVAL);
    }
    
    struct xv6fs_inode *dp = container_of(dir, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(dir->sb, struct xv6fs_superblock, vfs_sb);
    
    // Allocate new inode through VFS layer
    struct vfs_inode *new_inode = vfs_alloc_inode(dir->sb);
    if (IS_ERR_OR_NULL(new_inode)) {
        return new_inode == NULL ? ERR_PTR(-ENOMEM) : new_inode;
    }
    // vfs_alloc_inode returns the inode locked
    
    struct xv6fs_inode *ip = container_of(new_inode, struct xv6fs_inode, vfs_inode);
    ip->dev = dp->dev;
    new_inode->mode = mode;
    new_inode->n_links = 1;
    new_inode->size = 0;
    
    // Set major/minor device numbers
    ip->major = major(dev);
    ip->minor = minor(dev);
    if (S_ISCHR(mode)) {
        new_inode->cdev = dev;
    } else if (S_ISBLK(mode)) {
        new_inode->bdev = dev;
    }
    
    xv6fs_iupdate(ip);
    
    // Add directory entry
    char name_buf[DIRSIZ];
    memset(name_buf, 0, DIRSIZ);
    strncpy(name_buf, name, name_len);
    
    int ret = __xv6fs_dirlink(xv6_sb, dp, name_buf, new_inode->ino);
    if (ret != 0) {
        // TODO: Free inode on failure
        vfs_iunlock(new_inode);
        return ERR_PTR(ret);
    }
    
    vfs_idup(new_inode);
    vfs_iunlock(new_inode);  // VFS's vfs_mknod will re-lock it
    return new_inode;
}

/******************************************************************************
 * Inode lifecycle
 ******************************************************************************/

void xv6fs_destroy_inode(struct vfs_inode *inode) {
    if (inode == NULL) return;
    
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    struct xv6fs_superblock *xv6_sb = container_of(inode->sb, struct xv6fs_superblock, vfs_sb);
    
    xv6fs_begin_op(xv6_sb);
    xv6fs_itrunc(ip);
    
    // Mark inode as free on disk
    struct buf *bp = bread(ip->dev, XV6FS_IBLOCK(inode->ino, &xv6_sb->disk_sb));
    struct dinode *dip = (struct dinode*)bp->data + inode->ino % IPB;
    dip->type = 0;
    xv6fs_log_write(xv6_sb, bp);
    brelse(bp);
    
    xv6fs_end_op(xv6_sb);
}

void xv6fs_free_inode(struct vfs_inode *inode) {
    if (inode == NULL) return;
    
    struct xv6fs_inode *ip = container_of(inode, struct xv6fs_inode, vfs_inode);
    // Free the in-memory structure
    slab_free(ip);
}

/******************************************************************************
 * Open callback
 ******************************************************************************/

int xv6fs_open(struct vfs_inode *inode, struct vfs_file *file, int f_flags) {
    if (inode == NULL || file == NULL) {
        return -EINVAL;
    }
    
    if (S_ISREG(inode->mode)) {
        file->ops = &xv6fs_file_ops;
        return 0;
    }
    
    if (S_ISDIR(inode->mode)) {
        // Directories use dir_iter for reading
        file->ops = &xv6fs_file_ops;
        return 0;
    }
    
    if (S_ISLNK(inode->mode)) {
        /*
         * FIX: Allow opening symlinks with O_NOFOLLOW flag.
         * Previously this returned -ELOOP unconditionally, but POSIX requires
         * that symlinks can be opened with O_NOFOLLOW to allow fstat() on the
         * symlink itself (not its target). This is needed by programs like ls
         * that want to display symlink information.
         */
        file->ops = &xv6fs_file_ops;
        return 0;
    }
    
    // Character/block devices are handled by VFS core
    if (S_ISCHR(inode->mode) || S_ISBLK(inode->mode)) {
        return -EINVAL; // Should be handled by VFS
    }
    
    return -ENOSYS;
}

/******************************************************************************
 * VFS inode operations structure
 ******************************************************************************/

struct vfs_inode_ops xv6fs_inode_ops = {
    .lookup = xv6fs_lookup,
    .dir_iter = xv6fs_dir_iter,
    .readlink = xv6fs_readlink,
    .create = xv6fs_create,
    .link = xv6fs_link,
    .unlink = xv6fs_unlink,
    .mkdir = xv6fs_mkdir,
    .rmdir = xv6fs_rmdir,
    .mknod = xv6fs_mknod,
    .move = NULL,      // TODO: Implement move/rename
    .symlink = xv6fs_symlink,
    .truncate = xv6fs_truncate,
    .destroy_inode = xv6fs_destroy_inode,
    .free_inode = xv6fs_free_inode,
    .dirty_inode = xv6fs_dirty_inode,
    .sync_inode = xv6fs_sync_inode,
    .open = xv6fs_open,
};
