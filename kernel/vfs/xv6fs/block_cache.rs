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
//! This means [`ExtentRbOps`] genuinely is wired into
//! `xv6fs_block_cache.extent_tree.opts` and exercised by every insert; it
//! is not dead ABI ceremony.
//!
//! # Wave B free-fn -> associated-fn sweep
//!
//! Every free fn that used to live in this file is now an associated fn:
//! `impl FreeExtent` for the slab-backed `alloc`/`free` pair (was
//! `bcache_alloc_extent`/`bcache_free_extent`), `impl Xv6fsSuperblock` for
//! the `xv6_sb: *mut xv6fs_superblock` public API (`mark_free`/
//! `find_free_block`/`find_free_block_near`/`free_count`/`bcache_init`/
//! `bcache_destroy`, dropping the `xv6fs_bcache_` prefix -- `bcache_`
//! itself is kept on `init`/`destroy` to stay unambiguous on a shared
//! type), or an `impl Xv6fs` method (a ZST private to this file, same
//! convention as `truncate.rs`/`log.rs`) for the small `neg`/`mkdev`/
//! `sb_dev`/`bblock_ptr` helpers with no shared-type home -- `sb_dev` is
//! this file's own hardcoded-locally copy, kept private rather than
//! merged into `Xv6fsSuperblock` because `superblock.rs` already has its
//! own same-named `Xv6fsSuperblock::sb_dev` (a second inherent method of
//! that name would collide). The intrusive rb-tree wrappers
//! (`rb_first_node`/`rb_last_node`/`rb_next_node`/`rb_insert_color`/
//! `rb_delete_node_color`) and the slab facade (`slab_cache_init`/
//! `slab_alloc`/`slab_free`) stay FLOOR free fns -- thin C-ABI facades,
//! matching this crate's established precedent. The former `extern "C"`
//! rb-tree comparator callbacks (`extent_keys_cmp`/`extent_get_key`,
//! wired into `tree_opts.keys_cmp_fun`/`get_key_fun` as raw fn pointers)
//! are gone -- TRAIT-OPS moved their bodies verbatim into the
//! [`ExtentRbOps`] `RbOps` impl (see below), and the per-mount
//! `tree_opts` storage field they were wired into is deleted outright
//! (the ops are stateless, so one `'static EXTENT_RB_OPS` now serves
//! every mount). Every relocated body is byte-identical to its old
//! free-fn form; no raw-pointer first param became `&self`/`&T`.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings::{
    buf, free_extent, rb_node, rb_root, slab_cache_t, xv6fs_superblock, EINVAL, ENOSPC,
};
use crate::bintree::RbOps;
// N-R7d: the block-cache metadata is now a data-owning
// `SpinLock<BlockCacheInner>` (lock-owns-data). `spinlock_t` /
// `xv6fs_block_cache` are no longer named here -- the embedded lock lives
// inside the `SpinLock`, and the internal helpers operate on
// `*mut BlockCacheInner`.
use crate::sync::SpinLock;

// Wave B free-fn -> associated-fn sweep pulls in the superblock type: the
// `xv6fs_bcache_*` public-API family (first param `*mut xv6fs_superblock`)
// becomes `impl Xv6fsSuperblock` methods, following the same cross-file
// inherent-impl convention `truncate.rs`/`log.rs` already established for
// `Xv6fsSuperblock::balloc`/`Xv6fsSuperblock::begin_op` etc.
use super::superblock::Xv6fsSuperblock;

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
// Native `xv6fs_block_cache` — P3-N8 nativization (user directive:
// remove the C-compatible interfaces). `Xv6fsBlockCache` is the
// canonical native definition of `kernel/vfs/xv6fs/block_cache.h`'s
// `struct xv6fs_block_cache`: `build.rs` blocklists the bindgen
// emission and re-exports this type as
// `crate::bindings::xv6fs_block_cache` (facade `pub use`, N2 pattern).
//
// DERIVE DECISION (P3-N8): no derives. The bindgen emission had
// already (silently, via the P3-N2 struct-X quirk) lost Copy/Clone —
// flagged by N6 for a deliberate decision in this type's own wave.
// First principles agree with the established emission: it owns a live
// rb-tree of `free_extent`s, an embedded slab cache (`extent_cache`),
// and a spinlock, so bitwise duplication would corrupt the extent tree
// and double-own the allocator (the N1/N2 `tnode` NONCOPY precedent);
// no consumer `=`-copies or literal-constructs it (grep-verified — the
// only by-value use is the embed in the native `Xv6fsSuperblock`,
// zero-initialized in place). So: no derives, matching the
// boot-verified emission bit for bit.
//
// N-R7d (lock-owns-data) restructured this type: the inline `lock`
// (`spinlock_t`) and the metadata it guarded now live in a data-owning
// `SpinLock<BlockCacheInner>` (`cache`); the extent slab and the lock-free
// `initialized` flag stay lock-free siblings. The old bindgen
// `__bindgen_padding_0/1` are gone (the field list is no longer laid out to
// mirror the C emission byte-for-byte — this is an IN-MEMORY-only mount
// struct); the layout is instead pinned by the asserts below to keep the
// whole struct at the historical 1472 bytes (so the superblock stays 4096,
// the slab's one-page object cap). See the `Xv6fsBlockCache` type doc.
// ===========================================================================

