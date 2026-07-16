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
/// Wave P3-3C: this used to be an independent local mirror (pinned only
/// by a size/align *literal* assert at the bottom of this file, never
/// cross-checked directly against the real bindgen type) — one of the
/// "three-plus identical `ListNode` types" this module's own doc comment
/// above already called out as an accident of history. It is now a
/// re-export of the crate's one canonical native mirror,
/// `crate::list::ListNode`, which carries the direct-against-bindgen
/// `size_of`/`align_of`/`offset_of!` asserts (see `kernel/list.rs`).
/// Every existing `use crate::mm::cffi::{Spinlock, ListNode}` import
/// (and `ListNode::new()` call site, e.g. `mm/early_allocator.rs`,
/// `mm/slab.rs`) keeps working unchanged.
pub use crate::list::ListNode;

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

        // --- Per-CPU primitives (vm_pgtab_shims.c / slab_shims.c) ------

        // --- Intrusive list helpers (slab_shims.c) ---------------------

        // --- Generic kernel heap (kalloc.rs) ---------------------------

        // --- libc-shaped primitives (kernel/string.c) ------------------

        // --- Kernel panic helpers (kernel/printf.c) --------------------
        pub safe fn __panic_start();
        pub safe fn __panic_end() -> !;
    }
pub(crate) use crate::mm::cffi::{xv6_cpuid, xv6_list_detach, xv6_list_first, xv6_list_init, xv6_list_is_detached, xv6_list_is_empty, xv6_list_last, xv6_list_pop_front, xv6_list_push_back, xv6_list_push_front, xv6_pop_off, xv6_push_off};

    // `kmm_alloc`/`kmm_free`/`memset` are genuinely `unsafe fn` in their
    // canonical modules (`pub(crate)` since P3-D3a); the original extern
    // declarations here asserted `pub safe fn` (this whole module's
    // documented purpose — see the doc comment above `pub mod raw`: "lets
    // callers invoke them from outside an unsafe block ... the safe
    // keyword only collapses the boilerplate, it does not waive the
    // C-level invariants"). Thin safe wrappers preserve that facade for
    // every consumer of `cffi::raw` (in- and out-of-scope alike) instead
    // of pushing `unsafe {}` onto every call site.

    /// SAFETY: see [`crate::mm::kalloc::kmm_alloc`]'s contract.
    pub fn kmm_alloc(size: usize) -> *mut c_void {
        unsafe { crate::mm::kalloc::kmm_alloc(size) }
    }
    /// SAFETY: see [`crate::mm::kalloc::kmm_free`]'s contract.
    pub fn kmm_free(ptr: *mut c_void) {
        unsafe { crate::mm::kalloc::kmm_free(ptr) };
    }
    /// SAFETY: see [`crate::string::memset`]'s contract.
    pub fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void {
        unsafe { crate::string::memset(dst, c, n) }
    }

    // --- Spinlocks (kernel/lock/spin.c) ---------------------------------
    //
    // NOT plain `use` re-exports of `crate::lock::spinlock::spin_*`: this
    // module's `Spinlock` is a zero-sized opaque mirror type distinct from
    // (but layout-identical to) `crate::bindings::spinlock_t` — the same
    // "same C type, different Rust name" relationship documented at
    // `sync::KSpinlock::from_bindings`. The real functions now take
    // `*mut spinlock_t`; these thin wrappers preserve the original
    // `*mut Spinlock`-typed call sites (this crate has three-plus distinct
    // "locally convenient" pointer-type views of the one C `spinlock_t`)
    // via a pointer reinterpretation, not a layout change.

    /// SAFETY: see the module-level note above; `lock`/`name` carry the
    /// same "already-allocated, caller-owned for the lock's lifetime"
    /// contract the original C-ABI `spin_init` always had.
    pub(crate) fn spin_init(lock: *mut Spinlock, name: *const c_char) {
        unsafe {
            crate::lock::spinlock::spin_init(
                lock as *mut crate::bindings::spinlock_t,
                name as *mut c_char,
            );
        }
    }

    /// SAFETY: see [`spin_init`].
    pub(crate) fn spin_lock(lock: *mut Spinlock) {
        unsafe { crate::lock::spinlock::spin_lock(lock as *mut crate::bindings::spinlock_t) };
    }

    /// SAFETY: see [`spin_init`].
    pub(crate) fn spin_unlock(lock: *mut Spinlock) {
        unsafe { crate::lock::spinlock::spin_unlock(lock as *mut crate::bindings::spinlock_t) };
    }

    /// SAFETY: see [`spin_init`].
    pub(crate) fn spin_holding(lock: *mut Spinlock) -> c_int {
        unsafe { crate::lock::spinlock::spin_holding(lock as *mut crate::bindings::spinlock_t) }
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
    // The C panic macros print a final message and halt; `msg` is always a
    // NUL-terminated byte-string supplied by call sites, rendered via the
    // `Cs` adapter as a runtime C string (content unknown at this site).
    // `printf` is no longer called directly here (migrated to `kprintln!`
    // below) but its extern decl is left in place -- unused now, removed
    // crate-wide once every file is migrated.
    crate::kprintln!("{}", crate::printf::Cs(msg.as_ptr() as *const c_char));
    raw::__panic_end()
}

