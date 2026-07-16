//! Rust port of `kernel/lock/rwsem.c`. Preserves C ABI.

#![allow(non_camel_case_types, non_snake_case)]


use core::ffi::{c_char, c_int, c_void};
use core::ptr::{addr_of_mut, null_mut};

use crate::u;
use crate::bindings::{
    rwsem_t, spinlock_t, thread, timer_node, tq_t,
    thread_state_THREAD_INTERRUPTIBLE, thread_state_THREAD_UNINTERRUPTIBLE,
    EAGAIN, EDEADLK, EINTR, EINVAL, ETIMEDOUT,
    RWLOCK_PRIO_WRITE,
};
use crate::machine;
use crate::sync::KSpinlock;

// P3-D2a: the thread-queue primitives (kernel/proc/thread_queue.rs) are
// ordinary Rust fns, reached as plain crate-path items instead of
// `extern "C"` redeclarations.
use crate::proc::{tq_init, tq_size, tq_wait, tq_wait_cb, tq_wakeup, tq_wakeup_all};

// P3-D2b: `signal_pending` (proc/signal.rs) is a plain crate-path item
// now that its `#[no_mangle]` export is gone. The old redeclaration here
// said `-> u32`; the real fn returns `bool` (same 0/1 in `a0` under the
// old C ABI), so the call sites drop their `!= 0`.
use crate::proc::signal_pending;

unsafe extern "C" {
    pub safe fn sched_timer_set(tn: *mut timer_node, ticks: u64) -> c_int;
    pub safe fn sched_timer_done(tn: *mut timer_node);
}
pub(crate) use crate::lock::spinlock::{spin_holding, spin_lock, spin_unlock};

/// See `completion.rs`'s identical note: `crate::lock::spinlock::spin_init`
/// takes `name: *mut c_char`; this file's original extern declaration
/// typed it `*const c_char` (call site only ever passes a `'static`
/// string-literal pointer, never written through).
#[inline]
fn spin_init(lk: *mut spinlock_t, name: *const c_char) {
    // SAFETY: `name` is only read by the callee despite the `*mut`
    // parameter; the sole call site passes a `'static` string literal.
    unsafe { crate::lock::spinlock::spin_init(lk, name as *mut c_char) };
}

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3A.
//
// `crate::bindings::rwsem_t` (`kernel/inc/lock/rwlock_types.h`'s `struct
// rwsem`) has ~90 non-`lock/` referents (`KRwSem::raw` is embedded in
// bindgen structs across mm/proc/vfs), so the C-ABI entry points below
// and `KRwSem`/its guards' public field type keep `*mut rwsem_t`
// unchanged. `RawRwsem` is this file's own native, layout-identical
// working type: every private helper operates on it directly. `lock`
// reuses `spinlock::RawSpinlock`; `read_queue`/`write_queue` stay the
// bindgen `tq_t` (proc-owned, out of this wave's scope).
#[repr(C, align(64))]
pub(crate) struct RawRwsem {
    pub(crate) lock: crate::lock::spinlock::RawSpinlock,
    pub(crate) readers: c_int,
    pub(crate) holder_pid: c_int,
    pub(crate) read_queue: tq_t,
    pub(crate) write_queue: tq_t,
    pub(crate) name: *const c_char,
    pub(crate) flags: u64,
}

const _: () = {
    assert!(core::mem::size_of::<RawRwsem>() == core::mem::size_of::<rwsem_t>());
    assert!(core::mem::align_of::<RawRwsem>() == core::mem::align_of::<rwsem_t>());
    assert!(core::mem::offset_of!(RawRwsem, lock) == core::mem::offset_of!(rwsem_t, lock));
    assert!(
        core::mem::offset_of!(RawRwsem, readers) == core::mem::offset_of!(rwsem_t, readers)
    );
    assert!(
        core::mem::offset_of!(RawRwsem, holder_pid) == core::mem::offset_of!(rwsem_t, holder_pid)
    );
    assert!(
        core::mem::offset_of!(RawRwsem, read_queue) == core::mem::offset_of!(rwsem_t, read_queue)
    );
    assert!(
        core::mem::offset_of!(RawRwsem, write_queue)
            == core::mem::offset_of!(rwsem_t, write_queue)
    );
    assert!(core::mem::offset_of!(RawRwsem, name) == core::mem::offset_of!(rwsem_t, name));
    assert!(core::mem::offset_of!(RawRwsem, flags) == core::mem::offset_of!(rwsem_t, flags));
};

