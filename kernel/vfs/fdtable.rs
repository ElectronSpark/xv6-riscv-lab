//! VFS per-thread file-descriptor table — Rust port of
//! `kernel/vfs/fdtable.c` (Phase 2 Wave 15, see
//! `docs/rustify/phase2_plan.md`).
//!
//! This is the `vfs_fdtable`: the integer-fd -> `vfs_file*` map every
//! thread carries (`fd_count`, `files[NOFILE]`, `files_bitmap`,
//! `cloexec_bitmap`, `ref_count`, `lock`). It supplies fd allocation
//! (`vfs_fdtable_alloc_fd[_from]`), fd release (`vfs_fdtable_dealloc_fd`),
//! wait-free lookup (`vfs_fdtable_get_file`), fork/clone sharing-or-deep-copy
//! (`vfs_fdtable_clone`), teardown on last reference
//! (`vfs_fdtable_put`), the `FD_CLOEXEC` flag pair
//! (`vfs_fdtable_{get,set}_fdflags`), and `execve`'s close-on-exec sweep
//! (`vfs_fdtable_close_on_exec`). The rest of `kernel/vfs/` (`fs.c`,
//! `vfs_syscall.c`, and the `tmpfs/`/`xv6fs/`/`devtmpfs/` filesystem
//! drivers) remains C for now (Waves 16-20) and calls into this file
//! exactly as any other C translation unit would; `kernel/vfs/file.rs`
//! (Wave 14) and `kernel/mm/vm.rs`/`kernel/proc/{clone,exit}.rs` already
//! call these entry points through their own local `extern "C"`
//! declarations (unchanged by this port — the C ABI is identical).
//!
//! # Synchronization strategy (hybrid RCU + spinlock, matches the C
//! original's own header comment)
//!
//! * **Readers** ([`vfs_fdtable_get_file`]): `rcu_read_lock()` +
//!   `rcu_dereference` (mirrored below as an `Acquire` load on the
//!   `files[fd]` slot, matching this crate's established
//!   `__ATOMIC_CONSUME`-has-no-Rust-equivalent -> `Acquire` mapping, see
//!   `kernel/hlist.rs`/`kernel/irq/irq_core.rs`) — a wait-free lookup that
//!   bumps the file's refcount (`vfs_fdup`) before dropping the RCU
//!   section, so the file cannot be freed out from under the caller even
//!   if a concurrent `close()` deallocates the fd immediately after.
//! * **Writers** ([`vfs_fdtable_alloc_fd_from`], [`vfs_fdtable_dealloc_fd`],
//!   [`vfs_fdtable_get_fdflags`]/[`vfs_fdtable_set_fdflags`]): the caller
//!   must hold `fdtable.lock` (a `spinlock_t`), asserted via
//!   `KSpinlock::holding()` exactly like the C `assert(spin_holding(...))`
//!   guard clauses. Pointer *publication* into `files[fd]` still goes
//!   through `rcu_assign_pointer` (`Release` store below) so concurrent
//!   RCU readers never observe a torn/partially-initialized pointer.
//! * **Cloning** ([`vfs_fdtable_clone`]): holds only the *source*
//!   fdtable's `rcu_read_lock()` while walking `src.files[]` (protects
//!   against a concurrent `close()` freeing a file mid-copy); the
//!   destination table is not yet published to any other thread, so its
//!   own fields are touched with plain (non-atomic, non-RCU) reads/writes,
//!   exactly like the C original.
//!
//! # Lock map
//!
//! `vfs_fdtable.lock` (a `spinlock_t`) is the only lock this file owns.
//! Every acquire/release pair here is either a caller-held precondition
//! (asserted, never acquired locally — [`vfs_fdtable_alloc_fd_from`],
//! [`vfs_fdtable_dealloc_fd`], [`vfs_fdtable_get_fdflags`],
//! [`vfs_fdtable_set_fdflags`]) or a single, lexically-scoped critical
//! section local to this file ([`vfs_fdtable_close_on_exec`]), so the
//! latter uses [`crate::sync::KSpinlock`] RAII (same precedent as
//! `vfs/file.rs`'s `vfs_custom_fd_alloc`/`ftable_attach`/`ftable_detach`).
//!
//! # Refcount map
//!
//! `vfs_fdtable.ref_count` (a plain C `int`) is manipulated with the same
//! `atomic_dec_unless`/plain-`atomic_add` idiom the C original uses
//! (`smp/atomic.h`'s `atomic_oper_cond`, reimplemented locally per this
//! crate's established per-file-self-contained convention — see
//! `vfs/inode.rs`'s and `vfs/file.rs`'s identical helpers' doc for the
//! rationale): [`vfs_fdtable_put`] uses the CAS-loop `atomic_dec_unless`
//! (mirrors `vfs_fput`/`vfs_iput`'s "don't resurrect a reference that's
//! about to hit zero" pattern); [`vfs_fdtable_clone`]'s `CLONE_FILES`
//! share-path uses a plain `fetch_add` (matches the C's unconditional
//! `atomic_inc` — sound because the calling thread already holds a live
//! reference to `src`, so `ref_count` cannot be racing down to zero
//! concurrently). `fd_count` (also a plain C `int`) is bumped with a
//! plain `fetch_add`/`fetch_sub` (`SeqCst`, matching `atomic_add`/
//! `atomic_sub`) only at the two sites the C original used
//! `atomic_inc`/`atomic_dec` on it ([`vfs_fdtable_alloc_fd_from`],
//! [`vfs_fdtable_dealloc_fd`]); every other site touches it as a plain
//! field (clone's not-yet-published `dest`, put's exclusive-ownership
//! teardown loop) exactly as the C original does.
//!
//! # `bits.h` static-inline reimplementation
//!
//! `NOFILE == 64` (`kernel/inc/param.h`), and `vfs_fdtable.files_bitmap`/
//! `.cloexec_bitmap` are each declared `uint64[1]` (not
//! `uint64[(NOFILE+63)/64]` — a latent C fragility preserved as-is,
//! documented at [`NOFILE`]), so the whole 64-bit fd space is exactly one
//! `u64` word. The general multi-word/byte-prefix bitmap-scan algorithm
//! `bits.h` implements (`__bits_ctz_ptr_from`, used by the C original as
//! `bits_ctz_ptr_from_inv`) therefore collapses to a single masked
//! `trailing_zeros()` on that one word ([`first_clear_bit_from`]); the
//! per-bit test/set/clear macros (`bits_test[_and_set/_clear]_bit64`)
//! become plain `1u64 << bit` mask ops inlined at each call site (no
//! atomics in the C originals either — see `kernel/inc/bits.h`'s
//! `__bit_bitmap_setter`/`_clearer`/`_tester`, which are unsynchronized
//! read-modify-write, always called here under `fdtable.lock`). This
//! mirrors the same static-inline-reimplementation precedent
//! `kernel/hlist.rs`/`vfs/inode.rs`/`vfs/file.rs` already established for
//! `list.h`.
//!
//! # Style notes (rust-skills)
//!
//! Every `unsafe` block is scoped to the smallest expression that needs
//! it, with a `SAFETY:` comment citing the caller-held precondition
//! (non-null/in-bounds fd, lock held, or RCU section active). Public
//! `#[no_mangle] extern "C"` entry points keep their exact C names/
//! signatures (the ABI surface every other translation unit and the
//! several existing Rust `extern "C"` call sites depend on).
//! Ref/fd-count atomics use the narrowest correct `Ordering`
//! (documented per-site above), not a blanket `SeqCst`, except where the
//! C original itself used `__ATOMIC_SEQ_CST` (`atomic_add`/`atomic_sub`,
//! the CAS in `atomic_dec_unless`) — those keep `SeqCst` to match
//! exactly. No `unwrap`/`expect`/panicking indexing on
//! caller-controlled input: every `fd` is range-checked against
//! [`NOFILE`] before use, matching the C's `fd < 0 || fd >= NOFILE`
//! guard at each entry point.
//!
//! # Result-over-ERR_PTR (P3-CS3)
//!
//! This file is the first contained proof-of-concept for the standing
//! directive to prefer `Result` over the `ERR_PTR`/negative-`errno`
//! encoding (`kernel/kstd.rs`'s "Result-first" section) for
//! Rust-internal fallible code. Three file-private helpers whose callers
//! all live in this same file — [`fdtable_alloc_init`],
//! `alloc_fd_from_locked`, `clone_locked` — now return
//! `crate::kstd::KResult<T>` and compose with `?`/`match` instead of
//! hand-rolling `err_ptr`/null-pointer checks. Every public
//! `pub(crate) extern "C"` entry point in this file is a real
//! cross-module boundary (called from `vfs/vfs_syscall.rs` and
//! `proc/clone.rs`), so those keep their exact `c_int`/`ERR_PTR`-pointer
//! C-ABI signatures unchanged — the one `KResult` -> C-encoding
//! conversion happens right at each entry point's return, via a plain
//! `match` (`c_int` case) or [`crate::kstd::result_to_errptr`] (pointer
//! case), never inside the helpers themselves.

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::ptr::NonNull;
use core::sync::atomic::{fence, AtomicI32, AtomicPtr, Ordering};

