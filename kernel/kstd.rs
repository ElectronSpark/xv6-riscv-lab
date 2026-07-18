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
use core::ptr::NonNull;

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
    /// `ENOENT`: no such file or directory (added for P3-CS4,
    /// `vfs/vfs_syscall.rs`'s path-resolution cluster).
    NoEnt,
    /// `ENOTDIR`: not a directory (added for P3-CS4, `sys_vfs_chdir`).
    NotDir,
    /// `EINTR`: interrupted system call (added for P3-CS5,
    /// `vfs/vfs_syscall.rs`'s `sys_vfs_poll`/`sys_getdents`-adjacent
    /// signal-pending checks).
    Intr,
    /// `ELOOP`: too many symbolic links encountered (added for P3-CS5,
    /// `sys_vfs_stat`/`sys_vfs_lstat`/`sys_vfs_open`'s symlink-following
    /// loops).
    Loop,
    /// `EISDIR`: is a directory (added for P3-CS5, `sys_vfs_open`).
    IsDir,
    /// `EPERM`: operation not permitted (added for P3-CS5,
    /// `sys_vfs_link`'s directory-hardlink rejection).
    Perm,
    /// `ENOSYS`: function not implemented (added for P3-CS5,
    /// `sys_statfs`'s no-superblock fallback).
    NoSys,
    /// `EEXIST`: file already exists (added for P3-CS5, `sys_vfs_open`'s
    /// `O_CREAT | O_EXCL` path).
    Exist,
    /// `ENODEV`: no such device (added for P3-CS6, `vfs/file.rs`'s
    /// `open_cdev`/`vfs_ioctl` device-lookup-miss paths).
    NoDev,
    /// `ENXIO`: no such device or address (added for P3-CS6,
    /// `vfs/file.rs`'s `vfs_fileopen` socket/named-pipe-via-open
    /// rejection).
    NxIo,
    /// `ENOTTY`: inappropriate ioctl for device (added for P3-CS6,
    /// `vfs/file.rs`'s `vfs_ioctl` fallback).
    NotTy,
    /// `EOPNOTSUPP`: operation not supported (added for P3-CS6,
    /// `vfs/file.rs`'s block-device/no-driver-callback read/write paths).
    OpNotSupp,
    /// `ESPIPE`: illegal seek (added for P3-CS6, `vfs/file.rs`'s
    /// `vfs_filelseek` non-regular-file/non-seekable paths).
    SPipe,
    /// `EADDRINUSE`: address already in use (added for P3-CS6,
    /// `vfs/file.rs`'s `vfs_sockalloc` duplicate-socket-tuple check).
    AddrInUse,
    /// `EAGAIN`: transient race, retry (added for P3-CS7,
    /// `vfs/inode.rs`'s `vfs_namei`/`vfs_namei_once` mount-traversal
    /// retry loop — an inode/mount-root was caught mid-destroy; the
    /// caller yields and retries the whole lookup).
    Again,
    /// `EXDEV`: cross-device link/rename/lock not supported (added for
    /// P3-CS8, `vfs/inode.rs`'s `vfs_link`/`vfs_move`/
    /// `vfs_ilock_two_directories` cross-filesystem precondition checks).
    XDev,
    /// `ESRCH`: no such process (added for P3-CS10, `proc/thread_group.rs`'s
    /// `get_thread_group` tgid-lookup-miss paths).
    Srch,
    /// `EIO`: I/O error (added for P3-10a, the `vfs_file_ops` ->
    /// `FileOps`-trait pilot — the xv6fs/tmpfs/pty file-op bodies'
    /// inactive-pcache / torn-down-pair failure paths, now that those
    /// implementors return [`KResult`] natively instead of a raw
    /// negative `isize`).
    Io,
    /// `EFBIG`: file too large (added for P3-10a, the xv6fs/tmpfs
    /// `FileOps::write` max-file-size checks).
    FBig,
    /// `ENOSPC`: no space left on device (added for P3-10a, the xv6fs
    /// `FileOps::write` block-allocation-failure path).
    NoSpc,
    /// `EBUSY`: device or resource busy (added for P3-10b, the
    /// `vfs_inode_ops` -> `InodeOps`-trait wave — tmpfs's
    /// `move_`/rename busy-target check, now that the implementor
    /// returns [`KResult`] natively).
    Busy,
    /// Passthrough for an already-computed raw `-errno` `c_int` obtained
    /// from a cross-file boundary this cluster doesn't own — e.g. an
    /// `ERR_PTR`-encoded pointer from `vfs/inode.rs`
    /// (`vfs_nameiparent`/`vfs_mkdir`/`vfs_mknod`/...), or another
    /// file's own already-negative-errno `c_int` return
    /// (`vfs_filestat`/`vfs_inode_get_ref`/...). `n` is the raw negative
    /// value itself (`PTR_ERR`'s result, or the `c_int` as-is), not a
    /// positive `E*` code — added for P3-CS4 (`vfs/vfs_syscall.rs`) so
    /// composing a [`KResult`] from such a boundary never has to guess or
    /// narrow an arbitrary errno into one of the enumerated variants
    /// above (which would either lose information or panic on an
    /// unmapped value). See [`Errno::raw`]/[`Errno::neg`] for the exact
    /// round-trip.
    Raw(c_int),
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
            Errno::NoEnt => crate::bindings::ENOENT as c_int,
            Errno::NotDir => crate::bindings::ENOTDIR as c_int,
            Errno::Intr => crate::bindings::EINTR as c_int,
            Errno::Loop => crate::bindings::ELOOP as c_int,
            Errno::IsDir => crate::bindings::EISDIR as c_int,
            Errno::Perm => crate::bindings::EPERM as c_int,
            Errno::NoSys => crate::bindings::ENOSYS as c_int,
            Errno::Exist => crate::bindings::EEXIST as c_int,
            Errno::NoDev => crate::bindings::ENODEV as c_int,
            Errno::NxIo => crate::bindings::ENXIO as c_int,
            Errno::NotTy => crate::bindings::ENOTTY as c_int,
            Errno::OpNotSupp => crate::bindings::EOPNOTSUPP as c_int,
            Errno::SPipe => crate::bindings::ESPIPE as c_int,
            Errno::AddrInUse => crate::bindings::EADDRINUSE as c_int,
            Errno::Again => crate::bindings::EAGAIN as c_int,
            Errno::XDev => crate::bindings::EXDEV as c_int,
            Errno::Srch => crate::bindings::ESRCH as c_int,
            Errno::Io => crate::bindings::EIO as c_int,
            Errno::FBig => crate::bindings::EFBIG as c_int,
            Errno::NoSpc => crate::bindings::ENOSPC as c_int,
            Errno::Busy => crate::bindings::EBUSY as c_int,
            // `n` is already the raw negative `-errno` value; `raw()`'s
            // contract is to return the *positive* `E*` code, so negate
            // it back. `neg()` (below, `-self.raw()`) then recovers `n`
            // exactly: `-(-n) == n`, a lossless round-trip with no
            // separate special case needed there.
            Errno::Raw(n) => -n,
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