// ===========================================================================
// Canonical implementations of the cross-cutting primitives declared in
// `raw` above (`xv6_cpuid`/`xv6_push_off`/`xv6_pop_off`, and the intrusive
// list ops). These used to live in `slab_shims.rs`, which despite its name
// was the sole definer of these crate-wide primitives (consumed by
// `page.rs`, `slab.rs`, `early_allocator.rs`, `mm_safe.rs`, and
// `proc::cffi`) — an accident of history, not a slab-specific concern.
// Collapsing `slab_shims.rs` (mm WP3) means these need a real home; since
// `cffi.rs` already owns the `raw` *declarations* exactly once, it owns
// the *definitions* too. Consumers keep calling `cffi::raw::xv6_cpuid()`
// etc. unchanged — Rust resolves the extern declaration to the
// `#[no_mangle]` definition below via the linker, same as it would for a
// declaration/definition split across two C translation units.
// ===========================================================================
const CPU_LOCAL_PAGE_SIZE: u64 = 4096;
const SSTATUS_SIE: u64 = 1 << 1;

#[inline(always)]
fn read_tp() -> u64 {
    let tp: u64;
    // SAFETY: reads the `tp` register, which always holds this hart's
    // per-cpu block pointer once boot has installed it.
    unsafe {
        core::arch::asm!("mv {0}, tp", out(reg) tp, options(nomem, nostack, preserves_flags));
    }
    tp
}
#[inline(always)]
fn read_sstatus() -> u64 {
    let v: u64;
    // SAFETY: reads a CSR; no side effects.
    unsafe {
        core::arch::asm!("csrr {0}, sstatus", out(reg) v, options(nomem, nostack, preserves_flags));
    }
    v
}
#[inline(always)]
fn intr_off_raw() {
    // SAFETY: clears the supervisor interrupt-enable bit; always valid.
    unsafe {
        core::arch::asm!("csrc sstatus, {0}", in(reg) SSTATUS_SIE, options(nomem, nostack, preserves_flags));
    }
}
#[inline(always)]
fn intr_on_raw() {
    // SAFETY: sets the supervisor interrupt-enable bit; always valid.
    unsafe {
        core::arch::asm!("csrs sstatus, {0}", in(reg) SSTATUS_SIE, options(nomem, nostack, preserves_flags));
    }
}
#[inline(always)]
fn intr_get_raw() -> bool {
    (read_sstatus() & SSTATUS_SIE) != 0
}
#[inline(always)]
fn mycpu_local() -> *mut crate::bindings::cpu_local {
    read_tp() as *mut crate::bindings::cpu_local
}

/// Current hart's cpu index, derived from `tp`'s offset into the per-cpu
/// page (mirrors the `cpuid()` static-inline from `kernel/inc/smp/percpu.h`).
pub(crate) fn xv6_cpuid() -> c_int {
    let off = read_tp() & (CPU_LOCAL_PAGE_SIZE - 1);
    (off / core::mem::size_of::<crate::bindings::cpu_local>() as u64) as c_int
}

/// Disable interrupts, tracking nesting depth (mirrors `push_off()`).
#[no_mangle]
pub extern "C" fn xv6_push_off() {
    let old = intr_get_raw();
    if old {
        intr_off_raw();
    }
    // SAFETY: `mycpu_local()` points at this hart's per-cpu block, mapped
    // for the entire kernel lifetime; only this hart touches its own noff.
    unsafe {
        let c = mycpu_local();
        let noff = (*c).noff;
        if noff == 0 {
            (*c).intena = if old { 1 } else { 0 };
        }
        (*c).noff = noff + 1;
    }
}

/// Re-enable interrupts once the nesting depth reaches zero (mirrors
/// `pop_off()`).
#[no_mangle]
pub extern "C" fn xv6_pop_off() {
    // SAFETY: see `xv6_push_off`.
    unsafe {
        let c = mycpu_local();
        let n = (*c).noff - 1;
        (*c).noff = n;
        if n == 0 && (*c).intena != 0 {
            intr_on_raw();
        }
    }
}

// --- Intrusive doubly-linked list primitives (kernel/inc/list.h
// static-inlines, ported once for every mm/proc consumer). ----------------

pub(crate) fn xv6_list_init(entry: *mut ListNode) {
    // SAFETY: `entry` is a valid, exclusively-accessible `ListNode`.
    unsafe {
        (*entry).next = entry;
        (*entry).prev = entry;
    }
}

