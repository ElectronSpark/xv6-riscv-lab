//! Rust port of `kernel/lock/semaphore.c`. Preserves C ABI.

#![allow(non_camel_case_types, non_snake_case)]


use core::ffi::{c_char, c_int, c_void};
use core::ptr::{addr_of_mut, null_mut};
use core::sync::atomic::{AtomicI32, Ordering};

use crate::u;
use crate::bindings::{
    semaphore as sem_t, spinlock_t, thread, timer_node, tq_t,
    thread_state_THREAD_INTERRUPTIBLE, thread_state_THREAD_UNINTERRUPTIBLE,
    EAGAIN, EINTR, EINVAL, ENOENT, EOVERFLOW, ETIMEDOUT,
};
use crate::machine;
use crate::sync::KSpinlock;

// Matches kernel/inc/lock/semaphore.h.
const SEM_VALUE_MAX: c_int = 2_147_483_640;

// ---------------------------------------------------------------------------
// Externs
// ---------------------------------------------------------------------------

// P3-D2a: the thread-queue primitives (kernel/proc/thread_queue.rs) are
// ordinary Rust fns, reached as plain crate-path items instead of
// `extern "C"` redeclarations.
use crate::proc::{tq_init, tq_wait, tq_wait_cb, tq_wakeup};

