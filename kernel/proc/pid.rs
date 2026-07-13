//! `kernel/proc/pid.rs` — proc table, PID allocation, and `procdump_*`.
//!
//! Direct port of `kernel/proc/pid.c`. The actual proc_table storage
//! (the hlist, its buckets, the rwlock, initproc, counters, nextpid)
//! lives in `proc_rust_shims.c`'s `__proc_table` static, because it
//! contains _Atomic int64 and an embedded rwlock which bindgen cannot
//! model. All access goes through `xv6_proctab_*` and `xv6_pid_*`
//! C accessor shims.

#![allow(non_camel_case_types)]
#![allow(dead_code)]

use core::ffi::c_void;
use core::ptr;

use crate::proc::proc_shims;

#[repr(C)] pub struct Thread { _p: [u8; 0] }
#[repr(C)] pub struct Session { _p: [u8; 0] }

// errno values used here.
const ESRCH:  i32 = 3;
const EAGAIN: i32 = 11;

// thread_state values used here.
const THREAD_UNUSED: i32 = 0;

const MAXPID: i32 = 0x7FFFFFF0;

unsafe extern "C" {
    // pid lock & assertions.
    pub safe fn xv6_pid_wlock();
    pub safe fn xv6_pid_wunlock();
    pub safe fn xv6_pid_rlock();
    pub safe fn xv6_pid_runlock();
    pub safe fn xv6_pid_wholding() -> i32;
    pub safe fn xv6_pid_try_lock_upgrade() -> i32;

    // proc_table storage init.
    pub safe fn xv6_proctab_init_storage();

    // initproc.
    pub safe fn xv6_proctab_initproc_load() -> *mut Thread;
    pub safe fn xv6_proctab_initproc_store(p: *mut Thread);
    pub safe fn xv6_proctab_initproc_raw() -> *mut Thread;

    // PID slot reservation.
    pub safe fn xv6_proctab_alloc_pid_slot() -> i32;
    pub safe fn xv6_proctab_free_pid_slot();
    pub safe fn xv6_proctab_allocated_cnt() -> i64;

    // nextpid + registered_cnt.
    pub safe fn xv6_proctab_nextpid_get() -> i32;
    pub safe fn xv6_proctab_nextpid_set(v: i32);
    pub safe fn xv6_proctab_registered_inc();
    pub safe fn xv6_proctab_registered_dec();

    // hlist ops on proc_table.procs.
    pub safe fn xv6_proctab_get_locked(pid: i32) -> *mut Thread;
    pub safe fn xv6_proctab_get_rcu(pid: i32) -> *mut Thread;
    pub safe fn xv6_proctab_put_rcu(p: *mut Thread) -> *mut Thread;
    pub safe fn xv6_proctab_pop_rcu(p: *mut Thread) -> *mut Thread;

    // dump list.
    pub safe fn xv6_proctab_dmplist_add(p: *mut Thread);
    pub safe fn xv6_proctab_dmplist_del(p: *mut Thread);

    // RCU + thread shims.
    pub safe fn xv6_rcu_read_lock();
    pub safe fn xv6_rcu_read_unlock();

    // Print helpers.
    pub safe fn xv6_procdump_header();
    pub safe fn xv6_procdump_one(p: *mut Thread) -> i32;
    pub safe fn xv6_procdump_bt_header();
    pub safe fn xv6_procdump_bt_footer();
    pub safe fn xv6_procdump_bt_one(p: *mut Thread);
    pub safe fn xv6_procdump_bt_pid(pid: i32);
    pub safe fn xv6_procdump_tree_node(p: *mut Thread, depth: i32);
    pub safe fn xv6_procdump_tree_recursive(p: *mut Thread, depth: i32);
    pub safe fn xv6_dump_session(s: *mut Session);
    pub safe fn xv6_print_str(s: *const u8);
    pub safe fn xv6_print_str_d(s: *const u8, v: i32);

    // session iteration.
    pub safe fn session_for_each_all(
        fnp: unsafe extern "C" fn(*mut Session, *mut c_void),
        arg: *mut c_void,
    );

    // dmp_list_entry detached predicate.
    pub safe fn t_dmp_list_entry_is_detached(t: *mut Thread) -> i32;

    // misc thread accessors.
    pub safe fn t_pid(p: *mut Thread) -> i32;
    pub safe fn t_set_pid(p: *mut Thread, pid: i32);
    pub safe fn t_session(p: *mut Thread) -> *mut Session;
    pub safe fn session_sid(s: *mut Session) -> i32;
    pub safe fn xv6_is_err(p: *const c_void) -> i32;

    // syscall arg helper.
    pub safe fn argint(n: i32, ip: *mut i32) -> i32;

    pub safe fn xv6_panic(msg: *const u8) -> !;
}

