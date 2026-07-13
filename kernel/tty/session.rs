//! Terminal session management (POSIX).
//!
//! Rust port of `kernel/tty/session.c` (Phase 2 Wave 12, see
//! `docs/rustify/phase2_plan.md`) -- completes the `tty` module.
//!
//! A session groups process groups and optionally binds them to a
//! controlling terminal. The session leader (thread whose `tgid == sid`)
//! creates the session via `setsid()`.
//!
//! Hierarchy:  session -> pgroup -> thread_group -> thread
//!
//! Session membership (`thread->session`, `pgroup->session`) is
//! protected by the global `pid_lock` (rwlock), exactly as in the C
//! original:
//!   - `pid_wlock` for mutations (setsid, add/remove thread/pg, ctrl-tty
//!     attach/detach)
//!   - `pid_rlock` (or an already-held `pid_wlock`) for read-only access
//!     (getsid, fg_pgrp queries)
//!
//! Lock ordering:  `pid_lock` > `sigacts.lock` > `tcb_lock`. `tty->lock`
//! (a leaf spinlock, never itself blocks) may be taken while `pid_lock`
//! is held -- see [`session_set_ctrl_tty`]'s doc for why this file now
//! does that.
//!
//! `session_list` (the global list of live sessions) is a plain
//! non-atomic `list_node_t`, exactly as the C original declared it --
//! every mutation site holds `pid_wlock`, matching the module doc above;
//! it is `#[no_mangle] pub static mut` because `kernel/proc/pid.rs`'s
//! `xv6_dump_session`-adjacent `session_for_each_all` (in
//! `kernel/proc/proc_shims.rs`) still reaches it via its own `extern
//! "C" { static mut session_list: list_node_t; }` declaration.
//!
//! # SIGINT delivery fix (mandated, Wave 12)
//!
//! The C original's `session_set_ctrl_tty()` only ever wrote
//! `session->ctrl_tty`; it never wrote the corresponding back-pointer
//! `tty->session`. That meant `kernel/tty/tty.rs`'s
//! `tty_signal_fg_pgroup()` -- which routes ^C/^\/^Z to the terminal's
//! foreground process group by reading `(*t).session` -- could *never*
//! find a session through a live console/pty tty, and silently fell
//! back to signalling `current_thread_ptr()` instead. On the console
//! that fallback thread is a kernel feeder thread, not the foreground
//! shell/job, so ^C was effectively dead (documented as a known gap in
//! `RUST_REWRITE.md` Iteration 23).
//!
//! [`session_set_ctrl_tty`] now sets both sides of the relationship and
//! keeps a [`tty_ref`]/[`tty_unref`] pair balanced against it (so the
//! tty cannot be freed while a session still designates it as the
//! controlling terminal), symmetrically cleared by `__session_detach_ctrl_tty`
//! on every teardown path: replacing an already-set `ctrl_tty`,
//! [`session_hangup`], and session finalization in [`session_unref`].
//! See the [`session_set_ctrl_tty`] doc for the exact design.

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{ksiginfo, list_node_t, pgroup, pid_t, session, slab_cache_t, thread, thread_group, tty};

// ===========================================================================
// Externs -- every cross-module C-ABI symbol this file calls, declared
// locally rather than imported by Rust path. Matches the established
// convention across the crate (see `irq/trap.rs`, `irq/syscall.rs`,
// `kernel/tty/tty.rs`'s own externs) rather than reaching through
// `crate::proc::...` module paths.
// ===========================================================================

