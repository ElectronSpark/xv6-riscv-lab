//! `kernel/proc/pid.rs` — proc table, PID allocation, and `procdump_*`.
//!
//! Direct port of `kernel/proc/pid.c`. The actual proc_table storage
//! (the hlist, its buckets, the rwlock, initproc, counters, nextpid)
//! lives in `proc_rust_shims.c`'s `__proc_table` static, because it
//! contains _Atomic int64 and an embedded rwlock which bindgen cannot
//! model. All access goes through `xv6_proctab_*` and `xv6_pid_*`
//! C accessor shims.
//!
//! ## NO-STANDALONE-FN
//!
//! The proc-table / pid-allocator surface is exposed as associated
//! functions on two marker types instead of free functions:
//!   * [`ProcTable`] — the process table + `pid_lock` + initproc + the
//!     `procdump` orchestrator (lock ops, table membership, lookups).
//!   * [`Pid`] — the PID-number allocator (`alloc`/`free`).
//! Every call site names `ProcTable::<op>` / `Pid::<op>`.

#![allow(non_camel_case_types)]
#![allow(dead_code)]

use core::ffi::c_void;
use core::ptr;

use crate::proc::proc_shims;

// P3-D1a: formerly opaque `[u8; 0]` markers; now the real `crate::bindings`
// types so the direct Rust calls into `proc_shims` type-check unchanged.
pub type Thread = crate::bindings::thread;
pub type Session = crate::bindings::session;

// errno values used here.
const ESRCH:  i32 = 3;
const EAGAIN: i32 = 11;

// thread_state values used here.
const THREAD_UNUSED: i32 = 0;

const MAXPID: i32 = 0x7FFFFFF0;

// P3-D1a: the `xv6_*`/`t_*`/`session_*` accessor shims are ordinary Rust fns
// in `proc_shims.rs`; call them via crate paths instead of `extern "C"`
// redeclarations. Call sites keep the same bare names.
use crate::proc::proc_shims::{
    session_for_each_all, session_sid, t_dmp_list_entry_is_detached, t_pid, t_session, t_set_pid,
    xv6_dump_session, xv6_is_err, xv6_panic, xv6_pid_rlock, xv6_pid_runlock,
    xv6_pid_wholding, xv6_pid_wlock, xv6_pid_wunlock,
    xv6_procdump_bt_footer, xv6_procdump_bt_header, xv6_procdump_bt_one, xv6_procdump_bt_pid,
    xv6_procdump_header, xv6_procdump_one, xv6_procdump_tree_node, xv6_procdump_tree_recursive,
    xv6_proctab_alloc_pid_slot, xv6_proctab_allocated_cnt, xv6_proctab_dmplist_add,
    xv6_proctab_dmplist_del, xv6_proctab_free_pid_slot, xv6_proctab_get_locked,
    xv6_proctab_get_rcu, xv6_proctab_init_storage, xv6_proctab_initproc_load,
    xv6_proctab_initproc_raw, xv6_proctab_initproc_store, xv6_proctab_nextpid_get,
    xv6_proctab_nextpid_set, xv6_proctab_pop_rcu, xv6_proctab_put_rcu,
    xv6_proctab_registered_dec, xv6_proctab_registered_inc,
};

// P3-D3c: `irq/syscall.rs`'s `argint` is a plain (safe) Rust fn now that
// its `#[no_mangle]` export is gone.
use crate::irq::syscall::argint;

/// Marker type for the process table + `pid_lock` + initproc + `procdump`.
/// All operations are associated functions (NO-STANDALONE-FN); it is never
/// instantiated.
pub(crate) struct ProcTable;

/// Marker type for the PID-number allocator. `Pid::alloc`/`Pid::free`
/// reserve/release a slot in the global allocator (NO-STANDALONE-FN); never
/// instantiated.
pub(crate) struct Pid;

#[cold]
fn panic_pid(msg: &str) -> ! {
    let mut buf = [0u8; 96];
    let n = msg.as_bytes().len().min(buf.len() - 1);
    buf[..n].copy_from_slice(&msg.as_bytes()[..n]);
    xv6_panic(buf.as_ptr())
}