use crate::bindings::{
    slab_cache_t, spinlock_t, vfs_fdtable, vfs_file, EBADF, SLAB_FLAG_DEBUG_BITMAP,
    SLAB_FLAG_STATIC,
};
use crate::sync::KSpinlock;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N5 (VFS type family, fdtable slice).
//
// This IS the kernel-wide Rust definition of `kernel/inc/vfs/
// vfs_types.h`'s `struct vfs_fdtable` now: `build.rs` blocklists the
// bindgen-generated form and injects a `pub use
// crate::vfs::fdtable::VfsFdtable as vfs_fdtable;` facade re-export (no
// `_t` typedef exists). Field names/types reproduce bindgen's exactly
// (`files: [*mut vfs_file; NOFILE]` — bindgen hardcoded `64usize` from
// the macro-expanded header; this file's pre-existing [`NOFILE`] const
// documents the same value). Derived Copy/Clone exactly as the
// pre-nativization bindgen output did. The C `__ALIGNED_CACHELINE` on
// the embedded `spinlock_t` typedef gives the record align 64, carried
// here as an explicit `repr(align(64))` exactly as bindgen emitted it.
// ---------------------------------------------------------------------------

/// `struct vfs_fdtable` (`kernel/inc/vfs/vfs_types.h`): the per-thread
/// integer-fd -> `vfs_file*` table.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct VfsFdtable {
    pub lock: spinlock_t,
    pub fd_count: c_int,
    pub files: [*mut vfs_file; NOFILE],
    pub files_bitmap: [crate::bindings::uint64; 1],
    pub cloexec_bitmap: [crate::bindings::uint64; 1],
    pub ref_count: c_int,
}