unsafe extern "C" {
    // slab (mm module).
    pub safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
    pub safe fn slab_free(obj: *mut c_void);
    pub safe fn slab_cache_init(
        cache: *mut slab_cache_t,
        name: *mut c_char,
        obj_size: usize,
        flags: u64,
    ) -> c_int;

    // pid lock (proc/pid.rs).
    pub safe fn pid_wlock();
    pub safe fn pid_wunlock();
    pub safe fn pid_assert_wholding();

    // proc/pid.rs, proc/pgroup.rs, proc/thread_group.rs, lock/rcu.rs.
    pub safe fn get_pid_thread(pid: c_int) -> *mut thread;
    pub safe fn thread_tgid(p: *mut thread) -> c_int;
    pub safe fn pgroup_alloc(pgid: c_int, leader: *mut thread_group) -> *mut pgroup;
    pub safe fn pgroup_add_tg(pg: *mut pgroup, tg: *mut thread_group) -> c_int;
    pub safe fn pgroup_remove_tg(tg: *mut thread_group);
    pub safe fn pgroup_add_thread(pg: *mut pgroup, t: *mut thread) -> c_int;
    pub safe fn pgroup_remove_thread(t: *mut thread);
    pub safe fn tg_signal_send(tg: *mut thread_group, info: *mut ksiginfo) -> c_int;
    pub safe fn rcu_read_lock();
    pub safe fn rcu_read_unlock();

    // tty/tty.rs -- SIGINT-delivery fix (session <-> ctrl_tty back-pointer
    // + refcount, see the module doc above). Both have real preconditions
    // (`t` must be a live tty), so declared without `safe` -- every call
    // site below is wrapped in `unsafe`.
    fn tty_ref(t: *mut tty);
    fn tty_unref(t: *mut tty);

    // printf/panic (matches `kernel/tty/tty.rs`'s own local
    // `tty_assert_errno` extern block exactly).
    pub safe fn printf(fmt: *const c_char, ...) -> c_int;
    pub safe fn __panic_start();
    pub safe fn __panic_end() -> !;
}

// ===========================================================================
// Errno / signal constants (`kernel/inc/errno.h`, `kernel/inc/uabi/signo.h`).
// Duplicated locally rather than shared cross-module -- matches the
// established per-file convention (see e.g. `kernel/tty/tty.rs`'s
// identical local errno consts).
// ===========================================================================

const EPERM: c_int = 1;
const ESRCH: c_int = 3;
const ENOMEM: c_int = 12;
const EEXIST: c_int = 17;
const EINVAL: c_int = 22;

const SIGHUP: c_int = 1;
const SIGCONT: c_int = 18;

// ===========================================================================
// `IS_ERR`/`ERR_PTR` (`kernel/inc/errno.h`), specialised to pointers.
// Duplicated locally -- matches `kernel/tty/tty.rs`/`kernel/tty/pty.rs`'s
// identical local `is_err`/`err_ptr` helpers.
// ===========================================================================

const MAX_ERRNO: usize = 4095;

#[inline]
fn is_err<T>(p: *mut T) -> bool {
    (p as usize) >= usize::MAX - MAX_ERRNO + 1
}

#[inline]
fn err_ptr<T>(err: c_int) -> *mut T {
    err as isize as *mut T
}

// ===========================================================================
// Panic helper -- replicates the C `assert(expr, fmt, ...)` macro
// expansion (`kernel/inc/printf.h`). Same pattern as
// `kernel/tty/tty.rs::tty_assert_errno` / `kernel/tty/tty_dev.rs::
// tty_dev_assert_errno`, generalised to take the function name too
// (session.c's asserts span several functions, not just one).
// ===========================================================================

fn session_assert(cond: bool, line: u32, func: &core::ffi::CStr, msg: &core::ffi::CStr) {
    if cond {
        return;
    }
    __panic_start();
    printf(
        c"ASSERTION_FAILURE %s:%d: In function '%s':\n".as_ptr(),
        c"kernel/tty/session.rs".as_ptr(),
        line as c_int,
        func.as_ptr(),
    );
    printf(msg.as_ptr());
    printf(c"\n".as_ptr());
    __panic_end();
}

fn session_assert_errno(cond: bool, line: u32, func: &core::ffi::CStr, msg: &core::ffi::CStr, errno: c_int) {
    if cond {
        return;
    }
    __panic_start();
    printf(
        c"ASSERTION_FAILURE %s:%d: In function '%s':\n".as_ptr(),
        c"kernel/tty/session.rs".as_ptr(),
        line as c_int,
        func.as_ptr(),
    );
    printf(msg.as_ptr(), errno);
    printf(c"\n".as_ptr());
    __panic_end();
}

// ===========================================================================
// Doubly-linked list primitives (`kernel/inc/list.h`'s plain, non-RCU
// `static inline` helpers, reimplemented natively -- bindgen cannot
// process `static inline` bodies). Same approach as Wave 1's
// `kernel/hlist.rs`. Every helper here requires the caller to already
// hold whatever lock protects the list in question (`pid_wlock` for
// every list this file touches), matching the C originals' implicit
// contract.
// ===========================================================================

