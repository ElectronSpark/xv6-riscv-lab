//! Rust port of `kernel/lock/mutex.c`. Preserves C ABI.

#![allow(non_camel_case_types, non_snake_case)]


use core::ffi::{c_char, c_int, c_void};
use core::ptr::{addr_of_mut, null_mut};
use core::sync::atomic::{AtomicI32, Ordering};

use crate::u;
use crate::bindings::{
    mutex_t, spinlock_t, thread, timer_node, tq_t,
    thread_state_THREAD_INTERRUPTIBLE, thread_state_THREAD_UNINTERRUPTIBLE,
    EINTR, EINVAL, ETIMEDOUT,
};
use crate::machine;
use crate::sync::KSpinlock;

// P3-D2a: the thread-queue primitives (kernel/proc/thread_queue.rs) are
// ordinary Rust fns, reached as plain crate-path items instead of
// `extern "C"` redeclarations.
use crate::proc::{tq_init, tq_wait, tq_wait_cb, tq_wakeup};

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
// `crate::bindings::mutex_t` (`kernel/inc/lock/mutex_types.h`) has ~70
// non-`lock/` referents (`KMutex::raw` is embedded in bindgen structs
// all over vfs/dev), so the C-ABI entry points below (`mutex_init`,
// `mutex_lock`, ...) and `KMutex`/`KMutexGuard`'s public field type
// keep `*mut mutex_t` unchanged. `RawMutex` is this file's own native,
// layout-identical working type: every private helper below (and the
// public entry points, via one cast each) operates on it directly. The
// embedded `lk` field reuses `spinlock::RawSpinlock` (this wave's
// other lock-owned type); `wait_queue` stays the bindgen `tq_t` because
// thread-queues are proc-owned (`kernel/inc/proc/tq_type.h`), out of
// this wave's scope.
// P3-N2: `RawMutex` IS the kernel-wide Rust definition of `mutex_t`
// now (`build.rs` blocklists the bindgen `mutex`/`mutex_t` and
// re-exports this type under both names), so bindgen structs embedding
// a mutex by value (`tty.mutex`, `vfs_fdtable.lock`, ...) resolve here
// and require the same `Copy`/`Clone` derives the bindgen struct had.
// The `align(64)` reproduces what clang derived for the C struct from
// its `spinlock_t lk` member (the typedef carries aligned(64); see
// spinlock.rs's layout note).
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct RawMutex {
    pub(crate) lk: crate::lock::spinlock::RawSpinlock,
    pub(crate) wait_queue: tq_t,
    pub(crate) name: *mut c_char,
    pub(crate) holder: c_int,
}

// P3-N2 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (`#[repr(C)] #[repr(align(64))]
// pub struct mutex { lk: spinlock_t, wait_queue: tq_t, name: *mut
// c_char, holder: pid_t }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe against
// `kernel/inc/lock/mutex_types.h` (size 128, align 64,
// offsets 0/24/72/80).
const _: () = {
    assert!(core::mem::size_of::<RawMutex>() == 128, "mutex_t size (84 payload bytes padded to 2 cachelines)");
    assert!(core::mem::align_of::<RawMutex>() == 64, "mutex_t alignment (from embedded spinlock_t)");
    assert!(core::mem::offset_of!(RawMutex, lk) == 0, "mutex.lk offset");
    assert!(core::mem::offset_of!(RawMutex, wait_queue) == 24, "mutex.wait_queue offset");
    assert!(core::mem::offset_of!(RawMutex, name) == 72, "mutex.name offset");
    assert!(core::mem::offset_of!(RawMutex, holder) == 80, "mutex.holder offset");
};

/// Reinterpret the bindgen `*mut mutex_t` as the native mirror.
///
/// SAFETY: layout equivalence is proven at compile time by the
/// assertions above; the caller provides a valid, non-dangling
/// `*mut mutex_t`.
#[inline(always)]
fn as_native(m: *mut mutex_t) -> *mut RawMutex {
    m as *mut RawMutex
}

// ---------------------------------------------------------------------------
// Centralised-unsafe field accessors
// ---------------------------------------------------------------------------

#[inline(always)]
fn lk_ptr(m: *mut RawMutex) -> *mut spinlock_t {
    // SAFETY: structurally valid mutex.
    u! { addr_of_mut!((*m).lk) as *mut spinlock_t }
}
#[inline(always)]
fn wq_ptr(m: *mut RawMutex) -> *mut tq_t {
    // SAFETY: see `lk_ptr`.
    u! { addr_of_mut!((*m).wait_queue) }
}
#[inline(always)]
fn holder_atomic<'a>(m: *mut RawMutex) -> &'a AtomicI32 {
    // SAFETY: `holder` is a 4-byte aligned `pid_t` (`c_int`). We model
    // it as `AtomicI32` to perform `smp_store_release` / `_load_acquire`
    // / `atomic_cas` matching the C macros.
    u! { &*(addr_of_mut!((*m).holder) as *const AtomicI32) }
}
#[inline(always)]
fn set_name(m: *mut RawMutex, name: *mut c_char) {
    // SAFETY: caller has exclusive access at init time.
    u! { (*m).name = name; }
}
/// Mirror `__mutex_set_holder` (smp_store_release).
#[inline(always)]
fn set_holder(m: *mut RawMutex, pid: c_int) {
    holder_atomic(m).store(pid, Ordering::Release);
}
/// Mirror `__mutex_holder` (smp_load_acquire).
#[inline(always)]
fn get_holder(m: *mut RawMutex) -> c_int {
    holder_atomic(m).load(Ordering::Acquire)
}
/// Mirror `__mutex_try_set_holder` = atomic_cas(&holder, -1, pid).
/// Returns true on success (we took the lock).
#[inline(always)]
fn try_set_holder(m: *mut RawMutex, pid: c_int) -> bool {
    holder_atomic(m)
        .compare_exchange(-1, pid, Ordering::Acquire, Ordering::Relaxed)
        .is_ok()
}
/// Wake one waiter; caller holds `lk->lk`. Mirrors `__do_wakeup`.
fn do_wakeup(m: *mut RawMutex) {
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
// `u! { ... }` block (the reborrow from the C-supplied `*mut
// c_void` to `&mut MutexTimedCtx`); the rest of the callback body is
// plain safe Rust.
// ---------------------------------------------------------------------------

#[repr(C)]
struct MutexTimedCtx {
    lock: *mut RawMutex,
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
        if data.is_null() { None } else { Some(u! { &mut *(data as *mut Self) }) }
    }
    #[inline(always)] fn lock_ptr(&self) -> *mut RawMutex { self.lock }
    #[inline(always)] fn timer_node_ptr(&mut self) -> *mut timer_node {
        addr_of_mut!(self.timer)
    }
    #[inline(always)] fn timeout(&self) -> u64 { self.timeout_ms }
    #[inline(always)] fn armed(&self) -> bool { self.timer_armed }
    #[inline(always)] fn set_armed(&mut self, v: bool) { self.timer_armed = v; }
}

