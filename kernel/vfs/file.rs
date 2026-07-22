//! VFS file operations — Rust port of `kernel/vfs/file.c` (Phase 2
//! Wave 14, see `docs/rustify/phase2_plan.md`).
//!
//! This is the `vfs_file` lifecycle: allocation/refcounting
//! (`vfs_fileopen`/`vfs_fput`/`vfs_fdup`), dispatch of read/write/seek/
//! ioctl/stat through the character-device, block-device, and
//! `vfs_file_ops` vtable paths, anonymous-pipe and (stub) socket
//! allocation, and the global open-file table used for cache-shrink
//! accounting. `kernel/vfs/pipe.c` (Wave 14's sibling file, see
//! [`super::pipe`]) is the ring-buffer half of the pipe story this file
//! only calls into (`pipe_alloc`/`pipe_open`/`pipe_close`). The rest of
//! `kernel/vfs/` (`fs.c`, `fdtable.c`, `vfs_syscall.c`, and the
//! `tmpfs/`/`xv6fs/`/`devtmpfs/` filesystem drivers) remains C for now
//! (Waves 15-20) and is called through the `unsafe extern "C"` block
//! below, exactly as any other C translation unit would call these
//! functions.
//!
//! # Lock map
//!
//! `vfs_file.lock` (a `mutex_t`) protects `f_pos` and serializes
//! concurrent read/write/seek/truncate on one `vfs_file`; every
//! acquire/release pair in this file is a single, lexically-scoped
//! critical section (never spans a call into another C translation
//! unit while held, and the driver callbacks invoked *while* held
//! — `file->ops->read`/`write`/`llseek` — are documented at each call
//! site as sleep-safe), so unlike `vfs/inode.rs`'s `vfs_iput` this is a
//! clean RAII fit: [`crate::sync::KMutex`] via [`file_lock`]. The
//! global open-file table (`__vfs_ftable_lock` + the list head in the C
//! original) is likewise a single lexically-scoped critical section
//! and, as of Wave P3-8f, is a [`crate::sync::SpinLock`] that owns the
//! `list_node_t` head it protects directly (the two former
//! independently-paired globals collapsed into one lock-owns-data value,
//! same P3-8b/8d precedent as `ramdisk.rs`/`devtmpfs/superblock.rs`).
//! [`crate::sysnet::SOCKETS`] (owned by
//! `sysnet.rs`) is locked in one straight-line block inside
//! [`vfs_sockalloc`] with no early return while held — Wave P3-8d
//! upgraded it from a `KSpinlock::from_bindings` handle onto a raw
//! `spinlock_t` (the *pairing*, not the storage location, is what
//! determined RAII-fitness even before this — same reasoning
//! `vfs/inode.rs` applied to `fs_struct.lock`) to a genuine
//! `crate::sync::SpinLock<*mut sock>::lock()` call, `Deref`ing straight
//! to the list head instead of a separate `extern "C" { static mut
//! sockets }` reinterpretation.
//!
//! The inode lock (`vfs_ilock`/`vfs_iunlock`, owned by `vfs/inode.rs`)
//! and the two device-refcount calls (`cdev_get`/`cdev_put`,
//! `blkdev_get`/`blkdev_put`, owned by the still-C `dev/` module) are
//! cross-TU paired C-ABI calls, not wrapped in local RAII, matching the
//! `vm_rlock`/`vm_runlock` precedent (mm WP1) and `vfs/inode.rs`'s own
//! `vfs_ilock`/`vfs_iunlock`.
//!
//! # Refcount map
//!
//! `vfs_file.ref_count` (a plain C `int`) is manipulated by the same
//! `atomic_dec_unless`/`atomic_inc_unless` idiom `vfs/inode.rs` uses for
//! `vfs_inode.ref_count` (Acquire initial load, `SeqCst` CAS, mirroring
//! `smp/atomic.h`'s `atomic_oper_cond` verbatim) — reimplemented locally
//! here rather than shared, per this crate's established per-file
//! C-ABI-surface-is-self-contained convention (see `vfs/inode.rs`'s own
//! externs-block doc for the rationale). `__vfs_open_file_count` (a
//! plain C `int` bumped with `__atomic_add_fetch`/`__atomic_sub_fetch`,
//! `__ATOMIC_SEQ_CST`) becomes a straightforward `AtomicI32` static.
//!
//! # Style notes (rust-skills)
//!
//! Every `unsafe` block is scoped to the smallest expression that needs
//! it (`unsafe-minimize-scope`) with a `SAFETY:` comment at each
//! non-obvious site. `struct sock` is a second, file-local mirror of
//! the real definition in `kernel/sysnet.c` (which stays C through
//! Wave 28) — this is not a new hazard introduced by the port: the C
//! original already carries its own local shadow copy of that struct
//! (see the `// Socket structure from sysnet.c` comment at its
//! definition below) for exactly the reason this file needs field
//! access into an object it does not own the type of. `bool` in every
//! header this file binds against is the project's own
//! `typedef enum { false = 0, true = 1 } bool` (`kernel/inc/types.h`,
//! selected because this crate is compiled at `-std=` C17, not C23) —
//! an `int`-sized (4-byte) enum, *not* the 1-byte C23 `_Bool`/Rust
//! `bool`. Every signature in this file that carries a C `bool`
//! parameter (`user` on the read/write paths) therefore uses `c_int`,
//! matching the convention already established by `kernel/tty/tty.rs`,
//! `kernel/tty/pty.rs`, and `kernel/console.rs`'s own `pipe_read`/
//! `pipe_write`/`pipe_set_flags` externs — using Rust's 1-byte `bool`
//! here would silently break the calling convention.
//!
//! # Reference-ification (P3-7b)
//!
//! The read/write/stat/lseek/ioctl/truncate `*_inner` helpers take a
//! `&mut VfsFile` now, not a raw pointer: the null-check plus the single
//! `unsafe { &mut *file }` conversion happen exactly once, at each
//! `extern "C"` boundary (the outermost frame that genuinely receives a
//! raw pointer), so the interior plain-field logic (`f_flags`, `ops`,
//! `inode`) is ordinary safe access. What deliberately stays raw:
//! * the `pos` union — every union field read/write is `unsafe` in Rust
//!   regardless of how the container is borrowed;
//! * the [`FileOps`] trait dispatch (`ops.read`/`write`/`llseek`/
//!   `ioctl`) — its `file` parameter keeps the raw-pointer signature
//!   (cross-module trait boundary), reached by reborrowing the reference
//!   as `&raw mut *file`, a synchronous call;
//! * the lifetime-honesty exclusions — [`vfs_fput`] (drops the last
//!   refcount and frees), [`file_free`] (frees), [`file_alloc`], and
//!   [`vfs_fileopen_inner`] (conditionally `file_free`s the half-built
//!   file on every error path) keep their raw `file`: a `&mut` there
//!   would assert validity past the point the object may be freed.
//! Aliasing caveat, documented approximation (same class as the crate's
//! `__ATOMIC_CONSUME` -> `Acquire` mapping and `fdtable.rs`'s P3-7a
//! note): a shared (`dup`-ed) descriptor can be operated on by two
//! threads at once, so two `&mut VfsFile` views of one object may exist;
//! the C original is all-raw and expresses no such exclusivity. Those
//! accesses are serialized by `file_lock` on the fields that matter
//! (`pos`) and are invisible to each frame's codegen, but are a foreign
//! `&mut` overlap under a strict Stacked/Tree-Borrows reading — the same
//! layout-frozen approximation the arc accepts crate-wide.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

// P3-CS6: every `neg(E*)` site that used to build a raw negative `c_int`
// by hand is now `Errno::<Variant>` (converted to the C ABI encoding
// exactly once, at each function's boundary, via
// `result_to_errptr`/`result_to_neg_errno`/`Errno::neg`) -- only
// `EAGAIN` survives as a real import, for `vfs_fput`'s untouched
// (infallible-return) `fflush`-result comparison.
use crate::bindings::{
    blkdev_t, bool_, cdev_t, device_t, list_node_t, loff_t, pipe, slab_cache_t,
    spinlock_t, stat, vfs_file, vfs_inode, vfs_inode_ref,
    EAGAIN, SLAB_FLAG_DEBUG_BITMAP, SLAB_FLAG_STATIC,
};
use crate::sync::{KMutex, SpinLock};

// ===========================================================================
// Native uabi `struct stat` — P3-4b nativization (user directive: remove
// the C-compatible interfaces; userspace-ABI scrutiny class).
// `Stat` is the canonical KERNEL-SIDE definition of
// `kernel/inc/uabi/stat.h`'s `struct stat`: `build.rs` blocklists the
// bindgen emission and re-exports this type as `crate::bindings::stat`
// (facade `pub use`, N2 pattern).
//
// *** USERSPACE ABI — HANDLE WITH P3-4 SCRUTINY *** The C header STAYS:
// user/ programs (user.h's `fstat`/`stat` prototypes, ls.c, find.c, …)
// compile against uabi/stat.h, and the kernel `either_copyout`s this
// record BY VALUE into user buffers (sys_fstat/sys_stat paths in
// vfs_syscall.rs). A layout slip here is SILENT userspace breakage —
// `ls` printing garbage sizes, usertests failing — so this native and
// that header are two independent spellings of one by-value boundary
// contract. The byte-exact asserts below pin the native to the header.
// HOST determination: no host-side tool consumes uabi/stat.h
// (mkfs/mkfs.c includes only types.h/ondisk.h/param.h — grep-verified),
// so the gcc probe is target-only (unlike P3-4a's two-arch ondisk gate).
//
// DERIVE DECISION (P3-4b): Copy + Clone, exactly as the
// pre-nativization bindgen output derived (plain scalar fields).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen form + toolchain-gcc `_Static_assert` probe (rv64gc/lp64d —
// scratchpad p3_4b_uabi_probe.c); both agree on every value asserted
// below.
// ===========================================================================

