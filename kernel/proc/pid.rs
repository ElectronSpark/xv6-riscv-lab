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
    xv6_pid_try_lock_upgrade, xv6_pid_wholding, xv6_pid_wlock, xv6_pid_wunlock,
    xv6_procdump_bt_footer, xv6_procdump_bt_header, xv6_procdump_bt_one, xv6_procdump_bt_pid,
    xv6_procdump_header, xv6_procdump_one, xv6_procdump_tree_node, xv6_procdump_tree_recursive,
    xv6_proctab_alloc_pid_slot, xv6_proctab_allocated_cnt, xv6_proctab_dmplist_add,
    xv6_proctab_dmplist_del, xv6_proctab_free_pid_slot, xv6_proctab_get_locked,
    xv6_proctab_get_rcu, xv6_proctab_init_storage, xv6_proctab_initproc_load,
    xv6_proctab_initproc_raw, xv6_proctab_initproc_store, xv6_proctab_nextpid_get,
    xv6_proctab_nextpid_set, xv6_proctab_pop_rcu, xv6_proctab_put_rcu,
    xv6_proctab_registered_dec, xv6_proctab_registered_inc, xv6_rcu_read_lock,
    xv6_rcu_read_unlock,
};

// P3-D3c: `irq/syscall.rs`'s `argint` is a plain (safe) Rust fn now that
// its `#[no_mangle]` export is gone. ABI-truth note: this file's old
// redeclaration claimed `-> i32`; the real fn returns `()` -- the C ABI
// silently let the `let _ =` call sites read a stale return register.
// Unified to the real signature (both call sites drop their `let _ =`).
use crate::irq::syscall::argint;

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

// P3-1B: only caller is `proc/thread.rs::thread_init` (already a direct
// crate-path `use`) -- demoted.
// RUSTIFY-PROC: in-`crate::proc` caller only (thread.rs) -> pub(super).
pub(super) extern "C" fn __proctab_init() {
    xv6_proctab_init_storage();
}

// ---------------------------------------------------------------------------
// pid lock public surface (callable from other C code).
// ---------------------------------------------------------------------------

pub(crate) fn pid_wlock()   { xv6_pid_wlock(); }
pub(crate) fn pid_wunlock() { xv6_pid_wunlock(); }
// P3-D2b: the `pid_rlock`/`pid_runlock` wrappers deleted -- zero callers
// anywhere in the tree (proc/exit.rs's `ffi::pid_rlock`/`ffi::pid_runlock`
// wrap `proc_shims::xv6_pid_rlock`/`xv6_pid_runlock` directly, and the
// kernel/inc header prototypes do not count, 9d35f95 precedent).

// P3-1B: zero callers anywhere in the tree (grep-verified). Demoted from
// `#[no_mangle]`; the file's blanket `#![allow(dead_code)]` (line 11)
// already covers these, no per-item attribute needed.
// RUSTIFY-PROC: zero callers anywhere -> private (file `#![allow(dead_code)]`).
extern "C" fn pid_try_lock_upgrade() -> bool {
    xv6_pid_try_lock_upgrade() != 0
}
extern "C" fn pid_wholding() -> bool {
    xv6_pid_wholding() != 0
}
pub(crate) fn pid_assert_wholding() {
    if xv6_pid_wholding() == 0 {
        panic_pid("pid lock not held");
    }
}

// ---------------------------------------------------------------------------
// initproc set / get
// ---------------------------------------------------------------------------

