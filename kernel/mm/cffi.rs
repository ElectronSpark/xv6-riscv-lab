//! Canonical C-FFI surface shared by the mm crate.
//!
//! Before this module existed, every mm submodule (`page`, `slab`,
//! `pcache`, `mm_safe`, `early_allocator`) carried its own private
//! mirror of:
//!
//!   * Opaque types (`Spinlock`, `ListNode`),
//!   * `unsafe extern "C"` declarations of the kernel-wide primitives
//!     (spin locks, per-cpu push/pop, list helpers, `kmm_*`, `memset`,
//!     `__panic_*`).
//!
//! Each module then defined its own thin adapter wrappers on top.
//! That left three identical `Spinlock` types and three+ identical
//! `ListNode` types floating around the crate, plus the same extern
//! declarations duplicated four times — quietly held together by the
//! fact that all the mirror types had the same C ABI layout.
//!
//! This module owns those declarations exactly once. Consumers do:
//!
//! ```ignore
//! use crate::mm::cffi::{Spinlock, ListNode};
//! mod ffi {
//!     pub use crate::mm::cffi::raw::*;       // shared C primitives
//!     unsafe extern "C" {
//!         pub safe fn module_private_fn(...);   // local-only externs
//!     }
//! }
//! ```
//!
//! Module-private extern declarations (e.g. `xv6_buddy_*` for `page.rs`
//! or `xv6_pcache_*` for `pcache.rs`) remain in the module that uses
//! them — only the cross-cutting primitives are centralised here.
//!
//! Adapter wrappers (`cpuid()`, `push_off()`, idiomatic `list_*`)
//! continue to live in each module's `mod ffi` because the modules
//! prefer different ergonomic shapes (e.g. `slab.rs` returns
//! `Option<&mut _>` from `list_pop_front` while `page.rs` returns the
//! raw pointer). Unifying *those* is a separate refactor.

#![allow(dead_code)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

// ===========================================================================
// Mirror types — opaque, identical ABI to the C kernel's
// `spinlock_t` / `list_node_t`.
// ===========================================================================

/// Opaque mirror of the C kernel's `spinlock_t`.
///
/// The C typedef carries `__attribute__((aligned(64)))` but Rust forbids
/// a struct whose `align > size`, so we model the lock as a zero-sized
/// placeholder and always pass `*mut Spinlock`. Containing structs
/// reserve real storage via a 24-byte `[u8; SPINLOCK_BYTES]` field and
/// expose a `lock_ptr()` accessor that casts that storage to
/// `*mut Spinlock`.
#[repr(C)]
pub struct Spinlock {
    _opaque: [u8; 0],
}

/// Mirror of the C kernel's intrusive doubly-linked-list node.
///
/// Layout: two raw `next` / `prev` pointers, 16 bytes on a 64-bit
/// target. The `*_shims.c` files contain `_Static_assert`s pinning
/// this size.
#[repr(C)]
pub struct ListNode {
    pub prev: *mut ListNode,
    pub next: *mut ListNode,
}

impl ListNode {
    /// Construct a detached (self-referential is *not* assumed) node.
    /// Equivalent to the old `ListNode { prev: null_mut(), next: null_mut() }`
    /// literal scattered across modules; centralising it removes four
    /// identical constructor sites.
    #[inline]
    pub const fn new() -> Self {
        Self {
            prev: ptr::null_mut(),
            next: ptr::null_mut(),
        }
    }
}

// ===========================================================================
// Raw extern declarations
// ===========================================================================

/// All raw `extern "C"` symbols that more than one mm submodule needs.
///
/// They are marked `pub safe`, which lets callers invoke them from
/// outside an `{ ... }` block. The actual contracts (e.g. "the
/// lock pointer must be initialised") still apply — the `safe` keyword
/// only collapses the boilerplate, it does not waive the C-level
/// invariants. Each module decides whether to wrap them in safer
/// adapters (`&Spinlock`, `Option<&mut ListNode>`, …) on top.
pub mod raw {
    use super::{Spinlock, ListNode};
    use core::ffi::{c_char, c_int, c_void};