/// Decode a raw "value on success, negative errno on failure" `c_int`
/// (obtained from a cross-file boundary this cluster doesn't own, e.g.
/// `tty_ioctl`/`tty_read`) into a [`KResult<c_int>`]. The negative value
/// is carried verbatim in [`Errno::Raw`] (`Raw(n).neg() == n`), so
/// re-encoding at a dispatch boundary reproduces the original `c_int`
/// exactly — the `c_int` twin of `tty/ptmx.rs`'s isize-flavored
/// `count_result` (added for P3-10c, the device ops-table trait wave).
#[inline]
pub fn cint_result(ret: c_int) -> KResult<c_int> {
    if ret < 0 {
        Err(Errno::Raw(ret))
    } else {
        Ok(ret)
    }
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

/// The kernel panic entry point (`kernel/proc/proc_shims.rs`).
/// Re-exported once here so `kassert!` doesn't require every call-site
/// file to carry its own import just to use the macro.
pub(crate) use crate::proc::proc_shims::xv6_panic;

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
// `GenTable<T, CAP, TAG>` — hand-rolled generational slotmap  [N-R6a pilot]
// ===========================================================================
//
// The reusable primitive of the full-Rust-native proc/thread redesign
// (`RUST_NATIVE_REDESIGN.md` §3a/§5b). A fixed-capacity, `#![no_std]`,
// **zero-dependency** (the kernel has no external crates and keeps it that
// way) generational table that turns the C-transliteration "raw `*mut T`
// handle you hope is still live" pattern into a checked lookup:
//
// * Objects are stored as a **stable pointer** ([`NonNull<T>`]) — the table
//   never holds `T` by value, so the pointee keeps its existing slab/page
//   allocation and **never moves**. This is mandatory for the asm-adjacent
//   proc/thread objects (`current` = a raw `*mut Thread` the context switch
//   dereferences); a `Vec`-backed slotmap that relocates on growth is
//   forbidden for them. Session (this pilot's consumer) has the same
//   property for free.
// * A handle is a `Copy` typed key [`GenKey<TAG>`] = `{ index, generation }`,
//   **not** a pointer. The `TAG` const parameter makes keys of different
//   families (`Sid` vs a future `Pid`/`Tid`/`Pgid`) distinct incompatible
//   types even though they share this one implementation — a stale `Sid`
//   can never be silently used where a `Pid` is expected.
// * **Liveness = generation check.** [`insert`](GenTable::insert) bumps the
//   chosen slot's generation and stamps it into the returned key;
//   [`remove`](GenTable::remove) frees the slot and bumps its generation
//   again, so **every outstanding key to that object instantly goes stale**.
//   A subsequent [`get`](GenTable::get) with a stale key sees
//   `slot.generation != key.generation` and returns `None`. A
//   use-after-free that would be undefined behaviour through a raw `*mut T`
//   becomes a checked `None` — exactly the class of silent UB the whole
//   Rust-native effort exists to eliminate.
//
// # Generation wrap
//
// `generation` is a `u32` advanced with `wrapping_add`. A stale key could
// only alias a live slot after the *same slot index* is inserted-then-
// removed `2^32` times (≈4.3 billion reuse cycles) with that one stale key
// retained the whole time. At kernel scale (a session/pid is created on
// the order of once per `setsid`/`fork`) this is unreachable; the wrap is
// documented rather than defended, matching every production slotmap.
//
// # Synchronisation — NONE; the table is *not* internally synchronized
//
// `GenTable` has no interior locking. It is designed to live **behind the
// subsystem's existing lock**: mutators ([`insert`](GenTable::insert)/
// [`remove`](GenTable::remove)/[`remove_ptr`](GenTable::remove_ptr)) require
// exclusive access (`&mut self`), readers ([`get`](GenTable::get)/
// [`contains`](GenTable::contains)/[`for_each`](GenTable::for_each)) require
// shared access (`&self`). A `static` instance therefore lives in an
// `UnsafeCell` + `unsafe impl Sync` newtype (exactly like `proc_shims`'s
// `PROC_TABLE`), and every access is taken while holding the owning
// subsystem lock — for the session pilot that is `pid_lock` (write for
// mutation, read for scan), the identical discipline that protected the
// `session_list` this table replaces.
//
// ## Freeze / `noalias` note (memory `freeze-noalias-hazard`)
//
// `GenTable` is `Freeze` (its `Slot`s are plain `{ u32, Option<NonNull<T>> }`
// — no `UnsafeCell` interior). The N-R1 pilot proved that taking `&T` to a
// `Freeze` value that *another hart mutates during the borrow* lets LLVM
// hoist a field read out of a loop (lost wakeup / boot hang). That hazard
// does **not** apply here because the owning rwlock makes access exclusive:
// a `&self` scan runs under `pid_rlock`, which excludes every `&mut self`
// mutator (they hold `pid_wlock`), so no other hart writes the table while a
// shared reference into it is live. The reference and the writer are never
// simultaneously live — the precondition for the hazard is absent. The
// table also only ever hands out raw `NonNull<T>` (never `&T`/`&Session`)
// to its stored objects, so it introduces no new `&Freeze`-into-a-loop of
// the *pointee* either.

/// A typed, `Copy` generational handle into a [`GenTable`] of the same
/// `TAG`. `index` selects the slot; `generation` is checked against the
/// slot's live generation on every lookup so a handle to a removed (and
/// possibly recycled) object resolves to `None` instead of dangling.
///
/// The `TAG` const parameter is a compile-time family discriminator: a
/// `type Sid = GenKey<SID_TAG>` is a different, non-interchangeable type
/// from any other family's key, so keys cannot be crossed between tables.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct GenKey<const TAG: u64> {
    index: u32,
    generation: u32,
}

