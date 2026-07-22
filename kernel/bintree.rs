//! Intrusive binary search tree — Rust port of `kernel/bintree.c`.
//!
//! `struct rb_node` / `struct rb_root` are the real
//! `crate::bindings::{rb_node, rb_root}` types — no opaque stand-ins.
//! (The historical `struct rb_root_opts` fn-pointer table is gone —
//! TRAIT-OPS replaced it with the [`RbOps`] trait below, storing
//! `Option<&'static dyn RbOps>` directly in [`RawRbRoot::opts`].)
//!
//! This module implements the *uncolored* binary-search-tree operations
//! (link/unlink, rotation, in-order successor/predecessor, key lookup).
//! [`crate::rbtree`] builds red-black balancing on top of it, exactly as
//! `kernel/rbtree.c` builds on `kernel/bintree.c` in C.
//!
//! `kernel/inc/bintree.h` keeps its `static inline` helpers
//! (`rb_parent`, `rb_set_parent`, `rb_node_init`, `rb_root_init`,
//! `__rb_link_nodes`, `__rb_delink_node`, ...) for the remaining C
//! consumers (`dev/fdt.c`, `vfs/xv6fs/block_cache.c`, `timer/timer.c`,
//! ...) — those have no external linkage, so they cannot be called from
//! Rust and are reimplemented natively below as private
//! (`pub(crate)` where `rbtree.rs` also needs them) helpers.
//!
//! `__find_replacement_for_deletion` (a `static inline` helper in the
//! original `.c` file) and `__hlist_calc_node_bucket`-style dead code
//! are dropped: neither is called anywhere in the tree (verified by
//! grep across every `.c`/`.h`/`.rs` file) and neither has a header
//! declaration, so there is no ABI to preserve.

#![allow(non_camel_case_types)]

use core::ptr;

use crate::bindings::{rb_node, rb_root};

pub type RbNode = rb_node;
pub type RbRoot = rb_root;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3C, nativized in Wave P3-N1.
//
// `RawRbNode`/`RawRbRoot` ARE the kernel-wide Rust definitions of
// `kernel/inc/bintree_type.h`'s `struct rb_node` / `struct rb_root` now:
// `kernel/bindings.rs` re-exports `pub type rb_* = crate::bintree::RawRb*;`,
// so every remaining struct that embeds one (`vma.rb_entry`,
// `timer_node`/`timer_root`, `tnode`, `pcache_node`, ...) and every
// `crate::bindings::rb_*` path across the ~15 consumer files (mm's
// `pcache.rs`/`vm.rs`, proc's `thread_queue.rs`/`access.rs`,
// `timer/timer_core.rs`, `vfs/xv6fs/block_cache.rs`, `backtrace.rs`,
// `dev/fdt.rs`, ...) resolves here and compiles unchanged. The
// `RbNode`/`RbRoot` aliases above likewise keep resolving through
// `crate::bindings`. Layout is proven by the hardcoded `const _` asserts
// below.
//
// TRAIT-OPS: `struct rb_root_opts` (2 function pointers, 16 bytes) is
// GONE -- replaced by the [`RbOps`] trait below -- so `RawRbRoot::opts`
// is now a 16-byte `Option<&'static dyn RbOps>` fat pointer instead of an
// 8-byte thin `*mut RawRbRootOpts`; `RawRbRoot` grows 16 -> 24 bytes.
// Every embedder (`Pcache::page_map`, `Vm::vm_tree`, `Ttree::root`,
// `TimerRoot::root`, `FdtNode::children`/`FdtBlobInfo::root`,
// `BlockCacheInner::extent_tree`) feels this growth in its OWN following-
// field offsets -- each file's own layout-assert block documents its
// honest new numbers (see `mm/pcache.rs`/`mm/vm.rs`/
// `proc/thread_queue.rs`/`timer/timer_core.rs`/
// `vfs/xv6fs/block_cache.rs`; `dev/fdt.rs`'s `FdtNode`/`FdtBlobInfo` have
// no hardcoded offset asserts to update).
//
// Deliberately NOT wired into this file's own algorithm bodies today:
// unlike `RawSpinlock` (which replaced ad-hoc `as *const AtomicU32`-style
// reinterpret casts with one canonical cast point -- a genuine
// unsafe-reduction win), `rb_parent`/`rb_left`/`rb_right`/etc. below
// already do plain, correctly-typed field access on `RbNode` with no
// casting to simplify. Rewiring their signatures to `RawRbNode` would
// only relocate work (every one of the ~15 cross-module + `rbtree.rs`
// call sites would need a matching cast) for zero unsafe-reduction
// benefit, on the exact code path `rbtree.rs`'s own module doc calls
// "easy to subtly break by improving it" and "on the hot path" for
// `mm/vm.rs`/`mm/pcache.rs`/`proc/thread_queue.rs`. Left untouched by
// this wave; the layout proof below is the deliverable.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct RawRbNode {
    pub __parent_color: u64,
    pub left: *mut RawRbNode,
    pub right: *mut RawRbNode,
}

