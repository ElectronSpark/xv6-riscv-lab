//! Rust port of `kernel/lock/completion.c`.
//!
//! Implements the kernel completion primitive (counter + wait queue
//! protected by a spinlock). Exports the same `extern "C"` ABI as the
//! original C file; call sites elsewhere in the kernel see no change.
//!
//! Centralised-unsafe pattern: every raw pointer / FFI access is
//! confined to a single-line `u! { ... }` block, the rest of the
//! code is plain safe Rust.

#![allow(non_camel_case_types, non_snake_case)]


use core::ffi::{c_char, c_int, c_void};
use core::ptr::{addr_of_mut, null_mut};

use crate::u;
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
// to wrap each invocation in `u! { ... }`.

// P3-D2a: the thread-queue primitives (kernel/proc/thread_queue.rs) are
// ordinary Rust fns, reached as plain crate-path items instead of
// `extern "C"` redeclarations.
use crate::proc::{tq_bulk_move, tq_init, tq_size, tq_wait, tq_wait_cb, tq_wakeup, tq_wakeup_all};

// P3-D2b: `signal_pending` (proc/signal.rs) is a plain crate-path item
// now that its `#[no_mangle]` export is gone. The old redeclaration here
// said `-> u32`; the real fn returns `bool` (same 0/1 in `a0` under the
// old C ABI), so the call sites drop their `!= 0`.
use crate::proc::signal_pending;

// P3-D3c: `sched_timer_set`/`sched_timer_done` are genuinely `unsafe fn`
// in `crate::timer::sched_timer` now that their `#[no_mangle]` exports are
// gone; this file's original extern declarations asserted `pub safe fn`
// (usual FFI-facade convention). The thin wrappers below preserve that
// safe facade for the unchanged call sites.
/// SAFETY: see [`crate::timer::sched_timer::sched_timer_set`]'s contract.
fn sched_timer_set(tn: *mut timer_node, ticks: u64) -> c_int {
    unsafe { crate::timer::sched_timer::sched_timer_set(tn, ticks) }
}
/// SAFETY: see [`crate::timer::sched_timer::sched_timer_done`]'s contract.
fn sched_timer_done(tn: *mut timer_node) {
    unsafe { crate::timer::sched_timer::sched_timer_done(tn) }
}
pub(crate) use crate::lock::spinlock::{spin_holding, spin_lock, spin_unlock};

/// `crate::lock::spinlock::spin_init` takes `name: *mut c_char`; this
/// file's (and several sibling lock modules') original extern declaration
/// typed it `*const c_char` (call sites only ever pass a `&'static
/// [u8]`-derived string literal pointer, never written through). Thin
/// cast wrapper preserving the original call-site type.
#[inline]
fn spin_init(lk: *mut spinlock_t, name: *const c_char) {
    // SAFETY: `name` is only read by the callee despite the `*mut`
    // parameter; every call site here passes a `'static` string literal.
    unsafe { crate::lock::spinlock::spin_init(lk, name as *mut c_char) };
}

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3A.
//
// `crate::bindings::completion_t` (`kernel/inc/lock/completion_types.h`)
// has ~60 non-`lock/` referents (`KCompletion::raw` is embedded in
// bindgen structs across mm/dev/vfs), so the C-ABI entry points below
// keep `*mut completion_t` unchanged. `RawCompletion` is this file's
// own native, layout-identical working type; `lock` reuses
// `spinlock::RawSpinlock`, `wait_queue` stays the bindgen `tq_t`
// (proc-owned, out of this wave's scope).
// P3-N2: `RawCompletion` IS the kernel-wide Rust definition of
// `completion_t` now (`build.rs` blocklists the bindgen `completion_t`
// — the C type is an anonymous struct typedef, so that is its only
// name — and re-exports this type under it), so bindgen structs
// embedding a completion by value (`pcache.flush_completion`,
// `tty.completion`, `bio.io_completion`, ...) resolve here and require
// the same `Copy`/`Clone` derives the bindgen struct had.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct RawCompletion {
    pub(crate) lock: crate::lock::spinlock::RawSpinlock,
    pub(crate) done: c_int,
    pub(crate) wait_queue: tq_t,
}

// P3-N2 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (`#[repr(C)] #[repr(align(64))]
// pub struct completion_t { lock: spinlock_t, done: c_int, wait_queue:
// tq_t }`) and independently confirmed by a riscv64-unknown-elf-gcc
// `_Static_assert` probe against `kernel/inc/lock/completion_types.h`
// (size 128, align 64, offsets 0/24/32).
const _: () = {
    assert!(core::mem::size_of::<RawCompletion>() == 128, "completion_t size (80 payload bytes padded to 2 cachelines)");
    assert!(core::mem::align_of::<RawCompletion>() == 64, "completion_t alignment (from embedded spinlock_t)");
    assert!(core::mem::offset_of!(RawCompletion, lock) == 0, "completion.lock offset");
    assert!(core::mem::offset_of!(RawCompletion, done) == 24, "completion.done offset");
    assert!(core::mem::offset_of!(RawCompletion, wait_queue) == 32, "completion.wait_queue offset");
};

