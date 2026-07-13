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

unsafe extern "C" {
    pub safe fn cdev_register(dev: *mut cdev_t) -> c_int;

    pub safe fn tty_open(t: *mut tty) -> c_int;
    pub safe fn tty_close(t: *mut tty);
    pub safe fn tty_read(t: *mut tty, buf: *mut c_char, count: u64, user: c_int) -> i64;
    pub safe fn tty_write(t: *mut tty, buf: *const c_char, count: u64, user: c_int) -> i64;
    pub safe fn tty_ioctl(t: *mut tty, cmd: u64, arg: *mut c_void) -> c_int;
    pub safe fn tty_poll(t: *mut tty, events: c_short) -> c_int;

    pub safe fn session_get_ctrl_tty(s: *mut session) -> *mut tty;

    pub safe fn printf(fmt: *const c_char, ...) -> c_int;
    pub safe fn __panic_start();
    pub safe fn __panic_end() -> !;
}

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

fn tty_dev_assert_errno(cond: bool, line: u32, msg: &core::ffi::CStr, errno: c_int) {
    if cond {
        return;
    }
    __panic_start();
    printf(
        c"ASSERTION_FAILURE %s:%d: In function '%s':\n".as_ptr(),
        c"kernel/tty/tty_dev.rs".as_ptr(),
        line as c_int,
        c"ttydevinit".as_ptr(),
    );
    printf(msg.as_ptr(), errno);
    printf(c"\n".as_ptr());
    __panic_end();
}

// ===========================================================================
// Controlling-terminal lookup.
// ===========================================================================

/// Get the current thread's controlling terminal.
///
/// Returns null if the thread has no session or no controlling tty.
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
    session_get_ctrl_tty(session)
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

#[no_mangle]
pub extern "C" fn ttydevinit() {
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
        (*cdev).__bindgen_anon_1.set_readable(1);
        (*cdev).__bindgen_anon_1.set_writable(1);
        (*cdev).ops = TTY_CDEV_OPS;

        let ret = cdev_register(cdev);
        tty_dev_assert_errno(
            ret == 0,
            line!(),
            c"ttydevinit: cdev_register failed: %d\n",
            ret,
        );
    }
}
