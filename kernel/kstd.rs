//! Kernel "std"/prelude: the one home for small, low-level utilities that
//! were previously reimplemented independently in dozens of files across
//! the crate (the Linux-style `ERR_PTR` error-pointer encoding,
//! `container_of`, this crate's negative-`errno` `c_int`/`u64` ABI
//! convention, the `kassert!` call-site marker, ...). Centralizing them
//! here (Phase 3 wave P3-CS1) is a direct response to the standing
//! project directive: "centralize the libc or rust std like tools, do
//! not scatter them all over the place."
//!
//! # Why this existed as 30 copies
//!
//! Every item below had 2 or more byte-for-byte-identical copies
//! scattered through `bufcache.rs`, `console.rs`, `dev/*.rs`, `exec.rs`,
//! `lock/*.rs`, `proc/*.rs`, `tty/*.rs`, and `vfs/**/*.rs` — a direct
//! side effect of this crate's file-scoped, "keep every file's C-ABI
//! surface self-contained" porting convention from the mechanical
//! C-to-Rust wave-by-wave port (see e.g. the historical doc comment on
//! `vfs/inode.rs`'s local `err_ptr` family, which explicitly says it's
//! "reimplemented locally ... rather than reusing
//! `kernel/proc/access.rs`'s equivalents"). That convention was
//! reasonable *during* the port — it kept each wave's touch-list small
//! and avoided premature cross-file coupling while whole subsystems were
//! still partly C — but now that the kernel is 100% Rust the duplication
//! is pure entropy: 30 files defined their own copy of the same
//! four-function `ERR_PTR` family alone, all verified byte-identical in
//! semantics before this module was created (see the P3-CS1 worker
//! report). New code should reach for this module first; old duplicates
//! are folded in incrementally as files are touched (see `RUST_REWRITE.md`
//! for the migration/handoff list).
//!
//! # Result-first: `ERR_PTR` is a transitional C-ABI shim, not the target
//!
//! **Standing directive (mid-P3-CS1):** new and migrated Rust-internal
//! fallible code should return [`KResult<T>`] (`Result<T, Errno>`), not
//! an `ERR_PTR`-encoded pointer. The `ERR_PTR` family below is
//! centralized because it *already existed* 30 times over and a raw
//! pointer is still what a `#[no_mangle] extern "C"` boundary function
//! or an on-disk/ABI-fixed storage slot must produce — but it is a
//! **transitional shim for those boundaries only**, kept here so the 30
//! duplicate copies collapse to one instead of being perpetuated.
//! [`result_to_errptr`]/[`errptr_to_result`] convert between the two
//! encodings exactly at such a boundary. Bulk-converting the 30
//! pre-existing call sites from `ERR_PTR` to `Result` internally is
//! future-wave work (see `RUST_REWRITE.md`'s handoff list) — this wave
//! converts opportunistically where a local helper's callers are few and
//! entirely Rust-internal, and otherwise centralizes the pointer-encoding
//! helpers as-is so they stop being copy-pasted.
//!
//! # Contents
//!
//! * [`Errno`] / [`KResult`] / [`result_to_neg_errno`] / [`neg_errno`] —
//!   this crate's canonical fallible-return vocabulary. `KResult<T>` is
//!   the target shape for Rust-internal fallible functions; `Errno` only
//!   enumerates the handful of values `mm` originally needed —
//!   deliberately not a full `errno.h` mirror; broadening it into a
//!   crate-wide errno type is future work, not this wave's. Canonical
//!   home moved here from `mm::cffi`/`mm::sysmm`, which re-export for
//!   their existing callers.
//! * [`err_ptr`] / [`is_err`] / [`is_err_or_null`] / [`ptr_err`] /
//!   [`is_err_value`] / [`result_to_errptr`] / [`errptr_to_result`] — the
//!   Linux-style `ERR_PTR` error-pointer encoding (`kernel/inc/errno.h`'s
//!   `MAX_ERRNO`/`IS_ERR_VALUE`/`ERR_PTR`/`PTR_ERR`/`IS_ERR`/
//!   `IS_ERR_OR_NULL` macros) — **transitional**, see above. A C-ABI
//!   function that would normally return `*mut T` on success but also
//!   needs to report a `-errno` failure packs the negative errno into
//!   the top of the pointer's bit pattern instead of using an
//!   out-parameter. Valid kernel pointers never land in
//!   `[usize::MAX - 4094, usize::MAX]` (the top ~4KiB of the address
//!   space is never a real mapping), so `is_err` distinguishes the two
//!   cases by value alone.
//! * [`container_of`] — generic intrusive-member-to-payload pointer
//!   arithmetic (canonical home moved here from `mm::cffi`, which
//!   re-exports it for its existing callers).
//! * [`macro@kassert`] — the crate's `assert!`-with-a-kernel-panic-path
//!   call-site marker, matching the most common of the ~15 near-identical
//!   per-file copies (`if !(cond) { xv6_panic(msg) }`). Two other call
//!   targets exist in the tree (`proc/{rq,sched,thread}.rs`'s
//!   `kpanic!`-based two-arg form, and `irq/trap.rs`'s three-arg
//!   `kassert_fail`-based form) — both are semantically different from
//!   this one (different panic entry points/arity) and were deliberately
//!   left as-is rather than force-unified; see the P3-CS1 report.
//!
//! [`crate::u`] (defined in `lib.rs`, not here) is the crate's other
//! call-site-marker macro (`u! { EXPR }` expands to `unsafe { EXPR }`) —
//! mentioned here only because it's part of the same "one macro per
//! concept" cleanup; its handful of remaining `kernel/lock/*.rs` local
//! copies were deleted as part of this same wave.