// P3-1B: only caller is `proc/thread.rs` (crate-path `use`, casting its
// own `*mut bindings::thread` to this file's `pub` opaque `Thread` marker
// -- same precedent as `sysproc.rs`'s `thread_clone` wrapper) -- demoted.
// RUSTIFY-PROC: in-`crate::proc` caller only (thread.rs) -> pub(super).
pub(super) extern "C" fn __proctab_set_initproc(p: *mut Thread) {
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

pub(crate) fn __proctab_get_initproc() -> *mut Thread {
    if xv6_proctab_initproc_raw().is_null() {
        panic_pid("initproc not set");
    }
    xv6_proctab_initproc_load()
}

// ---------------------------------------------------------------------------
// __alloc_pid / __free_pid
// ---------------------------------------------------------------------------

// P3-1B: callers are `proc/thread.rs` and `proc/clone.rs` (both
// crate-path `use`, not `extern` redeclarations) -- demoted.
// RUSTIFY-PROC: in-`crate::proc` callers only (thread.rs, clone.rs) -> pub(super).
pub(super) extern "C" fn __alloc_pid() -> i32 {
    xv6_proctab_alloc_pid_slot()
}

// P3-1B: callers are `proc/clone.rs`, `proc/thread.rs`, `proc/exit.rs`,
// and `proc/proc_shims.rs` (all crate-path `use`, not `extern`
// redeclarations) -- demoted.
// RUSTIFY-PROC: in-`crate::proc` callers only (thread.rs, clone.rs, exit.rs,
// proc_shims.rs) -> pub(super).
pub(super) extern "C" fn __free_pid() {
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

// P3-1B: callers are `proc/clone.rs` and `proc/thread.rs` (both
// crate-path `use`, casting their own thread-pointer types to this
// file's `pub` opaque `Thread` marker -- same precedent as
// `sysproc.rs`'s `thread_clone` wrapper) -- demoted.
// RUSTIFY-PROC: in-`crate::proc` callers only (thread.rs, clone.rs) -> pub(super).
pub(super) extern "C" fn proctab_proc_add(p: *mut Thread) {
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
    // found. RUSTIFY-PROC: the old `while` loop advanced the global
    // `nextpid` on every probe and re-read it; the scan below is a pure
    // `successors(..).find(..)` over the identical candidate sequence
    // (`pid_lock` is held throughout this fn, so the intermediate `nextpid`
    // writes were never observable) — only the final `nextpid_advance` then
    // persists the allocator position, exactly as before. `.find`'s probe is
    // an opaque shim call per candidate PID, not a freeze-in-loop read.
    let start = xv6_proctab_nextpid_get();
    let newpid = core::iter::successors(Some(start), |&pid| {
        let np = pid.wrapping_add(1);
        let np = if np >= MAXPID { 2 } else { np };
        // stop once the scan has wrapped all the way back to `start`
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
    // N-R6d-1: register in the generational `THREAD_TABLE` (parallel to the
    // hlist above; the hlist stays the pid-lookup index until the last
    // sub-step). Same `pid_wlock` already asserted at the top of this fn. The
    // registry is sized to `NR_THREAD`, so `None` (table full) is impossible
    // here — panic in the "should not happen" style if it ever occurs rather
    // than leave a thread whose children could not resolve their parent edge.
    if crate::proc::thread::THREAD_TABLE
        .insert(p as *mut c_void as *mut crate::bindings::thread)
        .is_none()
    {
        panic_pid("thread_table_insert: registry full (should not happen)");
    }
}

pub(crate) fn get_pid_thread(pid: i32) -> *mut Thread {
    let p = xv6_proctab_get_rcu(pid);
    if p.is_null() {
        // ERR_PTR(-ESRCH) = (void *)(long)(-ESRCH)
        return (-(ESRCH as isize)) as *mut Thread;
    }
    p
}

// P3-1B: callers are `proc/exit.rs` and `proc/proc_shims.rs` (both
// crate-path `use`, casting their own thread-pointer types to this
// file's `pub` opaque `Thread` marker) -- demoted.
// RUSTIFY-PROC: in-`crate::proc` callers only (proc_shims.rs, thread.rs,
// exit.rs) -> pub(super).
pub(super) extern "C" fn proctab_proc_remove(p: *mut Thread) {
    if xv6_pid_wholding() == 0 {
        panic_pid("pid lock not held");
    }
    let existing = xv6_proctab_pop_rcu(p);
    xv6_proctab_dmplist_del(p);
    xv6_proctab_registered_dec();
    // N-R6d-1: deregister from the generational `THREAD_TABLE`, bumping the
    // slot generation so any outstanding `Tid` to this thread (e.g. a child's
    // `parent` edge) goes stale → resolves to null. Same `pid_wlock` asserted
    // above; runs before the thread object is freed (`thread_destroy`), so no
    // live `Tid` can dereference a freed thread.
    crate::proc::thread::THREAD_TABLE.remove(p as *mut c_void as *mut crate::bindings::thread);
    if !existing.is_null() && existing != p {
        panic_pid("thread_destroy called with a different proc");
    }
}

// ---------------------------------------------------------------------------
// procdump (orchestrates; C shim prints each row).
// ---------------------------------------------------------------------------

pub(crate) fn procdump() {
    xv6_procdump_header();
    proc_shims::for_each_proctab_thread(|p| {
        let _ = xv6_procdump_one(p as *mut Thread);
    });
}

// P3-1B/RUSTIFY-PROC: only caller is `sys_dumpproc` (same file) -> private.
extern "C" fn procdump_bt() {
    xv6_procdump_bt_header();
    proc_shims::for_each_proctab_thread(|p| {
        xv6_procdump_bt_one(p as *mut Thread);
    });
    xv6_procdump_bt_footer();
}

// P3-1B/RUSTIFY-PROC: only caller is `sys_dumpproc` (same file) -> private.
extern "C" fn procdump_bt_pid(pid: i32) {
    xv6_procdump_bt_pid(pid);
}

// ---------------------------------------------------------------------------
// procdump_tree / procdump_tree_pid
// ---------------------------------------------------------------------------

// P3-1B/RUSTIFY-PROC: only caller is `sys_dumpproc` (same file) -> private.
extern "C" fn procdump_tree() {
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

// P3-1B/RUSTIFY-PROC: only caller is `sys_dumpproc` (same file) -> private.
extern "C" fn procdump_tree_pid(target_pid: i32) {
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

// ---------------------------------------------------------------------------
// procdump_sessions / procdump_sessions_sid
// ---------------------------------------------------------------------------

unsafe extern "C" fn dump_session_cb(s: *mut Session, _arg: *mut c_void) {
    xv6_dump_session(s);
}

// P3-1B/RUSTIFY-PROC: only caller is `sys_dumpproc` (same file) -> private.
extern "C" fn procdump_sessions() {
    crate::kprintln!("\n=== Process Hierarchy (Session / PGroup / Process / Thread) ===");
    xv6_pid_rlock();
    session_for_each_all(Some(dump_session_cb), ptr::null_mut());
    xv6_pid_runlock();
    crate::kprintln!("\n=== End Hierarchy ===");
}

// P3-1B/RUSTIFY-PROC: only caller is `sys_dumpproc` (same file) -> private.
extern "C" fn procdump_sessions_sid(target_sid: i32) {
    crate::kprintln!("\n=== Session {} Hierarchy ===", target_sid);
    xv6_pid_rlock();
    let leader = get_pid_thread(target_sid);
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
// sys_dumpproc syscall
// ---------------------------------------------------------------------------

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
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

// P3-1B: zero callers anywhere in the tree (grep-verified) -- see
// `pid_try_lock_upgrade` above.
// RUSTIFY-PROC: zero callers anywhere -> private (ABI-preserved shell only).
unsafe extern "C" fn proctab_for_each_rcu(
    fnp: unsafe extern "C" fn(*mut Thread, *mut c_void),
    arg: *mut c_void,
) {
    proc_shims::for_each_proctab_thread(|p| {
        // SAFETY: `fnp` is a valid C callback per this function's own
        // (unsafe) contract; `arg` is caller-provided and opaque to us.
        unsafe { fnp(p as *mut Thread, arg) };
    });
}
