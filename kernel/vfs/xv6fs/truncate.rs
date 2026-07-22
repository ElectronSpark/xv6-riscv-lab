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
//!
//! # Wave A free-fn -> associated-fn sweep
//!
//! Every free fn that used to live in this file is now an associated fn:
//! `impl Xv6fsSuperblock` methods for the block-allocation/indirect-block
//! family (first param `*mut xv6fs_superblock`), `impl Xv6fsInode`
//! methods for the bmap/truncate family (first param `*mut xv6fs_inode`),
//! or an `impl Xv6fs` method for `xv6fs_truncate` (generic `*mut
//! vfs_inode`, dispatched through [`super::inode::Xv6fsInodeOps`]'s
//! `truncate`) and the small `bblock_ptr`/`neg` helpers with no natural
//! type home. `Xv6fs` here is a ZST private to this file — see
//! `superblock.rs`'s module doc for why each of this driver's three
//! Wave-A files gets its own (the scalar helpers collide across files by
//! name/signature otherwise). No floor items in this file: every function
//! ties to a concrete type or the local orchestration marker. Every
//! relocated body is byte-identical to its old free-fn form; no
//! raw-pointer first param became `&self`/`&T`.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_int, c_void};
use core::ptr;

use crate::bindings::{buf, loff_t, vfs_inode, xv6fs_inode, xv6fs_superblock};
use crate::kstd::{Errno, KResult};
use crate::proc::proc_shims::xv6_panic;

// Wave B free-fn -> associated-fn sweep: the block_cache/log public API is
// now `impl Xv6fsSuperblock` (see `block_cache.rs`'s/`log.rs`'s own module
// docs), reached below as `Xv6fsSuperblock::mark_free`/`find_free_block`/
// `find_free_block_near`/`begin_op`/`begin_op_nointr`/`end_op`/`log_write`.
use super::inode::Xv6fsInode;
use super::superblock::Xv6fsSuperblock;

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
use crate::bufcache::Buf;

/// ZST marker (Wave A free-fn -> associated-fn sweep, this file only) --
/// see the module doc. Private to this file except `truncate` itself,
/// which is `pub(crate)` because [`super::inode::Xv6fsInodeOps::truncate`]
/// calls it as `super::truncate::Xv6fs::truncate`.
pub(crate) struct Xv6fs;

impl Xv6fs {
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
}

// ===========================================================================
// Block allocation using cache
// ===========================================================================

impl Xv6fsSuperblock {
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
    unsafe fn balloc(xv6_sb: *mut xv6fs_superblock, dev: u32, hint: u32) -> u32 {
        unsafe {
            let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);
            let mut blockno: u32 = 0;

            // Try to find a free block using the cache.
            if (*xv6_sb).block_cache.initialized != 0 {
                let rc = if hint != 0 {
                    Xv6fsSuperblock::find_free_block_near(xv6_sb, hint, ptr::addr_of_mut!(blockno))
                } else {
                    Xv6fsSuperblock::find_free_block(xv6_sb, ptr::addr_of_mut!(blockno))
                };

                if rc == 0 {
                    // Block already removed from cache by find_free_block.

                    // Mark in on-disk bitmap.
                    let bp = Buf::read(dev, Xv6fs::bblock_ptr(blockno, (*disk_sb).bmapstart));
                    if bp.is_null() {
                        // Revert cache state on error.
                        Xv6fsSuperblock::mark_free(xv6_sb, blockno);
                        return 0;
                    }

                    let bi = (blockno % super::BPB) as usize;
                    let m: u8 = 1 << (bi % 8);
                    *(*bp).data.add(bi / 8) |= m;
                    Xv6fsSuperblock::log_write(xv6_sb, bp);
                    Buf::release(bp);

                    // Zero the block.
                    let zbp = Buf::read(dev, blockno);
                    if zbp.is_null() {
                        return 0;
                    }
                    memset((*zbp).data as *mut c_void, 0, super::BSIZE as usize);
                    Xv6fsSuperblock::log_write(xv6_sb, zbp);
                    Buf::release(zbp);

                    return blockno;
                }
            }

            0 // No free blocks.
        }
    }
}