#![allow(dead_code)]

use core::ffi::{c_char, c_int};

// ===========================================================================
// `ERR_PTR` family — TRANSITIONAL C-ABI/storage-boundary shim.
// ===========================================================================
//
// See the module doc's "Result-first" section: prefer [`KResult<T>`] for
// any new or migrated Rust-internal fallible return. This family exists
// so the pointer encoding a `#[no_mangle] extern "C"` boundary (or an
// on-disk/ABI-fixed slot) still needs has exactly one implementation
// instead of 30.

/// `MAX_ERRNO` (`kernel/inc/errno.h`): the largest magnitude negative
/// errno an `ERR_PTR`-encoded pointer can carry.
pub const MAX_ERRNO: isize = 4095;

/// `IS_ERR_VALUE(x)` (`kernel/inc/errno.h`) on a raw pointer bit pattern:
/// true iff `p` falls in the reserved `[-MAX_ERRNO, -1]` window.
#[inline(always)]
pub fn is_err_value(p: usize) -> bool {
    p >= (-(MAX_ERRNO)) as usize
}

/// `ERR_PTR(errno)` (`kernel/inc/errno.h`): pack a negative `errno` into
/// a pointer value. `errno` is expected to already be negative (callers
/// pass e.g. `-EINVAL`, matching every existing call site in the crate).
#[inline(always)]
pub fn err_ptr<T>(errno: c_int) -> *mut T {
    errno as isize as *mut T
}

/// `IS_ERR(ptr)` (`kernel/inc/errno.h`): true iff `p` is an
/// `ERR_PTR`-encoded error, not a real pointer.
#[inline(always)]
pub fn is_err<T>(p: *const T) -> bool {
    is_err_value(p as usize)
}

/// `IS_ERR_OR_NULL(ptr)` (`kernel/inc/errno.h`).
#[inline(always)]
pub fn is_err_or_null<T>(p: *const T) -> bool {
    p.is_null() || is_err(p)
}

/// `PTR_ERR(ptr)` (`kernel/inc/errno.h`): reinterpret an `ERR_PTR`-encoded
/// pointer as the negative errno it carries. Only meaningful when
/// `is_err(p)` is true.
#[inline(always)]
pub fn ptr_err<T>(p: *const T) -> c_int {
    p as isize as c_int
}

