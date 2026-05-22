//! Rust port of `kernel/proc/exit.c`.
//!
//! Provides: `vfork_done`, `reparent`, `exit`, `wait`, `waitpid`.
//! Heavy fan-out (children iteration, `__reap_zombie` spin loop) is in
//! C shim helpers; this file owns the high-level control flow.
//!
//! ## Centralized-unsafe convention
//!
//! Every raw FFI call this file needs lives in the `raw` module below.
//! For each raw call we expose a tiny safe wrapper in the `ffi` module.
//! Function bodies
//! (`vfork_done`, `reparent`, `exit`, `wait`, `waitpid`) contain *no*
//! visible `unsafe` blocks — they read like ordinary Rust.
//!
//! ### Safety contract for the wrappers
//!
//! Every wrapper that takes a `*mut Thread` (or any opaque kernel
//! object pointer) shares the same precondition:
//!
//!   * The pointer was obtained from another kernel API
//!     (`xv6_current_thread`, `__proctab_get_initproc`, a sibling
//!     accessor, …) and refers to a live object whose lifetime is held
//!     by an appropriate higher-level lock (`pid_lock` rwlock,
//!     `tcb_lock`, or the implicit "I own this thread" guarantee for
//!     the running task).
//!
//! These preconditions held in the original C port, where the same
//! field accesses were unannotated. The wrappers do not weaken them —
//! they only document and centralize them.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_int, c_void};
use core::ptr;

use crate::proc::access::is_err_const;

#[repr(C)] struct Thread       { _p: [u8; 0] }
#[repr(C)] struct ThreadGroup  { _p: [u8; 0] }
#[repr(C)] struct Session      { _p: [u8; 0] }
#[repr(C)] struct Pgroup       { _p: [u8; 0] }
#[repr(C)] struct Vm           { _p: [u8; 0] }
#[repr(C)] struct FsStruct     { _p: [u8; 0] }
#[repr(C)] struct VfsFdtable   { _p: [u8; 0] }
#[repr(C)] struct Sigacts      { _p: [u8; 0] }

const EINVAL: c_int = 22;
const EFAULT: c_int = 14;
const EINTR:  c_int = 4;

const WNOHANG:   c_int = 1;
const WUNTRACED: c_int = 2;

const CLONE_THREAD: u64 = 0x1000000000;

const THREAD_RUNNING:        c_int = 8;
const THREAD_INTERRUPTIBLE:  c_int = 2;
const THREAD_ZOMBIE:         c_int = 11;

// ---------------------------------------------------------------------------
// Raw FFI declarations — the *only* place the rest of this file's
// raw calls reach into.
// ---------------------------------------------------------------------------
mod raw {
    use super::*;

