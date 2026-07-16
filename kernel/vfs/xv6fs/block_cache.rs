//! xv6fs block allocation cache — Rust port of
//! `kernel/vfs/xv6fs/block_cache.c` (Phase 2 Wave 19, sub-wave B; see
//! `super` module doc and `docs/rustify/phase2_plan.md`).
//!
//! O(log n) free-block allocation using an rb-tree of free *extents*
//! (contiguous block runs), replacing a linear on-disk-bitmap scan.
//! Extents are merged when adjacent blocks are freed (both directions:
//! merge-with-previous, then check if that now also bridges to the
//! next extent, and merge-with-next when the previous extent doesn't
//! abut). Uses the Phase-2-Wave-1 Rust [`crate::rbtree`]/[`crate::bintree`]
//! port directly (`rb_insert_color`/`rb_delete_node_color`/
//! `rb_first_node`/`rb_next_node`/`rb_last_node`) — this is the second
//! real consumer of that port after `mm/vm.rs`'s VMA tree, and the first
//! whose tree is keyed by a filesystem-driver-private struct
//! ([`free_extent`]) rather than a core-kernel one.
//!
//! # Key insight: `rb_insert_color` does the BST insert too
//!
//! Unlike a typical intrusive-rbtree API where the caller links a node
//! into the tree by hand before calling the color-fixup pass,
//! `kernel/rbtree.rs`'s `rb_insert_color` (mirroring `kernel/rbtree.c`)
//! calls `bintree::rb_insert_node` internally to find the node's position
//! **by key** (via `root->opts->get_key_fun`/`keys_cmp_fun`) *and* link
//! it, before rebalancing. So [`bcache_insert_extent`]'s "no merge
//! possible" path can allocate a bare `free_extent`, set its
//! `start`/`length`, and call `rb_insert_color` directly — it does not
//! need to manually walk the tree to find an insertion point.
//! `rb_delete_node_color`, by contrast, uses the node's *existing*
//! parent-pointer chain (`__rb_node_link`), not a fresh key search, so it
//! needs no key-comparison setup at all.
//!
//! This means [`extent_keys_cmp`]/[`extent_get_key`] genuinely are wired
//! into `xv6fs_block_cache.tree_opts` and exercised by every insert; they
//! are not dead ABI ceremony.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings::{
    buf, free_extent, rb_node, rb_root, slab_cache_t, spinlock_t, xv6fs_block_cache, xv6fs_superblock, EINVAL, ENOSPC,
};

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N6 (mm type family, allocator-POD slice).
//
// This IS the kernel-wide Rust definition of `kernel/vfs/xv6fs/
// block_cache.h`'s `struct free_extent` now: `build.rs` blocklists the
// bindgen-generated form and injects a `pub use crate::vfs::xv6fs::
// block_cache::FreeExtent as free_extent;` facade re-export (no `_t`
// typedef exists), so this file's `free_extent` import above resolves
// right back here. Field names/types reproduce bindgen's exactly; the
// pre-nativization bindgen output derived NEITHER Copy nor Clone
// (intrusive `rb_node` embedder class, N1 precedent), so the native
// faithfully has no derives (accurate NONCOPY answer in build.rs's
// NativeTypeCallbacks — the still-bindgen `xv6fs_block_cache` only
// holds these behind pointers/the tree, never by value).
//
// Layout evidence (P3-N6): temporary in-tree `offset_of!` gate on the
// live bindgen form + cross-compiler `_Static_assert` probe (toolchain
// gcc, rv64gc/lp64d — scratchpad p3n6_static_assert_probe.c); the two
// agree on every value.
// ---------------------------------------------------------------------------

/// `struct free_extent` (`kernel/vfs/xv6fs/block_cache.h`) — one
/// contiguous range of free disk blocks, keyed by `start` in the
/// per-mount rb-tree.
#[repr(C)]
pub struct FreeExtent {
    pub rb_node: rb_node,
    pub start: crate::bindings::uint32,
    pub length: crate::bindings::uint32,
}

// P3-N6 hardcoded layout proof — gate + probe agree (see above).
const _: () = {
    assert!(core::mem::size_of::<FreeExtent>() == 32, "free_extent size");
    assert!(core::mem::align_of::<FreeExtent>() == 8, "free_extent align");
    assert!(core::mem::offset_of!(FreeExtent, rb_node) == 0, "free_extent.rb_node");
    assert!(core::mem::offset_of!(FreeExtent, start) == 24, "free_extent.start");
    assert!(core::mem::offset_of!(FreeExtent, length) == 28, "free_extent.length");
};

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

