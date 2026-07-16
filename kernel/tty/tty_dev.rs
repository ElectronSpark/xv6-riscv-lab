//! `/dev/tty` character device.
//!
//! Rust port of `kernel/tty/tty_dev.c` (Phase 2 Wave 10, see
//! `docs/rustify/phase2_plan.md`). Provides a character device node at
//! `/dev/tty` (major 5, minor 0) that forwards read/write/ioctl/poll
//! operations to the calling thread's controlling terminal. If the
//! thread has no controlling terminal, operations return `-ENXIO`.
//! This is analogous to Linux's `/dev/tty` (major 5, minor 0).
//!
//! # Locking / concurrency
//!
//! This file itself takes no locks — every operation resolves the
//! caller's controlling `tty` (via `session_get_ctrl_tty`, itself
//! locked internally by `kernel/tty/session.c`) and then forwards
//! straight into the `tty_*` API (`kernel/tty/tty.c`), which owns all
//! `struct tty` locking. There is no C-side manual lock/unlock pair in
//! the original `tty_dev.c` for a `KSpinlock` RAII conversion to
//! replace.

use core::ffi::{c_char, c_int, c_short, c_void};
use core::mem::MaybeUninit;

use crate::bindings::{bool_, cdev_ops_t, cdev_t, mode_t, session, tty};

// ===========================================================================
// Externs.
// ===========================================================================

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {
}
// P3-D2b: `pid_wlock`/`pid_wunlock` (proc/pid.rs) are plain crate-path
// items now that their `#[no_mangle]` exports are gone (identical
// signatures).
use crate::proc::{pid_wlock, pid_wunlock};
// P3-1D mesh sweep: dev/cdev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::dev::cdev::cdev_register;

// P3-1C mesh sweep: tty/{tty,session}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures). ABI-truth note: all six are real
// `unsafe extern "C" fn`s -- this file's former mirror declared them
// `safe`; every call site already sits inside an `unsafe extern "C" fn`
// body except `ctrl_tty()`'s `session_get_ctrl_tty` call, which gets an
// explicit `unsafe { }` block below.
use super::session::session_get_ctrl_tty;
use super::tty::{tty_close, tty_ioctl, tty_open, tty_poll, tty_read, tty_write};

const ENXIO: c_int = 6;

/// `S_IFCHR` (`kernel/inc/uabi/stat.h`).
const S_IFCHR: u32 = 0o020_000;

const TTY_DEV_MAJOR: c_int = 5;
const TTY_DEV_MINOR: c_int = 0;

// ===========================================================================
// Panic helper — replicates the C `assert(expr, fmt, arg)` macro
// expansion (same pattern as `kernel/console.rs::console_assert_errno`,
// not reused directly since `console.rs` is out of scope for this
// wave).
// ===========================================================================

// P3-2 (zero-C wave): `tty_dev_assert_errno` used to take its assertion
// message as a runtime `&CStr` embedding a `%d` -- `core::fmt`'s
// `format_args!` requires a literal format string, so (matching
// `console.rs`'s equivalent fix) its one call site below was inlined
// directly instead of kept as a helper.

// ===========================================================================
// Controlling-terminal lookup.
// ===========================================================================

/// Get the current thread's controlling terminal.
///
/// Returns null if the thread has no session or no controlling tty.
///
/// # Mandated fix (Wave 22, `RUST_REWRITE.md` Known issues)
///
/// `session_get_ctrl_tty()` documents (and, since Wave 12,
/// `pid_assert_wholding()`-asserts) that the caller must hold
/// `pid_wlock`. This function previously called it bare, which was
/// inert only because `/dev/tty` could not be opened at all (the
/// pre-Wave-21 minor-0 bug) — Wave 21 fixed that bug and made the
/// contract violation reachable, panicking on `cat /dev/tty`. Fixed by
/// taking `pid_wlock`/`pid_wunlock` around the call, matching every
/// other `session_get_ctrl_tty` caller's discipline (see
/// `kernel/tty/session.rs`).
#[inline]
fn ctrl_tty() -> *mut tty {
    let t = crate::machine::current_thread_ptr();
    if t.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `t` is the live current-thread pointer returned by
    // `current_thread_ptr()` (the Rust equivalent of the C `current`
    // macro), just checked non-null; `session` is a plain pointer
    // field with no further invariants to uphold for a read.
    let session = unsafe { (*t).session };
    if session.is_null() {
        return core::ptr::null_mut();
    }
    pid_wlock();
    // SAFETY: `session` was just checked non-null above.
    let t = unsafe { session_get_ctrl_tty(session) };
    pid_wunlock();
    t
}

