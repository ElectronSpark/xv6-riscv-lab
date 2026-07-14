//! Pure-Rust port of `kernel/proc/thread_group.c`.
//!
//! Owns the canonical public ABI symbols `thread_group_init`,
//! `thread_group_get/put/live_dec`, `get_thread_group`,
//! `tg_shared_pending_init/destroy`, `thread_group_alloc[/_kernel]`,
//! `thread_group_add/remove`, `thread_is_group_leader`, `thread_tgid`,
//! `thread_group_exit`, `tg_signal_send`, `tg_signal_pending`,
//! `tg_dequeue_signal`, `tg_sigpending_empty`, `tg_recalc_sigpending`,
//! and the `xv6_tgport_*` aliases that earlier sections call.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

// See `crate::u`'s doc comment (kernel/lib.rs) for the macro's contract.
use crate::u;

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::Ordering;

use crate::bindings::{
    ksiginfo as ksiginfo_t, list_node_t, pid_t, sigacts as sigacts_t,
    sigpending as sigpending_t, slab_alloc, slab_cache_t, slab_cache_init, slab_free, thread,
    thread_group, thread_state,
};
use crate::machine::{cpuid, CpuLocal};
use crate::proc::access::{
    KsigInfoAccess, SchedEntityRef, SigPendingRef, ThreadAccess, ThreadGroupAccess,
    ThreadSignalAccess, err_ptr, is_err, list_container_of as container_of,
    list_node_detach_raw, list_node_init_raw, list_node_is_detached_raw, list_node_is_empty_raw,
    list_node_next_raw, list_node_push_back_raw,
};
use crate::proc::ksiginfo_alloc;
use crate::proc::xv6_sigport_ksiginfo_free;
use crate::proc::xv6_sigport_sigacts_lock;
use crate::proc::xv6_sigport_sigacts_unlock;
use crate::ipi::ipi_send_single;

// ---------------- constants ----------------------------------------------
const SLAB_FLAG_STATIC: u64 = 1 << 1;
const TG_MAX_SIGINFO_PER_SIGNAL: c_int = 8;
const NSIG: usize = 32;
const SIGKILL: c_int = 9;
const SA_SIGINFO: c_int = 0x00000004;
const CPU_FLAG_NEEDS_RESCHED: u64 = 1;
const IPI_REASON_RESCHEDULE: c_int = 2;

const EINVAL: c_int = 22;
const ENOMEM: c_int = 12;
const EPERM: c_int = 1;
const ESRCH: c_int = 3;

const THREAD_FLAG_KILLED: u64 = 2;
const THREAD_FLAG_SIGPENDING: u64 = 4;

const THREAD_UNUSED: thread_state = 0;
const THREAD_INTERRUPTIBLE: thread_state = 2;
const THREAD_KIILABLE: thread_state = 3;
const THREAD_TIMER: thread_state = 4;
const THREAD_KIILABLE_TIMER: thread_state = 5;
const THREAD_UNINTERRUPTIBLE: thread_state = 6;
const THREAD_RUNNING: thread_state = 8;
const THREAD_STOPPED: thread_state = 9;
const THREAD_ZOMBIE: thread_state = 11;

// ---------------- extern C primitives -----------------------------------
unsafe extern "C" {
    fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;

    // panic / printf
    safe fn xv6_panic(msg: *const c_char) -> !;

    // thread group needs these from sibling subsystems
    safe fn get_pid_thread(pid: c_int) -> *mut thread;
    safe fn pgroup_remove_tg(tg: *mut thread_group);
    safe fn exit(code: c_int) -> !;

    // pid global rwlock helpers (Rust shims)
    safe fn xv6_pid_rlock();
    safe fn xv6_pid_runlock();
    safe fn xv6_pid_wholding() -> c_int;

    // signal helpers

    // scheduler wake helpers
    safe fn xv6_schport_scheduler_wakeup_interruptible(p: *mut thread);
    safe fn xv6_schport_scheduler_wakeup_stopped(p: *mut thread);

    // cpu id (trampoline from slab_shims.rs)
}

// Field offsets used for container_of.
#[inline]
fn tg_entry_offset_in_thread() -> usize {
    core::mem::offset_of!(thread, tg_entry)
}
#[inline]
fn list_entry_offset_in_ksiginfo() -> usize {
    core::mem::offset_of!(ksiginfo_t, list_entry)
}

#[inline(always)]
fn ta_of<'a>(p: *mut thread) -> Option<ThreadAccess<'a>> {
    ThreadAccess::from_ptr(p)
}

// thread flag helpers
#[inline]
fn thread_set_killed(t: *mut thread) {
    if let Some(ta) = ta_of(t) { ta.flags_set_bit(THREAD_FLAG_KILLED); }
}
#[inline]
fn thread_set_sigpending(t: *mut thread) {
    if let Some(ta) = ta_of(t) { ta.flags_set_bit(THREAD_FLAG_SIGPENDING); }
}
#[inline]
fn thread_clear_sigpending(t: *mut thread) {
    if let Some(ta) = ta_of(t) { ta.flags_clear_bit(THREAD_FLAG_SIGPENDING); }
}

