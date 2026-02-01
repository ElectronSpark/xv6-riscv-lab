/*
 * xv6fs block allocation and truncation
 *
 * TRANSACTION MANAGEMENT: FS-INTERNAL (not VFS-managed)
 * =====================================================
 * File operations (write, truncate) manage transactions internally because:
 * 1. They require BATCHED transactions for large files (multiple begin/end cycles)
 * 2. VFS holds inode lock before calling file ops, so VFS can't wrap them
 *
 * This is the "hybrid approach" documented in superblock.c:
 * - Metadata ops: VFS manages transactions via callbacks
 * - File ops: FS manages transactions internally (here)
 *
 * Lock ordering for truncate: inode_mutex → transaction
 * (Reversed from metadata ops, but safe because different inodes are involved)
 *
 * See superblock.c "Transaction Callbacks" comment for full design explanation.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include <mm/vm.h>
#include "dev/buf.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include <mm/slab.h>
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
