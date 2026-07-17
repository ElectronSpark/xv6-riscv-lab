//! `/dev/ptmx` and `/dev/pts/N` — VFS-integrated PTY lifecycle.
//!
//! Rust port of `kernel/tty/ptmx.c` (Phase 2 Wave 11, see
//! `docs/rustify/phase2_plan.md`). Every open fd — master or slave —
//! holds one reference on the shared [`PtyPair`]. When the last fd is
//! closed the tty and pair are freed.
//!
//! - `/dev/ptmx` (major 5, minor 2): opening allocates a new PTY pair
//!   (via `kernel/tty/pty.rs`'s `pty_alloc`). The returned fd is the
//!   master side. The slave index is obtained via the `TIOCGPTN`
//!   ioctl; the slave device appears at `/dev/pts/<index>`.
//! - `/dev/pts/N` (major 136, minor N+1): a character device whose
//!   `open_file` callback installs `FileOps` that forward to the
//!   PTY slave tty. Each open fd holds a pair ref.
//!
//! Lifecycle:
//! - ptmx open -> `pty_alloc()`, `pair.refcount = 1`
//! - pts open -> `pair.refcount += 1`
//! - any close -> `pair.refcount -= 1`; if 0 -> [`pty_pair_destroy`]
//! - master close -> also: `tty_hangup`, `cdev_unregister` (which
//!   removes the devtmpfs node via `dev->devname`)
//!
//! # Locking / concurrency
//!
//! [`PTY_TABLE`] (a [`crate::sync::SpinLock`]-owned index-allocation
//! array, Wave P3-8b) protects itself; each [`PtyPair`] has its own
//! `lock` protecting `refcount`/`master_open`/`cdev_live`. Both are
//! plain non-sleeping spinlock critical sections, same as the C
//! original.

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_short, c_void};
use core::mem::MaybeUninit;

use crate::bindings::{cdev_t, mode_t, slab_cache_t, spinlock_t, tty, vfs_file, vfs_inode};
use crate::sync::KSpinlock;

// P3-10a: the file-ops tables are `FileOps` trait implementors now
// (`&'static dyn` dispatch, see `vfs/file.rs`) — the former `static
// mut vfs_file_ops` tables became plain (immutable, `Sync`) unit-struct
// statics, and the read/write adapters return [`KResult`] natively.
use crate::kstd::{Errno, KResult};
use crate::vfs::file::FileOps;

use super::tty::load_acquire_u32;
// P3-1D mesh sweep: dev/cdev.rs is in scope for this wave; signatures are
// identical, so these become plain crate-path imports instead of `extern
// "C"` redeclarations.
use crate::dev::cdev::{cdev_register, cdev_unregister, CdevOps};

// ===========================================================================
// Externs.
// ===========================================================================

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {

}

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

// tty.c (this crate's `kernel/tty/tty.rs`, Phase 2 Wave 11) and pty.c
// (`kernel/tty/pty.rs`, Phase 2 Wave 10) APIs called from here are
// ordinary same-crate Rust items -- called directly through their
// module paths below rather than re-declared as `extern "C"`.
use super::pty::{pty_alloc, pty_master_read, pty_master_write};
use super::tty::{tty_close, tty_hangup, tty_ioctl, tty_open, tty_poll, tty_read, tty_unref, tty_write};

// ===========================================================================
// Constants.
// ===========================================================================

const PTMX_MAJOR: c_int = 5;
const PTMX_MINOR: c_int = 2;
/// Same as `PTY_MAJOR` in `kernel/inc/param.h`.
const PTS_MAJOR: c_int = 136;
const MAX_PTYS: usize = 64;

const ENOMEM: c_int = 12;
const EIO: c_int = 5;
const ENXIO: c_int = 6;
const ENOTTY: c_int = 25;
const ENOSPC: c_int = 28;

/// `TIOCGPTN` (`kernel/inc/uabi/termios.h`) -- get PTY slave number.
const TIOCGPTN: u64 = 0x8004_5430;

/// `kernel/inc/uabi/poll.h`.
const POLLIN: c_short = 0x0001;
const POLLOUT: c_short = 0x0004;