// P3-N5 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (verified in-tree by a temporary
// `offset_of!` gate on `crate::bindings::vfs_fdtable` before the
// switch) and independently confirmed by the cross-compiler
// `_Static_assert` probe (toolchain riscv64-unknown-elf-gcc,
// rv64gc/lp64d, gcc & clang-18 agree; scratchpad
// p3n5_static_assert_probe.c): 576/64, offsets 0/24/32/544/552/560.
const _: () = {
    assert!(core::mem::size_of::<VfsFdtable>() == 576, "vfs_fdtable size");
    assert!(core::mem::align_of::<VfsFdtable>() == 64, "vfs_fdtable alignment");
    assert!(core::mem::offset_of!(VfsFdtable, lock) == 0, "vfs_fdtable.lock offset");
    assert!(core::mem::offset_of!(VfsFdtable, fd_count) == 24, "vfs_fdtable.fd_count offset");
    assert!(core::mem::offset_of!(VfsFdtable, files) == 32, "vfs_fdtable.files offset");
    assert!(
        core::mem::offset_of!(VfsFdtable, files_bitmap) == 544,
        "vfs_fdtable.files_bitmap offset"
    );
    assert!(
        core::mem::offset_of!(VfsFdtable, cloexec_bitmap) == 552,
        "vfs_fdtable.cloexec_bitmap offset"
    );
    assert!(core::mem::offset_of!(VfsFdtable, ref_count) == 560, "vfs_fdtable.ref_count offset");
};

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally per this crate's established convention (see `vfs/inode.rs`'s /
// `vfs/file.rs`'s externs-block doc). Everything here is `safe`: passing
// a raw pointer is never itself unsafe, and every entry point below
// either null-checks internally or is called only with pointers this
// file already proved non-null.
// ===========================================================================

// P3-D3c: the spinlock primitives are genuinely `unsafe fn`s in
// `crate::lock::spinlock` now that their `#[no_mangle]` exports are gone;
// this file's original extern declarations asserted `safe fn` (usual
// FFI-facade convention). Thin wrappers preserve that safe facade for the
// unchanged call sites.
/// SAFETY: see [`crate::lock::spinlock::spin_init`]'s contract.
fn spin_init(l: *mut spinlock_t, name: *mut c_char) {
    unsafe { crate::lock::spinlock::spin_init(l, name) }
}

// P3-D3b: lock/rcu.rs's read-side entry points are plain safe Rust fns
// now that their `#[no_mangle]` exports are gone; reached by crate path.
use crate::lock::rcu::{rcu_read_lock, rcu_read_unlock};

// P3-D3a: the slab entry points are genuinely `unsafe fn` in
// `crate::mm::slab` now that their `#[no_mangle]` exports are gone; this
// file's original extern declarations asserted `safe fn` (usual FFI
// facade) and typed the cache pointer as the bindgen `slab_cache_t`
// rather than `crate::mm::slab::SlabCache` (same layout — see
// `cffi::raw`'s identical note). Thin cast + safe-facade wrappers
// preserve both.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
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

// P3-1C mesh sweep: vfs/file.rs is in scope for this wave, so these
// become plain crate-path imports instead of `extern "C"` redeclarations
// (identical signatures).
use crate::vfs::file::{vfs_fdup, vfs_fput};

// ===========================================================================
// Small helpers: negative-errno constants, ERR_PTR family, fd-space
// constants, `bits.h`/`errno.h` static-inline reimplementations.
// ===========================================================================