// thread state predicates
#[inline]
fn thread_state_load(t: *mut thread) -> thread_state {
    ta_of(t).map_or(THREAD_UNUSED, |ta| ta.state_load() as thread_state)
}
#[inline]
fn thread_is_zombie_state(t: *mut thread) -> bool {
    thread_state_load(t) == THREAD_ZOMBIE
}
#[inline]
fn THREAD_SLEEPING(t: *mut thread) -> bool {
    if t.is_null() { return false; }
    let st = thread_state_load(t);
    st == THREAD_INTERRUPTIBLE
        || st == THREAD_UNINTERRUPTIBLE
        || st == THREAD_KIILABLE
        || st == THREAD_TIMER
        || st == THREAD_KIILABLE_TIMER
}
#[inline]
fn THREAD_STOPPED_p(t: *mut thread) -> bool {
    !t.is_null() && thread_state_load(t) == THREAD_STOPPED
}
#[inline]
fn THREAD_RUNNING_p(t: *mut thread) -> bool {
    !t.is_null() && thread_state_load(t) == THREAD_RUNNING
}
#[inline]
fn THREAD_INTERRUPTIBLE_p(t: *mut thread) -> bool {
    !t.is_null() && thread_state_load(t) == THREAD_INTERRUPTIBLE
}
#[inline]
fn THREAD_ZOMBIE_p(t: *mut thread) -> bool {
    !t.is_null() && thread_state_load(t) == THREAD_ZOMBIE
}

// sigset helpers (sigset_t is u64)
type sigset_t = u64;
#[inline]
fn sigbad(signo: c_int) -> bool { signo < 1 || signo > NSIG as c_int }
#[inline]
fn sigismember(set: sigset_t, signo: c_int) -> bool {
    if sigbad(signo) { return false; }
    (set & (1u64 << (signo - 1))) != 0
}
#[inline]
fn sigaddset(set: &mut sigset_t, signo: c_int) {
    if !sigbad(signo) { *set |= 1u64 << (signo - 1); }
}
#[inline]
fn sigdelset(set: &mut sigset_t, signo: c_int) {
    if !sigbad(signo) { *set &= !(1u64 << (signo - 1)); }
}

// SET_NEEDS_RESCHED: mycpu()->flags |= CPU_FLAG_NEEDS_RESCHED.
// mycpu() = (struct cpu_local *)r_tp().
#[inline]
unsafe fn set_needs_resched() {
    CpuLocal::current().flags_or(CPU_FLAG_NEEDS_RESCHED);
}

// ---------------- list_foreach over tg_entry / list_entry ---------------
/// Iterate the thread_list of a thread_group (entries are `tg_entry` in
/// struct thread). Yields *mut thread for each element. Safe under
/// concurrent removal of the *current* node (Linux `list_foreach_node_safe`
/// semantics: caches the next pointer before invoking the body).
#[inline]
fn for_each_tg_thread<F: FnMut(*mut thread)>(head: *mut list_node_t, mut f: F) {
    let off = tg_entry_offset_in_thread();
    crate::list_for_each!(head, off, t, {
        f(t as *mut thread);
    });
}

/// Count entries on a sigpending queue (header list_node_t with embedded
/// ksiginfo entries linked via `list_entry`).
fn siginfo_queue_len(sq: *mut sigpending_t) -> c_int {
    // SAFETY: `sq`'s sole call site (`tg_signal_send`, below) passes the address of an
    // in-bounds element of the fixed-size `sig_pending` array embedded in an
    // already-valid `shared` struct; never null.
    unsafe { SigPendingRef::assume(sq) }.queue_len()
}

// ---------------- slab cache storage ------------------------------------
#[repr(transparent)]
struct CacheCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: `TG_POOL` is written in full by `slab_cache_init` (called
// once from `thread_group_init` below, before any `slab_alloc` on this
// cache) and otherwise only accessed through the C slab allocator's
// own internally-synchronized primitives (`slab_alloc`/`slab_free`),
// which serialize concurrent access via the cache's embedded locks.
unsafe impl Sync for CacheCell {}
static TG_POOL: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));
#[inline]
fn tg_pool() -> *mut slab_cache_t { TG_POOL.0.get() as *mut slab_cache_t }

