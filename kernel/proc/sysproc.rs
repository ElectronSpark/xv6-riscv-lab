//! Rust port of `kernel/proc/sysproc.c` — process / time / pgroup syscalls.
//!
//! Each `sys_*` is a thin extern-"C" entry point invoked by the kernel
//! syscall dispatch table. Heavy lifting (exit, wait, kill, clone, ...)
//! still lives in the existing C kernel routines; this module only
//! handles user-argument extraction, copyin/copyout, and trivial glue.
//!
//! NO-STANDALONE-FN floor record (whole file):
//! * Methods/assoc fns: the syscall bodies already delegate to methods —
//!   `Proc::{exit,wait,waitpid}`, `Pgroup::{setpgid,getpgid}`,
//!   `ThreadAccess::{resolve_tgid,pid,group_exit,parent_ptr,vm_ptr,...}`,
//!   `CloneArgs::spawn`; `Utsname::zeroed` is already an associated fn.
//! * Every `sys_*` stays a nullary `extern "C"` fn: it is reached out-of-`proc`
//!   only as a fn-pointer *value* in `crate::irq::syscall`'s dispatch table
//!   (the C syscall ABI), never through a struct receiver — the exact C-ABI
//!   dispatch-table floor. Argument state lives in the trapframe (via the
//!   `arg*` helpers / `current()`), not in a Rust value we could take `self` on.
//! * The three file-private helpers stay free (foreign-fn adapter floor, no
//!   natural receiver): `current` aliases the imported `xv6_current_thread`;
//!   `either_copyout` is a `*const`→`*mut` cast adapter over the foreign
//!   `mm::either_copyout`; `thread_clone` is a layout-identity typed wrapper
//!   that forwards to `CloneArgs::spawn`.

#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};

use crate::bindings;
// P3-1D mesh sweep: dev/nullrand.rs is in scope for this wave; real
// signature is `(buf: *mut u8, count: usize)`, not this file's former
// `len: c_int` mirror (ABI-truth drift -- `chunk` is bounds-checked
// non-negative at every call site, so `chunk as usize` is exact).
use crate::dev::nullrand::NullRandDev;
use crate::proc::proc_shims::xv6_current_thread;

// ---------------------------------------------------------------------------
// Constants mirrored from C headers.
// ---------------------------------------------------------------------------
const EINVAL: i64 = 22;
const EFAULT: i64 = 14;
const EINTR:  i64 = 4;

const SIGCHLD: u64 = 17;

// clone_flags.h
const CLONE_VFORK: u64 = 0x4000000000;
const CLONE_VM:    u64 = 0x8000000000;

// goldfish_rtc.h
const NS_PER_SEC: u64 = 1_000_000_000;
const NS_PER_US:  u64 = 1_000;

// ---------------------------------------------------------------------------
// Mirror layouts for tiny UABI structs the syscalls copy in/out.
// ---------------------------------------------------------------------------
#[repr(C)]
#[derive(Default, Copy, Clone)]
struct Timeval { tv_sec: i64, tv_usec: i64 }

#[repr(C)]
#[derive(Default, Copy, Clone)]
struct Timespec { tv_sec: i64, tv_nsec: i64 }

#[repr(C)]
#[derive(Copy, Clone)]
struct Utsname {
    sysname:  [u8; 65],
    nodename: [u8; 65],
    release:  [u8; 65],
    version:  [u8; 65],
    machine:  [u8; 65],
}
impl Utsname {
    const fn zeroed() -> Self {
        Self {
            sysname:  [0; 65],
            nodename: [0; 65],
            release:  [0; 65],
            version:  [0; 65],
            machine:  [0; 65],
        }
    }
}

/// Mirrors `struct clone_args` from `uabi/clone_flags.h` (8 × u64).
#[repr(C)]
#[derive(Default, Copy, Clone)]
struct CloneArgs {
    flags:      u64,
    stack:      u64,
    stack_size: u64,
    entry:      u64,
    esignal:    u64,
    tls:        u64,
    ctid:       u64,
    ptid:       u64,
}