/// Reinterpret the bindgen `*mut completion_t` as the native mirror.
///
/// SAFETY: layout equivalence is proven at compile time by the
/// assertions above; the caller provides a valid, non-dangling
/// `*mut completion_t`.
#[inline(always)]
fn as_native(c: *mut completion_t) -> *mut RawCompletion {
    c as *mut RawCompletion
}

// ---------------------------------------------------------------------------
// Field accessors (centralised unsafe)
// ---------------------------------------------------------------------------

#[inline(always)]
fn done_get(c: *mut RawCompletion) -> c_int {
    // SAFETY: caller holds `c->lock` (per the C contract).
    u! { (*c).done }
}
#[inline(always)]
fn done_set(c: *mut RawCompletion, v: c_int) {
    // SAFETY: caller holds `c->lock`.
    u! { (*c).done = v; }
}
#[inline(always)]
fn lock_ptr(c: *mut RawCompletion) -> *mut spinlock_t {
    // SAFETY: dereferences a structurally-valid completion. We only
    // take the address of the embedded lock; no field is read.
    u! { addr_of_mut!((*c).lock) as *mut spinlock_t }
}
#[inline(always)]
fn queue_ptr(c: *mut RawCompletion) -> *mut tq_t {
    // SAFETY: see `lock_ptr`.
    u! { addr_of_mut!((*c).wait_queue) }
}
#[inline(always)]
fn tq_counter(q: *mut tq_t) -> c_int {
    // SAFETY: caller holds the tq's protecting lock.
    u! { (*q).counter }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Try to consume one completion token. Caller must hold `c->lock`.
fn try_wait_for_completion_locked(c: *mut RawCompletion) -> bool {
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
fn completion_do_wake(c: *mut RawCompletion) {
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
    comp: *mut RawCompletion,
    timer: timer_node,
    timeout_ms: u64,
    timer_armed: bool,
}

impl CompletionTimedCtx {
    #[inline(always)]
    unsafe fn from_raw<'a>(data: *mut c_void) -> Option<&'a mut Self> {
        if data.is_null() { None } else { Some(u! { &mut *(data as *mut Self) }) }
    }
    #[inline(always)] fn comp_ptr(&self) -> *mut RawCompletion { self.comp }
    #[inline(always)] fn timer_node_ptr(&mut self) -> *mut timer_node { addr_of_mut!(self.timer) }
    #[inline(always)] fn timeout(&self) -> u64 { self.timeout_ms }
    #[inline(always)] fn armed(&self) -> bool { self.timer_armed }
    #[inline(always)] fn set_armed(&mut self, v: bool) { self.timer_armed = v; }
}

extern "C" fn completion_timed_sleep_cb(data: *mut c_void)-> c_int  { u! {
    // SAFETY: see `CompletionTimedCtx::from_raw`.
    let ctx = match u! { CompletionTimedCtx::from_raw(data) } {
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
}}

extern "C" fn completion_timed_wake_cb(data: *mut c_void, sleep_cb_status: c_int) { u! {
    // SAFETY: see `CompletionTimedCtx::from_raw`.
    let ctx = match u! { CompletionTimedCtx::from_raw(data) } {
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
}}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

pub(crate) fn completion_init(c: *mut completion_t) { u! {
    if c.is_null() { return; }
    let c = as_native(c);
    done_set(c, 0);
    spin_init(lock_ptr(c), b"completion_spin\0".as_ptr() as *const c_char);
    tq_init(queue_ptr(c),
            b"completion_queue\0".as_ptr() as *const c_char,
            lock_ptr(c));
}}

pub(crate) fn completion_reinit(c: *mut completion_t) { u! {
    if c.is_null() { return; }
    let c = as_native(c);
    done_set(c, 0);
}}

pub(crate) fn try_wait_for_completion(c: *mut completion_t)-> bool  { u! {
    if c.is_null() { return false; }
    let c = as_native(c);
    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    try_wait_for_completion_locked(c)
}}

pub(crate) fn wait_for_completion(c: *mut completion_t) { u! {
    if c.is_null() { return; }
    let c = as_native(c);
    let cur = machine::current_thread_ptr();
    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    while !try_wait_for_completion_locked(c) {
        machine::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
        let _ = tq_wait(queue_ptr(c), lock_ptr(c), null_mut());
    }
    if done_get(c) > 0 {
        completion_do_wake(c);
    }
}}

pub(crate) fn wait_for_completion_interruptible(c: *mut completion_t)-> c_int  { u! {
    if c.is_null() { return -(EINVAL as c_int); }
    let c = as_native(c);
    let cur = machine::current_thread_ptr();

    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    while !try_wait_for_completion_locked(c) {
        if signal_pending(cur) { return -(EINTR as c_int); }
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait(queue_ptr(c), lock_ptr(c), null_mut());
        if ret != 0
            && signal_pending(cur)
            && !try_wait_for_completion_locked(c)
        {
            return -(EINTR as c_int);
        }
    }
    if done_get(c) > 0 {
        completion_do_wake(c);
    }
    0
}}

pub(crate) fn wait_for_completion_timed(c: *mut completion_t,
                                                   timeout_ms: u64)-> c_int  { u! {
    if c.is_null() { return -(EINVAL as c_int); }
    let cur = machine::current_thread_ptr();

    if timeout_ms == 0 {
        if try_wait_for_completion(c) {
            return 0;
        }
        return -(ETIMEDOUT as c_int);
    }

    let c = as_native(c);
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
        if signal_pending(cur) { return -(EINTR as c_int); }
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
            timer: u! { core::mem::zeroed() },
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
            && signal_pending(cur)
            && !try_wait_for_completion_locked(c)
        {
            return -(EINTR as c_int);
        }
    }

    if done_get(c) > 0 {
        completion_do_wake(c);
    }
    0
}}

pub(crate) fn complete(c: *mut completion_t) { u! {
    if c.is_null() { return; }
    let c = as_native(c);
    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    let d = done_get(c);
    if d != MAX_COMPLETIONS {
        done_set(c, d + 1);
    }
    completion_do_wake(c);
}}

pub(crate) fn complete_all(c: *mut completion_t) { u! {
    if c.is_null() { return; }
    let c = as_native(c);

    // Local temporary queue: collect waiters, release the completion
    // lock, then wake them outside the lock. Avoids a lock convoy on
    // re-acquisition inside the scheduler. Matches C `complete_all`.
    let mut temp_queue: tq_t = u! { core::mem::zeroed() };
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
}}

pub(crate) fn completion_done(c: *mut completion_t)-> bool  { u! {
    if c.is_null() { return false; }
    let c = as_native(c);
    let _g = KSpinlock::from_bindings(lock_ptr(c)).lock();
    tq_size(queue_ptr(c)) == 0
}}

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
        u! { completion_init(self.raw); }
    }

    /// Re-arm the completion. Mirrors C `completion_reinit`.
    #[inline]
    pub fn reinit(self) {
        // SAFETY: as above; lock-protected internally.
        u! { completion_reinit(self.raw); }
    }

    /// Block (uninterruptible) until a token is available.
    #[inline]
    pub fn wait(self) {
        // SAFETY: handle wraps a structurally-valid completion.
        u! { wait_for_completion(self.raw); }
    }

    /// Non-blocking try-wait. `true` iff a token was consumed.
    #[inline]
    pub fn try_wait(self) -> bool {
        // SAFETY: see `wait`.
        u! { try_wait_for_completion(self.raw) }
    }

    /// Block (interruptible). Returns `Err(Interrupted)` on signal.
    #[inline]
    pub fn wait_interruptible(self) -> Result<(), CompletionError> {
        // SAFETY: see `wait`.
        let r = u! { wait_for_completion_interruptible(self.raw) };
        if r == 0 { Ok(()) }
        else if r == -(EINTR as c_int) { Err(CompletionError::Interrupted) }
        else { Err(CompletionError::Invalid) }
    }

    /// Block with a millisecond timeout.
    #[inline]
    pub fn wait_timed(self, timeout_ms: u64) -> Result<(), CompletionError> {
        // SAFETY: see `wait`.
        let r = u! { wait_for_completion_timed(self.raw, timeout_ms) };
        if r == 0 { Ok(()) }
        else if r == -(ETIMEDOUT as c_int) { Err(CompletionError::TimedOut) }
        else if r == -(EINTR as c_int) { Err(CompletionError::Interrupted) }
        else { Err(CompletionError::Invalid) }
    }

    /// Add a token, waking at most one waiter.
    #[inline]
    pub fn complete(self) {
        // SAFETY: see `wait`.
        u! { complete(self.raw); }
    }

    /// Saturate the counter and wake all waiters.
    #[inline]
    pub fn complete_all(self) {
        // SAFETY: see `wait`.
        u! { complete_all(self.raw); }
    }

    /// `true` iff there are no pending waiters.
    #[inline]
    pub fn done(self) -> bool {
        // SAFETY: see `wait`.
        u! { completion_done(self.raw) }
    }
}