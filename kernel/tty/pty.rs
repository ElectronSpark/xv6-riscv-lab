//! Pseudo-terminal (PTY) implementation.
//!
//! Rust port of `kernel/tty/pty.c` (Phase 2 Wave 10, see
//! `docs/rustify/phase2_plan.md`). A PTY pair consists of a master and
//! a slave. Data written to the master appears as input on the slave
//! (passes through the slave's line discipline), and data written to
//! the slave appears as output readable from the master.
//!
//! The master is just two pipes; the slave is a full `struct tty` that
//! uses those pipes. `pty_alloc()` creates the pair. The master side
//! is driven by `kernel/tty/ptmx.c` (still C, Wave 11), which holds
//! the slave `tty` pointer and calls [`pty_master_read`] /
//! [`pty_master_write`] directly — this file exports the same C ABI
//! `ptmx.c` already links against.
//!
//! ```text
//! master-write  ──►  slave tty_input()  (line discipline)
//! master-read   ◄──  slave output pipe
//! ```
//!
//! # Locking / concurrency
//!
//! This file takes no locks of its own — every operation forwards
//! straight into `pipe_read`/`pipe_write` (`kernel/vfs/pipe.c`) or
//! `tty_input` (`kernel/tty/tty.c`), which own their respective
//! locking. There is no C-side manual lock/unlock pair in the original
//! `pty.c` for a `KSpinlock` RAII conversion to replace.

use core::ffi::{c_char, c_int, c_void};

use crate::bindings::{bool_, pipe, tty, tty_ops};

// ===========================================================================
// Externs.
// ===========================================================================

unsafe extern "C" {
    pub safe fn either_copyin(dst: *mut c_void, user_src: c_int, src: u64, len: u64) -> c_int;
}

// P3-1C mesh sweep: tty/tty.rs and vfs/pipe.rs are in scope for this
// wave; converted from `extern "C"` redeclarations to plain crate-path
// items (identical signatures). ABI-truth note: `tty_alloc`/`tty_input`
// are real `unsafe extern "C" fn`s -- this file's former mirror declared
// them `safe`, but both call sites already sit inside an `unsafe fn`
// body (`pty_alloc`) or an explicit `unsafe { }` block, so no behavior
// changes.
use crate::tty::tty::{tty_alloc, tty_input};
use crate::vfs::pipe::{pipe_read, pipe_write};

const EFAULT: i64 = 14;

// `is_err`/`ptr_err`'s canonical home is `crate::kstd` (P3-CS1
// centralization).
use crate::kstd::{is_err, ptr_err};

// ===========================================================================
// PTY slave ops.
// ===========================================================================

unsafe extern "C" fn pty_slave_open(_tty: *mut tty) -> c_int {
    0
}

unsafe extern "C" fn pty_slave_close(_tty: *mut tty) {}

/// Slave write -> push into the output pipe (master can read it).
///
/// # Safety
/// `tty` must be a live slave `tty` allocated by [`pty_alloc`], whose
/// `output_pipe` is non-null for the pty's lifetime; `buf` must point
/// to at least `nr` readable bytes (the `tty_ops::write` contract).
unsafe extern "C" fn pty_slave_write(tty: *mut tty, buf: *const c_char, nr: usize) -> isize {
    // SAFETY: see fn doc.
    unsafe { pipe_write((*tty).output_pipe, buf, nr as u64, 0) as isize }
}

/// Slave read <- pull from the input pipe (fed by master write).
///
/// # Safety
/// `tty` must be a live slave `tty` allocated by [`pty_alloc`], whose
/// `input_pipe` is non-null for the pty's lifetime; `buf` must point
/// to at least `nr` writable bytes (the `tty_ops::read` contract).
unsafe extern "C" fn pty_slave_read(tty: *mut tty, buf: *mut c_char, nr: usize) -> isize {
    // SAFETY: see fn doc.
    unsafe { pipe_read((*tty).input_pipe, buf, nr as u64, 0) as isize }
}

static mut PTY_SLAVE_OPS: tty_ops = tty_ops {
    open: Some(pty_slave_open),
    close: Some(pty_slave_close),
    hangup: None,
    throttle: None,
    unthrottle: None,
    stop: None,
    start: None,
    discard_input: None,
    read: Some(pty_slave_read),
    write: Some(pty_slave_write),
    rx: None,
    tx: None,
    set_termios: None,
    set_winsize: None,
    ioctl: None,
};

// ===========================================================================
// PTY master.
// ===========================================================================