// ---------------------------------------------------------------------------
// FFI surface.
// ---------------------------------------------------------------------------
// P3-D3c: the arg-fetch helpers (`irq/syscall.rs`), `get_jiffs`
// (`timer/timer_core.rs`) and the KERNBASE physical address
// (`start_kernel.rs`) are plain crate-path imports now that their
// `#[no_mangle]` exports are gone (the extern block that used to sit
// here is deleted; `safestrcpy` moved to its own block below --
// string.rs's libc-shaped exports are a mandated keep).
use crate::irq::syscall::{argaddr, argint, argint64};
use crate::start_kernel::__physical_memory_start;
use crate::timer::timer_core::get_jiffs;

unsafe extern "C" {
    // Strings.
    pub safe fn safestrcpy(s: *mut c_char, t: *const c_char,
                           n: c_int) -> *mut c_char;
}
// P3-D3c: `proc/exit.rs`'s `exit` is a plain (safe) Rust fn now that its
// `#[no_mangle]` export is gone -- imported via its private sibling module
// path (the `crate::proc` glob would work too, but the direct path is
// unambiguous by construction).
use crate::proc::Proc;

// P3-D3a: `either_copyin`/`vm_copyin`/`vm_growheap` (mm/vm.rs) are
// ordinary (safe) Rust fns now that their `#[no_mangle]` exports are
// gone; reached as crate-path items instead of the `extern "C"`
// redeclarations that used to sit in the block above (identical
// signatures).
use crate::mm::{either_copyin, Vm};

// Divergence the old `either_copyout` redeclaration papered over: it
// typed `src` as `*const c_void`, while the real fn takes `*mut c_void`
// (the callee only reads through it) — the thin cast adapter preserves
// this file's read-only call-site types.
#[inline]
fn either_copyout(user_dst: c_int, dst: u64, src: *const c_void, len: u64) -> c_int {
    crate::mm::either_copyout(user_dst, dst, src as *mut c_void, len)
}

// P3-1B mesh sweep: the callees below are demoted (no more `#[no_mangle]`)
// and referenced as plain crate-path items instead of `extern "C"`
// redeclarations -- each has exactly one caller anywhere in the tree, this
// file.
use crate::proc::Pgroup;
// P3-1C mesh sweep: tty/session.rs is in scope for this wave; identical
// `pid_t`/`c_int` signatures, so these become plain crate-path imports
// instead of `extern "C"` redeclarations.
use crate::tty::session::SessionTable;
use crate::timer::sched_timer::sleep_ms_interruptible;
use crate::timer::goldfish_rtc::goldfish_rtc_read_ns;
// P3-1B2: `thread_group_exit`/`thread_tgid`/`signal_pending` used to be
// bridged via hand-maintained `extern "C"` redeclarations here, one of
// which (`thread_group_exit`) had drifted from the truth -- declared
// `-> !` even though the real definition (`proc/thread_group.rs`)
// returns `()` on the `p == NULL` path (unreachable in practice since
// `current()` is never null, but still ABI-unsound as a signature: a
// caller could be miscompiled to assume this call never returns).
// Direct crate-path calls make the compiler enforce the real signature
// instead of trusting a hand-copied one; see `sys_exit_group` below for
// the one adjustment this forced (an explicit trailing `0` since the
// call is no longer diverging). `thread_tgid`/`signal_pending` had no
// such drift (already `c_int`/`bool`-correct... `signal_pending`'s
// return actually was declared `c_int` here vs the real `bool` --
// harmless in practice, same as `sys_signal.rs`'s identical case, but
// now moot) -- pulled in alongside for the same reason.
// NO-STANDALONE-FN: `thread_group_exit`/`thread_tgid` are now the `ThreadAccess`
// methods `group_exit` / `resolve_tgid` (reached via the full access path below).
use crate::proc::access::ThreadAccess;

