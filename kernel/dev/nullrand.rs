//! `/dev/null`, `/dev/zero`, `/dev/random` character devices -- Rust port
//! of `kernel/dev/nullrand.c` (Phase 2 Wave 24; see
//! `docs/rustify/phase2_plan.md`).
//!
//! Three trivial `cdev_t` registrants on top of [`super::cdev`]'s
//! dispatch layer (`cdev_register`, Wave 21):
//!
//! * `/dev/null` (major/minor from `kernel/inc/param.h`'s
//!   `NULL_MAJOR`/`NULL_MINOR`): reads always return EOF (0 bytes),
//!   writes are discarded (report `count` bytes "written").
//! * `/dev/zero` (`ZERO_MAJOR`/`ZERO_MINOR`): reads fill the caller's
//!   buffer with zero bytes (in 64-byte `either_copyout` chunks for
//!   userspace destinations, a single `memset` for kernel destinations);
//!   writes are discarded like `/dev/null`.
//! * `/dev/random` (`RANDOM_MAJOR`/`RANDOM_MINOR`): reads fill the
//!   caller's buffer with pseudo-random bytes from [`random_fill_bytes`];
//!   writes are discarded like `/dev/null`.
//!
//! # RNG fidelity
//!
//! [`random_fill_bytes`] is also called directly (same-crate, no FFI) by
//! `kernel/proc/sysproc.rs`'s `sys_getrandom` -- that caller already
//! declares this exact symbol via its own local `unsafe extern "C"`
//! block (`buf: *mut u8, len: c_int`), a narrower-than-`size_t` mismatch
//! that predates this port. It is harmless on this target: RISC-V's
//! lp64d calling convention passes an `int` argument sign/zero-extended
//! to a full 64-bit register, and every call site passes a small
//! non-negative chunk size, so the callee reading the same register as a
//! `usize` observes an identical value. Left as-is -- `sysproc.rs` is
//! outside this wave's touch scope and the ABI is already proven live
//! (Wave 7's `sys_getrandom`).
//!
//! The generator itself is `xorshift64star` (Marsaglia's xorshift64,
//! star-multiplied by `2685821657736338717`), reseeded on every call
//! with the `time` CSR (`read_time()`, mirrors the C `r_time()` inline)
//! and the running thread pointer (`xv6_current_thread()`, mirrors the C
//! `current` macro) XORed into the state before mixing -- ported
//! line-for-line from the C, including the exact shift amounts (12, 25,
//! 27) and the RELAXED-ordered atomic load/store on the seed word (the C
//! used `__ATOMIC_RELAXED`; `Ordering::Relaxed` is the direct
//! equivalent -- this is a best-effort entropy mixer, not a
//! cryptographically-hardened DRBG, so there is no ordering requirement
//! beyond "the next caller mixes in a state some previous caller wrote,
//! eventually" -- multiple concurrent callers may race the
//! load-modify-store non-atomically as a *sequence* (the C had the same
//! race: two RELAXED ops don't compose into an atomic RMW), which is
//! acceptable for this best-effort use and preserves the original's
//! behavior exactly).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_int, c_void};
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicU64, Ordering};

use crate::bindings::{cdev_t, mode_t};
use crate::kstd::{Errno, KResult};

use super::cdev::{cdev_register, CdevOps};

// ---------------------------------------------------------------------------
// External C symbols, declared locally per this crate's established
// per-file convention (irq/trap.rs, tty/session.rs, mm/vm.rs, ...).
// ---------------------------------------------------------------------------
// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {
}

// P3-D3a: `either_copyout` (mm/vm.rs) is an ordinary (safe) Rust fn now
// that its `#[no_mangle]` export is gone. Divergence the old
// redeclaration papered over: it typed `src` as `*const c_void`, while
// the real fn takes `*mut c_void` (the callee only reads through it) —
// the thin cast adapter preserves this file's read-only call-site types.
#[inline]
fn either_copyout(user_dst: c_int, dst: u64, src: *const c_void, len: u64) -> c_int {
    crate::mm::either_copyout(user_dst, dst, src as *mut c_void, len)
}

