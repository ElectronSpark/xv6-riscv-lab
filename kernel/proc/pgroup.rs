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
// Kernel struct aliases — never dereferenced directly from this file.
// (P3-D1a: formerly opaque `[u8; 0]` markers; now the real `crate::bindings`
// types so the direct Rust calls into `proc_shims` type-check unchanged.)
// ---------------------------------------------------------------------------
pub type Thread      = crate::bindings::thread;
pub type ThreadGroup = crate::bindings::thread_group;
pub type Session     = crate::bindings::session;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N3.
//
// `Pgroup` IS the kernel-wide Rust definition of
// `kernel/inc/proc/pgroup_types.h`'s `struct pgroup` now: `build.rs`
// blocklists the bindgen-generated form and injects
// `pub use crate::proc::pgroup::Pgroup as pgroup;`, so every
// `crate::bindings::pgroup` path across the crate resolves here.
// `leader`/`session` pointer fields keep their bindgen pointee paths
// (`thread_group` redirects to the native in `thread_group.rs`,
// `session` to `tty/session.rs` — both this same wave); a pointer's own
// layout never depends on its pointee.
// ---------------------------------------------------------------------------

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 exited : 1; uint64 is_kernel : 1; }` inside
/// `struct pgroup` (bindgen's `pgroup__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `exited` at bit 0 and
/// `is_kernel` at bit 1 of the (8-byte) unit's byte 0 — identical to
/// bindgen's `get(0,1)`/`get(1,1)` accessors reproduced below.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct PgroupFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl PgroupFlagBits {
    #[inline]
    pub(crate) fn exited(&self) -> u64 {
        (self.bits & 0b01) as u64
    }
    #[inline]
    pub(crate) fn set_exited(&mut self, val: u64) {
        self.bits = (self.bits & !0b01) | ((val as u8) & 0b01);
    }
    #[inline]
    pub(crate) fn is_kernel(&self) -> u64 {
        ((self.bits >> 1) & 0b01) as u64
    }
    #[inline]
    pub(crate) fn set_is_kernel(&mut self, val: u64) {
        self.bits = (self.bits & !0b10) | (((val as u8) << 1) & 0b10);
    }
}

const _: () = {
    assert!(core::mem::size_of::<PgroupFlagBits>() == 8, "pgroup anon bitfield size");
    assert!(core::mem::align_of::<PgroupFlagBits>() == 8, "pgroup anon bitfield alignment");
};

#[repr(C)]
#[derive(Copy, Clone)]
pub struct Pgroup {
    pub(crate) list_entry: crate::list::ListNode,
    pub(crate) pgid: crate::bindings::pid_t,
    pub(crate) leader: *mut crate::bindings::thread_group,
    pub(crate) flags: PgroupFlagBits,
    pub(crate) t_cnt: core::ffi::c_int,
    pub(crate) threads: crate::list::ListNode,
    pub(crate) p_cnt: core::ffi::c_int,
    pub(crate) thread_groups: crate::list::ListNode,
    // N-R6d-2a: the session back-edge converted off `*mut session` to the
    // session family's generational key [`Sid`] (`GenKey<SID_TAG>`, 8
    // bytes/align 4 — the exact span the `*mut session` occupied, so this
    // offset (88, already 8-aligned) and the struct size (96) are unchanged;
    // the layout asserts below prove it). `Sid::NONE` (`generation == 0`) is
    // the "no session" edge. Resolved to a `*mut session` through the pilot's
    // `SESSION_TABLE` by the `session` accessors
    // (`PgroupAccess::session_ptr`/`set_session` → `proc_shims::pg_session`/
    // `pg_set_session` → `pgroup_session_resolve`/`pgroup_session_store`); a
    // stale `Sid` (session freed) resolves to null — the existing "session
    // gone" semantics readers already null-check.
    pub(crate) session: crate::tty::session::Sid,
}

// P3-N3 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// pgroup { list_entry: list_node_t, pgid: pid_t, leader: *mut
// thread_group, __bindgen_anon_1: pgroup__bindgen_ty_1, t_cnt: c_int,
// threads: list_node_t, p_cnt: c_int, thread_groups: list_node_t,
// session: *mut session }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe (rv64gc/lp64d)
// against `kernel/inc/proc/pgroup_types.h`: size 96, align 8, offsets
// 0/16/24/[32]/40/48/64/72/88 (the anonymous bitfield struct occupies
// [32,40), pinned in the probe by both neighbours).
const _: () = {
    assert!(core::mem::size_of::<Pgroup>() == 96, "pgroup size");
    assert!(core::mem::align_of::<Pgroup>() == 8, "pgroup alignment");
    assert!(core::mem::offset_of!(Pgroup, list_entry) == 0, "pgroup.list_entry offset");
    assert!(core::mem::offset_of!(Pgroup, pgid) == 16, "pgroup.pgid offset");
    assert!(core::mem::offset_of!(Pgroup, leader) == 24, "pgroup.leader offset");
    assert!(core::mem::offset_of!(Pgroup, flags) == 32, "pgroup anon bitfield offset");
    assert!(core::mem::offset_of!(Pgroup, t_cnt) == 40, "pgroup.t_cnt offset");
    assert!(core::mem::offset_of!(Pgroup, threads) == 48, "pgroup.threads offset");
    assert!(core::mem::offset_of!(Pgroup, p_cnt) == 64, "pgroup.p_cnt offset");
    assert!(core::mem::offset_of!(Pgroup, thread_groups) == 72, "pgroup.thread_groups offset");
    // N-R6d-2a: `session` is now an `Sid` (`GenKey`, 8 bytes/align 4), not a
    // `*mut session` (8 bytes/align 8). Same 8-byte span at an already-8-aligned
    // offset, so this offset and the `size == 96` assert above are unchanged.
    assert!(
        core::mem::size_of::<crate::tty::session::Sid>() == 8,
        "Sid must be 8 bytes to preserve pgroup.session span"
    );
    assert!(core::mem::offset_of!(Pgroup, session) == 88, "pgroup.session offset");
};