/// The lock-protected in-memory block-cache metadata (N-R7d) — the
/// rb-tree of free extents plus its bookkeeping counters. This is
/// everything `bcache_*` mutates under the cache lock; it does **not**
/// include the `extent_cache` slab (its own independently-synchronised
/// object — see [`Xv6fsBlockCache`]) nor the lock-free `initialized`
/// lifecycle flag.
///
/// Stored as `SpinLock<BlockCacheInner>` inside [`Xv6fsBlockCache::cache`].
/// Every metadata touch runs under the guard; the intrusive rb-tree
/// list-node ops (`rb_insert_color`/`rb_delete_node_color`/`container_of`/
/// next/prev — the separate list keystone) still run on a raw
/// `*mut BlockCacheInner` **derived from the held guard**, so they are left
/// raw exactly as before (out of scope). The single-threaded mount-time
/// build ([`Xv6fsSuperblock::bcache_init`]) reaches this struct via
/// [`SpinLock::data_ptr`] instead of the guard, because it streams the
/// on-disk bitmap across sleeping `bread`/`brelse` and so cannot hold the
/// spinlock — the block-cache analogue of the log's single-threaded
/// recovery.
///
/// TRAIT-OPS: the old `tree_opts` field (an embedded `rb_root_opts`
/// fn-pointer pair) is GONE -- [`ExtentRbOps`] below is a stateless ZST
/// (no per-mount data the comparator needs), so `extent_tree.opts` now
/// points directly at the single `'static EXTENT_RB_OPS`, and this struct
/// no longer needs to store its own copy of the ops table at all.
#[repr(C)]
pub struct BlockCacheInner {
    pub extent_tree: rb_root,
    pub nblocks: crate::bindings::uint32,
    pub data_start: crate::bindings::uint32,
    pub alloc_cursor: crate::bindings::uint32,
    pub free_count: crate::bindings::uint32,
    pub extent_count: crate::bindings::uint32,
}

// N-R7d interior-layout proof for the de-locked inner metadata (align 8,
// no align-64 member now that the slab lives outside — see below).
//
// TRAIT-OPS: `extent_tree` (`rb_root`) grew 16 -> 24 bytes (thin `opts`
// pointer -> `Option<&dyn RbOps>` fat pointer), but the `tree_opts` field
// (16 bytes) is deleted outright, so `BlockCacheInner` net-SHRINKS
// 56 -> 48 bytes; the outer `Xv6fsBlockCache::cache` (a
// `SpinLock<BlockCacheInner>`) simply gets a larger tail-padding gap
// before the align-64 `extent_cache` field, so `Xv6fsBlockCache`'s own
// size/offsets (1472 / 0 / 128 / 1408) are UNCHANGED -- see that
// struct's own assert block below.
const _: () = {
    assert!(core::mem::size_of::<BlockCacheInner>() == 48, "BlockCacheInner size");
    assert!(core::mem::align_of::<BlockCacheInner>() == 8, "BlockCacheInner alignment");
    assert!(core::mem::offset_of!(BlockCacheInner, extent_tree) == 0, "inner.extent_tree offset");
    assert!(core::mem::offset_of!(BlockCacheInner, nblocks) == 24, "inner.nblocks offset");
    assert!(core::mem::offset_of!(BlockCacheInner, data_start) == 28, "inner.data_start offset");
    assert!(core::mem::offset_of!(BlockCacheInner, alloc_cursor) == 32, "inner.alloc_cursor offset");
    assert!(core::mem::offset_of!(BlockCacheInner, free_count) == 36, "inner.free_count offset");
    assert!(core::mem::offset_of!(BlockCacheInner, extent_count) == 40, "inner.extent_count offset");
};