/// `S_IFCHR` (`kernel/inc/uabi/stat.h`).
const S_IFCHR: u32 = 0o020_000;

// ===========================================================================
// Panic helper -- see `kernel/tty/tty_dev.rs::tty_dev_assert_errno` for
// the established pattern this mirrors.
// ===========================================================================

// P3-2 (zero-C wave): `ptmx_assert_errno` used to take its assertion
// message as a runtime `&CStr` embedding a `%d` -- `core::fmt`'s
// `format_args!` requires a literal format string, so (matching
// `console.rs`/`tty_dev.rs`'s equivalent fix) it became a macro, message
// captured as a `:literal` at each of its two call sites below.
macro_rules! ptmx_assert_errno {
    ($cond:expr, $msg:literal, $errno:expr $(,)?) => {
        if !($cond) {
            __panic_start();
            crate::kprintln!(
                "ASSERTION_FAILURE {}:{}: In function '{}':",
                "kernel/tty/ptmx.rs",
                line!(),
                "ptmxinit",
            );
            crate::kprintln!($msg, $errno);
            __panic_end();
        }
    };
}

// ===========================================================================
// Per-PTY state.
// ===========================================================================

#[repr(C)]
struct PtyPair {
    /// The slave tty (allocated by `pty_alloc`).
    slave: *mut tty,
    /// Registered cdev for `/dev/pts/N`.
    slave_cdev: cdev_t,
    /// PTY index (N in `/dev/pts/N`).
    index: c_int,
    /// Open master + slave fds.
    refcount: c_int,
    /// Master side still alive?
    master_open: c_int,
    /// Slave cdev still registered?
    cdev_live: c_int,
    /// Protects `refcount` / `master_open` / `cdev_live`.
    lock: spinlock_t,
}

/// `UnsafeCell<MaybeUninit<T>>` + `unsafe impl Sync` for one file-scope
/// lock/cache/table storage cell. Same pattern as `kernel/kobject.rs`'s
/// `SyncCell<T>` (private there; the established crate convention is to
/// redeclare this tiny wrapper per file -- see e.g. `irq_core.rs`'s
/// `CacheCell`, `kernel/tty/tty.rs`'s own copy).
struct SyncCell<T>(UnsafeCell<MaybeUninit<T>>);
// SAFETY: `PTY_PAIR_CACHE`'s storage is written once by `ptmxinit()`
// before any other entry point runs, then only ever touched through the
// C kernel's own synchronised primitives (`slab_alloc`'s internal
// locking). `PTY_TABLE` (the other former user of this pattern) is now a
// `crate::sync::SpinLock` -- see its own doc comment.
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn uninit() -> Self {
        SyncCell(UnsafeCell::new(MaybeUninit::uninit()))
    }
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get() as *mut T
    }
}

static PTY_PAIR_CACHE: SyncCell<slab_cache_t> = SyncCell::uninit();
#[inline(always)]
fn pty_pair_cache_ptr() -> *mut slab_cache_t {
    PTY_PAIR_CACHE.get()
}

/// `static spinlock_t ptmx_lock;` + `static struct pty_pair
/// *pty_table[MAX_PTYS];` in the C original, now a single lock-owns-data
/// [`crate::sync::SpinLock`] (Wave P3-8b): the lock and the array it
/// protects are one Rust value instead of a separate lock handle plus a
/// raw `UnsafeCell` the caller had to trust was only touched under that
/// lock. `.lock()` returns a guard that `Deref`s/`DerefMut`s straight to
/// `&[*mut PtyPair; MAX_PTYS]` / `&mut [..]` -- every call site below
/// indexes it like a plain array, no `unsafe` required for the access
/// itself (only the pointers *stored* in the array are still raw, same
/// as before).
static PTY_TABLE: crate::sync::SpinLock<[*mut PtyPair; MAX_PTYS]> =
    crate::sync::SpinLock::new(c"ptmx", [core::ptr::null_mut(); MAX_PTYS]);

static mut PTMX_CDEV: MaybeUninit<cdev_t> = MaybeUninit::zeroed();

// ===========================================================================
// Refcount helpers.
// ===========================================================================