/// `thread_clone`'s real definition (`proc/clone.rs`) takes
/// `*mut crate::proc::CloneArgs` -- a distinct (but `#[repr(C)]`,
/// layout-identical) type from this file's own local `CloneArgs` mirror.
/// Previously bridged via an untyped `extern "C"` redeclaration (which
/// even had a `*const` vs `*mut` mismatch the ABI boundary papered over);
/// now a thin typed wrapper, same precedent as `mm/vm_pgtab.rs`'s
/// `page_alloc`/`__pa_to_page` re-typed wrappers from the P3-1A mesh sweep.
#[inline(always)]
fn thread_clone(args: *const CloneArgs) -> c_int {
    // SAFETY: `CloneArgs` here and `crate::proc::CloneArgs` are both
    // `#[repr(C)]` with the identical field layout (8 `u64`s, same order --
    // both mirror `uabi/clone_flags.h`'s `struct clone_args`); reinterpreting
    // the pointer is sound. `crate::proc::clone::thread_clone` only reads
    // through this pointer (never writes), so passing it through as `*mut`
    // is safe despite the `*const` source, matching the pre-existing
    // extern-declaration's own (looser) contract.
    crate::proc::CloneArgs::spawn(args as *mut CloneArgs as *mut crate::proc::CloneArgs)
}

#[inline(always)]
fn current() -> *mut bindings::thread { xv6_current_thread() }