/// Native `struct xv6fs_block_cache` (`kernel/vfs/xv6fs/block_cache.h`)
/// — the per-mount free-block extent cache. N-R7d: **lock-owns-data.**
/// Was a plain struct with an inline `spinlock_t` (`lock`) hand-guarded by
/// the `bcache_*` entry points; now three parts:
///
/// * `cache` — the lock-owning [`SpinLock<BlockCacheInner>`](BlockCacheInner)
///   (rb-tree + bookkeeping, the state the cache lock actually guards);
/// * `extent_cache` — the extent slab, kept a **lock-free sibling** rather
///   than moved inside the `SpinLock`: the slab allocator is independently
///   synchronised by its own internal lock (the cache lock was only ever
///   held *around* `slab_alloc`/`slab_free` to serialise the accompanying
///   tree edit, never to protect the slab's own state), so it does not
///   belong to the lock-owned data. Keeping it out also keeps the align-64
///   slab from forcing 40 bytes of dead padding into the `SpinLock`, which
///   would push this struct — and the whole superblock — over the slab's
///   one-page object cap (`SLAB_OBJ_MAX_SIZE`);
/// * `initialized` — the lifecycle flag, read *without* the lock by the
///   fast-path guards here, by `__xv6fs_balloc` (`truncate.rs`), and by
///   `statfs` (`superblock.rs`): exactly the C original's lock-free flag
///   read. Set once at init, cleared at destroy; a plain aligned `c_int`,
///   so the lock-free read cannot tear.
///
/// This type is still re-exported as `crate::bindings::xv6fs_block_cache`
/// (the facade `pub use` in `kernel/bindings.rs`) and is the declared type
/// of [`Xv6fsSuperblock::block_cache`](super::superblock::Xv6fsSuperblock).
#[repr(C, align(64))]
pub struct Xv6fsBlockCache {
    /// Lock-guarded metadata (rb-tree + bookkeeping).
    pub cache: SpinLock<BlockCacheInner>,
    /// Extent slab — independently synchronised (see the type doc).
    pub extent_cache: slab_cache_t,
    /// Lock-free lifecycle flag (see the type doc).
    pub initialized: core::ffi::c_int,
}

// N-R7d layout proof. Ordering `cache` (the small align-8
// `SpinLock<BlockCacheInner>`) first, then the align-64 `extent_cache`,
// then `initialized`, reproduces the historical 1472-byte
// `xv6fs_block_cache` size EXACTLY -- so `block_cache` still lands at 2624
// and Xv6fsSuperblock stays at 4096 (mandatory: the `xv6fs_sb` slab caps
// objects at one page, so any growth would fail mount). The lock's
// 24-byte `spinlock_t` lives in the `SpinLock`'s own prefix; the 48-byte
// gap the align-64 `extent_cache` opens after `cache` is the same padding
// the C struct spent on `__bindgen_padding_0/1` + cacheline-aligning the
// old inline `lock`.
const _: () = {
    assert!(core::mem::size_of::<Xv6fsBlockCache>() == 1472, "xv6fs_block_cache size");
    assert!(core::mem::align_of::<Xv6fsBlockCache>() == 64, "xv6fs_block_cache alignment");
    assert!(core::mem::offset_of!(Xv6fsBlockCache, cache) == 0, "bc.cache offset");
    assert!(core::mem::offset_of!(Xv6fsBlockCache, extent_cache) == 128, "bc.extent_cache offset");
    assert!(core::mem::offset_of!(Xv6fsBlockCache, initialized) == 1408, "bc.initialized offset");
};

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

