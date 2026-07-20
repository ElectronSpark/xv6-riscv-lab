//! Rust port of `kernel/proc/sys_signal.c` — signal-related syscalls.
//!
//! Each `sys_*` is a thin extern-"C" entry point invoked by the kernel
//! syscall dispatch table. The actual signal mechanics (sigprocmask,
//! sigaction, sigsuspend, kill, etc.) remain implemented in C; this
//! module only handles user-argument extraction and copyin/copyout.
//!
//! P3-N-METH floor record (whole file):
//! * Methods: none. Every `sys_*` is a nullary `extern "C"` dispatch-table
//!   entry (the C syscall ABI) with no struct receiver — the exact C-ABI
//!   boundary class. Argument state lives in the trapframe (reached via the
//!   `arg*` helpers / `current()`), not in a Rust value we could take `self`
//!   on. All backends (`sigprocmask`/`sigaction`/... in `proc/signal.rs`)
//!   are plain delegated calls.
//! * Iterators: none. The two loops are both floor — `sys_pause`'s
//!   `signal_pending`/`scheduler_yield` retry loop is the freeze-in-loop
//!   (noalias-hoist) hazard class, and `sys_sigreturn`'s `wfi()` loop is a
//!   divergent halt; neither is a read-only fixed-count scan. There are no
//!   manual sigset copy loops here — the copies go through the single
//!   `either_copyin`/`either_copyout` calls.
//! * Visibility: all 10 `sys_*` stay `pub(crate)` — reached out-of-proc as
//!   fn-pointer values in `crate::irq::syscall`'s dispatch table (a real,
//!   verified out-of-`proc` caller). The file-private helpers `either_copyout`
//!   and `current` are already minimal (`fn`).

#![allow(non_snake_case)]

use core::ffi::{c_int, c_void};

use crate::bindings;
use crate::proc::proc_shims::{xv6_current_thread, xv6_sizeof_sigaction, xv6_thread_state_set};

// errno
const EINVAL: i64 = 22;
const EFAULT: i64 = 14;
const EINTR:  i64 = 4;

// thread_state enum values mirrored from kernel/inc/proc/thread_types.h
const THREAD_INTERRUPTIBLE: c_int = 2;
const THREAD_RUNNING:       c_int = 8;

// Pinned by `_Static_assert` in kernel/proc/proc_rust_shims.c.
const SIZEOF_SIGACTION: u64 = 24;

type sigset_t = u64;

// P3-D3c: `irq/syscall.rs`'s arg-fetch helpers are plain (safe) Rust fns
// now that their `#[no_mangle]` exports are gone; identical signatures,
// plain `use`.
use crate::irq::syscall::{argaddr, argint};

// P3-D3a: `either_copyin`/`either_copyout` (mm/vm.rs) are ordinary (safe)
// Rust fns now that their `#[no_mangle]` exports are gone; reached as
// crate-path items instead of the `extern "C"` redeclarations that used
// to sit in the block above.
use crate::mm::either_copyin;

// Divergence the old `either_copyout` redeclaration papered over: it
// typed `src` as `*const c_void`, while the real fn takes `*mut c_void`
// (the callee only reads through it) — the thin cast adapter preserves
// this file's read-only call-site types.
#[inline]
fn either_copyout(user_dst: c_int, dst: u64, src: *const c_void, len: u64) -> c_int {
    crate::mm::either_copyout(user_dst, dst, src as *mut c_void, len)
}

// P3-D2b: the signal syscall backends (proc/signal.rs) are ordinary Rust
// fns now that their `#[no_mangle]` exports are gone; reached as plain
// (file-private, so no E0659 glob-reexport ambiguity) crate-path items
// instead of the `extern "C"` redeclarations that used to sit in the
// block above. `sigaction` used to be redeclared here with a narrowed
// opaque-pointer signature (`*const c_void`/`*mut c_void`); its single
// call site now casts to the real `*mut bindings::sigaction` instead
// (ABI-identical pointer casts). `signal_pending` used to be redeclared
// `-> c_int`; the real fn returns `bool` (same 0/1 in `a0` under the old
// C ABI), so its single call site drops the `!= 0`.

// P3-D2a: `scheduler_yield` (proc/sched.rs) is a plain crate-path item
// now that its `extern "C"` redeclaration is gone.
use crate::proc::scheduler_yield;

#[inline(always)]
fn current() -> *mut bindings::thread { xv6_current_thread() }

