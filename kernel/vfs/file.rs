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
//! global open-file table (`__vfs_ftable_lock`, a `spinlock_t`) and the
//! per-process `fdtable.lock` (`vfs_custom_fd_alloc`) are likewise
//! single lexically-scoped critical sections and use
//! [`crate::sync::KSpinlock`] RAII. [`crate::sysnet::SOCKETS`] (owned by
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
    spinlock_t, stat, thread, vfs_fdtable, vfs_file, vfs_file_ops, vfs_inode, vfs_inode_ref,
    EAGAIN, SLAB_FLAG_DEBUG_BITMAP, SLAB_FLAG_STATIC,
};
use crate::proc::proc_shims::xv6_current_thread;
use crate::sync::{KMutex, KSpinlock};

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally per this crate's established convention (see `vfs/inode.rs`'s
// externs-block doc). Nearly everything is `safe`: passing a raw
// pointer is never itself unsafe, and every entry point here either
// null-checks internally or is called only with pointers this file
// already proved non-null. `printf` (C-variadic) is the one exception.
// ===========================================================================

unsafe extern "C" {
    // printf.rs — C-variadic.

    // lock/spinlock.rs — `__vfs_ftable_lock` init only (lock/unlock go
    // through `crate::sync::KSpinlock` RAII, see the module doc).
    safe fn spin_init(l: *mut spinlock_t, name: *mut c_char);

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
    unsafe { crate::mm::kalloc() }
}
/// SAFETY: `pa` must originate from `kalloc` above.
#[inline]
fn kfree(pa: *mut c_void) {
    unsafe { crate::mm::kfree(pa) };
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
use crate::vfs::fdtable::vfs_fdtable_alloc_fd;
use crate::vfs::fs::{vfs_inode_deref, vfs_inode_get_ref, vfs_inode_put_ref};
use crate::vfs::inode::{vfs_ilock, vfs_itruncate, vfs_iunlock};
use crate::vfs::pipe::{pipe_alloc, pipe_close, pipe_open};

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

#[inline(always)]
fn refcount_atomic<'a>(file: *mut vfs_file) -> &'a AtomicI32 {
    // SAFETY: `ref_count` is a plain C `int` field, same size/align as
    // `AtomicI32`; caller ensures `file` is a live, aligned `vfs_file`.
    unsafe { &*(ptr::addr_of_mut!((*file).ref_count) as *const AtomicI32) }
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

static mut __VFS_FTABLE_LOCK: spinlock_t = spinlock_t {
    locked: 0,
    name: core::ptr::null_mut(),
    cpu: core::ptr::null_mut(),
};
static mut __VFS_FTABLE: list_node_t =
    list_node_t { prev: core::ptr::null_mut(), next: core::ptr::null_mut() };
static __VFS_OPEN_FILE_COUNT: AtomicI32 = AtomicI32::new(0);

fn ftable_lock() -> KSpinlock {
    // SAFETY: `__VFS_FTABLE_LOCK` is a `'static` mut whose address is
    // stable for the process lifetime; only its address is taken here.
    KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!(__VFS_FTABLE_LOCK) })
}

fn file_lock(file: *mut vfs_file) -> KMutex {
    // SAFETY: `file` is a live, aligned `vfs_file` (every call site's
    // precondition below); `lock` is a plain embedded `mutex_t` field.
    KMutex::from_ptr(unsafe { ptr::addr_of_mut!((*file).lock) })
}

fn ftable_attach(file: *mut vfs_file) {
    let _g = ftable_lock().lock();
    // SAFETY: `__VFS_FTABLE` is the live global open-file list head
    // (initialized once by `__vfs_file_init` before any attach/detach
    // can race in); `file->list_entry` is not yet linked anywhere
    // (every caller passes a freshly allocated file).
    unsafe {
        ln_push_back(ptr::addr_of_mut!(__VFS_FTABLE), ptr::addr_of_mut!((*file).list_entry));
    }
    let count = __VFS_OPEN_FILE_COUNT.fetch_add(1, Ordering::SeqCst) + 1;
    drop(_g);
    kassert!(count > 0, "vfs file open count overflow");
}

fn ftable_detach(file: *mut vfs_file) {
    let _g = ftable_lock().lock();
    // SAFETY: `file->list_entry` is linked into `__VFS_FTABLE` (every
    // caller passes a file that went through `ftable_attach`).
    unsafe { ln_detach(ptr::addr_of_mut!((*file).list_entry)) };
    let count = __VFS_OPEN_FILE_COUNT.fetch_sub(1, Ordering::SeqCst) - 1;
    drop(_g);
    kassert!(count >= 0, "vfs file open count underflow");
}

fn file_alloc() -> *mut vfs_file {
    let file = slab_alloc(vfs_file_slab()) as *mut vfs_file;
    if file.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `slab_alloc` returned a fresh, exclusively-owned
    // `size_of::<vfs_file>()` allocation; zeroing then field-initializing
    // it mirrors the C original's `memset` + explicit-field-init.
    unsafe {
        ptr::write_bytes(file, 0, 1);
        mutex_init(ptr::addr_of_mut!((*file).lock), c"vfs_file_lock".as_ptr() as *mut c_char);
        (*file).ref_count = 1;
    }
    file
}