// ===========================================================================
// `container_of` — the one canonical intrusive-node-to-payload helper.
// ===========================================================================
//
// Before this was consolidated (first into `mm::cffi::container_of`, now
// re-homed here), `pcache.rs` (three call sites: pcache/list, node/LRU,
// node/rb-tree), `slab.rs` (one: cache/registry-list), and `vm.rs` (two,
// inline: rb-tree node/vma) each hand-rolled the identical
// `(ptr as *mut u8).wrapping_sub(offset) as *mut T` byte-arithmetic
// pattern. The crate also has a canonical `container_of` specifically for
// `bindings::list_node_t` (`crate::machine::list_container_of`, used
// crate-wide by `proc`/`ll`/`lock` as well as `vm.rs` — left as its own
// thing to avoid adding a second list_node_t-shaped duplicate). This
// generic version is for every other intrusive-member type (`ListNode`,
// `rb_node`, …): the computation is pure pointer arithmetic and doesn't
// care about the member's Rust type, only its byte offset within `T`.
#[inline(always)]
pub fn container_of<T, M>(member: *mut M, member_offset: usize) -> *mut T {
    (member as *mut u8).wrapping_sub(member_offset) as *mut T
}

// ===========================================================================
// `Errno` — this crate's canonical negative-errno type for internal
// (non-`#[no_mangle]`) helpers.
// ===========================================================================
//
// The C ABI convention throughout this crate is "0 on success, negative
// `E*` value on failure" (`bindings::E*` are the positive musl-libc
// numbers from `kernel/inc/errno.h`). Internal helpers that used to
// return a raw `c_int` encoding that convention can instead return
// `Result<T, Errno>`; the `#[no_mangle]` C-ABI boundary function converts
// back to a negative `c_int` exactly once, via [`Errno::neg`]. Only the
// handful of `E*` values `mm` originally needed are represented — this
// is deliberately not a general `errno.h` mirror (see the module doc).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Errno {
    Inval,
    NoMem,
    Fault,
    Access,
    BadF,
    Range,
    NameTooLong,
    /// `EMFILE`: per-process open-file-descriptor limit reached (added for
    /// P3-CS3, `vfs/fdtable.rs`'s fd-allocation cluster).
    MFile,
}

/// This crate's canonical fallible-return type for Rust-internal
/// helpers: `Result<T, Errno>`. This is the **target idiom** — prefer it
/// over the `ERR_PTR` pointer encoding above for any function whose
/// callers are Rust and don't need a raw pointer's worth of ABI (see the
/// module doc's "Result-first" section). Convert to/from the transitional
/// pointer encoding only at an actual C-ABI/storage boundary, via
/// [`result_to_errptr`]/[`errptr_to_result`], or to the raw `c_int` ABI
/// return convention via [`result_to_neg_errno`].
pub type KResult<T> = Result<T, Errno>;

impl Errno {
    /// The positive `E*` value (e.g. `EINVAL`), as `bindgen` emits it.
    #[inline]
    pub const fn raw(self) -> c_int {
        match self {
            Errno::Inval => crate::bindings::EINVAL as c_int,
            Errno::NoMem => crate::bindings::ENOMEM as c_int,
            Errno::Fault => crate::bindings::EFAULT as c_int,
            Errno::Access => crate::bindings::EACCES as c_int,
            Errno::BadF => crate::bindings::EBADF as c_int,
            Errno::Range => crate::bindings::ERANGE as c_int,
            Errno::NameTooLong => crate::bindings::ENAMETOOLONG as c_int,
            Errno::MFile => crate::bindings::EMFILE as c_int,
        }
    }

    /// The `-errno` value used as a raw C ABI return code.
    #[inline]
    pub const fn neg(self) -> c_int {
        -self.raw()
    }
}