/// Reinterpret the bindgen `*mut rwsem_t` as the native mirror.
///
/// SAFETY: layout equivalence is proven at compile time by the
/// assertions above; the caller provides a valid, non-dangling
/// `*mut rwsem_t`.
#[inline(always)]
fn as_native(l: *mut rwsem_t) -> *mut RawRwsem {
    l as *mut RawRwsem
}

// ---------------------------------------------------------------------------
// Helpers — centralised unsafe field accessors
// ---------------------------------------------------------------------------

#[inline(always)]
fn lk_ptr(l: *mut RawRwsem) -> *mut spinlock_t {
    // SAFETY: structurally valid rwsem.
    u! { addr_of_mut!((*l).lock) as *mut spinlock_t }
}
#[inline(always)]
fn rq_ptr(l: *mut RawRwsem) -> *mut tq_t {
    // SAFETY: see `lk_ptr`.
    u! { addr_of_mut!((*l).read_queue) }
}
#[inline(always)]
fn wq_ptr(l: *mut RawRwsem) -> *mut tq_t {
    // SAFETY: see `lk_ptr`.
    u! { addr_of_mut!((*l).write_queue) }
}
#[inline(always)]
fn get_readers(l: *mut RawRwsem) -> c_int {
    // SAFETY: caller holds `l->lock`.
    u! { (*l).readers }
}
#[inline(always)]
fn set_readers(l: *mut RawRwsem, v: c_int) {
    // SAFETY: caller holds `l->lock`.
    u! { (*l).readers = v; }
}
#[inline(always)]
fn get_holder(l: *mut RawRwsem) -> c_int {
    // SAFETY: caller holds `l->lock`.
    u! { (*l).holder_pid }
}
#[inline(always)]
fn set_holder(l: *mut RawRwsem, v: c_int) {
    // SAFETY: caller holds `l->lock`.
    u! { (*l).holder_pid = v; }
}
#[inline(always)]
fn get_flags(l: *mut RawRwsem) -> u64 {
    // SAFETY: read-only of a constant-after-init field.
    u! { (*l).flags }
}
#[inline(always)]
fn set_name_flags(l: *mut RawRwsem, name: *const c_char, flags: u64) {
    // SAFETY: caller has exclusive access at init time.
    u! { (*l).name = name; (*l).flags = flags; }
}

// ---------------------------------------------------------------------------
// Wait-decision helpers (caller holds `l->lock`)
// ---------------------------------------------------------------------------

fn reader_should_wait(l: *mut RawRwsem) -> bool {
    if get_readers(l) == 0 {
        return get_holder(l) != -1;
    }
    if (get_flags(l) & RWLOCK_PRIO_WRITE as u64) != 0 && tq_size(wq_ptr(l)) > 0 {
        return true;
    }
    false
}

fn writer_should_wait(l: *mut RawRwsem, pid: c_int) -> bool {
    let h = get_holder(l);
    if h == pid { return false; }
    if h != -1 { return true; }
    if get_readers(l) > 0 { return true; }
    false
}

// Caller holds `l->lock`.
fn wake_readers(l: *mut RawRwsem) {
    let _ = tq_wakeup_all(rq_ptr(l), 0, 0);
}

// Caller holds `l->lock`.
fn wake_writer(l: *mut RawRwsem) {
    let next = tq_wakeup(wq_ptr(l), 0, 0);
    let pid = machine::thread_pid(next);
    if pid != -1 {
        set_holder(l, pid);
    }
}

fn do_wake_up(l: *mut RawRwsem) {
    if (get_flags(l) & RWLOCK_PRIO_WRITE as u64) != 0 {
        if tq_size(wq_ptr(l)) > 0 {
            wake_writer(l);
        } else if tq_size(rq_ptr(l)) > 0 {
            wake_readers(l);
        }
    } else {
        if tq_size(rq_ptr(l)) > 0 {
            wake_readers(l);
        } else if tq_size(wq_ptr(l)) > 0 {
            wake_writer(l);
        }
    }
}

// ---------------------------------------------------------------------------
// Timed-wait context — see `mutex.rs` for the design rationale: one
// unsafe block per callback (the reborrow), all field access via safe
// methods on the ctx struct.
// ---------------------------------------------------------------------------

#[repr(C)]
struct RwsemTimedCtx {
    lock: *mut RawRwsem,
    timer: timer_node,
    timeout_ms: u64,
    timer_armed: bool,
}

