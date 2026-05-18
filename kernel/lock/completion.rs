//! Rust port of `kernel/lock/completion.c`.
//!
//! Implements the kernel completion primitive (counter + wait queue
//! protected by a spinlock). Exports the same `extern "C"` ABI as the
//! original C file; call sites elsewhere in the kernel see no change.
//!
//! Centralised-unsafe pattern: every raw pointer / FFI access is
//! confined to a single-line `unsafe { ... }` block, the rest of the
//! code is plain safe Rust.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr::{addr_of_mut, null_mut};

use crate::bindings::{
    completion_t, spinlock_t, thread, timer_node, tq_t,
    thread_state_THREAD_INTERRUPTIBLE, thread_state_THREAD_UNINTERRUPTIBLE,
    EINTR, EINVAL, ETIMEDOUT,
};
use crate::machine;
use crate::sync::KSpinlock;

// Mirror C `MAX_COMPLETIONS`.
const MAX_COMPLETIONS: c_int = 65535;

// ---------------------------------------------------------------------------
// Externs from the rest of the kernel
// ---------------------------------------------------------------------------
//
// Non-variadic C symbols are declared `safe` so call sites do not need
// to wrap each invocation in `unsafe { ... }`.

unsafe extern "C" {
    pub safe fn spin_init(lk: *mut spinlock_t, name: *const c_char);
    pub safe fn spin_lock(lk: *mut spinlock_t);
    pub safe fn spin_unlock(lk: *mut spinlock_t);
    pub safe fn spin_holding(lk: *mut spinlock_t) -> c_int;

    pub safe fn tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t);
    pub safe fn tq_size(q: *mut tq_t) -> c_int;
    pub safe fn tq_wait(q: *mut tq_t, lock: *mut spinlock_t,
                        rdata: *mut u64) -> c_int;
    pub safe fn tq_wait_cb(q: *mut tq_t,
                           sleep_cb: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
                           wake_cb: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
                           data: *mut c_void,
                           rdata: *mut u64) -> c_int;
    pub safe fn tq_wakeup(q: *mut tq_t, error_no: c_int, rdata: u64) -> *mut thread;
    pub safe fn tq_wakeup_all(q: *mut tq_t, error_no: c_int, rdata: u64) -> c_int;
    pub safe fn tq_bulk_move(to: *mut tq_t, from: *mut tq_t) -> c_int;

    pub safe fn signal_pending(p: *mut thread) -> u32;

    pub safe fn sched_timer_set(tn: *mut timer_node, ticks: u64) -> c_int;
    pub safe fn sched_timer_done(tn: *mut timer_node);
}

// ---------------------------------------------------------------------------
// Field accessors (centralised unsafe)
// ---------------------------------------------------------------------------