// ===========================================================================
// cdev ops.
// ===========================================================================

unsafe extern "C" fn ttydev_open(_cdev: *mut cdev_t) -> c_int {
    let t = ctrl_tty();
    if t.is_null() {
        return -ENXIO;
    }
    tty_open(t)
}

unsafe extern "C" fn ttydev_release(_cdev: *mut cdev_t) -> c_int {
    let t = ctrl_tty();
    if t.is_null() {
        return 0;
    }
    tty_close(t);
    0
}

/// # Safety
/// `buffer` must point to at least `count` writable bytes if `user` is
/// false, or be a valid userspace address range otherwise (`tty_read`
/// validates the userspace case internally).
unsafe extern "C" fn ttydev_read(
    _cdev: *mut cdev_t,
    user: bool_,
    buffer: *mut c_void,
    count: usize,
) -> c_int {
    let t = ctrl_tty();
    if t.is_null() {
        return -ENXIO;
    }
    // SAFETY: forwarded straight to `tty_read`, which validates
    // `buffer` itself per the userspace/kernel split encoded by `user`
    // (caller's obligation, restated in this fn's doc).
    unsafe { tty_read(t, buffer as *mut c_char, count as u64, user as c_int) as c_int }
}

/// # Safety
/// `buffer` must point to at least `count` readable bytes if `user` is
/// false, or be a valid userspace address range otherwise (`tty_write`
/// validates the userspace case internally).
unsafe extern "C" fn ttydev_write(
    _cdev: *mut cdev_t,
    user: bool_,
    buffer: *const c_void,
    count: usize,
) -> c_int {
    let t = ctrl_tty();
    if t.is_null() {
        return -ENXIO;
    }
    // SAFETY: see `ttydev_read`.
    unsafe { tty_write(t, buffer as *const c_char, count as u64, user as c_int) as c_int }
}

unsafe extern "C" fn ttydev_ioctl(_cdev: *mut cdev_t, cmd: u64, arg: *mut c_void) -> c_int {
    let t = ctrl_tty();
    if t.is_null() {
        return -ENXIO;
    }
    tty_ioctl(t, cmd, arg)
}

unsafe extern "C" fn ttydev_poll(_cdev: *mut cdev_t, events: c_short) -> c_int {
    let t = ctrl_tty();
    if t.is_null() {
        return 0;
    }
    tty_poll(t, events)
}

// ===========================================================================
// Registration.
// ===========================================================================

static mut TTY_CDEV_OPS: cdev_ops_t = cdev_ops_t {
    read: None,
    write: None,
    open: None,
    release: None,
    ioctl: None,
    poll: None,
    open_file: None,
};

static mut TTY_CDEV: MaybeUninit<cdev_t> = MaybeUninit::zeroed();

pub(crate) extern "C" fn ttydevinit() {
    // SAFETY: `ttydevinit()` runs exactly once, from `start_kernel.c`'s
    // single-hart init sequence, before any other code can observe
    // `TTY_CDEV`/`TTY_CDEV_OPS`.
    unsafe {
        TTY_CDEV_OPS = cdev_ops_t {
            read: Some(ttydev_read),
            write: Some(ttydev_write),
            open: Some(ttydev_open),
            release: Some(ttydev_release),
            ioctl: Some(ttydev_ioctl),
            poll: Some(ttydev_poll),
            open_file: None,
        };

        let cdev = TTY_CDEV.as_mut_ptr();
        (*cdev).dev.major = TTY_DEV_MAJOR;
        (*cdev).dev.minor = TTY_DEV_MINOR;
        (*cdev).dev.devname = c"tty".as_ptr();
        (*cdev).dev.devmode = (S_IFCHR | 0o666) as mode_t;
        (*cdev).flags.set_readable(1);
        (*cdev).flags.set_writable(1);
        (*cdev).ops = TTY_CDEV_OPS;

        let ret = cdev_register(cdev);
        if ret != 0 {
            __panic_start();
            crate::kprintln!(
                "ASSERTION_FAILURE {}:{}: In function '{}':",
                "kernel/tty/tty_dev.rs",
                line!(),
                "ttydevinit",
            );
            crate::kprintln!("ttydevinit: cdev_register failed: {}", ret);
            __panic_end();
        }
    }
}
