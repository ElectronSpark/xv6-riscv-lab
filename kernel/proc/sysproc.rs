//! Rust port of `kernel/proc/sysproc.c` — process / time / pgroup syscalls.
//!
//! Each `sys_*` is a thin extern-"C" entry point invoked by the kernel
//! syscall dispatch table. Heavy lifting (exit, wait, kill, clone, ...)
//! still lives in the existing C kernel routines; this module only
//! handles user-argument extraction, copyin/copyout, and trivial glue.

#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings;

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
unsafe extern "C" {
    // Argument extraction (defs.h).
    pub safe fn argint(n: c_int, p: *mut c_int);
    pub safe fn argint64(n: c_int, p: *mut i64);
    pub safe fn argaddr(n: c_int, p: *mut u64);

    // Userspace copies (mm/vm.h).
    pub safe fn either_copyin(dst: *mut c_void, user_src: c_int,
                              src: u64, len: u64) -> c_int;
    pub safe fn either_copyout(user_dst: c_int, dst: u64,
                               src: *const c_void, len: u64) -> c_int;
    pub safe fn vm_copyin(vm: *mut bindings::vm, dst: *mut c_void,
                          uaddr: u64, len: u64) -> c_int;

    // Process / thread primitives.
    pub safe fn exit(status: c_int) -> !;
    pub safe fn vm_growheap(vm: *mut bindings::vm, n: i64) -> c_int;

    // Time.
    pub safe fn get_jiffs() -> u64;

    // Strings.
    pub safe fn safestrcpy(s: *mut c_char, t: *const c_char,
                           n: c_int) -> *mut c_char;

    // RNG.
    pub safe fn random_fill_bytes(buf: *mut u8, len: c_int);

    // C shim for `current` macro.
    pub safe fn xv6_current_thread() -> *mut bindings::thread;

    // Extern variable: KERNBASE physical address.
    static __physical_memory_start: u64;
}

// P3-1B mesh sweep: the callees below are demoted (no more `#[no_mangle]`)
// and referenced as plain crate-path items instead of `extern "C"`
// redeclarations -- each has exactly one caller anywhere in the tree, this
// file.
use crate::proc::{wait, waitpid, pgroup_setpgid, pgroup_getpgid};
// P3-1C mesh sweep: tty/session.rs is in scope for this wave; identical
// `pid_t`/`c_int` signatures, so these become plain crate-path imports
// instead of `extern "C"` redeclarations.
use crate::tty::session::{session_getsid, session_setsid};
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
use crate::proc::{thread_group_exit, thread_tgid, signal_pending};

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
    crate::proc::thread_clone(args as *mut CloneArgs as *mut crate::proc::CloneArgs)
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
    exit(n); // does not return
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_getpid() -> u64 {
    thread_tgid(current()) as u64
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
    thread_tgid(parent) as u64
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
    thread_group_exit(current(), n);
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
        if vm_copyin(cta.vm_ptr(),
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
    wait(p) as u64
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
    waitpid(pid, status_addr, opts) as u64
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
        if vm_growheap(cta.vm_ptr(), n) < 0 {
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
    if remaining > 0 && signal_pending(current()) {
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

    if remaining_ms > 0 && signal_pending(current()) {
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
    pgroup_setpgid(pid, pgid) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_getpgid() -> u64 {
    let mut pid: c_int = 0;
    argint(0, &mut pid);
    pgroup_getpgid(pid) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_setsid() -> u64 { session_setsid() as u64 }

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_getsid() -> u64 {
    let mut pid: c_int = 0;
    argint(0, &mut pid);
    session_getsid(pid) as u64
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
    let mut done: c_int = 0;
    while done < len {
        let mut chunk = len - done;
        if chunk > kbuf.len() as c_int {
            chunk = kbuf.len() as c_int;
        }
        random_fill_bytes(kbuf.as_mut_ptr(), chunk);
        if either_copyout(1, ubuf + done as u64,
                          kbuf.as_ptr() as *const c_void,
                          chunk as u64) < 0 {
            return if done != 0 { done as u64 } else { (-EFAULT) as u64 };
        }
        done += chunk;
    }
    let _ = ptr::null_mut::<u8>(); // silence unused import warning if any
    done as u64
}