// ===========================================================================
// SECTION 15.1  subsystem init
// ===========================================================================
#[no_mangle]
pub extern "C" fn thread_group_init(initproc: *mut thread) {
    // SAFETY: `slab_cache_init` is a bindgen `unsafe extern "C"`
    // function. `tg_pool()` points into the static `TG_POOL` cell,
    // which is valid for `'static` and not yet visible to any other
    // thread this early in boot, so passing it (with a `'static` name
    // pointer and the correctly computed `size_of::<thread_group>()`)
    // is sound.
    u! {
        slab_cache_init(
            tg_pool(),
            c"thread_group".as_ptr() as *mut c_char,
            core::mem::size_of::<thread_group>(),
            SLAB_FLAG_STATIC as _,
        );
        let ret = thread_group_alloc(initproc);
        if ret != 0 {
            xv6_panic(c"thread_group_init: thread_group_alloc failed".as_ptr());
        }
    }
}

// ===========================================================================
// SECTION 15.2  reference counting
// ===========================================================================
#[no_mangle]
pub extern "C" fn thread_group_get(tg: *mut thread_group) {
    thread_group_get_impl(tg)
}

fn thread_group_get_impl(tg: *mut thread_group) {
    if tg.is_null() { return; }
    if let Some(tga) = ThreadGroupAccess::from_ptr(tg) { tga.refcount_fetch_add(1); }
}

#[no_mangle]
pub extern "C" fn thread_group_put(tg: *mut thread_group) {
    if tg.is_null() { return; }
    // SAFETY: `tg` is checked non-null above, and `refcount_dec_unless`
    // returning `false` means this call observed (and dropped) the
    // last reference, so no other holder can still be using `tg`;
    // `slab_free` (bindgen `unsafe extern "C"`) may then reclaim it.
    u! {
        let tga = ThreadGroupAccess::assume(tg);
        if tga.refcount_dec_unless(1) {
            // refcount > 1, still alive
            return;
        }
        tg_shared_pending_destroy(tg);
        slab_free(tg as *mut c_void);
    }
}

#[no_mangle]
pub extern "C" fn thread_group_live_dec(tg: *mut thread_group) {
    if tg.is_null() { return; }
    // atomic_dec returns the *new* value via sub_fetch in C; original
    // assert was "non-negative". Use fetch_sub and check previous > 0.
    // SAFETY: `tg` is checked non-null at the top of `thread_group_live_dec` above.
    let prev = unsafe { ThreadGroupAccess::assume(tg) }.live_threads_fetch_sub(1);
    if prev <= 0 {
        xv6_panic(c"Thread group live thread count went negative".as_ptr());
    }
}

#[no_mangle]
pub extern "C" fn get_thread_group(tgid: pid_t) -> *mut thread_group {
    let t = get_pid_thread(tgid);
    if is_err(t) { return err_ptr(-ESRCH); }
    if t.is_null() { return err_ptr(-ESRCH); }
    // SAFETY: `t` is checked non-null (`is_err(t)` / `t.is_null()`) immediately above.
    let ta = unsafe { ThreadAccess::assume(t) };
    let tg = ta.thread_group_ptr();
    if tg.is_null() { return err_ptr(-ESRCH); }
    tg
}

// ===========================================================================
// SECTION 15.3  shared pending signal helpers
// ===========================================================================
#[no_mangle]
pub extern "C" fn tg_shared_pending_init(tg: *mut thread_group) {
    if tg.is_null() {
        xv6_panic(c"tg_shared_pending_init: NULL".as_ptr());
    }
    // SAFETY: `tg` is proven non-null by the diverging `xv6_panic` null check above.
    let sp = unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref();
    sp.set_sig_pending_mask(0);
    for i in 0..NSIG {
        sp.sig_pending_ref_index(i).queue_ref().init();
    }
}

#[no_mangle]
pub extern "C" fn tg_shared_pending_destroy(tg: *mut thread_group) {
    if tg.is_null() { return; }
    // SAFETY: `tg` is checked non-null at the top of `tg_shared_pending_destroy`
    // above.
    let sp = unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref();
    let ksi_off = list_entry_offset_in_ksiginfo();
    for i in 0..NSIG {
        let head = sp.sig_pending_ref_index(i).queue_ptr();
        let mut cur = list_node_next_raw(head);
        while cur != head {
            let next = list_node_next_raw(cur);
            let ksi = container_of::<ksiginfo_t>(cur, ksi_off);
            // SAFETY: `ksi` is obtained via `container_of` from a live node of the
            // shared-pending queue, which is only ever populated with real
            // `ksiginfo` entries allocated by `ksiginfo_alloc`.
            unsafe { KsigInfoAccess::assume(ksi) }.list_entry_ref().detach();
            xv6_sigport_ksiginfo_free(ksi);
            cur = next;
        }
    }
    sp.set_sig_pending_mask(0);
}

