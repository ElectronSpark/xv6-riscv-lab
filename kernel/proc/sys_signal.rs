//! Rust port of `kernel/proc/sys_signal.c` — signal-related syscalls.
//!
//! Each `sys_*` is a thin extern-"C" entry point invoked by the kernel
//! syscall dispatch table. The actual signal mechanics (sigprocmask,
//! sigaction, sigsuspend, kill, etc.) remain implemented in C; this
//! module only handles user-argument extraction and copyin/copyout.

#![allow(non_snake_case)]

use core::ffi::{c_int, c_void};

use crate::bindings;

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

unsafe extern "C" {
    pub safe fn argint(n: c_int, p: *mut c_int);
    pub safe fn argaddr(n: c_int, p: *mut u64);

    pub safe fn either_copyin(dst: *mut c_void, user_src: c_int,
                              src: u64, len: u64) -> c_int;
    pub safe fn either_copyout(user_dst: c_int, dst: u64,
                               src: *const c_void, len: u64) -> c_int;

    // The C function name `sigaction` collides with the C struct of the
    // same name; we keep the struct opaque (pointer-only) below.
    pub safe fn sigprocmask(how: c_int,
                            set: *const sigset_t,
                            oldset: *mut sigset_t) -> c_int;
    pub safe fn sigaction(signum: c_int,
                          act: *const c_void,
                          oldact: *mut c_void) -> c_int;
    pub safe fn sigpending(p: *mut bindings::thread,
                           set: *mut sigset_t) -> c_int;
    pub safe fn sigreturn() -> c_int;
    pub safe fn sigsuspend(mask: *const sigset_t) -> c_int;
    pub safe fn sigwait(set: *const sigset_t, sig: *mut c_int) -> c_int;

    pub safe fn kill(pid: c_int, sig: c_int) -> c_int;
    pub safe fn tgkill(tgid: c_int, tid: c_int, sig: c_int) -> c_int;
    pub safe fn tkill(tid: c_int, sig: c_int) -> c_int;

    pub safe fn signal_pending(p: *mut bindings::thread) -> c_int;
    pub safe fn scheduler_yield();

    pub safe fn xv6_current_thread() -> *mut bindings::thread;
    pub safe fn xv6_thread_state_set(p: *mut bindings::thread, s: c_int);
    pub safe fn xv6_sizeof_sigaction() -> u64;
}

#[inline(always)]
fn current() -> *mut bindings::thread { xv6_current_thread() }

// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn sys_sigprocmask() -> u64 {
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
    let ret = sigprocmask(how, set_ptr, &mut oldset);
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

#[no_mangle]
pub extern "C" fn sys_sigaction() -> u64 {
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

    let ret = sigaction(signum, p_act, p_oldact);
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

#[no_mangle]
pub extern "C" fn sys_sigpending() -> u64 {
    let mut set_addr: u64 = 0;
    argaddr(0, &mut set_addr);
    let mut set: sigset_t = 0;
    let ret = sigpending(current(), &mut set);
    if ret < 0 { return ret as u64; }
    if set_addr != 0 {
        if either_copyout(1, set_addr, &set as *const _ as *const c_void,
                          core::mem::size_of::<sigset_t>() as u64) < 0 {
            return (-EFAULT) as u64;
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn sys_sigreturn() -> u64 {
    let ret = sigreturn();
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

#[no_mangle]
pub extern "C" fn sys_pause() -> u64 {
    let p = current();
    loop {
        xv6_thread_state_set(p, THREAD_INTERRUPTIBLE);
        if signal_pending(p) != 0 {
            xv6_thread_state_set(p, THREAD_RUNNING);
            return (-EINTR) as u64;
        }
        scheduler_yield();
    }
}

#[no_mangle]
pub extern "C" fn sys_kill() -> u64 {
    let mut pid: c_int = 0;
    let mut sig: c_int = 0;
    argint(0, &mut pid);
    argint(1, &mut sig);
    kill(pid, sig) as u64
}

#[no_mangle]
pub extern "C" fn sys_tgkill() -> u64 {
    let mut tgid: c_int = 0;
    let mut tid:  c_int = 0;
    let mut sig:  c_int = 0;
    argint(0, &mut tgid);
    argint(1, &mut tid);
    argint(2, &mut sig);
    tgkill(tgid, tid, sig) as u64
}

#[no_mangle]
pub extern "C" fn sys_tkill() -> u64 {
    let mut tid: c_int = 0;
    let mut sig: c_int = 0;
    argint(0, &mut tid);
    argint(1, &mut sig);
    tkill(tid, sig) as u64
}

#[no_mangle]
pub extern "C" fn sys_sigsuspend() -> u64 {
    let mut mask_addr: u64 = 0;
    argaddr(0, &mut mask_addr);
    if mask_addr == 0 { return (-EINVAL) as u64; }

    let mut mask: sigset_t = 0;
    if either_copyin(&mut mask as *mut _ as *mut c_void, 1, mask_addr,
                     core::mem::size_of::<sigset_t>() as u64) < 0 {
        return (-EFAULT) as u64;
    }
    sigsuspend(&mask) as u64
}

#[no_mangle]
pub extern "C" fn sys_sigwait() -> u64 {
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
    let ret = sigwait(&set, &mut sig);
    if ret < 0 { return ret as u64; }
    if either_copyout(1, sig_addr, &sig as *const _ as *const c_void,
                      core::mem::size_of::<c_int>() as u64) < 0 {
        return (-EFAULT) as u64;
    }
    0
}