// P3-D3c: the rbtree/bintree primitives are genuinely `unsafe fn`s in
// `crate::{rbtree,bintree}` now that their `#[no_mangle]` exports are
// gone; this file's original extern declarations asserted `safe fn`
// (usual FFI-facade convention). Thin wrappers preserve that safe facade
// for the unchanged call sites.
//
// N-R7d: the hand-rolled `spin_init`/`spin_lock`/`spin_unlock` wrappers
// are gone — the cache lock is now embedded in `SpinLock<BlockCacheInner>`,
// so every acquire/release is the guard's `lock()`/`Drop`, and the
// diagnostic name is set once by `SpinLock::new`.
/// SAFETY: see [`crate::bintree::RbRoot::first_node`]'s contract.
fn rb_first_node(root: *mut rb_root) -> *mut rb_node {
    unsafe { crate::bintree::RbRoot::first_node(root) }
}
/// SAFETY: see [`crate::bintree::RbRoot::last_node`]'s contract.
fn rb_last_node(root: *mut rb_root) -> *mut rb_node {
    unsafe { crate::bintree::RbRoot::last_node(root) }
}
/// SAFETY: see [`crate::bintree::RbNode::next`]'s contract.
fn rb_next_node(node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::bintree::RbNode::next(node) }
}
/// SAFETY: see [`crate::rbtree::RbRoot::insert_color`]'s contract.
fn rb_insert_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::rbtree::RbRoot::insert_color(root, node) }
}
/// SAFETY: see [`crate::rbtree::RbRoot::delete_node_color`]'s contract.
fn rb_delete_node_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::rbtree::RbRoot::delete_node_color(root, node) }
}

// P3-D3c: `bufcache.rs`'s entry points are plain (safe) Rust fns now that
// their `#[no_mangle]` exports are gone; identical signatures, plain `use`.
use crate::bufcache::Buf;

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

/// ZST marker (Wave B free-fn -> associated-fn sweep, this file only) --
/// see the module doc. Holds the small helpers with no natural shared-type
/// home: `mkdev`/`sb_dev`/`bblock_ptr` are this file's own hardcoded-
/// locally copies (same rationale/precedent as `log.rs`'s and
/// `superblock.rs`'s own copies -- each file keeps a private duplicate
/// rather than sharing one `Xv6fsSuperblock::sb_dev`, since the scalar
/// helpers collide across files by name/signature otherwise, per
/// `truncate.rs`'s module doc).
struct Xv6fs;

impl Xv6fs {
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
    unsafe fn sb_dev(xv6_sb: *mut xv6fs_superblock) -> u32 {
        unsafe {
            let bdev = (*xv6_sb).blkdev;
            Xv6fs::mkdev((*bdev).dev.major, (*bdev).dev.minor)
        }
    }

    /// Mirrors `BBLOCK_PTR(b, sbp)` (`block_cache.c`, macro).
    #[inline(always)]
    fn bblock_ptr(b: u32, bmapstart: u32) -> u32 {
        b / super::BPB + bmapstart
    }
}

// ===========================================================================
// TRAIT-OPS: `RbOps` impl for the per-mount free-extent tree. Was the
// free-fn pair `extent_keys_cmp`/`extent_get_key` bound into the old
// per-instance `tree_opts` fn-pointer table; bodies moved verbatim
// (`extern "C"` dropped -- no longer a fn-pointer-table slot). Stateless
// -- shared by every mount via the single `'static EXTENT_RB_OPS` below.
// ===========================================================================

struct ExtentRbOps;
impl RbOps for ExtentRbOps {
    /// Mirrors `extent_keys_cmp()`. Compares two extent keys (both are
    /// addresses of live `free_extent`s, reinterpreted as `u64` -- see the
    /// module doc) by start block number, tie-broken by address.
    unsafe fn keys_cmp(&self, key1: u64, key2: u64) -> c_int {
        let ext1 = key1 as *const free_extent;
        let ext2 = key2 as *const free_extent;
        // SAFETY: both keys are addresses of live `free_extent`s (this
        // module's own `RbOps` callback contract -- every key that
        // reaches here originated from `get_key` or a live node being
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

    /// Mirrors `extent_get_key()`. Get key from rb_node -- returns the
    /// extent pointer as key. `rb_node` is the first field of
    /// `free_extent` (offset 0), so `node as u64` is numerically
    /// identical to `container_of(node, free_extent, rb_node) as u64`.
    unsafe fn get_key(&self, node: *mut rb_node) -> u64 {
        node as u64
    }
}

static EXTENT_RB_OPS: ExtentRbOps = ExtentRbOps;

// ===========================================================================
// Extent allocation helpers
// ===========================================================================

impl FreeExtent {
    /// # Safety
    /// `bc` must point to a live `BlockCacheInner`.
    unsafe fn alloc(extent_cache: *mut slab_cache_t) -> *mut free_extent {
        // SAFETY: `extent_cache` originates from `slab_cache_init` (its own
        // internal lock synchronises the allocation).
        let ext = slab_alloc(extent_cache) as *mut free_extent;
        if !ext.is_null() {
            // SAFETY: `ext` is a freshly allocated, exclusively-owned block.
            unsafe { ptr::write_bytes(ext as *mut u8, 0, core::mem::size_of::<free_extent>()) };
        }
        ext
    }