impl RwsemTimedCtx {
    #[inline(always)]
    unsafe fn from_raw<'a>(data: *mut c_void) -> Option<&'a mut Self> {
        if data.is_null() { None } else { Some(u! { &mut *(data as *mut Self) }) }
    }
    #[inline(always)] fn lock_ptr(&self) -> *mut RawRwsem { self.lock }
    #[inline(always)] fn timer_node_ptr(&mut self) -> *mut timer_node { addr_of_mut!(self.timer) }
    #[inline(always)] fn timeout(&self) -> u64 { self.timeout_ms }
    #[inline(always)] fn armed(&self) -> bool { self.timer_armed }
    #[inline(always)] fn set_armed(&mut self, v: bool) { self.timer_armed = v; }
}

extern "C" fn rwsem_timed_sleep_cb(data: *mut c_void)-> c_int  { u! {
    // SAFETY: see `RwsemTimedCtx::from_raw`.
    let ctx = match u! { RwsemTimedCtx::from_raw(data) } {
        Some(c) => c, None => return 0,
    };
    let l = ctx.lock_ptr();
    if l.is_null() { return 0; }

    ctx.set_armed(false);
    let timeout_ms = ctx.timeout();
    if timeout_ms > 0 {
        let tn = ctx.timer_node_ptr();
        if sched_timer_set(tn, timeout_ms) == 0 {
            ctx.set_armed(true);
        }
    }
    let status = spin_holding(lk_ptr(l));
    if status != 0 { spin_unlock(lk_ptr(l)); }
    status
}}

extern "C" fn rwsem_timed_wake_cb(data: *mut c_void, sleep_cb_status: c_int) { u! {
    // SAFETY: see `RwsemTimedCtx::from_raw`.
    let ctx = match u! { RwsemTimedCtx::from_raw(data) } {
        Some(c) => c, None => return,
    };
    let l = ctx.lock_ptr();
    if l.is_null() { return; }
    if ctx.armed() {
        let tn = ctx.timer_node_ptr();
        sched_timer_done(tn);
        ctx.set_armed(false);
    }
    if sleep_cb_status != 0 { spin_lock(lk_ptr(l)); }
}}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

pub(crate) fn rwsem_init(l: *mut rwsem_t, flags: u64, name: *const c_char)-> c_int  { u! {
    if l.is_null() || name.is_null() { return -1; }
    let l = as_native(l);
    spin_init(lk_ptr(l), b"rwsem spinlock\0".as_ptr() as *const c_char);
    set_readers(l, 0);
    tq_init(rq_ptr(l), b"rwsem read queue\0".as_ptr() as *const c_char, lk_ptr(l));
    tq_init(wq_ptr(l), b"rwsem write queue\0".as_ptr() as *const c_char, lk_ptr(l));
    set_name_flags(l, name, flags);
    set_holder(l, -1);
    0
}}

pub(crate) fn rwsem_acquire_read(l: *mut rwsem_t)-> c_int  { u! {
    if l.is_null() { return -1; }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    while reader_should_wait(l) {
        machine::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
        let ret = tq_wait(rq_ptr(l), lk_ptr(l), null_mut());
        if ret != 0 { return ret; }
    }
    set_readers(l, get_readers(l) + 1);
    0
}}

pub(crate) fn rwsem_try_acquire_read(l: *mut rwsem_t)-> c_int  { u! {
    if l.is_null() { return -(EINVAL as c_int); }
    let l = as_native(l);
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    if !reader_should_wait(l) {
        set_readers(l, get_readers(l) + 1);
        0
    } else {
        -(EAGAIN as c_int)
    }
}}

pub(crate) fn rwsem_acquire_read_interruptible(l: *mut rwsem_t)-> c_int  { u! {
    if l.is_null() { return -(EINVAL as c_int); }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    while reader_should_wait(l) {
        if signal_pending(cur) { return -(EINTR as c_int); }
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait(rq_ptr(l), lk_ptr(l), null_mut());
        if ret != 0 && signal_pending(cur) { return -(EINTR as c_int); }
    }
    set_readers(l, get_readers(l) + 1);
    0
}}