// ===========================================================================
// SECTION 15.4  thread group lifecycle
// ===========================================================================
#[no_mangle]
pub extern "C" fn thread_group_alloc(leader: *mut thread) -> c_int {
    if leader.is_null() {
        xv6_panic(c"thread_group_alloc: NULL leader".as_ptr());
    }
    // SAFETY: `leader` is checked non-null above (panics otherwise).
    // `slab_alloc`/`memset` are bindgen `unsafe extern "C"` functions;
    // `tg` is null-checked immediately after allocation, and before
    // `memset` writes exactly `size_of::<thread_group>()` zeroed bytes
    // into it, so the subsequent `ThreadGroupAccess::assume(tg)`
    // operates on a valid, freshly zero-initialized `thread_group`.
    u! {
        let tg = slab_alloc(tg_pool()) as *mut thread_group;
        if tg.is_null() { return -ENOMEM; }
        memset(tg as *mut c_void, 0, core::mem::size_of::<thread_group>());
        let lta = ThreadAccess::assume(leader);
        let tga = ThreadGroupAccess::assume(tg);
        tga.thread_list_ref().init();
        tga.list_entry_ref().init();
        tga.set_group_leader(leader);
        tga.set_tgid(lta.pid());
        tga.live_threads_store(1);
        tga.refcount_store(1);
        tga.group_exit_store(0);
        tga.set_group_exit_code(0);
        tga.set_group_exit_task(ptr::null_mut());
        tga.set_group_stop_count(0);
        tga.set_group_stop_signo(0);
        tga.set_pgroup(ptr::null_mut());
        tga.set_is_kernel(0);

        tg_shared_pending_init(tg);

        // Link leader into the group
        lta.set_thread_group(tg);
        lta.set_tgid(lta.pid());
        lta.tg_entry_ref().init();
        lta.tg_entry_ref().push_back(tga.thread_list_ptr());
        0
    }
}

#[no_mangle]
pub extern "C" fn thread_group_alloc_kernel(
    out_tg: *mut *mut thread_group,
    tgid: pid_t,
) -> c_int {
    if out_tg.is_null() { return -EINVAL; }
    // SAFETY: `out_tg` is checked non-null above; `slab_alloc`/`memset`
    // are bindgen `unsafe extern "C"` functions, and `tg` is
    // null-checked before `memset` zero-initializes exactly
    // `size_of::<thread_group>()` bytes, so `ThreadGroupAccess::assume(tg)`
    // sees a valid struct and the final `*out_tg = tg` writes through
    // the caller-supplied non-null out-pointer.
    u! {
        let tg = slab_alloc(tg_pool()) as *mut thread_group;
        if tg.is_null() { return -ENOMEM; }
        memset(tg as *mut c_void, 0, core::mem::size_of::<thread_group>());
        let tga = ThreadGroupAccess::assume(tg);
        tga.thread_list_ref().init();
        tga.list_entry_ref().init();
        tga.set_group_leader(ptr::null_mut());
        tga.set_tgid(tgid);
        tga.live_threads_store(0);
        tga.refcount_store(1);
        tga.group_exit_store(0);
        tga.set_group_exit_code(0);
        tga.set_group_exit_task(ptr::null_mut());
        tga.set_group_stop_count(0);
        tga.set_group_stop_signo(0);
        tga.set_pgroup(ptr::null_mut());
        tga.set_is_kernel(1);
        tg_shared_pending_init(tg);
        *out_tg = tg;
        0
    }
}

// Caller must hold pid_wlock.
#[no_mangle]
pub extern "C" fn thread_group_add(tg: *mut thread_group, child: *mut thread) {
    if tg.is_null() {
        xv6_panic(c"thread_group_add: NULL tg".as_ptr());
    }
    if child.is_null() {
        xv6_panic(c"thread_group_add: NULL child".as_ptr());
    }
    // SAFETY: `child` is proven non-null by the diverging `xv6_panic` null check
    // above.
    let cta = unsafe { ThreadAccess::assume(child) };
    // SAFETY: `tg` is proven non-null by the diverging `xv6_panic` null check above.
    let tga = unsafe { ThreadGroupAccess::assume(tg) };
    if !cta.thread_group_ptr().is_null() {
        xv6_panic(c"thread_group_add: child already in a group".as_ptr());
    }
    if xv6_pid_wholding() == 0 {
        xv6_panic(c"thread_group_add: caller must hold pid_wlock".as_ptr());
    }
    cta.set_thread_group(tg);
    cta.set_tgid(tga.tgid());
    cta.tg_entry_ref().init();
    cta.tg_entry_ref().push_back(tga.thread_list_ptr());
    thread_group_get_impl(tg);
    if !THREAD_ZOMBIE_p(child) {
        tga.live_threads_fetch_add(1);
    }
}

// Caller must hold pid_wlock.
#[no_mangle]
pub extern "C" fn thread_group_remove(p: *mut thread) -> bool {
    if p.is_null() { return true; }
    // SAFETY: `p` is checked non-null at the top of `thread_group_remove` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    let tg = ta.thread_group_ptr();
    if tg.is_null() { return true; }
    if xv6_pid_wholding() == 0 {
        xv6_panic(c"thread_group_remove: caller must hold pid_wlock".as_ptr());
    }
    let mut last = false;
    if !ta.tg_entry_ref().is_detached() {
        ta.tg_entry_ref().detach();
    }
    // atomic_sub returns the *previous* value
    // SAFETY: `tg` is checked non-null immediately above (`if tg.is_null() { return
    // true; }`).
    let prev = unsafe { ThreadGroupAccess::assume(tg) }.live_threads_fetch_sub(1);
    if prev <= 1 { last = true; }
    if last {
        pgroup_remove_tg(tg);
    }
    last
}

