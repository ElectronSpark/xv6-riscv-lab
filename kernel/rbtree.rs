//! Red-black tree coloring/balancing — Rust port of `kernel/rbtree.c`.
//!
//! Builds on top of [`crate::bintree`] exactly as `kernel/rbtree.c`
//! builds on `kernel/bintree.c` in C: `rb_insert_color` reuses
//! `bintree::rb_insert_node` for the plain BST insert, then performs the
//! red/black recoloring and rotation dance; the two delete entry points
//! reuse `bintree::{__rb_node_link, __rb_find_key_link, __rb_transplant,
//! __rb_replace_node, __rb_rotate_left, __rb_rotate_right}`.
//!
//! `kernel/inc/rbtree.h`'s `rb_is_node_black` macro and
//! `kernel/inc/bintree.h`'s `rb_parent`/`rb_left`/`rb_right`/
//! `rb_node_is_top`/`rb_root_is_initialized` are `static inline` (no
//! external linkage), so `bintree.rs` exposes `pub(crate)` copies of the
//! latter and this module reuses them directly instead of duplicating a
//! third copy.
//!
//! The algorithm bodies below (`rb_insert_color`,
//! `__rb_delete_color_fixup`, `__rb_do_delete_node_color`) are a
//! deliberately faithful, line-for-line transliteration of the C —
//! red-black balancing is easy to subtly break by "improving" it, and
//! this data structure is on the hot path for `mm/vm.rs` (VMA lookup),
//! `mm/pcache.rs` (page cache lookup), `proc/thread_queue.rs` (runqueue
//! ordering), and `timer/timer.c`.

#![allow(non_camel_case_types)]

use core::ptr;

pub(crate) use crate::bintree::{
    __rb_find_key_link, __rb_node_link, __rb_replace_node, __rb_rotate_left, __rb_rotate_right,
    __rb_transplant, rb_insert_node, rb_left, rb_node_is_top, rb_parent, rb_right,
    rb_root_is_initialized, rb_set_parent, RbNode, RbRoot,
};

/// Mirrors `rb_is_node_black()`. NULL nodes count as black, matching the
/// sentinel-free red-black tree convention used throughout this port.
///
/// # Safety
/// `node` must be null or point to a live `rb_node`.
#[inline(always)]
unsafe fn rb_is_node_black(node: *mut RbNode) -> bool {
    node.is_null() || unsafe { (*node).__parent_color & 1 != 0 }
}

/// Mirrors `rb_node_dye_black()`.
///
/// # Safety
/// `node` must be null or point to a live `rb_node`.
#[inline(always)]
unsafe fn rb_node_dye_black(node: *mut RbNode) {
    if node.is_null() {
        return;
    }
    unsafe { (*node).__parent_color |= 1 };
}

/// Mirrors `rb_node_dye_red()`.
///
/// # Safety
/// `node` must be null or point to a live `rb_node`.
#[inline(always)]
unsafe fn rb_node_dye_red(node: *mut RbNode) {
    if node.is_null() {
        return;
    }
    unsafe { (*node).__parent_color &= !1u64 };
}

/// Mirrors `rb_node_dye_as()`.
///
/// # Safety
/// `target` must be null or point to a live `rb_node`; `source` must be
/// null or point to a live `rb_node`.
#[inline(always)]
unsafe fn rb_node_dye_as(target: *mut RbNode, source: *mut RbNode) {
    unsafe {
        if rb_is_node_black(source) {
            rb_node_dye_black(target);
        } else {
            rb_node_dye_red(target);
        }
    }
}