// ---------------------------------------------------------------------------

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_sigprocmask() -> u64 {
    let mut how:        c_int = 0;
    let mut set_addr:   u64   = 0;
    let mut oldset_addr: u64  = 0;
    argint(0, &mut how);
    argaddr(1, &mut set_addr);
    argaddr(2, &mut oldset_addr);

    let mut set: sigset_t = 0;
    let mut oldset: sigset_t = 0;
    if set_addr != 0 {
        if either_copyin(&mut set as *mut _ as *mut c_void, 1, set_addr,
                         core::mem::size_of::<sigset_t>() as u64) < 0 {
            return (-EFAULT) as u64;
        }
    }
    let set_ptr: *const sigset_t = if set_addr != 0 { &set } else { core::ptr::null() };
    let ret = crate::proc::Signal::sigprocmask(how, set_ptr, &mut oldset);
    if ret < 0 {
        return ret as u64;
    }
    if oldset_addr != 0 {
        if either_copyout(1, oldset_addr, &oldset as *const _ as *const c_void,
                          core::mem::size_of::<sigset_t>() as u64) < 0 {
            return (-EFAULT) as u64;
        }
    }
    0
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_sigaction() -> u64 {
    let mut signum:      c_int = 0;
    let mut act_addr:    u64   = 0;
    let mut oldact_addr: u64   = 0;
    argint(0, &mut signum);
    argaddr(1, &mut act_addr);
    argaddr(2, &mut oldact_addr);

    // Sanity: drift guard for SIZEOF_SIGACTION.
    debug_assert_eq!(xv6_sizeof_sigaction(), SIZEOF_SIGACTION);

    let mut act_buf:    [u8; SIZEOF_SIGACTION as usize] = [0; SIZEOF_SIGACTION as usize];
    let mut oldact_buf: [u8; SIZEOF_SIGACTION as usize] = [0; SIZEOF_SIGACTION as usize];

    let mut p_act: *const c_void = core::ptr::null();
    let mut p_oldact: *mut c_void = core::ptr::null_mut();

    if act_addr != 0 {
        p_act = act_buf.as_ptr() as *const c_void;
        if either_copyin(act_buf.as_mut_ptr() as *mut c_void, 1, act_addr,
                         SIZEOF_SIGACTION) < 0 {
            return (-EFAULT) as u64;
        }
    }
    if oldact_addr != 0 {
        p_oldact = oldact_buf.as_mut_ptr() as *mut c_void;
    }

    // Cast the raw byte buffers to the real `struct sigaction` pointers
    // (the old extern redeclaration took `*const c_void`/`*mut c_void`;
    // the buffers are `SIZEOF_SIGACTION`-byte copies of the user struct,
    // pinned by the drift guard above).
    let ret = crate::proc::Signal::sigaction(
        signum,
        p_act as *mut bindings::sigaction,
        p_oldact as *mut bindings::sigaction,
    );
    if ret < 0 {
        return ret as u64;
    }

    if !p_oldact.is_null() {
        if either_copyout(1, oldact_addr,
                          oldact_buf.as_ptr() as *const c_void,
                          SIZEOF_SIGACTION) < 0 {
            return (-EFAULT) as u64;
        }
    }
    0
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_sigpending() -> u64 {
    let mut set_addr: u64 = 0;
    argaddr(0, &mut set_addr);
    let mut set: sigset_t = 0;
    let ret = crate::proc::access::ThreadAccess::from_ptr(current()).map_or(-22, |ta| ta.sigpending_query(&mut set));
    if ret < 0 { return ret as u64; }
    if set_addr != 0 {
        if either_copyout(1, set_addr, &set as *const _ as *const c_void,
                          core::mem::size_of::<sigset_t>() as u64) < 0 {
            return (-EFAULT) as u64;
        }
    }
    0
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_sigreturn() -> u64 {
    let ret = crate::proc::Signal::sigreturn();
    if ret < 0 { return ret as u64; }

    let p = current();
    // Mirror the C `assert(p != NULL, ...)`. In a non-panic build, halt.
    if p.is_null() {
        loop { crate::machine::wfi(); }
    }
    // Return the restored a0 from the sigframe so the syscall dispatcher
    // doesn't overwrite it (preserves e.g. -EINTR from sigsuspend).
    // SAFETY: `p` is proven non-null by the diverging null check (`if p.is_null() {
    // loop { ... } }`) above.
    unsafe { crate::proc::access::ThreadAccess::assume(p) }.trapframe_a0()
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_pause() -> u64 {
    let p = current();
    loop {
        xv6_thread_state_set(p, THREAD_INTERRUPTIBLE);
        if crate::proc::access::ThreadAccess::from_ptr(p).is_some_and(|ta| ta.signal_pending()) {
            xv6_thread_state_set(p, THREAD_RUNNING);
            return (-EINTR) as u64;
        }
        scheduler_yield();
    }
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_kill() -> u64 {
    let mut pid: c_int = 0;
    let mut sig: c_int = 0;
    argint(0, &mut pid);
    argint(1, &mut sig);
    crate::proc::Signal::kill(pid, sig) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_tgkill() -> u64 {
    let mut tgid: c_int = 0;
    let mut tid:  c_int = 0;
    let mut sig:  c_int = 0;
    argint(0, &mut tgid);
    argint(1, &mut tid);
    argint(2, &mut sig);
    crate::proc::Signal::tgkill(tgid, tid, sig) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_tkill() -> u64 {
    let mut tid: c_int = 0;
    let mut sig: c_int = 0;
    argint(0, &mut tid);
    argint(1, &mut sig);
    crate::proc::Signal::tkill(tid, sig) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_sigsuspend() -> u64 {
    let mut mask_addr: u64 = 0;
    argaddr(0, &mut mask_addr);
    if mask_addr == 0 { return (-EINVAL) as u64; }

    let mut mask: sigset_t = 0;
    if either_copyin(&mut mask as *mut _ as *mut c_void, 1, mask_addr,
                     core::mem::size_of::<sigset_t>() as u64) < 0 {
        return (-EFAULT) as u64;
    }
    crate::proc::Signal::sigsuspend(&mask) as u64
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_sigwait() -> u64 {
    let mut set_addr: u64 = 0;
    let mut sig_addr: u64 = 0;
    argaddr(0, &mut set_addr);
    argaddr(1, &mut sig_addr);

    if set_addr == 0 || sig_addr == 0 {
        return (-EINVAL) as u64;
    }
    let mut set: sigset_t = 0;
    if either_copyin(&mut set as *mut _ as *mut c_void, 1, set_addr,
                     core::mem::size_of::<sigset_t>() as u64) < 0 {
        return (-EFAULT) as u64;
    }
    let mut sig: c_int = 0;
    let ret = crate::proc::Signal::sigwait(&set, &mut sig);
    if ret < 0 { return ret as u64; }
    if either_copyout(1, sig_addr, &sig as *const _ as *const c_void,
                      core::mem::size_of::<c_int>() as u64) < 0 {
        return (-EFAULT) as u64;
    }
    0
}