// `kassert!`/`is_err_or_null`/`result_to_errptr`'s canonical homes are
// `crate::kstd`/crate root (P3-CS1 centralization). `is_err_or_null` is
// still needed here to consume `vfs_fdup`'s `ERR_PTR`-encoded return (a
// genuine cross-module/file.rs C-ABI boundary, out of this cluster's
// scope — see the P3-CS3 module doc note below); the fd-allocation and
// clone helpers below no longer hand-roll `err_ptr`/null-pointer checks
// themselves, using [`KResult`]/[`Errno`] internally instead
// (P3-CS3: Result-over-ERR_PTR proof-of-concept cluster).
use crate::kassert;
use crate::kstd::{is_err_or_null, result_to_errptr, Errno, KResult};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// `kernel/inc/param.h`: `#define NOFILE 64` — open files per thread.
/// Not bindgen-exposed (no existing Rust call site referenced it before
/// this wave); hardcoded here per the established local-constant
/// precedent (`kernel/proc/clone.rs`'s `CLONE_FILES`) rather than adding
/// a single-purpose bindgen allowlist entry. Also see the module doc's
/// "`bits.h` static-inline reimplementation" section: `vfs_fdtable`'s
/// bitmap fields are hardcoded `uint64[1]`, not sized off `NOFILE`, so
/// this constant implicitly doubles as "bits per bitmap word" below.
const NOFILE: usize = 64;

/// `uabi/clone_flags.h`: `#define CLONE_FILES 0x00100000`.
const CLONE_FILES: c_int = 0x0010_0000;

/// `uabi/fcntl.h`: `#define FD_CLOEXEC 1`.
const FD_CLOEXEC: c_int = 1;

/// Mirrors `fdtable.c`'s `IS_FD(fd)` macro: a `files[]` slot holds a real
/// pointer (not `NULL`, not some placeholder value `<= NOFILE`). Every
/// live heap pointer is far larger than `NOFILE`, so this is effectively
/// "non-null", preserved verbatim (including the C's non-obvious `>`
/// bound) rather than simplified to `!p.is_null()`.
#[inline(always)]
fn is_fd(p: *mut vfs_file) -> bool {
    (p as u64) > (NOFILE as u64)
}

/// Mirrors `bits_ctz_ptr_from_inv(bitmap, from, NOFILE)` specialized for
/// `vfs_fdtable`'s one-word bitmaps (see the module doc). Returns the
/// index of the first *clear* bit in `[from, NOFILE)` of `word`, or -1 if
/// none.
#[inline(always)]
fn first_clear_bit_from(word: u64, from: usize) -> i64 {
    debug_assert!(from < NOFILE);
    let candidates = if from == 0 { !word } else { !word & !((1u64 << from) - 1) };
    if candidates == 0 {
        -1
    } else {
        candidates.trailing_zeros() as i64
    }
}

/// Mirrors `rcu_dereference(p)` (`__ATOMIC_CONSUME` load; approximated as
/// `Acquire`, same mapping as `kernel/hlist.rs`/`kernel/irq/irq_core.rs`).
///
/// # Safety
/// `p` must be a live, aligned `*mut vfs_file` cell (a `files[i]` slot of
/// a live `vfs_fdtable`).
#[inline(always)]
unsafe fn rcu_deref_file(p: *mut *mut vfs_file) -> *mut vfs_file {
    // SAFETY: forwarded from the caller; `AtomicPtr<vfs_file>` and
    // `*mut vfs_file` share layout.
    unsafe { (*(p as *const AtomicPtr<vfs_file>)).load(Ordering::Acquire) }
}

/// Mirrors `rcu_assign_pointer(p, v)` (`__ATOMIC_RELEASE` store).
///
/// # Safety
/// `p` must be a live, aligned `*mut vfs_file` cell.
#[inline(always)]
unsafe fn rcu_assign_file(p: *mut *mut vfs_file, v: *mut vfs_file) {
    // SAFETY: forwarded from the caller.
    unsafe { (*(p as *const AtomicPtr<vfs_file>)).store(v, Ordering::Release) };
}

// ---------------------------------------------------------------------------
// `ref_count`/`fd_count` atomics — mirrors `smp/atomic.h`'s
// `atomic_dec_unless` (CAS loop) and `atomic_add`/`atomic_sub` (plain
// `SeqCst` fetch-add) exactly (see the module doc's Refcount map).
// ---------------------------------------------------------------------------

/// # Safety
/// `fdtable` must point to a live, aligned `vfs_fdtable`.
#[inline(always)]
fn refcount_atomic<'a>(fdtable: *mut vfs_fdtable) -> &'a AtomicI32 {
    // SAFETY: `ref_count` is a plain C `int` field, same size/align as
    // `AtomicI32`; caller ensures `fdtable` is a live, aligned
    // `vfs_fdtable`.
    unsafe { &*(ptr::addr_of_mut!((*fdtable).ref_count) as *const AtomicI32) }
}

/// # Safety
/// `fdtable` must point to a live, aligned `vfs_fdtable`.
#[inline(always)]
fn fd_count_atomic<'a>(fdtable: *mut vfs_fdtable) -> &'a AtomicI32 {
    // SAFETY: `fd_count` is a plain C `int` field, same size/align as
    // `AtomicI32`; caller ensures `fdtable` is a live, aligned
    // `vfs_fdtable`.
    unsafe { &*(ptr::addr_of_mut!((*fdtable).fd_count) as *const AtomicI32) }
}