/// # Safety
/// `root` must be null or point to a live `rb_root`; `node` must be null
/// or point to a live, detached `rb_node`.
#[no_mangle]
pub unsafe extern "C" fn rb_insert_color(root: *mut RbRoot, node: *mut RbNode) -> *mut RbNode {
    unsafe {
        if root.is_null() || node.is_null() {
            return ptr::null_mut();
        }

        // Reuses the plain BST insert; a return value other than `node`
        // means the key already existed (return the pre-existing node).
        let mut pos = rb_insert_node(root, node);
        if pos != node {
            return pos;
        }

        rb_node_dye_red(pos);
        let mut parent = rb_parent(pos);
        let mut grand_parent = rb_parent(parent);
        let mut uncle: *mut RbNode;
        // INVARIANT (matches the C original exactly, including its use of
        // direct `grand_parent->left`/`->right` field access rather than
        // the null-safe `rb_left`/`rb_right` helpers `rb_delete_color_fixup`
        // uses below): a red `parent` (the loop condition) can never be the
        // tree root, because the root is always black — so `grand_parent`
        // is provably non-null on every iteration.
        while !rb_is_node_black(parent) {
            if parent == (*grand_parent).left {
                uncle = (*grand_parent).right;
                if !rb_is_node_black(uncle) {
                    rb_node_dye_black(parent);
                    rb_node_dye_black(uncle);
                    rb_node_dye_red(grand_parent);
                    pos = grand_parent;
                } else if pos == (*parent).right {
                    __rb_rotate_left(root, parent);
                    __rb_rotate_right(root, grand_parent);
                    rb_node_dye_black(parent);
                    rb_node_dye_black(grand_parent);
                } else {
                    __rb_rotate_right(root, grand_parent);
                    rb_node_dye_black(pos);
                    rb_node_dye_black(grand_parent);
                    pos = parent;
                }
            } else {
                uncle = (*grand_parent).left;
                if !rb_is_node_black(uncle) {
                    rb_node_dye_black(parent);
                    rb_node_dye_black(uncle);
                    rb_node_dye_red(grand_parent);
                    pos = grand_parent;
                } else if pos == (*parent).left {
                    __rb_rotate_right(root, parent);
                    __rb_rotate_left(root, grand_parent);
                    rb_node_dye_black(parent);
                    rb_node_dye_black(grand_parent);
                } else {
                    __rb_rotate_left(root, grand_parent);
                    rb_node_dye_black(pos);
                    rb_node_dye_black(grand_parent);
                    pos = parent;
                }
            }
            parent = rb_parent(pos);
            grand_parent = rb_parent(parent);
        }

        if rb_node_is_top(pos) {
            rb_node_dye_black(pos);
        }

        node
    }
}

/// Mirrors `__rb_delete_color_fixup()`: re-establish black-height balance
/// after removing a black node.
///
/// # Safety
/// `root` must point to a live `rb_root`; `node` must be null or point to
/// a live `rb_node` within `*root`.
unsafe fn rb_delete_color_fixup(root: *mut RbRoot, mut node: *mut RbNode) {
    unsafe {
        let mut brother: *mut RbNode;
        let mut parent = rb_parent(node);
        while node != (*root).node && rb_is_node_black(node) {
            if node == rb_left(parent) {
                brother = rb_right(parent);
                if !rb_is_node_black(brother) {
                    rb_node_dye_red(parent);
                    rb_node_dye_black(brother);
                    __rb_rotate_left(root, parent);
                    parent = rb_parent(node);
                    brother = rb_right(parent);
                }
                if rb_is_node_black(rb_left(brother)) && rb_is_node_black(rb_right(brother)) {
                    rb_node_dye_red(brother);
                    node = parent;
                    parent = rb_parent(node);
                } else {
                    if rb_is_node_black(rb_right(brother)) {
                        rb_node_dye_black(rb_left(brother));
                        rb_node_dye_red(brother);
                        __rb_rotate_right(root, brother);
                        parent = rb_parent(node);
                        brother = rb_right(parent);
                    }

                    rb_node_dye_as(brother, parent);
                    rb_node_dye_black(parent);
                    rb_node_dye_black(rb_right(brother));
                    __rb_rotate_left(root, parent);
                    node = (*root).node;
                    parent = ptr::null_mut();
                }
            } else {
                brother = rb_left(parent);
                if !rb_is_node_black(brother) {
                    rb_node_dye_red(parent);
                    rb_node_dye_black(brother);
                    __rb_rotate_right(root, parent);
                    parent = rb_parent(node);
                    brother = rb_left(parent);
                }
                if rb_is_node_black(rb_left(brother)) && rb_is_node_black(rb_right(brother)) {
                    rb_node_dye_red(brother);
                    node = parent;
                    parent = rb_parent(node);
                } else {
                    if rb_is_node_black(rb_left(brother)) {
                        rb_node_dye_black(rb_right(brother));
                        rb_node_dye_red(brother);
                        __rb_rotate_left(root, brother);
                        parent = rb_parent(node);
                        brother = rb_left(parent);
                    }

                    rb_node_dye_as(brother, parent);
                    rb_node_dye_black(parent);
                    rb_node_dye_black(rb_left(brother));
                    __rb_rotate_right(root, parent);

                    node = (*root).node;
                    parent = ptr::null_mut();
                }
            }
        }

        rb_node_dye_black(node);
    }
}

