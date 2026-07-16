//! xv6fs block allocation and truncation — Rust port of
//! `kernel/vfs/xv6fs/truncate.c` (Phase 2 Wave 19, sub-wave B; see
//! `super` module doc and `docs/rustify/phase2_plan.md`).
//!
//! Block mapping (`bmap`: direct / single-indirect / double-indirect,
//! allocating on demand via [`super::block_cache`]'s rb-tree) and
//! grow/shrink truncation, batched into multiple log transactions for
//! large files (see `xv6fs_itrunc`'s own doc).
//!
//! # Transaction management: FS-internal, not VFS-managed
//!
//! File operations (write, truncate) manage transactions internally
//! because:
//! 1. They require BATCHED transactions for large files (multiple
//!    begin/end cycles).
//! 2. VFS holds the inode lock before calling file ops, so VFS can't wrap
//!    them.
//!
//! This is the "hybrid approach" documented in `superblock.rs`'s module
//! doc: metadata ops use VFS-managed transactions via callbacks; file ops
//! (here) manage transactions internally.
//!
//! Lock ordering for truncate: `inode_mutex` -> transaction (reversed
//! from metadata ops, but safe because different inodes are involved).
//!
//! # Intra-driver calls
//!
//! Following this crate's established filesystem-driver convention (see
//! `tmpfs/superblock.rs`'s module doc, e.g. its own
//! `super::inode::tmpfs_make_directory` call): calls to sibling xv6fs
//! submodules ([`super::inode`], [`super::log`], [`super::block_cache`])
//! use plain Rust module paths, not `extern "C"` blocks — those are
//! genuinely the same logical driver unit (`truncate.c`'s old
//! single-translation-unit boundary with the rest of xv6fs), unlike calls
//! into vfs core / mm / the classic buffer cache, which stay `extern
//! "C"`.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_int, c_void};
use core::ptr;

use crate::bindings::{buf, loff_t, vfs_inode, xv6fs_inode, xv6fs_superblock, EFBIG, EINVAL, ENOSPC};
use crate::proc::proc_shims::xv6_panic;