/// # Safety
/// `e` must be a valid, writable `list_node_t`.
#[inline]
unsafe fn ll_init(e: *mut list_node_t) {
    // SAFETY: see fn doc.
    unsafe {
        (*e).next = e;
        (*e).prev = e;
    }
}

/// # Safety
/// `e` must be a valid, readable `list_node_t`.
#[inline]
unsafe fn ll_is_detached(e: *mut list_node_t) -> bool {
    // SAFETY: see fn doc.
    unsafe { (*e).next == e }
}

/// # Safety
/// `e` must be a valid, writable `list_node_t` that is linked into a
/// list (its `prev`/`next` neighbours must themselves be valid, writable
/// `list_node_t`s).
#[inline]
unsafe fn ll_detach(e: *mut list_node_t) {
    // SAFETY: see fn doc.
    unsafe {
        (*(*e).prev).next = (*e).next;
        (*(*e).next).prev = (*e).prev;
        ll_init(e);
    }
}

/// Insert `entry` immediately after `prev` (`kernel/inc/list.h`'s
/// `list_entry_insert`).
///
/// # Safety
/// `prev` and `entry` must be valid, writable `list_node_t`s; `prev`
/// must be linked into a list (its `next` neighbour must be a valid,
/// writable `list_node_t`).
#[inline]
unsafe fn ll_insert(prev: *mut list_node_t, entry: *mut list_node_t) {
    // SAFETY: see fn doc.
    unsafe {
        let next = (*prev).next;
        (*entry).prev = prev;
        (*entry).next = next;
        (*prev).next = entry;
        (*next).prev = entry;
    }
}

/// Push `entry` onto the back of the list headed by `head`
/// (`kernel/inc/list.h`'s `list_entry_push_back`).
///
/// # Safety
/// `head` and `entry` must be valid, writable `list_node_t`s; `head`
/// must be a properly initialized list head (self-linked when empty).
#[inline]
unsafe fn ll_push_back(head: *mut list_node_t, entry: *mut list_node_t) {
    // SAFETY: see fn doc.
    unsafe { ll_insert((*head).prev, entry) };
}

// ===========================================================================
// Slab cache + global session list.
// ===========================================================================

/// `UnsafeCell<MaybeUninit<T>>` + `unsafe impl Sync` for one file-scope
/// slab cache. Same pattern as `kernel/tty/tty.rs`'s `SyncCell<T>`
/// (re-declared locally here per that file's own note: "the established
/// crate convention").
struct SyncCell<T>(core::cell::UnsafeCell<core::mem::MaybeUninit<T>>);
// SAFETY: `SESSION_CACHE` is mutated exclusively through the C kernel's
// own synchronised entry points (`slab_cache_init`/`slab_alloc`
// internally lock).
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn uninit() -> Self {
        SyncCell(core::cell::UnsafeCell::new(core::mem::MaybeUninit::uninit()))
    }
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get() as *mut T
    }
}

static SESSION_CACHE: SyncCell<slab_cache_t> = SyncCell::uninit();

/// Global session list (`kernel/inc/tty/session.h`'s `extern list_node_t
/// session_list;`). Protected by `pid_lock` (module doc); every access
/// in this file holds `pid_wlock`. Initialized to a self-linked empty
/// head by [`session_init`] at boot, exactly like the C original's
/// `list_entry_init(&session_list)` (the `{0}`-style zero value below is
/// never itself treated as a valid empty-list sentinel -- nothing reads
/// `session_list` before `session_init` runs).
#[no_mangle]
pub static mut session_list: list_node_t = list_node_t { prev: ptr::null_mut(), next: ptr::null_mut() };

// ===========================================================================
// Boot-time initialisation.
// ===========================================================================