/// Drop one reference. Returns `true` if this was the LAST reference
/// and the caller must call [`pty_pair_destroy`].
///
/// # Safety
/// `pair` must be a live `*mut PtyPair`.
unsafe fn pty_pair_put(pair: *mut PtyPair) -> bool {
    // SAFETY: caller guarantees `pair` is live (fn doc).
    let _g = unsafe { KSpinlock::from_bindings(&raw mut (*pair).lock).lock() };
    unsafe {
        (*pair).refcount -= 1;
        (*pair).refcount <= 0
    }
}

/// # Safety
/// `pair` must be a live, exclusively-teardownable `*mut PtyPair` (no
/// other reference outstanding -- the caller must have observed
/// [`pty_pair_put`] return `true`).
unsafe fn pty_pair_destroy(pair: *mut PtyPair) {
    // Clear the table slot so the index can be reused.
    {
        let mut table = PTY_TABLE.lock();
        // SAFETY: `index` is always in `[0, MAX_PTYS)` (only ever set
        // from the scan loop in `ptmx_open_file`).
        let idx = unsafe { (*pair).index as usize };
        if table[idx] == pair {
            table[idx] = core::ptr::null_mut();
        }
    }

    // Free the tty (closes pipe ends, releases slab).
    // SAFETY: `pair` is caller-guaranteed live and exclusively owned
    // (fn doc).
    let slave = unsafe { (*pair).slave };
    if !slave.is_null() {
        unsafe {
            tty_unref(slave);
            (*pair).slave = core::ptr::null_mut();
        }
    }

    unsafe { slab_free(pair as *mut c_void) };
}

/// Build the devtmpfs name `"pts/<idx>"` into `out` (NUL-terminated).
/// `out` must be at least 16 bytes (mirrors the C original's
/// contract). Returns the string length excluding the NUL.
fn pts_name(out: &mut [u8], idx: i32) -> usize {
    let prefix = b"pts/";
    let mut n = 0usize;
    for &b in prefix {
        out[n] = b;
        n += 1;
    }

    let mut digits = [0u8; 8];
    let mut di = 0usize;
    if idx == 0 {
        digits[di] = b'0';
        di += 1;
    } else {
        let mut v = idx;
        while v > 0 {
            digits[di] = b'0' + (v % 10) as u8;
            di += 1;
            v /= 10;
        }
    }
    for i in (0..di).rev() {
        out[n] = digits[i];
        n += 1;
    }
    out[n] = 0;
    n
}

// ===========================================================================
// `/dev/pts/N` -- slave `FileOps` implementor (installed by
// `open_file` callback).
// ===========================================================================

/// `tty_read`/`tty_write`'s raw `i64` ("bytes moved, or negative
/// errno") — and `pty_master_read`/`_write`'s equivalent `isize` — to
/// [`KResult<isize>`], preserving the exact errno via [`Errno::Raw`]
/// passthrough (results this adapter layer doesn't own).
#[inline]
fn count_result(ret: isize) -> KResult<isize> {
    if ret < 0 {
        Err(Errno::Raw(ret as c_int))
    } else {
        Ok(ret)
    }
}

/// Zero-sized `FileOps` implementor for `/dev/pts/N` slave fds
/// (P3-10a; was the `static mut vfs_file_ops PTS_SLAVE_FILE_OPS`
/// fn-pointer table — the old table's `None` slots `llseek`/`fsync`/
/// `fflush`/`fault` are not overridden, the trait defaults reproduce
/// their null-slot dispatch outcomes exactly).
struct PtsSlaveFileOps;

/// The single shared instance `pts_open_file` installs.
static PTS_SLAVE_FILE_OPS: PtsSlaveFileOps = PtsSlaveFileOps;

impl FileOps for PtsSlaveFileOps {
    unsafe fn read(&self, file: *mut vfs_file, buf: *mut c_char, count: usize, user: bool)
        -> KResult<isize> {
        // SAFETY: `file` is a live, open `vfs_file` (VFS contract); its
        // `private_data` was set by `pts_open_file` below.
        let pair = unsafe { (*file).private_data } as *mut PtyPair;
        if pair.is_null() || unsafe { (*pair).slave.is_null() } {
            return Err(Errno::Io);
        }
        count_result(unsafe { tty_read((*pair).slave, buf, count as u64, user as c_int) as isize })
    }