// ===========================================================================
// Block mapping
// ===========================================================================

impl Xv6fsSuperblock {
    /// Mirrors `__xv6fs_bmap_ind()`. Get the disk block address for entry
    /// `bn` of an indirect block, allocating the indirect block and/or the
    /// data block if they don't exist. `hint`: last allocated block for
    /// locality, 0 if none.
    ///
    /// # Safety
    /// `xv6_sb` must point to a live `xv6fs_superblock` with a transaction
    /// active; `entry` must point to a live `uint` (either an inode's
    /// `addrs[]` slot or a slot within an indirect block's data).
    unsafe fn bmap_ind(xv6_sb: *mut xv6fs_superblock, entry: *mut u32, dev: u32, bn: u32, hint: u32) -> u32 {
        unsafe {
            if *entry == 0 {
                // Allocate indirect block using cache.
                let addr = Xv6fsSuperblock::balloc(xv6_sb, dev, hint);
                if addr == 0 {
                    return 0;
                }
                *entry = addr;
            }

            let bp = Buf::read(dev, *entry);
            if bp.is_null() {
                return 0;
            }

            let a = (*bp).data as *mut u32;
            let mut addr = *a.add(bn as usize);

            if addr == 0 {
                // Allocate data block using cache with locality hint.
                let locality_hint = *entry; // Use indirect block as hint for locality.
                addr = Xv6fsSuperblock::balloc(xv6_sb, dev, locality_hint);
                if addr == 0 {
                    Buf::release(bp);
                    return 0;
                }

                *a.add(bn as usize) = addr;
                Xv6fsSuperblock::log_write(xv6_sb, bp);
            }

            Buf::release(bp);
            addr
        }
    }
}