impl<const TAG: u64> GenKey<TAG> {
    /// The slot index this key refers to.
    #[inline]
    pub const fn index(self) -> u32 {
        self.index
    }
    /// The generation this key was stamped with (matched against the
    /// slot's live generation by [`GenTable::get`]).
    #[inline]
    pub const fn generation(self) -> u32 {
        self.generation
    }
}

/// One table slot: the live `generation` plus the stored stable pointer
/// (`None` when the slot is free). Kept private — callers only ever see
/// [`GenKey`]s and [`NonNull<T>`]s.
struct Slot<T> {
    generation: u32,
    ptr: Option<NonNull<T>>,
}

impl<T> Slot<T> {
    /// A free slot at generation 0. Used as the array-repeat initializer in
    /// [`GenTable::new`]; being an associated `const` (not a runtime value)
    /// lets `[Slot::<T>::EMPTY; CAP]` build a const array without requiring
    /// `Slot<T>: Copy`.
    const EMPTY: Self = Slot { generation: 0, ptr: None };
}

/// A fixed-capacity generational slotmap of stable `NonNull<T>` pointers.
/// See the module section doc above for the full design, semantics, and
/// synchronisation contract (the table is **not** internally synchronized —
/// it lives behind the owning subsystem's lock).
pub struct GenTable<T, const CAP: usize, const TAG: u64> {
    slots: [Slot<T>; CAP],
}