// ---------------------------------------------------------------------------
// `RbOps` — TRAIT-OPS conversion of the old `rb_root_opts` fn-pointer
// table (2 raw `Option<unsafe extern "C" fn(...)>` slots) into a single
// trait, `HlistOps`/`PcacheOps` precedent.
//
// None-semantics: BOTH methods are REQUIRED (no defaulted body). Evidence
// from every call site below: `RbRoot::is_initialized()` (the sole gate
// `find_key_link`/`find_key_rup`/`find_key_rdown`/`find_key`/
// `insert_node`/`delete_key` all run before touching `opts`) rejected a
// root unless `keys_cmp_fun`/`get_key_fun` were BOTH `Some`. No table
// anywhere in the tree (grep-confirmed: `mm/pcache.rs`, `mm/vm.rs`,
// `backtrace.rs` x2, `timer/timer_core.rs`, `proc/thread_queue.rs` x2,
// `dev/fdt.rs`, `vfs/xv6fs/block_cache.rs`) ever populated only one of the
// two -- it was always "both or none". So the whole table collapses to a
// single `Option<&'static dyn RbOps>` on [`RawRbRoot`]: `None` reproduces
// the old all-`None`/null-`opts` (uninitialized) state exactly, and
// `Some` reproduces the old all-`Some` (post-init) state exactly.
pub trait RbOps: Sync {
    /// Three-way key comparison (`0` iff equal, `< 0` iff `key1 < key2`,
    /// `> 0` otherwise) — the tree's total order. Mirrors the old
    /// `opts.keys_cmp_fun` slot.
    ///
    /// # Safety
    /// `key1`/`key2` are opaque `u64` keys per this tree's own key
    /// convention (often a node address, sometimes a plain integer field)
    /// -- implementors that reinterpret a key as a pointer must uphold
    /// their own contract (documented per impl) that it is a live node.
    unsafe fn keys_cmp(&self, key1: u64, key2: u64) -> core::ffi::c_int;