#[cold]
fn panic_pid(msg: &str) -> ! {
    let mut buf = [0u8; 96];
    let n = msg.as_bytes().len().min(buf.len() - 1);
    buf[..n].copy_from_slice(&msg.as_bytes()[..n]);
    xv6_panic(buf.as_ptr())
}

// ---------------------------------------------------------------------------
// __proctab_init
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn __proctab_init() {
    xv6_proctab_init_storage();
}

// ---------------------------------------------------------------------------
// pid lock public surface (callable from other C code).
// ---------------------------------------------------------------------------

#[no_mangle] pub extern "C" fn pid_wlock()   { xv6_pid_wlock(); }
#[no_mangle] pub extern "C" fn pid_wunlock() { xv6_pid_wunlock(); }
#[no_mangle] pub extern "C" fn pid_rlock()   { xv6_pid_rlock(); }
#[no_mangle] pub extern "C" fn pid_runlock() { xv6_pid_runlock(); }

#[no_mangle]
pub extern "C" fn pid_try_lock_upgrade() -> bool {
    xv6_pid_try_lock_upgrade() != 0
}
#[no_mangle]
pub extern "C" fn pid_wholding() -> bool {
    xv6_pid_wholding() != 0
}
#[no_mangle]
pub extern "C" fn pid_assert_wholding() {
    if xv6_pid_wholding() == 0 {
        panic_pid("pid lock not held");
    }
}

// ---------------------------------------------------------------------------
// initproc set / get
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn __proctab_set_initproc(p: *mut Thread) {
    xv6_pid_wlock();
    if p.is_null() {
        panic_pid("NULL initproc");
    }
    if !xv6_proctab_initproc_raw().is_null() {
        panic_pid("initproc already set");
    }
    xv6_proctab_initproc_store(p);
    xv6_pid_wunlock();
}

#[no_mangle]
pub extern "C" fn __proctab_get_initproc() -> *mut Thread {
    if xv6_proctab_initproc_raw().is_null() {
        panic_pid("initproc not set");
    }
    xv6_proctab_initproc_load()
}

// ---------------------------------------------------------------------------
// __alloc_pid / __free_pid
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn __alloc_pid() -> i32 {
    xv6_proctab_alloc_pid_slot()
}

#[no_mangle]
pub extern "C" fn __free_pid() {
    if xv6_proctab_allocated_cnt() <= 0 {
        panic_pid("__free_pid: allocated_cnt underflow");
    }
    xv6_proctab_free_pid_slot();
}

// ---------------------------------------------------------------------------
// proctab_proc_add / proctab_proc_remove / get_pid_thread
// ---------------------------------------------------------------------------

#[inline]
fn nextpid_advance(pid: i32) {
    let mut np = pid.wrapping_add(1);
    if np >= MAXPID {
        np = 2;
    }
    xv6_proctab_nextpid_set(np);
}

#[no_mangle]
pub extern "C" fn proctab_proc_add(p: *mut Thread) {
    if xv6_pid_wholding() == 0 {
        panic_pid("pid lock not held");
    }
    if p.is_null() {
        panic_pid("NULL proc passed to proctab_proc_add");
    }
    if t_dmp_list_entry_is_detached(p) == 0 {
        panic_pid("Process is already in the dump list");
    }

    // Find an unused PID number.
    let start = xv6_proctab_nextpid_get();
    while !xv6_proctab_get_locked(xv6_proctab_nextpid_get()).is_null() {
        nextpid_advance(xv6_proctab_nextpid_get());
        if xv6_proctab_nextpid_get() == start {
            panic_pid("proctab_proc_add: no free PID (should not happen)");
        }
    }
    let newpid = xv6_proctab_nextpid_get();
    t_set_pid(p, newpid);
    nextpid_advance(newpid);

    let existing = xv6_proctab_put_rcu(p);
    if existing == p {
        panic_pid("Failed to add process");
    }
    if !existing.is_null() {
        panic_pid("Process with that pid already exists");
    }
    xv6_proctab_dmplist_add(p);
    xv6_proctab_registered_inc();
}

#[no_mangle]
pub extern "C" fn get_pid_thread(pid: i32) -> *mut Thread {
    let p = xv6_proctab_get_rcu(pid);
    if p.is_null() {
        // ERR_PTR(-ESRCH) = (void *)(long)(-ESRCH)
        return (-(ESRCH as isize)) as *mut Thread;
    }
    p
}

#[no_mangle]
pub extern "C" fn proctab_proc_remove(p: *mut Thread) {
    if xv6_pid_wholding() == 0 {
        panic_pid("pid lock not held");
    }
    let existing = xv6_proctab_pop_rcu(p);
    xv6_proctab_dmplist_del(p);
    xv6_proctab_registered_dec();
    if !existing.is_null() && existing != p {
        panic_pid("thread_destroy called with a different proc");
    }
}

