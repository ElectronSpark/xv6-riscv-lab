//! Intrusive binary search tree — Rust port of `kernel/bintree.c`.
//!
//! `struct rb_node` / `struct rb_root` / `struct rb_root_opts` are the
//! real bindgen-generated `crate::bindings::{rb_node, rb_root,
//! rb_root_opts}` types (already allowlisted in `build.rs` and included
//! via `wrapper.h`'s `rbtree.h` → `bintree.h` chain) — no opaque
//! stand-ins.
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

/// Low bits of `__parent_color` reserved for out-of-band data (only bit 0
/// — the red/black color — is used today, by `kernel/rbtree.c`). Mirrors
/// `_RB_COLOR_MASK` in `kernel/inc/bintree.h`.
const RB_COLOR_MASK: u64 = 7;

// ---------------------------------------------------------------------------
// Private re-implementations of `kernel/inc/bintree.h`'s `static inline`
// helpers. `pub(crate)` where `kernel/rbtree.rs` needs the same primitive
// (mirrors both `.c` files independently `#include`-ing the same header).
// ---------------------------------------------------------------------------

/// Mirrors `rb_parent()`.
///
/// # Safety
/// `node` must be null or point to a live, properly initialised `rb_node`.
#[inline(always)]
pub(crate) unsafe fn rb_parent(node: *mut RbNode) -> *mut RbNode {
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
pub(crate) unsafe fn rb_set_parent(node: *mut RbNode, parent: *mut RbNode) {
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
pub(crate) unsafe fn rb_left(node: *mut RbNode) -> *mut RbNode {
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
pub(crate) unsafe fn rb_right(node: *mut RbNode) -> *mut RbNode {
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
unsafe fn rb_node_is_empty(node: *mut RbNode) -> bool {
    node.is_null() || unsafe { rb_parent(node) == node }
}

/// Mirrors `rb_node_is_top()`.
///
/// # Safety
/// `node` must point to a live `rb_node`.
#[inline(always)]
pub(crate) unsafe fn rb_node_is_top(node: *mut RbNode) -> bool {
    unsafe { rb_parent(node).is_null() }
}

/// Mirrors `rb_node_is_leaf()`.
///
/// # Safety
/// `node` must point to a live `rb_node`.
#[inline(always)]
unsafe fn rb_node_is_leaf(node: *mut RbNode) -> bool {
    unsafe { (*node).left.is_null() && (*node).right.is_null() }
}

/// Mirrors `rb_root_is_initialized()`.
///
/// # Safety
/// `root` must be null or point to a live `rb_root`.
#[inline(always)]
pub(crate) unsafe fn rb_root_is_initialized(root: *mut RbRoot) -> bool {
    if root.is_null() {
        return false;
    }
    unsafe {
        let opts = (*root).opts;
        !opts.is_null() && (*opts).keys_cmp_fun.is_some() && (*opts).get_key_fun.is_some()
    }
}

/// Mirrors `rb_keys_cmp(root, key1, key2)`.
///
/// # Safety
/// `root` must be initialized (`rb_root_is_initialized` returns true).
#[inline(always)]
unsafe fn rb_keys_cmp(root: *mut RbRoot, key1: u64, key2: u64) -> i32 {
    unsafe {
        let cmp = (*(*root).opts).keys_cmp_fun.expect("BUG: rb_root: keys_cmp_fun is None");
        cmp(key1, key2)
    }
}

/// Mirrors `rb_get_node_key(root, node)`.
///
/// # Safety
/// `root` must be initialized; `node` must point to a live `rb_node`.
#[inline(always)]
unsafe fn rb_get_node_key(root: *mut RbRoot, node: *mut RbNode) -> u64 {
    unsafe {
        let get_key = (*(*root).opts).get_key_fun.expect("BUG: rb_root: get_key_fun is None");
        get_key(node)
    }
}

/// Mirrors `__rb_link_nodes()`.
///
/// # Safety
/// `node` must point to a live `rb_node`; `link` must point to a valid
/// `*mut rb_node` slot (either `&root->node` or a child-pointer field).
#[inline(always)]
unsafe fn rb_link_nodes(parent: *mut RbNode, node: *mut RbNode, link: *mut *mut RbNode) {
    unsafe {
        rb_set_parent(node, parent);
        *link = node;
    }
}

/// Mirrors `__rb_delink_node()`.
///
/// # Safety
/// Same as [`rb_link_nodes`].
#[inline(always)]
unsafe fn rb_delink_node(link: *mut *mut RbNode, node: *mut RbNode) {
    unsafe {
        *link = ptr::null_mut();
        rb_set_parent(node, node);
    }
}

// ---------------------------------------------------------------------------
// Public C ABI — exact symbol/signature parity with `kernel/inc/bintree.h`.
// ---------------------------------------------------------------------------

/// # Safety
/// `node` must be null or point to a live `rb_node` belonging to a
/// well-formed tree.
pub(crate) unsafe fn rb_brother(node: *mut RbNode) -> *mut RbNode {
    unsafe {
        let parent = rb_parent(node);
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

/// # Safety
/// `root` must point to a live `rb_root`; `node` must point to a live
/// `rb_node`; `ret_parent` must be null or a valid `*mut *mut rb_node`
/// out-param slot.
pub(crate) unsafe fn __rb_node_link(
    root: *mut RbRoot,
    node: *mut RbNode,
    ret_parent: *mut *mut RbNode,
) -> *mut *mut RbNode {
    unsafe {
        let parent = rb_parent(node);
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
#[no_mangle]
pub unsafe extern "C" fn rb_first_node(root: *mut RbRoot) -> *mut RbNode {
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
#[no_mangle]
pub unsafe extern "C" fn rb_last_node(root: *mut RbRoot) -> *mut RbNode {
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

/// # Safety
/// `node` must be null or point to a live `rb_node`.
#[no_mangle]
pub unsafe extern "C" fn rb_next_node(node: *mut RbNode) -> *mut RbNode {
    unsafe {
        if rb_node_is_empty(node) {
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
            parent = rb_parent(pos);
            if !(!parent.is_null() && pos == (*parent).right) {
                break;
            }
        }
        parent
    }
}

/// # Safety
/// `node` must be null or point to a live `rb_node`.
pub(crate) unsafe fn rb_prev_node(node: *mut RbNode) -> *mut RbNode {
    unsafe {
        if rb_node_is_empty(node) {
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
            parent = rb_parent(pos);
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
pub(crate) unsafe fn __rb_replace_node(
    link: *mut *mut RbNode,
    new_node: *mut RbNode,
    old_node: *mut RbNode,
) {
    unsafe {
        *new_node = *old_node;
        *link = new_node;
        if !(*old_node).left.is_null() {
            rb_set_parent((*old_node).left, new_node);
        }
        if !(*old_node).right.is_null() {
            rb_set_parent((*old_node).right, new_node);
        }
        // Mirrors `rb_node_init(old_node)`: make it a detached/empty node.
        (*old_node).__parent_color = old_node as u64;
    }
}

/// # Safety
/// `root` must point to a live `rb_root`; `old_node` must point to a live
/// `rb_node` currently linked into `*root`; `new_node` must be null or a
/// live `rb_node`.
pub(crate) unsafe fn __rb_transplant(
    root: *mut RbRoot,
    new_node: *mut RbNode,
    old_node: *mut RbNode,
) {
    unsafe {
        let parent = rb_parent(old_node);
        if parent.is_null() {
            (*root).node = new_node;
        } else if (*parent).left == old_node {
            (*parent).left = new_node;
        } else {
            (*parent).right = new_node;
        }
        if !new_node.is_null() {
            rb_set_parent(new_node, parent);
        }
    }
}

/// # Safety
/// `root` must point to a live, initialized `rb_root`; `ret_parent` must
/// point to a valid `*mut rb_node` out-param slot.
pub(crate) unsafe fn __rb_find_key_link(
    root: *mut RbRoot,
    ret_parent: *mut *mut RbNode,
    key: u64,
) -> *mut *mut RbNode {
    unsafe {
        let mut pos = (*root).node;
        let mut parent: *mut RbNode = ptr::null_mut();
        let mut link = ptr::addr_of_mut!((*root).node);
        while !pos.is_null() {
            let cmp_result = rb_keys_cmp(root, rb_get_node_key(root, pos), key);
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
#[no_mangle]
pub unsafe extern "C" fn rb_find_key_rup(root: *mut RbRoot, key: u64) -> *mut RbNode {
    unsafe {
        if !rb_root_is_initialized(root) {
            return ptr::null_mut();
        }
        let mut parent: *mut RbNode = ptr::null_mut();
        let link = __rb_find_key_link(root, ptr::addr_of_mut!(parent), key);
        if link.is_null() {
            return ptr::null_mut();
        }
        if !(*link).is_null() {
            return *link;
        }
        if parent.is_null() {
            return ptr::null_mut();
        }
        let pkey = rb_get_node_key(root, parent);
        if rb_keys_cmp(root, pkey, key) >= 0 {
            return parent;
        }
        rb_next_node(parent)
    }
}

/// # Safety
/// `root` must be null or point to a live `rb_root`.
///
/// NOT demoted: `backtrace.rs` (out of this wave's scope) calls this via
/// `crate::bindings::rb_find_key_rdown(...)` — the bindgen-generated
/// extern declaration from `kernel/inc/bintree.h`'s prototype, resolved
/// by the linker against this exact C symbol name, not a Rust-level
/// `crate::bintree::` path call. Discovered as a real link failure
/// (`undefined reference to 'rb_find_key_rdown'`) when first demoted.
#[no_mangle]
pub unsafe extern "C" fn rb_find_key_rdown(root: *mut RbRoot, key: u64) -> *mut RbNode {
    unsafe {
        if !rb_root_is_initialized(root) {
            return ptr::null_mut();
        }
        let mut parent: *mut RbNode = ptr::null_mut();
        let link = __rb_find_key_link(root, ptr::addr_of_mut!(parent), key);
        if link.is_null() {
            return ptr::null_mut();
        }
        if !(*link).is_null() {
            return *link;
        }
        if parent.is_null() {
            return ptr::null_mut();
        }
        let pkey = rb_get_node_key(root, parent);
        if rb_keys_cmp(root, pkey, key) <= 0 {
            return parent;
        }
        rb_prev_node(parent)
    }
}

/// # Safety
/// `root` must be null or point to a live `rb_root`.
pub(crate) unsafe fn rb_find_key(root: *mut RbRoot, key: u64) -> *mut RbNode {
    unsafe {
        if !rb_root_is_initialized(root) {
            return ptr::null_mut();
        }
        let mut parent: *mut RbNode = ptr::null_mut();
        let link = __rb_find_key_link(root, ptr::addr_of_mut!(parent), key);
        if link.is_null() {
            return ptr::null_mut();
        }
        *link
    }
}

/// # Safety
/// `root` must be null or point to a live, initialized `rb_root`;
/// `new_node` must be null or point to a live, detached `rb_node`.
pub(crate) unsafe fn rb_insert_node(root: *mut RbRoot, new_node: *mut RbNode) -> *mut RbNode {
    unsafe {
        if !rb_root_is_initialized(root) || new_node.is_null() {
            return ptr::null_mut();
        }
        let key = rb_get_node_key(root, new_node);
        let mut parent: *mut RbNode = ptr::null_mut();
        let link = __rb_find_key_link(root, ptr::addr_of_mut!(parent), key);
        if (*link).is_null() {
            rb_link_nodes(parent, new_node, link);
            (*new_node).left = ptr::null_mut();
            (*new_node).right = ptr::null_mut();
        }
        *link
    }
}

/// # Safety
/// `root` must be null or point to a live, initialized `rb_root`.
pub(crate) unsafe fn rb_delete_key(root: *mut RbRoot, key: u64) -> *mut RbNode {
    unsafe {
        if !rb_root_is_initialized(root) {
            return ptr::null_mut();
        }
        let mut parent: *mut RbNode = ptr::null_mut();
        let link = __rb_find_key_link(root, ptr::addr_of_mut!(parent), key);
        let delete_node = *link;
        if delete_node.is_null() {
            return ptr::null_mut();
        }

        if rb_node_is_leaf(delete_node) {
            rb_delink_node(link, delete_node);
            return delete_node;
        }
        if (*delete_node).left.is_null() {
            __rb_transplant(root, (*delete_node).right, delete_node);
            rb_set_parent(delete_node, delete_node);
            return delete_node;
        } else if (*delete_node).right.is_null() {
            __rb_transplant(root, (*delete_node).left, delete_node);
            rb_set_parent(delete_node, delete_node);
            return delete_node;
        }

        let mut leaf_link = ptr::addr_of_mut!((*delete_node).left);
        let mut leaf = *leaf_link;
        while !(*leaf).right.is_null() {
            leaf_link = ptr::addr_of_mut!((*leaf).right);
            leaf = *leaf_link;
        }

        __rb_transplant(root, (*leaf).left, leaf);
        __rb_replace_node(link, leaf, delete_node);
        delete_node
    }
}

/// # Safety
/// `root` must point to a live `rb_root`; `node` must be null or point to
/// a live `rb_node` linked into `*root`.
pub(crate) unsafe fn __rb_rotate_left(root: *mut RbRoot, node: *mut RbNode) -> *mut RbNode {
    unsafe {
        if node.is_null() {
            return ptr::null_mut();
        }
        let link = __rb_node_link(root, node, ptr::null_mut());
        let parent = rb_parent(node);
        let right = (*node).right;
        if right.is_null() {
            return node;
        }
        let right_left = (*right).left;

        let link = if parent.is_null() { ptr::addr_of_mut!((*root).node) } else { link };
        rb_link_nodes(parent, right, link);
        if !right_left.is_null() {
            rb_link_nodes(node, right_left, ptr::addr_of_mut!((*node).right));
        } else {
            (*node).right = ptr::null_mut();
        }
        rb_link_nodes(right, node, ptr::addr_of_mut!((*right).left));

        right
    }
}

/// # Safety
/// Same as [`__rb_rotate_left`].
pub(crate) unsafe fn __rb_rotate_right(root: *mut RbRoot, node: *mut RbNode) -> *mut RbNode {
    unsafe {
        if node.is_null() {
            return ptr::null_mut();
        }
        let link = __rb_node_link(root, node, ptr::null_mut());
        let parent = rb_parent(node);
        let left = (*node).left;
        if left.is_null() {
            return node;
        }
        let left_right = (*left).right;

        let link = if parent.is_null() { ptr::addr_of_mut!((*root).node) } else { link };
        rb_link_nodes(parent, left, link);
        if !left_right.is_null() {
            rb_link_nodes(node, left_right, ptr::addr_of_mut!((*node).left));
        } else {
            (*node).left = ptr::null_mut();
        }
        rb_link_nodes(left, node, ptr::addr_of_mut!((*left).right));

        left
    }
}