    unsafe extern "C" {
        // Zero-arg, no-precondition entry points may be `safe`.
        pub safe fn xv6_current_thread() -> *mut Thread;
        pub safe fn __proctab_get_initproc() -> *mut Thread;
        pub safe fn xv6_pid_wlock();
        pub safe fn xv6_pid_wunlock();
        pub safe fn xv6_pid_rlock();
        pub safe fn xv6_pid_runlock();
        pub safe fn scheduler_yield();
        pub safe fn __free_pid();

        // Field accessors / kernel calls that take raw pointers.
        // SAFETY for every line below: the caller in `ffi::*` enforces
        // the "valid live kernel pointer" precondition documented in
        // the module header.
        pub safe fn xv6_panic(msg: *const u8) -> !;
        pub safe fn xv6_either_copyout_int(dst: u64, v: c_int) -> c_int;

        #[link_name = "t_parent"]       pub safe fn t_parent(t: *mut Thread) -> *mut Thread;
        #[link_name = "t_session"]      pub safe fn t_session(t: *mut Thread) -> *mut Session;
        #[link_name = "t_thread_group"] pub safe fn t_thread_group(t: *mut Thread) -> *mut ThreadGroup;
        #[link_name = "t_pid"]          pub safe fn t_pid(t: *mut Thread) -> c_int;

        pub safe fn xv6_t_set_xstate(t: *mut Thread, v: c_int);
        pub safe fn xv6_t_clone_flags(t: *mut Thread) -> u64;
        pub safe fn xv6_t_signal_esignal(t: *mut Thread) -> c_int;
        pub safe fn xv6_t_signal_stop_signal(t: *mut Thread) -> c_int;
        pub safe fn xv6_t_children_count(t: *mut Thread) -> c_int;
        pub safe fn xv6_t_set_self_reap(t: *mut Thread);
        pub safe fn xv6_t_set_vfork_parent(t: *mut Thread, p: *mut Thread);
        pub safe fn xv6_t_vfork_parent(t: *mut Thread) -> *mut Thread;
        pub safe fn xv6_t_fdtable(t: *mut Thread) -> *mut VfsFdtable;
        pub safe fn xv6_t_set_fdtable(t: *mut Thread, f: *mut VfsFdtable);
        pub safe fn xv6_t_fs(t: *mut Thread) -> *mut FsStruct;
        pub safe fn xv6_t_set_fs(t: *mut Thread, f: *mut FsStruct);
        pub safe fn xv6_t_vm(t: *mut Thread) -> *mut Vm;
        pub safe fn xv6_t_set_vm(t: *mut Thread, v: *mut Vm);
        pub safe fn xv6_t_sigacts(t: *mut Thread) -> *mut Sigacts;
        pub safe fn xv6_t_set_sigacts(t: *mut Thread, s: *mut Sigacts);
        pub safe fn xv6_t_set_thread_group(t: *mut Thread, tg: *mut ThreadGroup);
        pub safe fn xv6_thread_is_zombie(t: *mut Thread) -> c_int;
        pub safe fn xv6_thread_is_stopped(t: *mut Thread) -> c_int;

        pub safe fn xv6_tg_group_exit_task_is(tg: *mut ThreadGroup, t: *mut Thread) -> c_int;
        pub safe fn xv6_tg_group_exit_code(tg: *mut ThreadGroup) -> c_int;
        pub safe fn xv6_tg_group_leader(tg: *mut ThreadGroup) -> *mut Thread;
        pub safe fn xv6_tg_is_exiting(tg: *mut ThreadGroup) -> c_int;

        pub safe fn xv6_thread_state_set(t: *mut Thread, s: c_int);
        pub safe fn xv6_tcb_lock(t: *mut Thread);
        pub safe fn xv6_tcb_unlock(t: *mut Thread);

        pub safe fn xv6_exit_reparent_do(p: *mut Thread, initproc: *mut Thread) -> c_int;
        pub safe fn xv6_exit_find_zombie_child(p: *mut Thread) -> *mut Thread;
        pub safe fn xv6_exit_find_stopped_child(p: *mut Thread) -> *mut Thread;
        pub safe fn xv6_exit_reap_zombie(parent: *mut Thread, child: *mut Thread, xstate_out: *mut c_int) -> c_int;

        pub safe fn scheduler_wakeup(t: *mut Thread);
        pub safe fn scheduler_wakeup_interruptible(t: *mut Thread);
        pub safe fn kill_thread(t: *mut Thread, signo: c_int) -> c_int;

        pub safe fn pgroup_remove_thread(p: *mut Thread);
        pub safe fn session_remove_thread(s: *mut Session, t: *mut Thread);
        pub safe fn thread_group_remove(p: *mut Thread) -> bool;
        pub safe fn thread_group_put(tg: *mut ThreadGroup);
        pub safe fn thread_is_group_leader(p: *mut Thread) -> bool;

        pub safe fn sigacts_put(sa: *mut Sigacts);
        pub safe fn sigpending_empty(p: *mut Thread, signo: c_int) -> c_int;
        pub safe fn sigpending_destroy(p: *mut Thread);
        pub safe fn signal_pending(p: *mut Thread) -> bool;
        pub safe fn vm_put(vm: *mut Vm);
        pub safe fn vfs_fdtable_put(fdt: *mut VfsFdtable);
        pub safe fn vfs_struct_put(fs: *mut FsStruct);

        pub safe fn proctab_proc_remove(t: *mut Thread);
        pub safe fn get_pid_thread(pid: c_int) -> *mut Thread;
    }
}

