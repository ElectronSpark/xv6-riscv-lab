//! `kernel/proc/pgroup.rs` — POSIX process-group operations.
//!
//! Direct port of `kernel/proc/pgroup.c`. All struct-field access goes
//! through the C shims in `proc_rust_shims.c` because `struct pgroup`,
//! `struct thread`, and `struct thread_group` use bitfields and
//! `_Atomic int` members that bindgen cannot reproduce safely.
//!
//! Hierarchy:  session → pgroup → thread_group → thread
//! Lock ordering:  pid_lock > sigacts.lock > tcb_lock

#![allow(non_camel_case_types)]
#![allow(dead_code)]

use core::ffi::c_void;
use core::ptr;

use crate::proc::access::{err_ptr, is_err_const};

// ---------------------------------------------------------------------------
// Opaque kernel structs — never dereferenced from Rust.
// ---------------------------------------------------------------------------
#[repr(C)] pub struct Pgroup       { _p: [u8; 0] }
#[repr(C)] pub struct Thread       { _p: [u8; 0] }
#[repr(C)] pub struct ThreadGroup  { _p: [u8; 0] }
#[repr(C)] pub struct Session      { _p: [u8; 0] }

// Errno constants used here (matches kernel/inc/errno.h).
const EPERM:  i32 = 1;
const ESRCH:  i32 = 3;
const ENOMEM: i32 = 12;
const EEXIST: i32 = 17;
const EINVAL: i32 = 22;