/// Native uabi `struct stat` (`kernel/inc/uabi/stat.h`) — the record
/// `fstat(2)`/`stat(2)` copy out to userspace by value.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Stat {
    /// File system's disk device.
    pub dev: crate::bindings::int32,
    /// Inode number.
    pub ino: crate::bindings::uint64,
    /// Permission and type bits (`S_IS*` macros).
    pub mode: crate::bindings::mode_t,
    /// Number of links to file.
    pub nlink: crate::bindings::uint32,
    /// Size of file in bytes.
    pub size: crate::bindings::uint64,
}

// P3-4b hardcoded layout proof — the USERSPACE byte contract
// (`uabi/stat.h` `struct stat`), every field. Values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the target gcc probe.
const _: () = {
    assert!(core::mem::size_of::<Stat>() == 32, "stat size (USERSPACE ABI)");
    assert!(core::mem::align_of::<Stat>() == 8, "stat alignment");
    assert!(core::mem::offset_of!(Stat, dev) == 0, "stat.dev offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Stat, ino) == 8, "stat.ino offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Stat, mode) == 16, "stat.mode offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Stat, nlink) == 20, "stat.nlink offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Stat, size) == 24, "stat.size offset (USERSPACE ABI)");
};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N5 (VFS type family, file slice).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/vfs/
// vfs_types.h`'s `struct vfs_file`/`struct vfs_file_ops` now: `build.rs`
// blocklists the bindgen-generated forms and injects `pub use
// crate::vfs::file::... as ...;` facade re-exports (no `_t` typedefs
// exist). The ops-table fn-pointer fields reproduce bindgen's
// `Option<unsafe extern "C" fn ...>` forms exactly (trait-ification is
// P3-10's job). The anonymous C position union became the named real
// Rust union [`VfsFilePos`] (field `pos`; N2's `tnode` precedent —
// bindgen would otherwise degrade it to `__BindgenUnionField` blobs the
// moment its `vfs_dir_iter`/`pipe` members went native); consumers'
// `__bindgen_anon_1` sites re-pointed to `.pos`. Copy fidelity: all
// three derived Copy/Clone in the pre-nativization bindgen output.
// `vfs_inode_ref` (kernel/inc/types.h) deliberately stays
// bindgen-emitted — out of this wave's scope — embedded by value via its
// `crate::bindings` path, the sanctioned mixed-tier pattern. The C
// `__ALIGNED_CACHELINE` on the embedded `mutex_t` typedef gives the
// record align 64, carried here as `repr(align(64))` + the explicit
// `_pad0` bindgen emitted before `lock`.
// ---------------------------------------------------------------------------

/// Native replacement for the anonymous C union inside `struct vfs_file`
/// (bindgen's `vfs_file__bindgen_ty_1`, 16/8): the file position for
/// regular files, directory-iterator state for directories, or the
/// backing object pointer for device/pipe/socket files.
#[repr(C)]
#[derive(Copy, Clone)]
pub union VfsFilePos {
    pub f_pos: loff_t,
    pub dir_iter: crate::bindings::vfs_dir_iter,
    pub cdev: *mut cdev_t,
    pub blkdev: *mut blkdev_t,
    pub pipe: *mut pipe,
    pub sock: *mut crate::bindings::sock,
}

/// `struct vfs_file` — as of wave P3-10a this layout is NATIVE-OWNED
/// (post-P3-6 there is no bindgen, no wrapper.h, and no C consumer of
/// this record; `kernel/inc/vfs/vfs_types.h` no longer constrains it).
/// The anonymous union became the named `pos` field back in P3-N5; the
/// former `ops: *mut vfs_file_ops` table pointer is now a real Rust
/// trait object, `Option<&'static dyn FileOps>` (a 16-byte fat pointer;
/// `None` replaces the old null-ops "direct device/socket I/O" marker).
/// The explicit `_pad0` bindgen used to emit before `lock` is gone: the
/// fat `ops` pointer widened the header so `private_data` now ends
/// exactly at the 64-byte boundary `lock` sits on.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct VfsFile {
    pub list_entry: list_node_t,
    pub inode: vfs_inode_ref,
    pub f_flags: c_int,
    pub ref_count: c_int,
    pub ops: Option<&'static dyn FileOps>,
    pub private_data: *mut c_void,
    pub lock: crate::bindings::mutex_t,
    pub pos: VfsFilePos,
}

/// The per-filesystem/driver file-operations vtable — wave P3-10a's
/// replacement for the C-style `struct vfs_file_ops` fn-pointer table
/// (the ops-table redesign PILOT: full Rust style, C-compatible
/// interface removed). Implementors are zero-sized unit structs with a
/// `static` instance (`XV6FS_FILE_OPS`, `TMPFS_FILE_OPS`,
/// `PIPE_FILE_OPS`, `PTS_SLAVE_FILE_OPS`, `PTMX_MASTER_FILE_OPS`)
/// installed into [`VfsFile::ops`] as `Some(&STATIC)`.
///
/// Slot-nullability mapping (each old `Option<fn>` slot's `None`
/// dispatch behavior is preserved exactly):
///
/// * `read`/`write` — required methods: every historical table filled
///   both slots, so "ops present but slot `None`" never existed; the
///   old dispatch's missing-slot fallbacks keyed off the whole table
///   being null, which is now `VfsFile::ops == None`.
/// * `llseek` — default `Err(Errno::SPipe)`, exactly what
///   `vfs_filelseek` returned for a `None` slot ("not seekable").
/// * `release`/`fflush`/`fsync` — default `Ok(())`: a `None` slot was
///   silently skipped by `file_free`/`vfs_fput` (no call, no error
///   log), and `Ok(())` takes the identical no-log path.
/// * `poll`/`ioctl`/`fault` — return `Option<..>` where `None` means
///   "op not provided": their dispatch sites (`vfs_poll_scan`,
///   `vfs_ioctl`, `xv6_vm_call_vma_fault`) have genuine *fallback*
///   behavior for a missing slot (always-ready poll, `dev_ioctl`/
///   `ENOTTY`, generic file-page loader) that a sentinel `Errno` could
///   not represent without conflating it with a real driver error.
///   Note `fault`'s `Some(ptr)` payload may itself be null — that is
///   the driver's own "handled, but failed" result and is returned to
///   the faulting path as-is, NOT a cue to fall back.
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU.
pub trait FileOps: Sync {
    /// Read up to `count` bytes into `buf` (a user VA when `user`).
    ///
    /// # Safety
    /// `file` must be a live, open `vfs_file` whose `ops` is this
    /// instance; `buf` must be valid for `count` bytes in the `user`-
    /// selected address space. May sleep.
    unsafe fn read(&self, file: *mut VfsFile, buf: *mut c_char, count: usize, user: bool)
        -> KResult<isize>;

    /// Write up to `count` bytes from `buf` (a user VA when `user`).
    ///
    /// # Safety
    /// Same contract as [`FileOps::read`].
    unsafe fn write(&self, file: *mut VfsFile, buf: *const c_char, count: usize, user: bool)
        -> KResult<isize>;

    /// Compute (but do not store) the new file position.
    ///
    /// # Safety
    /// `file` must be a live, open `vfs_file` whose `ops` is this
    /// instance.
    unsafe fn llseek(&self, _file: *mut VfsFile, _offset: loff_t, _whence: c_int)
        -> KResult<loff_t> {
        Err(Errno::SPipe) // Not seekable (old `None`-slot behavior).
    }

    /// Last-reference teardown hook, called from `file_free`.
    ///
    /// # Safety
    /// `file` must be a live `vfs_file` holding its final reference;
    /// `inode` is `file`'s (possibly null) backing inode.
    unsafe fn release(&self, _inode: *mut vfs_inode, _file: *mut VfsFile) -> KResult<()> {
        Ok(())
    }

    /// Flush a byte range to stable storage. (No VFS dispatch site
    /// exists yet — slot preserved from the C table for the future
    /// `fsync(2)` path; xv6fs already implements it.)
    ///
    /// # Safety
    /// `file` must be a live, open `vfs_file` whose `ops` is this
    /// instance.
    unsafe fn fsync(&self, _file: *mut VfsFile, _start: loff_t, _len: loff_t) -> KResult<()> {
        Ok(())
    }

    /// Flush all dirty data, called from `vfs_fput` before the inode
    /// reference is dropped.
    ///
    /// # Safety
    /// `file` must be a live `vfs_file` holding its final reference.
    unsafe fn fflush(&self, _file: *mut VfsFile) -> KResult<()> {
        Ok(())
    }

    /// Poll for readiness: `Some(revents)` if this driver implements
    /// polling, `None` to let `vfs_poll_scan` fall back.
    ///
    /// # Safety
    /// `file` must be a live, open `vfs_file` whose `ops` is this
    /// instance.
    unsafe fn poll(&self, _file: *mut VfsFile, _events: core::ffi::c_short) -> Option<c_int> {
        None
    }

    /// Driver ioctl: `Some(result)` (the raw `c_int`, which may itself
    /// be a negative errno, passed through to userspace exactly as the
    /// old fn-pointer result was) or `None` to let `vfs_ioctl` fall
    /// back (`dev_ioctl` / `ENOTTY`).
    ///
    /// # Safety
    /// `file` must be a live, open `vfs_file` whose `ops` is this
    /// instance; `arg`'s validity is `cmd`-specific.
    unsafe fn ioctl(&self, _file: *mut VfsFile, _cmd: u64, _arg: *mut c_void) -> Option<c_int> {
        None
    }

    /// mmap demand-page hook: `Some(page)` if this driver handles
    /// file-backed faults (`page` null on failure, exactly as the old
    /// callback returned null), `None` to let `xv6_vm_call_vma_fault`
    /// fall back to the generic file-page loader.
    ///
    /// # Safety
    /// `file` must be a live, open `vfs_file` whose `ops` is this
    /// instance; `vma` must be a live mapping of `file` containing `va`.
    unsafe fn fault(&self, _file: *mut VfsFile, _vma: *mut crate::bindings::vma, _va: u64)
        -> Option<*mut c_void> {
        None
    }
}