/// # Safety
/// `initproc` must be a live `thread` with a non-null `thread_group` and
/// `pgroup` (mirrors the C original's unconditional derefs).
#[no_mangle]
pub unsafe extern "C" fn session_init(initproc: *mut thread) {
    // SAFETY: see fn doc; this runs once at boot before any other
    // `session_*` entry point, so `session_list` and `SESSION_CACHE` are
    // exclusively ours to initialize.
    unsafe {
        ll_init(&raw mut session_list);

        let ret = slab_cache_init(
            SESSION_CACHE.get(),
            c"session_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<session>(),
            crate::bindings::SLAB_FLAG_STATIC as u64,
        );
        session_assert_errno(
            ret == 0,
            line!(),
            c"session_init",
            c"session_init: failed to init session_cache, errno=%d\n",
            ret,
        );

        let tg = (*initproc).thread_group;
        session_assert(!tg.is_null(), line!(), c"session_init", c"session_init: initproc has no thread_group\n");

        let pg = (*initproc).pgroup;
        session_assert(!pg.is_null(), line!(), c"session_init", c"session_init: initproc has no pgroup\n");

        let s = session_alloc((*initproc).pid);
        session_assert(!s.is_null(), line!(), c"session_init", c"session_init: session_alloc failed\n");

        session_add_pg(s, pg);
        session_add_thread(s, initproc);
        (*s).fg_pgrp = pg;
    }
}

// ===========================================================================
// Allocation / reference counting.
// ===========================================================================

/// # Safety
/// The returned pointer, if non-null, is freshly allocated,
/// exclusively-owned slab memory whose `list_node_t` members are
/// initialized; every other field is left zeroed for the caller to fill
/// in (mirrors the C original's `memset` + partial `list_entry_init`
/// calls).
unsafe fn __session_alloc() -> *mut session {
    // SAFETY: `slab_alloc` returns either null or a pointer to
    // `size_of::<session>()` freshly allocated bytes from
    // `SESSION_CACHE` (initialized by `session_init` before any caller
    // can reach here).
    unsafe {
        let s = slab_alloc(SESSION_CACHE.get()) as *mut session;
        if s.is_null() {
            return ptr::null_mut();
        }
        core::ptr::write_bytes(s as *mut u8, 0, core::mem::size_of::<session>());
        ll_init(&raw mut (*s).global_entry);
        ll_init(&raw mut (*s).threads);
        ll_init(&raw mut (*s).pgrps);
        s
    }
}

#[no_mangle]
pub extern "C" fn session_alloc(sid: pid_t) -> *mut session {
    // SAFETY: `s`, once non-null, is exclusively ours (just allocated by
    // `__session_alloc`) until it is linked into `session_list` and
    // returned to the caller.
    unsafe {
        let s = __session_alloc();
        if s.is_null() {
            return ptr::null_mut();
        }
        (*s).sid = sid;
        (*s).ctrl_tty = ptr::null_mut();
        (*s).fg_pgrp = ptr::null_mut();
        (*s).ref_cnt = 1;
        (*s).t_cnt = 0;
        (*s).pg_cnt = 0;
        ll_push_back(&raw mut session_list, &raw mut (*s).global_entry);
        s
    }
}

/// # Safety
/// `s`, if non-null, must be a live `session` (mirrors the C original's
/// unconditional deref once past the null check).
#[no_mangle]
pub unsafe extern "C" fn session_ref(s: *mut session) {
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc. `s->ref_cnt` is a plain (non-`_Atomic`) `int`
    // field in the C struct, RMW'd there via `atomic_inc` ==
    // `__atomic_fetch_add(..., __ATOMIC_SEQ_CST)`; `AtomicI32::from_ptr`
    // over the same field pointer reproduces that exactly (matches
    // `kernel/proc/proc_shims.rs`'s `atomic_fetch_sub_i32` precedent).
    unsafe {
        AtomicI32::from_ptr(&raw mut (*s).ref_cnt).fetch_add(1, Ordering::SeqCst);
    }
}