// N-R6d-2a: the `pgroup.session` back-edge resolve/store — mirror of the
// thread edge (`thread_session_resolve`/`thread_session_store`), reading/
// writing this struct's own generational `Sid` field and delegating the
// Sid↔`*mut session` mapping to the pilot's `SESSION_TABLE`. Backing the
// `pg_session`/`pg_set_session` shims (signatures unchanged, so every caller
// — pgroup.rs itself, access.rs `PgroupAccess` — is untouched).

/// Resolve pgroup `pg`'s stored `session` [`Sid`] to a live `*mut session`, or
/// null if [`NONE`](crate::tty::session::Sid) ("no session") or stale (session
/// freed → the "session gone" answer callers null-check). Caller holds
/// `pid_lock` (reader or writer — [`session_lookup`](crate::tty::session::session_lookup)
/// requires it; single non-looping generation-checked read, no `&Session`).
pub(crate) fn pgroup_session_resolve(pg: *mut Pgroup) -> *mut Session {
    // SAFETY: `pg` is a live `*mut pgroup` (shim contract); reading its own
    // `session` `Sid` field is a plain aligned word read.
    let sid = unsafe { (*pg).session };
    crate::tty::session::session_lookup(sid).unwrap_or(ptr::null_mut())
}

/// Store `s` as pgroup `pg`'s `session` edge: `s == null` → `Sid::NONE`,
/// otherwise `s`'s own cached [`Sid`] (`session.self_sid`). Caller holds
/// `pid_wlock` (every `set_session` site does).
pub(crate) fn pgroup_session_store(pg: *mut Pgroup, s: *mut Session) {
    let sid = crate::tty::session::session_key_of(s);
    // SAFETY: `pg` is a live `*mut pgroup`; writing its own `session` field
    // under the caller's `pid_wlock` (exclusive) is race-free.
    unsafe {
        (*pg).session = sid;
    }
}

// Errno constants used here (matches kernel/inc/errno.h).
const EPERM:  i32 = 1;
const ESRCH:  i32 = 3;
const ENOMEM: i32 = 12;
const EEXIST: i32 = 17;
const EINVAL: i32 = 22;

// ---------------------------------------------------------------------------
// extern shim surface
// ---------------------------------------------------------------------------
// P3-D1a: the `xv6_*`/`pg_*`/`t_*`/`tg_*` accessor shims are ordinary Rust
// fns in `proc_shims.rs`; call them via crate paths instead of `extern "C"`
// redeclarations. Call sites keep the same bare names.
use crate::proc::proc_shims::{
    pg_atomic_dec_t_cnt, pg_dec_p_cnt, pg_dec_t_cnt, pg_exited, pg_inc_p_cnt, pg_inc_t_cnt,
    pg_is_kernel, pg_list_entry_detach, pg_list_entry_init, pg_list_entry_is_detached, pg_p_cnt,
    pg_pgid, pg_session, pg_set_exited, pg_set_leader, pg_set_p_cnt, pg_set_pgid, pg_set_session,
    pg_set_t_cnt, pg_t_cnt, pg_tg_list_entry_is_detached, pg_tgs_detach, pg_tgs_init,
    pg_tgs_push_back, pg_threads_detach, pg_threads_init, pg_threads_push_back, t_parent,
    t_pg_entry_init, t_pg_entry_is_detached, t_pgid, t_pgroup, t_pid, t_session, t_set_pgid,
    t_set_pgroup, t_sid, t_thread_group, tg_list_entry_init, tg_pgroup, tg_set_pgroup,
    xv6_current_thread, xv6_panic, xv6_pgroup_slab_alloc, xv6_pgroup_slab_free,
    xv6_pgroup_slab_init, xv6_pid_rlock, xv6_pid_runlock, xv6_pid_wholding, xv6_pid_wlock,
    xv6_pid_wunlock, xv6_tg_send_signo,
};

// P3-D3b: the `pub safe fn rcu_read_lock/_unlock` extern redeclarations
// that sat here were dead — this file's RCU read-side sections go through
// the `xv6_rcu_read_lock`/`xv6_rcu_read_unlock` shims (see the comment
// below) — so they are simply deleted now that lock/rcu.rs's
// `#[no_mangle]` exports are gone.

// P3-D2b: `get_pid_thread` (proc/pid.rs) and `thread_tgid`
// (proc/thread_group.rs) are plain (file-private, so no E0659
// glob-reexport ambiguity) crate-path items now that their
// `#[no_mangle]` exports are gone (identical signatures) -- they used
// to be `extern "C"` redeclarations in the block above.
use crate::proc::{get_pid_thread, thread_tgid};

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

// `rcu_read_lock` / `rcu_read_unlock` are static-inline; use the Rust shims.
use crate::proc::proc_shims::{xv6_rcu_read_lock as rcu_lock, xv6_rcu_read_unlock as rcu_unlock};

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

pub(crate) fn pgroup_alloc(
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

pub(crate) fn pgroup_add_thread(
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

pub(crate) fn pgroup_remove_thread(t: *mut Thread) {
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

pub(crate) fn pgroup_add_tg(
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

pub(crate) fn pgroup_remove_tg(tg: *mut ThreadGroup) {
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

pub(crate) fn get_pgroup(pgid: i32) -> *mut Pgroup {
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

pub(crate) fn pgroup_kill(pgid: i32, signum: i32) -> i32 {
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