impl<T, const CAP: usize, const TAG: u64> GenTable<T, CAP, TAG> {
    /// Compile-time layout guards, forced to evaluate by [`new`](Self::new)
    /// at every monomorphization: the key must be exactly two `u32`s, the
    /// slot must be niche-optimized to pointer-plus-generation (so a stored
    /// `Option<NonNull<T>>` costs no discriminant word), and `CAP` must fit
    /// the `u32` index space.
    const LAYOUT_OK: () = {
        assert!(
            core::mem::size_of::<GenKey<TAG>>() == 8,
            "GenKey must be exactly two u32 fields (8 bytes)"
        );
        assert!(
            core::mem::align_of::<GenKey<TAG>>() == 4,
            "GenKey must be u32-aligned"
        );
        assert!(
            core::mem::size_of::<Slot<T>>() <= 2 * core::mem::size_of::<usize>(),
            "Slot<T> must be niche-optimized (pointer + generation, no Option discriminant)"
        );
        assert!(
            CAP <= u32::MAX as usize,
            "GenTable capacity must fit the u32 index space"
        );
    };

    /// Construct an empty table. `const` so it can initialize a `static`
    /// (all slots free at generation 0 → the array is all-zero/`None` and
    /// lands in BSS, no data-segment cost).
    pub const fn new() -> Self {
        // Force the compile-time layout guards for this monomorphization.
        let _: () = Self::LAYOUT_OK;
        GenTable {
            slots: [Slot::<T>::EMPTY; CAP],
        }
    }

    /// Insert `p`, returning its fresh key, or `None` if the table is full.
    ///
    /// Takes the first free slot (linear scan — inserts are rare at kernel
    /// scale), bumps that slot's generation, and stamps the new generation
    /// into the returned key so it is distinct from every key previously
    /// handed out for the same slot index.
    ///
    /// Requires `&mut self`: the caller must hold the owning subsystem lock
    /// exclusively (the session pilot: `pid_wlock`).
    pub fn insert(&mut self, p: NonNull<T>) -> Option<GenKey<TAG>> {
        for (i, slot) in self.slots.iter_mut().enumerate() {
            if slot.ptr.is_none() {
                slot.generation = slot.generation.wrapping_add(1);
                slot.ptr = Some(p);
                return Some(GenKey {
                    index: i as u32,
                    generation: slot.generation,
                });
            }
        }
        None
    }