// P3-D3c: the spinlock and rbtree/bintree primitives are genuinely
// `unsafe fn`s in `crate::lock::spinlock`/`crate::{rbtree,bintree}` now
// that their `#[no_mangle]` exports are gone; this file's original extern
// declarations asserted `safe fn` (usual FFI-facade convention). Thin
// wrappers preserve that safe facade for the unchanged call sites.
/// SAFETY: see [`crate::lock::spinlock::spin_lock`]'s contract.
fn spin_lock(l: *mut spinlock_t) {
    unsafe { crate::lock::spinlock::spin_lock(l) }
}
/// SAFETY: see [`crate::lock::spinlock::spin_unlock`]'s contract.
fn spin_unlock(l: *mut spinlock_t) {
    unsafe { crate::lock::spinlock::spin_unlock(l) }
}
/// SAFETY: see [`crate::lock::spinlock::spin_init`]'s contract.
fn spin_init(l: *mut spinlock_t, name: *mut c_char) {
    unsafe { crate::lock::spinlock::spin_init(l, name) }
}
/// SAFETY: see [`crate::bintree::rb_first_node`]'s contract.
fn rb_first_node(root: *mut rb_root) -> *mut rb_node {
    unsafe { crate::bintree::rb_first_node(root) }
}
/// SAFETY: see [`crate::bintree::rb_last_node`]'s contract.
fn rb_last_node(root: *mut rb_root) -> *mut rb_node {
    unsafe { crate::bintree::rb_last_node(root) }
}
/// SAFETY: see [`crate::bintree::rb_next_node`]'s contract.
fn rb_next_node(node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::bintree::rb_next_node(node) }
}
/// SAFETY: see [`crate::rbtree::rb_insert_color`]'s contract.
fn rb_insert_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::rbtree::rb_insert_color(root, node) }
}
/// SAFETY: see [`crate::rbtree::rb_delete_node_color`]'s contract.
fn rb_delete_node_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::rbtree::rb_delete_node_color(root, node) }
}

// P3-D3c: `bufcache.rs`'s entry points are plain (safe) Rust fns now that
// their `#[no_mangle]` exports are gone; identical signatures, plain `use`.
use crate::bufcache::{bread, brelse};