impl ProcTable {
    // -----------------------------------------------------------------------
    // init
    // -----------------------------------------------------------------------
    // P3-1B: only caller is `proc/thread.rs::thread_init`.
    pub(crate) fn init() {
        xv6_proctab_init_storage();
    }

    // -----------------------------------------------------------------------
    // pid lock surface
    // -----------------------------------------------------------------------
    pub(crate) fn wlock()   { xv6_pid_wlock(); }
    pub(crate) fn wunlock() { xv6_pid_wunlock(); }

    pub(crate) fn assert_wholding() {
        if xv6_pid_wholding() == 0 {
            panic_pid("pid lock not held");
        }
    }

    // -----------------------------------------------------------------------
    // initproc set / get
    // -----------------------------------------------------------------------
    // P3-1B: only caller is `proc/thread.rs`.
    pub(crate) fn set_initproc(p: *mut Thread) {
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

    pub(crate) fn get_initproc() -> *mut Thread {
        if xv6_proctab_initproc_raw().is_null() {
            panic_pid("initproc not set");
        }
        xv6_proctab_initproc_load()
    }

    // -----------------------------------------------------------------------
    // proctab_proc_add / proctab_proc_remove / get_thread
    // -----------------------------------------------------------------------
    // P3-1B: callers are `proc/thread.rs` and `proc/clone.rs`.
    pub(crate) fn proc_add(p: *mut Thread) {
        if xv6_pid_wholding() == 0 {
            panic_pid("pid lock not held");
        }
        if p.is_null() {
            panic_pid("NULL proc passed to proctab_proc_add");
        }
        if t_dmp_list_entry_is_detached(p) == 0 {
            panic_pid("Process is already in the dump list");
        }

        // Find an unused PID number: scan candidate PIDs starting at the
        // current `nextpid`, wrapping at MAXPID back to 2, until a free slot is
        // found. `pid_lock` is held throughout this fn.
        let start = xv6_proctab_nextpid_get();
        let newpid = core::iter::successors(Some(start), |&pid| {
            let np = pid.wrapping_add(1);
            let np = if np >= MAXPID { 2 } else { np };
            (np != start).then_some(np)
        })
        .find(|&pid| xv6_proctab_get_locked(pid).is_null())
        .unwrap_or_else(|| panic_pid("proctab_proc_add: no free PID (should not happen)"));
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
        // N-R6d-1: register in the generational `THREAD_TABLE`.
        if crate::proc::thread::THREAD_TABLE
            .insert(p as *mut c_void as *mut crate::bindings::thread)
            .is_none()
        {
            panic_pid("thread_table_insert: registry full (should not happen)");
        }
    }

    pub(crate) fn get_thread(pid: i32) -> *mut Thread {
        let p = xv6_proctab_get_rcu(pid);
        if p.is_null() {
            // ERR_PTR(-ESRCH) = (void *)(long)(-ESRCH)
            return (-(ESRCH as isize)) as *mut Thread;
        }
        p
    }

    // P3-1B: callers are `proc/exit.rs` and `proc/proc_shims.rs`.
    pub(crate) fn proc_remove(p: *mut Thread) {
        if xv6_pid_wholding() == 0 {
            panic_pid("pid lock not held");
        }
        let existing = xv6_proctab_pop_rcu(p);
        xv6_proctab_dmplist_del(p);
        xv6_proctab_registered_dec();
        // N-R6d-1: deregister from the generational `THREAD_TABLE`.
        crate::proc::thread::THREAD_TABLE.remove(p as *mut c_void as *mut crate::bindings::thread);
        if !existing.is_null() && existing != p {
            panic_pid("thread_destroy called with a different proc");
        }
    }

    // -----------------------------------------------------------------------
    // procdump (orchestrates; C shim prints each row).
    // -----------------------------------------------------------------------
    pub(crate) fn dump() {
        xv6_procdump_header();
        proc_shims::for_each_proctab_thread(|p| {
            let _ = xv6_procdump_one(p as *mut Thread);
        });
    }
}

impl Pid {
    // P3-1B: callers are `proc/thread.rs` and `proc/clone.rs`.
    pub(crate) fn alloc() -> i32 {
        xv6_proctab_alloc_pid_slot()
    }