// ===========================================================================
// SECTION 15.5  queries
// ===========================================================================
#[no_mangle]
pub extern "C" fn thread_is_group_leader(p: *mut thread) -> bool {
    if p.is_null() { return true; }
    // SAFETY: `p` is checked non-null at the top of `thread_is_group_leader` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    let tg = ta.thread_group_ptr();
    if tg.is_null() { return true; }
    // SAFETY: `tg` is checked non-null immediately above (`if tg.is_null() { return
    // true; }`).
    unsafe { ThreadGroupAccess::assume(tg) }.group_leader_ptr() == p
}

#[no_mangle]
pub extern "C" fn thread_tgid(p: *mut thread) -> c_int {
    if p.is_null() { return -1; }
    // SAFETY: `p` is checked non-null at the top of `thread_tgid` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    let tg = ta.thread_group_ptr();
    if tg.is_null() { return ta.pid(); }
    // SAFETY: `tg` is checked non-null immediately above (`if tg.is_null() { return
    // ta.pid(); }`).
    let tga = unsafe { ThreadGroupAccess::assume(tg) };
    let tgid = tga.tgid();
    if tgid > 0 { tgid } else { ta.pid() }
}

// ===========================================================================
// SECTION 15.6  group exit
// ===========================================================================
#[no_mangle]
pub extern "C" fn thread_group_exit(p: *mut thread, code: c_int) {
    if p.is_null() { return; }
    // SAFETY: `p` is checked non-null above; `tg` is checked non-null
    // immediately after being read from `ta.thread_group_ptr()`, so it
    // is a live `thread_group` when passed to the `unsafe fn
    // tg_sigkill_all`, which requires exactly that.
    u! {
        let ta = ThreadAccess::assume(p);
        let tg = ta.thread_group_ptr();
        if tg.is_null() { exit(code); }
        let tga = ThreadGroupAccess::assume(tg);
        // CAS group_exit_task from NULL to p; only the first wins.
        let cas_ok = tga.group_exit_task_compare_exchange_null(p);
        if !cas_ok {
            exit(code);
        }
        tga.set_group_exit_code(code);
        tga.set_group_exit_task(p);
        // Broadcast SIGKILL to siblings (skip self).
        tg_sigkill_all(tg, p);
        exit(code);
    }
}

// ===========================================================================
// SECTION 15.7  signal delivery — helpers
// ===========================================================================
unsafe fn tg_sigkill_all(tg: *mut thread_group, skip: *mut thread) {
    // SAFETY: callers of this `unsafe fn` (`thread_group_exit`,
    // `tg_signal_send`) guarantee `tg` is a live, non-null
    // `*mut thread_group`, so projecting `&raw mut (*tg).thread_list`
    // is sound; `for_each_tg_thread` only reads the resulting list
    // head.
    u! {
        xv6_pid_rlock();
        for_each_tg_thread(&raw mut (*tg).thread_list, |t| {
            if t == skip { return; }
            thread_set_killed(t);
            thread_set_sigpending(t);
            if THREAD_SLEEPING(t) {
                xv6_schport_scheduler_wakeup_interruptible(t);
            } else if THREAD_STOPPED_p(t) {
                xv6_schport_scheduler_wakeup_stopped(t);
            }
        });
        xv6_pid_runlock();
    }
}

/// Pick an eligible thread from the group to handle a signal.
unsafe fn tg_pick_thread(tg: *mut thread_group, signo: c_int) -> *mut thread {
    if tg.is_null() { return ptr::null_mut(); }
    // SAFETY: `tg` is checked non-null above, so
    // `ThreadGroupAccess::from_raw(tg)` returns `Some` and
    // `unwrap_unchecked` is sound. `leader` is guarded by
    // `!leader.is_null()` before every deref/`from_raw` use.
    // `for_each_tg_thread` yields only live `*mut thread` nodes linked
    // on `tg`'s `thread_list` via `tg_entry`, so each `t` is non-null
    // and valid for `from_raw`/field access.
    u! {
        let tga = crate::proc::access::ThreadGroupAccess::from_raw(tg).unwrap_unchecked();
        let leader = tga.group_leader_ptr();
        if !leader.is_null() {
            let lta = crate::proc::access::ThreadAccess::from_raw(leader).unwrap_unchecked();
            if !lta.sigacts_ptr().is_null() {
                let mask = (*leader).signal.sig_mask;
                let st = thread_state_load(leader);
                if !sigismember(mask, signo) && st != THREAD_ZOMBIE && st != THREAD_UNUSED {
                    return leader;
                }
            }
        }
        let mut chosen: *mut thread = ptr::null_mut();
        for_each_tg_thread(&raw mut (*tg).thread_list, |t| {
            if !chosen.is_null() { return; }
            if t == leader { return; }
            let st = thread_state_load(t);
            if st == THREAD_UNUSED || st == THREAD_ZOMBIE { return; }
            let tt = crate::proc::access::ThreadAccess::from_raw(t).unwrap_unchecked();
            if tt.sigacts_ptr().is_null() { return; }
            if !sigismember((*t).signal.sig_mask, signo) {
                chosen = t;
            }
        });
        if !chosen.is_null() { return chosen; }
        leader
    }
}