fn file_free(file: *mut vfs_file) {
    if file.is_null() {
        return;
    }
    // SAFETY: `file` is a live, exclusively-owned `vfs_file` about to be
    // returned to the slab (every caller holds the last reference).
    unsafe {
        let ops = (*file).ops;
        if let Some(release) = ops.as_ref().and_then(|o| o.release) {
            let ret = release((*file).inode.inode, file);
            if ret != 0 {
                crate::kprintln!("__vfs_file_free: file ops release failed, errno={}", ret);
            }
        }
    }
    slab_free(file as *mut c_void);
}

pub(crate) extern "C" fn __vfs_file_init() {
    let ret = slab_cache_init(
        vfs_file_slab(),
        c"vfs_file_cache".as_ptr() as *mut c_char,
        core::mem::size_of::<vfs_file>(),
        (SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP) as u64,
    );
    kassert!(ret == 0, "Failed to initialize vfs_file_cache slab cache");
    // SAFETY: called exactly once, at boot, before any other thread can
    // observe `__VFS_FTABLE_LOCK`/`__VFS_FTABLE` (matches the C
    // original's single `vfs_init()` call site).
    unsafe {
        spin_init(
            ptr::addr_of_mut!(__VFS_FTABLE_LOCK),
            c"vfs_file_table_lock".as_ptr() as *mut c_char,
        );
        let head = ptr::addr_of_mut!(__VFS_FTABLE);
        (*head).prev = head;
        (*head).next = head;
    }
    __VFS_OPEN_FILE_COUNT.store(0, Ordering::SeqCst);
}

pub(crate) extern "C" fn __vfs_file_shrink_cache() {
    slab_cache_shrink(vfs_file_slab(), 0x7fffffff);
}

// ===========================================================================
// Open-path helpers: character/block device files.
// ===========================================================================

/// Open a character device file. Internal helper (P3-CS6: returns
/// [`KResult`] — its sole caller, [`vfs_fileopen_inner`], is itself
/// `KResult`-returning, so the former `c_int` error encoding just added
/// an extra decode step at the one call site).
fn open_cdev(inode: *mut vfs_inode, file: *mut vfs_file) -> KResult<()> {
    // SAFETY: non-null `inode` (every call site checks `S_ISCHR` first,
    // which already dereferenced `inode->mode`).
    let raw_cdev = unsafe { (*inode).__bindgen_anon_2.cdev };
    let major = ((raw_cdev >> 20) & 0xFFF) as c_int;
    let minor = (raw_cdev & 0xFFFFF) as c_int;
    let cdev = cdev_get(major, minor);
    if is_err(cdev) {
        // Cross-module `ERR_PTR` this cluster doesn't own (`dev/cdev.rs`)
        // -- passed through verbatim, same precedent as
        // `vfs_syscall.rs`'s `Errno::Raw` sites.
        return Err(Errno::Raw(ptr_err(cdev)));
    }
    if cdev.is_null() {
        return Err(Errno::NoDev);
    }

    // SAFETY: `cdev` is non-null and non-error (checked above); reading
    // `ops.open_file` is a plain field access.
    let open_file = unsafe { (*cdev).ops.open_file };
    if let Some(open_file_fn) = open_file {
        // SAFETY: `cdev`/`file` are both live; `open_file` is a
        // filesystem/driver callback with the documented contract that
        // it may install `file->ops`/`file->private_data`.
        let ret = unsafe { open_file_fn(cdev, file) };
        if ret != 0 {
            cdev_put(cdev);
            return Err(Errno::Raw(ret));
        }
        // SAFETY: non-null `file`.
        if unsafe { !(*file).ops.is_null() } {
            // `open_file` took over: the VFS does not also store
            // `file->cdev` (the cdev manages its own refcount in that
            // case), matching the C original's comment exactly.
            cdev_put(cdev);
            return Ok(());
        }
    }

    // SAFETY: non-null `file`.
    unsafe {
        (*file).__bindgen_anon_1.cdev = cdev;
        (*file).ops = ptr::null_mut(); // Device files use direct device I/O.
    }
    Ok(())
}