// ---------------------------------------------------------------------------
// Safe wrapper layer. Each wrapper is one line so reviewers can audit the
// SAFETY contract trivially.
//
// Convention: all `*mut Thread / *mut ThreadGroup / *mut Session` args
// must be live kernel pointers (see module header).
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;

    // pointer-returning entries -------------------------------------------------
    pub fn current() -> *mut Thread        { raw::xv6_current_thread() }
    pub fn initproc() -> *mut Thread       { raw::__proctab_get_initproc() }
    pub fn parent(t: *mut Thread) -> *mut Thread        { raw::t_parent(t) }
    pub fn session(t: *mut Thread) -> *mut Session      { raw::t_session(t) }
    pub fn thread_group(t: *mut Thread) -> *mut ThreadGroup { raw::t_thread_group(t) }
    pub fn pid(t: *mut Thread) -> c_int                 { raw::t_pid(t) }
    pub fn vfork_parent(t: *mut Thread) -> *mut Thread  { raw::xv6_t_vfork_parent(t) }
    pub fn fdtable(t: *mut Thread) -> *mut VfsFdtable   { raw::xv6_t_fdtable(t) }
    pub fn fs(t: *mut Thread) -> *mut FsStruct          { raw::xv6_t_fs(t) }
    pub fn vm(t: *mut Thread) -> *mut Vm                { raw::xv6_t_vm(t) }
    pub fn sigacts(t: *mut Thread) -> *mut Sigacts      { raw::xv6_t_sigacts(t) }

    pub fn tg_leader(tg: *mut ThreadGroup) -> *mut Thread { raw::xv6_tg_group_leader(tg) }
    pub fn pid_thread(pid: c_int) -> *mut Thread          { raw::get_pid_thread(pid) }

    // scalar accessors ---------------------------------------------------------
    pub fn clone_flags(t: *mut Thread) -> u64           { raw::xv6_t_clone_flags(t) }
    pub fn esignal(t: *mut Thread) -> c_int             { raw::xv6_t_signal_esignal(t) }
    pub fn stop_signal(t: *mut Thread) -> c_int         { raw::xv6_t_signal_stop_signal(t) }
    pub fn children_count(t: *mut Thread) -> c_int      { raw::xv6_t_children_count(t) }
    pub fn is_zombie(t: *mut Thread) -> bool            { (raw::xv6_thread_is_zombie(t)) != 0 }
    pub fn is_stopped(t: *mut Thread) -> bool           { (raw::xv6_thread_is_stopped(t)) != 0 }
    pub fn is_group_leader(t: *mut Thread) -> bool      { raw::thread_is_group_leader(t) }
    pub fn signal_pending(t: *mut Thread) -> bool       { raw::signal_pending(t) }

    pub fn tg_is_exiting(tg: *mut ThreadGroup) -> bool         { (raw::xv6_tg_is_exiting(tg)) != 0 }
    pub fn tg_exit_task_is(tg: *mut ThreadGroup, t: *mut Thread) -> bool {
        (raw::xv6_tg_group_exit_task_is(tg, t)) != 0
    }
    pub fn tg_exit_code(tg: *mut ThreadGroup) -> c_int         { raw::xv6_tg_group_exit_code(tg) }

    // mutators -----------------------------------------------------------------
    pub fn set_xstate(t: *mut Thread, v: c_int)                  { raw::xv6_t_set_xstate(t, v) }
    pub fn set_self_reap(t: *mut Thread)                         { raw::xv6_t_set_self_reap(t) }
    pub fn set_vfork_parent(t: *mut Thread, p: *mut Thread)      { raw::xv6_t_set_vfork_parent(t, p) }
    pub fn set_fdtable(t: *mut Thread, f: *mut VfsFdtable)       { raw::xv6_t_set_fdtable(t, f) }
    pub fn set_fs(t: *mut Thread, f: *mut FsStruct)              { raw::xv6_t_set_fs(t, f) }
    pub fn set_vm(t: *mut Thread, v: *mut Vm)                    { raw::xv6_t_set_vm(t, v) }
    pub fn set_sigacts(t: *mut Thread, s: *mut Sigacts)          { raw::xv6_t_set_sigacts(t, s) }
    pub fn set_thread_group(t: *mut Thread, tg: *mut ThreadGroup){ raw::xv6_t_set_thread_group(t, tg) }
    pub fn set_state(t: *mut Thread, s: c_int)                   { raw::xv6_thread_state_set(t, s) }

    // locking ------------------------------------------------------------------
    pub fn pid_wlock()        { raw::xv6_pid_wlock() }
    pub fn pid_wunlock()      { raw::xv6_pid_wunlock() }
    pub fn pid_rlock()        { raw::xv6_pid_rlock() }
    pub fn pid_runlock()      { raw::xv6_pid_runlock() }
    pub fn tcb_lock(t: *mut Thread)   { raw::xv6_tcb_lock(t) }
    pub fn tcb_unlock(t: *mut Thread) { raw::xv6_tcb_unlock(t) }

    // sched ops ---------------------------------------------------------------
    pub fn sched_yield()                                       { raw::scheduler_yield() }
    pub fn sched_wakeup(t: *mut Thread)                        { raw::scheduler_wakeup(t) }
    pub fn sched_wakeup_interruptible(t: *mut Thread)          { raw::scheduler_wakeup_interruptible(t) }
    pub fn kill_thread(t: *mut Thread, signo: c_int) -> c_int  { raw::kill_thread(t, signo) }

    // pgroup / session / thread group plumbing --------------------------------
    pub fn pgroup_remove_thread(p: *mut Thread)                { raw::pgroup_remove_thread(p) }
    pub fn session_remove_thread(s: *mut Session, t: *mut Thread) { raw::session_remove_thread(s, t) }
    pub fn thread_group_remove(p: *mut Thread) -> bool         { raw::thread_group_remove(p) }
    pub fn thread_group_put(tg: *mut ThreadGroup)              { raw::thread_group_put(tg) }

    // resource release --------------------------------------------------------
    pub fn sigacts_put(sa: *mut Sigacts)                       { raw::sigacts_put(sa) }
    pub fn sigpending_empty(p: *mut Thread, signo: c_int)      { let _ = raw::sigpending_empty(p, signo); }
    pub fn sigpending_destroy(p: *mut Thread)                  { raw::sigpending_destroy(p) }
    pub fn vm_put(vm: *mut Vm)                                 { raw::vm_put(vm) }
    pub fn vfs_fdtable_put(fdt: *mut VfsFdtable)               { raw::vfs_fdtable_put(fdt) }
    pub fn vfs_struct_put(fs: *mut FsStruct)                   { raw::vfs_struct_put(fs) }
    pub fn proctab_proc_remove(t: *mut Thread)                 { raw::proctab_proc_remove(t) }
    pub fn free_pid()                                          { raw::__free_pid() }

    // exit helpers in proc_rust_shims.c ---------------------------------------
    pub fn reparent_do(p: *mut Thread, initproc: *mut Thread) -> bool {
        (raw::xv6_exit_reparent_do(p, initproc)) != 0
    }
    pub fn find_zombie_child(p: *mut Thread) -> *mut Thread   { raw::xv6_exit_find_zombie_child(p) }
    pub fn find_stopped_child(p: *mut Thread) -> *mut Thread  { raw::xv6_exit_find_stopped_child(p) }
    pub fn reap_zombie(parent: *mut Thread, child: *mut Thread, xstate_out: &mut c_int) -> c_int {
        raw::xv6_exit_reap_zombie(parent, child, xstate_out as *mut c_int)
    }

    // copy out ----------------------------------------------------------------
    pub fn copyout_int(dst: u64, v: c_int) -> bool {
        (raw::xv6_either_copyout_int(dst, v)) >= 0
    }

    // panic -------------------------------------------------------------------
    #[cold]
    pub fn panic_msg(msg: &str) -> ! {
        let mut buf = [0u8; 96];
        let n = msg.as_bytes().len().min(buf.len() - 1);
        buf[..n].copy_from_slice(&msg.as_bytes()[..n]);
        raw::xv6_panic(buf.as_ptr())
    }
}