// P3-D3a: the slab entry points are genuinely `unsafe fn` in
// `crate::mm::slab` now that their `#[no_mangle]` exports are gone; this
// file's original extern declarations asserted `safe fn` (usual FFI
// facade) and typed the cache pointer as the bindgen `slab_cache_t`
// rather than `crate::mm::slab::SlabCache` (same layout) and `name` as
// `*const c_char` rather than the real `*mut c_char` (the callee only
// reads it). Thin cast + safe-facade wrappers preserve both.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *const c_char, obj_size: usize, flags: u64,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(
            cache as *mut crate::mm::slab::SlabCache,
            name as *mut c_char,
            obj_size,
            flags,
        )
    }
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    unsafe { crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}
/// SAFETY: `obj` must originate from `slab_alloc` above.
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// Mirrors `mkdev(m, n)`/`xv6fs_sb_dev()` -- hardcoded locally, same
/// rationale/precedent as `superblock.rs`'s own copy.
#[inline(always)]
const fn mkdev(major: i32, minor: i32) -> u32 {
    ((major << 20) | minor) as u32
}
/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock` with a non-null
/// `blkdev`.
#[inline(always)]
unsafe fn xv6fs_sb_dev(xv6_sb: *mut xv6fs_superblock) -> u32 {
    unsafe {
        let bdev = (*xv6_sb).blkdev;
        mkdev((*bdev).dev.major, (*bdev).dev.minor)
    }
}

/// Mirrors `BBLOCK_PTR(b, sbp)` (`block_cache.c`, macro).
#[inline(always)]
fn bblock_ptr(b: u32, bmapstart: u32) -> u32 {
    b / super::BPB + bmapstart
}

// ===========================================================================
// Red-Black Tree callbacks for insertion/deletion
// ===========================================================================

/// Mirrors `extent_keys_cmp()`. Compares two extent keys (both are
/// addresses of live `free_extent`s, reinterpreted as `u64` -- see the
/// module doc) by start block number, tie-broken by address.
extern "C" fn extent_keys_cmp(key1: u64, key2: u64) -> c_int {
    let ext1 = key1 as *const free_extent;
    let ext2 = key2 as *const free_extent;
    // SAFETY: both keys are addresses of live `free_extent`s (this
    // module's own `rb_root_opts` callback contract -- every key that
    // reaches here originated from `extent_get_key` or a live node being
    // inserted).
    unsafe {
        if (*ext1).start < (*ext2).start {
            return -1;
        }
        if (*ext1).start > (*ext2).start {
            return 1;
        }
    }
    if key1 < key2 {
        -1
    } else if key1 > key2 {
        1
    } else {
        0
    }
}

/// Mirrors `extent_get_key()`. Get key from rb_node -- returns the extent
/// pointer as key. `rb_node` is the first field of `free_extent` (offset
/// 0), so `node as u64` is numerically identical to
/// `container_of(node, free_extent, rb_node) as u64`.
extern "C" fn extent_get_key(node: *mut rb_node) -> u64 {
    node as u64
}

// ===========================================================================
// Extent allocation helpers
// ===========================================================================

/// # Safety
/// `bc` must point to a live `xv6fs_block_cache`.
unsafe fn bcache_alloc_extent(bc: *mut xv6fs_block_cache) -> *mut free_extent {
    // SAFETY: `bc` is live.
    let ext = slab_alloc(unsafe { ptr::addr_of_mut!((*bc).extent_cache) }) as *mut free_extent;
    if !ext.is_null() {
        // SAFETY: `ext` is a freshly allocated, exclusively-owned block.
        unsafe { ptr::write_bytes(ext as *mut u8, 0, core::mem::size_of::<free_extent>()) };
    }
    ext
}

fn bcache_free_extent(_bc: *mut xv6fs_block_cache, ext: *mut free_extent) {
    slab_free(ext as *mut c_void);
}

// ===========================================================================
// Internal tree operations (caller must hold lock)
//
// Simple manual traversal for O(log n) searches.
// ===========================================================================

/// Mirrors `bcache_find_extent_le()`. Find the extent with `start <=
/// blockno` (floor search). Returns null if no such extent exists.
///
/// # Safety
/// `bc` must point to a live `xv6fs_block_cache`.
unsafe fn bcache_find_extent_le(bc: *mut xv6fs_block_cache, blockno: u32) -> *mut free_extent {
    // SAFETY: `bc` is live.
    unsafe {
        let mut node = (*bc).extent_tree.node;
        let mut best: *mut free_extent = ptr::null_mut();
        while !node.is_null() {
            let ext = node as *mut free_extent;
            if (*ext).start <= blockno {
                best = ext;
                node = (*node).right;
            } else {
                node = (*node).left;
            }
        }
        best
    }
}

/// Mirrors `bcache_find_extent_ge()`. Find the extent with `start >=
/// blockno` (ceiling search). Returns null if no such extent exists.
///
/// # Safety
/// `bc` must point to a live `xv6fs_block_cache`.
unsafe fn bcache_find_extent_ge(bc: *mut xv6fs_block_cache, blockno: u32) -> *mut free_extent {
    // SAFETY: `bc` is live.
    unsafe {
        let mut node = (*bc).extent_tree.node;
        let mut best: *mut free_extent = ptr::null_mut();
        while !node.is_null() {
            let ext = node as *mut free_extent;
            if (*ext).start >= blockno {
                best = ext;
                node = (*node).left;
            } else {
                node = (*node).right;
            }
        }
        best
    }
}

/// Mirrors `bcache_find_extent_containing()`.
///
/// # Safety
/// `bc` must point to a live `xv6fs_block_cache`.
unsafe fn bcache_find_extent_containing(bc: *mut xv6fs_block_cache, blockno: u32) -> *mut free_extent {
    // SAFETY: `bc` is live.
    let ext = unsafe { bcache_find_extent_le(bc, blockno) };
    // SAFETY: `ext` is either null or a live extent returned above.
    if !ext.is_null() && blockno < unsafe { (*ext).start + (*ext).length } {
        return ext;
    }
    ptr::null_mut()
}