// ===========================================================================
// SECTION 15.8  tg_signal_send / pending / dequeue / sigpending_empty
// ===========================================================================
#[no_mangle]
pub extern "C" fn tg_signal_send(tg: *mut thread_group, info: *mut ksiginfo_t) -> c_int {
    if tg.is_null() || info.is_null() { return -EINVAL; }
    // SAFETY: `tg` and `info` are checked non-null above, so
    // `(*info).signo` and `(*tg).__bindgen_anon_1` are valid.
    // `sigbad` bounds `signo` to `1..=NSIG` before any array indexing.
    // `leader` and `sigacts` are each null-checked immediately before
    // the first deref that uses them (`tga.group_leader_ptr()` /
    // `lta.sigacts_ptr()`), and `sigacts` stays live for the duration
    // of this block because we hold `xv6_sigport_sigacts_lock(sigacts)`
    // across all subsequent field accesses through it, released only
    // just before each early return.
    u! {
        if sigbad((*info).signo) { return -EINVAL; }
        if (*tg).__bindgen_anon_1.is_kernel() != 0 { return -EPERM; }
        let signo = (*info).signo;

        let tga = ThreadGroupAccess::assume(tg);
        let shared = tga.shared_pending_ref();
        if tga.live_threads_load_acquire() <= 0 {
            return -ESRCH;
        }

        if signo == SIGKILL {
            tg_sigkill_all(tg, ptr::null_mut());
            return 0;
        }

        xv6_pid_rlock();
        let leader = tga.group_leader_ptr();
        if leader.is_null() {
            xv6_pid_runlock();
            return -ESRCH;
        }
        let lta = crate::proc::access::ThreadAccess::from_raw(leader).unwrap_unchecked();
        if lta.sigacts_ptr().is_null() {
            xv6_pid_runlock();
            return -ESRCH;
        }
        let sigacts = lta.sigacts_ptr();
        xv6_sigport_sigacts_lock(sigacts);

        let stop_mask = (*sigacts).sa_sigstop;
        let cont_mask = (*sigacts).sa_sigcont;

        if sigismember((*sigacts).sa_sigignore, signo) {
            xv6_sigport_sigacts_unlock(sigacts);
            xv6_pid_runlock();
            return 0;
        }

        let is_cont = sigismember(cont_mask, signo);
        let is_stop = sigismember(stop_mask, signo);
        let is_term = sigismember((*sigacts).sa_sigterm, signo);

        if is_cont {
            shared.and_sig_pending_mask(!stop_mask);
            for_each_tg_thread(tga.thread_list_ptr(), |t| {
                ThreadSignalAccess::assume_thread(t).and_sig_pending_mask(!stop_mask);
            });
        }
        if is_stop {
            shared.and_sig_pending_mask(!cont_mask);
            for_each_tg_thread(tga.thread_list_ptr(), |t| {
                ThreadSignalAccess::assume_thread(t).and_sig_pending_mask(!cont_mask);
            });
        }

        let act = &raw mut (*sigacts).sa[signo as usize];
        if ((*act).sa_flags & SA_SIGINFO) != 0 {
            let sq = shared.sig_pending_ptr_index((signo - 1) as usize);
            let qlen = siginfo_queue_len(sq);
            if qlen >= TG_MAX_SIGINFO_PER_SIGNAL {
                let head = SigPendingRef::assume(sq).queue_ptr();
                if !list_node_is_empty_raw(head) {
                    let first = list_node_next_raw(head);
                    let old = container_of::<ksiginfo_t>(first, list_entry_offset_in_ksiginfo());
                    if !old.is_null() {
                        KsigInfoAccess::assume(old).list_entry_ref().detach();
                        xv6_sigport_ksiginfo_free(old);
                    }
                }
            }
            let ksi = ksiginfo_alloc();
            if !ksi.is_null() {
                let ka = KsigInfoAccess::assume(ksi);
                ka.copy_from(info);
                ka.list_entry_ref().init();
                ka.list_entry_ref().push_back(SigPendingRef::assume(sq).queue_ptr());
            }
        } else {
            if sigismember(shared.sig_pending_mask(), signo) && !is_cont {
                xv6_sigport_sigacts_unlock(sigacts);
                xv6_pid_runlock();
                return 0;
            }
        }

        shared.or_sig_pending_mask(1u64 << (signo - 1));

        let mut target: *mut thread = ptr::null_mut();
        if !is_cont {
            target = tg_pick_thread(tg, signo);
            if !target.is_null() {
                thread_set_sigpending(target);
            }
        }

        xv6_sigport_sigacts_unlock(sigacts);

        if is_cont {
            for_each_tg_thread(tga.thread_list_ptr(), |t| {
                thread_set_sigpending(t);
                if THREAD_STOPPED_p(t) {
                    xv6_schport_scheduler_wakeup_stopped(t);
                } else if THREAD_INTERRUPTIBLE_p(t) {
                    xv6_schport_scheduler_wakeup_interruptible(t);
                }
            });
        } else if !target.is_null() {
            if is_term && THREAD_STOPPED_p(target) {
                xv6_schport_scheduler_wakeup_stopped(target);
            } else if THREAD_INTERRUPTIBLE_p(target) {
                xv6_schport_scheduler_wakeup_interruptible(target);
            } else if THREAD_RUNNING_p(target) {
                let se = ThreadAccess::assume(target).sched_entity_ptr();
                let target_cpu = SchedEntityRef::assume(se).cpu_id_load_acquire();
                if target_cpu != cpuid() {
                    ipi_send_single(target_cpu, IPI_REASON_RESCHEDULE);
                } else {
                    set_needs_resched();
                }
            }
        }

        xv6_pid_runlock();
        0
    }
}