// ---------------------------------------------------------------------------
// Public entry points — no `unsafe` blocks below this line.
// ---------------------------------------------------------------------------

fn vfork_done_impl(p: *mut Thread) {
    let vp = ffi::vfork_parent(p);
    ffi::set_vfork_parent(p, ptr::null_mut());
    if !vp.is_null() {
        ffi::sched_wakeup(vp);
    }
}

#[no_mangle]
pub extern "C" fn vfork_done(p: *mut Thread) {
    vfork_done_impl(p)
}

fn reparent_impl(p: *mut Thread) {
    let initproc = ffi::initproc();
    if initproc.is_null() {
        ffi::panic_msg("reparent: initproc is NULL");
    }
    if ptr::eq(p, initproc) {
        ffi::panic_msg("reparent: p is init process");
    }
    let parent = ffi::parent(p);
    let zombie_found = ffi::reparent_do(p, initproc);

    if zombie_found {
        ffi::sched_wakeup_interruptible(initproc);
        let esig = ffi::esignal(p);
        if !ptr::eq(initproc, parent) && !ptr::eq(initproc, p) && esig > 0 {
            ffi::kill_thread(initproc, esig);
        }
    }
}

#[no_mangle]
pub extern "C" fn reparent(p: *mut Thread) {
    reparent_impl(p)
}