/// Mirrors `bcache_insert_extent()`. Insert a new extent into the tree,
/// attempting to merge with adjacent extents.
///
/// # Safety
/// `bc` must point to a live `xv6fs_block_cache` with its lock held.
unsafe fn bcache_insert_extent(bc: *mut xv6fs_block_cache, start: u32, length: u32) {
    unsafe {
        let end = start + length;

        // Check for merge with previous extent.
        let prev = bcache_find_extent_le(bc, start);
        if !prev.is_null() && (*prev).start + (*prev).length == start {
            // Merge with previous: extend it.
            (*prev).length += length;
            (*bc).free_count += length;

            // Check if we can also merge with next.
            let next_node = rb_next_node(ptr::addr_of_mut!((*prev).rb_node));
            if !next_node.is_null() {
                let next = next_node as *mut free_extent;
                if (*prev).start + (*prev).length == (*next).start {
                    // Merge all three into prev.
                    (*prev).length += (*next).length;
                    rb_delete_node_color(ptr::addr_of_mut!((*bc).extent_tree), ptr::addr_of_mut!((*next).rb_node));
                    bcache_free_extent(bc, next);
                    (*bc).extent_count -= 1;
                }
            }
            return;
        }

        // Check for merge with next extent.
        let next = bcache_find_extent_ge(bc, start);
        if !next.is_null() && end == (*next).start {
            // Merge with next: move its start back. Need to remove and
            // re-insert since the key changes.
            rb_delete_node_color(ptr::addr_of_mut!((*bc).extent_tree), ptr::addr_of_mut!((*next).rb_node));
            (*next).start = start;
            (*next).length += length;
            rb_insert_color(ptr::addr_of_mut!((*bc).extent_tree), ptr::addr_of_mut!((*next).rb_node));
            (*bc).free_count += length;
            return;
        }

        // No merge possible, create new extent.
        let ext = bcache_alloc_extent(bc);
        if ext.is_null() {
            // Out of memory -- silently fail (cache is optimization only).
            return;
        }

        (*ext).start = start;
        (*ext).length = length;
        rb_insert_color(ptr::addr_of_mut!((*bc).extent_tree), ptr::addr_of_mut!((*ext).rb_node));
        (*bc).extent_count += 1;
        (*bc).free_count += length;
    }
}

/// Mirrors `bcache_alloc_from_extent()`. Allocate one block from an
/// extent, preferring the END for efficiency: allocating from the end
/// only requires decrementing length (O(1)), whereas allocating from the
/// start requires re-keying the tree (O(log n)).
///
/// # Safety
/// `bc`/`ext` must point to a live `xv6fs_block_cache`/`free_extent`
/// (the latter currently linked into `bc`'s tree), with `bc`'s lock held.
unsafe fn bcache_alloc_from_extent(bc: *mut xv6fs_block_cache, ext: *mut free_extent) -> u32 {
    unsafe {
        // Allocate from the end of the extent.
        let blockno = (*ext).start + (*ext).length - 1;

        if (*ext).length == 1 {
            // Remove entire extent.
            rb_delete_node_color(ptr::addr_of_mut!((*bc).extent_tree), ptr::addr_of_mut!((*ext).rb_node));
            bcache_free_extent(bc, ext);
            (*bc).extent_count -= 1;
        } else {
            // Simply shrink from the end -- no key change, O(1).
            (*ext).length -= 1;
        }

        (*bc).free_count -= 1;
        blockno
    }
}

// ===========================================================================
// Public API
// ===========================================================================

/// Mark a block as free in the cache.
///
/// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_bcache_mark_free(xv6_sb: *mut xv6fs_superblock, blockno: u32) {
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe {
        let bc = ptr::addr_of_mut!((*xv6_sb).block_cache);
        if (*bc).initialized == 0 || blockno < (*bc).data_start {
            return;
        }
        spin_lock(ptr::addr_of_mut!((*bc).lock));
        bcache_insert_extent(bc, blockno, 1);
        spin_unlock(ptr::addr_of_mut!((*bc).lock));
    }
}