/// Open a block device file. Always succeeds: if the device can't be
/// found, the file is left with a null `blkdev`/`ops` so it can still be
/// `stat`ed but not used for I/O (matches the C original's "device not
/// found -- allow open for stat but not I/O" comment). P3-CS6: dropped
/// the always-`0` `c_int` return the C ABI encoding required — this
/// helper's only caller ([`vfs_fileopen_inner`]) never observed a
/// non-zero result (verified by inspection: every path below ends in
/// `return`, none in a nonzero value), so the signature change is a
/// behavior-preserving simplification, not a semantic one.
fn open_blkdev(inode: *mut vfs_inode, file: *mut vfs_file) {
    // SAFETY: non-null `inode`.
    let raw_bdev = unsafe { (*inode).__bindgen_anon_2.bdev };
    let major = ((raw_bdev >> 20) & 0xFFF) as c_int;
    let minor = (raw_bdev & 0xFFFFF) as c_int;
    let blkdev = blkdev_get(major, minor);
    // SAFETY: non-null `file`.
    unsafe {
        if is_err(blkdev) || blkdev.is_null() {
            // Device not found -- allow open for stat but not I/O.
            (*file).__bindgen_anon_1.blkdev = ptr::null_mut();
            (*file).ops = ptr::null_mut();
            return;
        }
        (*file).__bindgen_anon_1.blkdev = blkdev;
        (*file).ops = ptr::null_mut(); // Device files use direct device I/O.
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

    vfs_ilock(inode);
    // @TODO: check permission
    let file = file_alloc();
    if file.is_null() {
        vfs_iunlock(inode);
        return Err(Errno::NoMem);
    }
    // SAFETY: non-null `file`.
    let ret = vfs_inode_get_ref(inode, unsafe { ptr::addr_of_mut!((*file).inode) });
    if ret != 0 {
        file_free(file);
        vfs_iunlock(inode);
        return Err(Errno::Raw(ret));
    }

    if is_chr(mode) {
        if let Err(e) = open_cdev(inode, file) {
            vfs_iunlock(inode);
            // SAFETY: non-null `file`.
            vfs_inode_put_ref(unsafe { ptr::addr_of_mut!((*file).inode) });
            file_free(file);
            return Err(e);
        }
        vfs_iunlock(inode);
        ftable_attach(file);
        unsafe { (*file).f_flags = f_flags };
        return Ok(file);
    }

    if is_blk(mode) {
        open_blkdev(inode, file); // Infallible -- see its own doc comment.
        vfs_iunlock(inode);
        ftable_attach(file);
        unsafe { (*file).f_flags = f_flags };
        return Ok(file);
    }

    // Regular files and directories use inode->ops->open.
    // SAFETY: non-null `inode`.
    let open_fn = unsafe { (*(*inode).ops).open };
    let Some(open_fn) = open_fn else {
        vfs_iunlock(inode);
        vfs_inode_put_ref(unsafe { ptr::addr_of_mut!((*file).inode) });
        file_free(file);
        return Err(Errno::NoSys);
    };
    // SAFETY: `inode`/`file` are both live; `open` is a filesystem
    // driver callback invoked with the inode lock held, matching its
    // documented contract.
    let ret = unsafe { open_fn(inode, file, f_flags) };
    if ret != 0 {
        vfs_iunlock(inode);
        vfs_inode_put_ref(unsafe { ptr::addr_of_mut!((*file).inode) });
        file_free(file);
        return Err(Errno::Raw(ret));
    }
    // SAFETY: non-null `file`.
    if unsafe { (*file).ops.is_null() } {
        vfs_iunlock(inode);
        vfs_inode_put_ref(unsafe { ptr::addr_of_mut!((*file).inode) });
        file_free(file);
        crate::kprintln!("vfs_fileopen: file operations not set by inode open");
        return Err(Errno::Inval);
    }

    vfs_iunlock(inode);
    ftable_attach(file);
    // SAFETY: non-null `file`.
    unsafe {
        (*file).f_flags = f_flags;
        (*file).__bindgen_anon_1.f_pos = 0;
    }
    Ok(file)
}

pub(crate) extern "C" fn vfs_fileopen(inode: *mut vfs_inode, f_flags: c_int) -> *mut vfs_file {
    result_to_errptr(vfs_fileopen_inner(inode, f_flags))
}

/// Release a file reference. Decrements the file's reference count; on
/// last reference, detaches from the global table, flushes/closes the
/// backing object (cdev/blkdev/pipe), drops the inode reference, and
/// frees the `vfs_file`.
pub(crate) extern "C" fn vfs_fput(file: *mut vfs_file) {
    if file.is_null() {
        return;
    }
    if atomic_dec_unless(refcount_atomic(file), 1) {
        return;
    }
    // File descriptors are shared through dup, thus when refcount
    // reaches 1, no other threads will be using it. No need to lock the
    // file structure.
    ftable_detach(file);

    // SAFETY: non-null `file`.
    let inode = vfs_inode_deref(unsafe { ptr::addr_of_mut!((*file).inode) });

    // Flush dirty pages before releasing the inode reference, so all
    // data is written to disk before the inode can be torn down (avoids
    // the flush worker racing with inode destruction).
    // SAFETY: non-null `file`.
    let ops = unsafe { (*file).ops };
    if let Some(fflush) = unsafe { ops.as_ref() }.and_then(|o| o.fflush) {
        // SAFETY: `file` is live; `fflush` is a filesystem driver
        // callback with the documented "flush dirty data" contract.
        let ret = unsafe { fflush(file) };
        if ret != 0 && ret != neg(EAGAIN) {
            crate::kprintln!("vfs_fput: fflush failed: {}", ret);
        }
    }

    // Handle special file cleanup. Anonymous-pipe cleanup (pipe != NULL,
    // inode == NULL) is handled by `pipe_file_ops.release` via
    // `file_free` below, not here.
    if !inode.is_null() {
        // SAFETY: non-null `inode`.
        let mode = unsafe { (*inode).mode };
        if is_chr(mode) {
            // SAFETY: non-null `file`.
            let cdev = unsafe { (*file).__bindgen_anon_1.cdev };
            if !cdev.is_null() {
                let ret = cdev_put(cdev);
                unsafe { (*file).__bindgen_anon_1.cdev = ptr::null_mut() };
                if ret != 0 {
                    crate::kprintln!("vfs_fput: cdev_put failed: {}", ret);
                }
            }
        } else if is_blk(mode) {
            // SAFETY: non-null `file`.
            let ret = unsafe { blkdev_put((*file).__bindgen_anon_1.blkdev) };
            unsafe { (*file).__bindgen_anon_1.blkdev = ptr::null_mut() };
            if ret != 0 {
                crate::kprintln!("vfs_fput: blkdev_put failed: {}", ret);
            }
        } else if is_fifo(mode) {
            // SAFETY: non-null `file`.
            let pi = unsafe { (*file).__bindgen_anon_1.pipe };
            if !pi.is_null() {
                // SAFETY: non-null `file`.
                let writable = (unsafe { (*file).f_flags } & O_ACCMODE) != O_RDONLY;
                pipe_close(pi, writable as c_int);
            }
        }
        // Sockets are not opened via inodes, so no cleanup here.
    }

    // SAFETY: non-null `file`.
    vfs_inode_put_ref(unsafe { ptr::addr_of_mut!((*file).inode) });
    file_free(file);
}

/// Duplicate a file reference (increment refcount). Returns `NULL` if
/// `file` was already closed (or `NULL`).
pub(crate) extern "C" fn vfs_fdup(file: *mut vfs_file) -> *mut vfs_file {
    if file.is_null() {
        return ptr::null_mut();
    }
    if !atomic_inc_unless(refcount_atomic(file), 0) {
        return ptr::null_mut(); // File was already closed.
    }
    file
}

/// Core logic behind [`vfs_ioctl`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). Only `EBADF`/`ENODEV`/`ENOTTY` are
/// this function's own failures; the driver-callback (`ops.ioctl`) and
/// `dev_ioctl` results pass through as `Ok` unconditionally (they may
/// themselves already be a negative `E*` value), matching the original's
/// unconditional early `return`s.
fn vfs_ioctl_inner(file: *mut vfs_file, cmd: u64, arg: *mut c_void) -> KResult<c_int> {
    if file.is_null() {
        return Err(Errno::BadF);
    }

    // Fast path: character / block device files -- dispatch to the
    // device layer.
    // SAFETY: non-null `file`.
    let inode = vfs_inode_deref(unsafe { ptr::addr_of_mut!((*file).inode) });
    if !inode.is_null() {
        // SAFETY: non-null `inode`.
        let mode = unsafe { (*inode).mode };
        if is_blk(mode) || is_chr(mode) {
            // Prefer custom file ops (installed by cdev open_file, e.g.
            // PTY master).
            // SAFETY: non-null `file`.
            let ops = unsafe { (*file).ops };
            if let Some(ioctl) = unsafe { ops.as_ref() }.and_then(|o| o.ioctl) {
                // SAFETY: `file` is live; `ioctl` is a driver callback.
                return Ok(unsafe { ioctl(file, cmd, arg) });
            }
            // SAFETY: non-null `file`; the cdev pointer reinterprets as
            // `device_t*` because `device_t dev` is `cdev_t`'s first
            // field (repr(C), matches the C original's cast exactly).
            let dev = unsafe { (*file).__bindgen_anon_1.cdev as *mut device_t };
            if !dev.is_null() {
                return Ok(dev_ioctl(dev, cmd, arg));
            }
            return Err(Errno::NoDev);
        }
    }

    // Fallback: custom file descriptors (pipes, sockets, etc.).
    // SAFETY: non-null `file`.
    let ops = unsafe { (*file).ops };
    if let Some(ioctl) = unsafe { ops.as_ref() }.and_then(|o| o.ioctl) {
        // SAFETY: `file` is live; `ioctl` is a driver callback.
        return Ok(unsafe { ioctl(file, cmd, arg) });
    }

    Err(Errno::NotTy)
}

pub(crate) extern "C" fn vfs_ioctl(file: *mut vfs_file, cmd: u64, arg: *mut c_void) -> c_int {
    match vfs_ioctl_inner(file, cmd, arg) {
        Ok(v) => v,
        Err(e) => e.neg(),
    }
}

/// Core logic behind [`vfs_fileread`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). Every own-failure early return
/// (`EBADF`/`EOPNOTSUPP`/`EISDIR`/`EINVAL`) is now the matching `Errno`
/// variant; the driver-callback (`ops.read`) and `cdev_read` results
/// pass through as `Ok` unconditionally (their `isize` may itself
/// already be a negative errno), matching the original's unconditional
/// `return`s of those values.
fn vfs_fileread_inner(file: *mut vfs_file, buf: *mut c_void, n: usize, user: c_int) -> KResult<isize> {
    if file.is_null() {
        return Err(Errno::BadF);
    }
    if buf.is_null() {
        return Err(Errno::Fault);
    }
    if n == 0 {
        return Ok(0); // POSIX: zero-length read succeeds.
    }

    // SAFETY: non-null `file`.
    let inode = vfs_inode_deref(unsafe { ptr::addr_of_mut!((*file).inode) });

    // Handle pipe/socket read -- these don't have inodes.
    if inode.is_null() {
        // SAFETY: non-null `file`.
        let ops = unsafe { (*file).ops };
        let Some(read) = unsafe { ops.as_ref() }.and_then(|o| o.read) else {
            return Err(Errno::BadF); // Not a readable file object.
        };
        let _g = file_lock(file).lock();
        // SAFETY: non-null `file`.
        if unsafe { (*file).f_flags } & O_ACCMODE == O_WRONLY {
            return Err(Errno::BadF); // File not opened for reading.
        }
        // SAFETY: `file`/`buf` are live for the call; `read` is a
        // filesystem driver callback that may sleep, matching its
        // documented contract -- held under `file_lock` exactly as the
        // C original does for this path.
        return Ok(unsafe { read(file, buf as *mut c_char, n, user as bool_) });
    }

    // Character device read: check access and call without holding the
    // file lock across the actual I/O -- cdev operations may sleep and
    // handle their own synchronization, matching the C original.
    // SAFETY: non-null `inode`.
    if is_chr(unsafe { (*inode).mode }) {
        let _g = file_lock(file).lock();
        // SAFETY: non-null `file`.
        if unsafe { (*file).f_flags } & O_ACCMODE == O_WRONLY {
            return Err(Errno::BadF);
        }
        // SAFETY: non-null `file`.
        let ops = unsafe { (*file).ops };
        let ret = if let Some(read) = unsafe { ops.as_ref() }.and_then(|o| o.read) {
            // Custom file ops installed by cdev open_file (e.g. PTY master).
            // SAFETY: see the pipe/socket read call site above.
            unsafe { read(file, buf as *mut c_char, n, user as bool_) }
        } else {
            // SAFETY: non-null `file`.
            let cdev = unsafe { (*file).__bindgen_anon_1.cdev };
            cdev_read(cdev, user as bool_, buf, n) as isize
        };
        return Ok(ret);
    }

    let _g = file_lock(file).lock();
    // SAFETY: non-null `file`.
    if unsafe { (*file).f_flags } & O_ACCMODE == O_WRONLY {
        return Err(Errno::BadF); // File not opened for reading.
    }

    // Block device read -- not directly supported, use the buffer cache.
    // SAFETY: non-null `inode`.
    if is_blk(unsafe { (*inode).mode }) {
        return Err(Errno::OpNotSupp);
    }

    // Regular files.
    // Note: we do NOT lock the inode here. The driver callback (e.g.
    // xv6fs_file_read) is responsible for acquiring the inode lock to
    // read size and data. This avoids lock-ordering issues where the
    // driver may need to acquire transactions that conflict with VFS
    // locking order (transaction -> superblock -> inode).
    // SAFETY: non-null `inode`.
    let mode = unsafe { (*inode).mode };
    if !is_reg(mode) {
        return Err(if is_dir(mode) { Errno::IsDir } else { Errno::Inval });
    }
    // SAFETY: non-null `file`.
    let ops = unsafe { (*file).ops };
    let Some(read) = unsafe { ops.as_ref() }.and_then(|o| o.read) else {
        return Err(Errno::OpNotSupp);
    };
    // Pass the requested size to the driver; the driver handles EOF and
    // size checks internally.
    // SAFETY: see the pipe/socket read call site above.
    let ret = unsafe { read(file, buf as *mut c_char, n, user as bool_) };
    if ret > 0 {
        // SAFETY: non-null `file`.
        unsafe { (*file).__bindgen_anon_1.f_pos += ret as loff_t };
    }
    Ok(ret)
}

pub(crate) extern "C" fn vfs_fileread(file: *mut vfs_file, buf: *mut c_void, n: usize, user: c_int) -> isize {
    match vfs_fileread_inner(file, buf, n, user) {
        Ok(v) => v,
        Err(e) => e.neg() as isize,
    }
}

/// Core logic behind [`vfs_filestat`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). Only `EBADF`/`EFAULT` are this
/// function's own failures; a filesystem-supplied `getattr`'s result
/// passes through as `Ok` unconditionally (it may itself already be a
/// negative errno), matching the original's unconditional `return`.
fn vfs_filestat_inner(file: *mut vfs_file, out: *mut stat) -> KResult<c_int> {
    if file.is_null() {
        return Err(Errno::BadF);
    }
    if out.is_null() {
        return Err(Errno::Fault);
    }

    // SAFETY: non-null `file`.
    let inode = vfs_inode_deref(unsafe { ptr::addr_of_mut!((*file).inode) });
    if inode.is_null() {
        // Custom file descriptors (PTY slaves, etc.) have no backing
        // inode. Return a synthetic stat indicating a character device
        // so that isatty() works correctly.
        // SAFETY: non-null `file`/`out`.
        if unsafe { !(*file).ops.is_null() } {
            unsafe {
                ptr::write_bytes(out, 0, 1);
                (*out).mode = S_IFCHR | 0o666;
            }
            return Ok(0);
        }
        return Err(Errno::BadF);
    }

    // SAFETY: non-null `inode`.
    if let Some(getattr) = unsafe { (*(*inode).ops).getattr } {
        // SAFETY: `inode`/`out` are both live; `getattr` is a
        // filesystem driver callback.
        return Ok(unsafe { getattr(inode, out) });
    }

    // Generic fallback when the filesystem doesn't implement inode
    // getattr yet.
    vfs_ilock(inode);
    // SAFETY: non-null `inode`/`out`.
    unsafe {
        ptr::write_bytes(out, 0, 1);
        (*out).dev = if !(*inode).sb.is_null() { (*inode).sb as u64 as i32 } else { 0 };
        (*out).ino = (*inode).ino;
        (*out).mode = (*inode).mode;
        (*out).nlink = (*inode).n_links;
        (*out).size = (*inode).size as u64;
    }
    vfs_iunlock(inode);
    Ok(0)
}

pub(crate) extern "C" fn vfs_filestat(file: *mut vfs_file, out: *mut stat) -> c_int {
    match vfs_filestat_inner(file, out) {
        Ok(v) => v,
        Err(e) => e.neg(),
    }
}

/// Core logic behind [`vfs_filewrite`], factored out as a private helper
/// returning [`KResult`] (P3-CS6) — same shape/rationale as
/// [`vfs_fileread_inner`] with the `O_RDONLY`/`O_WRONLY` access checks
/// swapped.
fn vfs_filewrite_inner(
    file: *mut vfs_file,
    buf: *const c_void,
    n: usize,
    user: c_int,
) -> KResult<isize> {
    if file.is_null() {
        return Err(Errno::BadF);
    }
    if buf.is_null() {
        return Err(Errno::Fault);
    }
    if n == 0 {
        return Ok(0); // POSIX: zero-length write succeeds.
    }

    // SAFETY: non-null `file`.
    let inode = vfs_inode_deref(unsafe { ptr::addr_of_mut!((*file).inode) });

    // Handle pipe/socket write -- these don't have inodes.
    if inode.is_null() {
        // SAFETY: non-null `file`.
        let ops = unsafe { (*file).ops };
        let Some(write) = unsafe { ops.as_ref() }.and_then(|o| o.write) else {
            return Err(Errno::BadF); // Not a writable file object.
        };
        let _g = file_lock(file).lock();
        // SAFETY: non-null `file`.
        if unsafe { (*file).f_flags } & O_ACCMODE == O_RDONLY {
            return Err(Errno::BadF); // File not opened for writing.
        }
        // SAFETY: `file`/`buf` live for the call; `write` may sleep,
        // held under `file_lock` exactly as the C original does.
        return Ok(unsafe { write(file, buf as *const c_char, n, user as bool_) });
    }

    // Character device write: same no-file-lock-across-I/O rationale as
    // `vfs_fileread`.
    // SAFETY: non-null `inode`.
    if is_chr(unsafe { (*inode).mode }) {
        let _g = file_lock(file).lock();
        // SAFETY: non-null `file`.
        if unsafe { (*file).f_flags } & O_ACCMODE == O_RDONLY {
            return Err(Errno::BadF);
        }
        // SAFETY: non-null `file`.
        let ops = unsafe { (*file).ops };
        let ret = if let Some(write) = unsafe { ops.as_ref() }.and_then(|o| o.write) {
            // SAFETY: see the pipe/socket write call site above.
            unsafe { write(file, buf as *const c_char, n, user as bool_) }
        } else {
            // SAFETY: non-null `file`.
            let cdev = unsafe { (*file).__bindgen_anon_1.cdev };
            cdev_write(cdev, user as bool_, buf, n) as isize
        };
        return Ok(ret);
    }

    let _g = file_lock(file).lock();
    // SAFETY: non-null `file`.
    if unsafe { (*file).f_flags } & O_ACCMODE == O_RDONLY {
        return Err(Errno::BadF); // File not opened for writing.
    }

    // Block device write -- not directly supported, use the buffer cache.
    // SAFETY: non-null `inode`.
    if is_blk(unsafe { (*inode).mode }) {
        return Err(Errno::OpNotSupp);
    }

    // Regular files. We do NOT lock the inode here -- the driver
    // callback (e.g. xv6fs_file_write) acquires transactions and the
    // inode lock in the correct order internally; see the C original's
    // comment (transaction -> inode) for the deadlock this avoids. The
    // file lock still protects the file position and serializes
    // concurrent writes.
    // SAFETY: non-null `inode`.
    let mode = unsafe { (*inode).mode };
    if !is_reg(mode) {
        return Err(if is_dir(mode) { Errno::IsDir } else { Errno::Inval });
    }
    // SAFETY: non-null `file`.
    let ops = unsafe { (*file).ops };
    let Some(write) = unsafe { ops.as_ref() }.and_then(|o| o.write) else {
        return Err(Errno::OpNotSupp);
    };
    // The driver handles file extension, size updates, and truncation
    // internally.
    // SAFETY: see the pipe/socket write call site above.
    let ret = unsafe { write(file, buf as *const c_char, n, user as bool_) };
    if ret > 0 {
        // SAFETY: non-null `file`.
        unsafe { (*file).__bindgen_anon_1.f_pos += ret as loff_t };
    }
    Ok(ret)
}

pub(crate) extern "C" fn vfs_filewrite(
    file: *mut vfs_file,
    buf: *const c_void,
    n: usize,
    user: c_int,
) -> isize {
    match vfs_filewrite_inner(file, buf, n, user) {
        Ok(v) => v,
        Err(e) => e.neg() as isize,
    }
}

/// Core logic behind [`vfs_filelseek`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). Only `EBADF`/`EINVAL`/`ESPIPE` are
/// this function's own failures; the driver-callback (`ops.llseek`)
/// result passes through as `Ok` unconditionally, matching the
/// original's unconditional `return ret`.
fn vfs_filelseek_inner(file: *mut vfs_file, offset: loff_t, whence: c_int) -> KResult<loff_t> {
    if file.is_null() {
        return Err(Errno::BadF);
    }
    // SAFETY: non-null `file`.
    let inode = vfs_inode_deref(unsafe { ptr::addr_of_mut!((*file).inode) });
    if inode.is_null() {
        return Err(Errno::Inval);
    }

    // lseek only applies to regular files.
    // SAFETY: non-null `inode`.
    if !is_reg(unsafe { (*inode).mode }) {
        return Err(Errno::SPipe); // Illegal seek.
    }
    let _g = file_lock(file).lock();
    // Note: we do NOT lock the inode here -- the driver callback (e.g.
    // xv6fs_file_llseek) is responsible for acquiring it when needed
    // (e.g. for SEEK_END), matching the read/write design above.
    // SAFETY: non-null `file`.
    let ops = unsafe { (*file).ops };
    let Some(llseek) = unsafe { ops.as_ref() }.and_then(|o| o.llseek) else {
        return Err(Errno::SPipe); // Not seekable.
    };
    // SAFETY: `file` is live; `llseek` is a filesystem driver callback.
    let ret = unsafe { llseek(file, offset, whence) };
    if ret >= 0 {
        // SAFETY: non-null `file`.
        unsafe { (*file).__bindgen_anon_1.f_pos = ret };
    }
    Ok(ret)
}

pub(crate) extern "C" fn vfs_filelseek(file: *mut vfs_file, offset: loff_t, whence: c_int) -> loff_t {
    match vfs_filelseek_inner(file, offset, whence) {
        Ok(v) => v,
        Err(e) => e.neg() as loff_t,
    }
}

/// Core logic behind [`truncate`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). Only `EBADF`/`EISDIR`/`EINVAL` are
/// this function's own failures; `vfs_itruncate`'s result passes through
/// as `Ok` unconditionally (it may itself already be a negative errno),
/// matching the original's unconditional `return`.
fn truncate_inner(file: *mut vfs_file, length: loff_t) -> KResult<c_int> {
    if file.is_null() {
        return Err(Errno::BadF);
    }
    // SAFETY: non-null `file`.
    let inode = vfs_inode_deref(unsafe { ptr::addr_of_mut!((*file).inode) });
    if inode.is_null() {
        return Err(Errno::Inval);
    }

    // SAFETY: non-null `inode`.
    let mode = unsafe { (*inode).mode };
    if !is_reg(mode) {
        return Err(if is_dir(mode) { Errno::IsDir } else { Errno::Inval });
    }
    if length < 0 {
        return Err(Errno::Inval);
    }

    let _g = file_lock(file).lock();
    // `vfs_ilock` is acquired inside `vfs_itruncate`.
    Ok(vfs_itruncate(inode, length))
}

pub(crate) extern "C" fn truncate(file: *mut vfs_file, length: loff_t) -> c_int {
    match truncate_inner(file, length) {
        Ok(v) => v,
        Err(e) => e.neg(),
    }
}

// ===========================================================================
// VFS pipe allocation.
// ===========================================================================

/// Core logic behind [`vfs_pipealloc`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). The `pipe_alloc` `ERR_PTR` failure is a
/// cross-module boundary this cluster doesn't own -- stays
/// `Err(Errno::Raw(..))`, same precedent as `open_cdev`.
fn vfs_pipealloc_inner(rf: *mut *mut vfs_file, wf: *mut *mut vfs_file) -> KResult<()> {
    // SAFETY: `rf`/`wf` are caller-owned out-params (every caller in the
    // tree passes stack-local `struct vfs_file *` addresses).
    unsafe {
        *rf = ptr::null_mut();
        *wf = ptr::null_mut();
    }

    let read_file = file_alloc();
    if read_file.is_null() {
        return Err(Errno::NoMem);
    }

    let write_file = file_alloc();
    if write_file.is_null() {
        file_free(read_file);
        return Err(Errno::NoMem);
    }

    let pi = pipe_alloc(0);
    if is_err(pi) {
        file_free(read_file);
        file_free(write_file);
        return Err(Errno::Raw(ptr_err(pi)));
    }

    pipe_open(read_file, pi, O_RDONLY);
    ftable_attach(read_file);

    pipe_open(write_file, pi, O_WRONLY);
    ftable_attach(write_file);

    // SAFETY: see above.
    unsafe {
        *rf = read_file;
        *wf = write_file;
    }
    Ok(())
}

pub(crate) extern "C" fn vfs_pipealloc(rf: *mut *mut vfs_file, wf: *mut *mut vfs_file) -> c_int {
    result_to_neg_errno(vfs_pipealloc_inner(rf, wf))
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

/// Allocate a `vfs_file` with caller-supplied ops and `private_data`,
/// attach it to the global file table, install it into the current
/// process's fd table, and return the fd number. Returns a negative
/// errno on failure.
/// Core logic behind [`vfs_custom_fd_alloc`], factored out as a private
/// helper returning [`KResult`] (P3-CS6). `fd`'s sole failure mode (fd
/// table exhaustion) becomes `Err(Errno::MFile)`; a successful fd passes
/// through as `Ok`.
fn vfs_custom_fd_alloc_inner(
    ops: *mut vfs_file_ops,
    private_data: *mut c_void,
    flags: c_int,
) -> KResult<c_int> {
    let f = file_alloc();
    if f.is_null() {
        return Err(Errno::NoMem);
    }

    // SAFETY: non-null `f`.
    unsafe {
        (*f).f_flags = flags;
        (*f).ops = ops;
        (*f).private_data = private_data;
    }
    ftable_attach(f);

    // SAFETY: `xv6_current_thread()` returns the live current thread.
    let fdtable = unsafe { (*xv6_current_thread()).fdtable };
    let fd = {
        // SAFETY: non-null `fdtable` (every thread has one).
        let _g = KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fdtable).lock) }).lock();
        vfs_fdtable_alloc_fd(fdtable, f)
    };
    vfs_fput(f); // Drop our ref; the fdtable now owns it.

    if fd < 0 {
        return Err(Errno::MFile);
    }
    Ok(fd)
}