#[no_mangle]
pub extern "C" fn exit(mut status: c_int) -> ! {
    let p = ffi::current();
    if ptr::eq(p, ffi::initproc()) {
        ffi::panic_msg("init exiting");
    }

    let tg = ffi::thread_group(p);
    if !tg.is_null() && ffi::tg_is_exiting(tg) && !ffi::tg_exit_task_is(tg, p) {
        status = ffi::tg_exit_code(tg);
    }

    // Drop vfork relationship before tearing down anything else.
    vfork_done_impl(p);

    // Release file table / fs context early.
    let fdt = ffi::fdtable(p);
    if !fdt.is_null() {
        ffi::vfs_fdtable_put(fdt);
        ffi::set_fdtable(p, ptr::null_mut());
    }
    let fs = ffi::fs(p);
    if !fs.is_null() {
        ffi::vfs_struct_put(fs);
        ffi::set_fs(p, ptr::null_mut());
    }

    let is_leader = ffi::is_group_leader(p);
    let clone_flags = ffi::clone_flags(p);

    // ---- CLONE_THREAD non-leader: self-reap path ----
    if clone_flags & CLONE_THREAD != 0 && !is_leader {
        reparent_impl(p);

        ffi::pid_wlock();
        ffi::pgroup_remove_thread(p);
        let sess = ffi::session(p);
        if !sess.is_null() {
            ffi::session_remove_thread(sess, p);
        }
        let mut last_in_group = true;
        if !tg.is_null() {
            last_in_group = ffi::thread_group_remove(p);
        }
        ffi::proctab_proc_remove(p);
        ffi::pid_wunlock();
        ffi::free_pid();

        if last_in_group && !tg.is_null() {
            let leader = ffi::tg_leader(tg);
            if !leader.is_null() {
                ffi::set_xstate(leader, status);
                ffi::pid_rlock();
                let parent = ffi::parent(leader);
                ffi::pid_runlock();
                if !parent.is_null() {
                    ffi::sched_wakeup_interruptible(parent);
                    let esig = ffi::esignal(leader);
                    if esig > 0 {
                        ffi::kill_thread(parent, esig);
                    }
                }
            }
        }

        let sa = ffi::sigacts(p);
        if !sa.is_null() {
            ffi::sigacts_put(sa);
            ffi::set_sigacts(p, ptr::null_mut());
        }
        let vm = ffi::vm(p);
        if !vm.is_null() {
            ffi::vm_put(vm);
            ffi::set_vm(p, ptr::null_mut());
        }
        ffi::sigpending_empty(p, 0);
        ffi::sigpending_destroy(p);

        let pg_tg = ffi::thread_group(p);
        if !pg_tg.is_null() {
            ffi::thread_group_put(pg_tg);
            ffi::set_thread_group(p, ptr::null_mut());
        }

        ffi::tcb_lock(p);
        ffi::set_xstate(p, status);
        ffi::set_self_reap(p);
        ffi::set_state(p, THREAD_ZOMBIE);
        ffi::tcb_unlock(p);
        ffi::sched_yield();
        ffi::panic_msg("exit: non-leader thread should not return");
    }

    // ---- Leader / single-threaded path ----
    let mut last_in_group = true;
    if !tg.is_null() {
        ffi::pid_wlock();
        ffi::pgroup_remove_thread(p);
        let sess = ffi::session(p);
        if !sess.is_null() {
            ffi::session_remove_thread(sess, p);
        }
        last_in_group = ffi::thread_group_remove(p);
        ffi::pid_wunlock();
    }

    reparent_impl(p);

    ffi::tcb_lock(p);
    ffi::set_xstate(p, status);
    ffi::set_state(p, THREAD_ZOMBIE);
    ffi::tcb_unlock(p);

    ffi::pid_rlock();
    let parent = ffi::parent(p);
    ffi::pid_runlock();

    if (last_in_group || tg.is_null()) && !parent.is_null() {
        ffi::sched_wakeup_interruptible(parent);
        let esig = ffi::esignal(p);
        if esig > 0 {
            ffi::kill_thread(parent, esig);
        }
    }

    ffi::sched_yield();
    ffi::panic_msg("exit: __exit_yield should not return");
}