/// Find a free block using rb-tree search with wear leveling. O(log n)
/// search starting from the allocation cursor.
///
/// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_bcache_find_free_block(xv6_sb: *mut xv6fs_superblock, blockno_out: *mut u32) -> c_int {
    // SAFETY: `xv6_sb`/`blockno_out` are live (caller's contract).
    unsafe {
        let bc = ptr::addr_of_mut!((*xv6_sb).block_cache);
        if (*bc).initialized == 0 {
            return neg(EINVAL);
        }

        spin_lock(ptr::addr_of_mut!((*bc).lock));

        if (*bc).free_count == 0 || (*bc).extent_tree.node.is_null() {
            spin_unlock(ptr::addr_of_mut!((*bc).lock));
            return neg(ENOSPC);
        }

        // Find extent at or after cursor for wear leveling.
        let mut ext = bcache_find_extent_ge(bc, (*bc).alloc_cursor);

        // Wrap around if no extent found after cursor.
        if ext.is_null() {
            ext = rb_first_node(ptr::addr_of_mut!((*bc).extent_tree)) as *mut free_extent;
        }
        // `ext` is guaranteed non-null since we checked the tree is not
        // empty above.

        *blockno_out = bcache_alloc_from_extent(bc, ext);
        (*bc).alloc_cursor = *blockno_out + 1;
        if (*bc).alloc_cursor >= (*bc).data_start + (*bc).nblocks {
            (*bc).alloc_cursor = (*bc).data_start;
        }

        spin_unlock(ptr::addr_of_mut!((*bc).lock));
        0
    }
}

/// Find a free block near a hint block for better locality. Uses O(log n)
/// rb-tree search -- no linear fallback paths.
///
/// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_bcache_find_free_block_near(xv6_sb: *mut xv6fs_superblock, hint: u32, blockno_out: *mut u32) -> c_int {
    // SAFETY: `xv6_sb`/`blockno_out` are live (caller's contract).
    unsafe {
        let bc = ptr::addr_of_mut!((*xv6_sb).block_cache);
        if (*bc).initialized == 0 {
            return neg(EINVAL);
        }

        spin_lock(ptr::addr_of_mut!((*bc).lock));

        if (*bc).free_count == 0 || (*bc).extent_tree.node.is_null() {
            spin_unlock(ptr::addr_of_mut!((*bc).lock));
            return neg(ENOSPC);
        }

        // Clamp hint to valid range.
        let mut hint = hint;
        if hint < (*bc).data_start {
            hint = (*bc).data_start;
        } else if hint >= (*bc).data_start + (*bc).nblocks {
            hint = (*bc).data_start + (*bc).nblocks - 1;
        }

        // Try to find extent containing the hint -- O(log n).
        let ext = bcache_find_extent_containing(bc, hint);
        if !ext.is_null() {
            *blockno_out = bcache_alloc_from_extent(bc, ext);
            spin_unlock(ptr::addr_of_mut!((*bc).lock));
            return 0;
        }

        // Find extent at or after hint -- O(log n).
        let ext = bcache_find_extent_ge(bc, hint);
        if !ext.is_null() {
            *blockno_out = bcache_alloc_from_extent(bc, ext);
            spin_unlock(ptr::addr_of_mut!((*bc).lock));
            return 0;
        }

        // No extent at/after hint means all extents are before hint. The
        // last (largest key) extent is closest to hint -- O(log n).
        let ext = rb_last_node(ptr::addr_of_mut!((*bc).extent_tree)) as *mut free_extent;
        *blockno_out = bcache_alloc_from_extent(bc, ext);
        spin_unlock(ptr::addr_of_mut!((*bc).lock));
        0
    }
}

/// Get the number of free blocks.
///
/// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_bcache_free_count(xv6_sb: *mut xv6fs_superblock) -> u32 {
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe {
        let bc = ptr::addr_of_mut!((*xv6_sb).block_cache);
        if (*bc).initialized == 0 {
            return 0;
        }
        spin_lock(ptr::addr_of_mut!((*bc).lock));
        let count = (*bc).free_count;
        spin_unlock(ptr::addr_of_mut!((*bc).lock));
        count
    }
}

