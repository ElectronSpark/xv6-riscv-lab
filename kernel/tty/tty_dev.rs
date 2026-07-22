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

use crate::bindings::{cdev_t, mode_t, session, tty};
use crate::kstd::{cint_result, Errno, KResult};

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
use crate::proc::ProcTable;
// P3-1D mesh sweep: dev/cdev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration. P3-10c: `CdevOps` is the ops-table trait
// this file's `TtyCdevOps` implements.
use crate::dev::cdev::{cdev_register, CdevOps};

// P3-1C mesh sweep: tty/{tty,session}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures). ABI-truth note: all six are real
// `unsafe extern "C" fn`s -- this file's former mirror declared them
// `safe`; every call site already sits inside an `unsafe extern "C" fn`
// body except `ctrl_tty()`'s `session_get_ctrl_tty` call, which gets an
// explicit `unsafe { }` block below.
use super::session::{Session, SessionTable};
use super::tty::Tty;

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

/// Zero-sized marker hosting `/dev/tty`'s device-level (no live-`tty`-
/// instance) operations -- `ctrl_tty` doesn't take a `tty` receiver (it
/// *resolves* one from the current thread), and `ttydevinit` is a global,
/// once-at-boot registration, so neither fits `impl Tty`. Plays the same
/// namespacing role `SessionTable` plays for `kernel/tty/session.rs`'s
/// singleton-registry operations.
pub(crate) struct TtyDev;

impl TtyDev {
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
        // macro), just checked non-null; `session` is now a generational `Sid`
        // (N-R6d-2a) — a plain aligned word read with no further invariants.
        let sid = unsafe { (*t).session };
        if sid.is_none() {
            return core::ptr::null_mut();
        }
        ProcTable::wlock();
        // Resolve the `Sid` to a live session under `pid_wlock` (required by
        // `session_lookup`); a stale key (session freed) resolves to null → no
        // controlling tty, the same answer the old null-pointer path gave.
        let session = SessionTable::lookup(sid).unwrap_or(core::ptr::null_mut());
        let t = if session.is_null() {
            core::ptr::null_mut()
        } else {
            // SAFETY: `session` is a live session pointer just resolved above.
            unsafe { Session::get_ctrl_tty(session) }
        };
        ProcTable::wunlock();
        t
    }
}

// ===========================================================================
// cdev ops.
// ===========================================================================

/// Zero-sized [`CdevOps`] implementor for `/dev/tty` (P3-10c; was the
/// `static mut cdev_ops_t TTY_CDEV_OPS` fn-pointer table,
/// runtime-populated in `ttydevinit` -- now an immutable `Sync` static;
/// the six former `extern "C"` callbacks are trait methods now,
/// `open_file` is not overridden so the trait default reproduces the
/// old `None` slot).
struct TtyCdevOps;

/// The single shared instance `ttydevinit` installs.
static TTY_CDEV_OPS: TtyCdevOps = TtyCdevOps;

impl CdevOps for TtyCdevOps {
    unsafe fn open(&self, _cdev: *mut cdev_t) -> KResult<()> {
        let t = TtyDev::ctrl_tty();
        if t.is_null() {
            return Err(Errno::NxIo);
        }
        // SAFETY: `t` is the live controlling tty just resolved.
        match unsafe { Tty::open(t) } {
            0 => Ok(()),
            ret => Err(Errno::Raw(ret)),
        }
    }

    unsafe fn release(&self, _cdev: *mut cdev_t) -> KResult<()> {
        let t = TtyDev::ctrl_tty();
        if !t.is_null() {
            // SAFETY: `t` is the live controlling tty just resolved.
            unsafe { Tty::close(t) };
        }
        Ok(())
    }

    unsafe fn read(&self, _cdev: *mut cdev_t, user: bool, buf: *mut c_void, count: usize)
        -> KResult<c_int> {
        let t = TtyDev::ctrl_tty();
        if t.is_null() {
            return Err(Errno::NxIo);
        }
        // SAFETY: forwarded straight to `Tty::read`, which validates
        // `buf` itself per the userspace/kernel split encoded by `user`
        // (dispatch contract).
        cint_result(unsafe { Tty::read(t, buf as *mut c_char, count as u64, user as c_int) as c_int })
    }

    unsafe fn write(&self, _cdev: *mut cdev_t, user: bool, buf: *const c_void, count: usize)
        -> KResult<c_int> {
        let t = TtyDev::ctrl_tty();
        if t.is_null() {
            return Err(Errno::NxIo);
        }
        // SAFETY: see `read` above.
        cint_result(unsafe {
            Tty::write(t, buf as *const c_char, count as u64, user as c_int) as c_int
        })
    }

    unsafe fn ioctl(&self, _cdev: *mut cdev_t, cmd: u64, arg: *mut c_void) -> KResult<c_int> {
        let t = TtyDev::ctrl_tty();
        if t.is_null() {
            return Err(Errno::NxIo);
        }
        // SAFETY: `t` live; `arg`'s validity forwarded from the
        // dispatch contract.
        cint_result(unsafe { Tty::ioctl(t, cmd, arg) })
    }

    unsafe fn poll(&self, _cdev: *mut cdev_t, events: c_short) -> Option<c_int> {
        let t = TtyDev::ctrl_tty();
        if t.is_null() {
            return Some(0);
        }
        // SAFETY: `t` live.
        Some(unsafe { Tty::poll(t, events) })
    }
}

// ===========================================================================
// Registration.
// ===========================================================================

static mut TTY_CDEV: MaybeUninit<cdev_t> = MaybeUninit::zeroed();

// NO-STANDALONE-FN: `ttydevinit` relocated to `TtyDev::init`, dropping the
// implicit `tty` naming prefix (matches `Tty::init`'s own naming for the
// line-discipline slab cache); `extern "C"` preserved exactly (called once
// from `start_kernel.rs`).
impl TtyDev {
    pub(crate) extern "C" fn init() {
        // SAFETY: `TtyDev::init()` runs exactly once, from
        // `start_kernel.c`'s single-hart init sequence, before any other
        // code can observe `TTY_CDEV`.
        unsafe {
            let cdev = TTY_CDEV.as_mut_ptr();
            (*cdev).dev.major = TTY_DEV_MAJOR;
            (*cdev).dev.minor = TTY_DEV_MINOR;
            (*cdev).dev.devname = c"tty".as_ptr();
            (*cdev).dev.devmode = (S_IFCHR | 0o666) as mode_t;
            (*cdev).flags.set_readable(1);
            (*cdev).flags.set_writable(1);
            (*cdev).ops = Some(&TTY_CDEV_OPS);

            let ret = cdev_register(cdev);
            if ret != 0 {
                __panic_start();
                crate::kprintln!(
                    "ASSERTION_FAILURE {}:{}: In function '{}':",
                    "kernel/tty/tty_dev.rs",
                    line!(),
                    "TtyDev::init",
                );
                crate::kprintln!("TtyDev::init: cdev_register failed: {}", ret);
                __panic_end();
            }
        }
    }
}