pub(crate) fn rwsem_acquire_read_timed(l: *mut rwsem_t, timeout_ms: u64)-> c_int  { u! {
    if l.is_null() { return -(EINVAL as c_int); }
    if timeout_ms == 0 {
        return if rwsem_try_acquire_read(l) == 0 { 0 } else { -(ETIMEDOUT as c_int) };
    }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    let start = machine::read_time();
    let timeout_ticks = machine::ms_to_rawticks(timeout_ms);
    let tm = machine::tick_ms();
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    while reader_should_wait(l) {
        if signal_pending(cur) { return -(EINTR as c_int); }
        let elapsed = machine::read_time().wrapping_sub(start);
        if elapsed >= timeout_ticks { return -(ETIMEDOUT as c_int); }
        let remaining_ticks = timeout_ticks - elapsed;
        let mut remaining_ms = if tm == 0 { 1 } else { (remaining_ticks + tm - 1) / tm };
        if remaining_ms == 0 { remaining_ms = 1; }
        let mut ctx = RwsemTimedCtx {
            lock: l,
            // SAFETY: zero-init timer_node mirrors C `.timer = {0}`.
            timer: u! { core::mem::zeroed() },
            timeout_ms: remaining_ms,
            timer_armed: false,
        };
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait_cb(
            rq_ptr(l),
            Some(rwsem_timed_sleep_cb),
            Some(rwsem_timed_wake_cb),
            &mut ctx as *mut _ as *mut c_void,
            null_mut(),
        );
        if ret != 0 && signal_pending(cur) { return -(EINTR as c_int); }
    }
    set_readers(l, get_readers(l) + 1);
    0
}}

pub(crate) fn rwsem_acquire_write(l: *mut rwsem_t)-> c_int  { u! {
    if l.is_null() { return -1; }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    while writer_should_wait(l, pid) {
        machine::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
        let ret = tq_wait(wq_ptr(l), lk_ptr(l), null_mut());
        if ret != 0 { return ret; }
    }
    set_holder(l, pid);
    0
}}

pub(crate) fn rwsem_try_acquire_write(l: *mut rwsem_t)-> c_int  { u! {
    if l.is_null() { return -(EINVAL as c_int); }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    if get_holder(l) == pid { return -(EDEADLK as c_int); }
    if !writer_should_wait(l, pid) {
        set_holder(l, pid);
        0
    } else {
        -(EAGAIN as c_int)
    }
}}

pub(crate) fn rwsem_acquire_write_interruptible(l: *mut rwsem_t)-> c_int  { u! {
    if l.is_null() { return -(EINVAL as c_int); }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    if get_holder(l) == pid { return -(EDEADLK as c_int); }
    while writer_should_wait(l, pid) {
        if signal_pending(cur) { return -(EINTR as c_int); }
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait(wq_ptr(l), lk_ptr(l), null_mut());
        if ret != 0 && signal_pending(cur) { return -(EINTR as c_int); }
    }
    set_holder(l, pid);
    0
}}

pub(crate) fn rwsem_acquire_write_timed(l: *mut rwsem_t, timeout_ms: u64)-> c_int  { u! {
    if l.is_null() { return -(EINVAL as c_int); }
    if timeout_ms == 0 {
        return if rwsem_try_acquire_write(l) == 0 { 0 } else { -(ETIMEDOUT as c_int) };
    }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);
    let start = machine::read_time();
    let timeout_ticks = machine::ms_to_rawticks(timeout_ms);
    let tm = machine::tick_ms();
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    if get_holder(l) == pid { return -(EDEADLK as c_int); }
    while writer_should_wait(l, pid) {
        if signal_pending(cur) { return -(EINTR as c_int); }
        let elapsed = machine::read_time().wrapping_sub(start);
        if elapsed >= timeout_ticks { return -(ETIMEDOUT as c_int); }
        let remaining_ticks = timeout_ticks - elapsed;
        let mut remaining_ms = if tm == 0 { 1 } else { (remaining_ticks + tm - 1) / tm };
        if remaining_ms == 0 { remaining_ms = 1; }
        let mut ctx = RwsemTimedCtx {
            lock: l,
            // SAFETY: zero-init timer_node.
            timer: u! { core::mem::zeroed() },
            timeout_ms: remaining_ms,
            timer_armed: false,
        };
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait_cb(
            wq_ptr(l),
            Some(rwsem_timed_sleep_cb),
            Some(rwsem_timed_wake_cb),
            &mut ctx as *mut _ as *mut c_void,
            null_mut(),
        );
        if ret != 0 && signal_pending(cur) { return -(EINTR as c_int); }
    }
    set_holder(l, pid);
    0
}}

pub(crate) fn rwsem_release(l: *mut rwsem_t) { u! {
    if l.is_null() { return; }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    let self_pid = machine::thread_pid(cur);
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    if get_holder(l) == self_pid && self_pid != -1 {
        set_holder(l, -1);
        do_wake_up(l);
    } else {
        let r = get_readers(l);
        if r > 0 {
            set_readers(l, r - 1);
            if r - 1 == 0 {
                do_wake_up(l);
            }
        }
    }
}}

pub(crate) fn rwsem_is_write_holding(l: *mut rwsem_t)-> bool  { u! {
    if l.is_null() { return false; }
    let l = as_native(l);
    let cur = machine::current_thread_ptr();
    if cur.is_null() { return false; }
    let pid = machine::thread_pid(cur);
    let _g = KSpinlock::from_bindings(lk_ptr(l)).lock();
    get_holder(l) == pid
}}