    unsafe fn write(&self, file: *mut vfs_file, buf: *const c_char, count: usize, user: bool)
        -> KResult<isize> {
        let pair = unsafe { (*file).private_data } as *mut PtyPair;
        if pair.is_null() || unsafe { (*pair).slave.is_null() } {
            return Err(Errno::Io);
        }
        count_result(unsafe { tty_write((*pair).slave, buf, count as u64, user as c_int) as isize })
    }

    unsafe fn ioctl(&self, file: *mut vfs_file, cmd: u64, arg: *mut c_void) -> Option<c_int> {
        let pair = unsafe { (*file).private_data } as *mut PtyPair;
        if pair.is_null() || unsafe { (*pair).slave.is_null() } {
            return Some(-EIO);
        }
        Some(unsafe { tty_ioctl((*pair).slave, cmd, arg) })
    }

    unsafe fn poll(&self, file: *mut vfs_file, events: c_short) -> Option<c_int> {
        let pair = unsafe { (*file).private_data } as *mut PtyPair;
        if pair.is_null() || unsafe { (*pair).slave.is_null() } {
            return Some(0);
        }
        Some(unsafe { tty_poll((*pair).slave, events) })
    }

    unsafe fn release(&self, _inode: *mut vfs_inode, file: *mut vfs_file) -> KResult<()> {
        let pair = unsafe { (*file).private_data } as *mut PtyPair;
        if pair.is_null() {
            return Ok(());
        }
        unsafe {
            (*file).private_data = core::ptr::null_mut();

            // Drop the tty-level "open" ref taken in `pts_open_file`.
            tty_close((*pair).slave);

            // Drop the pair ref -- may destroy.
            if pty_pair_put(pair) {
                pty_pair_destroy(pair);
            }
        }
        Ok(())
    }
}

// ===========================================================================
// `/dev/pts/N` -- cdev (`open_file` installs the `FileOps` above).
// ===========================================================================

/// Zero-sized [`CdevOps`] implementor for the per-pair `/dev/pts/N`
/// slave cdevs (P3-10c; was a per-pair-written `cdev_ops_t` struct
/// whose contents were constant -- now one immutable shared `Sync`
/// static; the old `None` slots `read`/`write`/`ioctl`/`poll` are not
/// overridden, the trait defaults reproduce their null-slot dispatch
/// outcomes exactly -- slave I/O goes through `FileOps` instead).
struct PtsCdevOps;

/// The single shared instance `ptmx_open_file` installs on every
/// freshly-allocated pair's `slave_cdev`.
static PTS_CDEV_OPS: PtsCdevOps = PtsCdevOps;