impl Xv6fsInode {
    /// Read-only bmap -- returns 0 if the block doesn't exist (for sparse
    /// files). Does not allocate any blocks.
    ///
    /// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
    /// declaration.
    pub(crate) extern "C" fn bmap_read(ip: *mut xv6fs_inode, mut bn: u32) -> u32 {
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
                let bp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize]);
                if bp.is_null() {
                    return 0;
                }
                let addr = *((*bp).data as *mut u32).add(bn as usize);
                Buf::release(bp);
                return addr;
            }
            bn -= super::NINDIRECT;

            // Double indirect block.
            if bn < super::NDINDIRECT {
                if (*ip).addrs[super::NDIRECT as usize + 1] == 0 {
                    return 0;
                }
                let bp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                if bp.is_null() {
                    return 0;
                }

                let l1_idx = bn / super::NINDIRECT;
                let l2_idx = bn % super::NINDIRECT;
                let a = (*bp).data as *mut u32;

                let l1_val = *a.add(l1_idx as usize);
                if l1_val == 0 {
                    Buf::release(bp);
                    return 0;
                }
                let l1_addr = l1_val;
                Buf::release(bp);

                let bp = Buf::read(dev, l1_addr);
                if bp.is_null() {
                    return 0;
                }
                let addr = *((*bp).data as *mut u32).add(l2_idx as usize);
                Buf::release(bp);
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
    pub(crate) extern "C" fn bmap(ip: *mut xv6fs_inode, mut bn: u32) -> u32 {
        // SAFETY: `ip` is live with a transaction active (caller's contract).
        unsafe {
            let xv6_sb = (*ip).vfs_inode.sb as *mut xv6fs_superblock;
            let dev = (*ip).dev;

            // Get hint for locality: use last allocated block if any.
            // (Goal #2) reverse scan of the direct slots -> `.iter().rev().find()`.
            // Snapshot the Copy `[u32; 13]` first (inode is locked here, so this
            // is a stable, behavior-identical copy — and it sidesteps taking a
            // reference into a raw-pointer deref).
            let addrs = (*ip).addrs;
            let hint = addrs[..super::NDIRECT as usize].iter().rev().copied().find(|&a| a != 0).unwrap_or(0);

            // Direct blocks.
            if bn < super::NDIRECT {
                let mut addr = (*ip).addrs[bn as usize];
                if addr == 0 {
                    // Allocate new block using cache with locality hint.
                    addr = Xv6fsSuperblock::balloc(xv6_sb, dev, hint);
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
                return Xv6fsSuperblock::bmap_ind(xv6_sb, ptr::addr_of_mut!((*ip).addrs[super::NDIRECT as usize]), dev, bn, hint);
            }
            bn -= super::NINDIRECT;

            // Double indirect block.
            if bn < super::NDINDIRECT {
                if (*ip).addrs[super::NDIRECT as usize + 1] == 0 {
                    // Allocate double indirect block using cache.
                    let addr = Xv6fsSuperblock::balloc(xv6_sb, dev, hint);
                    if addr == 0 {
                        return 0;
                    }
                    (*ip).addrs[super::NDIRECT as usize + 1] = addr;
                }

                let bp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                if bp.is_null() {
                    return 0;
                }

                let l1_idx = bn / super::NINDIRECT;
                let l2_idx = bn % super::NINDIRECT;
                let a = (*bp).data as *mut u32;

                let addr = Xv6fsSuperblock::bmap_ind(xv6_sb, a.add(l1_idx as usize), dev, l2_idx, (*ip).addrs[super::NDIRECT as usize + 1]);
                if *a.add(l1_idx as usize) != 0 {
                    Xv6fsSuperblock::log_write(xv6_sb, bp);
                }
                Buf::release(bp);
                return addr;
            }

            xv6_panic(c"xv6fs_bmap: block number too large".as_ptr());
        }
    }
}

// ===========================================================================
// Truncate
// ===========================================================================

impl Xv6fsSuperblock {
    /// Mirrors `__xv6fs_bfree()`. Free a block.
    ///
    /// # Safety
    /// `xv6_sb` must point to a live `xv6fs_superblock` with a transaction
    /// active.
    unsafe fn bfree(xv6_sb: *mut xv6fs_superblock, dev: u32, b: u32) {
        unsafe {
            let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);
            let bp = Buf::read(dev, Xv6fs::bblock_ptr(b, (*disk_sb).bmapstart));
            if bp.is_null() {
                // See `inode.rs`'s module doc "Fidelity note": the C original
                // does not NULL-check `Buf::read()` results uniformly either, but
                // an unconditional deref here would be Rust UB.
                xv6_panic(c"xv6fs_bfree: bread failed".as_ptr());
            }
            let bi = (b % super::BPB) as usize;
            let m: u8 = 1 << (bi % 8);
            if *(*bp).data.add(bi / 8) & m == 0 {
                xv6_panic(c"xv6fs_bfree: freeing free block".as_ptr());
            }
            *(*bp).data.add(bi / 8) &= !m;
            Xv6fsSuperblock::log_write(xv6_sb, bp);
            Buf::release(bp);

            // Update the block cache.
            Xv6fsSuperblock::mark_free(xv6_sb, b);
        }
    }

    /// Mirrors `__xv6fs_itrunc_ind_partial()`. Free indirect blocks starting
    /// from index `start_idx`. If `start_idx == 0`, frees the indirect block
    /// itself too. Returns the number of blocks freed.
    ///
    /// # Safety
    /// `xv6_sb` must point to a live `xv6fs_superblock` with a transaction
    /// active; `entry` must point to a live `uint` slot.
    unsafe fn itrunc_ind_partial(xv6_sb: *mut xv6fs_superblock, entry: *mut u32, dev: u32, start_idx: u32) -> i32 {
        unsafe {
            if *entry == 0 {
                return 0;
            }

            let bp = Buf::read(dev, *entry);
            if bp.is_null() {
                xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
            }
            let a = (*bp).data as *mut u32;
            let mut freed: i32 = 0;

            // (Goal #2) fixed-range indirect-slot walk -> range iterator.
            for j in start_idx..super::NINDIRECT {
                let v = *a.add(j as usize);
                if v != 0 {
                    Xv6fsSuperblock::bfree(xv6_sb, dev, v);
                    *a.add(j as usize) = 0;
                    freed += 1;
                }
            }

            if freed > 0 {
                Xv6fsSuperblock::log_write(xv6_sb, bp);
            }
            Buf::release(bp);

            // If we freed from the beginning, free the indirect block itself.
            if start_idx == 0 {
                Xv6fsSuperblock::bfree(xv6_sb, dev, *entry);
                *entry = 0;
                freed += 1;
            }

            freed
        }
    }

    /// Mirrors `__xv6fs_itrunc_ind()` (full version). Free indirect blocks.
    ///
    /// # Safety
    /// Same as [`Xv6fsSuperblock::itrunc_ind_partial`].
    unsafe fn itrunc_ind(xv6_sb: *mut xv6fs_superblock, entry: *mut u32, dev: u32) {
        unsafe { Xv6fsSuperblock::itrunc_ind_partial(xv6_sb, entry, dev, 0) };
    }
}

