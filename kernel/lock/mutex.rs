//! Rust port of `kernel/lock/mutex.c`. Preserves C ABI.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr::{addr_of_mut, null_mut};
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{
    mutex_t, spinlock_t, thread, timer_node, tq_t,
    thread_state_THREAD_INTERRUPTIBLE, thread_state_THREAD_UNINTERRUPTIBLE,
    EINTR, EINVAL, ETIMEDOUT,
};
use crate::machine;
use crate::sync::KSpinlock;

unsafe extern "C" {
    pub safe fn spin_init(lk: *mut spinlock_t, name: *const c_char);
    pub safe fn spin_lock(lk: *mut spinlock_t);
    pub safe fn spin_unlock(lk: *mut spinlock_t);
    pub safe fn spin_holding(lk: *mut spinlock_t) -> c_int;

    pub safe fn tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t);
    pub safe fn tq_wait(q: *mut tq_t, lock: *mut spinlock_t,
                        rdata: *mut u64) -> c_int;
    pub safe fn tq_wait_cb(q: *mut tq_t,
                           sleep_cb: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
                           wake_cb: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
                           data: *mut c_void,
                           rdata: *mut u64) -> c_int;
    pub safe fn tq_wakeup(q: *mut tq_t, error_no: c_int, rdata: u64) -> *mut thread;

    pub safe fn signal_pending(p: *mut thread) -> u32;
    pub safe fn sched_timer_set(tn: *mut timer_node, ticks: u64) -> c_int;
    pub safe fn sched_timer_done(tn: *mut timer_node);
}

// ---------------------------------------------------------------------------
// Centralised-unsafe field accessors
// ---------------------------------------------------------------------------

#[inline(always)]
fn lk_ptr(m: *mut mutex_t) -> *mut spinlock_t {
    // SAFETY: structurally valid mutex.
    unsafe { addr_of_mut!((*m).lk) }
}
#[inline(always)]
fn wq_ptr(m: *mut mutex_t) -> *mut tq_t {
    // SAFETY: see `lk_ptr`.
    unsafe { addr_of_mut!((*m).wait_queue) }
}
#[inline(always)]
fn holder_atomic<'a>(m: *mut mutex_t) -> &'a AtomicI32 {
    // SAFETY: `holder` is a 4-byte aligned `pid_t` (`c_int`). We model
    // it as `AtomicI32` to perform `smp_store_release` / `_load_acquire`
    // / `atomic_cas` matching the C macros.
    unsafe { &*(addr_of_mut!((*m).holder) as *const AtomicI32) }
}
#[inline(always)]
fn set_name(m: *mut mutex_t, name: *mut c_char) {
    // SAFETY: caller has exclusive access at init time.
    unsafe { (*m).name = name; }
}
/// Mirror `__mutex_set_holder` (smp_store_release).
#[inline(always)]
fn set_holder(m: *mut mutex_t, pid: c_int) {
    holder_atomic(m).store(pid, Ordering::Release);
}
/// Mirror `__mutex_holder` (smp_load_acquire).
#[inline(always)]
fn get_holder(m: *mut mutex_t) -> c_int {
    holder_atomic(m).load(Ordering::Acquire)
}
/// Mirror `__mutex_try_set_holder` = atomic_cas(&holder, -1, pid).
/// Returns true on success (we took the lock).
#[inline(always)]
fn try_set_holder(m: *mut mutex_t, pid: c_int) -> bool {
    holder_atomic(m)
        .compare_exchange(-1, pid, Ordering::Acquire, Ordering::Relaxed)
        .is_ok()
}
/// Wake one waiter; caller holds `lk->lk`. Mirrors `__do_wakeup`.
fn do_wakeup(m: *mut mutex_t) {
    let next = tq_wakeup(wq_ptr(m), 0, 0);
    if next.is_null() {
        set_holder(m, -1);
        return;
    }
    // `machine::thread_pid` already filters out null and IS_ERR pointers,
    // returning -1; in that case we leave the holder unchanged to
    // match the C `assert(!IS_ERR(...))` shape (do not propagate the
    // bogus value into `holder`).
    let pid = machine::thread_pid(next);
    if pid != -1 {
        set_holder(m, pid);
    }
}

// ---------------------------------------------------------------------------
// Timed-wait context — every field access is encapsulated in a safe
// method on `MutexTimedCtx`. Each callback contains exactly **one**
// `unsafe { ... }` block (the reborrow from the C-supplied `*mut
// c_void` to `&mut MutexTimedCtx`); the rest of the callback body is
// plain safe Rust.
// ---------------------------------------------------------------------------

#[repr(C)]
struct MutexTimedCtx {
    lock: *mut mutex_t,
    timer: timer_node,
    timeout_ms: u64,
    timer_armed: bool,
}