    unsafe extern "C" {
        // --- Spinlocks (kernel/lock/spin.c) -----------------------------
        pub safe fn spin_init(lock: *mut Spinlock, name: *const c_char);
        pub safe fn spin_lock(lock: *mut Spinlock);
        pub safe fn spin_unlock(lock: *mut Spinlock);
        pub safe fn spin_holding(lock: *mut Spinlock) -> c_int;

        // --- Per-CPU primitives (vm_pgtab_shims.c / slab_shims.c) ------
        pub safe fn xv6_cpuid() -> c_int;
        pub safe fn xv6_push_off();
        pub safe fn xv6_pop_off();

        // --- Intrusive list helpers (slab_shims.c) ---------------------
        pub safe fn xv6_list_init(entry: *mut ListNode);
        pub safe fn xv6_list_is_empty(head: *const ListNode) -> c_int;
        pub safe fn xv6_list_is_detached(entry: *const ListNode) -> c_int;
        pub safe fn xv6_list_detach(entry: *mut ListNode);
        pub safe fn xv6_list_push_front(head: *mut ListNode, entry: *mut ListNode);
        pub safe fn xv6_list_pop_front(head: *mut ListNode) -> *mut ListNode;
        pub safe fn xv6_list_first(head: *const ListNode) -> *mut ListNode;
        pub safe fn xv6_list_last(head: *const ListNode) -> *mut ListNode;

        // --- Generic kernel heap (kalloc.rs) ---------------------------
        pub safe fn kmm_alloc(n: usize) -> *mut c_void;
        pub safe fn kmm_free(ptr: *mut c_void);

        // --- libc-shaped primitives (kernel/string.c) ------------------
        pub safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;

        // --- Kernel panic helpers (kernel/printf.c) --------------------
        pub safe fn __panic_start();
        pub safe fn __panic_end() -> !;
    }
}

// Re-export the raw symbols at the module top so callers can write
// `cffi::spin_lock(...)` rather than `cffi::raw::spin_lock(...)`.
pub use raw::*;

// ===========================================================================
// Tiny shared adapters
// ===========================================================================
//
// These are the *uncontroversial* type-converting wrappers — the
// idiomatic shapes that every consumer was independently rewriting in
// its own `mod ffi`. Modules that want a different shape (e.g. an
// `Option<&mut _>` returning `list_pop_front`) can still define their
// own on top of the raw symbols.

#[inline]
pub fn cpuid() -> u32 {
    raw::xv6_cpuid() as u32
}

#[inline]
pub fn push_off() {
    raw::xv6_push_off()
}

#[inline]
pub fn pop_off() {
    raw::xv6_pop_off()
}

#[inline]
pub fn spin_holding_b(l: *mut Spinlock) -> bool {
    raw::spin_holding(l) != 0
}

#[inline]
pub fn list_is_empty_b(h: *const ListNode) -> bool {
    raw::xv6_list_is_empty(h) != 0
}

#[inline]
pub fn list_is_detached_b(e: *const ListNode) -> bool {
    raw::xv6_list_is_detached(e) != 0
}

/// Convenience panic wrapper used by the safe-adapter functions.
#[inline]
pub fn panic_bytes(msg: &[u8]) -> ! {
    raw::__panic_start();
    // The C panic macros print a final message and halt; we feed the
    // raw bytes through `printf("%s", msg)`. Since `printf` is
    // variadic we can't declare it as `safe`, so callers that want
    // a richer message keep their existing module-local panic shim.
    unsafe extern "C" {
        fn printf(fmt: *const c_char, ...) -> c_int;
    }
    unsafe {
        printf(b"%s\n\0".as_ptr() as *const c_char, msg.as_ptr());
    }
    raw::__panic_end()
}

// ===========================================================================
// Compile-time guards
// ===========================================================================
//
// These const-evaluations fail compilation if a future bindgen update
// changes the canonical layout. They also keep `c_char` / `c_void`
// "used" so a no-import build still compiles cleanly.
const _: () = {
    // 16 bytes on a 64-bit target.
    assert!(core::mem::size_of::<ListNode>() == 16);
    assert!(core::mem::align_of::<ListNode>() == 8);
    // Opaque, ZST.
    assert!(core::mem::size_of::<Spinlock>() == 0);
};

// Suppress unused-import warnings if the cffi consumer set happens to
// not need them in a particular build configuration.
#[allow(dead_code)]
const _USED: (Option<*const c_void>, Option<c_char>, Option<c_int>) = (None, None, None);