/// `ITRUNC_BATCH_SIZE` (`truncate.c`, `(MAXOPBLOCKS - 5) / 2`) -- maximum
/// blocks to free per transaction to stay within log limits. Each
/// `bfree` modifies the bitmap (1 write), so ~`MAXOPBLOCKS/2` blocks can
/// be freed per transaction.
const ITRUNC_BATCH_SIZE: i32 = (super::MAXOPBLOCKS - 5) / 2;

impl Xv6fsInode {
    /// Truncate inode `ip` to zero length, in batches to handle large files.
    ///
    /// IMPORTANT: caller must have called `xv6fs_begin_op` before calling
    /// this. This function may call `xv6fs_end_op`/`xv6fs_begin_op_nointr`
    /// internally to commit batches. When this function returns, a
    /// transaction is still active (caller should call `xv6fs_end_op`).
    ///
    /// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
    /// declaration.
    pub(crate) extern "C" fn itrunc(ip: *mut xv6fs_inode) {
        // SAFETY: `ip` is live with a transaction active (caller's contract).
        unsafe {
            let xv6_sb = (*ip).vfs_inode.sb as *mut xv6fs_superblock;
            let dev = (*ip).dev;
            let mut freed_this_batch: i32 = 0;

            // Free direct blocks. (Goal #2) direct-slot walk -> range iterator.
            for i in 0..super::NDIRECT as usize {
                if (*ip).addrs[i] != 0 {
                    Xv6fsSuperblock::bfree(xv6_sb, dev, (*ip).addrs[i]);
                    (*ip).addrs[i] = 0;
                    freed_this_batch += 1;

                    if freed_this_batch >= ITRUNC_BATCH_SIZE {
                        // Commit current batch and start new transaction.
                        Xv6fsInode::iupdate(ip);
                        Xv6fsSuperblock::end_op(xv6_sb);
                        Xv6fsSuperblock::begin_op_nointr(xv6_sb);
                        freed_this_batch = 0;
                    }
                }
            }

            // Free indirect blocks.
            if (*ip).addrs[super::NDIRECT as usize] != 0 {
                let mut bp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize]);
                if bp.is_null() {
                    xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                }
                let mut a = (*bp).data as *mut u32;

                // (Goal #2) single-indirect slot walk -> range iterator (the
                // batch-commit path re-`bread`s and re-binds `bp`/`a`).
                for j in 0..super::NINDIRECT {
                    let v = *a.add(j as usize);
                    if v != 0 {
                        Xv6fsSuperblock::bfree(xv6_sb, dev, v);
                        *a.add(j as usize) = 0;
                        freed_this_batch += 1;

                        if freed_this_batch >= ITRUNC_BATCH_SIZE {
                            Xv6fsSuperblock::log_write(xv6_sb, bp);
                            Buf::release(bp);
                            Xv6fsInode::iupdate(ip);
                            Xv6fsSuperblock::end_op(xv6_sb);
                            Xv6fsSuperblock::begin_op_nointr(xv6_sb);
                            freed_this_batch = 0;
                            bp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize]);
                            if bp.is_null() {
                                xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                            }
                            a = (*bp).data as *mut u32;
                        }
                    }
                }
                Xv6fsSuperblock::log_write(xv6_sb, bp);
                Buf::release(bp);
                Xv6fsSuperblock::bfree(xv6_sb, dev, (*ip).addrs[super::NDIRECT as usize]);
                (*ip).addrs[super::NDIRECT as usize] = 0;
                freed_this_batch += 1;

                if freed_this_batch >= ITRUNC_BATCH_SIZE {
                    Xv6fsInode::iupdate(ip);
                    Xv6fsSuperblock::end_op(xv6_sb);
                    Xv6fsSuperblock::begin_op_nointr(xv6_sb);
                    freed_this_batch = 0;
                }
            }

            // Free double indirect blocks.
            if (*ip).addrs[super::NDIRECT as usize + 1] != 0 {
                let mut dbp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                if dbp.is_null() {
                    xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                }
                let mut da = (*dbp).data as *mut u32;

                // (Goal #2) double-indirect L1 walk -> range iterator (batch
                // paths re-`bread` and re-bind `dbp`/`da`, and `bp`/`a` below).
                for j in 0..super::NINDIRECT {
                    let dv = *da.add(j as usize);
                    if dv != 0 {
                        let mut bp = Buf::read(dev, dv);
                        if bp.is_null() {
                            xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                        }
                        let mut a = (*bp).data as *mut u32;

                        // (Goal #2) L2 slot walk -> range iterator.
                        for k in 0..super::NINDIRECT {
                            let v = *a.add(k as usize);
                            if v != 0 {
                                Xv6fsSuperblock::bfree(xv6_sb, dev, v);
                                *a.add(k as usize) = 0;
                                freed_this_batch += 1;

                                if freed_this_batch >= ITRUNC_BATCH_SIZE {
                                    Xv6fsSuperblock::log_write(xv6_sb, bp);
                                    Buf::release(bp);
                                    Xv6fsSuperblock::log_write(xv6_sb, dbp);
                                    Buf::release(dbp);
                                    Xv6fsInode::iupdate(ip);
                                    Xv6fsSuperblock::end_op(xv6_sb);
                                    Xv6fsSuperblock::begin_op_nointr(xv6_sb);
                                    freed_this_batch = 0;
                                    dbp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                                    if dbp.is_null() {
                                        xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                                    }
                                    da = (*dbp).data as *mut u32;
                                    bp = Buf::read(dev, *da.add(j as usize));
                                    if bp.is_null() {
                                        xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                                    }
                                    a = (*bp).data as *mut u32;
                                }
                            }
                        }
                        Xv6fsSuperblock::log_write(xv6_sb, bp);
                        Buf::release(bp);
                        Xv6fsSuperblock::bfree(xv6_sb, dev, *da.add(j as usize));
                        *da.add(j as usize) = 0;
                        freed_this_batch += 1;

                        if freed_this_batch >= ITRUNC_BATCH_SIZE {
                            Xv6fsSuperblock::log_write(xv6_sb, dbp);
                            Buf::release(dbp);
                            Xv6fsInode::iupdate(ip);
                            Xv6fsSuperblock::end_op(xv6_sb);
                            Xv6fsSuperblock::begin_op_nointr(xv6_sb);
                            freed_this_batch = 0;
                            dbp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                            if dbp.is_null() {
                                xv6_panic(c"xv6fs_itrunc: bread failed".as_ptr());
                            }
                            da = (*dbp).data as *mut u32;
                        }
                    }
                }
                Xv6fsSuperblock::log_write(xv6_sb, dbp);
                Buf::release(dbp);
                Xv6fsSuperblock::bfree(xv6_sb, dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                (*ip).addrs[super::NDIRECT as usize + 1] = 0;
            }

            (*ip).vfs_inode.size = 0;
            Xv6fsInode::iupdate(ip);
            // Note: transaction is still active -- caller must call xv6fs_end_op.
        }
    }

    /// Mirrors `__xv6fs_truncate_partial()`. Partial truncate: free blocks
    /// from `first_block` to end of file, in small batches to stay within
    /// transaction limits.
    ///
    /// # Safety
    /// `ip` must point to a live `xv6fs_inode` with a transaction active.
    unsafe fn truncate_partial(ip: *mut xv6fs_inode, first_block: u32) -> c_int {
        unsafe {
            let xv6_sb = (*ip).vfs_inode.sb as *mut xv6fs_superblock;
            let dev = (*ip).dev;

            // Free direct blocks from first_block onwards.
            // (Goal #2) direct-slot walk -> range iterator.
            for i in first_block..super::NDIRECT {
                if (*ip).addrs[i as usize] != 0 {
                    Xv6fsSuperblock::bfree(xv6_sb, dev, (*ip).addrs[i as usize]);
                    (*ip).addrs[i as usize] = 0;
                }
            }

            // Handle indirect blocks.
            if first_block <= super::NDIRECT {
                // All indirect blocks need to be freed.
                Xv6fsSuperblock::itrunc_ind(xv6_sb, ptr::addr_of_mut!((*ip).addrs[super::NDIRECT as usize]), dev);
            } else if first_block < super::NDIRECT + super::NINDIRECT {
                // Partial indirect block freeing.
                let ind_start = first_block - super::NDIRECT;
                Xv6fsSuperblock::itrunc_ind_partial(xv6_sb, ptr::addr_of_mut!((*ip).addrs[super::NDIRECT as usize]), dev, ind_start);
            }

            // Handle double indirect blocks.
            let dind_threshold = super::NDIRECT + super::NINDIRECT;
            if first_block <= dind_threshold {
                // All double indirect blocks need to be freed.
                if (*ip).addrs[super::NDIRECT as usize + 1] != 0 {
                    let bp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                    if bp.is_null() {
                        xv6_panic(c"xv6fs_truncate: bread failed".as_ptr());
                    }
                    let a = (*bp).data as *mut u32;

                    // (Goal #2) L1 walk (free every L2 block) -> range iterator.
                    for j in 0..super::NINDIRECT {
                        let v = *a.add(j as usize);
                        if v != 0 {
                            Xv6fsSuperblock::itrunc_ind(xv6_sb, a.add(j as usize), dev);
                        }
                    }
                    Buf::release(bp);
                    Xv6fsSuperblock::bfree(xv6_sb, dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                    (*ip).addrs[super::NDIRECT as usize + 1] = 0;
                }
            } else if first_block < dind_threshold + super::NDINDIRECT {
                // Partial double indirect freeing.
                if (*ip).addrs[super::NDIRECT as usize + 1] != 0 {
                    let rel_block = first_block - dind_threshold;
                    let mut l1_start = rel_block / super::NINDIRECT;
                    let l2_start = rel_block % super::NINDIRECT;

                    let bp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                    if bp.is_null() {
                        xv6_panic(c"xv6fs_truncate: bread failed".as_ptr());
                    }
                    let a = (*bp).data as *mut u32;
                    let mut modified = false;

                    // Handle partial first L1 entry.
                    if l2_start > 0 && *a.add(l1_start as usize) != 0 {
                        Xv6fsSuperblock::itrunc_ind_partial(xv6_sb, a.add(l1_start as usize), dev, l2_start);
                        l1_start += 1;
                    }

                    // Free remaining L1 entries completely.
                    // (Goal #2) remaining-L1 walk -> range iterator.
                    for j in l1_start..super::NINDIRECT {
                        let v = *a.add(j as usize);
                        if v != 0 {
                            Xv6fsSuperblock::itrunc_ind(xv6_sb, a.add(j as usize), dev);
                            modified = true;
                        }
                    }

                    if modified {
                        Xv6fsSuperblock::log_write(xv6_sb, bp);
                    }
                    Buf::release(bp);

                    // Check if all L1 entries are now zero; free the dind block.
                    let bp = Buf::read(dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                    if bp.is_null() {
                        xv6_panic(c"xv6fs_truncate: bread failed".as_ptr());
                    }
                    let a = (*bp).data as *mut u32;
                    // (Goal #2) all-L1-entries-zero scan -> `.any()`.
                    let all_zero = !(0..super::NINDIRECT).any(|j| *a.add(j as usize) != 0);
                    Buf::release(bp);

                    if all_zero {
                        Xv6fsSuperblock::bfree(xv6_sb, dev, (*ip).addrs[super::NDIRECT as usize + 1]);
                        (*ip).addrs[super::NDIRECT as usize + 1] = 0;
                    }
                }
            }

            0
        }
    }
}

impl Xv6fs {
    /// Mirrors `xv6fs_truncate()` (P3-10b: `KResult`-native, reached
    /// through [`super::inode::Xv6fsInodeOps`]'s `truncate` — the old
    /// `extern "C"` C-ABI spelling is gone with the fn-pointer table).
    /// `xv6fs_begin_op` failures (a `c_int` boundary this driver's log
    /// layer owns; `-EINTR` today) pass through as `Errno::Raw`.
    pub(crate) fn truncate(inode: *mut vfs_inode, new_size: loff_t) -> KResult<()> {
        if inode.is_null() {
            return Err(Errno::Inval);
        }
        if new_size < 0 {
            return Err(Errno::Inval);
        }

        // Check max file size (MAXFILE blocks).
        if new_size > super::MAXFILE as loff_t * super::BSIZE as loff_t {
            return Err(Errno::FBig);
        }

        let ip = inode as *mut xv6fs_inode;
        // SAFETY: `inode` is live (caller's contract).
        let xv6_sb = unsafe { (*inode).sb as *mut xv6fs_superblock };
        let old_size = unsafe { (*inode).size };

        if new_size == old_size {
            return Ok(()); // No change needed.
        }

        if new_size == 0 {
            // Full truncation -- use the optimized path.
            let ret = Xv6fsSuperblock::begin_op(xv6_sb);
            if ret != 0 {
                return Err(Errno::Raw(ret));
            }
            Xv6fsInode::itrunc(ip);
            Xv6fsSuperblock::end_op(xv6_sb);
            return Ok(());
        }

        if new_size < old_size {
            // Shrinking file -- free blocks beyond new size. Calculate the
            // first block to free (the block containing the byte at
            // new_size). If new_size is block-aligned, start from that
            // block; otherwise keep the partial block and free from the next
            // block.
            let first_block = (new_size as u32 + super::BSIZE - 1) / super::BSIZE;

            let ret = Xv6fsSuperblock::begin_op(xv6_sb);
            if ret != 0 {
                return Err(Errno::Raw(ret));
            }
            // SAFETY: `ip` is live with a transaction now active.
            unsafe {
                Xv6fsInode::truncate_partial(ip, first_block);
                (*inode).size = new_size;
            }
            Xv6fsInode::iupdate(ip);
            Xv6fsSuperblock::end_op(xv6_sb);
            return Ok(());
        }

        // Extending file -- allocate blocks as needed.
        let old_blocks = (old_size as u32 + super::BSIZE - 1) / super::BSIZE;
        let new_blocks = (new_size as u32 + super::BSIZE - 1) / super::BSIZE;

        let ret = Xv6fsSuperblock::begin_op(xv6_sb);
        if ret != 0 {
            return Err(Errno::Raw(ret));
        }
        // (Goal #2) block-extend walk -> range iterator.
        for bn in old_blocks..new_blocks {
            if Xv6fsInode::bmap(ip, bn) == 0 {
                Xv6fsSuperblock::end_op(xv6_sb);
                return Err(Errno::NoSpc);
            }
        }
        // SAFETY: `inode` is live.
        unsafe { (*inode).size = new_size };
        Xv6fsInode::iupdate(ip);
        Xv6fsSuperblock::end_op(xv6_sb);

        Ok(())
    }
}