impl CdevOps for PtsCdevOps {
    /// cdev open/release are no-ops; lifecycle is via `FileOps`.
    unsafe fn open(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn release(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn open_file(&self, cdev: *mut cdev_t, file: *mut vfs_file) -> Option<c_int> {
        // SAFETY: dispatch contract (`open_cdev`) -- `cdev`/`file` are
        // both live; `cdev` is a `pair.slave_cdev` (the only cdev this
        // implementor is ever installed on).
        Some(unsafe { pts_open_file(cdev, file) })
    }
}

/// Called by `__vfs_open_cdev` (via [`PtsCdevOps::open_file`]).
/// Installs the slave file ops on the `vfs_file` so that the VFS
/// manages the fd lifecycle. The cdev kobject ref is released by
/// `__vfs_open_cdev` immediately after this returns (because we set
/// `file->ops`).
unsafe fn pts_open_file(cdev: *mut cdev_t, file: *mut vfs_file) -> c_int {
    // SAFETY: `cdev` is caller-guaranteed to be `&pair.slave_cdev` for
    // some live `PtyPair` (the only cdev this callback is ever
    // installed on, in `ptmx_open_file` below).
    let pair = unsafe {
        crate::mm::cffi::container_of::<PtyPair, cdev_t>(cdev, core::mem::offset_of!(PtyPair, slave_cdev))
    };

    // Reject open after master closed (device is being torn down).
    {
        let _g = unsafe { KSpinlock::from_bindings(&raw mut (*pair).lock).lock() };
        if unsafe { (*pair).master_open } == 0 {
            return -ENXIO;
        }
        unsafe { (*pair).refcount += 1 };
    }

    // tty-level open (bumps tty refcount).
    let ret = unsafe { tty_open((*pair).slave) };
    if ret != 0 {
        if unsafe { pty_pair_put(pair) } {
            unsafe { pty_pair_destroy(pair) };
        }
        return ret;
    }

    unsafe {
        (*file).ops = Some(&PTS_SLAVE_FILE_OPS);
        (*file).private_data = pair as *mut c_void;
    }
    0
}


// ===========================================================================
// PTY master -- `FileOps` implementor (installed by `ptmx` `open_file`).
// ===========================================================================

/// Zero-sized `FileOps` implementor for `/dev/ptmx` master fds
/// (P3-10a; was the `static mut vfs_file_ops PTMX_MASTER_FILE_OPS`
/// fn-pointer table — the old table's `None` slots `llseek`/`fsync`/
/// `fflush`/`fault` are not overridden, the trait defaults reproduce
/// their null-slot dispatch outcomes exactly).
struct PtmxMasterFileOps;

/// The single shared instance `ptmx_open_file` installs.
static PTMX_MASTER_FILE_OPS: PtmxMasterFileOps = PtmxMasterFileOps;

impl FileOps for PtmxMasterFileOps {
    unsafe fn read(&self, file: *mut vfs_file, buf: *mut c_char, count: usize, user: bool)
        -> KResult<isize> {
        let pair = unsafe { (*file).private_data } as *mut PtyPair;
        if pair.is_null() || unsafe { (*pair).slave.is_null() } {
            return Err(Errno::NxIo);
        }
        count_result(unsafe {
            pty_master_read((*pair).slave, buf, count, user as crate::bindings::bool_)
        })
    }

    unsafe fn write(&self, file: *mut vfs_file, buf: *const c_char, count: usize, user: bool)
        -> KResult<isize> {
        let pair = unsafe { (*file).private_data } as *mut PtyPair;
        if pair.is_null() || unsafe { (*pair).slave.is_null() } {
            return Err(Errno::NxIo);
        }
        count_result(unsafe {
            pty_master_write((*pair).slave, buf, count, user as crate::bindings::bool_)
        })
    }

    unsafe fn ioctl(&self, file: *mut vfs_file, cmd: u64, arg: *mut c_void) -> Option<c_int> {
        Some(ptmx_fops_ioctl(file, cmd, arg))
    }

    unsafe fn poll(&self, file: *mut vfs_file, events: c_short) -> Option<c_int> {
        Some(ptmx_fops_poll(file, events))
    }

    unsafe fn release(&self, inode: *mut vfs_inode, file: *mut vfs_file) -> KResult<()> {
        ptmx_fops_release(inode, file);
        Ok(())
    }
}

fn ptmx_fops_release(_inode: *mut vfs_inode, file: *mut vfs_file) {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() {
        return;
    }
    unsafe { (*file).private_data = core::ptr::null_mut() };

    // ---- master-specific teardown ----
    let do_unregister;
    {
        let _g = unsafe { KSpinlock::from_bindings(&raw mut (*pair).lock).lock() };
        unsafe { (*pair).master_open = 0 };
        do_unregister = unsafe { (*pair).cdev_live } != 0;
        unsafe { (*pair).cdev_live = 0 };
    }

    // Hang up the slave tty so any blocked readers/writers unblock.
    let slave = unsafe { (*pair).slave };
    if !slave.is_null() {
        unsafe { tty_hangup(slave) };
    }

    // Unregister the slave cdev. `device_unregister()` also removes
    // `/dev/pts/N` from devtmpfs via `dev->devname`. Since all slave
    // fds use `open_file` (no file holds a kobject ref), the kobject
    // drops to 0 immediately and `pts_cdev_release` fires (a no-op).
    // Existing slave fds continue to work because they use
    // `FileOps` directly.
    if do_unregister {
        unsafe { cdev_unregister(&raw mut (*pair).slave_cdev) };
    }

    // Drop the pair ref -- may destroy.
    if unsafe { pty_pair_put(pair) } {
        unsafe { pty_pair_destroy(pair) };
    }
}

fn ptmx_fops_ioctl(file: *mut vfs_file, cmd: u64, arg: *mut c_void) -> c_int {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() {
        return -ENXIO;
    }

    if cmd == TIOCGPTN {
        // Return the slave PTY index (what N in /dev/pts/N).
        let idxp = arg as *mut c_int;
        unsafe { *idxp = (*pair).index };
        return 0;
    }

    // Forward termios / winsize ioctls to the slave tty.
    let slave = unsafe { (*pair).slave };
    if !slave.is_null() {
        return unsafe { tty_ioctl(slave, cmd, arg) };
    }
    -ENOTTY
}

fn ptmx_fops_poll(file: *mut vfs_file, events: c_short) -> c_int {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() || unsafe { (*pair).slave.is_null() } {
        return 0;
    }

    let mut revents: c_short = 0;

    // Master is readable when the slave's output pipe has data.
    if events & POLLIN != 0 {
        // SAFETY: `pair.slave` is a live tty for `pair`'s lifetime.
        let outp = unsafe { (*(*pair).slave).output_pipe };
        if !outp.is_null() {
            // SAFETY: `outp` is a live pipe.
            let nw = unsafe { load_acquire_u32(&raw const (*outp).nwrite) };
            let nr = unsafe { load_acquire_u32(&raw const (*outp).nread) };
            if nw.wrapping_sub(nr) > 0 {
                revents |= POLLIN;
            }
        }
    }

    // Master is always writable (slave input pipe has space).
    if events & POLLOUT != 0 {
        revents |= POLLOUT;
    }

    revents as c_int
}

// ===========================================================================
// `/dev/ptmx` -- character device (`open_file` allocates a PTY pair).
// ===========================================================================

/// Zero-sized [`CdevOps`] implementor for `/dev/ptmx` (P3-10c; same
/// conversion note as [`PtsCdevOps`] -- `open_file` does the real
/// work, cdev open/release are no-ops).
struct PtmxCdevOps;

/// The single shared instance `ptmxinit` installs.
static PTMX_CDEV_OPS: PtmxCdevOps = PtmxCdevOps;

impl CdevOps for PtmxCdevOps {
    unsafe fn open(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn release(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn open_file(&self, cdev: *mut cdev_t, file: *mut vfs_file) -> Option<c_int> {
        // SAFETY: dispatch contract (`open_cdev`) -- `cdev`/`file` are
        // both live.
        Some(unsafe { ptmx_open_file(cdev, file) })
    }
}

unsafe fn ptmx_open_file(_cdev: *mut cdev_t, file: *mut vfs_file) -> c_int {
    // Allocate a PTY index -- reuse freed slots.
    let idx: i32;
    {
        let mut table = PTY_TABLE.lock();
        let mut found: i32 = -1;
        for (i, slot) in table.iter().enumerate() {
            if slot.is_null() {
                found = i as i32;
                break;
            }
        }
        if found < 0 {
            return -ENOSPC;
        }
        // Reserve the slot temporarily (non-null sentinel) so
        // concurrent opens skip it while we finish setting it up.
        table[found as usize] = core::ptr::without_provenance_mut(1);
        idx = found;
    }

    // Allocate the pair structure.
    let pair = unsafe { slab_alloc(pty_pair_cache_ptr()) } as *mut PtyPair;
    if pair.is_null() {
        PTY_TABLE.lock()[idx as usize] = core::ptr::null_mut();
        return -ENOMEM;
    }

    // SAFETY: `pair` is freshly allocated, exclusively-owned slab
    // memory; every field is written before any other code can observe
    // `pair` (matches the C original's `memset(pair, 0, ...)` followed
    // by explicit field assignment -- plain assignment through a raw
    // pointer is a pure store, never a read of the old value, so no
    // pre-zeroing is required for the scalar/pointer fields; the
    // embedded `cdev_t` is written as an explicit zeroed value below
    // before any bitfield accessor -- which does read-modify-write --
    // touches it).
    unsafe {
        (*pair).slave = core::ptr::null_mut();
        (*pair).slave_cdev = MaybeUninit::zeroed().assume_init();
        (*pair).index = idx;
        (*pair).refcount = 1;
        (*pair).master_open = 1;
        (*pair).cdev_live = 0;
        KSpinlock::from_bindings(&raw mut (*pair).lock).init(c"pty_pair".as_ptr());
    }

    // Build slave name "pts/N".
    let mut name = [0u8; 32];
    pts_name(&mut name, idx);

    // Allocate the slave tty.
    let mut slave: *mut tty = core::ptr::null_mut();
    let dev_minor = idx + 1; // device framework rejects minor 0
    let ret = unsafe { pty_alloc(&mut slave, name.as_ptr() as *const c_char, dev_minor) };
    if ret != 0 {
        PTY_TABLE.lock()[idx as usize] = core::ptr::null_mut();
        unsafe { slab_free(pair as *mut c_void) };
        return ret;
    }
    unsafe { (*pair).slave = slave };

    // Set up the slave cdev with `open_file` so slave fds get file ops.
    unsafe {
        (*pair).slave_cdev.dev.major = PTS_MAJOR;
        (*pair).slave_cdev.dev.minor = dev_minor;
        (*pair).slave_cdev.flags.set_readable(1);
        (*pair).slave_cdev.flags.set_writable(1);
        (*pair).slave_cdev.ops = Some(&PTS_CDEV_OPS);
        // devname/devmode so `device_register()` auto-creates the
        // devtmpfs node.
        (*pair).slave_cdev.dev.devname = (*slave).name.as_ptr(); // e.g. "pts/0"
        (*pair).slave_cdev.dev.devmode = (S_IFCHR | 0o620) as mode_t;
    }

    let ret = unsafe { cdev_register(&raw mut (*pair).slave_cdev) };
    if ret != 0 {
        crate::kprintln!("ptmx: failed to register pts/{} cdev: {}", idx, ret);
        unsafe { tty_unref(slave) };
        PTY_TABLE.lock()[idx as usize] = core::ptr::null_mut();
        unsafe { slab_free(pair as *mut c_void) };
        return ret;
    }
    unsafe { (*pair).cdev_live = 1 };

    // Record in global table.
    PTY_TABLE.lock()[idx as usize] = pair;

    // Install master file ops on the opened file.
    unsafe {
        (*file).ops = Some(&PTMX_MASTER_FILE_OPS);
        (*file).private_data = pair as *mut c_void;
    }

    0
}


// ===========================================================================
// Initialization.
// ===========================================================================

pub(crate) extern "C" fn ptmxinit() {
    // SAFETY: `PTY_PAIR_CACHE`/`PTMX_CDEV` are written here (their first
    // use) before any other `ptmx_*`/`pts_*` entry point can run
    // (mirrors the C original's `start_kernel.c`-ordered single call).
    // `PTY_TABLE` needs no such runtime init -- its `SpinLock::new` is a
    // `const fn`, already in the initialised state.
    let ret = unsafe {
        slab_cache_init(
            pty_pair_cache_ptr(),
            c"pty_pair".as_ptr() as *mut c_char,
            core::mem::size_of::<PtyPair>(),
            crate::bindings::SLAB_FLAG_STATIC as u64,
        )
    };
    ptmx_assert_errno!(ret == 0, "ptmxinit: slab_cache_init failed: {}", ret);

    unsafe {
        let cdev = PTMX_CDEV.as_mut_ptr();
        (*cdev).dev.major = PTMX_MAJOR;
        (*cdev).dev.minor = PTMX_MINOR;
        (*cdev).dev.devname = c"ptmx".as_ptr();
        (*cdev).dev.devmode = (S_IFCHR | 0o666) as mode_t;
        (*cdev).flags.set_readable(1);
        (*cdev).flags.set_writable(1);
        (*cdev).ops = Some(&PTMX_CDEV_OPS);

        let ret = cdev_register(cdev);
        ptmx_assert_errno!(ret == 0, "ptmxinit: cdev_register failed: {}", ret);
    }

    crate::kprintln!(
        "ptmx: /dev/ptmx registered (major {}, minor {})",
        PTMX_MAJOR,
        PTMX_MINOR,
    );
}