// ---------------------------------------------------------------------------
// procdump (orchestrates; C shim prints each row).
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn procdump() {
    xv6_procdump_header();
    proc_shims::for_each_proctab_thread(|p| {
        let _ = xv6_procdump_one(p as *mut Thread);
    });
}

#[no_mangle]
pub extern "C" fn procdump_bt() {
    xv6_procdump_bt_header();
    proc_shims::for_each_proctab_thread(|p| {
        xv6_procdump_bt_one(p as *mut Thread);
    });
    xv6_procdump_bt_footer();
}

#[no_mangle]
pub extern "C" fn procdump_bt_pid(pid: i32) {
    xv6_procdump_bt_pid(pid);
}

// ---------------------------------------------------------------------------
// procdump_tree / procdump_tree_pid
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn procdump_tree() {
    xv6_print_str(b"Process Tree:\n\0".as_ptr());
    xv6_pid_rlock();
    let initproc = xv6_proctab_initproc_raw();
    if initproc.is_null() {
        xv6_print_str(b"No init process\n\0".as_ptr());
        xv6_pid_runlock();
        return;
    }
    xv6_procdump_tree_recursive(initproc, 0);
    xv6_pid_runlock();
}

#[no_mangle]
pub extern "C" fn procdump_tree_pid(target_pid: i32) {
    xv6_print_str_d(b"Process Tree (from pid %d):\n\0".as_ptr(), target_pid);
    xv6_pid_rlock();
    let p = xv6_proctab_get_locked(target_pid);
    if p.is_null() {
        xv6_print_str_d(b"Process %d not found\n\0".as_ptr(), target_pid);
        xv6_pid_runlock();
        return;
    }
    xv6_procdump_tree_recursive(p, 0);
    xv6_pid_runlock();
}

// ---------------------------------------------------------------------------
// procdump_sessions / procdump_sessions_sid
// ---------------------------------------------------------------------------

unsafe extern "C" fn dump_session_cb(s: *mut Session, _arg: *mut c_void) {
    xv6_dump_session(s);
}

#[no_mangle]
pub extern "C" fn procdump_sessions() {
    xv6_print_str(
        b"\n=== Process Hierarchy (Session / PGroup / Process / Thread) ===\n\0"
            .as_ptr(),
    );
    xv6_pid_rlock();
    session_for_each_all(dump_session_cb, ptr::null_mut());
    xv6_pid_runlock();
    xv6_print_str(b"\n=== End Hierarchy ===\n\0".as_ptr());
}

#[no_mangle]
pub extern "C" fn procdump_sessions_sid(target_sid: i32) {
    xv6_print_str_d(b"\n=== Session %d Hierarchy ===\n\0".as_ptr(), target_sid);
    xv6_pid_rlock();
    let leader = get_pid_thread(target_sid);
    let bad = xv6_is_err(leader as *const c_void) != 0 || {
        let s = t_session(leader);
        s.is_null() || session_sid(s) != target_sid
    };
    if bad {
        xv6_print_str_d(b"Session %d not found\n\0".as_ptr(), target_sid);
        xv6_pid_runlock();
        xv6_print_str(b"\n=== End Hierarchy ===\n\0".as_ptr());
        return;
    }
    xv6_dump_session(t_session(leader));
    xv6_pid_runlock();
    xv6_print_str(b"\n=== End Hierarchy ===\n\0".as_ptr());
}

// ---------------------------------------------------------------------------
// sys_dumpproc syscall
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn sys_dumpproc() -> u64 {
    let mut mode: i32 = 0;
    let mut id: i32 = 0;
    let _ = argint(0, &mut mode);
    let _ = argint(1, &mut id);
    let target = if id >= 0 { Some(id) } else { None };
    match (mode, target) {
        (1, Some(pid)) => procdump_tree_pid(pid),
        (1, None)      => procdump_tree(),
        (2, Some(sid)) => procdump_sessions_sid(sid),
        (2, None)      => procdump_sessions(),
        (3, Some(pid)) => procdump_bt_pid(pid),
        (3, None)      => procdump_bt(),
        _              => procdump(),
    }
    0
}

// ---------------------------------------------------------------------------
// proctab_for_each_rcu — kept as a callback-taking `extern "C"` entry point
// for any (currently nonexistent, but ABI-preserved) external C caller;
// internally it now drives the safe `for_each_proctab_thread` iterator
// instead of the old `xv6_proctab_foreach_rcu` C-callback trampoline.
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn proctab_for_each_rcu(
    fnp: unsafe extern "C" fn(*mut Thread, *mut c_void),
    arg: *mut c_void,
) {
    proc_shims::for_each_proctab_thread(|p| {
        // SAFETY: `fnp` is a valid C callback per this function's own
        // (unsafe) contract; `arg` is caller-provided and opaque to us.
        unsafe { fnp(p as *mut Thread, arg) };
    });
}