    // P3-1B: callers are `proc/clone.rs`, `proc/thread.rs`, `proc/exit.rs`,
    // and `proc/proc_shims.rs`.
    pub(crate) fn free() {
        if xv6_proctab_allocated_cnt() <= 0 {
            panic_pid("__free_pid: allocated_cnt underflow");
        }
        xv6_proctab_free_pid_slot();
    }
}

#[inline]
fn nextpid_advance(pid: i32) {
    let mut np = pid.wrapping_add(1);
    if np >= MAXPID {
        np = 2;
    }
    xv6_proctab_nextpid_set(np);
}

// ---------------------------------------------------------------------------
// procdump variants (private helpers of the `sys_dumpproc` dispatch fn-ptr).
// GENUINE FLOOR: reached only through the `sys_dumpproc` extern "C" entry that
// the syscall dispatch table stores by address; kept as private free helpers
// (leaf print orchestrators, no domain receiver).
// ---------------------------------------------------------------------------

fn procdump_bt() {
    xv6_procdump_bt_header();
    proc_shims::for_each_proctab_thread(|p| {
        xv6_procdump_bt_one(p as *mut Thread);
    });
    xv6_procdump_bt_footer();
}

fn procdump_bt_pid(pid: i32) {
    xv6_procdump_bt_pid(pid);
}

fn procdump_tree() {
    crate::kprintln!("Process Tree:");
    xv6_pid_rlock();
    let initproc = xv6_proctab_initproc_raw();
    if initproc.is_null() {
        crate::kprintln!("No init process");
        xv6_pid_runlock();
        return;
    }
    xv6_procdump_tree_recursive(initproc, 0);
    xv6_pid_runlock();
}

fn procdump_tree_pid(target_pid: i32) {
    crate::kprintln!("Process Tree (from pid {}):", target_pid);
    xv6_pid_rlock();
    let p = xv6_proctab_get_locked(target_pid);
    if p.is_null() {
        crate::kprintln!("Process {} not found", target_pid);
        xv6_pid_runlock();
        return;
    }
    xv6_procdump_tree_recursive(p, 0);
    xv6_pid_runlock();
}

unsafe extern "C" fn dump_session_cb(s: *mut Session, _arg: *mut c_void) {
    xv6_dump_session(s);
}

fn procdump_sessions() {
    crate::kprintln!("\n=== Process Hierarchy (Session / PGroup / Process / Thread) ===");
    xv6_pid_rlock();
    session_for_each_all(Some(dump_session_cb), ptr::null_mut());
    xv6_pid_runlock();
    crate::kprintln!("\n=== End Hierarchy ===");
}

fn procdump_sessions_sid(target_sid: i32) {
    crate::kprintln!("\n=== Session {} Hierarchy ===", target_sid);
    xv6_pid_rlock();
    let leader = ProcTable::get_thread(target_sid);
    let bad = xv6_is_err(leader as *const c_void) != 0 || {
        let s = t_session(leader);
        s.is_null() || session_sid(s) != target_sid
    };
    if bad {
        crate::kprintln!("Session {} not found", target_sid);
        xv6_pid_runlock();
        crate::kprintln!("\n=== End Hierarchy ===");
        return;
    }
    xv6_dump_session(t_session(leader));
    xv6_pid_runlock();
    crate::kprintln!("\n=== End Hierarchy ===");
}

// ---------------------------------------------------------------------------
// sys_dumpproc syscall — GENUINE FLOOR: stored by address in `irq/syscall.rs`'s
// dispatch table (`extern "C"` fn-ptr), so it cannot be an associated fn.
// ---------------------------------------------------------------------------

pub(crate) extern "C" fn sys_dumpproc() -> u64 {
    let mut mode: i32 = 0;
    let mut id: i32 = 0;
    argint(0, &mut mode);
    argint(1, &mut id);
    let target = if id >= 0 { Some(id) } else { None };
    match (mode, target) {
        (1, Some(pid)) => procdump_tree_pid(pid),
        (1, None)      => procdump_tree(),
        (2, Some(sid)) => procdump_sessions_sid(sid),
        (2, None)      => procdump_sessions(),
        (3, Some(pid)) => procdump_bt_pid(pid),
        (3, None)      => procdump_bt(),
        _              => ProcTable::dump(),
    }
    0
}