impl MutexTimedCtx {
    /// Reborrow the C-supplied raw pointer as a `&mut Self`.
    ///
    /// SAFETY: callers pass a `*mut c_void` that originates from
    /// `&mut MutexTimedCtx as *mut _ as *mut c_void` on the same
    /// stack frame; the tq machinery guarantees the lifetime spans
    /// the sleep/wake callback pair.
    #[inline(always)]
    unsafe fn from_raw<'a>(data: *mut c_void) -> Option<&'a mut Self> {
        if data.is_null() { None } else { Some(unsafe { &mut *(data as *mut Self) }) }
    }
    #[inline(always)] fn lock_ptr(&self) -> *mut mutex_t { self.lock }
    #[inline(always)] fn timer_node_ptr(&mut self) -> *mut timer_node {
        addr_of_mut!(self.timer)
    }
    #[inline(always)] fn timeout(&self) -> u64 { self.timeout_ms }
    #[inline(always)] fn armed(&self) -> bool { self.timer_armed }
    #[inline(always)] fn set_armed(&mut self, v: bool) { self.timer_armed = v; }
}

unsafe extern "C" fn mutex_timed_sleep_cb(data: *mut c_void) -> c_int {
    // SAFETY: see `MutexTimedCtx::from_raw`.
    let ctx = match unsafe { MutexTimedCtx::from_raw(data) } {
        Some(c) => c,
        None => return 0,
    };
    let m = ctx.lock_ptr();
    if m.is_null() { return 0; }

    ctx.set_armed(false);
    let timeout_ms = ctx.timeout();
    if timeout_ms > 0 {
        let tn = ctx.timer_node_ptr();
        if sched_timer_set(tn, timeout_ms) == 0 {
            ctx.set_armed(true);
        }
    }
    let status = spin_holding(lk_ptr(m));
    if status != 0 { spin_unlock(lk_ptr(m)); }
    status
}