pub(crate) extern "C" fn vfs_custom_fd_alloc(
    ops: *mut vfs_file_ops,
    private_data: *mut c_void,
    flags: c_int,
) -> c_int {
    match vfs_custom_fd_alloc_inner(ops, private_data, flags) {
        Ok(fd) => fd,
        Err(e) => e.neg(),
    }
}

/// Core logic behind [`vfs_sockalloc`], factored out as a private helper
/// returning [`KResult`] (P3-CS6). The duplicate-tuple check is this
/// function's only "real" runtime failure mode -- `Err(Errno::AddrInUse)`.
/// Note the exact original control flow is preserved on that path: `f`
/// was already `ftable_attach`ed before the duplicate check runs, and
/// the original never called the matching `ftable_detach`/`vfs_fput`
/// before `file_free`ing it (a pre-existing quirk, not introduced by
/// this conversion -- kept byte-for-byte since behavior must stay
/// identical).
fn vfs_sockalloc_inner(
    out: *mut *mut vfs_file,
    raddr: u32,
    lport: u16,
    rport: u16,
) -> KResult<()> {
    // SAFETY: caller-owned out-param.
    unsafe { *out = ptr::null_mut() };

    let f = file_alloc();
    if f.is_null() {
        return Err(Errno::NoMem);
    }

    let si = kalloc() as *mut sock;
    if si.is_null() {
        file_free(f);
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
        (*f).__bindgen_anon_1.sock = si as *mut crate::bindings::sock;
        (*f).ops = ptr::null_mut(); // Sockets use direct socket I/O.
    }
    ftable_attach(f);

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
        let mut pos = *guard as *mut sock;
        let mut dup = false;
        while !pos.is_null() {
            // SAFETY: `pos` is a live node on the `sockets` list.
            let (praddr, plport, prport) = unsafe { ((*pos).raddr, (*pos).lport, (*pos).rport) };
            if praddr == raddr && plport == lport && prport == rport {
                dup = true;
                break;
            }
            pos = unsafe { (*pos).next };
        }
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
        file_free(f);
        return Err(Errno::AddrInUse);
    }

    // SAFETY: caller-owned out-param.
    unsafe { *out = f };
    Ok(())
}

pub(crate) extern "C" fn vfs_sockalloc(
    out: *mut *mut vfs_file,
    raddr: u32,
    lport: u16,
    rport: u16,
) -> c_int {
    result_to_neg_errno(vfs_sockalloc_inner(out, raddr, lport, rport))
}