// ---------------------------------------------------------------------------
// extern shim surface
// ---------------------------------------------------------------------------
unsafe extern "C" {
    // pid lock + assertion.
    pub safe fn xv6_pid_wlock();
    pub safe fn xv6_pid_wunlock();
    pub safe fn xv6_pid_rlock();
    pub safe fn xv6_pid_runlock();
    pub safe fn xv6_pid_wholding() -> i32;

    // misc helpers.
    pub safe fn xv6_current_thread() -> *mut Thread;

    // pgroup slab.
    pub safe fn xv6_pgroup_slab_init() -> i32;
    pub safe fn xv6_pgroup_slab_alloc() -> *mut Pgroup;
    pub safe fn xv6_pgroup_slab_free(pg: *mut Pgroup);

    // pgroup field accessors.
    pub safe fn pg_pgid(pg: *mut Pgroup) -> i32;
    pub safe fn pg_t_cnt(pg: *mut Pgroup) -> i32;
    pub safe fn pg_p_cnt(pg: *mut Pgroup) -> i32;
    pub safe fn pg_exited(pg: *mut Pgroup) -> i32;
    pub safe fn pg_is_kernel(pg: *mut Pgroup) -> i32;
    pub safe fn pg_session(pg: *mut Pgroup) -> *mut Session;
    pub safe fn pg_set_pgid(pg: *mut Pgroup, v: i32);
    pub safe fn pg_set_leader(pg: *mut Pgroup, tg: *mut ThreadGroup);
    pub safe fn pg_set_session(pg: *mut Pgroup, s: *mut Session);
    pub safe fn pg_set_exited(pg: *mut Pgroup, v: i32);
    pub safe fn pg_set_t_cnt(pg: *mut Pgroup, v: i32);
    pub safe fn pg_set_p_cnt(pg: *mut Pgroup, v: i32);
    pub safe fn pg_inc_t_cnt(pg: *mut Pgroup);
    pub safe fn pg_dec_t_cnt(pg: *mut Pgroup);
    pub safe fn pg_inc_p_cnt(pg: *mut Pgroup);
    pub safe fn pg_dec_p_cnt(pg: *mut Pgroup);
    pub safe fn pg_atomic_dec_t_cnt(pg: *mut Pgroup) -> i32;
    pub safe fn pg_list_entry_init(pg: *mut Pgroup);
    pub safe fn pg_list_entry_detach(pg: *mut Pgroup);
    pub safe fn pg_list_entry_is_detached(pg: *mut Pgroup) -> i32;
    pub safe fn pg_threads_init(pg: *mut Pgroup);
    pub safe fn pg_tgs_init(pg: *mut Pgroup);
    pub safe fn pg_threads_push_back(pg: *mut Pgroup, t: *mut Thread);
    pub safe fn pg_threads_detach(t: *mut Thread);
    pub safe fn pg_tgs_push_back(pg: *mut Pgroup, tg: *mut ThreadGroup);
    pub safe fn pg_tgs_detach(tg: *mut ThreadGroup);
    pub safe fn pg_tg_list_entry_is_detached(tg: *mut ThreadGroup) -> i32;
    pub safe fn pg_for_each_tg(
        pg: *mut Pgroup,
        fnp: unsafe extern "C" fn(*mut ThreadGroup, *mut c_void),
        arg: *mut c_void,
    );

    // thread field accessors.
    pub safe fn t_pid(p: *mut Thread) -> i32;
    pub safe fn t_pgid(p: *mut Thread) -> i32;
    pub safe fn t_sid(p: *mut Thread) -> i32;
    pub safe fn t_set_pgid(p: *mut Thread, v: i32);
    pub safe fn t_parent(p: *mut Thread) -> *mut Thread;
    pub safe fn t_pgroup(p: *mut Thread) -> *mut Pgroup;
    pub safe fn t_session(p: *mut Thread) -> *mut Session;
    pub safe fn t_thread_group(p: *mut Thread) -> *mut ThreadGroup;
    pub safe fn t_set_pgroup(p: *mut Thread, pg: *mut Pgroup);
    pub safe fn t_pg_entry_init(t: *mut Thread);
    pub safe fn t_pg_entry_is_detached(t: *mut Thread) -> i32;

    // thread_group accessors.
    pub safe fn tg_pgroup(tg: *mut ThreadGroup) -> *mut Pgroup;
    pub safe fn tg_set_pgroup(tg: *mut ThreadGroup, pg: *mut Pgroup);
    pub safe fn tg_list_entry_init(tg: *mut ThreadGroup);
    pub safe fn tg_for_each_thread(
        tg: *mut ThreadGroup,
        fnp: unsafe extern "C" fn(*mut Thread, *mut c_void),
        arg: *mut c_void,
    );

    // ksiginfo helper.
    pub safe fn xv6_tg_send_signo(tg: *mut ThreadGroup, signo: i32) -> i32;

    // From session.h / thread.h / thread_group.h (real extern fns).
    pub safe fn get_pid_thread(pid: i32) -> *mut Thread;
    // Not `pub`: shares a bare name with the real definition glob-
    // reexported from `thread_group.rs` at `crate::proc`. Making this
    // crate-visible would trigger E0659 ambiguous-glob-reexport the
    // moment any other proc submodule imports the real one by its bare
    // name (P3-1B2 sweep, same finding as `clone.rs`/`sys_signal.rs`).
    // Only ever called from within this file, so file-private is both
    // sufficient and correct.
    safe fn thread_tgid(t: *mut Thread) -> i32;
    pub safe fn rcu_read_lock();
    pub safe fn rcu_read_unlock();
    pub safe fn xv6_panic(msg: *const u8) -> !;
}

// P3-1C mesh sweep: tty/session.rs is in scope for this wave, same
// opaque-marker reinterpret precedent used throughout this file (this
// file's `Session`/`Pgroup` are distinct-but-layout-identical stand-ins
// for the real `crate::bindings` structs).
/// SAFETY: only called with a live session and a live pgroup (both
/// non-null-checked by the caller immediately before the call) --
/// matches the real fn's contract. Its `c_int` return is discarded here
/// exactly as both call sites in this file always did.
fn session_add_pg(s: *mut Session, pg: *mut Pgroup) -> i32 {
    unsafe {
        crate::tty::session::session_add_pg(
            s as *mut c_void as *mut crate::bindings::session,
            pg as *mut c_void as *mut crate::bindings::pgroup,
        )
    }
}
/// SAFETY: see `session_add_pg`.
fn session_remove_pg(s: *mut Session, pg: *mut Pgroup) -> i32 {
    unsafe {
        crate::tty::session::session_remove_pg(
            s as *mut c_void as *mut crate::bindings::session,
            pg as *mut c_void as *mut crate::bindings::pgroup,
        )
    }
}

// `rcu_read_lock` / `rcu_read_unlock` are static-inline; expose via shim.
unsafe extern "C" {
    #[link_name = "xv6_rcu_read_lock"]   safe fn rcu_lock();
    #[link_name = "xv6_rcu_read_unlock"] safe fn rcu_unlock();
}