#[inline(always)]
fn atomic_dec_unless(a: &AtomicI32, unless: i32) -> bool {
    let mut val = a.load(Ordering::Acquire);
    while val != unless {
        match a.compare_exchange(val, val - 1, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => return true,
            Err(cur) => val = cur,
        }
    }
    false
}

// ===========================================================================
// Global state: the `vfs_fdtable` slab cache.
// ===========================================================================

#[repr(transparent)]
struct FdtableSlabCell(core::cell::UnsafeCell<core::mem::MaybeUninit<slab_cache_t>>);
// SAFETY: written in full by `slab_cache_init` (called once from
// `__vfs_fdtable_global_init`, before any `slab_alloc`/`slab_free` on
// this cache) and otherwise only touched through the C slab allocator's
// own internally-synchronized entry points, matching the identical
// `FileSlabCell` precedent in `vfs/file.rs`.
unsafe impl Sync for FdtableSlabCell {}
static __VFS_FDTABLE_SLAB: FdtableSlabCell =
    FdtableSlabCell(core::cell::UnsafeCell::new(core::mem::MaybeUninit::zeroed()));
#[inline]
fn fdtable_slab() -> *mut slab_cache_t {
    __VFS_FDTABLE_SLAB.0.get() as *mut slab_cache_t
}

/// Initialize the fdtable slab cache. Called once during VFS
/// initialization.
pub(crate) extern "C" fn __vfs_fdtable_global_init() {
    let ret = slab_cache_init(
        fdtable_slab(),
        c"vfs_fdtable_cache".as_ptr() as *mut c_char,
        core::mem::size_of::<vfs_fdtable>(),
        (SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP) as u64,
    );
    kassert!(ret == 0, "Failed to initialize vfs_fdtable_cache slab cache");
}

/// Allocate and initialize a new fdtable (`ref_count = 1`, all fields
/// zeroed). Returns `Err(Errno::NoMem)` on allocation failure.
///
/// P3-CS3 (Result-over-ERR_PTR): this is a private, file-internal helper
/// (both call sites — [`vfs_fdtable_init`] and [`clone_locked`] — live in
/// this file), so it returns [`KResult`] directly instead of the
/// transitional `ERR_PTR`/null-pointer convention: neither caller can
/// forget to check for failure (the compiler forces it via `?`/`match`),
/// unlike the previous `if fdtable.is_null() { ... }` duplicated at both
/// sites.
fn fdtable_alloc_init() -> KResult<NonNull<vfs_fdtable>> {
    let fdtable = slab_alloc(fdtable_slab()) as *mut vfs_fdtable;
    let fdtable = NonNull::new(fdtable).ok_or(Errno::NoMem)?;
    // SAFETY: `slab_alloc` returned a fresh, exclusively-owned
    // `size_of::<vfs_fdtable>()` allocation (confirmed non-null just
    // above); zeroing then field-init mirrors the C original's `memset` +
    // explicit-field-init.
    unsafe {
        ptr::write_bytes(fdtable.as_ptr(), 0, 1);
        spin_init(
            ptr::addr_of_mut!((*fdtable.as_ptr()).lock),
            c"vfs_fdtable_lock".as_ptr() as *mut c_char,
        );
        (*fdtable.as_ptr()).ref_count = 1;
    }
    Ok(fdtable)
}

fn fdtable_free(fdtable: *mut vfs_fdtable) {
    if !fdtable.is_null() {
        slab_free(fdtable as *mut c_void);
    }
}

// ===========================================================================
// Public C-ABI entry points.
// ===========================================================================

/// Core fd-allocation logic behind [`vfs_fdtable_alloc_fd_from`], factored
/// out as a private helper returning [`KResult`] (P3-CS3: this function's
/// only caller is in this same file, so the `Result` cascade is fully
/// contained — see the module's Result-over-ERR_PTR note). Every early
/// failure used to be a separate `return neg(E*)` the caller had to trust
/// was exhaustive; here the three distinct failure modes (bad args, out of
/// fds, OOM duplicating the file reference) are ordinary `Err` returns
/// composed with `?`, and the one success path is the one `Ok`.
///
/// LOCKING: caller MUST hold `fdtable.lock` (same contract as
/// [`vfs_fdtable_alloc_fd_from`]).
fn alloc_fd_from_locked(
    fdtable: *mut vfs_fdtable,
    file: *mut vfs_file,
    start_fd: c_int,
) -> KResult<usize> {
    if fdtable.is_null() || file.is_null() {
        return Err(Errno::Inval);
    }
    if start_fd < 0 || start_fd as usize >= NOFILE {
        return Err(Errno::Inval);
    }
    kassert!(
        // SAFETY: non-null `fdtable` (checked above); only the lock's
        // address is taken.
        KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fdtable).lock) }).holding(),
        "vfs_fdtable_alloc_fd_from: fdtable lock not held"
    );
    // SAFETY: non-null `fdtable`; lock held (asserted above), so this
    // plain field read cannot race a concurrent writer.
    if unsafe { (*fdtable).fd_count } as usize >= NOFILE {
        return Err(Errno::MFile); // Too many open files
    }
    // SAFETY: non-null `fdtable`; lock held.
    let word = unsafe { (*fdtable).files_bitmap[0] };
    let fd = first_clear_bit_from(word, start_fd as usize);
    if fd < 0 || fd as usize >= NOFILE {
        return Err(Errno::MFile); // No free file descriptor
    }
    let fd = fd as usize;

    if vfs_fdup(file).is_null() {
        return Err(Errno::NoMem); // Failed to duplicate file reference
    }

    // SAFETY: non-null `fdtable`; `fd` in `[0, NOFILE)`; lock held.
    unsafe {
        (*fdtable).files_bitmap[0] |= 1u64 << fd;
        (*fdtable).cloexec_bitmap[0] &= !(1u64 << fd);
        rcu_assign_file(ptr::addr_of_mut!((*fdtable).files[fd]), file);
    }
    fd_count_atomic(fdtable).fetch_add(1, Ordering::SeqCst);
    Ok(fd)
}