#[no_mangle]
pub extern "C" fn tg_signal_pending(tg: *mut thread_group, p: *mut thread) -> bool {
    if tg.is_null() || p.is_null() { return false; }
    // SAFETY: `p` is checked non-null at the top of `tg_signal_pending` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    if ta.sigacts_ptr().is_null() { return false; }
    // SAFETY: `tg` is checked non-null at the top of `tg_signal_pending` above.
    let shared = unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref().sig_pending_mask_atomic_load();
    // SAFETY: `p` is checked non-null at the top of `tg_signal_pending` above.
    let blocked = unsafe { ThreadSignalAccess::assume_thread(p) }.sig_mask();
    (shared & !blocked) != 0
}

// Caller must hold sigacts lock and pid_rlock (or pid_wlock).
#[no_mangle]
pub extern "C" fn tg_dequeue_signal(tg: *mut thread_group, signo: c_int) -> *mut ksiginfo_t {
    if tg.is_null() || sigbad(signo) { return ptr::null_mut(); }
    // SAFETY: `tg` is checked non-null at the top of `tg_dequeue_signal` above.
    let shared = unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref();
    let sq = shared.sig_pending_ref_index((signo - 1) as usize);
    let head = sq.queue_ptr();
    let mut ksi: *mut ksiginfo_t = ptr::null_mut();
    if !list_node_is_empty_raw(head) {
        let first = list_node_next_raw(head);
        ksi = container_of::<ksiginfo_t>(first, list_entry_offset_in_ksiginfo());
        // SAFETY: `ksi` is obtained via `container_of` from the just-verified non-
        // empty (`!list_node_is_empty_raw(head)`) queue, so it is a live
        // `ksiginfo` entry.
        unsafe { KsigInfoAccess::assume(ksi) }.list_entry_ref().detach();
    }
    if list_node_is_empty_raw(head) {
        shared.and_sig_pending_mask(!(1u64 << (signo - 1)));
    }
    ksi
}

// Caller must hold sigacts lock.
#[no_mangle]
pub extern "C" fn tg_sigpending_empty(tg: *mut thread_group, signo: c_int) {
    if tg.is_null() || sigbad(signo) { return; }
    // SAFETY: `tg` is checked non-null at the top of `tg_sigpending_empty` above.
    let shared = unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref();
    let head = shared.sig_pending_ref_index((signo - 1) as usize).queue_ptr();
    let ksi_off = list_entry_offset_in_ksiginfo();
    let mut cur = list_node_next_raw(head);
    while cur != head {
        let next = list_node_next_raw(cur);
        let ksi = container_of::<ksiginfo_t>(cur, ksi_off);
        // SAFETY: `ksi` is obtained via `container_of` from a live node of the queue
        // being walked (`cur != head` loop invariant), which is only ever
        // populated with real `ksiginfo` entries.
        unsafe { KsigInfoAccess::assume(ksi) }.list_entry_ref().detach();
        xv6_sigport_ksiginfo_free(ksi);
        cur = next;
    }
    shared.and_sig_pending_mask(!(1u64 << (signo - 1)));
}