    fn free(ext: *mut free_extent) {
        // `slab_free` recovers the owning cache from the object header, so no
        // `extent_cache` handle is needed here.
        slab_free(ext as *mut c_void);
    }
}

// ===========================================================================
// Internal tree operations (caller must hold lock)
//
// N-METH sweep (goal #1): the five `bc: *mut BlockCacheInner` free fns
// became inherent methods on [`BlockCacheInner`]. The receiver is now a
// safe `&self`/`&mut self` reference -- reached directly through the held
// `SpinLock<BlockCacheInner>` guard (which `DerefMut`s to the inner), so the
// `&raw mut *cache` dance is gone at every public call site. `BlockCacheInner`
// is non-Freeze-relevant here because the guard grants exclusive access for
// the method's duration (no other hart can touch it without the lock) and no
// method sleeps, so no reference ever spans a yield. The intrusive rb-tree
// list-node ops (`rb_insert_color`/`rb_delete_node_color`/next + the
// left/right BST descents) stay RAW -- the separate list keystone, out of
// scope. `find_extent_*` became plain safe fns (the tree-well-formed type
// invariant discharges the node-walk unsafety); `insert_extent`/
// `alloc_from_extent` stay `unsafe fn` (raw `extent_cache`/`ext` preconditions).
// ===========================================================================

impl BlockCacheInner {
    /// Mirrors `bcache_find_extent_le()`. Find the extent with `start <=
    /// blockno` (floor search). Returns null if no such extent exists.
    fn find_extent_le(&self, blockno: u32) -> *mut free_extent {
        // SAFETY: `extent_tree`'s nodes are live `free_extent`s of a
        // well-formed tree (the type invariant, upheld by insert/alloc/
        // destroy) -- exactly the descent contract.
        unsafe {
            let mut node = self.extent_tree.node;
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
    fn find_extent_ge(&self, blockno: u32) -> *mut free_extent {
        // SAFETY: see `find_extent_le` (same tree-well-formed invariant).
        unsafe {
            let mut node = self.extent_tree.node;
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
    fn find_extent_containing(&self, blockno: u32) -> *mut free_extent {
        let ext = self.find_extent_le(blockno);
        // SAFETY: `ext` is null or a live extent returned above.
        if !ext.is_null() && blockno < unsafe { (*ext).start + (*ext).length } {
            return ext;
        }
        ptr::null_mut()
    }

    /// Mirrors `bcache_insert_extent()`. Insert a new extent into the tree,
    /// attempting to merge with adjacent extents.
    ///
    /// # Safety
    /// The cache lock must be held (guaranteed by `&mut self` from the
    /// guard, or the single-threaded mount build); `extent_cache` must be
    /// the live sibling extent slab.
    unsafe fn insert_extent(&mut self, extent_cache: *mut slab_cache_t, start: u32, length: u32) {
        unsafe {
            let end = start + length;

            // Check for merge with previous extent.
            let prev = self.find_extent_le(start);
            if !prev.is_null() && (*prev).start + (*prev).length == start {
                // Merge with previous: extend it.
                (*prev).length += length;
                self.free_count += length;

                // Check if we can also merge with next.
                let next_node = rb_next_node(ptr::addr_of_mut!((*prev).rb_node));
                if !next_node.is_null() {
                    let next = next_node as *mut free_extent;
                    if (*prev).start + (*prev).length == (*next).start {
                        // Merge all three into prev.
                        (*prev).length += (*next).length;
                        rb_delete_node_color(ptr::addr_of_mut!(self.extent_tree), ptr::addr_of_mut!((*next).rb_node));
                        FreeExtent::free(next);
                        self.extent_count -= 1;
                    }
                }
                return;
            }

            // Check for merge with next extent.
            let next = self.find_extent_ge(start);
            if !next.is_null() && end == (*next).start {
                // Merge with next: move its start back. Need to remove and
                // re-insert since the key changes.
                rb_delete_node_color(ptr::addr_of_mut!(self.extent_tree), ptr::addr_of_mut!((*next).rb_node));
                (*next).start = start;
                (*next).length += length;
                rb_insert_color(ptr::addr_of_mut!(self.extent_tree), ptr::addr_of_mut!((*next).rb_node));
                self.free_count += length;
                return;
            }

            // No merge possible, create new extent.
            let ext = FreeExtent::alloc(extent_cache);
            if ext.is_null() {
                // Out of memory -- silently fail (cache is optimization only).
                return;
            }

            (*ext).start = start;
            (*ext).length = length;
            rb_insert_color(ptr::addr_of_mut!(self.extent_tree), ptr::addr_of_mut!((*ext).rb_node));
            self.extent_count += 1;
            self.free_count += length;
        }
    }

    /// Mirrors `bcache_alloc_from_extent()`. Allocate one block from an
    /// extent, preferring the END for efficiency: allocating from the end
    /// only requires decrementing length (O(1)), whereas allocating from the
    /// start requires re-keying the tree (O(log n)).
    ///
    /// # Safety
    /// The cache lock must be held; `ext` must point to a live `free_extent`
    /// currently linked into this cache's tree.
    unsafe fn alloc_from_extent(&mut self, ext: *mut free_extent) -> u32 {
        unsafe {
            // Allocate from the end of the extent.
            let blockno = (*ext).start + (*ext).length - 1;

            if (*ext).length == 1 {
                // Remove entire extent.
                rb_delete_node_color(ptr::addr_of_mut!(self.extent_tree), ptr::addr_of_mut!((*ext).rb_node));
                FreeExtent::free(ext);
                self.extent_count -= 1;
            } else {
                // Simply shrink from the end -- no key change, O(1).
                (*ext).length -= 1;
            }

            self.free_count -= 1;
            blockno
        }
    }
}

// ===========================================================================
// Public API
// ===========================================================================

impl Xv6fsSuperblock {
    /// Mark a block as free in the cache.
    ///
    /// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
    /// declaration.
    pub(crate) extern "C" fn mark_free(xv6_sb: *mut xv6fs_superblock, blockno: u32) {
        // SAFETY: `xv6_sb` is live (caller's contract).
        unsafe {
            // `initialized` is the lock-free lifecycle flag (see
            // `Xv6fsBlockCache`); reading it without the lock matches the C
            // original and `__xv6fs_balloc`.
            if (*xv6_sb).block_cache.initialized == 0 {
                return;
            }
            let extent_cache = ptr::addr_of_mut!((*xv6_sb).block_cache.extent_cache);
            let mut cache = (*xv6_sb).block_cache.cache.lock();
            if blockno < cache.data_start {
                return; // guard drops -> release
            }
            // Insert through the guard (`DerefMut` -> `&mut BlockCacheInner`); the
            // sibling extent slab (computed before locking) rides in as a param.
            cache.insert_extent(extent_cache, blockno, 1);
        } // guard drops -> release
    }

    /// Find a free block using rb-tree search with wear leveling. O(log n)
    /// search starting from the allocation cursor.
    ///
    /// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
    /// declaration.
    pub(crate) extern "C" fn find_free_block(xv6_sb: *mut xv6fs_superblock, blockno_out: *mut u32) -> c_int {
        // SAFETY: `xv6_sb`/`blockno_out` are live (caller's contract).
        unsafe {
            if (*xv6_sb).block_cache.initialized == 0 {
                return Xv6fs::neg(EINVAL);
            }

            let mut cache = (*xv6_sb).block_cache.cache.lock();

            if cache.free_count == 0 || cache.extent_tree.node.is_null() {
                return Xv6fs::neg(ENOSPC); // guard drops -> release
            }

            // Find extent at or after cursor for wear leveling (methods run
            // through the guard; rb_first_node stays the raw list keystone).
            let cursor = cache.alloc_cursor;
            let mut ext = cache.find_extent_ge(cursor);

            // Wrap around if no extent found after cursor.
            if ext.is_null() {
                ext = rb_first_node(ptr::addr_of_mut!(cache.extent_tree)) as *mut free_extent;
            }
            // `ext` is guaranteed non-null since we checked the tree is not
            // empty above.

            *blockno_out = cache.alloc_from_extent(ext);
            cache.alloc_cursor = *blockno_out + 1;
            if cache.alloc_cursor >= cache.data_start + cache.nblocks {
                cache.alloc_cursor = cache.data_start;
            }

            0
        } // guard drops -> release
    }

    /// Find a free block near a hint block for better locality. Uses O(log n)
    /// rb-tree search -- no linear fallback paths.
    ///
    /// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
    /// declaration.
    pub(crate) extern "C" fn find_free_block_near(xv6_sb: *mut xv6fs_superblock, hint: u32, blockno_out: *mut u32) -> c_int {
        // SAFETY: `xv6_sb`/`blockno_out` are live (caller's contract).
        unsafe {
            if (*xv6_sb).block_cache.initialized == 0 {
                return Xv6fs::neg(EINVAL);
            }

            let mut cache = (*xv6_sb).block_cache.cache.lock();

            if cache.free_count == 0 || cache.extent_tree.node.is_null() {
                return Xv6fs::neg(ENOSPC); // guard drops -> release
            }

            // Tree search/alloc run through the guard (methods); rb_last_node
            // stays the raw list keystone. Every early `return` below drops the
            // guard (== the old `spin_unlock`).

            // Clamp hint to valid range.
            let mut hint = hint;
            if hint < cache.data_start {
                hint = cache.data_start;
            } else if hint >= cache.data_start + cache.nblocks {
                hint = cache.data_start + cache.nblocks - 1;
            }

            // Try to find extent containing the hint -- O(log n).
            let ext = cache.find_extent_containing(hint);
            if !ext.is_null() {
                *blockno_out = cache.alloc_from_extent(ext);
                return 0;
            }

            // Find extent at or after hint -- O(log n).
            let ext = cache.find_extent_ge(hint);
            if !ext.is_null() {
                *blockno_out = cache.alloc_from_extent(ext);
                return 0;
            }

            // No extent at/after hint means all extents are before hint. The
            // last (largest key) extent is closest to hint -- O(log n).
            let ext = rb_last_node(ptr::addr_of_mut!(cache.extent_tree)) as *mut free_extent;
            *blockno_out = cache.alloc_from_extent(ext);
            0
        } // guard drops -> release
    }

    /// Get the number of free blocks.
    ///
    /// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
    /// declaration.
    pub(crate) extern "C" fn free_count(xv6_sb: *mut xv6fs_superblock) -> u32 {
        // SAFETY: `xv6_sb` is live (caller's contract).
        unsafe {
            if (*xv6_sb).block_cache.initialized == 0 {
                return 0;
            }
            let cache = (*xv6_sb).block_cache.cache.lock();
            cache.free_count
        } // guard drops -> release
    }

    /// Initialize the block cache from the on-disk bitmap.
    ///
    /// Kept `#[no_mangle]`/exported per `block_cache.h`'s `extern`
    /// declaration.
    pub(crate) extern "C" fn bcache_init(xv6_sb: *mut xv6fs_superblock) -> c_int {
        // SAFETY: `xv6_sb` is live (caller's contract, mount-time setup with
        // no concurrent access yet).
        unsafe {
            let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);
            let dev = Xv6fs::sb_dev(xv6_sb);

            // Re-init guard: the lock-free lifecycle flag (mount zero-initialised
            // it to 0). Single-threaded here.
            if (*xv6_sb).block_cache.initialized != 0 {
                return 0;
            }

            // Calculate data blocks.
            let data_start = (*disk_sb).bmapstart + ((*disk_sb).size + super::BPB - 1) / super::BPB;
            if data_start > (*disk_sb).size {
                return Xv6fs::neg(EINVAL);
            }
            let nblocks = (*disk_sb).size - data_start;

            // Establish the *named* lock around a zeroed inner (console/log pilot
            // pattern). The mount path zero-initialised the whole superblock, so
            // the slot is an unnamed, unlocked `SpinLock` over a zeroed
            // `BlockCacheInner`; overwrite it with a properly *named* lock (the
            // name a deadlock panic prints). `SpinLock`/`BlockCacheInner` hold no
            // `Drop` types, so `ptr::write` correctly does not drop the previous
            // all-zero bytes.
            let cache_slot = ptr::addr_of_mut!((*xv6_sb).block_cache.cache);
            ptr::write(cache_slot, SpinLock::new(c"bcache", core::mem::zeroed::<BlockCacheInner>()));

            // Build the tree LOCK-FREE via `data_ptr()`: the bitmap scan streams
            // the disk across sleeping `bread`/`brelse`, so it cannot hold the
            // spinlock; mount is single-threaded (the superblock is not published
            // to the VFS until mount returns), so there is no concurrent accessor
            // -- the block-cache analogue of the log's single-threaded recovery.
            let bc: *mut BlockCacheInner = (*cache_slot).data_ptr();
            (*bc).nblocks = nblocks;
            (*bc).data_start = data_start;
            (*bc).alloc_cursor = data_start;
            (*bc).free_count = 0;
            (*bc).extent_count = 0;

            // Initialize the extent slab (the lock-free sibling object, not part
            // of the lock-owned inner).
            let extent_cache = ptr::addr_of_mut!((*xv6_sb).block_cache.extent_cache);
            slab_cache_init(
                extent_cache,
                c"bcache_extent".as_ptr(),
                core::mem::size_of::<free_extent>(),
                0,
            );

            // Initialize rb-tree.
            (*bc).extent_tree.node = ptr::null_mut();
            (*bc).extent_tree.opts = Some(&EXTENT_RB_OPS);

            // Scan on-disk bitmap and build extent tree.
            let mut last_bitmap_block: u32 = u32::MAX;
            let mut bp: *mut buf = ptr::null_mut();

            let mut run_start: u32 = 0;
            let mut run_length: u32 = 0;
            let mut in_run = false;

            // `nblocks` is fixed (computed once above) -> range walk. Loop
            // STRUCTURE only: the bitmap streaming across sleeping bread/brelse
            // and every bit-test byte are unchanged. `insert_extent` is called
            // via a transient `&mut *bc` (from the single-threaded mount
            // `data_ptr()`; it does not sleep, so the reference never spans a
            // yield).
            for b in 0..nblocks {
                let blockno = data_start + b;
                let bitmap_block = Xv6fs::bblock_ptr(blockno, (*disk_sb).bmapstart);

                if bitmap_block != last_bitmap_block {
                    if !bp.is_null() {
                        Buf::release(bp);
                    }
                    bp = Buf::read(dev, bitmap_block);
                    if bp.is_null() {
                        last_bitmap_block = u32::MAX;
                        // Treat read errors as used blocks.
                        if in_run {
                            (*bc).insert_extent(extent_cache, run_start, run_length);
                            in_run = false;
                        }
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
                    (*bc).insert_extent(extent_cache, run_start, run_length);
                    in_run = false;
                }
            }

            // Flush any remaining run.
            if in_run {
                (*bc).insert_extent(extent_cache, run_start, run_length);
            }

            if !bp.is_null() {
                Buf::release(bp);
            }

            // Publish: set the lock-free lifecycle flag last (after the tree is
            // fully built), so any later `initialized`-guarded reader sees a
            // complete cache. `initialized` is the outer sibling field, not part
            // of the guarded inner.
            (*xv6_sb).block_cache.initialized = 1;
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
    pub(crate) extern "C" fn bcache_destroy(xv6_sb: *mut xv6fs_superblock) {
        // SAFETY: `xv6_sb` is live (caller's contract).
        unsafe {
            if (*xv6_sb).block_cache.initialized == 0 {
                return;
            }

            {
                let mut cache = (*xv6_sb).block_cache.cache.lock();
                let bc = &raw mut *cache;

                // Free all extents (raw intrusive-rbtree teardown; `slab_free`
                // does not sleep, so it is fine under the spinlock).
                let mut node = rb_first_node(ptr::addr_of_mut!((*bc).extent_tree));
                while !node.is_null() {
                    let ext = node as *mut free_extent;
                    node = rb_next_node(node);
                    rb_delete_node_color(ptr::addr_of_mut!((*bc).extent_tree), ptr::addr_of_mut!((*ext).rb_node));
                    FreeExtent::free(ext);
                }

                (*bc).free_count = 0;
                (*bc).extent_count = 0;
            } // guard drops -> release

            // Clear the lock-free lifecycle flag after the tree teardown (and
            // after releasing the lock, matching the "publish last" ordering of
            // init); a racing `initialized`-guarded reader either bails or finds
            // an empty tree under the lock.
            (*xv6_sb).block_cache.initialized = 0;

            // Note: slab cache memory will be reclaimed automatically.
        }
    }
} // impl Xv6fsSuperblock
