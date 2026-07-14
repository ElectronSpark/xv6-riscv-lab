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
//!   `open_file` callback installs `vfs_file_ops` that forward to the
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
//! `PTMX_LOCK` (a [`crate::sync::KSpinlock`]) protects the
//! `PTY_TABLE` index-allocation array; each [`PtyPair`] has its own
//! `lock` protecting `refcount`/`master_open`/`cdev_live`. Both are
//! plain non-sleeping spinlock critical sections, same as the C
//! original.

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_short, c_void};
use core::mem::MaybeUninit;

use crate::bindings::{cdev_ops_t, cdev_t, mode_t, slab_cache_t, spinlock_t, tty, vfs_file, vfs_file_ops, vfs_inode};
use crate::sync::KSpinlock;

use super::tty::load_acquire_u32;
// P3-1D mesh sweep: dev/cdev.rs is in scope for this wave; signatures are
// identical, so these become plain crate-path imports instead of `extern
// "C"` redeclarations.
use crate::dev::cdev::{cdev_register, cdev_unregister};

// ===========================================================================
// Externs.
// ===========================================================================

unsafe extern "C" {
    pub safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
    pub safe fn slab_free(obj: *mut c_void);
    pub safe fn slab_cache_init(
        cache: *mut slab_cache_t,
        name: *mut c_char,
        obj_size: usize,
        flags: u64,
    ) -> c_int;

    pub safe fn __panic_start();
    pub safe fn __panic_end() -> !;
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
// SAFETY: `PTMX_LOCK`'s and `PTY_PAIR_CACHE`'s storage is written once
// by `ptmxinit()` before any other entry point runs, then only ever
// touched through the C kernel's own synchronised primitives
// (`spin_lock`/`spin_unlock` via `KSpinlock`, `slab_alloc`'s internal
// locking). `PTY_TABLE`'s array is only ever mutated while holding
// `ptmx_lock()`.
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

static PTMX_LOCK: SyncCell<spinlock_t> = SyncCell::uninit();
#[inline(always)]
fn ptmx_lock() -> KSpinlock {
    KSpinlock::from_bindings(PTMX_LOCK.get())
}

static PTY_PAIR_CACHE: SyncCell<slab_cache_t> = SyncCell::uninit();
#[inline(always)]
fn pty_pair_cache_ptr() -> *mut slab_cache_t {
    PTY_PAIR_CACHE.get()
}

/// `static struct pty_pair *pty_table[MAX_PTYS];` -- protected by
/// [`ptmx_lock`].
struct PtyTableCell(UnsafeCell<[*mut PtyPair; MAX_PTYS]>);
// SAFETY: see `SyncCell`'s note above -- all mutation happens under
// `ptmx_lock()`.
unsafe impl Sync for PtyTableCell {}
static PTY_TABLE: PtyTableCell = PtyTableCell(UnsafeCell::new([core::ptr::null_mut(); MAX_PTYS]));

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
        let _g = ptmx_lock().lock();
        // SAFETY: mutation is serialised by `ptmx_lock()` (fn doc of
        // `PTY_TABLE`); `index` is always in `[0, MAX_PTYS)` (only ever
        // set from the scan loop in `ptmx_open_file`).
        unsafe {
            let table = &mut *PTY_TABLE.0.get();
            let idx = (*pair).index as usize;
            if table[idx] == pair {
                table[idx] = core::ptr::null_mut();
            }
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
// `/dev/pts/N` -- slave `vfs_file_ops` (installed by `open_file`
// callback).
// ===========================================================================

unsafe extern "C" fn pts_fops_read(file: *mut vfs_file, buf: *mut c_char, count: usize, user: crate::bindings::bool_) -> isize {
    // SAFETY: `file` is a live, open `vfs_file` (VFS contract); its
    // `private_data` was set by `pts_open_file` below.
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() || unsafe { (*pair).slave.is_null() } {
        return -(EIO as isize);
    }
    unsafe { tty_read((*pair).slave, buf, count as u64, user as c_int) as isize }
}

unsafe extern "C" fn pts_fops_write(file: *mut vfs_file, buf: *const c_char, count: usize, user: crate::bindings::bool_) -> isize {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() || unsafe { (*pair).slave.is_null() } {
        return -(EIO as isize);
    }
    unsafe { tty_write((*pair).slave, buf, count as u64, user as c_int) as isize }
}

unsafe extern "C" fn pts_fops_ioctl(file: *mut vfs_file, cmd: u64, arg: *mut c_void) -> c_int {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() || unsafe { (*pair).slave.is_null() } {
        return -EIO;
    }
    unsafe { tty_ioctl((*pair).slave, cmd, arg) }
}

unsafe extern "C" fn pts_fops_poll(file: *mut vfs_file, events: c_short) -> c_int {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() || unsafe { (*pair).slave.is_null() } {
        return 0;
    }
    unsafe { tty_poll((*pair).slave, events) }
}

unsafe extern "C" fn pts_fops_release(_inode: *mut vfs_inode, file: *mut vfs_file) -> c_int {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() {
        return 0;
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
    0
}

static mut PTS_SLAVE_FILE_OPS: vfs_file_ops = vfs_file_ops {
    read: Some(pts_fops_read),
    write: Some(pts_fops_write),
    llseek: None,
    release: Some(pts_fops_release),
    fsync: None,
    fflush: None,
    poll: Some(pts_fops_poll),
    ioctl: Some(pts_fops_ioctl),
    fault: None,
};

// ===========================================================================
// `/dev/pts/N` -- cdev (`open_file` installs the `vfs_file_ops` above).
// ===========================================================================

/// Called by `__vfs_open_cdev`. Installs the slave file ops on the
/// `vfs_file` so that the VFS manages the fd lifecycle. The cdev
/// kobject ref is released by `__vfs_open_cdev` immediately after this
/// returns (because we set `file->ops`).
unsafe extern "C" fn pts_open_file(cdev: *mut cdev_t, file: *mut vfs_file) -> c_int {
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
        (*file).ops = &raw mut PTS_SLAVE_FILE_OPS;
        (*file).private_data = pair as *mut c_void;
    }
    0
}

/// cdev open/release are no-ops; lifecycle is via `vfs_file_ops`.
unsafe extern "C" fn pts_cdev_open(_cdev: *mut cdev_t) -> c_int {
    0
}
unsafe extern "C" fn pts_cdev_release(_cdev: *mut cdev_t) -> c_int {
    0
}

// ===========================================================================
// PTY master -- `vfs_file_ops` (installed by `ptmx` `open_file`).
// ===========================================================================

unsafe extern "C" fn ptmx_fops_read(file: *mut vfs_file, buf: *mut c_char, count: usize, user: crate::bindings::bool_) -> isize {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() || unsafe { (*pair).slave.is_null() } {
        return -(ENXIO as isize);
    }
    unsafe { pty_master_read((*pair).slave, buf, count, user) }
}

unsafe extern "C" fn ptmx_fops_write(file: *mut vfs_file, buf: *const c_char, count: usize, user: crate::bindings::bool_) -> isize {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() || unsafe { (*pair).slave.is_null() } {
        return -(ENXIO as isize);
    }
    unsafe { pty_master_write((*pair).slave, buf, count, user) }
}

unsafe extern "C" fn ptmx_fops_release(_inode: *mut vfs_inode, file: *mut vfs_file) -> c_int {
    let pair = unsafe { (*file).private_data } as *mut PtyPair;
    if pair.is_null() {
        return 0;
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
    // `vfs_file_ops` directly.
    if do_unregister {
        unsafe { cdev_unregister(&raw mut (*pair).slave_cdev) };
    }

    // Drop the pair ref -- may destroy.
    if unsafe { pty_pair_put(pair) } {
        unsafe { pty_pair_destroy(pair) };
    }

    0
}

unsafe extern "C" fn ptmx_fops_ioctl(file: *mut vfs_file, cmd: u64, arg: *mut c_void) -> c_int {
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

unsafe extern "C" fn ptmx_fops_poll(file: *mut vfs_file, events: c_short) -> c_int {
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

static mut PTMX_MASTER_FILE_OPS: vfs_file_ops = vfs_file_ops {
    read: Some(ptmx_fops_read),
    write: Some(ptmx_fops_write),
    llseek: None,
    release: Some(ptmx_fops_release),
    fsync: None,
    fflush: None,
    poll: Some(ptmx_fops_poll),
    ioctl: Some(ptmx_fops_ioctl),
    fault: None,
};

// ===========================================================================
// `/dev/ptmx` -- character device (`open_file` allocates a PTY pair).
// ===========================================================================

unsafe extern "C" fn ptmx_open_file(_cdev: *mut cdev_t, file: *mut vfs_file) -> c_int {
    // Allocate a PTY index -- reuse freed slots.
    let idx: i32;
    {
        let _g = ptmx_lock().lock();
        // SAFETY: mutation serialised by `ptmx_lock()`.
        let table = unsafe { &mut *PTY_TABLE.0.get() };
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
        let _g = ptmx_lock().lock();
        unsafe { (*PTY_TABLE.0.get())[idx as usize] = core::ptr::null_mut() };
        drop(_g);
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
        let _g = ptmx_lock().lock();
        unsafe { (*PTY_TABLE.0.get())[idx as usize] = core::ptr::null_mut() };
        drop(_g);
        unsafe { slab_free(pair as *mut c_void) };
        return ret;
    }
    unsafe { (*pair).slave = slave };

    // Set up the slave cdev with `open_file` so slave fds get file ops.
    unsafe {
        (*pair).slave_cdev.dev.major = PTS_MAJOR;
        (*pair).slave_cdev.dev.minor = dev_minor;
        (*pair).slave_cdev.__bindgen_anon_1.set_readable(1);
        (*pair).slave_cdev.__bindgen_anon_1.set_writable(1);
        (*pair).slave_cdev.ops = cdev_ops_t {
            read: None,
            write: None,
            open: Some(pts_cdev_open),
            release: Some(pts_cdev_release),
            ioctl: None,
            poll: None,
            open_file: Some(pts_open_file),
        };
        // devname/devmode so `device_register()` auto-creates the
        // devtmpfs node.
        (*pair).slave_cdev.dev.devname = (*slave).name.as_ptr(); // e.g. "pts/0"
        (*pair).slave_cdev.dev.devmode = (S_IFCHR | 0o620) as mode_t;
    }

    let ret = unsafe { cdev_register(&raw mut (*pair).slave_cdev) };
    if ret != 0 {
        crate::kprintln!("ptmx: failed to register pts/{} cdev: {}", idx, ret);
        unsafe { tty_unref(slave) };
        let _g = ptmx_lock().lock();
        unsafe { (*PTY_TABLE.0.get())[idx as usize] = core::ptr::null_mut() };
        drop(_g);
        unsafe { slab_free(pair as *mut c_void) };
        return ret;
    }
    unsafe { (*pair).cdev_live = 1 };

    // Record in global table.
    {
        let _g = ptmx_lock().lock();
        unsafe { (*PTY_TABLE.0.get())[idx as usize] = pair };
    }

    // Install master file ops on the opened file.
    unsafe {
        (*file).ops = &raw mut PTMX_MASTER_FILE_OPS;
        (*file).private_data = pair as *mut c_void;
    }

    0
}

/// The ptmx cdev open/release are no-ops -- `open_file` does the real
/// work.
unsafe extern "C" fn ptmx_cdev_open(_cdev: *mut cdev_t) -> c_int {
    0
}
unsafe extern "C" fn ptmx_cdev_release(_cdev: *mut cdev_t) -> c_int {
    0
}

// ===========================================================================
// Initialization.
// ===========================================================================

pub(crate) extern "C" fn ptmxinit() {
    // SAFETY: `PTY_PAIR_CACHE`/`PTMX_LOCK`/`PTMX_CDEV` are written here
    // (their first use) before any other `ptmx_*`/`pts_*` entry point
    // can run (mirrors the C original's `start_kernel.c`-ordered single
    // call).
    let ret = unsafe {
        slab_cache_init(
            pty_pair_cache_ptr(),
            c"pty_pair".as_ptr() as *mut c_char,
            core::mem::size_of::<PtyPair>(),
            crate::bindings::SLAB_FLAG_STATIC as u64,
        )
    };
    ptmx_assert_errno!(ret == 0, "ptmxinit: slab_cache_init failed: {}", ret);

    ptmx_lock().init(c"ptmx".as_ptr());

    unsafe {
        let cdev = PTMX_CDEV.as_mut_ptr();
        (*cdev).dev.major = PTMX_MAJOR;
        (*cdev).dev.minor = PTMX_MINOR;
        (*cdev).dev.devname = c"ptmx".as_ptr();
        (*cdev).dev.devmode = (S_IFCHR | 0o666) as mode_t;
        (*cdev).__bindgen_anon_1.set_readable(1);
        (*cdev).__bindgen_anon_1.set_writable(1);
        (*cdev).ops = cdev_ops_t {
            read: None,
            write: None,
            open: Some(ptmx_cdev_open),
            release: Some(ptmx_cdev_release),
            ioctl: None,
            poll: None,
            open_file: Some(ptmx_open_file),
        };

        let ret = cdev_register(cdev);
        ptmx_assert_errno!(ret == 0, "ptmxinit: cdev_register failed: {}", ret);
    }

    crate::kprintln!(
        "ptmx: /dev/ptmx registered (major {}, minor {})",
        PTMX_MAJOR,
        PTMX_MINOR,
    );
}