// P3-10a layout facts — NATIVE-OWNED, no C mirror exists (bindgen,
// wrapper.h, and every C consumer are gone; these asserts document the
// new truth rather than pinning to a header). Kept: size/align and the
// two invariants the code actually leans on — `lock` starting exactly
// at the 64-byte cache-line boundary (the record's whole reason for
// `align(64)`, now satisfied without the old `_pad0` because the fat
// `ops` pointer widened the header to exactly 64 bytes) and the
// niche-optimized `Option<&'static dyn FileOps>` staying a plain fat
// pointer (16/8). Deleted: the stale C-fidelity offsets for the moved
// fields and the whole `vfs_file_ops` table block (the table type
// itself no longer exists).
const _: () = {
    assert!(core::mem::size_of::<VfsFilePos>() == 16, "vfs_file pos union size");
    assert!(core::mem::align_of::<VfsFilePos>() == 8, "vfs_file pos union alignment");
    assert!(core::mem::size_of::<Option<&'static dyn FileOps>>() == 16, "file ops fat pointer size");
    assert!(core::mem::align_of::<Option<&'static dyn FileOps>>() == 8, "file ops fat pointer alignment");
    assert!(core::mem::size_of::<VfsFile>() == 256, "vfs_file size");
    assert!(core::mem::align_of::<VfsFile>() == 64, "vfs_file alignment");
    assert!(core::mem::offset_of!(VfsFile, lock) == 64, "vfs_file.lock offset (cache-line boundary)");
    assert!(core::mem::offset_of!(VfsFile, pos) == 192, "vfs_file.pos offset");
};

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally per this crate's established convention (see `vfs/inode.rs`'s
// externs-block doc). Nearly everything is `safe`: passing a raw
// pointer is never itself unsafe, and every entry point here either
// null-checks internally or is called only with pointers this file
// already proved non-null. `printf` (C-variadic) is the one exception.
// ===========================================================================

// P3-D3c: `crate::lock::spinlock::RawSpinlock::init` is genuinely `unsafe fn` now
// that its `#[no_mangle]` export is gone; this file's original extern
// declaration asserted `safe fn` (usual FFI-facade convention). The thin
// wrapper preserves that safe facade for the unchanged call sites.
/// SAFETY: see [`crate::lock::spinlock::RawSpinlock::init`]'s contract.
fn spin_init(l: *mut spinlock_t, name: *mut c_char) {
    unsafe { crate::lock::spinlock::RawSpinlock::init(l, name) }
}

unsafe extern "C" {
    // printf.rs — C-variadic.

    // list.rs (list.h static inlines have no external linkage in the C
    // original either — reimplemented locally below, same precedent as
    // `vfs/inode.rs`'s `ln_init`/`ln_detach`). Nothing extern here.

}

// P3-D3b: lock/mutex.rs's `mutex_init` (for `file->lock`) is a plain safe
// Rust fn now that its `#[no_mangle]` export is gone; reached by crate path.
use crate::lock::mutex::mutex_init;

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
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_cache_shrink(cache: *mut slab_cache_t, nums: c_int) -> c_int {
    unsafe { crate::mm::slab_cache_shrink(cache as *mut crate::mm::slab::SlabCache, nums) }
}

// `kalloc`/`kfree` are genuinely `unsafe fn` in `crate::mm::kalloc`; thin
// safe wrappers preserve the `safe fn` facade the old redeclarations
// asserted.
/// SAFETY: see [`crate::mm::kalloc::kalloc`]'s contract.
#[inline]
fn kalloc() -> *mut c_void {
    unsafe { crate::mm::kalloc::Kmem::kalloc() }
}
/// SAFETY: `pa` must originate from `kalloc` above.
#[inline]
fn kfree(pa: *mut c_void) {
    unsafe { crate::mm::kalloc::Kmem::kfree(pa) };
}
// P3-1D mesh sweep: dev/cdev.rs, dev/blkdev.rs, dev/dev.rs, net.rs, and
// sysnet.rs are all in scope for this wave; these become plain crate-path
// imports instead of `extern "C"` redeclarations. `cdev_read`/`cdev_write`
// take a real `bool_` (`c_uint`) `user` parameter (not `c_int`, this
// file's former locally-declared view) — the two call sites below now
// cast, matching the `ops.read`/`ops.write` custom-fn call sites right
// next to them, which already did `user as bool_`.
use crate::dev::cdev::{cdev_get, cdev_put, cdev_read, cdev_write};
use crate::dev::blkdev::{blkdev_get, blkdev_put};
use crate::dev::dev::dev_ioctl;
use crate::net::mbufq_init;
// Wave P3-8d: `sysnet.rs`'s `sock_lock`/`sockets` (two independently-
// paired globals) were migrated to a single `SpinLock<*mut sock>` --
// `SOCKETS` is locked directly below instead of a raw `spinlock_t`
// handle (see `vfs_sockalloc`'s updated call site and this file's
// module doc).
use crate::sysnet::SOCKETS;

// P3-1C mesh sweep: vfs/{inode,fs,fdtable,pipe}.rs are in scope for this
// wave; converted from `extern "C"` redeclarations to plain crate-path
// items (identical signatures, same `crate::bindings::*` types this file
// already imports).
use crate::vfs::fs::FsStruct;
use crate::vfs::inode::VfsInode;
use crate::vfs::pipe::Pipe;

// `kassert!`'s canonical home is crate root / `crate::kstd` (P3-CS2
// centralization).
use crate::kassert;

// ===========================================================================
// Small helpers: negative-errno constants, ERR_PTR family, mode/open-flag
// bits, `list.h` static-inline reimplementations, refcount atomics.
// ===========================================================================

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// `is_err`/`ptr_err`'s canonical home is `crate::kstd` (P3-CS2
// centralization). Note `kstd::ptr_err` returns `c_int`, not `isize` —
// this file's call sites already cast the result to `c_int` at the use
// site, so the narrower return type is a no-op there.
//
// P3-CS6: this file's internal fallible logic now returns [`KResult<T>`]
// (see the module doc's "Result-first" note in `kstd.rs`) — `err_ptr` is
// no longer needed locally (every former `err_ptr(..)` call site below is
// now `Err(Errno::..)`, converted to the transitional `ERR_PTR` encoding
// exactly once at each function's C-ABI boundary via
// [`result_to_errptr`]/[`result_to_neg_errno`]).
use crate::kstd::{is_err, ptr_err, result_to_errptr, result_to_neg_errno, Errno, KResult};

// `uabi/stat.h`'s `S_IF*`/`S_IS*` macros (full set — this file touches
// every file type `vfs/inode.rs` didn't need).
const S_IFMT: u32 = 0o170000;
const S_IFIFO: u32 = 0o010000;
const S_IFCHR: u32 = 0o020000;
const S_IFBLK: u32 = 0o060000;
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const S_IFSOCK: u32 = 0o140000;
#[inline(always)]
fn is_chr(mode: u32) -> bool {
    mode & S_IFMT == S_IFCHR
}
#[inline(always)]
fn is_blk(mode: u32) -> bool {
    mode & S_IFMT == S_IFBLK
}
#[inline(always)]
fn is_fifo(mode: u32) -> bool {
    mode & S_IFMT == S_IFIFO
}
#[inline(always)]
fn is_sock(mode: u32) -> bool {
    mode & S_IFMT == S_IFSOCK
}
#[inline(always)]
fn is_dir(mode: u32) -> bool {
    mode & S_IFMT == S_IFDIR
}
#[inline(always)]
fn is_reg(mode: u32) -> bool {
    mode & S_IFMT == S_IFREG
}

// `uabi/fcntl.h`'s `O_*` bits (musl layout: `O_ACCMODE = 03 | O_SEARCH`,
// but only the low two bits matter for the RDONLY/WRONLY/RDWR checks
// this file performs, exactly like the C original).
const O_RDONLY: c_int = 0o0;
const O_WRONLY: c_int = 0o1;
const O_RDWR: c_int = 0o2;
const O_ACCMODE: c_int = 0o3 | 0o10000000; // `O_SEARCH` = `O_PATH`.

// ---------------------------------------------------------------------------
// `list.h` `static inline` primitive this file needs (no external
// linkage in the C original — reimplemented natively, same precedent as
// `vfs/inode.rs`'s `ln_init`/`ln_detach`).
// ---------------------------------------------------------------------------

/// Mirrors `list_node_push_back()`.
///
/// # Safety
/// `head` must point to a live, circular `list_node_t` list head; `node`
/// must not already be linked into any list.
#[inline(always)]
unsafe fn ln_push_back(head: *mut list_node_t, node: *mut list_node_t) {
    unsafe {
        let tail = (*head).prev;
        (*node).prev = tail;
        (*node).next = head;
        (*tail).next = node;
        (*head).prev = node;
    }
}

/// Mirrors `list_node_detach()`.
///
/// # Safety
/// `node` must point to a live, linked `list_node_t`.
#[inline(always)]
unsafe fn ln_detach(node: *mut list_node_t) {
    unsafe {
        let prev = (*node).prev;
        let next = (*node).next;
        (*prev).next = next;
        (*next).prev = prev;
        (*node).prev = node;
        (*node).next = node;
    }
}

// ---------------------------------------------------------------------------
// `ref_count` atomics — mirrors `smp/atomic.h`'s `atomic_dec_unless`/
// `atomic_inc_unless` exactly (see `vfs/inode.rs`'s identical helpers
// and their ordering rationale).
// ---------------------------------------------------------------------------

impl VfsFile {
    /// N-METH: inherent assoc fn (was a free fn on the raw `*mut
    /// vfs_file`); raw-pointer param kept (P3-7b's refcount aliasing
    /// screen, same precedent as `VfsInode::refcount_atomic`).
    #[inline(always)]
    fn refcount_atomic<'a>(file: *mut vfs_file) -> &'a AtomicI32 {
        // SAFETY: `ref_count` is a plain C `int` field, same size/align as
        // `AtomicI32`; caller ensures `file` is a live, aligned `vfs_file`.
        unsafe { &*(ptr::addr_of_mut!((*file).ref_count) as *const AtomicI32) }
    }
}

fn atomic_oper_cond(
    a: &AtomicI32,
    mut op: impl FnMut(i32) -> i32,
    mut cond: impl FnMut(i32) -> bool,
) -> bool {
    let mut val = a.load(Ordering::Acquire);
    while cond(val) {
        let new_val = op(val);
        match a.compare_exchange(val, new_val, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => return true,
            Err(cur) => val = cur,
        }
    }
    false
}
#[inline(always)]
fn atomic_dec_unless(a: &AtomicI32, unless: i32) -> bool {
    atomic_oper_cond(a, |v| v - 1, move |v| v != unless)
}
#[inline(always)]
fn atomic_inc_unless(a: &AtomicI32, unless: i32) -> bool {
    atomic_oper_cond(a, |v| v + 1, move |v| v != unless)
}

// ===========================================================================
// Global state: the `vfs_file` slab cache, the global open-file table,
// and its counter.
// ===========================================================================

#[repr(transparent)]
struct FileSlabCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: written in full by `slab_cache_init` (called once from
// `__vfs_file_init`, before any `slab_alloc`/`slab_free` on this cache)
// and otherwise only touched through the C slab allocator's own
// internally-synchronized entry points, matching the identical
// `CacheCell` precedent in `kernel/proc/thread_group.rs` et al.
unsafe impl Sync for FileSlabCell {}
static __VFS_FILE_SLAB: FileSlabCell = FileSlabCell(UnsafeCell::new(MaybeUninit::zeroed()));
#[inline]
fn vfs_file_slab() -> *mut slab_cache_t {
    __VFS_FILE_SLAB.0.get() as *mut slab_cache_t
}

/// Wave P3-8f: `__vfs_ftable_lock` (a bare `spinlock_t`) + the global
/// open-file list head used to be two independently-paired globals (a
/// `KSpinlock::from_bindings` handle plus a separate `static mut
/// list_node_t`). Now the lock owns the list head directly
/// ([`crate::sync::SpinLock`], same P3-8b/8d precedent as `ramdisk.rs`/
/// `devtmpfs/superblock.rs`): `.lock()` returns a guard that `DerefMut`s
/// straight to the `list_node_t` head, so `ftable_attach`/`ftable_detach`
/// stop reaching through a raw `static mut`. The head is self-referential
/// once initialised (`next == prev == &head`, the `LIST_ENTRY_INITIALIZED`
/// idiom) — that self-reference can't be written in a `const` initializer
/// (the static's own address isn't knowable inside its own initializer),
/// so the lock starts life wrapping a null-pointered placeholder and
/// [`__vfs_file_init`] completes the one-time fixup through the guard
/// (now briefly under the lock — a strict improvement; it was a
/// `spin_init` on a separate `spinlock_t` plus a raw write to a separate
/// `static mut` before).
static __VFS_FTABLE: SpinLock<list_node_t> = SpinLock::new(
    c"vfs_file_table_lock",
    list_node_t { prev: core::ptr::null_mut(), next: core::ptr::null_mut() },
);
static __VFS_OPEN_FILE_COUNT: AtomicI32 = AtomicI32::new(0);

impl VfsFile {
    /// A [`KMutex`] handle onto this file's embedded `lock` (`mutex_t`),
    /// which serializes `pos`/read/write/seek/truncate. N-METH: inherent
    /// method (was a free fn on the raw `*mut vfs_file`); with `&mut self`
    /// the address-of is a plain safe `ptr::addr_of_mut!`, no `unsafe`.
    fn file_lock(&mut self) -> KMutex {
        KMutex::from_ptr(ptr::addr_of_mut!(self.lock))
    }
}

impl VfsFile {
    /// N-METH: inherent assoc fn (was the free fn `ftable_attach`); raw
    /// `file` param kept (operates on a not-yet-live/just-built file, per
    /// the module doc's lifetime-honesty exclusions).
    fn ftable_attach(file: *mut vfs_file) {
        let mut g = __VFS_FTABLE.lock();
        // SAFETY: `g` proves the lock is held; `&raw mut *g` is the live
        // global open-file list head (initialized once by
        // `VfsFile::vfs_file_init` before any attach/detach can race in);
        // `file->list_entry` is not yet linked anywhere (every caller
        // passes a freshly allocated file).
        unsafe {
            ln_push_back(&raw mut *g, ptr::addr_of_mut!((*file).list_entry));
        }
        let count = __VFS_OPEN_FILE_COUNT.fetch_add(1, Ordering::SeqCst) + 1;
        drop(g);
        kassert!(count > 0, "vfs file open count overflow");
    }

    /// N-METH: inherent assoc fn (was the free fn `ftable_detach`); raw
    /// `file` param kept (called from [`VfsFile::vfs_fput`] on the file
    /// being torn down).
    fn ftable_detach(file: *mut vfs_file) {
        let g = __VFS_FTABLE.lock();
        // SAFETY: `g` proves the lock is held (the critical section the head
        // and every linked node share); `file->list_entry` is linked into
        // `__VFS_FTABLE` (every caller passes a file that went through
        // `ftable_attach`), so `ln_detach` only relinks its own neighbours.
        unsafe { ln_detach(ptr::addr_of_mut!((*file).list_entry)) };
        let count = __VFS_OPEN_FILE_COUNT.fetch_sub(1, Ordering::SeqCst) - 1;
        drop(g);
        kassert!(count >= 0, "vfs file open count underflow");
    }

    /// N-METH: inherent assoc fn (was the free fn `file_alloc`); no live
    /// receiver exists yet (constructs a fresh file), so it stays a raw
    /// out-pointer-returning constructor per the module doc's
    /// lifetime-honesty exclusions.
    fn file_alloc() -> *mut vfs_file {
        let file = slab_alloc(vfs_file_slab()) as *mut vfs_file;
        if file.is_null() {
            return ptr::null_mut();
        }
        // SAFETY: `slab_alloc` returned a fresh, exclusively-owned
        // `size_of::<vfs_file>()` allocation; zeroing then field-initializing
        // it mirrors the C original's `memset` + explicit-field-init. `ops`
        // is written explicitly: all-zero bytes are not a documented `None`
        // for the `Option<&'static dyn FileOps>` fat pointer (P3-10a), so
        // the raw `write` (a pure store — no read/drop of the zeroed bytes)
        // establishes the field's validity before anyone loads it.
        unsafe {
            ptr::write_bytes(file, 0, 1);
            ptr::addr_of_mut!((*file).ops).write(None);
            mutex_init(ptr::addr_of_mut!((*file).lock), c"vfs_file_lock".as_ptr() as *mut c_char);
            (*file).ref_count = 1;
        }
        file
    }

    /// N-METH: inherent assoc fn (was the free fn `file_free`); raw
    /// `file` param kept (frees the object -- lifetime-honesty exclusion).
    fn file_free(file: *mut vfs_file) {
        if file.is_null() {
            return;
        }
        // SAFETY: `file` is a live, exclusively-owned `vfs_file` about to be
        // returned to the slab (every caller holds the last reference);
        // `release` is the driver's teardown hook with exactly that
        // contract. A missing table (`None` ops) skips the call, and the
        // trait's default `release` is `Ok(())` — both reproduce the old
        // null-table/`None`-slot "silently skip" behavior.
        unsafe {
            if let Some(ops) = (*file).ops {
                if let Err(e) = ops.release((*file).inode.inode, file) {
                    crate::kprintln!("__vfs_file_free: file ops release failed, errno={}", e.neg());
                }
            }
        }
        slab_free(file as *mut c_void);
    }

    /// N-METH: inherent assoc fn (was the free fn `__vfs_file_init`); name
    /// kept verbatim (NAMESPACING, not renaming) per the `8283168`/
    /// `VfsInode::vfs_idup` precedent.
    pub(crate) extern "C" fn __vfs_file_init() {
        let ret = slab_cache_init(
            vfs_file_slab(),
            c"vfs_file_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<vfs_file>(),
            (SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP) as u64,
        );
        kassert!(ret == 0, "Failed to initialize vfs_file_cache slab cache");
        // Complete the one-time list-head self-reference fixup through the
        // guard (Wave P3-8f: `__VFS_FTABLE` owns its lock now, so the head is
        // reached via `.lock()` rather than a `spin_init` on a separate
        // `spinlock_t` plus a raw write to a separate `static mut` — the
        // `SpinLock::new` `const fn` already put the lock in its initialised
        // state). Called exactly once, at boot, before any other thread can
        // observe `__VFS_FTABLE` (matches the C original's single
        // `vfs_init()` call site).
        {
            let mut head = __VFS_FTABLE.lock();
            let addr: *mut list_node_t = &raw mut *head;
            // SAFETY: `addr` is the live, stable address of the list head the
            // guard owns; writing its own address into `next`/`prev` is the
            // `LIST_ENTRY_INITIALIZED` self-reference idiom.
            unsafe {
                (*addr).prev = addr;
                (*addr).next = addr;
            }
        }
        __VFS_OPEN_FILE_COUNT.store(0, Ordering::SeqCst);
    }

    /// N-METH: inherent assoc fn (was the free fn `__vfs_file_shrink_cache`).
    pub(crate) extern "C" fn __vfs_file_shrink_cache() {
        slab_cache_shrink(vfs_file_slab(), 0x7fffffff);
    }
}

// ===========================================================================
// Open-path helpers: character/block device files.
// ===========================================================================

impl VfsFile {
    /// Open a character device file into `self`. Internal helper (P3-CS6:
    /// returns [`KResult`] — its sole caller, [`vfs_fileopen_inner`], is
    /// itself `KResult`-returning, so the former `c_int` error encoding
    /// just added an extra decode step at the one call site). N-METH:
    /// inherent method on the file it builds up (was a free fn taking the
    /// fresh `&mut VfsFile`).
    fn open_cdev(&mut self, inode: *mut vfs_inode) -> KResult<()> {
        // P3-7b: `self` is the exclusively-owned fresh file (sole caller
        // `vfs_fileopen_inner` reborrows it) — `ops` is safe field access;
        // the `pos` union write and the raw-`self` `open_file` callback
        // keep raw.
        // SAFETY: non-null `inode` (every call site checks `S_ISCHR`
        // first, which already dereferenced `inode->mode`).
        let raw_cdev = unsafe { (*inode).dev_mnt.cdev };
        let major = ((raw_cdev >> 20) & 0xFFF) as c_int;
        let minor = (raw_cdev & 0xFFFFF) as c_int;
        let cdev = cdev_get(major, minor);
        if is_err(cdev) {
            // Cross-module `ERR_PTR` this cluster doesn't own
            // (`dev/cdev.rs`) -- passed through verbatim, same precedent
            // as `vfs_syscall.rs`'s `Errno::Raw` sites.
            return Err(Errno::Raw(ptr_err(cdev)));
        }
        if cdev.is_null() {
            return Err(Errno::NoDev);
        }

        // SAFETY: `cdev` is non-null and non-error (checked above); `self`
        // is live; `open_file` is a driver hook with the documented
        // contract that it may install `file->ops`/`file->private_data`
        // (P3-10c: `CdevOps::open_file` returns `None` — the default,
        // mirroring the old `None` table slot — to select the
        // direct-device-I/O fallback below).
        let open_file = unsafe { (*cdev).ops.and_then(|o| o.open_file(cdev, &raw mut *self)) };
        if let Some(ret) = open_file {
            if ret != 0 {
                cdev_put(cdev);
                return Err(Errno::Raw(ret));
            }
            if self.ops.is_some() {
                // `open_file` took over: the VFS does not also store
                // `file->cdev` (the cdev manages its own refcount in that
                // case), matching the C original's comment exactly.
                cdev_put(cdev);
                return Ok(());
            }
        }

        // SAFETY: `pos.cdev` is the active union member of a cdev file.
        unsafe { self.pos.cdev = cdev };
        self.ops = None; // Device files use direct device I/O.
        Ok(())
    }

    /// Open a block device file into `self`. Always succeeds: if the
    /// device can't be found, the file is left with a null `blkdev`/`ops`
    /// so it can still be `stat`ed but not used for I/O (matches the C
    /// original's "device not found -- allow open for stat but not I/O"
    /// comment). P3-CS6: dropped the always-`0` `c_int` return the C ABI
    /// encoding required — this helper's only caller
    /// ([`vfs_fileopen_inner`]) never observed a non-zero result (every
    /// path below ends in `return`, none in a nonzero value), so the
    /// signature change is a behavior-preserving simplification, not a
    /// semantic one. N-METH: inherent method (was a free fn on the raw
    /// `*mut vfs_file`; `self` narrows the `unsafe` to just the `pos`
    /// union writes).
    fn open_blkdev(&mut self, inode: *mut vfs_inode) {
        // SAFETY: non-null `inode`.
        let raw_bdev = unsafe { (*inode).dev_mnt.bdev };
        let major = ((raw_bdev >> 20) & 0xFFF) as c_int;
        let minor = (raw_bdev & 0xFFFFF) as c_int;
        let blkdev = blkdev_get(major, minor);
        if is_err(blkdev) || blkdev.is_null() {
            // Device not found -- allow open for stat but not I/O.
            // SAFETY: `pos.blkdev` is the active union member of a blkdev file.
            unsafe { self.pos.blkdev = ptr::null_mut() };
            self.ops = None;
            return;
        }
        // SAFETY: `pos.blkdev` is the active union member of a blkdev file.
        unsafe { self.pos.blkdev = blkdev };
        self.ops = None; // Device files use direct device I/O.
    }
}

// ===========================================================================
// Public VFS file API.
// ===========================================================================

/// Core logic behind [`vfs_fileopen`], factored out as a private helper
/// returning [`KResult`] (P3-CS6, following the `vfs_syscall.rs` P3-CS4/5
/// `*_inner` precedent). Every former `err_ptr(..)` site below is now
/// `Err(Errno::..)` with the exact same errno value; the one `KResult` ->
/// `ERR_PTR` conversion happens once, in [`vfs_fileopen`] itself, via
/// [`result_to_errptr`]. Cross-module already-negative `c_int` results
/// (`vfs_inode_get_ref`, the inode `open` callback) stay `Err(Errno::Raw(..))`
/// — a boundary this cluster doesn't own, same precedent as
/// `vfs_syscall.rs`'s `open_inner`.
impl VfsFile {
/// N-METH: inherent assoc fn (was the free fn `vfs_fileopen_inner`); raw
/// `inode`/return kept (constructs a fresh file -- lifetime-honesty
/// exclusion).
fn vfs_fileopen_inner(inode: *mut vfs_inode, f_flags: c_int) -> KResult<*mut vfs_file> {
    if inode.is_null() || unsafe { (*inode).sb.is_null() } {
        return Err(Errno::Inval);
    }
    // SAFETY: non-null `inode`.
    let mode = unsafe { (*inode).mode };

    if is_sock(mode) {
        return Err(Errno::NxIo); // Sockets cannot be opened via inode.
    }
    if is_fifo(mode) {
        return Err(Errno::NxIo); // Named pipes not supported via open yet.
    }

    VfsInode::vfs_ilock(inode);
    // @TODO: check permission
    let file = VfsFile::file_alloc();
    if file.is_null() {
        VfsInode::vfs_iunlock(inode);
        return Err(Errno::NoMem);
    }
    // SAFETY: `file_alloc` returned a fresh, exclusively-owned, non-null
    // `vfs_file` — an honest `&mut` for the build-up (P3-7b). It is not
    // published until `ftable_attach` and not reachable by any other
    // thread even after, so plain fields (`inode`, `ops`, `f_flags`) go
    // safe; every error path reborrows `&raw mut *f` for `file_free` and
    // `return`s immediately, so the borrow never outlives the free. The
    // `pos` union write and the raw-pointer C-ABI callbacks keep raw.
    let f = unsafe { &mut *file };
    let ret = FsStruct::vfs_inode_get_ref(inode, &raw mut f.inode);
    if ret != 0 {
        VfsFile::file_free(&raw mut *f);
        VfsInode::vfs_iunlock(inode);
        return Err(Errno::Raw(ret));
    }

    if is_chr(mode) {
        if let Err(e) = f.open_cdev(inode) {
            VfsInode::vfs_iunlock(inode);
            FsStruct::vfs_inode_put_ref(&raw mut f.inode);
            VfsFile::file_free(&raw mut *f);
            return Err(e);
        }
        VfsInode::vfs_iunlock(inode);
        VfsFile::ftable_attach(&raw mut *f);
        f.f_flags = f_flags;
        return Ok(&raw mut *f);
    }

    if is_blk(mode) {
        f.open_blkdev(inode); // Infallible -- see its own doc comment.
        VfsInode::vfs_iunlock(inode);
        VfsFile::ftable_attach(&raw mut *f);
        f.f_flags = f_flags;
        return Ok(&raw mut *f);
    }

    // Regular files and directories use the driver's `InodeOps::open`
    // (P3-10b: a required trait method — the old `None`-slot
    // cleanup-and-`ENOSYS` path had no live instance; a driver refusing
    // an open returns `Err`, taking the identical cleanup below).
    // SAFETY: `inode` is live and `&raw mut *f` reborrows the exclusive
    // reference as the raw pointer `open` still takes; the callback runs
    // with the inode lock held, matching its documented contract.
    if let Err(e) = unsafe { crate::vfs::inode::inode_ops(inode).open(inode, &raw mut *f, f_flags) } {
        VfsInode::vfs_iunlock(inode);
        FsStruct::vfs_inode_put_ref(&raw mut f.inode);
        VfsFile::file_free(&raw mut *f);
        return Err(e);
    }
    if f.ops.is_none() {
        VfsInode::vfs_iunlock(inode);
        FsStruct::vfs_inode_put_ref(&raw mut f.inode);
        VfsFile::file_free(&raw mut *f);
        crate::kprintln!("vfs_fileopen: file operations not set by inode open");
        return Err(Errno::Inval);
    }

    VfsInode::vfs_iunlock(inode);
    VfsFile::ftable_attach(&raw mut *f);
    f.f_flags = f_flags;
    // SAFETY: `pos.f_pos` is the active union member of a regular file.
    unsafe { f.pos.f_pos = 0 };
    Ok(&raw mut *f)
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `vfs_fileopen`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_fileopen(inode: *mut vfs_inode, f_flags: c_int) -> *mut vfs_file {
    result_to_errptr(VfsFile::vfs_fileopen_inner(inode, f_flags))
}

/// Release a file reference. Decrements the file's reference count; on
/// last reference, detaches from the global table, flushes/closes the
/// backing object (cdev/blkdev/pipe), drops the inode reference, and
/// frees the `vfs_file`. N-METH: inherent assoc fn (was the free
/// `extern "C"` fn `vfs_fput`); name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_fput(file: *mut vfs_file) {
    if file.is_null() {
        return;
    }
    if atomic_dec_unless(VfsFile::refcount_atomic(file), 1) {
        return;
    }
    // File descriptors are shared through dup, thus when refcount
    // reaches 1, no other threads will be using it. No need to lock the
    // file structure.
    VfsFile::ftable_detach(file);

    // SAFETY: the final reference was just dropped above (the `dup`
    // sharing means refcount==1 => genuinely exclusive), so this thread
    // owns `file` outright — an honest `&mut` for the teardown (P3-7b,
    // mirroring `fdtable.rs`'s `vfs_fdtable_put`). The borrow ends before
    // `file_free` consumes the raw pointer. Plain fields (`ops`,
    // `f_flags`, `inode`) go safe; the `pos` union and the raw-`file`
    // `fflush` dispatch stay `unsafe`.
    let f = unsafe { &mut *file };

    let inode = FsStruct::vfs_inode_deref(&raw mut f.inode);

    // Flush dirty pages before releasing the inode reference, so all
    // data is written to disk before the inode can be torn down (avoids
    // the flush worker racing with inode destruction). `fflush` is the
    // driver's final-reference "flush dirty data" hook; the trait's
    // default is `Ok(())`, reproducing the old `None`-slot skip, and
    // `-EAGAIN` stays un-logged exactly as the old raw-`c_int` compare.
    if let Some(ops) = f.ops {
        // SAFETY: `&raw mut *f` reborrows the exclusive reference as the
        // raw pointer `fflush` still takes (P3-7b keeps trait dispatch
        // raw).
        if let Err(e) = unsafe { ops.fflush(&raw mut *f) } {
            if e.neg() != neg(EAGAIN) {
                crate::kprintln!("vfs_fput: fflush failed: {}", e.neg());
            }
        }
    }

    // Handle special file cleanup. Anonymous-pipe cleanup (pipe != NULL,
    // inode == NULL) is handled by `pipe_file_ops.release` via
    // `file_free` below, not here.
    if !inode.is_null() {
        // SAFETY: non-null `inode`.
        let mode = unsafe { (*inode).mode };
        if is_chr(mode) {
            // SAFETY: `pos.cdev` is the active union member of a cdev file.
            let cdev = unsafe { f.pos.cdev };
            if !cdev.is_null() {
                let ret = cdev_put(cdev);
                // SAFETY: same active-union-member store.
                unsafe { f.pos.cdev = ptr::null_mut() };
                if ret != 0 {
                    crate::kprintln!("vfs_fput: cdev_put failed: {}", ret);
                }
            }
        } else if is_blk(mode) {
            // SAFETY: `pos.blkdev` is the active union member of a blkdev file.
            let ret = unsafe { blkdev_put(f.pos.blkdev) };
            // SAFETY: same active-union-member store.
            unsafe { f.pos.blkdev = ptr::null_mut() };
            if ret != 0 {
                crate::kprintln!("vfs_fput: blkdev_put failed: {}", ret);
            }
        } else if is_fifo(mode) {
            // SAFETY: `pos.pipe` is the active union member of a fifo file.
            let pi = unsafe { f.pos.pipe };
            if !pi.is_null() {
                let writable = (f.f_flags & O_ACCMODE) != O_RDONLY;
                Pipe::pipe_close(pi, writable as c_int);
            }
        }
        // Sockets are not opened via inodes, so no cleanup here.
    }

    FsStruct::vfs_inode_put_ref(&raw mut f.inode);
    VfsFile::file_free(&raw mut *f);
}

/// Duplicate a file reference (increment refcount). Returns `NULL` if
/// `file` was already closed (or `NULL`). N-METH: inherent assoc fn (was
/// the free `extern "C"` fn `vfs_fdup`); name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_fdup(file: *mut vfs_file) -> *mut vfs_file {
    if file.is_null() {
        return ptr::null_mut();
    }
    if !atomic_inc_unless(VfsFile::refcount_atomic(file), 0) {
        return ptr::null_mut(); // File was already closed.
    }
    file
}
}

impl VfsFile {
/// Core logic behind [`vfs_ioctl`], factored out as a private method
/// returning [`KResult`] (P3-CS6). Only `EBADF`/`ENODEV`/`ENOTTY` are
/// this method's own failures; the driver-callback (`ops.ioctl`) and
/// `dev_ioctl` results pass through as `Ok` unconditionally (they may
/// themselves already be a negative `E*` value), matching the original's
/// unconditional early `return`s. N-METH: inherent method (was the free
/// fn `vfs_ioctl_inner(file: &mut VfsFile, ..)`).
fn ioctl_inner(&mut self, cmd: u64, arg: *mut c_void) -> KResult<c_int> {
    // P3-7b: `self` is the reference the `vfs_ioctl` boundary validated
    // once, so plain fields (`ops`, `inode`) are safe access; only the
    // `pos` union read and the raw-`self` trait dispatch (unchanged
    // signature) keep their `unsafe`.
    let ops = self.ops;

    // Fast path: character / block device files -- dispatch to the
    // device layer.
    let inode = FsStruct::vfs_inode_deref(&raw mut self.inode);
    if !inode.is_null() {
        // SAFETY: non-null `inode`.
        let mode = unsafe { (*inode).mode };
        if is_blk(mode) || is_chr(mode) {
            // Prefer custom file ops (installed by cdev open_file, e.g.
            // PTY master). `ioctl`'s `None` return means "op not
            // provided" — fall through, exactly like the old `None`
            // table slot.
            // SAFETY: `&raw mut *self` reborrows the live reference as
            // the raw pointer `ioctl` still takes (P3-7b keeps trait
            // dispatch raw); the callback is synchronous.
            if let Some(ret) = ops.and_then(|o| unsafe { o.ioctl(&raw mut *self, cmd, arg) }) {
                return Ok(ret);
            }
            // SAFETY: the cdev pointer reinterprets as `device_t*`
            // because `device_t dev` is `cdev_t`'s first field (repr(C),
            // matches the C original's cast exactly).
            let dev = unsafe { self.pos.cdev as *mut device_t };
            if !dev.is_null() {
                return Ok(dev_ioctl(dev, cmd, arg));
            }
            return Err(Errno::NoDev);
        }
    }

    // Fallback: custom file descriptors (pipes, sockets, etc.).
    // SAFETY: see the device-path callback above.
    if let Some(ret) = ops.and_then(|o| unsafe { o.ioctl(&raw mut *self, cmd, arg) }) {
        return Ok(ret);
    }

    Err(Errno::NotTy)
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `vfs_ioctl`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_ioctl(file: *mut vfs_file, cmd: u64, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return Errno::BadF.neg(); // Same errno the old null check produced.
    }
    // SAFETY: non-null (just checked); the caller passes a live, open
    // `vfs_file` it holds a reference to, valid for the call. P3-7b
    // boundary conversion — see the module doc's aliasing note.
    match (unsafe { &mut *file }).ioctl_inner(cmd, arg) {
        Ok(v) => v,
        Err(e) => e.neg(),
    }
}
}

impl VfsFile {
/// Core logic behind [`vfs_fileread`], factored out as a private method
/// returning [`KResult`] (P3-CS6). Every own-failure early return
/// (`EBADF`/`EOPNOTSUPP`/`EISDIR`/`EINVAL`) is now the matching `Errno`
/// variant; the driver-callback (`ops.read`) and `cdev_read` results
/// pass through as `Ok` unconditionally (their `isize` may itself
/// already be a negative errno), matching the original's unconditional
/// `return`s of those values. N-METH: inherent method (was the free fn
/// `vfs_fileread_inner(file: &mut VfsFile, ..)`).
fn read_inner(&mut self, buf: *mut c_void, n: usize, user: c_int) -> KResult<isize> {
    if buf.is_null() {
        return Err(Errno::Fault);
    }
    if n == 0 {
        return Ok(0); // POSIX: zero-length read succeeds.
    }

    // P3-7b: `self` is a reference — plain fields (`ops`, `f_flags`,
    // `inode`) are safe access; the `pos` union and the raw-`self` trait
    // dispatch (unchanged signature) keep their `unsafe`.
    let inode = FsStruct::vfs_inode_deref(&raw mut self.inode);

    // Handle pipe/socket read -- these don't have inodes.
    if inode.is_null() {
        // (`read` is a required trait method, so "table present but read
        // slot missing" — which never occurred with the fn-pointer
        // tables either — can no longer be expressed; only a missing
        // table reaches the `BadF` arm, same outcomes as before.)
        let Some(ops) = self.ops else {
            return Err(Errno::BadF); // Not a readable file object.
        };
        let _g = self.file_lock().lock();
        if self.f_flags & O_ACCMODE == O_WRONLY {
            return Err(Errno::BadF); // File not opened for reading.
        }
        // SAFETY: `&raw mut *self` reborrows the live reference as the
        // raw pointer `read` still takes; `buf` is valid for `n` bytes;
        // `read` may sleep, held under `file_lock` exactly as the C
        // original does for this path (P3-7b keeps trait dispatch raw).
        return unsafe { ops.read(&raw mut *self, buf as *mut c_char, n, user != 0) };
    }

    // SAFETY: `inode` is non-null (checked above) and stays live while
    // `self` (its holder) is open — a shared read of its `mode`.
    let inode_mode = unsafe { (*inode).mode };

    // Character device read: check access and call without holding the
    // file lock across the actual I/O -- cdev operations may sleep and
    // handle their own synchronization, matching the C original.
    if is_chr(inode_mode) {
        let _g = self.file_lock().lock();
        if self.f_flags & O_ACCMODE == O_WRONLY {
            return Err(Errno::BadF);
        }
        return if let Some(ops) = self.ops {
            // Custom file ops installed by cdev open_file (e.g. PTY master).
            // SAFETY: see the pipe/socket read call site above.
            unsafe { ops.read(&raw mut *self, buf as *mut c_char, n, user != 0) }
        } else {
            // SAFETY: `pos.cdev` is the active union member of a cdev file.
            let cdev = unsafe { self.pos.cdev };
            Ok(cdev_read(cdev, user as bool_, buf, n) as isize)
        };
    }

    let _g = self.file_lock().lock();
    if self.f_flags & O_ACCMODE == O_WRONLY {
        return Err(Errno::BadF); // File not opened for reading.
    }

    // Block device read -- not directly supported, use the buffer cache.
    if is_blk(inode_mode) {
        return Err(Errno::OpNotSupp);
    }

    // Regular files.
    // Note: we do NOT lock the inode here. The driver callback (e.g.
    // xv6fs_file_read) is responsible for acquiring the inode lock to
    // read size and data. This avoids lock-ordering issues where the
    // driver may need to acquire transactions that conflict with VFS
    // locking order (transaction -> superblock -> inode).
    if !is_reg(inode_mode) {
        return Err(if is_dir(inode_mode) { Errno::IsDir } else { Errno::Inval });
    }
    let Some(ops) = self.ops else {
        return Err(Errno::OpNotSupp);
    };
    // Pass the requested size to the driver; the driver handles EOF and
    // size checks internally. A driver error propagates as `Err` (and,
    // exactly like the old negative-`isize` return, skips the position
    // bump).
    // SAFETY: see the pipe/socket read call site above.
    let ret = unsafe { ops.read(&raw mut *self, buf as *mut c_char, n, user != 0) }?;
    if ret > 0 {
        // SAFETY: `pos.f_pos` is the active union member of a regular file.
        unsafe { self.pos.f_pos += ret as loff_t };
    }
    Ok(ret)
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `vfs_fileread`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_fileread(file: *mut vfs_file, buf: *mut c_void, n: usize, user: c_int) -> isize {
    if file.is_null() {
        return Errno::BadF.neg() as isize; // Same errno the old null check produced.
    }
    // SAFETY: non-null (just checked); the caller passes a live, open
    // `vfs_file` it holds a reference to. P3-7b boundary conversion.
    match (unsafe { &mut *file }).read_inner(buf, n, user) {
        Ok(v) => v,
        Err(e) => e.neg() as isize,
    }
}
}

impl VfsFile {
/// Core logic behind [`vfs_filestat`], factored out as a private method
/// returning [`KResult`] (P3-CS6). Only `EBADF`/`EFAULT` are this
/// method's own failures; a filesystem-supplied `getattr`'s result
/// passes through as `Ok` unconditionally (it may itself already be a
/// negative errno), matching the original's unconditional `return`.
/// N-METH: inherent method (was the free fn `vfs_filestat_inner`).
fn stat_inner(&mut self, out: *mut stat) -> KResult<c_int> {
    if out.is_null() {
        return Err(Errno::Fault);
    }

    // P3-7b: `self` is a reference — `inode`/`ops` are safe field access
    // (the `out` writes and the raw-`inode` getattr dispatch stay raw).
    let inode = FsStruct::vfs_inode_deref(&raw mut self.inode);
    if inode.is_null() {
        // Custom file descriptors (PTY slaves, etc.) have no backing
        // inode. Return a synthetic stat indicating a character device
        // so that isatty() works correctly.
        if self.ops.is_some() {
            // SAFETY: non-null `out` (checked above).
            unsafe {
                ptr::write_bytes(out, 0, 1);
                (*out).mode = S_IFCHR | 0o666;
            }
            return Ok(0);
        }
        return Err(Errno::BadF);
    }

    // SAFETY: non-null `inode`. P3-10b: `getattr` is a required trait
    // method, so the generic fallback below now keys on the whole ops
    // table being absent (the zeroed dummy root inode) — exactly the
    // reachable half of the old `None` check.
    if let Some(ops) = unsafe { (*inode).ops } {
        // SAFETY: `inode` (not locked — the driver locks it) and `out`
        // are both live; `getattr` is a filesystem driver callback.
        return match unsafe { ops.getattr(inode, out) } {
            Ok(()) => Ok(0),
            Err(e) => Err(e),
        };
    }

    // Generic fallback when the inode has no driver ops (the dummy
    // VFS-root inode).
    VfsInode::vfs_ilock(inode);
    // SAFETY: non-null `inode`/`out`.
    unsafe {
        ptr::write_bytes(out, 0, 1);
        (*out).dev = if !(*inode).sb.is_null() { (*inode).sb as u64 as i32 } else { 0 };
        (*out).ino = (*inode).ino;
        (*out).mode = (*inode).mode;
        (*out).nlink = (*inode).n_links;
        (*out).size = (*inode).size as u64;
    }
    VfsInode::vfs_iunlock(inode);
    Ok(0)
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `vfs_filestat`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_filestat(file: *mut vfs_file, out: *mut stat) -> c_int {
    if file.is_null() {
        return Errno::BadF.neg(); // Same errno the old null check produced.
    }
    // SAFETY: non-null (just checked); live open file. P3-7b boundary.
    match (unsafe { &mut *file }).stat_inner(out) {
        Ok(v) => v,
        Err(e) => e.neg(),
    }
}

/// Core logic behind [`vfs_filewrite`], factored out as a private method
/// returning [`KResult`] (P3-CS6) — same shape/rationale as
/// [`VfsFile::read_inner`] with the `O_RDONLY`/`O_WRONLY` access checks
/// swapped. N-METH: inherent method (was the free fn `vfs_filewrite_inner`).
fn write_inner(
    &mut self,
    buf: *const c_void,
    n: usize,
    user: c_int,
) -> KResult<isize> {
    if buf.is_null() {
        return Err(Errno::Fault);
    }
    if n == 0 {
        return Ok(0); // POSIX: zero-length write succeeds.
    }

    // P3-7b: `self` is a reference (see `VfsFile::read_inner`'s note).
    let inode = FsStruct::vfs_inode_deref(&raw mut self.inode);

    // Handle pipe/socket write -- these don't have inodes.
    if inode.is_null() {
        let Some(ops) = self.ops else {
            return Err(Errno::BadF); // Not a writable file object.
        };
        let _g = self.file_lock().lock();
        if self.f_flags & O_ACCMODE == O_RDONLY {
            return Err(Errno::BadF); // File not opened for writing.
        }
        // SAFETY: `&raw mut *self` reborrows the live reference as the
        // raw pointer `write` still takes; `buf` is valid for `n` bytes;
        // `write` may sleep, held under `file_lock` exactly as the C
        // original does (P3-7b keeps trait dispatch raw).
        return unsafe { ops.write(&raw mut *self, buf as *const c_char, n, user != 0) };
    }

    // SAFETY: `inode` is non-null (checked above), live while `self` is
    // open — a shared read of its `mode`.
    let inode_mode = unsafe { (*inode).mode };

    // Character device write: same no-file-lock-across-I/O rationale as
    // `vfs_fileread`.
    if is_chr(inode_mode) {
        let _g = self.file_lock().lock();
        if self.f_flags & O_ACCMODE == O_RDONLY {
            return Err(Errno::BadF);
        }
        return if let Some(ops) = self.ops {
            // SAFETY: see the pipe/socket write call site above.
            unsafe { ops.write(&raw mut *self, buf as *const c_char, n, user != 0) }
        } else {
            // SAFETY: `pos.cdev` is the active union member of a cdev file.
            let cdev = unsafe { self.pos.cdev };
            Ok(cdev_write(cdev, user as bool_, buf, n) as isize)
        };
    }

    let _g = self.file_lock().lock();
    if self.f_flags & O_ACCMODE == O_RDONLY {
        return Err(Errno::BadF); // File not opened for writing.
    }

    // Block device write -- not directly supported, use the buffer cache.
    if is_blk(inode_mode) {
        return Err(Errno::OpNotSupp);
    }

    // Regular files. We do NOT lock the inode here -- the driver
    // callback (e.g. xv6fs_file_write) acquires transactions and the
    // inode lock in the correct order internally; see the C original's
    // comment (transaction -> inode) for the deadlock this avoids. The
    // file lock still protects the file position and serializes
    // concurrent writes.
    if !is_reg(inode_mode) {
        return Err(if is_dir(inode_mode) { Errno::IsDir } else { Errno::Inval });
    }
    let Some(ops) = self.ops else {
        return Err(Errno::OpNotSupp);
    };
    // The driver handles file extension, size updates, and truncation
    // internally. A driver error propagates as `Err` (and, exactly like
    // the old negative-`isize` return, skips the position bump).
    // SAFETY: see the pipe/socket write call site above.
    let ret = unsafe { ops.write(&raw mut *self, buf as *const c_char, n, user != 0) }?;
    if ret > 0 {
        // SAFETY: `pos.f_pos` is the active union member of a regular file.
        unsafe { self.pos.f_pos += ret as loff_t };
    }
    Ok(ret)
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `vfs_filewrite`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_filewrite(
    file: *mut vfs_file,
    buf: *const c_void,
    n: usize,
    user: c_int,
) -> isize {
    if file.is_null() {
        return Errno::BadF.neg() as isize; // Same errno the old null check produced.
    }
    // SAFETY: non-null (just checked); live open file. P3-7b boundary.
    match (unsafe { &mut *file }).write_inner(buf, n, user) {
        Ok(v) => v,
        Err(e) => e.neg() as isize,
    }
}

/// Core logic behind [`vfs_filelseek`], factored out as a private method
/// returning [`KResult`] (P3-CS6). Only `EBADF`/`EINVAL`/`ESPIPE` are
/// this method's own failures; the driver-callback (`ops.llseek`)
/// result passes through as `Ok` unconditionally, matching the
/// original's unconditional `return ret`. N-METH: inherent method (was
/// the free fn `vfs_filelseek_inner`).
fn lseek_inner(&mut self, offset: loff_t, whence: c_int) -> KResult<loff_t> {
    // P3-7b: `self` is a reference — `inode`/`ops` are safe field access;
    // the `pos` union and the raw-`self` trait dispatch stay `unsafe`.
    let inode = FsStruct::vfs_inode_deref(&raw mut self.inode);
    if inode.is_null() {
        return Err(Errno::Inval);
    }

    // lseek only applies to regular files.
    // SAFETY: non-null `inode` (checked above).
    if !is_reg(unsafe { (*inode).mode }) {
        return Err(Errno::SPipe); // Illegal seek.
    }
    let _g = self.file_lock().lock();
    // Note: we do NOT lock the inode here -- the driver callback (e.g.
    // xv6fs_file_llseek) is responsible for acquiring it when needed
    // (e.g. for SEEK_END), matching the read/write design above.
    // (The trait's default `llseek` is `Err(Errno::SPipe)`, reproducing
    // the old `None`-slot "not seekable" outcome; a missing table takes
    // the same arm here.)
    let Some(ops) = self.ops else {
        return Err(Errno::SPipe); // Not seekable.
    };
    // SAFETY: `&raw mut *self` reborrows the live reference as the raw
    // pointer `llseek` still takes (P3-7b keeps trait dispatch raw). A
    // driver error propagates as `Err` (skipping the position store,
    // exactly like the old negative-`loff_t` return).
    let ret = unsafe { ops.llseek(&raw mut *self, offset, whence) }?;
    if ret >= 0 {
        // SAFETY: `pos.f_pos` is the active union member of a regular file.
        unsafe { self.pos.f_pos = ret };
    }
    Ok(ret)
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `vfs_filelseek`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_filelseek(file: *mut vfs_file, offset: loff_t, whence: c_int) -> loff_t {
    if file.is_null() {
        return Errno::BadF.neg() as loff_t; // Same errno the old null check produced.
    }
    // SAFETY: non-null (just checked); live open file. P3-7b boundary.
    match (unsafe { &mut *file }).lseek_inner(offset, whence) {
        Ok(v) => v,
        Err(e) => e.neg() as loff_t,
    }
}

/// Core logic behind [`truncate`], factored out as a private method
/// returning [`KResult`] (P3-CS6). Only `EBADF`/`EISDIR`/`EINVAL` are
/// this method's own failures; `vfs_itruncate`'s result passes through
/// as `Ok` unconditionally (it may itself already be a negative errno),
/// matching the original's unconditional `return`. N-METH: inherent
/// method (was the free fn `truncate_inner`).
fn truncate_inner(&mut self, length: loff_t) -> KResult<c_int> {
    // P3-7b: `self` is a reference — `inode` is safe field access.
    let inode = FsStruct::vfs_inode_deref(&raw mut self.inode);
    if inode.is_null() {
        return Err(Errno::Inval);
    }

    // SAFETY: non-null `inode` (checked above).
    let mode = unsafe { (*inode).mode };
    if !is_reg(mode) {
        return Err(if is_dir(mode) { Errno::IsDir } else { Errno::Inval });
    }
    if length < 0 {
        return Err(Errno::Inval);
    }

    let _g = self.file_lock().lock();
    // `vfs_ilock` is acquired inside `vfs_itruncate`.
    Ok(VfsInode::vfs_itruncate(inode, length))
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `truncate`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn truncate(file: *mut vfs_file, length: loff_t) -> c_int {
    if file.is_null() {
        return Errno::BadF.neg(); // Same errno the old null check produced.
    }
    // SAFETY: non-null (just checked); live open file. P3-7b boundary.
    match (unsafe { &mut *file }).truncate_inner(length) {
        Ok(v) => v,
        Err(e) => e.neg(),
    }
}
}

// ===========================================================================
// VFS pipe allocation.
// ===========================================================================

impl VfsFile {
/// Core logic behind [`vfs_pipealloc`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). The `pipe_alloc` `ERR_PTR` failure is a
/// cross-module boundary this cluster doesn't own -- stays
/// `Err(Errno::Raw(..))`, same precedent as `open_cdev`. N-METH: inherent
/// assoc fn (was the free fn `vfs_pipealloc_inner`).
fn vfs_pipealloc_inner(rf: *mut *mut vfs_file, wf: *mut *mut vfs_file) -> KResult<()> {
    // SAFETY: `rf`/`wf` are caller-owned out-params (every caller in the
    // tree passes stack-local `struct vfs_file *` addresses).
    unsafe {
        *rf = ptr::null_mut();
        *wf = ptr::null_mut();
    }

    let read_file = VfsFile::file_alloc();
    if read_file.is_null() {
        return Err(Errno::NoMem);
    }

    let write_file = VfsFile::file_alloc();
    if write_file.is_null() {
        VfsFile::file_free(read_file);
        return Err(Errno::NoMem);
    }

    let pi = Pipe::pipe_alloc(0);
    if is_err(pi) {
        VfsFile::file_free(read_file);
        VfsFile::file_free(write_file);
        return Err(Errno::Raw(ptr_err(pi)));
    }

    Pipe::pipe_open(read_file, pi, O_RDONLY);
    VfsFile::ftable_attach(read_file);

    Pipe::pipe_open(write_file, pi, O_WRONLY);
    VfsFile::ftable_attach(write_file);

    // SAFETY: see above.
    unsafe {
        *rf = read_file;
        *wf = write_file;
    }
    Ok(())
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `vfs_pipealloc`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_pipealloc(rf: *mut *mut vfs_file, wf: *mut *mut vfs_file) -> c_int {
    result_to_neg_errno(VfsFile::vfs_pipealloc_inner(rf, wf))
}
}

// ===========================================================================
// VFS socket allocation.
// ===========================================================================

/// Socket structure -- a local mirror of `struct sock` from
/// `kernel/sysnet.c` (still C). See the module doc for why this
/// duplication is faithful to the C original rather than a new hazard.
#[repr(C)]
struct sock {
    next: *mut sock,
    raddr: u32,
    lport: u16,
    rport: u16,
    lock: spinlock_t,
    rxq: mbufq,
}

/// Mirrors `struct mbufq` (`kernel/inc/dev/net.h`) -- only used here as
/// an opaque-but-correctly-sized/aligned field of the local `sock`
/// mirror above; its contents are entirely owned and manipulated by
/// `mbufq_init` (still C).
#[repr(C)]
struct mbufq {
    head: *mut c_void,
    tail: *mut c_void,
}

// P3-10a: `vfs_custom_fd_alloc`/`vfs_custom_fd_alloc_inner` DELETED.
// The pair ("allocate a vfs_file with caller-supplied ops table +
// private_data, install into the current fd table") was a C-facing
// convenience API with ZERO callers anywhere in the tree (Rust drivers
// install their ops directly in their own open paths; the
// `inc/vfs/file.h` prototype is vestigial — no C remains). Its `ops:
// *mut vfs_file_ops` parameter was this family's last table-by-pointer
// C-compatible interface; rather than carry a dead fn through the trait
// conversion (`extern "C"` + a fat `Option<&'static dyn FileOps>` arg
// would also trip `improper_ctypes_definitions`), it is removed per the
// wave directive. Trivially reintroducible as
// `fn(Option<&'static dyn FileOps>, *mut c_void, c_int) -> KResult<c_int>`
// when a real caller (e.g. a future sysnet fd path) appears.

impl VfsFile {
/// Core logic behind [`vfs_sockalloc`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). The duplicate-tuple check is this
/// function's only "real" runtime failure mode -- `Err(Errno::AddrInUse)`.
/// Note the exact original control flow is preserved on that path: `f`
/// was already `ftable_attach`ed before the duplicate check runs, and
/// the original never called the matching `ftable_detach`/`vfs_fput`
/// before `file_free`ing it (a pre-existing quirk, not introduced by
/// this conversion -- kept byte-for-byte since behavior must stay
/// identical). N-METH: inherent assoc fn (was the free fn
/// `vfs_sockalloc_inner`).
fn vfs_sockalloc_inner(
    out: *mut *mut vfs_file,
    raddr: u32,
    lport: u16,
    rport: u16,
) -> KResult<()> {
    // SAFETY: caller-owned out-param.
    unsafe { *out = ptr::null_mut() };

    let f = VfsFile::file_alloc();
    if f.is_null() {
        return Err(Errno::NoMem);
    }

    let si = kalloc() as *mut sock;
    if si.is_null() {
        VfsFile::file_free(f);
        return Err(Errno::NoMem);
    }

    // SAFETY: `si` is a fresh, exclusively-owned `sock`-sized allocation
    // (`kalloc()` returns a full page, `sizeof(struct sock)` fits).
    unsafe {
        (*si).raddr = raddr;
        (*si).lport = lport;
        (*si).rport = rport;
        spin_init(ptr::addr_of_mut!((*si).lock), c"sock".as_ptr() as *mut c_char);
        // `mbufq_init` (`net.rs`) takes the real `crate::net::mbufq`; this
        // file's `mbufq` is a byte-layout-identical local mirror (see this
        // struct's own doc comment) -- pointer cast, same precedent as the
        // `sock`/`crate::bindings::sock` handoff a few lines below.
        mbufq_init(ptr::addr_of_mut!((*si).rxq) as *mut crate::net::mbufq);
    }

    // SAFETY: non-null `f`.
    unsafe {
        (*f).f_flags = O_RDWR;
        (*f).pos.sock = si as *mut crate::bindings::sock;
        (*f).ops = None; // Sockets use direct socket I/O.
    }
    VfsFile::ftable_attach(f);

    // Add to the list of sockets (checking for duplicates).
    let dup: bool = {
        // SAFETY: `SOCKETS` (`sysnet.rs`) is a live `SpinLock<*mut
        // crate::sysnet::sock>`; this file's local `sock` (above) is a
        // byte-layout-identical mirror of that type (see the module
        // doc), so casting the guard's pointer to/from this file's
        // `sock` type is a plain reinterpretation of the same memory,
        // not a type-confusion hazard -- same precedent as the `sock`/
        // `crate::bindings::sock` handoff a few lines above.
        let mut guard = SOCKETS.lock();
        let head = *guard as *mut sock;
        // N-METH goal #2: the manual `next`-pointer walk over the global
        // socket list is now a `core::iter::successors` chain terminated
        // by `.any(..)` (behavior-identical -- a read-only scan for a
        // matching tuple, under `SOCKETS.lock()` held for the whole
        // block; the `sock` chain is a bespoke `next` field, not a
        // `list_node_t`, so `list_for_each!` does not apply).
        let dup = core::iter::successors(
            (!head.is_null()).then_some(head),
            // SAFETY: `p` is a live node on the `sockets` list (held
            // under the lock); reading `next` advances to the successor.
            |&p| {
                let n = unsafe { (*p).next };
                (!n.is_null()).then_some(n)
            },
        )
        .any(|p| {
            // SAFETY: `p` is a live node on the `sockets` list.
            let (praddr, plport, prport) = unsafe { ((*p).raddr, (*p).lport, (*p).rport) };
            praddr == raddr && plport == lport && prport == rport
        });
        if !dup {
            // SAFETY: `si` is a freshly initialized, exclusively-owned node.
            unsafe {
                (*si).next = *guard as *mut sock;
            }
            *guard = si as *mut crate::sysnet::sock;
        }
        dup
    };

    if dup {
        kfree(si as *mut c_void);
        VfsFile::file_free(f);
        return Err(Errno::AddrInUse);
    }

    // SAFETY: caller-owned out-param.
    unsafe { *out = f };
    Ok(())
}

/// N-METH: inherent assoc fn (was the free `extern "C"` fn `vfs_sockalloc`);
/// name kept verbatim (NAMESPACING).
pub(crate) extern "C" fn vfs_sockalloc(
    out: *mut *mut vfs_file,
    raddr: u32,
    lport: u16,
    rport: u16,
) -> c_int {
    result_to_neg_errno(VfsFile::vfs_sockalloc_inner(out, raddr, lport, rport))
}
}