// `xv6_current_thread()` -- shim for the `current` macro (same precedent
// as every other Rust module reading `current`).
use crate::proc::proc_shims::xv6_current_thread;

/// Replicates the C `assert(expr, fmt, ret)` expansion used three times
/// in `nullranddevinit()`. Same special-casing precedent as
/// `kernel/ipi.rs`'s `assert_hartid_in_range` (a fixed `kassert!`-style
/// macro can't carry a runtime `%d`).
fn assert_registered(ret: c_int, which: &core::ffi::CStr, func: &core::ffi::CStr) {
    if ret == 0 {
        return;
    }
    __panic_start();
    crate::kprintln!(
        "ASSERTION_FAILURE {}:{}: In function '{}':",
        "kernel/dev/nullrand.rs",
        line!() as c_int,
        crate::printf::Cs(func.as_ptr()),
    );
    crate::kprintln!(
        "nullranddevinit: failed to register {} cdev: {}",
        crate::printf::Cs(which.as_ptr()),
        ret,
    );
    __panic_end();
}

// ---------------------------------------------------------------------------
// Major/minor numbers (kernel/inc/param.h).
// ---------------------------------------------------------------------------
const NULL_MAJOR: c_int = 1;
const NULL_MINOR: c_int = 2;
const RANDOM_MAJOR: c_int = 1;
const RANDOM_MINOR: c_int = 3;
const ZERO_MAJOR: c_int = 1;
const ZERO_MINOR: c_int = 4;

/// `S_IFCHR` (`kernel/inc/uabi/stat.h`); same local copy as
/// `kernel/console.rs`'s `S_IFCHR`.
const S_IFCHR: u32 = 0o020_000;

// ---------------------------------------------------------------------------
// /dev/null
// ---------------------------------------------------------------------------

/// Zero-sized [`CdevOps`] implementor for `/dev/null` (P3-10c; was the
/// `static cdev_ops_t NULL_CDEV_OPS` fn-pointer table -- the four
/// former `extern "C"` callbacks are trait methods now; the old `None`
/// slots `ioctl`/`poll`/`open_file` are not overridden, the trait
/// defaults reproduce their null-slot dispatch outcomes exactly).
struct NullCdevOps;

/// The single shared instance `nullranddevinit` installs.
static NULL_CDEV_OPS: NullCdevOps = NullCdevOps;