/// Allocate a file descriptor starting from a minimum fd `>= start_fd`.
/// Increments `file`'s reference count via `vfs_fdup`.
///
/// LOCKING: caller MUST hold `fdtable.lock`.
///
/// C-ABI boundary (P3-CS3): this `pub(crate) extern "C"` entry point is
/// called from `vfs/vfs_syscall.rs` (a different module) expecting the
/// crate's established "fd or negative errno" `c_int` convention, so the
/// one [`KResult`] -> `c_int` conversion happens right here, at the edge —
/// [`alloc_fd_from_locked`] itself never constructs a negative-errno
/// `c_int` or an `ERR_PTR`.
pub(crate) extern "C" fn vfs_fdtable_alloc_fd_from(
    fdtable: *mut vfs_fdtable,
    file: *mut vfs_file,
    start_fd: c_int,
) -> c_int {
    match alloc_fd_from_locked(fdtable, file, start_fd) {
        Ok(fd) => fd as c_int,
        Err(e) => e.neg(),
    }
}

/// Allocate the lowest available file descriptor for `file`.
///
/// LOCKING: caller MUST hold `fdtable.lock`.
pub(crate) extern "C" fn vfs_fdtable_alloc_fd(fdtable: *mut vfs_fdtable, file: *mut vfs_file) -> c_int {
    vfs_fdtable_alloc_fd_from(fdtable, file, 0)
}

/// Create a new, empty fdtable. Used when a new thread doesn't share its
/// parent's fdtable. Panics on allocation failure (matches the C
/// original's `assert`).
pub(crate) extern "C" fn vfs_fdtable_init() -> *mut vfs_fdtable {
    let fdtable = fdtable_alloc_init();
    kassert!(fdtable.is_ok(), "vfs_fdtable_init: fdtable is NULL");
    // NOTE: the C original re-`memset`s `files`/`files_bitmap`/
    // `cloexec_bitmap` here, redundant with `fdtable_alloc_init`'s own
    // `write_bytes(0)` (which already zeroed the whole struct including
    // these fields) — elided as a provable no-op, not a behavior change.
    //
    // PANIC: `kassert!` above already diverged on `Err`; this `expect`
    // never fires — same guarantee the original `!fdtable.is_null()`
    // assert gave the raw-pointer return.
    fdtable.expect("BUG: fdtable_alloc_init returned Err after kassert passed").as_ptr()
}

/// Core clone/share logic behind [`vfs_fdtable_clone`], factored out as a
/// private helper returning [`KResult`] (P3-CS3: only called from this
/// file). The two failure modes (`src` null, `dest` allocation OOM) are
/// `Err` returns instead of hand-encoded `ERR_PTR`s; the two success
/// paths (share `src`, or the freshly built `dest`) are `Ok(NonNull)` —
/// there is no representable state where this helper hands back a raw
/// pointer that might secretly be an encoded error, unlike the pointer
/// this replaces.
///
/// `vfs_fdup`'s own return (`dst_file` below) stays `ERR_PTR`-checked via
/// [`is_err_or_null`] on purpose: that call crosses into `vfs/file.rs`, a
/// different file's still-transitional C-ABI surface, out of this
/// cluster's contained scope (see the module's Result-over-ERR_PTR note).
fn clone_locked(src: *mut vfs_fdtable, clone_flags: c_int) -> KResult<NonNull<vfs_fdtable>> {
    let src_nn = NonNull::new(src).ok_or(Errno::Inval)?;

    rcu_read_lock();
    if clone_flags & CLONE_FILES != 0 {
        // Share the fdtable.
        refcount_atomic(src).fetch_add(1, Ordering::SeqCst);
        rcu_read_unlock();
        return Ok(src_nn);
    }

    let dest_nn = match fdtable_alloc_init() {
        Ok(nn) => nn,
        Err(e) => {
            rcu_read_unlock();
            return Err(e); // Allocation failed
        }
    };
    let dest = dest_nn.as_ptr();
    // NOTE: `dest` is freshly allocated & fully zeroed by
    // `fdtable_alloc_init`; the C original's `dest->fd_count = 0` plus
    // `memset` of `files`/`files_bitmap`/`cloexec_bitmap` here is
    // redundant with that (same elision as `vfs_fdtable_init` above).

    // Duplicate file references while holding the RCU read lock — this
    // protects against a concurrent close() deallocating `src`'s files.
    for i in 0..NOFILE {
        // SAFETY: `rcu_read_lock()` held; `src` is a live fdtable (every
        // caller holds a reference to it, per this function's contract).
        let src_file = unsafe { rcu_deref_file(ptr::addr_of_mut!((*src).files[i])) };
        if is_fd(src_file) {
            let dst_file = vfs_fdup(src_file);
            if !is_err_or_null(dst_file) {
                // SAFETY: `dest` is exclusively owned (not yet published
                // to any other thread); `i` is in `[0, NOFILE)`.
                unsafe {
                    (*dest).files[i] = dst_file;
                    (*dest).fd_count += 1;
                    (*dest).files_bitmap[0] |= 1u64 << i;
                    if (*src).cloexec_bitmap[0] & (1u64 << i) != 0 {
                        (*dest).cloexec_bitmap[0] |= 1u64 << i;
                    }
                }
            } else {
                // SAFETY: see above.
                unsafe { (*dest).files[i] = ptr::null_mut() };
            }
        }
    }
    rcu_read_unlock();
    // Ensure all writes are visible before returning (matches the C
    // original's explicit `smp_mb()`).
    fence(Ordering::SeqCst);

    Ok(dest_nn)
}