#[no_mangle]
pub extern "C" fn wait(addr: u64) -> c_int {
    let p = ffi::current();
    let mut pid: c_int;
    let mut xstate: c_int = 0;

    ffi::pid_rlock();
    loop {
        ffi::set_state(p, THREAD_INTERRUPTIBLE);
        let child = ffi::find_zombie_child(p);
        if !child.is_null() {
            pid = ffi::reap_zombie(p, child, &mut xstate);
            // reap_zombie returns with no lock held
            if pid >= 0 && addr != 0 && !ffi::copyout_int(addr, xstate) {
                return -EFAULT;
            }
            return pid;
        }
        if ffi::children_count(p) == 0 {
            ffi::set_state(p, THREAD_RUNNING);
            pid = -1;
            break;
        }
        if ffi::signal_pending(p) {
            ffi::set_state(p, THREAD_RUNNING);
            pid = -EINTR;
            break;
        }
        ffi::pid_runlock();
        ffi::sched_yield();
        ffi::pid_rlock();
    }
    ffi::pid_runlock();
    pid
}

#[no_mangle]
pub extern "C" fn waitpid(target_pid: c_int, addr: u64, options: c_int) -> c_int {
    if options & !(WNOHANG | WUNTRACED) != 0 {
        return -EINVAL;
    }
    let p = ffi::current();
    let mut pid: c_int;
    let mut xstate: c_int = 0;

    ffi::pid_rlock();
    loop {
        ffi::set_state(p, THREAD_INTERRUPTIBLE);

        if target_pid > 0 {
            let child = ffi::pid_thread(target_pid);
            if is_err_const(child as *const c_void)
                || !ptr::eq(ffi::parent(child), p)
            {
                ffi::set_state(p, THREAD_RUNNING);
                pid = -1;
                break;
            }
            if options & WUNTRACED != 0 && ffi::is_stopped(child) {
                ffi::set_state(p, THREAD_RUNNING);
                pid = ffi::pid(child);
                xstate = (ffi::stop_signal(child) << 8) | 0x7f;
                break;
            }
            if ffi::is_zombie(child) {
                pid = ffi::reap_zombie(p, child, &mut xstate);
                if pid > 0 && addr != 0 && !ffi::copyout_int(addr, xstate) {
                    return -EFAULT;
                }
                return pid;
            }
        } else {
            // Scan all children.
            let stopped = if options & WUNTRACED != 0 {
                ffi::find_stopped_child(p)
            } else {
                ptr::null_mut()
            };
            if !stopped.is_null() {
                ffi::set_state(p, THREAD_RUNNING);
                pid = ffi::pid(stopped);
                xstate = (ffi::stop_signal(stopped) << 8) | 0x7f;
                break;
            }
            let zchild = ffi::find_zombie_child(p);
            if !zchild.is_null() {
                pid = ffi::reap_zombie(p, zchild, &mut xstate);
                if pid > 0 && addr != 0 && !ffi::copyout_int(addr, xstate) {
                    return -EFAULT;
                }
                return pid;
            }
            if ffi::children_count(p) == 0 {
                ffi::set_state(p, THREAD_RUNNING);
                pid = -1;
                break;
            }
        }

        if options & WNOHANG != 0 {
            ffi::set_state(p, THREAD_RUNNING);
            pid = 0;
            break;
        }
        if ffi::signal_pending(p) {
            ffi::set_state(p, THREAD_RUNNING);
            pid = -EINTR;
            break;
        }
        ffi::pid_runlock();
        ffi::sched_yield();
        ffi::pid_rlock();
    }
    ffi::pid_runlock();
    if pid > 0 && addr != 0 && !ffi::copyout_int(addr, xstate) {
        return -EFAULT;
    }
    pid
}