extern "C" fn mutex_timed_sleep_cb(data: *mut c_void)-> c_int  { u! {
    // SAFETY: see `MutexTimedCtx::from_raw`.
    let ctx = match u! { MutexTimedCtx::from_raw(data) } {
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
}}

extern "C" fn mutex_timed_wake_cb(data: *mut c_void, sleep_cb_status: c_int) { u! {
    // SAFETY: see `MutexTimedCtx::from_raw`.
    let ctx = match u! { MutexTimedCtx::from_raw(data) } {
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
}}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

pub(crate) fn mutex_init(m: *mut mutex_t, name: *mut c_char) { u! {
    let m = as_native(m);
    spin_init(lk_ptr(m), b"sleep lock\0".as_ptr() as *const c_char);
    tq_init(wq_ptr(m),
            b"sleep lock wait queue\0".as_ptr() as *const c_char,
            lk_ptr(m));
    set_name(m, name);
    set_holder(m, -1);
}}

pub(crate) fn mutex_lock(m: *mut mutex_t) { u! {
    let m = as_native(m);
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);

    if try_set_holder(m, pid) { return; }

    let _g = KSpinlock::from_bindings(lk_ptr(m)).lock();
    if try_set_holder(m, pid) { return; }

    while get_holder(m) != pid {
        machine::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
        let _ = tq_wait(wq_ptr(m), lk_ptr(m), null_mut());
    }
}}

pub(crate) fn mutex_unlock(m: *mut mutex_t) { u! {
    let m = as_native(m);
    let _g = KSpinlock::from_bindings(lk_ptr(m)).lock();
    do_wakeup(m);
}}

pub(crate) fn holding_mutex(m: *mut mutex_t)-> c_int  { u! {
    let m = as_native(m);
    let cur = machine::current_thread_ptr();
    if cur.is_null() { return 0; }
    // SeqCst load to match `__atomic_load_n(..., SEQ_CST)` in C.
    let h = holder_atomic(m).load(Ordering::SeqCst);
    if h == machine::thread_pid(cur) { 1 } else { 0 }
}}

pub(crate) fn mutex_trylock(m: *mut mutex_t)-> c_int  { u! {
    let m = as_native(m);
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);
    if try_set_holder(m, pid) { 1 } else { 0 }
}}

pub(crate) fn mutex_lock_interruptible(m: *mut mutex_t)-> c_int  { u! {
    if m.is_null() { return -(EINVAL as c_int); }
    let m = as_native(m);
    let cur = machine::current_thread_ptr();
    let pid = machine::thread_pid(cur);

    if try_set_holder(m, pid) { return 0; }

    let _g = KSpinlock::from_bindings(lk_ptr(m)).lock();
    if try_set_holder(m, pid) { return 0; }

    while get_holder(m) != pid {
        if signal_pending(cur) { return -(EINTR as c_int); }
        machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = tq_wait(wq_ptr(m), lk_ptr(m), null_mut());
        if ret != 0 && signal_pending(cur) && get_holder(m) != pid {
            return -(EINTR as c_int);
        }
    }
    0
}}

pub(crate) fn mutex_lock_timed(m: *mut mutex_t, timeout_ms: u64)-> c_int  { u! {
    if m.is_null() { return -(EINVAL as c_int); }
    let m = as_native(m);
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
            timer: u! { core::mem::zeroed() },
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
        if ret != 0 && signal_pending(cur) && get_holder(m) != pid {
            return -(EINTR as c_int);
        }
    }
    0
}}

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
        u! { mutex_init(self.raw, name); }
    }

    /// `true` iff the current thread holds this mutex.
    #[inline]
    pub fn holding(self) -> bool {
        // SAFETY: see `init`.
        u! { holding_mutex(self.raw) != 0 }
    }

    /// Acquire (uninterruptible). Returns RAII guard.
    #[inline]
    pub fn lock(self) -> KMutexGuard {
        // SAFETY: see `init`.
        u! { mutex_lock(self.raw); }
        KMutexGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Non-blocking acquire.
    #[inline]
    pub fn try_lock(self) -> Option<KMutexGuard> {
        // SAFETY: see `init`. Returns 1 on success.
        let got = u! { mutex_trylock(self.raw) };
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
        let r = u! { mutex_lock_interruptible(self.raw) };
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
        let r = u! { mutex_lock_timed(self.raw, timeout_ms) };
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
        u! { mutex_unlock(self.raw); }
    }
}