/// Data from the master goes through the slave's line discipline via
/// `tty_input()`.
///
/// # Safety
/// `slave` must be a live `tty` (as allocated by [`pty_alloc`]). `buf`
/// must point to at least `count` readable bytes if `user` is false,
/// or be a valid userspace address range otherwise (checked internally
/// by `either_copyin`).
pub(crate) unsafe extern "C" fn pty_master_write(
    slave: *mut tty,
    buf: *const c_char,
    count: usize,
    user: bool_,
) -> isize {
    let mut kbuf = [0u8; 64];
    let mut written: usize = 0;

    while written < count {
        let batch = core::cmp::min(count - written, kbuf.len());

        if user != 0 {
            // SAFETY: `either_copyin` validates the userspace range
            // itself; `kbuf` is a valid `batch`-byte write target.
            let r = unsafe {
                either_copyin(
                    kbuf.as_mut_ptr() as *mut c_void,
                    1,
                    (buf as u64).wrapping_add(written as u64),
                    batch as u64,
                )
            };
            if r < 0 {
                return if written > 0 { written as isize } else { -EFAULT as isize };
            }
        } else {
            // SAFETY: caller guarantees `buf[..count]` is readable
            // kernel memory when `user` is false (fn doc); `written +
            // batch <= count` by construction of `batch` above.
            unsafe {
                core::ptr::copy_nonoverlapping(
                    (buf as *const u8).add(written),
                    kbuf.as_mut_ptr(),
                    batch,
                );
            }
        }

        // Feed through the slave's line discipline.
        // SAFETY: `slave` is caller-guaranteed live (fn doc); `kbuf[..batch]`
        // was just filled above by either path.
        let ret = unsafe { tty_input(slave, kbuf.as_ptr() as *const c_char, batch as u64) };
        if ret < 0 {
            return if written > 0 { written as isize } else { ret as isize };
        }
        written += ret as usize;
    }

    written as isize
}

/// Read data that the slave has written (i.e. the slave's output pipe).
///
/// # Safety
/// `slave` must be a live `tty` (as allocated by [`pty_alloc`]). `buf`
/// must point to at least `count` writable bytes if `user` is false,
/// or be a valid userspace address range otherwise (checked internally
/// by `pipe_read`).
pub(crate) unsafe extern "C" fn pty_master_read(
    slave: *mut tty,
    buf: *mut c_char,
    count: usize,
    user: bool_,
) -> isize {
    // SAFETY: forwarded straight to `pipe_read`, which validates `buf`
    // itself per the userspace/kernel split encoded by `user`; `slave`
    // is caller-guaranteed live (fn doc), so `(*slave).output_pipe` is
    // a valid read.
    unsafe { pipe_read((*slave).output_pipe, buf, count as u64, user as c_int) as isize }
}

// ===========================================================================
// Allocation.
// ===========================================================================

/// Create a PTY master/slave pair.
///
/// On success, `*slave_out` receives a pointer to the slave `tty`.
/// `minor` (minor device number for `/dev/pts/N`) is unused: the
/// devtmpfs node is created automatically by `device_register()` when
/// the slave cdev is registered in `ptmx_open_file()`.
///
/// Returns 0 on success, negative errno on failure.
///
/// The caller interacts with the master side through
/// [`pty_master_read`] / [`pty_master_write`], passing the slave `tty`
/// pointer.
///
/// # Safety
/// `slave_out` must be a valid, writable `*mut *mut tty`. `name` must
/// be a valid, NUL-terminated C string for the duration of this call
/// (the `tty_alloc` contract).
pub(crate) unsafe extern "C" fn pty_alloc(
    slave_out: *mut *mut tty,
    name: *const c_char,
    _minor: c_int,
) -> c_int {
    // SAFETY: `PTY_SLAVE_OPS` is a static table of function pointers,
    // fully initialized at compile time and never mutated afterward;
    // forming a raw pointer to it (never dereferenced here) is not a
    // data race. `static mut` (rather than `static`) is required only
    // because `tty_alloc`'s C signature takes `*mut tty_ops`.
    let ops = unsafe { &raw mut PTY_SLAVE_OPS };
    let slave = tty_alloc(name, ops);
    if is_err(slave) {
        return ptr_err(slave);
    }

    // devtmpfs node is now created automatically by device_register()
    // when the slave cdev is registered in ptmx_open_file().

    // SAFETY: caller guarantees `slave_out` is valid and writable (fn doc).
    unsafe {
        *slave_out = slave;
    }
    0
}

/// Clean up for a PTY pair.
///
/// The devtmpfs node is now removed automatically by
/// `device_unregister()` when the slave cdev is unregistered (via
/// `dev->devname`). This function is retained for any future
/// non-devtmpfs cleanup.
pub(crate) extern "C" fn pty_dealloc(_slave: *mut tty) {
    // devtmpfs removal is handled by cdev_unregister -> device_unregister.
}