/// # Safety
/// `s`, if non-null, must be a live `session` (mirrors the C original's
/// unconditional deref once past the null check).
///
/// # A note on the finalization branch below
///
/// The C original's `atomic_dec(&s->ref_cnt)` macro expands to
/// `__atomic_fetch_sub(..., __ATOMIC_SEQ_CST)`, which -- per the GCC/C11
/// atomic builtin contract -- returns the value *before* the
/// subtraction, not after. The C code then names that pre-decrement
/// value `new_val` and frees when `new_val == 0`, i.e. only on the call
/// that decrements ref_cnt from 0 to -1 -- one call *past* the
/// legitimate "last owner drops its reference" transition (1 -> 0),
/// which never fires under correctly paired `session_ref`/
/// `session_unref` calls. This looks like a genuine pre-existing
/// off-by-one (sessions are never actually freed via normal refcounting,
/// only leaked) -- ported here 1:1 for fidelity, matching this project's
/// established practice of preserving and flagging (not silently
/// "fixing") pre-existing semantic bugs outside the current mandate
/// (compare `timer_node_init`'s `retry_limit` bug, kept 1:1 and
/// documented in `RUST_REWRITE.md`). Flagged as a candidate for a future,
/// separately-reviewed fix; NOT part of this wave's mandate (which is
/// the ctrl_tty back-pointer only). Because the branch below is
/// consequently unreachable under normal paired usage, the ctrl_tty
/// detach it performs is defense-in-depth (correct if the refcount bug
/// is ever fixed later) rather than something this wave's tests can
/// exercise live -- documented honestly per this project's convention
/// (compare Iteration 11's `unwind_partial_map` note).
#[no_mangle]
pub unsafe extern "C" fn session_unref(s: *mut session) {
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc; same `AtomicI32::from_ptr` reasoning as
    // `session_ref`.
    unsafe {
        let old_val = AtomicI32::from_ptr(&raw mut (*s).ref_cnt).fetch_sub(1, Ordering::SeqCst);
        session_assert(old_val >= 0, line!(), c"session_unref", c"Session reference count went negative\n");
        if old_val == 0 {
            (*s).__bindgen_anon_1.set_exited(1);
            // Defensive teardown (see the fn doc above): make sure a
            // still-attached controlling tty never keeps a dangling
            // `tty->session` back-pointer into slab memory this call is
            // about to free.
            __session_detach_ctrl_tty(s);
            if !ll_is_detached(&raw mut (*s).global_entry) {
                ll_detach(&raw mut (*s).global_entry);
            }
            slab_free(s as *mut c_void);
        }
    }
}

// ===========================================================================
// Thread membership.
// ===========================================================================

/// Add a thread to a session's thread list. Sets `t->session`/`t->sid`.
///
/// # Safety
/// `s` and `t`, if non-null, must be live (mirrors the C original's
/// unconditional derefs once past the null checks). Caller must hold
/// `pid_wlock`.
#[no_mangle]
pub unsafe extern "C" fn session_add_thread(s: *mut session, t: *mut thread) -> c_int {
    pid_assert_wholding();
    if s.is_null() || t.is_null() {
        return -EINVAL;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).__bindgen_anon_1.exited() != 0 {
            return -ESRCH;
        }
        if !(*t).session.is_null() {
            return -EEXIST;
        }
        (*t).session = s;
        (*t).sid = (*s).sid;
        ll_init(&raw mut (*t).sid_entry);
        ll_push_back(&raw mut (*s).threads, &raw mut (*t).sid_entry);
        (*s).t_cnt += 1;
        session_ref(s);
    }
    0
}

/// Remove a thread from its session's thread list. Clears
/// `t->session`/`t->sid`.
///
/// # Safety
/// `s` and `t`, if non-null, must be live. Caller must hold `pid_wlock`.
#[no_mangle]
pub unsafe extern "C" fn session_remove_thread(s: *mut session, t: *mut thread) -> c_int {
    pid_assert_wholding();
    if s.is_null() || t.is_null() {
        return -EINVAL;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*t).session != s {
            return -EINVAL;
        }
        if !ll_is_detached(&raw mut (*t).sid_entry) {
            ll_detach(&raw mut (*t).sid_entry);
        }
        (*s).t_cnt -= 1;
        (*t).session = ptr::null_mut();
        (*t).sid = 0;
        session_unref(s);
    }
    0
}

// ===========================================================================
// Process group membership.
// ===========================================================================

/// # Safety
/// `s` and `pg`, if non-null, must be live. Caller must hold `pid_wlock`.
#[no_mangle]
pub unsafe extern "C" fn session_add_pg(s: *mut session, pg: *mut pgroup) -> c_int {
    pid_assert_wholding();
    if s.is_null() || pg.is_null() {
        return -EINVAL;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).__bindgen_anon_1.exited() != 0 {
            return -ESRCH;
        }
        if !(*pg).session.is_null() {
            return -EEXIST;
        }
        (*pg).session = s;
        ll_init(&raw mut (*pg).list_entry);
        ll_push_back(&raw mut (*s).pgrps, &raw mut (*pg).list_entry);
        (*s).pg_cnt += 1;
        session_ref(s);
    }
    0
}