/// Clone or share an fdtable for fork/clone. If `clone_flags &
/// CLONE_FILES`, shares `src` (bumps its refcount and returns it);
/// otherwise deep-copies with duplicated file references.
///
/// Returns the (possibly shared) fdtable, or `ERR_PTR` on failure.
///
/// C-ABI boundary (P3-CS3): `proc/clone.rs` (a different module) calls
/// this expecting the crate's transitional `ERR_PTR`-encoded-pointer
/// convention (it decodes the result with its own local `check_ptr`
/// helper), so [`result_to_errptr`] does the one `KResult` -> `ERR_PTR`
/// conversion right here, at the edge — [`clone_locked`] itself never
/// constructs an `ERR_PTR`.
pub(crate) extern "C" fn vfs_fdtable_clone(
    src: *mut vfs_fdtable,
    clone_flags: c_int,
) -> *mut vfs_fdtable {
    result_to_errptr(clone_locked(src, clone_flags).map(NonNull::as_ptr))
}

/// Release a reference to an fdtable. On last reference, closes all open
/// files and frees the fdtable. Called when a thread exits or unshares
/// its fdtable.
pub(crate) extern "C" fn vfs_fdtable_put(fdtable: *mut vfs_fdtable) {
    if fdtable.is_null() {
        return;
    }

    if atomic_dec_unless(refcount_atomic(fdtable), 1) {
        return; // Still has references
    }

    // No need to hold the lock here since no other references exist.
    // First, close all open files in the range [0, NOFILE).
    for i in 0..NOFILE {
        // SAFETY: exclusive access (last reference just dropped above);
        // plain (non-RCU, non-atomic) field access, matching the C
        // original exactly (it does not use rcu_dereference/
        // rcu_assign_pointer here either).
        let file = unsafe { (*fdtable).files[i] };
        if is_fd(file) {
            vfs_fput(file);
            // SAFETY: see above.
            unsafe {
                (*fdtable).files[i] = ptr::null_mut();
                (*fdtable).fd_count -= 1;
            }
        }
    }

    kassert!(
        // SAFETY: exclusive access, plain field read.
        unsafe { (*fdtable).fd_count } >= 0,
        "vfs_fdtable_destroy: fd_count negative"
    );
    fdtable_free(fdtable);
}

/// Look up the file for `fd`, returning it with an incremented refcount
/// (caller must `vfs_fput()` when done). Wait-free (RCU read lock only).
pub(crate) extern "C" fn vfs_fdtable_get_file(fdtable: *mut vfs_fdtable, fd: c_int) -> *mut vfs_file {
    if fdtable.is_null() || fd < 0 || fd as usize >= NOFILE {
        return ptr::null_mut(); // Invalid arguments
    }
    rcu_read_lock();
    // SAFETY: `rcu_read_lock()` held; `fd` in `[0, NOFILE)`; `fdtable`
    // non-null.
    let file = unsafe { rcu_deref_file(ptr::addr_of_mut!((*fdtable).files[fd as usize])) };
    if is_fd(file) {
        let file = vfs_fdup(file);
        rcu_read_unlock();
        return file;
    }
    rcu_read_unlock();
    ptr::null_mut() // File descriptor not allocated
}