unsafe extern "C" {

    pub safe fn signal_pending(p: *mut thread) -> u32;
    pub safe fn sched_timer_set(tn: *mut timer_node, ticks: u64) -> c_int;
    pub safe fn sched_timer_done(tn: *mut timer_node);
    pub safe fn sleep_ms(ms: u64);
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
// `crate::bindings::semaphore` (`sem_t` alias; `kernel/inc/lock/
// semaphore_types.h`) has ~30 non-`lock/` referents (`KSemaphore::raw`
// and `sync::sync.rs`'s own `Semaphore` wrapper both embed bindgen
// pointers), so the C-ABI-adjacent entry points below (`sem_init`,
// `sem_wait`, ... — all re-exported and called directly by
// `sync::sync.rs`) keep `*mut sem_t` unchanged. `RawSemaphore` is this
// file's own native, layout-identical working type; `lk` reuses
// `spinlock::RawSpinlock`, `wait_queue` stays the bindgen `tq_t`
// (proc-owned, out of this wave's scope).
#[repr(C, align(64))]
pub(crate) struct RawSemaphore {
    pub(crate) lk: crate::lock::spinlock::RawSpinlock,
    pub(crate) wait_queue: tq_t,
    pub(crate) value: c_int,
    pub(crate) name: *const c_char,
}

const _: () = {
    assert!(core::mem::size_of::<RawSemaphore>() == core::mem::size_of::<sem_t>());
    assert!(core::mem::align_of::<RawSemaphore>() == core::mem::align_of::<sem_t>());
    assert!(core::mem::offset_of!(RawSemaphore, lk) == core::mem::offset_of!(sem_t, lk));
    assert!(
        core::mem::offset_of!(RawSemaphore, wait_queue)
            == core::mem::offset_of!(sem_t, wait_queue)
    );
    assert!(core::mem::offset_of!(RawSemaphore, value) == core::mem::offset_of!(sem_t, value));
    assert!(core::mem::offset_of!(RawSemaphore, name) == core::mem::offset_of!(sem_t, name));
};

/// Reinterpret the bindgen `*mut sem_t` as the native mirror.
///
/// SAFETY: layout equivalence is proven at compile time by the
/// assertions above; the caller provides a valid, non-dangling
/// `*mut sem_t`.
#[inline(always)]
fn as_native(s: *mut sem_t) -> *mut RawSemaphore {
    s as *mut RawSemaphore
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#[inline(always)]
fn lk_ptr(s: *mut RawSemaphore) -> *mut spinlock_t {
    // SAFETY: caller passes a structurally-valid semaphore.
    u! { addr_of_mut!((*s).lk) as *mut spinlock_t }
}
#[inline(always)]
fn wq_ptr(s: *mut RawSemaphore) -> *mut tq_t {
    // SAFETY: see `lk_ptr`.
    u! { addr_of_mut!((*s).wait_queue) }
}
#[inline(always)]
fn value_atomic<'a>(s: *mut RawSemaphore) -> &'a AtomicI32 {
    // SAFETY: `value` is a `c_int` (4-byte aligned), and we model
    // it as an `AtomicI32` for SeqCst load/store/RMW. Caller asserts
    // pointer validity.
    u! { &*(addr_of_mut!((*s).value) as *const AtomicI32) }
}
#[inline(always)]
fn value_inc(s: *mut RawSemaphore) -> c_int {
    value_atomic(s).fetch_add(1, Ordering::SeqCst) + 1
}
#[inline(always)]
fn value_dec(s: *mut RawSemaphore) -> c_int {
    value_atomic(s).fetch_sub(1, Ordering::SeqCst) - 1
}
#[inline(always)]
fn value_get(s: *mut RawSemaphore) -> c_int {
    value_atomic(s).load(Ordering::SeqCst)
}
#[inline(always)]
fn name_of(s: *mut RawSemaphore) -> *const c_char {
    // SAFETY: structurally valid sem; `name` is a pointer field.
    u! { (*s).name }
}
#[inline(always)]
fn set_name_value(s: *mut RawSemaphore, n: *const c_char, value: c_int) {
    // SAFETY: caller has exclusive access at init time.
    u! { (*s).name = n; (*s).value = value; }
}

/// Centralised variadic-printf wrapper for the one error message in
/// `sem_wait` — the body is the only `u! { ... }` block needed
/// in the public API path.
#[inline(always)]
fn printf_post_failed(name: *const c_char) {
    // SAFETY: format string is a static NUL-terminated literal;
    // `name` is a pointer field of a semaphore (always valid C string).
    u! {
        crate::kprintln!(
            "Failed to post semaphore '{}' when thread was interrupted",
            crate::printf::Cs(name));
    }
}

// ---------------------------------------------------------------------------
// Timed-wait context
// ---------------------------------------------------------------------------

#[repr(C)]
struct SemTimedCtx {
    sem: *mut RawSemaphore,
    timer: timer_node,
    timeout_ms: u64,
    timer_armed: bool,
}

impl SemTimedCtx {
    #[inline(always)]
    unsafe fn from_raw<'a>(data: *mut c_void) -> Option<&'a mut Self> {
        if data.is_null() { None } else { Some(u! { &mut *(data as *mut Self) }) }
    }
    #[inline(always)] fn sem_ptr(&self) -> *mut RawSemaphore { self.sem }
    #[inline(always)] fn timer_node_ptr(&mut self) -> *mut timer_node { addr_of_mut!(self.timer) }
    #[inline(always)] fn timeout(&self) -> u64 { self.timeout_ms }
    #[inline(always)] fn armed(&self) -> bool { self.timer_armed }
    #[inline(always)] fn set_armed(&mut self, v: bool) { self.timer_armed = v; }
}

extern "C" fn sem_timed_sleep_cb(data: *mut c_void)-> c_int  { u! {
    // SAFETY: see `SemTimedCtx::from_raw`.
    let ctx = match u! { SemTimedCtx::from_raw(data) } {
        Some(c) => c, None => return 0,
    };
    let sem = ctx.sem_ptr();
    if sem.is_null() { return 0; }

    ctx.set_armed(false);
    let timeout_ms = ctx.timeout();
    if timeout_ms > 0 {
        let tn = ctx.timer_node_ptr();
        if sched_timer_set(tn, timeout_ms) == 0 {
            ctx.set_armed(true);
        }
    }
    let status = spin_holding(lk_ptr(sem));
    if status != 0 {
        spin_unlock(lk_ptr(sem));
    }
    status
}}

extern "C" fn sem_timed_wake_cb(data: *mut c_void, sleep_cb_status: c_int) { u! {
    // SAFETY: see `SemTimedCtx::from_raw`.
    let ctx = match u! { SemTimedCtx::from_raw(data) } {
        Some(c) => c, None => return,
    };
    let sem = ctx.sem_ptr();
    if sem.is_null() { return; }
    if ctx.armed() {
        let tn = ctx.timer_node_ptr();
        sched_timer_done(tn);
        ctx.set_armed(false);
    }
    if sleep_cb_status != 0 {
        spin_lock(lk_ptr(sem));
    }
}}

// ---------------------------------------------------------------------------
// Wake one waiter; mirrors C `__sem_do_post`. Caller holds `sem->lk`.
// ---------------------------------------------------------------------------

fn sem_do_post(s: *mut RawSemaphore) -> c_int {
    let val = value_inc(s);
    if val <= 0 {
        let t = tq_wakeup(wq_ptr(s), 0, 0);
        if t.is_null() {
            return -(ENOENT as c_int);
        }
        // IS_ERR / PTR_ERR: in xv6 `*mut thread` carrying errno is an
        // address in the top kernel region. Treat any address with the
        // top byte set as an error and decode the embedded negative
        // errno. Mirrors `IS_ERR(t) ? PTR_ERR(t) : 0`.
        let addr = t as usize;
        // err.h: `IS_ERR_VALUE(x) = (unsigned long)-x <= MAX_ERRNO` (4095).
        // PTR_ERR(t) = (long)t (already negative).
        let neg = addr as isize;
        if (-neg) as usize <= 4095 && neg < 0 {
            return neg as c_int;
        }
    }
    0
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

pub(crate) fn sem_init(s: *mut sem_t, name: *const c_char, value: c_int)-> c_int  { u! {
    if s.is_null() { return -(EINVAL as c_int); }
    if value < 0 { return -(EINVAL as c_int); }
    let s = as_native(s);
    let n = if name.is_null() {
        b"unnamed\0".as_ptr() as *const c_char
    } else {
        name
    };
    set_name_value(s, n, value);
    spin_init(lk_ptr(s), b"semaphore spinlock\0".as_ptr() as *const c_char);
    tq_init(wq_ptr(s),
            b"semaphore wait queue\0".as_ptr() as *const c_char,
            lk_ptr(s));
    0
}}

pub(crate) fn sem_trywait(s: *mut sem_t)-> c_int  { u! {
    if s.is_null() { return -(EINVAL as c_int); }
    let s = as_native(s);
    let _g = KSpinlock::from_bindings(lk_ptr(s)).lock();
    if value_get(s) > 0 {
        value_dec(s);
        return 0;
    }
    -(EAGAIN as c_int)
}}

pub(crate) fn sem_wait(s: *mut sem_t)-> c_int  { u! {
    if s.is_null() { return -(EINVAL as c_int); }
    let s = as_native(s);
    let cur = machine::current_thread_ptr();

    let _g = KSpinlock::from_bindings(lk_ptr(s)).lock();
    let val = value_dec(s);
    if val < -SEM_VALUE_MAX {
        value_inc(s);
        return -(EOVERFLOW as c_int);
    }
    if val >= 0 { return 0; }
    machine::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
    let ret = tq_wait(wq_ptr(s), lk_ptr(s), null_mut());
    if ret != 0 {
        let wake_ret = sem_do_post(s);
        if wake_ret != 0 && wake_ret != -(ENOENT as c_int) {
            printf_post_failed(name_of(s));
        }
    }
    ret
}}

pub(crate) fn sem_wait_interruptible(s: *mut sem_t)-> c_int  { u! {
    if s.is_null() { return -(EINVAL as c_int); }
    let cur = machine::current_thread_ptr();
    loop {
        let ret = sem_trywait(s);
        if ret == 0 { return 0; }
        if ret != -(EAGAIN as c_int) { return ret; }
        if signal_pending(cur) != 0 { return -(EINTR as c_int); }
        sleep_ms(1);
    }
}}

pub(crate) fn sem_timedwait(s: *mut sem_t, timeout_ms: u64)-> c_int  { u! {
    if s.is_null() { return -(EINVAL as c_int); }
    let cur = machine::current_thread_ptr();

    if timeout_ms == 0 {
        return if sem_trywait(s) == 0 { 0 } else { -(ETIMEDOUT as c_int) };
    }

    let s = as_native(s);
    let _g = KSpinlock::from_bindings(lk_ptr(s)).lock();
    let val = value_dec(s);
    if val < -SEM_VALUE_MAX {
        value_inc(s);
        return -(EOVERFLOW as c_int);
    }
    if val >= 0 { return 0; }

    let timeout_ticks = machine::ms_to_rawticks(timeout_ms);
    let start = machine::read_time();
    let mut ctx = SemTimedCtx {
        sem: s,
        // SAFETY: zero-init of `timer_node` matches C `.timer = {0}`.
        timer: u! { core::mem::zeroed() },
        timeout_ms,
        timer_armed: false,
    };

    machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
    let ret = tq_wait_cb(
        wq_ptr(s),
        Some(sem_timed_sleep_cb),
        Some(sem_timed_wake_cb),
        &mut ctx as *mut _ as *mut c_void,
        null_mut(),
    );

    if ret != 0 {
        let wake_ret = sem_do_post(s);
        if wake_ret != 0 && wake_ret != -(ENOENT as c_int) {
            // SAFETY: variadic printf.
            u! {
                crate::kprintln!(
                    "Failed to post semaphore '{}' after timed wait wakeup",
                    crate::printf::Cs(name_of(s)));
            }
        }
        if signal_pending(cur) != 0 { return -(EINTR as c_int); }
        if (machine::read_time().wrapping_sub(start)) >= timeout_ticks {
            return -(ETIMEDOUT as c_int);
        }
    }
    ret
}}

pub(crate) fn sem_post(s: *mut sem_t)-> c_int  { u! {
    if s.is_null() { return -(EINVAL as c_int); }
    let s = as_native(s);
    let _g = KSpinlock::from_bindings(lk_ptr(s)).lock();
    if value_get(s) == SEM_VALUE_MAX { return -(EOVERFLOW as c_int); }
    let ret = sem_do_post(s);
    if ret == -(ENOENT as c_int) { 0 } else { ret }
}}

pub(crate) fn sem_getvalue(s: *mut sem_t, value: *mut c_int)-> c_int  { u! {
    if s.is_null() || value.is_null() { return -(EINVAL as c_int); }
    let s = as_native(s);
    let _g = KSpinlock::from_bindings(lk_ptr(s)).lock();
    let v = value_get(s);
    // SAFETY: caller-supplied out-pointer.
    u! { *value = v; }
    0
}}

// ===========================================================================
// Rust-native typed handle
// ===========================================================================

use core::marker::PhantomData;

/// Errors returned by the semaphore API.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum SemError {
    Invalid,
    /// Non-blocking variant would have blocked (`EAGAIN`).
    WouldBlock,
    Interrupted,
    TimedOut,
    Overflow,
    NotFound,
    Other(c_int),
}

fn map_sem_err(r: c_int) -> SemError {
    match -r as u32 {
        EINVAL => SemError::Invalid,
        EAGAIN => SemError::WouldBlock,
        EINTR => SemError::Interrupted,
        ETIMEDOUT => SemError::TimedOut,
        EOVERFLOW => SemError::Overflow,
        ENOENT => SemError::NotFound,
        _ => SemError::Other(r),
    }
}

/// Typed handle to a kernel `semaphore`.
#[derive(Clone, Copy)]
pub struct KSemaphore {
    raw: *mut sem_t,
    _ns: PhantomData<*const ()>,
}

impl KSemaphore {
    #[inline(always)]
    pub fn from_ptr(raw: *mut sem_t) -> Self {
        Self { raw, _ns: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut sem_t { self.raw }

    /// Initialise. `name` must be a static NUL-terminated string.
    #[inline]
    pub fn init(self, name: *const c_char, value: c_int) -> Result<(), SemError> {
        // SAFETY: handle wraps live storage; `name` validity per caller.
        let r = u! { sem_init(self.raw, name, value) };
        if r == 0 { Ok(()) } else { Err(map_sem_err(r)) }
    }

    /// Non-blocking acquire.
    #[inline]
    pub fn try_wait(self) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { sem_trywait(self.raw) };
        if r == 0 { Ok(()) } else { Err(map_sem_err(r)) }
    }

    /// Blocking acquire (uninterruptible).
    #[inline]
    pub fn wait(self) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { sem_wait(self.raw) };
        if r == 0 { Ok(()) } else { Err(map_sem_err(r)) }
    }

    /// Blocking acquire (interruptible).
    #[inline]
    pub fn wait_interruptible(self) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { sem_wait_interruptible(self.raw) };
        if r == 0 { Ok(()) } else { Err(map_sem_err(r)) }
    }

    /// Timed acquire.
    #[inline]
    pub fn timed_wait(self, timeout_ms: u64) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { sem_timedwait(self.raw, timeout_ms) };
        if r == 0 { Ok(()) } else { Err(map_sem_err(r)) }
    }

    /// Release one token.
    #[inline]
    pub fn post(self) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { sem_post(self.raw) };
        if r == 0 { Ok(()) } else { Err(map_sem_err(r)) }
    }

    /// Read the current counter value.
    #[inline]
    pub fn value(self) -> Result<c_int, SemError> {
        let mut v: c_int = 0;
        // SAFETY: see `init`; `&mut v` is a valid out-pointer.
        let r = u! { sem_getvalue(self.raw, &mut v) };
        if r == 0 { Ok(v) } else { Err(map_sem_err(r)) }
    }
}