/// Convert a `Result<T, Errno>` into this crate's "0 / value on success,
/// negative errno on failure" `c_int` ABI convention, for the common case
/// where `T` doesn't carry a value the caller needs back through the
/// return slot (e.g. `Result<(), Errno>`, or callers that discard `T`).
#[inline]
pub fn result_to_neg_errno<T>(r: Result<T, Errno>) -> c_int {
    match r {
        Ok(_) => 0,
        Err(e) => e.neg(),
    }
}

/// Negate a positive `E*` value into the `-errno` `u64` this crate's
/// syscall-return-value ABI expects (`mm::sysmm`'s original use case:
/// syscall handlers that return `u64`, not `c_int`).
#[inline]
pub fn neg_errno(e: i32) -> u64 {
    (-e) as i64 as u64
}

// ===========================================================================
// `KResult<T>` <-> `ERR_PTR` bridging — use exactly at the boundary where
// a Rust-internal `Result` meets a raw-pointer C-ABI/storage convention.
// ===========================================================================

/// Convert a `KResult<*mut T>` into the transitional `ERR_PTR` pointer
/// encoding, for returning a fallible Rust-internal result across a
/// boundary that still speaks raw `*mut T` (a `#[no_mangle]` C-ABI
/// function, or a call site not yet converted to `Result`).
#[inline]
pub fn result_to_errptr<T>(r: KResult<*mut T>) -> *mut T {
    match r {
        Ok(p) => p,
        Err(e) => err_ptr(e.neg()),
    }
}

/// Convert an `ERR_PTR`-encoded pointer into a `Result<*mut T, c_int>`,
/// the inverse of packing a pointer with [`err_ptr`]. Returns the raw
/// negative `c_int` errno rather than [`Errno`] — a pointer's encoded
/// errno can be any `E*` value, not just the handful [`Errno`]
/// enumerates, so reconstructing an `Errno` from an arbitrary `ERR_PTR`
/// isn't always possible; widening `Errno` into a full mirror (so this
/// could return `KResult<*mut T>` instead) is future work, not this
/// wave's. Does *not* treat a null pointer as an error — pair with an
/// explicit null check first if the caller's convention is
/// `IS_ERR_OR_NULL`.
#[inline]
pub fn errptr_to_result<T>(p: *mut T) -> Result<*mut T, c_int> {
    if is_err(p) { Err(ptr_err(p)) } else { Ok(p) }
}

// ===========================================================================
// `kassert!` — canonical panic-on-false call-site marker.
// ===========================================================================

unsafe extern "C" {
    /// The kernel panic entry point (`#[no_mangle]`,
    /// `kernel/proc/proc_shims.rs`). Declared once here so `kassert!`
    /// doesn't require every call-site file to carry its own copy of
    /// this extern block just to use the macro.
    pub(crate) safe fn xv6_panic(msg: *const c_char) -> !;
}

/// `assert!`-with-kernel-panic-path call-site marker:
/// `kassert!(cond, "msg")` expands to
/// `if !(cond) { xv6_panic("msg\0".as_ptr() as *const c_char) }`,
/// mirroring the C `assert(expr, fmt)` macro (`kernel/inc/printf.h`)
/// with its format arguments dropped (every migrated call site already
/// passed a fixed message, not a dynamic `printf`-style format string).
///
/// `$msg` must be a string literal (or `concat!`-composable literal
/// expression) — it is spliced into `concat!` at macro-expansion time.
#[macro_export]
macro_rules! kassert {
    ($cond:expr, $msg:expr) => {
        if !($cond) {
            $crate::kstd::xv6_panic(
                ::core::concat!($msg, "\0").as_ptr() as *const ::core::ffi::c_char
            )
        }
    };
}

// ===========================================================================
// Compile-time guard: keep `c_char` "used" even in configurations where
// only a subset of this module's items get referenced.
// ===========================================================================
const _: () = {
    let _ = core::mem::size_of::<c_char>();
};