    /// Resolve `k` to its stored pointer, or `None` if the key is stale
    /// (the slot's generation has moved on because the object was removed,
    /// possibly and its slot reused) or the slot is empty / the index is
    /// out of range. This is the use-after-free → checked-`None` conversion.
    #[inline]
    pub fn get(&self, k: GenKey<TAG>) -> Option<NonNull<T>> {
        let slot = self.slots.get(k.index as usize)?;
        if slot.generation == k.generation {
            // On a generation match the slot is occupied by construction
            // (removal always bumps the generation away from any live key);
            // returning `slot.ptr` is `None`-safe regardless.
            slot.ptr
        } else {
            None
        }
    }

    /// Remove the object `k` refers to, freeing its slot and bumping the
    /// slot generation so **all** outstanding keys to it go stale. Returns
    /// the removed pointer, or `None` if `k` was already stale / empty /
    /// out of range (idempotent double-remove is a safe no-op returning
    /// `None`, never a double-free).
    ///
    /// Requires `&mut self` (owning lock held exclusively).
    pub fn remove(&mut self, k: GenKey<TAG>) -> Option<NonNull<T>> {
        let slot = self.slots.get_mut(k.index as usize)?;
        if slot.generation != k.generation || slot.ptr.is_none() {
            return None;
        }
        let p = slot.ptr.take();
        slot.generation = slot.generation.wrapping_add(1);
        p
    }

    /// Remove whichever slot currently holds pointer `p` (linear scan),
    /// bumping its generation so outstanding keys go stale. Returns the
    /// now-invalidated key, or `None` if `p` is not present.
    ///
    /// This is the pointer-keyed teardown path for consumers that hold the
    /// object as a bare `*mut T`/`NonNull<T>` (not its `GenKey`) at free
    /// time — e.g. the session pilot, whose `Session` struct is a
    /// layout-frozen `#[repr(C)]` type with no room to cache its own key,
    /// so `session_unref`'s free branch removes by the pointer it already
    /// holds. `remove(key)` above is the key-keyed twin.
    ///
    /// Requires `&mut self` (owning lock held exclusively).
    pub fn remove_ptr(&mut self, p: NonNull<T>) -> Option<GenKey<TAG>> {
        for (i, slot) in self.slots.iter_mut().enumerate() {
            if slot.ptr == Some(p) {
                let stale = GenKey {
                    index: i as u32,
                    generation: slot.generation,
                };
                slot.ptr = None;
                slot.generation = slot.generation.wrapping_add(1);
                return Some(stale);
            }
        }
        None
    }

    /// True iff `k` currently resolves to a live object.
    #[inline]
    pub fn contains(&self, k: GenKey<TAG>) -> bool {
        self.get(k).is_some()
    }

    /// Visit every live object's stable pointer, in ascending slot order.
    /// Hands out raw `NonNull<T>` (never `&T`) so the callback decides how
    /// to dereference under the caller's lock — no `&Freeze` into the loop
    /// (see the module section's freeze note).
    ///
    /// Requires `&self` (owning lock held at least for read).
    pub fn for_each(&self, mut f: impl FnMut(NonNull<T>)) {
        for slot in self.slots.iter() {
            if let Some(p) = slot.ptr {
                f(p);
            }
        }
    }
}

impl<T, const CAP: usize, const TAG: u64> Default for GenTable<T, CAP, TAG> {
    #[inline]
    fn default() -> Self {
        Self::new()
    }
}

// ===========================================================================
// Compile-time guard: keep `c_char` "used" even in configurations where
// only a subset of this module's items get referenced.
// ===========================================================================
const _: () = {
    let _ = core::mem::size_of::<c_char>();
};