// Caller must hold pid_rlock or pid_wlock.
#[no_mangle]
pub extern "C" fn tg_recalc_sigpending(tg: *mut thread_group) {
    if tg.is_null() { return; }
    // SAFETY: `tg` is checked non-null at the top of `tg_recalc_sigpending` above.
    let tga = unsafe { ThreadGroupAccess::assume(tg) };
    let shared = tga.shared_pending_ref().sig_pending_mask();
    for_each_tg_thread(tga.thread_list_ptr(), |t| {
        // SAFETY: `t` is yielded by `for_each_tg_thread` walking
        // `tga.thread_list_ptr()`, which by kernel invariant only links
        // threads currently live in this group.
        let tt = unsafe { ThreadAccess::assume(t) };
        if tt.sigacts_ptr().is_null() { return; }
        // SAFETY: `t` is yielded by `for_each_tg_thread` walking
        // `tga.thread_list_ptr()`, which by kernel invariant only links
        // threads currently live in this group.
        let ts = unsafe { ThreadSignalAccess::assume_thread(t) };
        let blocked = ts.sig_mask();
        let thread_pending = ts.sig_pending_mask_atomic_load();
        if ((thread_pending | shared) & !blocked) != 0 {
            thread_set_sigpending(t);
        } else {
            thread_clear_sigpending(t);
        }
    });
}

// ===========================================================================
// xv6_tgport_* aliases (kept for existing callers in proc_rust_shims.c).
// ===========================================================================
macro_rules! tgport_alias {
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* ) -> $ret:ty) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) -> $ret { $target($($pn),*) }
    };
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* )) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) { $target($($pn),*) }
    };
}

// SAFETY: currently vacuous for every existing `tgport_unsafe_alias!`
// target (thread_group_init, thread_group_put, thread_group_alloc,
// thread_group_alloc_kernel, thread_group_exit, tg_signal_send) — all
// of them are plain `pub extern "C" fn`s, not `unsafe fn`, so calling
// `$target(...)` needs no unsafe context on its own. Kept as a distinct
// macro (rather than folding these aliases into `tgport_alias!`) for
// call-table symmetry with the analogous `thport_alias!` /
// `thport_unsafe_alias!` split in `proc/thread.rs`, and so a future
// target that does need unsafe can opt in without a call-site rewrite.
macro_rules! tgport_unsafe_call { ($e:expr) => {{ u! { $e } }}; }

macro_rules! tgport_unsafe_alias {
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* ) -> $ret:ty) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) -> $ret { tgport_unsafe_call!($target($($pn),*)) }
    };
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* )) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) { tgport_unsafe_call!($target($($pn),*)) }
    };
}

tgport_unsafe_alias!(xv6_tgport_thread_group_init => thread_group_init(initproc: *mut thread));
tgport_alias!(xv6_tgport_thread_group_get => thread_group_get(tg: *mut thread_group));
tgport_unsafe_alias!(xv6_tgport_thread_group_put => thread_group_put(tg: *mut thread_group));
tgport_alias!(xv6_tgport_thread_group_live_dec => thread_group_live_dec(tg: *mut thread_group));
tgport_alias!(xv6_tgport_get_thread_group => get_thread_group(tgid: pid_t) -> *mut thread_group);
tgport_alias!(xv6_tgport_tg_shared_pending_init => tg_shared_pending_init(tg: *mut thread_group));
tgport_alias!(xv6_tgport_tg_shared_pending_destroy => tg_shared_pending_destroy(tg: *mut thread_group));
tgport_unsafe_alias!(xv6_tgport_thread_group_alloc => thread_group_alloc(leader: *mut thread) -> c_int);
tgport_unsafe_alias!(xv6_tgport_thread_group_alloc_kernel => thread_group_alloc_kernel(out_tg: *mut *mut thread_group, tgid: pid_t) -> c_int);
tgport_alias!(xv6_tgport_thread_group_add => thread_group_add(tg: *mut thread_group, child: *mut thread));
tgport_alias!(xv6_tgport_thread_group_remove => thread_group_remove(p: *mut thread) -> bool);
tgport_alias!(xv6_tgport_thread_is_group_leader => thread_is_group_leader(p: *mut thread) -> bool);
tgport_alias!(xv6_tgport_thread_tgid => thread_tgid(p: *mut thread) -> c_int);
tgport_unsafe_alias!(xv6_tgport_thread_group_exit => thread_group_exit(p: *mut thread, code: c_int));
tgport_unsafe_alias!(xv6_tgport_tg_signal_send => tg_signal_send(tg: *mut thread_group, info: *mut ksiginfo_t) -> c_int);
tgport_alias!(xv6_tgport_tg_signal_pending => tg_signal_pending(tg: *mut thread_group, p: *mut thread) -> bool);
tgport_alias!(xv6_tgport_tg_dequeue_signal => tg_dequeue_signal(tg: *mut thread_group, signo: c_int) -> *mut ksiginfo_t);
tgport_alias!(xv6_tgport_tg_sigpending_empty => tg_sigpending_empty(tg: *mut thread_group, signo: c_int));
tgport_alias!(xv6_tgport_tg_recalc_sigpending => tg_recalc_sigpending(tg: *mut thread_group));