#[inline(always)]
fn done_get(c: *mut completion_t) -> c_int {
    // SAFETY: caller holds `c->lock` (per the C contract).
    unsafe { (*c).done }
}
#[inline(always)]
fn done_set(c: *mut completion_t, v: c_int) {
    // SAFETY: caller holds `c->lock`.
    unsafe { (*c).done = v; }
}
#[inline(always)]
fn lock_ptr(c: *mut completion_t) -> *mut spinlock_t {
    // SAFETY: dereferences a structurally-valid completion. We only
    // take the address of the embedded lock; no field is read.
    unsafe { addr_of_mut!((*c).lock) }
}
#[inline(always)]
fn queue_ptr(c: *mut completion_t) -> *mut tq_t {
    // SAFETY: see `lock_ptr`.
    unsafe { addr_of_mut!((*c).wait_queue) }
}
#[inline(always)]
fn tq_counter(q: *mut tq_t) -> c_int {
    // SAFETY: caller holds the tq's protecting lock.
    unsafe { (*q).counter }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Try to consume one completion token. Caller must hold `c->lock`.
fn try_wait_for_completion_locked(c: *mut completion_t) -> bool {
    let d = done_get(c);
    if d <= 0 {
        return false;
    }
    if d != MAX_COMPLETIONS {
        done_set(c, d - 1);
    }
    true
}

/// Wake one waiter if any. Caller must hold `c->lock`.
fn completion_do_wake(c: *mut completion_t) {
    let q = queue_ptr(c);
    if tq_size(q) > 0 {
        // Return value (`*mut thread`) is intentionally discarded:
        // the C code keeps the same `(void)p` comment about interrupts.
        let _ = tq_wakeup(q, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Timed-wait context + callbacks
// ---------------------------------------------------------------------------

#[repr(C)]
struct CompletionTimedCtx {
    comp: *mut completion_t,
    timer: timer_node,
    timeout_ms: u64,
    timer_armed: bool,
}

impl CompletionTimedCtx {
    #[inline(always)]
    unsafe fn from_raw<'a>(data: *mut c_void) -> Option<&'a mut Self> {
        if data.is_null() { None } else { Some(unsafe { &mut *(data as *mut Self) }) }
    }
    #[inline(always)] fn comp_ptr(&self) -> *mut completion_t { self.comp }
    #[inline(always)] fn timer_node_ptr(&mut self) -> *mut timer_node { addr_of_mut!(self.timer) }
    #[inline(always)] fn timeout(&self) -> u64 { self.timeout_ms }
    #[inline(always)] fn armed(&self) -> bool { self.timer_armed }
    #[inline(always)] fn set_armed(&mut self, v: bool) { self.timer_armed = v; }
}

unsafe extern "C" fn completion_timed_sleep_cb(data: *mut c_void) -> c_int {
    // SAFETY: see `CompletionTimedCtx::from_raw`.
    let ctx = match unsafe { CompletionTimedCtx::from_raw(data) } {
        Some(c) => c, None => return 0,
    };
    let comp = ctx.comp_ptr();
    if comp.is_null() { return 0; }

    ctx.set_armed(false);
    let timeout_ms = ctx.timeout();
    if timeout_ms > 0 {
        let tn = ctx.timer_node_ptr();
        if sched_timer_set(tn, timeout_ms) == 0 {
            ctx.set_armed(true);
        }
    }

    let status = spin_holding(lock_ptr(comp));
    if status != 0 {
        spin_unlock(lock_ptr(comp));
    }
    status
}

unsafe extern "C" fn completion_timed_wake_cb(data: *mut c_void, sleep_cb_status: c_int) {
    // SAFETY: see `CompletionTimedCtx::from_raw`.
    let ctx = match unsafe { CompletionTimedCtx::from_raw(data) } {
        Some(c) => c, None => return,
    };
    let comp = ctx.comp_ptr();
    if comp.is_null() { return; }

    if ctx.armed() {
        let tn = ctx.timer_node_ptr();
        sched_timer_done(tn);
        ctx.set_armed(false);
    }

    if sleep_cb_status != 0 {
        spin_lock(lock_ptr(comp));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn completion_init(c: *mut completion_t) {
    if c.is_null() { return; }
    done_set(c, 0);
    spin_init(lock_ptr(c), b"completion_spin\0".as_ptr() as *const c_char);
    tq_init(queue_ptr(c),
            b"completion_queue\0".as_ptr() as *const c_char,
            lock_ptr(c));
}

#[no_mangle]
pub unsafe extern "C" fn completion_reinit(c: *mut completion_t) {
    if c.is_null() { return; }
    done_set(c, 0);
}

#[no_mangle]
pub unsafe extern "C" fn try_wait_for_completion(c: *mut completion_t) -> bool {
    if c.is_null() { return false; }
    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    try_wait_for_completion_locked(c)
}

#[no_mangle]
pub unsafe extern "C" fn wait_for_completion(c: *mut completion_t) {
    if c.is_null() { return; }
    let cur = machine::current_thread_ptr();
    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    while !try_wait_for_completion_locked(c) {
        machine::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
        let _ = tq_wait(queue_ptr(c), lock_ptr(c), null_mut());
    }
    if done_get(c) > 0 {
        completion_do_wake(c);
    }
}

#[no_mangle]
pub unsafe extern "C" fn wait_for_completion_interruptible(c: *mut completion_t) -> c_int {
    if c.is_null() { return -(EINVAL as c_int); }
    let cur = machine::current_thread_ptr();

    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    while !try_wait_for_completion_locked(c) {
        if signal_pending(cur) != 0 { return -(EINTR as c_int); }
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait(queue_ptr(c), lock_ptr(c), null_mut());
        if ret != 0
            && signal_pending(cur) != 0
            && !try_wait_for_completion_locked(c)
        {
            return -(EINTR as c_int);
        }
    }
    if done_get(c) > 0 {
        completion_do_wake(c);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn wait_for_completion_timed(c: *mut completion_t,
                                                   timeout_ms: u64) -> c_int {
    if c.is_null() { return -(EINVAL as c_int); }
    let cur = machine::current_thread_ptr();

    if timeout_ms == 0 {
        if try_wait_for_completion(c) {
            return 0;
        }
        return -(ETIMEDOUT as c_int);
    }

    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    if try_wait_for_completion_locked(c) {
        if done_get(c) > 0 {
            completion_do_wake(c);
        }
        return 0;
    }

    let timeout_ticks = machine::ms_to_rawticks(timeout_ms);
    let start = machine::read_time();

    while !try_wait_for_completion_locked(c) {
        if signal_pending(cur) != 0 { return -(EINTR as c_int); }
        let now = machine::read_time();
        let elapsed = now.wrapping_sub(start);
        if elapsed >= timeout_ticks { return -(ETIMEDOUT as c_int); }
        let remaining_ticks = timeout_ticks - elapsed;
        let tm = machine::tick_ms();
        let mut remaining_ms = if tm == 0 { 1 } else { (remaining_ticks + tm - 1) / tm };
        if remaining_ms == 0 { remaining_ms = 1; }

        let mut ctx = CompletionTimedCtx {
            comp: c,
            // SAFETY: zeroed `timer_node` is the documented initial
            // state (matches C `.timer = {0}` implicit zero-init).
            timer: unsafe { core::mem::zeroed() },
            timeout_ms: remaining_ms,
            timer_armed: false,
        };

        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait_cb(
            queue_ptr(c),
            Some(completion_timed_sleep_cb),
            Some(completion_timed_wake_cb),
            &mut ctx as *mut _ as *mut c_void,
            null_mut(),
        );
        if ret != 0
            && signal_pending(cur) != 0
            && !try_wait_for_completion_locked(c)
        {
            return -(EINTR as c_int);
        }
    }

    if done_get(c) > 0 {
        completion_do_wake(c);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn complete(c: *mut completion_t) {
    if c.is_null() { return; }
    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    let d = done_get(c);
    if d != MAX_COMPLETIONS {
        done_set(c, d + 1);
    }
    completion_do_wake(c);
}

#[no_mangle]
pub unsafe extern "C" fn complete_all(c: *mut completion_t) {
    if c.is_null() { return; }

    // Local temporary queue: collect waiters, release the completion
    // lock, then wake them outside the lock. Avoids a lock convoy on
    // re-acquisition inside the scheduler. Matches C `complete_all`.
    let mut temp_queue: tq_t = unsafe { core::mem::zeroed() };
    tq_init(&mut temp_queue as *mut tq_t,
            b"completion_temp\0".as_ptr() as *const c_char,
            null_mut());

    {
        let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
        done_set(c, MAX_COMPLETIONS);
        tq_bulk_move(&mut temp_queue as *mut tq_t, queue_ptr(c));
    }

    if tq_counter(&mut temp_queue as *mut tq_t) > 0 {
        tq_wakeup_all(&mut temp_queue as *mut tq_t, 0, 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn completion_done(c: *mut completion_t) -> bool {
    if c.is_null() { return false; }
    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    tq_size(queue_ptr(c)) == 0
}

// ===========================================================================
// Rust-native typed handle
// ===========================================================================
//
// `KCompletion` wraps a `*mut completion_t` and exposes the same
// operations as the C ABI, but with safe (`!unsafe`) call sites and
// `Result`-shaped error reporting. The handle itself is just a
// pointer wrapper; the storage is owned and pinned by the kernel
// (typically a `static` or an inline field of a longer-lived struct).
//
// Construct via [`KCompletion::from_ptr`]; the pointer must be
// initialised (via `init`) before any concurrent operation.

use core::marker::PhantomData;

/// Errors returned by the blocking completion-wait variants.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum CompletionError {
    /// Interrupted by a signal.
    Interrupted,
    /// Operation timed out (only `wait_timed`).
    TimedOut,
    /// `EINVAL` from the underlying primitive (null `c`, etc.).
    Invalid,
}

/// Typed handle to a kernel `completion_t`.
///
/// `Copy`-by-value because it is just a pointer; deliberately
/// `!Send + !Sync` so the handle itself does not cross harts.
#[derive(Clone, Copy)]
pub struct KCompletion {
    raw: *mut completion_t,
    _ns: PhantomData<*const ()>,
}

impl KCompletion {
    /// Wrap a pre-allocated `completion_t` pointer.
    #[inline(always)]
    pub fn from_ptr(raw: *mut completion_t) -> Self {
        Self { raw, _ns: PhantomData }
    }

    /// Borrow the raw pointer (escape hatch for FFI call sites).
    #[inline(always)]
    pub fn as_ptr(self) -> *mut completion_t { self.raw }

    /// Initialise the underlying storage. Mirrors C `completion_init`.
    #[inline]
    pub fn init(self) {
        // SAFETY: `completion_init` accepts any valid `*mut completion_t`;
        // caller asserts the pointer is to live storage not yet shared.
        unsafe { completion_init(self.raw); }
    }

    /// Re-arm the completion. Mirrors C `completion_reinit`.
    #[inline]
    pub fn reinit(self) {
        // SAFETY: as above; lock-protected internally.
        unsafe { completion_reinit(self.raw); }
    }

    /// Block (uninterruptible) until a token is available.
    #[inline]
    pub fn wait(self) {
        // SAFETY: handle wraps a structurally-valid completion.
        unsafe { wait_for_completion(self.raw); }
    }

    /// Non-blocking try-wait. `true` iff a token was consumed.
    #[inline]
    pub fn try_wait(self) -> bool {
        // SAFETY: see `wait`.
        unsafe { try_wait_for_completion(self.raw) }
    }

    /// Block (interruptible). Returns `Err(Interrupted)` on signal.
    #[inline]
    pub fn wait_interruptible(self) -> Result<(), CompletionError> {
        // SAFETY: see `wait`.
        let r = unsafe { wait_for_completion_interruptible(self.raw) };
        if r == 0 { Ok(()) }
        else if r == -(EINTR as c_int) { Err(CompletionError::Interrupted) }
        else { Err(CompletionError::Invalid) }
    }

    /// Block with a millisecond timeout.
    #[inline]
    pub fn wait_timed(self, timeout_ms: u64) -> Result<(), CompletionError> {
        // SAFETY: see `wait`.
        let r = unsafe { wait_for_completion_timed(self.raw, timeout_ms) };
        if r == 0 { Ok(()) }
        else if r == -(ETIMEDOUT as c_int) { Err(CompletionError::TimedOut) }
        else if r == -(EINTR as c_int) { Err(CompletionError::Interrupted) }
        else { Err(CompletionError::Invalid) }
    }

    /// Add a token, waking at most one waiter.
    #[inline]
    pub fn complete(self) {
        // SAFETY: see `wait`.
        unsafe { complete(self.raw); }
    }

    /// Saturate the counter and wake all waiters.
    #[inline]
    pub fn complete_all(self) {
        // SAFETY: see `wait`.
        unsafe { complete_all(self.raw); }
    }

    /// `true` iff there are no pending waiters.
    #[inline]
    pub fn done(self) -> bool {
        // SAFETY: see `wait`.
        unsafe { completion_done(self.raw) }
    }
}
