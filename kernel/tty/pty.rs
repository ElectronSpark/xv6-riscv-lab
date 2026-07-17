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

use crate::bindings::{bool_, pipe, tty};
use crate::kstd::{Errno, KResult};
use crate::tty::tty::TtyOps;

// ===========================================================================
// Externs.
// ===========================================================================

// P3-D3a: `either_copyin` (mm/vm.rs) is an ordinary (safe) Rust fn now
// that its `#[no_mangle]` export is gone; reached as a crate-path item
// instead of the `extern "C"` redeclaration that used to sit here
// (identical signature).
use crate::mm::either_copyin;

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

/// The PTY slave's [`TtyOps`] implementor — wave P3-10d's replacement
/// for the C-style `static mut PTY_SLAVE_OPS: tty_ops` fn-pointer
/// table (and the crate's only `TtyOps` implementor; the console tty
/// runs with `ops == None`).
///
/// Only `read`/`write` are overridden: the old table's
/// `open`/`close` slots were behaviorally identical to the trait
/// defaults (return-0 / no-op) and every other slot was `None`, which
/// the defaults reproduce exactly.
struct PtySlaveOps;

/// Decode a `pipe_read`/`pipe_write` "count or negative errno" `i64`
/// into the [`TtyOps`] `KResult<usize>` convention, losslessly
/// (`Errno::Raw` carries the exact negative value back across the
/// dispatch boundary).
#[inline]
fn pipe_ret_to_result(r: i64) -> KResult<usize> {
    if r < 0 {
        Err(Errno::Raw(r as c_int))
    } else {
        Ok(r as usize)
    }
}

impl TtyOps for PtySlaveOps {
    /// Slave read <- pull from the input pipe (fed by master write).
    ///
    /// # Safety
    /// `tty` must be a live slave `tty` allocated by [`pty_alloc`],
    /// whose `input_pipe` is non-null for the pty's lifetime; `buf`
    /// must point to at least `nr` writable bytes (the
    /// [`TtyOps::read`] contract).
    unsafe fn read(&self, tty: *mut tty, buf: *mut c_char, nr: usize) -> KResult<usize> {
        // SAFETY: see fn doc.
        pipe_ret_to_result(unsafe { pipe_read((*tty).input_pipe, buf, nr as u64, 0) })
    }

    /// Slave write -> push into the output pipe (master can read it).
    ///
    /// # Safety
    /// `tty` must be a live slave `tty` allocated by [`pty_alloc`],
    /// whose `output_pipe` is non-null for the pty's lifetime; `buf`
    /// must point to at least `nr` readable bytes (the
    /// [`TtyOps::write`] contract).
    unsafe fn write(&self, tty: *mut tty, buf: *const c_char, nr: usize) -> KResult<usize> {
        // SAFETY: see fn doc.
        pipe_ret_to_result(unsafe { pipe_write((*tty).output_pipe, buf, nr as u64, 0) })
    }
}

static PTY_SLAVE_OPS: PtySlaveOps = PtySlaveOps;

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
    // P3-10d: the ops table is a `&'static dyn TtyOps` trait object
    // now (the old `static mut` existed only because `tty_alloc`'s C
    // signature took `*mut tty_ops`).
    let slave = tty_alloc(name, Some(&PTY_SLAVE_OPS));
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