unsafe extern "C" fn mutex_timed_wake_cb(data: *mut c_void, sleep_cb_status: c_int) {
    // SAFETY: see `MutexTimedCtx::from_raw`.
    let ctx = match unsafe { MutexTimedCtx::from_raw(data) } {
        Some(c) => c,
        None => return,
    };
    let m = ctx.lock_ptr();
    if m.is_null() { return; }
    if ctx.armed() {
        let tn = ctx.timer_node_ptr();
        sched_timer_done(tn);
        ctx.set_armed(false);
    }
    if sleep_cb_status != 0 { spin_lock(lk_ptr(m)); }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn mutex_init(m: *mut mutex_t, name: *mut c_char) {
    spin_init(lk_ptr(m), b"sleep lock\0".as_ptr() as *const c_char);
    tq_init(wq_ptr(m),
            b"sleep lock wait queue\0".as_ptr() as *const c_char,
            lk_ptr(m));
    set_name(m, name);
    set_holder(m, -1);
}

#[no_mangle]
pub unsafe extern "C" fn mutex_lock(m: *mut mutex_t) {
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);

    if try_set_holder(m, pid) { return; }

    let _g = KSpinlock::from_bindings(lk_ptr(m)).lock();
    if try_set_holder(m, pid) { return; }

    while get_holder(m) != pid {
        machine::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
        let _ = tq_wait(wq_ptr(m), lk_ptr(m), null_mut());
    }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_unlock(m: *mut mutex_t) {
    let _g = KSpinlock::from_bindings(lk_ptr(m)).lock();
    do_wakeup(m);
}

#[no_mangle]
pub unsafe extern "C" fn holding_mutex(m: *mut mutex_t) -> c_int {
    let cur = machine::current_thread_ptr();
    if cur.is_null() { return 0; }
    // SeqCst load to match `__atomic_load_n(..., SEQ_CST)` in C.
    let h = holder_atomic(m).load(Ordering::SeqCst);
    if h == machine::thread_pid(cur) { 1 } else { 0 }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_trylock(m: *mut mutex_t) -> c_int {
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);
    if try_set_holder(m, pid) { 1 } else { 0 }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_lock_interruptible(m: *mut mutex_t) -> c_int {
    if m.is_null() { return -(EINVAL as c_int); }
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);

    if try_set_holder(m, pid) { return 0; }

    let _g = KSpinlock::from_bindings(lk_ptr(m)).lock();
    if try_set_holder(m, pid) { return 0; }

    while get_holder(m) != pid {
        if signal_pending(cur) != 0 { return -(EINTR as c_int); }
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait(wq_ptr(m), lk_ptr(m), null_mut());
        if ret != 0 && signal_pending(cur) != 0 && get_holder(m) != pid {
            return -(EINTR as c_int);
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn mutex_lock_timed(m: *mut mutex_t, timeout_ms: u64) -> c_int {
    if m.is_null() { return -(EINVAL as c_int); }
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);

    if try_set_holder(m, pid) { return 0; }
    if timeout_ms == 0 { return -(ETIMEDOUT as c_int); }

    let _g = KSpinlock::from_bindings(lk_ptr(m)).lock();
    if try_set_holder(m, pid) { return 0; }

    let timeout_ticks = machine::ms_to_rawticks(timeout_ms);
    let start = machine::read_time();
    let tm = machine::tick_ms();

    while get_holder(m) != pid {
        let now = machine::read_time();
        let elapsed = now.wrapping_sub(start);
        if elapsed >= timeout_ticks { return -(ETIMEDOUT as c_int); }
        let remaining_ticks = timeout_ticks - elapsed;
        let mut remaining_ms = if tm == 0 { 1 } else { (remaining_ticks + tm - 1) / tm };
        if remaining_ms == 0 { remaining_ms = 1; }

        let mut ctx = MutexTimedCtx {
            lock: m,
            // SAFETY: zero-init of `timer_node` mirrors C `.timer = {0}`.
            timer: unsafe { core::mem::zeroed() },
            timeout_ms: remaining_ms,
            timer_armed: false,
        };
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait_cb(
            wq_ptr(m),
            Some(mutex_timed_sleep_cb),
            Some(mutex_timed_wake_cb),
            &mut ctx as *mut _ as *mut c_void,
            null_mut(),
        );
        if ret != 0 && signal_pending(cur) != 0 && get_holder(m) != pid {
            return -(EINTR as c_int);
        }
    }
    0
}

// ===========================================================================
// Rust-native typed handle
// ===========================================================================

use core::marker::PhantomData;

/// Errors returned by the blocking lock variants.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum MutexError {
    Invalid,
    Interrupted,
    TimedOut,
    Other(c_int),
}

fn map_mutex_err(r: c_int) -> MutexError {
    match -r as u32 {
        EINVAL => MutexError::Invalid,
        EINTR => MutexError::Interrupted,
        ETIMEDOUT => MutexError::TimedOut,
        _ => MutexError::Other(r),
    }
}

/// Typed handle to a kernel `mutex_t`.
#[derive(Clone, Copy)]
pub struct KMutex {
    raw: *mut mutex_t,
    _ns: PhantomData<*const ()>,
}

impl KMutex {
    #[inline(always)]
    pub fn from_ptr(raw: *mut mutex_t) -> Self {
        Self { raw, _ns: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut mutex_t { self.raw }

    #[inline]
    pub fn init(self, name: *mut c_char) {
        // SAFETY: handle wraps live storage.
        unsafe { mutex_init(self.raw, name); }
    }

    /// `true` iff the current thread holds this mutex.
    #[inline]
    pub fn holding(self) -> bool {
        // SAFETY: see `init`.
        unsafe { holding_mutex(self.raw) != 0 }
    }

    /// Acquire (uninterruptible). Returns RAII guard.
    #[inline]
    pub fn lock(self) -> KMutexGuard {
        // SAFETY: see `init`.
        unsafe { mutex_lock(self.raw); }
        KMutexGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Non-blocking acquire.
    #[inline]
    pub fn try_lock(self) -> Option<KMutexGuard> {
        // SAFETY: see `init`. Returns 1 on success.
        let got = unsafe { mutex_trylock(self.raw) };
        if got != 0 {
            Some(KMutexGuard { raw: self.raw, _ns: PhantomData })
        } else {
            None
        }
    }

    /// Acquire (interruptible).
    #[inline]
    pub fn lock_interruptible(self) -> Result<KMutexGuard, MutexError> {
        // SAFETY: see `init`.
        let r = unsafe { mutex_lock_interruptible(self.raw) };
        if r == 0 {
            Ok(KMutexGuard { raw: self.raw, _ns: PhantomData })
        } else {
            Err(map_mutex_err(r))
        }
    }

    /// Acquire with timeout.
    #[inline]
    pub fn lock_timed(self, timeout_ms: u64) -> Result<KMutexGuard, MutexError> {
        // SAFETY: see `init`.
        let r = unsafe { mutex_lock_timed(self.raw, timeout_ms) };
        if r == 0 {
            Ok(KMutexGuard { raw: self.raw, _ns: PhantomData })
        } else {
            Err(map_mutex_err(r))
        }
    }

    /// Run `f` while holding the mutex (uninterruptible acquire).
    #[inline]
    pub fn with<R>(self, f: impl FnOnce() -> R) -> R {
        let _g = self.lock();
        f()
    }
}

/// RAII guard for an acquired [`KMutex`].
#[must_use = "mutex is released when the guard is dropped"]
pub struct KMutexGuard {
    raw: *mut mutex_t,
    _ns: PhantomData<*const ()>,
}

impl Drop for KMutexGuard {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: guard was constructed only after a successful acquire.
        unsafe { mutex_unlock(self.raw); }
    }
}