use super::block_cache::{xv6fs_bcache_find_free_block, xv6fs_bcache_find_free_block_near, xv6fs_bcache_mark_free};
use super::inode::xv6fs_iupdate;
use super::log::{xv6fs_begin_op, xv6fs_begin_op_nointr, xv6fs_end_op, xv6fs_log_write};

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention (this
// block: symbols outside the xv6fs driver).
// ===========================================================================

unsafe extern "C" {
    // string.rs.
    safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;

}

// P3-D3c: `bufcache.rs`'s entry points are plain (safe) Rust fns now that
// their `#[no_mangle]` exports are gone; identical signatures, plain `use`.
use crate::bufcache::{bread, brelse};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// Mirrors `BBLOCK_PTR(b, sbp)` (`truncate.c`, macro) -- hardcoded
/// locally, same rationale/precedent as `block_cache.rs`'s own copy.
#[inline(always)]
fn bblock_ptr(b: u32, bmapstart: u32) -> u32 {
    b / super::BPB + bmapstart
}

// ===========================================================================
// Block allocation using cache
// ===========================================================================

/// Mirrors `__xv6fs_balloc()`. Allocate a block using the block cache.
/// Marks the block as used in both the cache and the on-disk bitmap.
/// Zeros the block and returns the block number. Returns 0 if no free
/// block is available.
///
/// `hint`: if non-zero, try to allocate near this block for locality.
///
/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock` with a transaction
/// active.
unsafe fn __xv6fs_balloc(xv6_sb: *mut xv6fs_superblock, dev: u32, hint: u32) -> u32 {
    unsafe {
        let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);
        let mut blockno: u32 = 0;

        // Try to find a free block using the cache.
        if (*xv6_sb).block_cache.initialized != 0 {
            let rc = if hint != 0 {
                xv6fs_bcache_find_free_block_near(xv6_sb, hint, ptr::addr_of_mut!(blockno))
            } else {
                xv6fs_bcache_find_free_block(xv6_sb, ptr::addr_of_mut!(blockno))
            };

            if rc == 0 {
                // Block already removed from cache by find_free_block.

                // Mark in on-disk bitmap.
                let bp = bread(dev, bblock_ptr(blockno, (*disk_sb).bmapstart));
                if bp.is_null() {
                    // Revert cache state on error.
                    xv6fs_bcache_mark_free(xv6_sb, blockno);
                    return 0;
                }

                let bi = (blockno % super::BPB) as usize;
                let m: u8 = 1 << (bi % 8);
                *(*bp).data.add(bi / 8) |= m;
                xv6fs_log_write(xv6_sb, bp);
                brelse(bp);

                // Zero the block.
                let zbp = bread(dev, blockno);
                if zbp.is_null() {
                    return 0;
                }
                memset((*zbp).data as *mut c_void, 0, super::BSIZE as usize);
                xv6fs_log_write(xv6_sb, zbp);
                brelse(zbp);

                return blockno;
            }
        }

        0 // No free blocks.
    }
}

// ===========================================================================
// Block mapping
// ===========================================================================

/// Mirrors `__xv6fs_bmap_ind()`. Get the disk block address for entry
/// `bn` of an indirect block, allocating the indirect block and/or the
/// data block if they don't exist. `hint`: last allocated block for
/// locality, 0 if none.
///
/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock` with a transaction
/// active; `entry` must point to a live `uint` (either an inode's
/// `addrs[]` slot or a slot within an indirect block's data).
unsafe fn __xv6fs_bmap_ind(xv6_sb: *mut xv6fs_superblock, entry: *mut u32, dev: u32, bn: u32, hint: u32) -> u32 {
    unsafe {
        if *entry == 0 {
            // Allocate indirect block using cache.
            let addr = __xv6fs_balloc(xv6_sb, dev, hint);
            if addr == 0 {
                return 0;
            }
            *entry = addr;
        }

        let bp = bread(dev, *entry);
        if bp.is_null() {
            return 0;
        }

        let a = (*bp).data as *mut u32;
        let mut addr = *a.add(bn as usize);

        if addr == 0 {
            // Allocate data block using cache with locality hint.
            let locality_hint = *entry; // Use indirect block as hint for locality.
            addr = __xv6fs_balloc(xv6_sb, dev, locality_hint);
            if addr == 0 {
                brelse(bp);
                return 0;
            }

            *a.add(bn as usize) = addr;
            xv6fs_log_write(xv6_sb, bp);
        }

        brelse(bp);
        addr
    }
}

/// Read-only bmap -- returns 0 if the block doesn't exist (for sparse
/// files). Does not allocate any blocks.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_bmap_read(ip: *mut xv6fs_inode, mut bn: u32) -> u32 {
    // SAFETY: `ip` is live (caller's contract).
    unsafe {
        let dev = (*ip).dev;

        // Direct blocks.
        if bn < super::NDIRECT {
            return (*ip).addrs[bn as usize]; // May be 0 for sparse files.
        }
        bn -= super::NDIRECT;

        // Single indirect block.
        if bn < super::NINDIRECT {
            if (*ip).addrs[super::NDIRECT as usize] == 0 {
                return 0;
            }
            let bp = bread(dev, (*ip).addrs[super::NDIRECT as usize]);
            if bp.is_null() {
                return 0;
            }
            let addr = *((*bp).data as *mut u32).add(bn as usize);
            brelse(bp);
            return addr;
        }
        bn -= super::NINDIRECT;

        // Double indirect block.
        if bn < super::NDINDIRECT {
            if (*ip).addrs[super::NDIRECT as usize + 1] == 0 {
                return 0;
            }
            let bp = bread(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
            if bp.is_null() {
                return 0;
            }

            let l1_idx = bn / super::NINDIRECT;
            let l2_idx = bn % super::NINDIRECT;
            let a = (*bp).data as *mut u32;

            let l1_val = *a.add(l1_idx as usize);
            if l1_val == 0 {
                brelse(bp);
                return 0;
            }
            let l1_addr = l1_val;
            brelse(bp);

            let bp = bread(dev, l1_addr);
            if bp.is_null() {
                return 0;
            }
            let addr = *((*bp).data as *mut u32).add(l2_idx as usize);
            brelse(bp);
            return addr;
        }

        0 // Block number out of range.
    }
}

/// Mirrors `xv6fs_bmap()`. Get (allocating) the disk block address for
/// the `bn`-th block of inode `ip`.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_bmap(ip: *mut xv6fs_inode, mut bn: u32) -> u32 {
    // SAFETY: `ip` is live with a transaction active (caller's contract).
    unsafe {
        let xv6_sb = (*ip).vfs_inode.sb as *mut xv6fs_superblock;
        let dev = (*ip).dev;

        // Get hint for locality: use last allocated block if any.
        let mut hint: u32 = 0;
        let mut i = super::NDIRECT as isize - 1;
        while i >= 0 {
            if (*ip).addrs[i as usize] != 0 {
                hint = (*ip).addrs[i as usize];
                break;
            }
            i -= 1;
        }

        // Direct blocks.
        if bn < super::NDIRECT {
            let mut addr = (*ip).addrs[bn as usize];
            if addr == 0 {
                // Allocate new block using cache with locality hint.
                addr = __xv6fs_balloc(xv6_sb, dev, hint);
                if addr == 0 {
                    return 0;
                }
                (*ip).addrs[bn as usize] = addr;
            }
            return addr;
        }
        bn -= super::NDIRECT;

        // Single indirect block.
        if bn < super::NINDIRECT {
            return __xv6fs_bmap_ind(xv6_sb, ptr::addr_of_mut!((*ip).addrs[super::NDIRECT as usize]), dev, bn, hint);
        }
        bn -= super::NINDIRECT;

        // Double indirect block.
        if bn < super::NDINDIRECT {
            if (*ip).addrs[super::NDIRECT as usize + 1] == 0 {
                // Allocate double indirect block using cache.
                let addr = __xv6fs_balloc(xv6_sb, dev, hint);
                if addr == 0 {
                    return 0;
                }
                (*ip).addrs[super::NDIRECT as usize + 1] = addr;
            }

            let bp = bread(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
            if bp.is_null() {
                return 0;
            }

            let l1_idx = bn / super::NINDIRECT;
            let l2_idx = bn % super::NINDIRECT;
            let a = (*bp).data as *mut u32;

            let addr = __xv6fs_bmap_ind(xv6_sb, a.add(l1_idx as usize), dev, l2_idx, (*ip).addrs[super::NDIRECT as usize + 1]);
            if *a.add(l1_idx as usize) != 0 {
                xv6fs_log_write(xv6_sb, bp);
            }
            brelse(bp);
            return addr;
        }

        xv6_panic(c"xv6fs_bmap: block number too large".as_ptr());
    }
}

// ===========================================================================
// Truncate
// ===========================================================================

/// Mirrors `__xv6fs_bfree()`. Free a block.
///
/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock` with a transaction
/// active.
unsafe fn __xv6fs_bfree(xv6_sb: *mut xv6fs_superblock, dev: u32, b: u32) {
    unsafe {
        let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);
        let bp = bread(dev, bblock_ptr(b, (*disk_sb).bmapstart));
        if bp.is_null() {
            // See `inode.rs`'s module doc "Fidelity note": the C original
            // does not NULL-check `bread()` results uniformly either, but
            // an unconditional deref here would be Rust UB.
            xv6_panic(c"xv6fs_bfree: bread failed".as_ptr());
        }
        let bi = (b % super::BPB) as usize;
        let m: u8 = 1 << (bi % 8);
        if *(*bp).data.add(bi / 8) & m == 0 {
            xv6_panic(c"xv6fs_bfree: freeing free block".as_ptr());
        }
        *(*bp).data.add(bi / 8) &= !m;
        xv6fs_log_write(xv6_sb, bp);
        brelse(bp);

        // Update the block cache.
        xv6fs_bcache_mark_free(xv6_sb, b);
    }
}

/// Mirrors `__xv6fs_itrunc_ind_partial()`. Free indirect blocks starting
/// from index `start_idx`. If `start_idx == 0`, frees the indirect block
/// itself too. Returns the number of blocks freed.
///
/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock` with a transaction
/// active; `entry` must point to a live `uint` slot.
unsafe fn __xv6fs_itrunc_ind_partial(xv6_sb: *mut xv6fs_superblock, entry: *mut u32, dev: u32, start_idx: u32) -> i32 {
    unsafe {
        if *entry == 0 {
            return 0;
        }

        let bp = bread(dev, *entry);
        if bp.is_null() {
            xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
        }
        let a = (*bp).data as *mut u32;
        let mut freed: i32 = 0;

        let mut j = start_idx;
        while j < super::NINDIRECT {
            let v = *a.add(j as usize);
            if v != 0 {
                __xv6fs_bfree(xv6_sb, dev, v);
                *a.add(j as usize) = 0;
                freed += 1;
            }
            j += 1;
        }

        if freed > 0 {
            xv6fs_log_write(xv6_sb, bp);
        }
        brelse(bp);

        // If we freed from the beginning, free the indirect block itself.
        if start_idx == 0 {
            __xv6fs_bfree(xv6_sb, dev, *entry);
            *entry = 0;
            freed += 1;
        }

        freed
    }
}

/// Mirrors `__xv6fs_itrunc_ind()` (full version). Free indirect blocks.
///
/// # Safety
/// Same as [`__xv6fs_itrunc_ind_partial`].
unsafe fn __xv6fs_itrunc_ind(xv6_sb: *mut xv6fs_superblock, entry: *mut u32, dev: u32) {
    unsafe { __xv6fs_itrunc_ind_partial(xv6_sb, entry, dev, 0) };
}

/// `ITRUNC_BATCH_SIZE` (`truncate.c`, `(MAXOPBLOCKS - 5) / 2`) -- maximum
/// blocks to free per transaction to stay within log limits. Each
/// `bfree` modifies the bitmap (1 write), so ~`MAXOPBLOCKS/2` blocks can
/// be freed per transaction.
const ITRUNC_BATCH_SIZE: i32 = (super::MAXOPBLOCKS - 5) / 2;

/// Truncate inode `ip` to zero length, in batches to handle large files.
///
/// IMPORTANT: caller must have called `xv6fs_begin_op` before calling
/// this. This function may call `xv6fs_end_op`/`xv6fs_begin_op_nointr`
/// internally to commit batches. When this function returns, a
/// transaction is still active (caller should call `xv6fs_end_op`).
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_itrunc(ip: *mut xv6fs_inode) {
    // SAFETY: `ip` is live with a transaction active (caller's contract).
    unsafe {
        let xv6_sb = (*ip).vfs_inode.sb as *mut xv6fs_superblock;
        let dev = (*ip).dev;
        let mut freed_this_batch: i32 = 0;

        // Free direct blocks.
        let mut i: usize = 0;
        while i < super::NDIRECT as usize {
            if (*ip).addrs[i] != 0 {
                __xv6fs_bfree(xv6_sb, dev, (*ip).addrs[i]);
                (*ip).addrs[i] = 0;
                freed_this_batch += 1;

                if freed_this_batch >= ITRUNC_BATCH_SIZE {
                    // Commit current batch and start new transaction.
                    xv6fs_iupdate(ip);
                    xv6fs_end_op(xv6_sb);
                    xv6fs_begin_op_nointr(xv6_sb);
                    freed_this_batch = 0;
                }
            }
            i += 1;
        }

        // Free indirect blocks.
        if (*ip).addrs[super::NDIRECT as usize] != 0 {
            let mut bp = bread(dev, (*ip).addrs[super::NDIRECT as usize]);
            if bp.is_null() {
                xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
            }
            let mut a = (*bp).data as *mut u32;

            let mut j: u32 = 0;
            while j < super::NINDIRECT {
                let v = *a.add(j as usize);
                if v != 0 {
                    __xv6fs_bfree(xv6_sb, dev, v);
                    *a.add(j as usize) = 0;
                    freed_this_batch += 1;

                    if freed_this_batch >= ITRUNC_BATCH_SIZE {
                        xv6fs_log_write(xv6_sb, bp);
                        brelse(bp);
                        xv6fs_iupdate(ip);
                        xv6fs_end_op(xv6_sb);
                        xv6fs_begin_op_nointr(xv6_sb);
                        freed_this_batch = 0;
                        bp = bread(dev, (*ip).addrs[super::NDIRECT as usize]);
                        if bp.is_null() {
                            xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                        }
                        a = (*bp).data as *mut u32;
                    }
                }
                j += 1;
            }
            xv6fs_log_write(xv6_sb, bp);
            brelse(bp);
            __xv6fs_bfree(xv6_sb, dev, (*ip).addrs[super::NDIRECT as usize]);
            (*ip).addrs[super::NDIRECT as usize] = 0;
            freed_this_batch += 1;

            if freed_this_batch >= ITRUNC_BATCH_SIZE {
                xv6fs_iupdate(ip);
                xv6fs_end_op(xv6_sb);
                xv6fs_begin_op_nointr(xv6_sb);
                freed_this_batch = 0;
            }
        }

        // Free double indirect blocks.
        if (*ip).addrs[super::NDIRECT as usize + 1] != 0 {
            let mut dbp = bread(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
            if dbp.is_null() {
                xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
            }
            let mut da = (*dbp).data as *mut u32;

            let mut j: u32 = 0;
            while j < super::NINDIRECT {
                let dv = *da.add(j as usize);
                if dv != 0 {
                    let mut bp = bread(dev, dv);
                    if bp.is_null() {
                        xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                    }
                    let mut a = (*bp).data as *mut u32;

                    let mut k: u32 = 0;
                    while k < super::NINDIRECT {
                        let v = *a.add(k as usize);
                        if v != 0 {
                            __xv6fs_bfree(xv6_sb, dev, v);
                            *a.add(k as usize) = 0;
                            freed_this_batch += 1;

                            if freed_this_batch >= ITRUNC_BATCH_SIZE {
                                xv6fs_log_write(xv6_sb, bp);
                                brelse(bp);
                                xv6fs_log_write(xv6_sb, dbp);
                                brelse(dbp);
                                xv6fs_iupdate(ip);
                                xv6fs_end_op(xv6_sb);
                                xv6fs_begin_op_nointr(xv6_sb);
                                freed_this_batch = 0;
                                dbp = bread(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                                if dbp.is_null() {
                                    xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                                }
                                da = (*dbp).data as *mut u32;
                                bp = bread(dev, *da.add(j as usize));
                                if bp.is_null() {
                                    xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                                }
                                a = (*bp).data as *mut u32;
                            }
                        }
                        k += 1;
                    }
                    xv6fs_log_write(xv6_sb, bp);
                    brelse(bp);
                    __xv6fs_bfree(xv6_sb, dev, *da.add(j as usize));
                    *da.add(j as usize) = 0;
                    freed_this_batch += 1;

                    if freed_this_batch >= ITRUNC_BATCH_SIZE {
                        xv6fs_log_write(xv6_sb, dbp);
                        brelse(dbp);
                        xv6fs_iupdate(ip);
                        xv6fs_end_op(xv6_sb);
                        xv6fs_begin_op_nointr(xv6_sb);
                        freed_this_batch = 0;
                        dbp = bread(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                        if dbp.is_null() {
                            xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                        }
                        da = (*dbp).data as *mut u32;
                    }
                }
                j += 1;
            }
            xv6fs_log_write(xv6_sb, dbp);
            brelse(dbp);
            __xv6fs_bfree(xv6_sb, dev, (*ip).addrs[super::NDIRECT as usize + 1]);
            (*ip).addrs[super::NDIRECT as usize + 1] = 0;
        }

        (*ip).vfs_inode.size = 0;
        xv6fs_iupdate(ip);
        // Note: transaction is still active -- caller must call xv6fs_end_op.
    }
}

/// Mirrors `__xv6fs_truncate_partial()`. Partial truncate: free blocks
/// from `first_block` to end of file, in small batches to stay within
/// transaction limits.
///
/// # Safety
/// `ip` must point to a live `xv6fs_inode` with a transaction active.
unsafe fn __xv6fs_truncate_partial(ip: *mut xv6fs_inode, first_block: u32) -> c_int {
    unsafe {
        let xv6_sb = (*ip).vfs_inode.sb as *mut xv6fs_superblock;
        let dev = (*ip).dev;

        // Free direct blocks from first_block onwards.
        let mut i = first_block;
        while i < super::NDIRECT {
            if (*ip).addrs[i as usize] != 0 {
                __xv6fs_bfree(xv6_sb, dev, (*ip).addrs[i as usize]);
                (*ip).addrs[i as usize] = 0;
            }
            i += 1;
        }

        // Handle indirect blocks.
        if first_block <= super::NDIRECT {
            // All indirect blocks need to be freed.
            __xv6fs_itrunc_ind(xv6_sb, ptr::addr_of_mut!((*ip).addrs[super::NDIRECT as usize]), dev);
        } else if first_block < super::NDIRECT + super::NINDIRECT {
            // Partial indirect block freeing.
            let ind_start = first_block - super::NDIRECT;
            __xv6fs_itrunc_ind_partial(xv6_sb, ptr::addr_of_mut!((*ip).addrs[super::NDIRECT as usize]), dev, ind_start);
        }

        // Handle double indirect blocks.
        let dind_threshold = super::NDIRECT + super::NINDIRECT;
        if first_block <= dind_threshold {
            // All double indirect blocks need to be freed.
            if (*ip).addrs[super::NDIRECT as usize + 1] != 0 {
                let bp = bread(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                if bp.is_null() {
                    xv6_panic(c"xv6fs_truncate: bread failed".as_ptr());
                }
                let a = (*bp).data as *mut u32;

                let mut j: u32 = 0;
                while j < super::NINDIRECT {
                    let v = *a.add(j as usize);
                    if v != 0 {
                        __xv6fs_itrunc_ind(xv6_sb, a.add(j as usize), dev);
                    }
                    j += 1;
                }
                brelse(bp);
                __xv6fs_bfree(xv6_sb, dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                (*ip).addrs[super::NDIRECT as usize + 1] = 0;
            }
        } else if first_block < dind_threshold + super::NDINDIRECT {
            // Partial double indirect freeing.
            if (*ip).addrs[super::NDIRECT as usize + 1] != 0 {
                let rel_block = first_block - dind_threshold;
                let mut l1_start = rel_block / super::NINDIRECT;
                let l2_start = rel_block % super::NINDIRECT;

                let bp = bread(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                if bp.is_null() {
                    xv6_panic(c"xv6fs_truncate: bread failed".as_ptr());
                }
                let a = (*bp).data as *mut u32;
                let mut modified = false;

                // Handle partial first L1 entry.
                if l2_start > 0 && *a.add(l1_start as usize) != 0 {
                    __xv6fs_itrunc_ind_partial(xv6_sb, a.add(l1_start as usize), dev, l2_start);
                    l1_start += 1;
                }

                // Free remaining L1 entries completely.
                let mut j = l1_start;
                while j < super::NINDIRECT {
                    let v = *a.add(j as usize);
                    if v != 0 {
                        __xv6fs_itrunc_ind(xv6_sb, a.add(j as usize), dev);
                        modified = true;
                    }
                    j += 1;
                }

                if modified {
                    xv6fs_log_write(xv6_sb, bp);
                }
                brelse(bp);

                // Check if all L1 entries are now zero; free the dind block.
                let bp = bread(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                if bp.is_null() {
                    xv6_panic(c"xv6fs_truncate: bread failed".as_ptr());
                }
                let a = (*bp).data as *mut u32;
                let mut all_zero = true;
                let mut j: u32 = 0;
                while j < super::NINDIRECT {
                    if *a.add(j as usize) != 0 {
                        all_zero = false;
                        break;
                    }
                    j += 1;
                }
                brelse(bp);

                if all_zero {
                    __xv6fs_bfree(xv6_sb, dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                    (*ip).addrs[super::NDIRECT as usize + 1] = 0;
                }
            }
        }

        0
    }
}

/// Mirrors `xv6fs_truncate()`.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration; this is also [`super::inode::XV6FS_INODE_OPS`]'s
/// `.truncate` entry.
pub(crate) extern "C" fn xv6fs_truncate(inode: *mut vfs_inode, new_size: loff_t) -> c_int {
    if inode.is_null() {
        return neg(EINVAL);
    }
    if new_size < 0 {
        return neg(EINVAL);
    }

    // Check max file size (MAXFILE blocks).
    if new_size > super::MAXFILE as loff_t * super::BSIZE as loff_t {
        return neg(EFBIG);
    }

    let ip = inode as *mut xv6fs_inode;
    // SAFETY: `inode` is live (caller's contract).
    let xv6_sb = unsafe { (*inode).sb as *mut xv6fs_superblock };
    let old_size = unsafe { (*inode).size };

    if new_size == old_size {
        return 0; // No change needed.
    }

    if new_size == 0 {
        // Full truncation -- use the optimized path.
        let ret = xv6fs_begin_op(xv6_sb);
        if ret != 0 {
            return ret;
        }
        xv6fs_itrunc(ip);
        xv6fs_end_op(xv6_sb);
        return 0;
    }

    if new_size < old_size {
        // Shrinking file -- free blocks beyond new size. Calculate the
        // first block to free (the block containing the byte at
        // new_size). If new_size is block-aligned, start from that
        // block; otherwise keep the partial block and free from the next
        // block.
        let first_block = (new_size as u32 + super::BSIZE - 1) / super::BSIZE;

        let ret = xv6fs_begin_op(xv6_sb);
        if ret != 0 {
            return ret;
        }
        // SAFETY: `ip` is live with a transaction now active.
        unsafe {
            __xv6fs_truncate_partial(ip, first_block);
            (*inode).size = new_size;
        }
        xv6fs_iupdate(ip);
        xv6fs_end_op(xv6_sb);
        return 0;
    }

    // Extending file -- allocate blocks as needed.
    let old_blocks = (old_size as u32 + super::BSIZE - 1) / super::BSIZE;
    let new_blocks = (new_size as u32 + super::BSIZE - 1) / super::BSIZE;

    let ret = xv6fs_begin_op(xv6_sb);
    if ret != 0 {
        return ret;
    }
    let mut bn = old_blocks;
    while bn < new_blocks {
        if xv6fs_bmap(ip, bn) == 0 {
            xv6fs_end_op(xv6_sb);
            return neg(ENOSPC);
        }
        bn += 1;
    }
    // SAFETY: `inode` is live.
    unsafe { (*inode).size = new_size };
    xv6fs_iupdate(ip);
    xv6fs_end_op(xv6_sb);

    0
}