/// Initialize the block cache from the on-disk bitmap.
///
/// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_bcache_init(xv6_sb: *mut xv6fs_superblock) -> c_int {
    // SAFETY: `xv6_sb` is live (caller's contract, mount-time setup with
    // no concurrent access yet).
    unsafe {
        let bc = ptr::addr_of_mut!((*xv6_sb).block_cache);
        let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);
        let dev = xv6fs_sb_dev(xv6_sb);

        if (*bc).initialized != 0 {
            return 0;
        }

        // Calculate data blocks.
        let data_start = (*disk_sb).bmapstart + ((*disk_sb).size + super::BPB - 1) / super::BPB;
        if data_start > (*disk_sb).size {
            return neg(EINVAL);
        }
        let nblocks = (*disk_sb).size - data_start;

        // Initialize basic fields.
        spin_init(ptr::addr_of_mut!((*bc).lock), c"bcache".as_ptr() as *mut c_char);
        (*bc).nblocks = nblocks;
        (*bc).data_start = data_start;
        (*bc).alloc_cursor = data_start;
        (*bc).free_count = 0;
        (*bc).extent_count = 0;

        // Initialize slab cache for extents.
        slab_cache_init(
            ptr::addr_of_mut!((*bc).extent_cache),
            c"bcache_extent".as_ptr(),
            core::mem::size_of::<free_extent>(),
            0,
        );

        // Initialize rb-tree.
        (*bc).tree_opts.keys_cmp_fun = Some(extent_keys_cmp);
        (*bc).tree_opts.get_key_fun = Some(extent_get_key);
        (*bc).extent_tree.node = ptr::null_mut();
        (*bc).extent_tree.opts = ptr::addr_of_mut!((*bc).tree_opts);

        // Scan on-disk bitmap and build extent tree.
        let mut last_bitmap_block: u32 = u32::MAX;
        let mut bp: *mut buf = ptr::null_mut();

        let mut run_start: u32 = 0;
        let mut run_length: u32 = 0;
        let mut in_run = false;

        let mut b: u32 = 0;
        while b < nblocks {
            let blockno = data_start + b;
            let bitmap_block = bblock_ptr(blockno, (*disk_sb).bmapstart);

            if bitmap_block != last_bitmap_block {
                if !bp.is_null() {
                    brelse(bp);
                }
                bp = bread(dev, bitmap_block);
                if bp.is_null() {
                    last_bitmap_block = u32::MAX;
                    // Treat read errors as used blocks.
                    if in_run {
                        bcache_insert_extent(bc, run_start, run_length);
                        in_run = false;
                    }
                    b += 1;
                    continue;
                }
                last_bitmap_block = bitmap_block;
            }

            let bi = (blockno % super::BPB) as usize;
            let m: u8 = 1 << (bi % 8);
            let used = (*(*bp).data.add(bi / 8) & m) != 0;

            if !used {
                // Block is free.
                if in_run {
                    run_length += 1;
                } else {
                    run_start = blockno;
                    run_length = 1;
                    in_run = true;
                }
            } else if in_run {
                // Block is used.
                bcache_insert_extent(bc, run_start, run_length);
                in_run = false;
            }

            b += 1;
        }

        // Flush any remaining run.
        if in_run {
            bcache_insert_extent(bc, run_start, run_length);
        }

        if !bp.is_null() {
            brelse(bp);
        }

        (*bc).initialized = 1;
        crate::kprintln!(
            "xv6fs: block cache initialized: {} data blocks, {} free in {} extents",
            nblocks as c_int,
            (*bc).free_count as c_int,
            (*bc).extent_count as c_int,
        );

        0
    }
}

/// Destroy the block cache and free memory.
///
/// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_bcache_destroy(xv6_sb: *mut xv6fs_superblock) {
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe {
        let bc = ptr::addr_of_mut!((*xv6_sb).block_cache);
        if (*bc).initialized == 0 {
            return;
        }

        spin_lock(ptr::addr_of_mut!((*bc).lock));

        // Free all extents.
        let mut node = rb_first_node(ptr::addr_of_mut!((*bc).extent_tree));
        while !node.is_null() {
            let ext = node as *mut free_extent;
            node = rb_next_node(node);
            rb_delete_node_color(ptr::addr_of_mut!((*bc).extent_tree), ptr::addr_of_mut!((*ext).rb_node));
            bcache_free_extent(bc, ext);
        }

        (*bc).initialized = 0;
        (*bc).free_count = 0;
        (*bc).extent_count = 0;

        spin_unlock(ptr::addr_of_mut!((*bc).lock));

        // Note: slab cache memory will be reclaimed automatically.
    }
}