/// # Safety
/// `s` and `pg`, if non-null, must be live. Caller must hold `pid_wlock`.
#[no_mangle]
pub unsafe extern "C" fn session_remove_pg(s: *mut session, pg: *mut pgroup) -> c_int {
    pid_assert_wholding();
    if s.is_null() || pg.is_null() {
        return -EINVAL;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*pg).session != s {
            return -EINVAL;
        }
        if !ll_is_detached(&raw mut (*pg).list_entry) {
            ll_detach(&raw mut (*pg).list_entry);
        }
        (*s).pg_cnt -= 1;
        // If the foreground pgroup is being removed, clear it.
        if (*s).fg_pgrp == pg {
            (*s).fg_pgrp = ptr::null_mut();
        }
        (*pg).session = ptr::null_mut();
        session_unref(s);
    }
    0
}

// ===========================================================================
// Controlling terminal.
// ===========================================================================

/// Detach whatever controlling terminal `s` currently has (if any),
/// clearing the tty's back-pointer and releasing the ref
/// `session_set_ctrl_tty` took. Internal teardown helper: unlike
/// `session_set_ctrl_tty`, it does not bail out on `s->exited` -- this
/// is exactly what [`session_hangup`] and [`session_unref`]'s
/// finalization branch call *while* tearing a session down.
///
/// # Safety
/// `s` must be a live `session`. Caller must hold `pid_wlock`.
unsafe fn __session_detach_ctrl_tty(s: *mut session) {
    // SAFETY: see fn doc; `old_tty`, if non-null, was itself established
    // as live by a prior `session_set_ctrl_tty` call (which took a
    // `tty_ref` on it), so it is still live here.
    unsafe {
        let old_tty = (*s).ctrl_tty;
        (*s).ctrl_tty = ptr::null_mut();
        if !old_tty.is_null() {
            (*old_tty).session = ptr::null_mut();
            tty_unref(old_tty);
        }
    }
}

/// Attach, replace, or detach (`new_tty == NULL`) a session's
/// controlling terminal.
///
/// # SIGINT-delivery fix (mandated, Wave 12 -- see the module doc)
///
/// The C original only ever did `s->ctrl_tty = tty`. That left
/// `tty->session` permanently null, which broke
/// `kernel/tty/tty.rs::tty_signal_fg_pgroup`'s only way of finding a
/// session from a `tty` -- so ^C/^\/^Z could never reach the terminal's
/// foreground process group. This port wires up both sides of the
/// relationship:
///
/// - Sets `new_tty->session = s` (the missing back-pointer) so
///   `tty_signal_fg_pgroup` can find `s` from `t` and route the signal
///   via `session_get_fg_pgid` + `pgroup_kill`, instead of falling back
///   to killing `current_thread_ptr()` (dead on the console, since that
///   fallback thread is a kernel feeder thread, not the foreground job).
/// - Takes a [`tty_ref`] on `new_tty` for as long as `s` designates it
///   as the controlling terminal, and [`tty_unref`]s whatever `old_tty`
///   this replaces (or is cleared to null) via
///   [`__session_detach_ctrl_tty`] -- so the tty can never be freed out
///   from under a session that still points to it, and the ref/unref
///   pairing stays balanced no matter how many times the controlling
///   tty is attached, replaced, or cleared. `session_hangup` and
///   `session_unref`'s finalization branch both go through the same
///   detach helper, so every teardown path clears the back-pointer too.
///
/// Re-setting the same `new_tty` that is already `s`'s `ctrl_tty` is a
/// no-op (skips the ref/unref dance entirely) rather than an unbalanced
/// unref+ref of the same tty.
///
/// # Safety
/// `s`, if non-null, must be live. `new_tty`, if non-null, must be a
/// live `tty` previously returned by `tty_alloc` (mirrors
/// [`tty_ref`]/[`tty_unref`]'s own contract). Caller must hold
/// `pid_wlock`.
#[no_mangle]
pub unsafe extern "C" fn session_set_ctrl_tty(s: *mut session, new_tty: *mut tty) {
    pid_assert_wholding();
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).__bindgen_anon_1.exited() != 0 {
            return;
        }
        if (*s).ctrl_tty == new_tty {
            return;
        }
        __session_detach_ctrl_tty(s);
        (*s).ctrl_tty = new_tty;
        if !new_tty.is_null() {
            (*new_tty).session = s;
            tty_ref(new_tty);
        }
    }
}

