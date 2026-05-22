//! `xv6_sigport_*` C-ABI aliases for [`crate::proc::signal`] functions.
//!
//! Kept in a dedicated module so `signal.rs` stays focused on the logic.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

use core::ffi::{c_int, c_void};

use crate::bindings::{
    ksiginfo as ksiginfo_t, sigacts as sigacts_t, sigaction as sigaction_t, sigset_t,
    stack as stack_t, thread, thread_signal_t,
};
use crate::proc::signal::*;

macro_rules! unsafe_signal_call {
    ($e:expr) => {{ unsafe { $e } }};
}

macro_rules! sigport_alias {
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* ) -> $ret:ty) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) -> $ret { $target($($pn),*) }
    };
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* )) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) { $target($($pn),*) }
    };
}

macro_rules! sigport_unsafe_alias {
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* ) -> $ret:ty) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) -> $ret { unsafe_signal_call!($target($($pn),*)) }
    };
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* )) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) { unsafe_signal_call!($target($($pn),*)) }
    };
}

sigport_alias!(xv6_sigport_sigacts_lock => sigacts_lock(sa: *mut sigacts_t));
sigport_alias!(xv6_sigport_sigacts_unlock => sigacts_unlock(sa: *mut sigacts_t));
sigport_alias!(xv6_sigport_sigacts_holding => sigacts_holding(sa: *mut sigacts_t) -> c_int);
sigport_unsafe_alias!(xv6_sigport_ksiginfo_free => ksiginfo_free(ksi: *mut ksiginfo_t));
sigport_alias!(xv6_sigport_recalc_sigpending_tsk => recalc_sigpending_tsk(p: *mut thread) -> bool);
sigport_alias!(xv6_sigport_recalc_sigpending => recalc_sigpending());
sigport_alias!(xv6_sigport_signal_pending => signal_pending(p: *mut thread) -> bool);
sigport_unsafe_alias!(xv6_sigport_signal_notify => signal_notify(p: *mut thread) -> c_int);
sigport_alias!(xv6_sigport_signal_terminated => signal_terminated(p: *mut thread) -> bool);
sigport_unsafe_alias!(xv6_sigport_signal_test_clear_stopped => signal_test_clear_stopped(p: *mut thread) -> bool);
sigport_unsafe_alias!(xv6_sigport_signal_restore => signal_restore(p: *mut thread, ctx: *mut c_void) -> c_int);
sigport_unsafe_alias!(xv6_sigport___signal_send => __signal_send(p: *mut thread, info: *mut ksiginfo_t) -> c_int);
sigport_unsafe_alias!(xv6_sigport_signal_send => signal_send(pid: c_int, info: *mut ksiginfo_t) -> c_int);
sigport_unsafe_alias!(xv6_sigport_kill_proc => kill_proc(p: *mut thread, signum: c_int) -> c_int);
sigport_alias!(xv6_sigport_kill => kill(pid: c_int, signum: c_int) -> c_int);
sigport_unsafe_alias!(xv6_sigport_signal_init => signal_init());
sigport_alias!(xv6_sigport_sigpending_init => sigpending_init(p: *mut thread));
sigport_alias!(xv6_sigport_sigpending_destroy => sigpending_destroy(p: *mut thread));
sigport_alias!(xv6_sigport_sigpending_clone => sigpending_clone(dst: *mut thread_signal_t, src: *mut thread_signal_t, cf: u64, esig: c_int));
sigport_unsafe_alias!(xv6_sigport_sigstack_init => sigstack_init(stack: *mut stack_t));
sigport_unsafe_alias!(xv6_sigport_sigpending_empty => sigpending_empty(p: *mut thread, signo: c_int) -> c_int);
sigport_unsafe_alias!(xv6_sigport_sigacts_exec => sigacts_exec(sa: *mut sigacts_t));
sigport_unsafe_alias!(xv6_sigport_sigacts_put => sigacts_put(sa: *mut sigacts_t));
sigport_alias!(xv6_sigport_signal_pending_locked => signal_pending_locked(p: *mut thread, sa: *mut sigacts_t) -> bool);
sigport_unsafe_alias!(xv6_sigport_sigaction => sigaction(signum: c_int, act: *mut sigaction_t, oldact: *mut sigaction_t) -> c_int);
sigport_unsafe_alias!(xv6_sigport_sigprocmask => sigprocmask(how: c_int, set: *const sigset_t, oldset: *mut sigset_t) -> c_int);
sigport_alias!(xv6_sigport_sigpending => sigpending(p: *mut thread, set: *mut sigset_t) -> c_int);
sigport_unsafe_alias!(xv6_sigport_sigreturn => sigreturn() -> c_int);
sigport_unsafe_alias!(xv6_sigport_handle_signal => handle_signal());
sigport_unsafe_alias!(xv6_sigport_kill_thread => kill_thread(p: *mut thread, signum: c_int) -> c_int);
sigport_alias!(xv6_sigport_tgkill => tgkill(tgid: c_int, tid: c_int, signum: c_int) -> c_int);
sigport_alias!(xv6_sigport_tkill => tkill(tid: c_int, signum: c_int) -> c_int);
sigport_alias!(xv6_sigport_killed => killed(p: *mut thread) -> c_int);
sigport_alias!(xv6_sigport_kill_from_kernel => kill_from_kernel(pid: c_int, signum: c_int) -> c_int);
sigport_unsafe_alias!(xv6_sigport_sigsuspend => sigsuspend(mask: *const sigset_t) -> c_int);
sigport_unsafe_alias!(xv6_sigport_sigwait => sigwait(set: *const sigset_t, sig: *mut c_int) -> c_int);