    /// Extract a node's key. Mirrors the old `opts.get_key_fun` slot.
    ///
    /// # Safety
    /// `node` must be a live [`RawRbNode`] currently linked into (or
    /// about to be linked into) the tree this ops table is installed on.
    unsafe fn get_key(&self, node: *mut RawRbNode) -> u64;
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RawRbRoot {
    pub node: *mut RawRbNode,
    /// The tree's dispatch table -- a real Rust trait object,
    /// `Option<&'static dyn RbOps>` (a 16-byte fat pointer; `None` is the
    /// pre-init / zeroed placeholder state, matching the old all-`None`
    /// `opts` value bit-for-bit: `Option<&dyn Trait>` niches on the data
    /// pointer being null, so an all-zero `RawRbRoot` still reads back
    /// `opts == None`, exactly like the old null `*mut RawRbRootOpts` did
    /// for e.g. `backtrace.rs`'s `static mut KSYM_RB_ROOT` before
    /// `ksymbols_init` runs). `Option<&'static dyn RbOps>` is itself
    /// `Copy`/`Clone` (shared references always are), so `#[derive(Copy,
    /// Clone)]` above keeps working unchanged -- every existing by-value
    /// copy site (`proc/thread_queue.rs`'s `dummy_root: rb_root =
    /// self.root_copy()`, `backtrace.rs`'s `let mut search_root =
    /// KSYM_RB_ROOT`) compiles as before.
    pub opts: Option<&'static dyn RbOps>,
}

// P3-N1 hardcoded layout proof — values captured from the pre-nativization
// bindgen output (kernel_bindings.rs) for `kernel/inc/bintree_type.h`'s
// `struct rb_node` (`__attribute__((aligned(8)))` == natural alignment:
// u64 + 2 pointers) and `struct rb_root` (was 2 pointers; TRAIT-OPS grows
// `opts` from an 8-byte thin pointer to a 16-byte `Option<&dyn RbOps>` fat
// pointer, so `rb_root` itself grows 16 -> 24 bytes). All 8-byte members,
// no padding, on riscv64/lp64d.
const _: () = {
    assert!(core::mem::size_of::<RawRbNode>() == 24, "rb_node: u64 + 2 x 8-byte pointer");
    assert!(core::mem::align_of::<RawRbNode>() == 8, "rb_node: aligned(8)");
    assert!(core::mem::offset_of!(RawRbNode, __parent_color) == 0, "rb_node.__parent_color offset");
    assert!(core::mem::offset_of!(RawRbNode, left) == 8, "rb_node.left offset");
    assert!(core::mem::offset_of!(RawRbNode, right) == 16, "rb_node.right offset");

    assert!(core::mem::size_of::<Option<&'static dyn RbOps>>() == 16, "rb-tree ops fat pointer size");
    assert!(core::mem::align_of::<Option<&'static dyn RbOps>>() == 8, "rb-tree ops fat pointer alignment");

    assert!(core::mem::size_of::<RawRbRoot>() == 24, "rb_root: 8-byte pointer + 16-byte ops fat pointer");
    assert!(core::mem::align_of::<RawRbRoot>() == 8, "rb_root: natural pointer alignment");
    assert!(core::mem::offset_of!(RawRbRoot, node) == 0, "rb_root.node offset");
    assert!(core::mem::offset_of!(RawRbRoot, opts) == 8, "rb_root.opts offset");
};

/// Low bits of `__parent_color` reserved for out-of-band data (only bit 0
/// — the red/black color — is used today, by `kernel/rbtree.c`). Mirrors
/// `_RB_COLOR_MASK` in `kernel/inc/bintree.h`.
const RB_COLOR_MASK: u64 = 7;

// ---------------------------------------------------------------------------
// Private re-implementations of `kernel/inc/bintree.h`'s `static inline`
// helpers. `pub(crate)` where `kernel/rbtree.rs` needs the same primitive
// (mirrors both `.c` files independently `#include`-ing the same header).
//
// KERNEL-OO: relocated onto `impl RbNode`/`impl RbRoot` (raw params, no
// `&self` — `RbNode`/`RbRoot` are `Freeze`, intrusively linked, and
// mutated cross-hart under external locking; the doc-preserved contracts
// below are unchanged).
// ---------------------------------------------------------------------------

impl RbNode {
    /// Mirrors `rb_parent()`.
    ///
    /// # Safety
    /// `node` must be null or point to a live, properly initialised `rb_node`.
    #[inline(always)]
    pub(crate) unsafe fn parent(node: *mut RbNode) -> *mut RbNode {
        if node.is_null() {
            return ptr::null_mut();
        }
        // SAFETY: `node` is non-null and live per the caller contract.
        let pc = unsafe { (*node).__parent_color };
        (pc & !RB_COLOR_MASK) as *mut RbNode
    }

    /// Mirrors `rb_set_parent()`.
    ///
    /// # Safety
    /// `node` must point to a live, properly initialised `rb_node`; `parent`
    /// must be 8-byte aligned (guaranteed by `rb_node`'s `aligned(8)` layout)
    /// so the low 3 bits stay free for the color.
    #[inline(always)]
    pub(crate) unsafe fn set_parent(node: *mut RbNode, parent: *mut RbNode) {
        // SAFETY: `node` is non-null and live per the caller contract.
        unsafe {
            let mut pc = (*node).__parent_color;
            pc &= RB_COLOR_MASK;
            pc |= parent as u64;
            (*node).__parent_color = pc;
        }
    }

    /// Mirrors `rb_left()`.
    ///
    /// # Safety
    /// `node` must be null or point to a live `rb_node`.
    #[inline(always)]
    pub(crate) unsafe fn left(node: *mut RbNode) -> *mut RbNode {
        if node.is_null() {
            return ptr::null_mut();
        }
        unsafe { (*node).left }
    }

    /// Mirrors `rb_right()`.
    ///
    /// # Safety
    /// `node` must be null or point to a live `rb_node`.
    #[inline(always)]
    pub(crate) unsafe fn right(node: *mut RbNode) -> *mut RbNode {
        if node.is_null() {
            return ptr::null_mut();
        }
        unsafe { (*node).right }
    }

    /// Mirrors `rb_node_is_empty()`.
    ///
    /// # Safety
    /// `node` must be null or point to a live `rb_node`.
    #[inline(always)]
    unsafe fn is_empty(node: *mut RbNode) -> bool {
        node.is_null() || unsafe { Self::parent(node) == node }
    }

    /// Mirrors `rb_node_is_top()`.
    ///
    /// # Safety
    /// `node` must point to a live `rb_node`.
    #[inline(always)]
    pub(crate) unsafe fn is_top(node: *mut RbNode) -> bool {
        unsafe { Self::parent(node).is_null() }
    }

    /// Mirrors `rb_node_is_leaf()`.
    ///
    /// # Safety
    /// `node` must point to a live `rb_node`.
    #[inline(always)]
    unsafe fn is_leaf(node: *mut RbNode) -> bool {
        unsafe { (*node).left.is_null() && (*node).right.is_null() }
    }

    /// Mirrors `__rb_link_nodes()`.
    ///
    /// # Safety
    /// `node` must point to a live `rb_node`; `link` must point to a valid
    /// `*mut rb_node` slot (either `&root->node` or a child-pointer field).
    #[inline(always)]
    unsafe fn link(parent: *mut RbNode, node: *mut RbNode, link: *mut *mut RbNode) {
        unsafe {
            Self::set_parent(node, parent);
            *link = node;
        }
    }

    /// Mirrors `__rb_delink_node()`.
    ///
    /// # Safety
    /// Same as [`RbNode::link`].
    #[inline(always)]
    unsafe fn delink(link: *mut *mut RbNode, node: *mut RbNode) {
        unsafe {
            *link = ptr::null_mut();
            Self::set_parent(node, node);
        }
    }
}

impl RbRoot {
    /// Mirrors `rb_root_is_initialized()`.
    ///
    /// # Safety
    /// `root` must be null or point to a live `rb_root`.
    #[inline(always)]
    pub(crate) unsafe fn is_initialized(root: *mut RbRoot) -> bool {
        if root.is_null() {
            return false;
        }
        unsafe { (*root).opts.is_some() }
    }

    /// Mirrors `rb_keys_cmp(root, key1, key2)`.
    ///
    /// # Safety
    /// `root` must be initialized (`RbRoot::is_initialized` returns true).
    #[inline(always)]
    unsafe fn keys_cmp(root: *mut RbRoot, key1: u64, key2: u64) -> i32 {
        unsafe {
            let ops = (*root).opts.expect("BUG: rb_root: opts is None");
            ops.keys_cmp(key1, key2)
        }
    }

    /// Mirrors `rb_get_node_key(root, node)`.
    ///
    /// # Safety
    /// `root` must be initialized; `node` must point to a live `rb_node`.
    #[inline(always)]
    unsafe fn node_key(root: *mut RbRoot, node: *mut RbNode) -> u64 {
        unsafe {
            let ops = (*root).opts.expect("BUG: rb_root: opts is None");
            ops.get_key(node)
        }
    }
}

// ---------------------------------------------------------------------------
// Public C ABI — exact symbol/signature parity with `kernel/inc/bintree.h`.
// Relocated onto `impl RbNode`/`impl RbRoot` per the crate-wide convention
// (redundant `rb_`/leading `__rb_` prefixes dropped).
// ---------------------------------------------------------------------------

impl RbNode {
    /// # Safety
    /// `node` must be null or point to a live `rb_node` belonging to a
    /// well-formed tree.
    pub(crate) unsafe fn brother(node: *mut RbNode) -> *mut RbNode {
        unsafe {
            let parent = Self::parent(node);
            if parent.is_null() || parent == node {
                return ptr::null_mut();
            }
            if node == (*parent).left {
                (*parent).right
            } else {
                (*parent).left
            }
        }
    }
}

impl RbRoot {
    /// # Safety
    /// `root` must point to a live `rb_root`; `node` must point to a live
    /// `rb_node`; `ret_parent` must be null or a valid `*mut *mut rb_node`
    /// out-param slot.
    pub(crate) unsafe fn node_link(
        root: *mut RbRoot,
        node: *mut RbNode,
        ret_parent: *mut *mut RbNode,
    ) -> *mut *mut RbNode {
        unsafe {
            let parent = RbNode::parent(node);
            if !ret_parent.is_null() {
                *ret_parent = parent;
            }
            if parent.is_null() {
                return ptr::addr_of_mut!((*root).node);
            }
            if parent == node {
                return ptr::null_mut();
            }
            if node == (*parent).left {
                ptr::addr_of_mut!((*parent).left)
            } else {
                ptr::addr_of_mut!((*parent).right)
            }
        }
    }

    /// # Safety
    /// `root` must be null or point to a live `rb_root`.
    pub(crate) unsafe fn first_node(root: *mut RbRoot) -> *mut RbNode {
        if root.is_null() {
            return ptr::null_mut();
        }
        unsafe {
            let mut pos = (*root).node;
            if pos.is_null() {
                return ptr::null_mut();
            }
            while !(*pos).left.is_null() {
                pos = (*pos).left;
            }
            pos
        }
    }

    /// # Safety
    /// `root` must be null or point to a live `rb_root`.
    pub(crate) unsafe fn last_node(root: *mut RbRoot) -> *mut RbNode {
        if root.is_null() {
            return ptr::null_mut();
        }
        unsafe {
            let mut pos = (*root).node;
            if pos.is_null() {
                return ptr::null_mut();
            }
            while !(*pos).right.is_null() {
                pos = (*pos).right;
            }
            pos
        }
    }
}

impl RbNode {
    /// # Safety
    /// `node` must be null or point to a live `rb_node`.
    pub(crate) unsafe fn next(node: *mut RbNode) -> *mut RbNode {
        unsafe {
            if Self::is_empty(node) {
                return ptr::null_mut();
            }
            let mut parent = node;
            let mut pos = (*parent).right;

            if !pos.is_null() {
                while !(*pos).left.is_null() {
                    pos = (*pos).left;
                }
                return pos;
            }

            loop {
                pos = parent;
                parent = Self::parent(pos);
                if !(!parent.is_null() && pos == (*parent).right) {
                    break;
                }
            }
            parent
        }
    }
}

/// In-order forward iterator over a tree's nodes: yields the same
/// sequence as the hand-rolled `let mut n = rb_first_node(root); while
/// !n.is_null() { ...; n = rb_next_node(n); }` walk that appears at the
/// `mm`/`vfs`/`proc` consumer sites, as one `for n in root.iter()`.
///
/// Holds a bare cursor (no borrow of the tree), so it composes with the
/// intrusive-pointer idiom the rest of this module speaks: each `next()`
/// returns a `*mut RbNode` and advances via [`RbNode::next`].
///
/// This is a *plain successor walk*: it reads the current node to find
/// its successor. Deleting or re-linking the node the iterator is
/// currently sitting on mid-walk is a use-after-free — such teardown
/// loops must keep capturing the successor *before* mutating (see
/// `vfs/xv6fs/block_cache.rs::xv6fs_bcache_destroy`) and stay hand-rolled.
pub(crate) struct RbTreeIter {
    cur: *mut RbNode,
}

impl Iterator for RbTreeIter {
    type Item = *mut RbNode;

    #[inline]
    fn next(&mut self) -> Option<*mut RbNode> {
        if self.cur.is_null() {
            return None;
        }
        let node = self.cur;
        // SAFETY: `node` is non-null here and, by the iterator's contract
        // (tree not structurally mutated at the cursor mid-walk), still a
        // live node of a well-formed tree — exactly `RbNode::next`'s
        // requirement.
        self.cur = unsafe { RbNode::next(node) };
        Some(node)
    }
}

impl RawRbRoot {
    /// In-order forward iterator over this tree's nodes (`rb_first_node`
    /// then repeated [`RbNode::next`]).
    ///
    /// # Safety
    /// `self` must be a live, well-formed `rb_root`, and the tree must not
    /// be structurally mutated at the iterator's current node for the
    /// lifetime of the returned [`RbTreeIter`] (deleting the *current*
    /// node mid-walk is a use-after-free; capture the successor first).
    #[inline]
    pub(crate) unsafe fn iter(&self) -> RbTreeIter {
        // The walk reads through raw pointers (`rb_first_node` takes
        // `*mut`), matching the module-wide intrusive-pointer idiom; the
        // `&self` receiver is not retained past this call.
        let root = self as *const RawRbRoot as *mut RawRbRoot;
        // SAFETY: `root` points to `self`, a live `rb_root` per the
        // caller contract.
        RbTreeIter {
            cur: unsafe { RbRoot::first_node(root) },
        }
    }
}

impl RbNode {
    /// # Safety
    /// `node` must be null or point to a live `rb_node`.
    pub(crate) unsafe fn prev(node: *mut RbNode) -> *mut RbNode {
        unsafe {
            if Self::is_empty(node) {
                return ptr::null_mut();
            }
            let mut parent = node;
            let mut pos = (*parent).left;

            if !pos.is_null() {
                while !(*pos).right.is_null() {
                    pos = (*pos).right;
                }
                return pos;
            }

            loop {
                pos = parent;
                parent = Self::parent(pos);
                if !(!parent.is_null() && pos == (*parent).left) {
                    break;
                }
            }
            parent
        }
    }

    /// # Safety
    /// `link` must point to a valid `*mut rb_node` slot that currently holds
    /// `old_node`; `new_node` and `old_node` must point to live `rb_node`s.
    pub(crate) unsafe fn replace(
        link: *mut *mut RbNode,
        new_node: *mut RbNode,
        old_node: *mut RbNode,
    ) {
        unsafe {
            *new_node = *old_node;
            *link = new_node;
            if !(*old_node).left.is_null() {
                Self::set_parent((*old_node).left, new_node);
            }
            if !(*old_node).right.is_null() {
                Self::set_parent((*old_node).right, new_node);
            }
            // Mirrors `rb_node_init(old_node)`: make it a detached/empty node.
            (*old_node).__parent_color = old_node as u64;
        }
    }
}

impl RbRoot {
    /// # Safety
    /// `root` must point to a live `rb_root`; `old_node` must point to a live
    /// `rb_node` currently linked into `*root`; `new_node` must be null or a
    /// live `rb_node`.
    pub(crate) unsafe fn transplant(
        root: *mut RbRoot,
        new_node: *mut RbNode,
        old_node: *mut RbNode,
    ) {
        unsafe {
            let parent = RbNode::parent(old_node);
            if parent.is_null() {
                (*root).node = new_node;
            } else if (*parent).left == old_node {
                (*parent).left = new_node;
            } else {
                (*parent).right = new_node;
            }
            if !new_node.is_null() {
                RbNode::set_parent(new_node, parent);
            }
        }
    }
}

impl RbRoot {
    /// # Safety
    /// `root` must point to a live, initialized `rb_root`; `ret_parent` must
    /// point to a valid `*mut rb_node` out-param slot.
    pub(crate) unsafe fn find_key_link(
        root: *mut RbRoot,
        ret_parent: *mut *mut RbNode,
        key: u64,
    ) -> *mut *mut RbNode {
        unsafe {
            let mut pos = (*root).node;
            let mut parent: *mut RbNode = ptr::null_mut();
            let mut link = ptr::addr_of_mut!((*root).node);
            while !pos.is_null() {
                let cmp_result = Self::keys_cmp(root, Self::node_key(root, pos), key);
                if cmp_result > 0 {
                    link = ptr::addr_of_mut!((*pos).left);
                } else if cmp_result < 0 {
                    link = ptr::addr_of_mut!((*pos).right);
                } else {
                    // Exact key match: return the link to it directly.
                    break;
                }
                parent = pos;
                pos = *link;
            }
            *ret_parent = parent;
            link
        }
    }

    /// # Safety
    /// `root` must be null or point to a live `rb_root`.
    pub(crate) unsafe fn find_key_rup(root: *mut RbRoot, key: u64) -> *mut RbNode {
        unsafe {
            if !Self::is_initialized(root) {
                return ptr::null_mut();
            }
            let mut parent: *mut RbNode = ptr::null_mut();
            let link = Self::find_key_link(root, ptr::addr_of_mut!(parent), key);
            if link.is_null() {
                return ptr::null_mut();
            }
            if !(*link).is_null() {
                return *link;
            }
            if parent.is_null() {
                return ptr::null_mut();
            }
            let pkey = Self::node_key(root, parent);
            if Self::keys_cmp(root, pkey, key) >= 0 {
                return parent;
            }
            RbNode::next(parent)
        }
    }

    /// # Safety
    /// `root` must be null or point to a live `rb_root`.
    ///
    /// P3-D3c: demoted. The one consumer that used to resolve this by C
    /// symbol name (`backtrace.rs`, via the bindgen-generated
    /// `crate::bindings::rb_find_key_rdown` extern declaration -- the cause
    /// of the earlier link failure that kept this `#[no_mangle]`) now calls
    /// `crate::bintree::RbRoot::find_key_rdown` directly, so no linker-resolved
    /// reference remains.
    pub(crate) unsafe fn find_key_rdown(root: *mut RbRoot, key: u64) -> *mut RbNode {
        unsafe {
            if !Self::is_initialized(root) {
                return ptr::null_mut();
            }
            let mut parent: *mut RbNode = ptr::null_mut();
            let link = Self::find_key_link(root, ptr::addr_of_mut!(parent), key);
            if link.is_null() {
                return ptr::null_mut();
            }
            if !(*link).is_null() {
                return *link;
            }
            if parent.is_null() {
                return ptr::null_mut();
            }
            let pkey = Self::node_key(root, parent);
            if Self::keys_cmp(root, pkey, key) <= 0 {
                return parent;
            }
            RbNode::prev(parent)
        }
    }

    /// # Safety
    /// `root` must be null or point to a live `rb_root`.
    pub(crate) unsafe fn find_key(root: *mut RbRoot, key: u64) -> *mut RbNode {
        unsafe {
            if !Self::is_initialized(root) {
                return ptr::null_mut();
            }
            let mut parent: *mut RbNode = ptr::null_mut();
            let link = Self::find_key_link(root, ptr::addr_of_mut!(parent), key);
            if link.is_null() {
                return ptr::null_mut();
            }
            *link
        }
    }

    /// # Safety
    /// `root` must be null or point to a live, initialized `rb_root`;
    /// `new_node` must be null or point to a live, detached `rb_node`.
    pub(crate) unsafe fn insert_node(root: *mut RbRoot, new_node: *mut RbNode) -> *mut RbNode {
        unsafe {
            if !Self::is_initialized(root) || new_node.is_null() {
                return ptr::null_mut();
            }
            let key = Self::node_key(root, new_node);
            let mut parent: *mut RbNode = ptr::null_mut();
            let link = Self::find_key_link(root, ptr::addr_of_mut!(parent), key);
            if (*link).is_null() {
                RbNode::link(parent, new_node, link);
                (*new_node).left = ptr::null_mut();
                (*new_node).right = ptr::null_mut();
            }
            *link
        }
    }

    /// # Safety
    /// `root` must be null or point to a live, initialized `rb_root`.
    pub(crate) unsafe fn delete_key(root: *mut RbRoot, key: u64) -> *mut RbNode {
        unsafe {
            if !Self::is_initialized(root) {
                return ptr::null_mut();
            }
            let mut parent: *mut RbNode = ptr::null_mut();
            let link = Self::find_key_link(root, ptr::addr_of_mut!(parent), key);
            let delete_node = *link;
            if delete_node.is_null() {
                return ptr::null_mut();
            }

            if RbNode::is_leaf(delete_node) {
                RbNode::delink(link, delete_node);
                return delete_node;
            }
            if (*delete_node).left.is_null() {
                Self::transplant(root, (*delete_node).right, delete_node);
                RbNode::set_parent(delete_node, delete_node);
                return delete_node;
            } else if (*delete_node).right.is_null() {
                Self::transplant(root, (*delete_node).left, delete_node);
                RbNode::set_parent(delete_node, delete_node);
                return delete_node;
            }

            let mut leaf_link = ptr::addr_of_mut!((*delete_node).left);
            let mut leaf = *leaf_link;
            while !(*leaf).right.is_null() {
                leaf_link = ptr::addr_of_mut!((*leaf).right);
                leaf = *leaf_link;
            }

            Self::transplant(root, (*leaf).left, leaf);
            RbNode::replace(link, leaf, delete_node);
            delete_node
        }
    }

    /// # Safety
    /// `root` must point to a live `rb_root`; `node` must be null or point to
    /// a live `rb_node` linked into `*root`.
    pub(crate) unsafe fn rotate_left(root: *mut RbRoot, node: *mut RbNode) -> *mut RbNode {
        unsafe {
            if node.is_null() {
                return ptr::null_mut();
            }
            let link = Self::node_link(root, node, ptr::null_mut());
            let parent = RbNode::parent(node);
            let right = (*node).right;
            if right.is_null() {
                return node;
            }
            let right_left = (*right).left;

            let link = if parent.is_null() { ptr::addr_of_mut!((*root).node) } else { link };
            RbNode::link(parent, right, link);
            if !right_left.is_null() {
                RbNode::link(node, right_left, ptr::addr_of_mut!((*node).right));
            } else {
                (*node).right = ptr::null_mut();
            }
            RbNode::link(right, node, ptr::addr_of_mut!((*right).left));

            right
        }
    }

    /// # Safety
    /// Same as [`RbRoot::rotate_left`].
    pub(crate) unsafe fn rotate_right(root: *mut RbRoot, node: *mut RbNode) -> *mut RbNode {
        unsafe {
            if node.is_null() {
                return ptr::null_mut();
            }
            let link = Self::node_link(root, node, ptr::null_mut());
            let parent = RbNode::parent(node);
            let left = (*node).left;
            if left.is_null() {
                return node;
            }
            let left_right = (*left).right;

            let link = if parent.is_null() { ptr::addr_of_mut!((*root).node) } else { link };
            RbNode::link(parent, left, link);
            if !left_right.is_null() {
                RbNode::link(node, left_right, ptr::addr_of_mut!((*node).left));
            } else {
                (*node).left = ptr::null_mut();
            }
            RbNode::link(left, node, ptr::addr_of_mut!((*left).right));

            left
        }
    }
}