/// Remove `fd` from the table and return the file that was associated
/// with it. Does NOT decrement the file's refcount — caller must handle
/// that (typically after an RCU grace period, so concurrent readers from
/// [`vfs_fdtable_get_file`] can't observe a freed file).
///
/// LOCKING: caller MUST hold `fdtable.lock`.
pub(crate) extern "C" fn vfs_fdtable_dealloc_fd(fdtable: *mut vfs_fdtable, fd: c_int) -> *mut vfs_file {
    if fdtable.is_null() || fd < 0 || fd as usize >= NOFILE {
        return ptr::null_mut(); // Invalid arguments
    }
    kassert!(
        // SAFETY: non-null `fdtable`; only the lock's address is taken.
        KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fdtable).lock) }).holding(),
        "vfs_fdtable_dealloc_fd: fdtable lock not held"
    );
    let fd = fd as usize;
    // SAFETY: non-null `fdtable`; `fd` in bounds; lock held.
    let file = unsafe { (*fdtable).files[fd] };
    if !is_fd(file) {
        return ptr::null_mut(); // File descriptor not allocated
    }

    // SAFETY: non-null `fdtable`; `fd` in bounds; lock held.
    unsafe {
        rcu_assign_file(ptr::addr_of_mut!((*fdtable).files[fd]), ptr::null_mut());
        (*fdtable).files_bitmap[0] &= !(1u64 << fd);
        (*fdtable).cloexec_bitmap[0] &= !(1u64 << fd);
    }
    fd_count_atomic(fdtable).fetch_sub(1, Ordering::SeqCst);
    file
}

/// Get the `FD_CLOEXEC` flag for `fd`, or a negative errno.
///
/// LOCKING: caller MUST hold `fdtable.lock`.
pub(crate) extern "C" fn vfs_fdtable_get_fdflags(fdtable: *mut vfs_fdtable, fd: c_int) -> c_int {
    if fdtable.is_null() || fd < 0 || fd as usize >= NOFILE {
        return neg(EBADF);
    }
    kassert!(
        // SAFETY: non-null `fdtable`; only the lock's address is taken.
        KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fdtable).lock) }).holding(),
        "vfs_fdtable_get_fdflags: fdtable lock not held"
    );
    let fd = fd as usize;
    // SAFETY: non-null `fdtable`; `fd` in bounds; lock held.
    let file = unsafe { (*fdtable).files[fd] };
    if !is_fd(file) {
        return neg(EBADF);
    }
    let mut flags = 0;
    // SAFETY: non-null `fdtable`; `fd` in bounds; lock held.
    if unsafe { (*fdtable).cloexec_bitmap[0] } & (1u64 << fd) != 0 {
        flags |= FD_CLOEXEC;
    }
    flags
}

/// Set (or clear) the `FD_CLOEXEC` flag for `fd`. Returns 0 on success, a
/// negative errno on failure.
///
/// LOCKING: caller MUST hold `fdtable.lock`.
pub(crate) extern "C" fn vfs_fdtable_set_fdflags(
    fdtable: *mut vfs_fdtable,
    fd: c_int,
    flags: c_int,
) -> c_int {
    if fdtable.is_null() || fd < 0 || fd as usize >= NOFILE {
        return neg(EBADF);
    }
    kassert!(
        // SAFETY: non-null `fdtable`; only the lock's address is taken.
        KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fdtable).lock) }).holding(),
        "vfs_fdtable_set_fdflags: fdtable lock not held"
    );
    let fd = fd as usize;
    // SAFETY: non-null `fdtable`; `fd` in bounds; lock held.
    let file = unsafe { (*fdtable).files[fd] };
    if !is_fd(file) {
        return neg(EBADF);
    }

    // SAFETY: non-null `fdtable`; `fd` in bounds; lock held.
    unsafe {
        if flags & FD_CLOEXEC != 0 {
            (*fdtable).cloexec_bitmap[0] |= 1u64 << fd;
        } else {
            (*fdtable).cloexec_bitmap[0] &= !(1u64 << fd);
        }
    }
    0
}

/// Close every fd with `FD_CLOEXEC` set. Called from `exec.c` on a
/// successful `execve`.
pub(crate) extern "C" fn vfs_fdtable_close_on_exec(fdtable: *mut vfs_fdtable) {
    if fdtable.is_null() {
        return;
    }

    let mut to_close: [*mut vfs_file; NOFILE] = [ptr::null_mut(); NOFILE];
    let mut close_count = 0usize;

    {
        // SAFETY: non-null `fdtable`; only the lock's address is taken.
        let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fdtable).lock) }).lock();
        for fd in 0..NOFILE {
            // SAFETY: lock held (guard `_g` above); `fd` in bounds.
            let was_set = unsafe {
                let mask = 1u64 << fd;
                let w = (*fdtable).cloexec_bitmap[0];
                (*fdtable).cloexec_bitmap[0] = w & !mask;
                w & mask != 0
            };
            if !was_set {
                continue;
            }

            let file = vfs_fdtable_dealloc_fd(fdtable, fd as c_int);
            if is_fd(file) {
                to_close[close_count] = file;
                close_count += 1;
            }
        }
    } // lock released here, before the fput loop (matches the C original)

    for entry in to_close.iter().take(close_count) {
        vfs_fput(*entry);
    }
}