#[inline]
fn pid_assert_wholding() {
    if xv6_pid_wholding() == 0 {
        panic_pgroup("pid_wlock must be held");
    }
}

#[cold]
fn panic_pgroup(msg: &str) -> ! {
    let mut buf = [0u8; 96];
    let n = msg.as_bytes().len().min(buf.len() - 1);
    buf[..n].copy_from_slice(&msg.as_bytes()[..n]);
    xv6_panic(buf.as_ptr())
}

// ---------------------------------------------------------------------------
// Rust loops written in idiomatic macro-style (replaces legacy C loop macros)
// ---------------------------------------------------------------------------

macro_rules! pg_for_each_tg {
    ($pg:expr, $tg:ident, $body:block) => {
        let (head, offset) = unsafe {
            let pg_raw = $pg as *mut crate::bindings::pgroup;
            let p_groups = &raw mut (*pg_raw).thread_groups;
            let off = core::mem::offset_of!(crate::bindings::thread_group, list_entry);
            (p_groups as *mut crate::bindings::list_node_t, off)
        };
        $crate::list_for_each!(head, offset, $tg, {
            let $tg = $tg as *mut ThreadGroup;
            $body
        });
    };
}

macro_rules! tg_for_each_thread {
    ($tg:expr, $t:ident, $body:block) => {
        let (head, offset) = unsafe {
            let tg_raw = $tg as *mut crate::bindings::thread_group;
            let t_list = &raw mut (*tg_raw).thread_list;
            let off = core::mem::offset_of!(crate::bindings::thread, tg_entry);
            (t_list as *mut crate::bindings::list_node_t, off)
        };
        $crate::list_for_each!(head, offset, $t, {
            let $t = $t as *mut Thread;
            $body
        });
    };
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Called each time a thread group is removed from a process group.
/// If the process group is empty (no thread groups), remove it from
/// its session and free it. Caller must hold pid_wlock.
fn __pgroup_cleanup(pg: *mut Pgroup) {
    if pg.is_null() {
        return;
    }
    if pg_p_cnt(pg) > 0 {
        return;
    }
    if pg_is_kernel(pg) != 0 {
        return;
    }
    pg_set_exited(pg, 1);
    let sess = pg_session(pg);
    if !sess.is_null() {
        session_remove_pg(sess, pg);
    }
    if pg_list_entry_is_detached(pg) == 0 {
        pg_list_entry_detach(pg);
    }
    xv6_pgroup_slab_free(pg);
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn pgroup_alloc(
    pgid: i32,
    leader: *mut ThreadGroup,
) -> *mut Pgroup {
    let pg = xv6_pgroup_slab_alloc();
    if pg.is_null() {
        return ptr::null_mut();
    }
    // Zero whatever the slab returned (struct pgroup is small but we don't
    // know the layout here; rely on the field setters to clobber what we
    // care about, then explicitly zero the bitfield + counters).
    pg_set_pgid(pg, pgid);
    pg_set_leader(pg, leader);
    pg_list_entry_init(pg);
    pg_threads_init(pg);
    pg_tgs_init(pg);
    pg_set_t_cnt(pg, 0);
    pg_set_p_cnt(pg, 0);
    pg_set_exited(pg, 0);
    pg_set_session(pg, ptr::null_mut());
    pg
}

// P3-1B: only caller is `proc/thread.rs::init_entry` (via its own `extern`
// declaration typed `*mut bindings::thread`, a different opaque marker
// type than this file's local `Thread` -- converted to a typed thin
// wrapper at the call site, same precedent as `sysproc.rs`'s
// `thread_clone`) -- demoted.
pub(crate) extern "C" fn pgroup_init(initproc: *mut Thread) {
    let ret = xv6_pgroup_slab_init();
    if ret != 0 {
        panic_pgroup("Failed to initialize pgroup slab cache");
    }
    let tg = t_thread_group(initproc);
    if tg.is_null() {
        panic_pgroup("pgroup_init: initproc has no thread_group");
    }
    let pg = pgroup_alloc(t_pid(initproc), tg);
    if pg.is_null() {
        panic_pgroup("pgroup_init: pgroup_alloc failed");
    }
    pgroup_add_tg(pg, tg);
    pgroup_add_thread(pg, initproc);
}

// ---------------------------------------------------------------------------
// Thread membership
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn pgroup_add_thread(
    pg: *mut Pgroup,
    t: *mut Thread,
) -> i32 {
    pid_assert_wholding();
    if pg.is_null() || t.is_null() {
        return -EINVAL;
    }
    if pg_exited(pg) != 0 {
        return -ESRCH;
    }
    if !t_pgroup(t).is_null() {
        return -EEXIST;
    }
    t_set_pgroup(t, pg);
    t_set_pgid(t, pg_pgid(pg));
    t_pg_entry_init(t);
    pg_threads_push_back(pg, t);
    pg_inc_t_cnt(pg);
    0
}

#[no_mangle]
pub extern "C" fn pgroup_remove_thread(t: *mut Thread) {
    pid_assert_wholding();
    if t.is_null() {
        return;
    }
    let pg = t_pgroup(t);
    if pg.is_null() {
        return;
    }
    if pg_t_cnt(pg) <= 0 {
        panic_pgroup("Process group thread count went negative");
    }
    if t_pg_entry_is_detached(t) != 0 {
        panic_pgroup("Thread is not in the process group list");
    }
    pg_threads_detach(t);
    pg_dec_t_cnt(pg);
    t_set_pgroup(t, ptr::null_mut());
    t_set_pgid(t, 0);
}

// ---------------------------------------------------------------------------
// Thread group membership
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn pgroup_add_tg(
    pg: *mut Pgroup,
    tg: *mut ThreadGroup,
) -> i32 {
    pid_assert_wholding();
    if pg.is_null() || tg.is_null() {
        return -EINVAL;
    }
    if pg_exited(pg) != 0 {
        return -ESRCH;
    }
    if !tg_pgroup(tg).is_null() {
        return -EEXIST;
    }
    tg_list_entry_init(tg);
    pg_tgs_push_back(pg, tg);
    tg_set_pgroup(tg, pg);
    pg_inc_p_cnt(pg);
    0
}

#[no_mangle]
pub extern "C" fn pgroup_remove_tg(tg: *mut ThreadGroup) {
    pid_assert_wholding();
    if tg.is_null() {
        return;
    }
    let pg = tg_pgroup(tg);
    if pg.is_null() {
        return;
    }
    if pg_p_cnt(pg) <= 0 {
        panic_pgroup("Process group thread group count went negative");
    }
    if pg_tg_list_entry_is_detached(tg) == 0 {
        pg_tgs_detach(tg);
    }
    pg_dec_p_cnt(pg);
    tg_set_pgroup(tg, ptr::null_mut());
    __pgroup_cleanup(pg);
}

// P3-1B: zero callers anywhere in the tree (grep-verified). Demoted from
// `#[no_mangle]`; `#[allow(dead_code)]` documents the gap (matches this
// wave's `goldfish_rtc_init`/`sched_timer_add` precedent). Note: the file
// already carries a blanket `#![allow(dead_code)]` (line 12), so this
// per-item attribute is documentation, not a functional requirement.
pub(crate) extern "C" fn pgroup_live_dec(pg: *mut Pgroup) {
    if pg.is_null() {
        return;
    }
    if pg_atomic_dec_t_cnt(pg) <= 0 {
        panic_pgroup("Process group live thread count went negative");
    }
}

// ---------------------------------------------------------------------------
// Migration
// ---------------------------------------------------------------------------

// P3-1B: only called internally within this file (`pgroup_setpgid`) --
// demoted.
pub(crate) extern "C" fn pgroup_migrate_tg(
    tg: *mut ThreadGroup,
    new_pg: *mut Pgroup,
) {
    pid_assert_wholding();
    if tg.is_null() {
        panic_pgroup("pgroup_migrate_tg: NULL tg");
    }
    if new_pg.is_null() {
        panic_pgroup("pgroup_migrate_tg: NULL new_pg");
    }
    if !tg_pgroup(tg).is_null() {
        pgroup_remove_tg(tg);
    }
    pgroup_add_tg(new_pg, tg);

    tg_for_each_thread!(tg, t, {
        if t_pgroup(t) != new_pg {
            pgroup_remove_thread(t);
        }
        if t_pgroup(t).is_null() {
            pgroup_add_thread(new_pg, t);
        }
    });
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn get_pgroup(pgid: i32) -> *mut Pgroup {
    let t = get_pid_thread(pgid);
    if is_err_const(t as *const c_void) {
        return err_ptr::<Pgroup>(-ESRCH);
    }
    let pg = t_pgroup(t);
    if pg.is_null() {
        return err_ptr::<Pgroup>(-ESRCH);
    }
    pg
}

// ---------------------------------------------------------------------------
// setpgid / getpgid
// ---------------------------------------------------------------------------

// P3-1B: only caller is `proc/sysproc.rs::sys_setpgid` (crate-path `use`,
// not an `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn pgroup_setpgid(pid: i32, pgid: i32) -> i32 {
    let p = xv6_current_thread();
    let mut pid = pid;
    let mut pgid = pgid;
    let tgid = thread_tgid(p);

    if pid == 0 {
        pid = tgid;
    }
    if pgid == 0 {
        pgid = pid;
    }
    if pgid < 0 {
        return -EINVAL;
    }

    xv6_pid_wlock();

    // Find the target thread.
    let target;
    if pid == t_pid(p) || pid == tgid {
        target = p;
    } else {
        target = get_pid_thread(pid);
        if is_err_const(target as *const c_void) {
            xv6_pid_wunlock();
            return -ESRCH;
        }
    }

    // Must be self or a child of self.
    if target != p && t_parent(target) != p {
        xv6_pid_wunlock();
        return -ESRCH;
    }

    let target_tgid = thread_tgid(target);
    // Cannot change session leader's pgid.
    if t_sid(target) == target_tgid {
        xv6_pid_wunlock();
        return -EPERM;
    }
    // Must be in the same session.
    if t_sid(target) != t_sid(p) {
        xv6_pid_wunlock();
        return -EPERM;
    }
    // Already in the right group?
    if t_pgid(target) == pgid {
        xv6_pid_wunlock();
        return 0;
    }

    let tg = t_thread_group(target);
    let new_pg: *mut Pgroup;

    if pgid == target_tgid {
        // Creating a new process group with target as leader.
        new_pg = pgroup_alloc(pgid, tg);
        if new_pg.is_null() {
            xv6_pid_wunlock();
            return -ENOMEM;
        }
        let sess = t_session(target);
        if !sess.is_null() {
            session_add_pg(sess, new_pg);
        }
    } else {
        // Joining an existing process group.
        new_pg = get_pgroup(pgid);
        if is_err_const(new_pg as *const c_void) {
            xv6_pid_wunlock();
            return -EPERM;
        }
        if pg_exited(new_pg) != 0 {
            xv6_pid_wunlock();
            return -EPERM;
        }
        if pg_session(new_pg) != t_session(target) {
            xv6_pid_wunlock();
            return -EPERM;
        }
    }

    pgroup_migrate_tg(tg, new_pg);
    xv6_pid_wunlock();
    0
}

// P3-1B: only caller is `proc/sysproc.rs::sys_getpgid` (crate-path `use`,
// not an `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn pgroup_getpgid(pid: i32) -> i32 {
    if pid == 0 {
        return t_pgid(xv6_current_thread());
    }
    if pid < 0 {
        return -EINVAL;
    }

    rcu_lock();
    let target = get_pid_thread(pid);
    if is_err_const(target as *const c_void) {
        rcu_unlock();
        return -ESRCH;
    }
    let pgid = t_pgid(target);
    rcu_unlock();
    pgid
}

// ---------------------------------------------------------------------------
// Process-group-wide signal delivery
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn pgroup_kill(pgid: i32, signum: i32) -> i32 {
    xv6_pid_rlock();
    let pg = get_pgroup(pgid);
    if is_err_const(pg as *const c_void) || pg_exited(pg) != 0 {
        xv6_pid_runlock();
        return -ESRCH;
    }
    if pg_is_kernel(pg) != 0 {
        xv6_pid_runlock();
        return -EPERM;
    }

    let mut count = 0;
    pg_for_each_tg!(pg, tg, {
        xv6_tg_send_signo(tg, signum);
        count += 1;
    });
    xv6_pid_runlock();

    if count > 0 { 0 } else { -ESRCH }
}