// ---------------------------------------------------------------------------
// Syscall implementations.
// ---------------------------------------------------------------------------

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_exit() -> u64 {
    let mut n: c_int = 0;
    argint(0, &mut n);
    Proc::exit(n); // does not return
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_getpid() -> u64 {
    ThreadAccess::from_ptr(current()).map_or(-1, |ta| ta.resolve_tgid()) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_getppid() -> u64 {
    // SAFETY: `current()` (`xv6_current_thread()`) is the running thread that the
    // currently running thread's pointer (from
    // `xv6_current_thread()`/`current()`) is a kernel-wide invariant: always
    // non-null while executing kernel code on behalf of a thread.
    let cta = unsafe { crate::proc::access::ThreadAccess::assume(current()) };
    let parent = cta.parent_ptr();
    if parent.is_null() {
        return 1;
    }
    ThreadAccess::from_ptr(parent).map_or(-1, |ta| ta.resolve_tgid()) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_gettid() -> u64 {
    // SAFETY: `current()` (`xv6_current_thread()`) is the running thread that the
    // currently running thread's pointer (from
    // `xv6_current_thread()`/`current()`) is a kernel-wide invariant: always
    // non-null while executing kernel code on behalf of a thread.
    unsafe { crate::proc::access::ThreadAccess::assume(current()) }.pid() as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_exit_group() -> u64 {
    let mut n: c_int = 0;
    argint(0, &mut n);
    // `thread_group_exit`'s real signature returns `()`, not `!` (see the
    // P3-1B2 note above): every reachable path (`current()` is never
    // null) actually diverges internally via `exit(code)`, but the type
    // system no longer takes that on faith, so an explicit trailing
    // value is required here same as any ordinary `-> u64` fn.
    // NO-STANDALONE-FN: former `thread_group_exit(current(), n)`; the delegator's
    // null-`p` early-return maps to the `None` arm here (`current()` is never null,
    // so the method runs and diverges internally via `exit(n)`).
    if let Some(t) = ThreadAccess::from_ptr(current()) { t.group_exit(n); }
    0
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_vfork() -> u64 {
    let args = CloneArgs {
        flags:   CLONE_VM | CLONE_VFORK,
        esignal: SIGCHLD,
        ..Default::default()
    };
    thread_clone(&args) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_clone() -> u64 {
    let mut uargs: u64 = 0;
    argaddr(0, &mut uargs);

    let mut args = CloneArgs::default();
    if uargs == 0 {
        // No args provided -> default to fork behaviour.
        args.flags   = SIGCHLD;
        args.esignal = SIGCHLD;
    } else {
        // SAFETY: `current()` (`xv6_current_thread()`) is the running thread that the
        // currently running thread's pointer (from
        // `xv6_current_thread()`/`current()`) is a kernel-wide invariant:
        // always non-null while executing kernel code on behalf of a thread.
        let cta = unsafe { crate::proc::access::ThreadAccess::assume(current()) };
        if Vm::vm_copyin(cta.vm_ptr(),
                     &mut args as *mut _ as *mut c_void,
                     uargs,
                     core::mem::size_of::<CloneArgs>() as u64) < 0 {
            return (-EFAULT) as u64;
        }
        // Linux convention: if esignal not explicitly set, take low byte of flags.
        if args.esignal == 0 {
            args.esignal = args.flags & 0xFF;
        }
    }
    thread_clone(&args) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_wait() -> u64 {
    let mut p: u64 = 0;
    argaddr(0, &mut p);
    Proc::wait(p) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_waitpid() -> u64 {
    let mut pid:     c_int = 0;
    let mut opts:    c_int = 0;
    let mut status_addr: u64 = 0;
    argint(0, &mut pid);
    argaddr(1, &mut status_addr);
    argint(2, &mut opts);
    Proc::waitpid(pid, status_addr, opts) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_sbrk() -> u64 {
    let mut n: i64 = 0;
    argint64(0, &mut n);
    // SAFETY: `current()` (`xv6_current_thread()`) is the running thread that the
    // currently running thread's pointer (from
    // `xv6_current_thread()`/`current()`) is a kernel-wide invariant: always
    // non-null while executing kernel code on behalf of a thread.
    let cta = unsafe { crate::proc::access::ThreadAccess::assume(current()) };
    if let Some(addr) = cta.get_heap_addr() {
        if Vm::vm_growheap(cta.vm_ptr(), n) < 0 {
            return (-1i64) as u64;
        }
        addr
    } else {
        (-1i64) as u64
    }
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_sleep() -> u64 {
    let mut n: c_int = 0;
    argint(0, &mut n);
    let n = if n < 0 { 0u64 } else { n as u64 };
    let remaining = sleep_ms_interruptible(n);
    if remaining > 0 && crate::proc::access::ThreadAccess::from_ptr(current()).is_some_and(|ta| ta.signal_pending()) {
        return (-EINTR) as u64;
    }
    0
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_uptime() -> u64 { get_jiffs() }

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_gettimeofday() -> u64 {
    let mut tv_addr: u64 = 0;
    let mut tz_addr: u64 = 0;
    argaddr(0, &mut tv_addr);
    argaddr(1, &mut tz_addr);
    let _ = tz_addr;

    if tv_addr == 0 {
        return (-EINVAL) as u64;
    }
    let t = goldfish_rtc_read_ns();
    let tv = Timeval {
        tv_sec:  (t / NS_PER_SEC) as i64,
        tv_usec: ((t % NS_PER_SEC) / NS_PER_US) as i64,
    };
    if either_copyout(1, tv_addr, &tv as *const _ as *const c_void,
                      core::mem::size_of::<Timeval>() as u64) < 0 {
        return (-EFAULT) as u64;
    }
    0
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_nanosleep() -> u64 {
    let mut req_addr: u64 = 0;
    let mut rem_addr: u64 = 0;
    argaddr(0, &mut req_addr);
    argaddr(1, &mut rem_addr);

    if req_addr == 0 {
        return (-EINVAL) as u64;
    }

    let mut req = Timespec::default();
    if either_copyin(&mut req as *mut _ as *mut c_void, 1, req_addr,
                     core::mem::size_of::<Timespec>() as u64) < 0 {
        return (-EFAULT) as u64;
    }

    if req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1_000_000_000 {
        return (-EINVAL) as u64;
    }

    let total_ns = (req.tv_sec as u64).wrapping_mul(1_000_000_000)
                 + req.tv_nsec as u64;
    let mut ms = (total_ns + 999_999) / 1_000_000;
    if total_ns > 0 && ms == 0 { ms = 1; }

    let remaining_ms = sleep_ms_interruptible(ms);

    if remaining_ms > 0 && crate::proc::access::ThreadAccess::from_ptr(current()).is_some_and(|ta| ta.signal_pending()) {
        if rem_addr != 0 {
            let rem_ns = remaining_ms.wrapping_mul(1_000_000);
            let rem = Timespec {
                tv_sec:  (rem_ns / 1_000_000_000) as i64,
                tv_nsec: (rem_ns % 1_000_000_000) as i64,
            };
            // Best-effort write; ignore copyout failure per POSIX.
            let _ = either_copyout(1, rem_addr, &rem as *const _ as *const c_void,
                                   core::mem::size_of::<Timespec>() as u64);
        }
        return (-EINTR) as u64;
    }

    if rem_addr != 0 {
        let rem = Timespec::default();
        if either_copyout(1, rem_addr, &rem as *const _ as *const c_void,
                          core::mem::size_of::<Timespec>() as u64) < 0 {
            return (-EFAULT) as u64;
        }
    }
    0
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_uname() -> u64 {
    let mut addr: u64 = 0;
    argaddr(0, &mut addr);
    if addr == 0 {
        return (-EINVAL) as u64;
    }

    let mut u = Utsname::zeroed();
    safestrcpy(u.sysname.as_mut_ptr()  as *mut c_char, b"xv6\0".as_ptr()      as *const c_char, 65);
    safestrcpy(u.nodename.as_mut_ptr() as *mut c_char, b"xv6\0".as_ptr()      as *const c_char, 65);
    safestrcpy(u.release.as_mut_ptr()  as *mut c_char, b"0.1\0".as_ptr()      as *const c_char, 65);
    safestrcpy(u.version.as_mut_ptr()  as *mut c_char, b"xv6-tmp\0".as_ptr()  as *const c_char, 65);
    safestrcpy(u.machine.as_mut_ptr()  as *mut c_char, b"riscv64\0".as_ptr()  as *const c_char, 65);

    if either_copyout(1, addr, &u as *const _ as *const c_void,
                      core::mem::size_of::<Utsname>() as u64) < 0 {
        return (-EFAULT) as u64;
    }
    0
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_kernbase() -> u64 {
    // SAFETY: read of a kernel-image extern variable; address is stable.
    unsafe { __physical_memory_start }
}

// ---- Process group / session syscalls --------------------------------------

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_setpgid() -> u64 {
    let mut pid:  c_int = 0;
    let mut pgid: c_int = 0;
    argint(0, &mut pid);
    argint(1, &mut pgid);
    Pgroup::setpgid(pid, pgid) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_getpgid() -> u64 {
    let mut pid: c_int = 0;
    argint(0, &mut pid);
    Pgroup::getpgid(pid) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_setsid() -> u64 { SessionTable::setsid() as u64 }

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_getsid() -> u64 {
    let mut pid: c_int = 0;
    argint(0, &mut pid);
    SessionTable::getsid(pid) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_getrandom() -> u64 {
    let mut ubuf: u64 = 0;
    let mut len:  c_int = 0;
    argaddr(0, &mut ubuf);
    argint(1, &mut len);

    if len < 0 { return (-EINVAL) as u64; }
    if len == 0 { return 0; }

    let mut kbuf = [0u8; 64];
    // `done` walks `[0, len)` in `kbuf`-sized strides; each iteration fills and
    // copies out `chunk = min(len - done, kbuf.len())` bytes. The chunk sizes
    // sum to exactly `len`, so a normal exit means all `len` bytes were written
    // (was a manual `while done < len { ...; done += chunk }` accumulator).
    for done in (0..len).step_by(kbuf.len()) {
        let chunk = core::cmp::min(len - done, kbuf.len() as c_int);
        NullRandDev::random_fill_bytes(kbuf.as_mut_ptr(), chunk as usize);
        if either_copyout(1, ubuf + done as u64,
                          kbuf.as_ptr() as *const c_void,
                          chunk as u64) < 0 {
            return if done != 0 { done as u64 } else { (-EFAULT) as u64 };
        }
    }
    len as u64
}