impl CdevOps for NullCdevOps {
    unsafe fn open(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn release(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    /// Reads always return EOF (0 bytes).
    unsafe fn read(&self, _cdev: *mut cdev_t, _user: bool, _buf: *mut c_void, _count: usize)
        -> KResult<c_int> {
        Ok(0)
    }
    /// Writes are discarded (report `count` bytes "written").
    unsafe fn write(&self, _cdev: *mut cdev_t, _user: bool, _buf: *const c_void, count: usize)
        -> KResult<c_int> {
        Ok(count as c_int)
    }
}

// ---------------------------------------------------------------------------
// /dev/random
// ---------------------------------------------------------------------------

/// Xorshift64star state seed (matches the C literal exactly).
static RAND_STATE: AtomicU64 = AtomicU64::new(0x9e3779b97f4a7c15);

fn xorshift64star() -> u64 {
    let mut x = RAND_STATE.load(Ordering::Relaxed);
    x ^= crate::machine::read_time();
    // SAFETY: `xv6_current_thread()` always returns a live, non-null
    // pointer (the running thread) -- only its bit pattern is used here,
    // as XOR-mixing entropy, matching the C's `(uint64)current`; the
    // pointer is never dereferenced.
    x ^= xv6_current_thread() as u64;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    x = x.wrapping_mul(2685821657736338717);
    RAND_STATE.store(x, Ordering::Relaxed);
    x
}

/// `void random_fill_bytes(void *buf, size_t count)` -- fills `buf` with
/// `count` pseudo-random bytes. Called both by [`random_read`] (kernel
/// destination path) and, cross-module within this same crate, by
/// `kernel/proc/sysproc.rs`'s `sys_getrandom` (see this module's doc
/// comment for the signature-fidelity note on that caller).
// P3-1D mesh sweep: caller (`proc/sysproc.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn random_fill_bytes(buf: *mut u8, count: usize) {
    if buf.is_null() || count == 0 {
        return;
    }
    let mut i = 0usize;
    while i < count {
        let mut r = xorshift64star();
        let mut b = 0;
        while b < 8 && i < count {
            // SAFETY: `buf` is caller-provided and valid for `count`
            // bytes (checked non-null above); `i < count` is checked by
            // the loop condition on every iteration.
            unsafe { *buf.add(i) = (r & 0xFF) as u8 };
            r >>= 8;
            b += 1;
            i += 1;
        }
    }
}

/// Zero-sized [`CdevOps`] implementor for `/dev/random` (P3-10c; same
/// conversion note as [`NullCdevOps`]).
struct RandomCdevOps;

/// The single shared instance `nullranddevinit` installs.
static RANDOM_CDEV_OPS: RandomCdevOps = RandomCdevOps;

impl CdevOps for RandomCdevOps {
    unsafe fn open(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn release(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    /// Fills the caller's buffer with pseudo-random bytes.
    unsafe fn read(&self, _cdev: *mut cdev_t, user: bool, buf: *mut c_void, count: usize)
        -> KResult<c_int> {
        if buf.is_null() {
            return Err(Errno::Inval);
        }
        if count == 0 {
            return Ok(0);
        }

        if !user {
            random_fill_bytes(buf as *mut u8, count);
            return Ok(count as c_int);
        }

        let mut kbuf = [0u8; 64];
        let mut done: usize = 0;
        while done < count {
            let chunk = core::cmp::min(count - done, kbuf.len());
            random_fill_bytes(kbuf.as_mut_ptr(), chunk);

            // SAFETY: `either_copyout` validates the userspace
            // destination range itself; `kbuf` is a live local buffer
            // of at least `chunk` bytes.
            let r = either_copyout(1, buf as u64 + done as u64, kbuf.as_ptr() as *const c_void, chunk as u64);
            if r < 0 {
                return if done != 0 { Ok(done as c_int) } else { Err(Errno::Fault) };
            }
            done += chunk;
        }
        Ok(done as c_int)
    }
    /// Writes are discarded like `/dev/null`.
    unsafe fn write(&self, _cdev: *mut cdev_t, _user: bool, _buf: *const c_void, count: usize)
        -> KResult<c_int> {
        Ok(count as c_int)
    }
}

// ---------------------------------------------------------------------------
// /dev/zero
// ---------------------------------------------------------------------------

/// Zero-sized [`CdevOps`] implementor for `/dev/zero` (P3-10c; same
/// conversion note as [`NullCdevOps`]; `poll` is deliberately not
/// overridden -- "/dev/zero is always ready -- handled by fallback",
/// the C comment on its old `None` slot).
struct ZeroCdevOps;

/// The single shared instance `nullranddevinit` installs.
static ZERO_CDEV_OPS: ZeroCdevOps = ZeroCdevOps;

impl CdevOps for ZeroCdevOps {
    unsafe fn open(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn release(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }
    /// Fills the caller's buffer with zero bytes.
    unsafe fn read(&self, _cdev: *mut cdev_t, user: bool, buf: *mut c_void, count: usize)
        -> KResult<c_int> {
        if buf.is_null() {
            return Err(Errno::Inval);
        }
        if count == 0 {
            return Ok(0);
        }

        if !user {
            // SAFETY: `buf` is caller-provided and valid for `count`
            // bytes (checked non-null above); a byte-value memset can't
            // misalign/overrun a correctly-sized destination.
            unsafe { core::ptr::write_bytes(buf as *mut u8, 0, count) };
            return Ok(count as c_int);
        }

        // Zero out in chunks to userspace (matches the C's `static
        // const char zeros[64]`).
        const ZEROS: [u8; 64] = [0u8; 64];
        let mut done: usize = 0;
        while done < count {
            let chunk = core::cmp::min(count - done, ZEROS.len());
            // SAFETY: `either_copyout` validates the userspace
            // destination range itself; `ZEROS` is a live buffer of at
            // least `chunk` bytes.
            let r = either_copyout(1, buf as u64 + done as u64, ZEROS.as_ptr() as *const c_void, chunk as u64);
            if r < 0 {
                return if done != 0 { Ok(done as c_int) } else { Err(Errno::Fault) };
            }
            done += chunk;
        }
        Ok(done as c_int)
    }
    /// Writes are discarded like `/dev/null`.
    unsafe fn write(&self, _cdev: *mut cdev_t, _user: bool, _buf: *const c_void, count: usize)
        -> KResult<c_int> {
        Ok(count as c_int)
    }
}

// ---------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------

/// Every field not set explicitly by [`nullranddevinit`] is
/// intentionally left zero, matching the C static initializers'
/// implicit zero-fill for `.dev.kobj`/`.dev.type_`/`.dev.unregistering`
/// (same `MaybeUninit::zeroed()` + populate-then-register precedent as
/// `kernel/console.rs`'s `CONSOLE_CDEV`).
static mut NULL_CDEV: MaybeUninit<cdev_t> = MaybeUninit::zeroed();
static mut RANDOM_CDEV: MaybeUninit<cdev_t> = MaybeUninit::zeroed();
static mut ZERO_CDEV: MaybeUninit<cdev_t> = MaybeUninit::zeroed();

/// `void nullranddevinit(void)` -- registers `/dev/null`, `/dev/random`,
/// `/dev/zero`. Called once from `start_kernel.c`'s single-hart
/// post-init sequence (see that file's `nullranddevinit();` call site).
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn nullranddevinit() {
    // SAFETY: `nullranddevinit()` runs exactly once, from
    // `start_kernel.c`'s single-hart init sequence, before any other
    // code can observe `NULL_CDEV`/`RANDOM_CDEV`/`ZERO_CDEV` (same
    // reasoning as `kernel/console.rs`'s `consoledevinit`).
    unsafe {
        let cdev = NULL_CDEV.as_mut_ptr();
        (*cdev).dev.major = NULL_MAJOR;
        (*cdev).dev.minor = NULL_MINOR;
        (*cdev).dev.devname = c"null".as_ptr();
        (*cdev).dev.devmode = (S_IFCHR | 0o666) as mode_t;
        (*cdev).flags.set_readable(1);
        (*cdev).flags.set_writable(1);
        (*cdev).ops = Some(&NULL_CDEV_OPS);
        let ret = cdev_register(cdev);
        assert_registered(ret, c"null", c"nullranddevinit");

        let cdev = RANDOM_CDEV.as_mut_ptr();
        (*cdev).dev.major = RANDOM_MAJOR;
        (*cdev).dev.minor = RANDOM_MINOR;
        (*cdev).dev.devname = c"random".as_ptr();
        (*cdev).dev.devmode = (S_IFCHR | 0o666) as mode_t;
        (*cdev).flags.set_readable(1);
        (*cdev).flags.set_writable(1);
        (*cdev).ops = Some(&RANDOM_CDEV_OPS);
        let ret = cdev_register(cdev);
        assert_registered(ret, c"random", c"nullranddevinit");

        let cdev = ZERO_CDEV.as_mut_ptr();
        (*cdev).dev.major = ZERO_MAJOR;
        (*cdev).dev.minor = ZERO_MINOR;
        (*cdev).dev.devname = c"zero".as_ptr();
        (*cdev).dev.devmode = (S_IFCHR | 0o666) as mode_t;
        (*cdev).flags.set_readable(1);
        (*cdev).flags.set_writable(1);
        (*cdev).ops = Some(&ZERO_CDEV_OPS);
        let ret = cdev_register(cdev);
        assert_registered(ret, c"zero", c"nullranddevinit");
    }
}