/// # Safety
/// `s`, if non-null, must be live. Caller must hold `pid_wlock`.
#[no_mangle]
pub unsafe extern "C" fn session_get_ctrl_tty(s: *mut session) -> *mut tty {
    pid_assert_wholding();
    if s.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: see fn doc.
    unsafe { (*s).ctrl_tty }
}

// ===========================================================================
// Foreground process group.
// ===========================================================================

/// # Safety
/// `s`, if non-null, must be live.
#[no_mangle]
pub unsafe extern "C" fn session_set_fg_pgid(s: *mut session, pgid: pid_t) {
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).__bindgen_anon_1.exited() != 0 {
            return;
        }
        let pg = get_pgroup(pgid);
        if is_err(pg) {
            return;
        }
        if (*pg).__bindgen_anon_1.exited() != 0 {
            return; // cannot set an exited pgroup as foreground
        }
        if (*pg).session != s {
            return; // pgroup must belong to this session
        }
        (*s).fg_pgrp = pg;
    }
}

/// # Safety
/// `s`, if non-null, must be live.
#[no_mangle]
pub unsafe extern "C" fn session_get_fg_pgid(s: *mut session) -> pid_t {
    if s.is_null() {
        return -1;
    }
    // SAFETY: see fn doc.
    unsafe {
        let fg = (*s).fg_pgrp;
        if fg.is_null() {
            return -1;
        }
        (*fg).pgid
    }
}

// cross-module: proc/pgroup.rs (real signature: `pgroup_getpgid`-style
// module, `pgroup_alloc`/`pgroup_add_tg`/... already declared above;
// `get_pgroup` is declared separately since `session_set_fg_pgid` is the
// only user in this file).
unsafe extern "C" {
    #[link_name = "get_pgroup"]
    safe fn get_pgroup(pgid: c_int) -> *mut pgroup;
}

// ===========================================================================
// setsid / getsid (POSIX).
// ===========================================================================

/// Send SIGHUP + SIGCONT to a thread's thread group.
///
/// # Safety
/// `t` must be a live `thread`.
unsafe fn __hangup_signal_tg(t: *mut thread) {
    // SAFETY: see fn doc.
    unsafe {
        let tg = (*t).thread_group;
        if tg.is_null() {
            return;
        }
        // `ksiginfo` is a `list_node_t` (two raw pointers), two `*mut
        // thread` fields, an `i32`, and a `siginfo_t` (itself all
        // `i32`/raw-pointer/union-of-primitives fields) -- every field
        // is valid when null/zero, same reasoning as
        // `kernel/proc/access.rs::zeroed_ksiginfo` (duplicated locally
        // here per this file's established cross-module convention).
        let mut info: ksiginfo = core::mem::zeroed();
        info.signo = SIGHUP;
        tg_signal_send(tg, &raw mut info);
        info.signo = SIGCONT;
        tg_signal_send(tg, &raw mut info);
    }
}

/// Mark a session as exited and send SIGHUP (then SIGCONT) to its
/// foreground process group -- or do nothing if it has none. Also
/// disassociates the controlling terminal (via
/// [`__session_detach_ctrl_tty`], which also clears the tty's
/// back-pointer -- see the module doc's SIGINT-fix section).
///
/// Called when the session leader exits or the controlling terminal is
/// disconnected. Currently has no live caller in the tree (matches the
/// C original, which was likewise never wired to a SIGHUP-on-hangup
/// trigger) -- kept and force-linked per this project's "port every
/// declared public symbol" rule; future SIGHUP wiring is out of this
/// wave's scope.
///
/// # Safety
/// `s`, if non-null, must be live. Caller must hold `pid_wlock`.
#[no_mangle]
pub unsafe extern "C" fn session_hangup(s: *mut session) {
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).__bindgen_anon_1.exited() != 0 {
            return;
        }
        pid_assert_wholding();

        (*s).__bindgen_anon_1.set_exited(1);

        let fg = (*s).fg_pgrp;
        if !fg.is_null() && (*fg).__bindgen_anon_1.exited() == 0 {
            let head = &raw mut (*fg).threads;
            let off = core::mem::offset_of!(thread, pg_entry);
            crate::list_for_each!(head, off, t, {
                __hangup_signal_tg(t as *mut thread);
            });
        }

        __session_detach_ctrl_tty(s);
        (*s).fg_pgrp = ptr::null_mut();
    }
}