// ===========================================================================
// Rust-native typed handle
// ===========================================================================

use core::marker::PhantomData;

/// Errors from `rwsem_*` operations.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum RwSemError {
    Invalid,
    WouldBlock,
    Interrupted,
    TimedOut,
    Other(c_int),
}

fn map_rwsem_err(r: c_int) -> RwSemError {
    match -r as u32 {
        EINVAL => RwSemError::Invalid,
        EAGAIN => RwSemError::WouldBlock,
        EINTR => RwSemError::Interrupted,
        ETIMEDOUT => RwSemError::TimedOut,
        _ => RwSemError::Other(r),
    }
}

/// Typed handle to a kernel `rwsem_t`.
#[derive(Clone, Copy)]
pub struct KRwSem {
    raw: *mut rwsem_t,
    _ns: PhantomData<*const ()>,
}

impl KRwSem {
    #[inline(always)]
    pub fn from_ptr(raw: *mut rwsem_t) -> Self {
        Self { raw, _ns: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut rwsem_t { self.raw }

    #[inline]
    pub fn init(self, flags: u64, name: *const c_char) -> Result<(), RwSemError> {
        // SAFETY: handle wraps live storage.
        let r = u! { rwsem_init(self.raw, flags, name) };
        if r == 0 { Ok(()) } else { Err(map_rwsem_err(r)) }
    }

    /// Acquire shared (read). Returns RAII guard.
    #[inline]
    pub fn read(self) -> Result<KRwSemReadGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { rwsem_acquire_read(self.raw) };
        if r == 0 {
            Ok(KRwSemReadGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(map_rwsem_err(r)) }
    }

    #[inline]
    pub fn try_read(self) -> Result<KRwSemReadGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { rwsem_try_acquire_read(self.raw) };
        if r == 0 {
            Ok(KRwSemReadGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(map_rwsem_err(r)) }
    }

    #[inline]
    pub fn read_interruptible(self) -> Result<KRwSemReadGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { rwsem_acquire_read_interruptible(self.raw) };
        if r == 0 {
            Ok(KRwSemReadGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(map_rwsem_err(r)) }
    }

    #[inline]
    pub fn read_timed(self, timeout_ms: u64) -> Result<KRwSemReadGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { rwsem_acquire_read_timed(self.raw, timeout_ms) };
        if r == 0 {
            Ok(KRwSemReadGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(map_rwsem_err(r)) }
    }

    /// Acquire exclusive (write).
    #[inline]
    pub fn write(self) -> Result<KRwSemWriteGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { rwsem_acquire_write(self.raw) };
        if r == 0 {
            Ok(KRwSemWriteGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(map_rwsem_err(r)) }
    }

    #[inline]
    pub fn try_write(self) -> Result<KRwSemWriteGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { rwsem_try_acquire_write(self.raw) };
        if r == 0 {
            Ok(KRwSemWriteGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(map_rwsem_err(r)) }
    }

    #[inline]
    pub fn write_interruptible(self) -> Result<KRwSemWriteGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { rwsem_acquire_write_interruptible(self.raw) };
        if r == 0 {
            Ok(KRwSemWriteGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(map_rwsem_err(r)) }
    }

    #[inline]
    pub fn write_timed(self, timeout_ms: u64) -> Result<KRwSemWriteGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { rwsem_acquire_write_timed(self.raw, timeout_ms) };
        if r == 0 {
            Ok(KRwSemWriteGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(map_rwsem_err(r)) }
    }

    #[inline]
    pub fn is_write_holding(self) -> bool {
        // SAFETY: see `init`.
        u! { rwsem_is_write_holding(self.raw) }
    }
}

/// Read-side guard for [`KRwSem`].
#[must_use = "rwsem read lock is released when the guard is dropped"]
pub struct KRwSemReadGuard {
    raw: *mut rwsem_t,
    _ns: PhantomData<*const ()>,
}
impl Drop for KRwSemReadGuard {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: guard tracks a successful acquire.
        u! { rwsem_release(self.raw); }
    }
}

/// Write-side guard for [`KRwSem`].
#[must_use = "rwsem write lock is released when the guard is dropped"]
pub struct KRwSemWriteGuard {
    raw: *mut rwsem_t,
    _ns: PhantomData<*const ()>,
}
impl Drop for KRwSemWriteGuard {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: guard tracks a successful acquire.
        u! { rwsem_release(self.raw); }
    }
}