/// Mirrors `__rb_do_delete_node_color()`.
///
/// # Safety
/// `root` must point to a live `rb_root`; `link` must point to a valid
/// `*mut rb_node` slot; `parent` is unused by the algorithm itself but
/// kept in the signature to mirror the C original.
unsafe fn rb_do_delete_node_color(
    root: *mut RbRoot,
    _parent: *mut RbNode,
    link: *mut *mut RbNode,
) {
    unsafe {
        let delete_node = *link;
        if delete_node.is_null() {
            return;
        }

        let mut target = delete_node;

        if !(*target).left.is_null() && !(*target).right.is_null() {
            let mut successor = (*target).right;
            while !(*successor).left.is_null() {
                successor = (*successor).left;
            }
            // swap successor and delete_node
            target = successor;
        }

        let replacement = if !(*target).left.is_null() { (*target).left } else { (*target).right };

        if !replacement.is_null() {
            __rb_transplant(root, replacement, target);
            // `rb_set_parent(target, target)`: unlike `rb_node_init`, this
            // preserves `target`'s current color bits and only replaces the
            // parent-pointer portion (with `target`'s own 8-byte-aligned
            // address, whose low 3 bits are already 0).
            rb_set_parent(target, target);
            if rb_is_node_black(target) {
                rb_delete_color_fixup(root, replacement);
            }
        } else if rb_node_is_top(target) {
            (*root).node = ptr::null_mut();
        } else {
            if rb_is_node_black(target) {
                rb_delete_color_fixup(root, target);
            }
            if !rb_node_is_top(target) {
                let target_link = __rb_node_link(root, target, ptr::null_mut());
                *target_link = ptr::null_mut();
                rb_set_parent(target, target);
            }
        }

        if target != delete_node {
            let link = __rb_node_link(root, delete_node, ptr::null_mut());
            __rb_replace_node(link, target, delete_node);
        }
    }
}

/// # Safety
/// `root` must point to a live `rb_root`; `node` must be null or point to
/// a live `rb_node`.
#[no_mangle]
pub unsafe extern "C" fn rb_delete_node_color(root: *mut RbRoot, node: *mut RbNode) -> *mut RbNode {
    unsafe {
        let mut parent = ptr::null_mut();
        let link = __rb_node_link(root, node, ptr::addr_of_mut!(parent));
        if link.is_null() || (*link).is_null() {
            return ptr::null_mut();
        }
        let target = *link;
        rb_do_delete_node_color(root, parent, link);
        target
    }
}

/// # Safety
/// `root` must be null or point to a live `rb_root`.
pub(crate) unsafe fn rb_delete_key_color(root: *mut RbRoot, key: u64) -> *mut RbNode {
    unsafe {
        if !rb_root_is_initialized(root) {
            return ptr::null_mut();
        }
        let mut parent = ptr::null_mut();
        let link = __rb_find_key_link(root, ptr::addr_of_mut!(parent), key);
        if link.is_null() || (*link).is_null() {
            return ptr::null_mut();
        }
        let target = *link;
        rb_do_delete_node_color(root, parent, link);
        target
    }
}