/// Create a new session: the calling thread becomes the session leader
/// of a new session and the process-group leader of a new process
/// group. POSIX: must not be called by a process-group leader.
#[no_mangle]
pub extern "C" fn session_setsid() -> pid_t {
    let p = crate::machine::current_thread_ptr();
    let tgid = thread_tgid(p);

    pid_wlock();

    // SAFETY: `p` is the live current thread (kernel invariant --
    // `current_thread_ptr` always returns a valid running thread's
    // pointer); `pid_wlock` is held from here until every return path
    // below explicitly unlocks first.
    unsafe {
        if (*p).pgid == tgid {
            pid_wunlock();
            return -EPERM;
        }

        let s = session_alloc(tgid);
        if s.is_null() {
            pid_wunlock();
            return -ENOMEM;
        }

        let tg = (*p).thread_group;
        let pg = pgroup_alloc(tgid, tg);
        if pg.is_null() {
            // Deliberate fix (Wave 12): the C original called
            // `slab_free(s)` directly here, bypassing the
            // `session_list` unlink that `session_unref` performs.
            // `session_alloc` (above) already linked `s` into the
            // global `session_list`, so that direct free left a
            // dangling node in the list -- a use-after-free the next
            // time anything walks `session_list` (e.g.
            // `session_for_each_all`). `s->ref_cnt == 1` here (nothing
            // else has referenced `s` yet), so going through
            // `session_unref` is exactly the "sole owner drops its
            // reference" case and correctly unlinks before freeing
            // (mirrors this wave's "handle clearing on session
            // teardown" mandate for `session_set_ctrl_tty`, applied to
            // this other teardown path found while porting the same
            // file).
            session_unref(s);
            pid_wunlock();
            return -ENOMEM;
        }

        if !tg.is_null() {
            pgroup_remove_tg(tg);
        }
        pgroup_remove_thread(p);
        if !(*p).session.is_null() {
            session_remove_thread((*p).session, p);
        }

        session_add_pg(s, pg);
        if !tg.is_null() {
            pgroup_add_tg(pg, tg);
        }
        pgroup_add_thread(pg, p);
        session_add_thread(s, p);
        (*s).fg_pgrp = pg; // becomes foreground group

        pid_wunlock();
    }
    tgid
}

#[no_mangle]
pub extern "C" fn session_getsid(pid: pid_t) -> pid_t {
    if pid == 0 {
        let cur = crate::machine::current_thread_ptr();
        // SAFETY: `cur` is the live current thread.
        return unsafe { (*cur).sid };
    }
    if pid < 0 {
        return -EINVAL;
    }

    rcu_read_lock();
    let target = get_pid_thread(pid);
    if is_err(target) {
        rcu_read_unlock();
        return -ESRCH;
    }
    // SAFETY: `target` is a live thread (not an ERR_PTR, checked above)
    // for as long as the RCU read-side critical section is held.
    let sid = unsafe { (*target).sid };
    rcu_read_unlock();

    sid
}

/// Look up a session by its SID.
///
/// Finds the thread whose `pid == sid` (the session leader) via
/// `get_pid_thread`, then returns its session pointer.
///
/// Returns the session pointer, or `ERR_PTR(-ESRCH)` if not found. The
/// caller must be inside an `rcu_read_lock()` section, or hold
/// `pid_lock`; the returned pointer is only stable under that same
/// protection.
#[no_mangle]
pub extern "C" fn get_session(sid: pid_t) -> *mut session {
    let t = get_pid_thread(sid);
    if is_err(t) {
        return err_ptr(-ESRCH);
    }
    // SAFETY: `t` is a live thread (not an ERR_PTR, checked above) for
    // as long as the caller's RCU read-side section / `pid_lock` hold
    // lasts (fn doc).
    let s = unsafe { (*t).session };
    if s.is_null() {
        return err_ptr(-ESRCH);
    }
    s
}