pub(crate) fn xv6_list_is_empty(head: *const ListNode) -> c_int {
    // SAFETY: `head` is a valid, initialized `ListNode`.
    unsafe { if (*head).next == head as *mut ListNode { 1 } else { 0 } }
}

pub(crate) fn xv6_list_is_detached(entry: *const ListNode) -> c_int {
    // SAFETY: `entry` is a valid, initialized `ListNode`.
    unsafe {
        let p = (*entry).prev;
        let n = (*entry).next;
        if p == entry as *mut ListNode && n == entry as *mut ListNode { 1 } else { 0 }
    }
}

pub(crate) fn xv6_list_detach(entry: *mut ListNode) {
    // SAFETY: `entry` is currently linked into some list (or self-linked);
    // unlinking it and re-initializing it as a singleton is always sound.
    unsafe {
        let p = (*entry).prev;
        let n = (*entry).next;
        (*p).next = n;
        (*n).prev = p;
    }
    xv6_list_init(entry);
}

/// Not exported: no caller (C or Rust) needs it standalone, only
/// `xv6_list_push_front`/`xv6_list_push_back` below.
fn list_insert_after(prev: *mut ListNode, new: *mut ListNode) {
    // SAFETY: `prev` is a linked (or head) node; `new` is detached storage
    // the caller owns exclusively.
    unsafe {
        let nxt = (*prev).next;
        (*new).next = nxt;
        (*new).prev = prev;
        (*nxt).prev = new;
        (*prev).next = new;
    }
}

pub(crate) fn xv6_list_push_front(head: *mut ListNode, entry: *mut ListNode) {
    list_insert_after(head, entry);
}

#[no_mangle]
pub extern "C" fn xv6_list_push_back(head: *mut ListNode, entry: *mut ListNode) {
    // SAFETY: `head` is a valid, initialized `ListNode`.
    unsafe { list_insert_after((*head).prev, entry) };
}

pub(crate) fn xv6_list_pop_front(head: *mut ListNode) -> *mut ListNode {
    if xv6_list_is_empty(head) != 0 {
        return ptr::null_mut();
    }
    // SAFETY: `head` is non-empty, so `.next` is a live linked node.
    let first = unsafe { (*head).next };
    xv6_list_detach(first);
    first
}

pub(crate) fn xv6_list_first(head: *const ListNode) -> *mut ListNode {
    if xv6_list_is_empty(head) != 0 {
        ptr::null_mut()
    } else {
        // SAFETY: just proved non-empty above.
        unsafe { (*head).next }
    }
}

pub(crate) fn xv6_list_last(head: *const ListNode) -> *mut ListNode {
    if xv6_list_is_empty(head) != 0 {
        ptr::null_mut()
    } else {
        // SAFETY: just proved non-empty above.
        unsafe { (*head).prev }
    }
}

// ===========================================================================
// `container_of` / `Errno` / `result_to_neg_errno` — re-exported from
// `crate::kstd` (P3-CS1 centralization), which is now their canonical
// home. Kept re-exported here under their original `mm::cffi::*` paths so
// every existing `mm` call site (`pcache.rs`, `slab.rs`, `vm.rs`, ...)
// needed zero changes. See `kstd.rs`'s module doc for the full history:
// this generic `container_of` fills the gap `machine::list_container_of`
// (the `bindings::list_node_t`-specific one, still used crate-wide as-is)
// doesn't cover — any other intrusive-member type (`ListNode`, `rb_node`,
// …) — and `Errno` is `mm`'s "0 / value on success, negative `E*` on
// failure" `c_int` ABI convention, deliberately not a general `errno.h`
// mirror.
pub use crate::kstd::{container_of, result_to_neg_errno, Errno};

// ===========================================================================
// Compile-time guards
// ===========================================================================
//
// These const-evaluations fail compilation if a future bindgen update
// changes the canonical layout. They also keep `c_char` / `c_void`
// "used" so a no-import build still compiles cleanly.
const _: () = {
    // `ListNode` is now `crate::list::ListNode` (re-export, see above),
    // whose own `const _` block already cross-checks it directly against
    // `bindings::list_node_t`. The literal check below is a cheap
    // redundant local sanity net, not the layout gate itself.
    assert!(core::mem::size_of::<ListNode>() == 16);
    assert!(core::mem::align_of::<ListNode>() == 8);
    // Opaque, ZST.
    assert!(core::mem::size_of::<Spinlock>() == 0);
};

// Suppress unused-import warnings if the cffi consumer set happens to
// not need them in a particular build configuration.
#[allow(dead_code)]
const _USED: (Option<*const c_void>, Option<c_char>, Option<c_int>) = (None, None, None);
