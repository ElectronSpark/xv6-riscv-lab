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
// NO-STANDALONE-FN: the `tq_init`/`tq_wait`/`tq_wait_cb`/`tq_wakeup` free-fn
// delegators were deleted; each call site builds a `TqRef` handle via
// `from_ptr` and invokes the corresponding inherent method.
use crate::proc::access::TqRef;

// P3-D2b: `signal_pending` (proc/signal.rs) is a plain crate-path item
// now that its `#[no_mangle]` export is gone. The old redeclaration here
// said `-> u32`; the real fn returns `bool` (same 0/1 in `a0` under the
// old C ABI), so the call sites drop their `!= 0`.

// P3-D3c: `sched_timer_set`/`sched_timer_done` are genuinely `unsafe fn`
// in `crate::timer::sched_timer` now that their `#[no_mangle]` exports are
// gone; this file's original extern declarations asserted `pub safe fn`
// (usual FFI-facade convention). The thin wrappers below preserve that
// safe facade for the unchanged call sites.
/// SAFETY: see [`crate::timer::sched_timer::sched_timer_set`]'s contract.
fn sched_timer_set(tn: *mut timer_node, ticks: u64) -> c_int {
    unsafe { crate::timer::sched_timer::SchedTimer::sched_timer_set(tn, ticks) }
}
/// SAFETY: see [`crate::timer::sched_timer::sched_timer_done`]'s contract.
fn sched_timer_done(tn: *mut timer_node) {
    unsafe { crate::timer::sched_timer::SchedTimer::sched_timer_done(tn) }
}
// `sleep_ms` stayed a safe fn -- plain crate-path import.
use crate::timer::sched_timer::SchedTimer;
use crate::lock::spinlock::RawSpinlock;

/// See `completion.rs`'s identical note: `crate::lock::spinlock::RawSpinlock::init`
/// takes `name: *mut c_char`; this file's original extern declaration
/// typed it `*const c_char` (call site only ever passes a `'static`
/// string-literal pointer, never written through).
#[inline]
fn spin_init(lk: *mut spinlock_t, name: *const c_char) {
    // SAFETY: `name` is only read by the callee despite the `*mut`
    // parameter; the sole call site passes a `'static` string literal.
    unsafe { crate::lock::spinlock::RawSpinlock::init(lk, name as *mut c_char) };
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
// P3-N2: `RawSemaphore` IS the kernel-wide Rust definition of `struct
// semaphore` now (`build.rs` blocklists the bindgen `semaphore` and
// re-exports this type under that name; the C `sem_t` typedef never
// made it into the bindgen output — this file's `sem_t` is a local
// `use ... semaphore as sem_t` rename, which keeps resolving here).
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct RawSemaphore {
    pub(crate) lk: crate::lock::spinlock::RawSpinlock,
    pub(crate) wait_queue: tq_t,
    pub(crate) value: c_int,
    pub(crate) name: *const c_char,
}

// P3-N2 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (`#[repr(C)] #[repr(align(64))]
// pub struct semaphore { lk: spinlock_t, wait_queue: tq_t, value:
// c_int, name: *const c_char }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe against
// `kernel/inc/lock/semaphore_types.h` (size 128, align 64,
// offsets 0/24/72/80).
const _: () = {
    assert!(core::mem::size_of::<RawSemaphore>() == 128, "sem_t size (88 payload bytes padded to 2 cachelines)");
    assert!(core::mem::align_of::<RawSemaphore>() == 64, "sem_t alignment (from embedded spinlock_t)");
    assert!(core::mem::offset_of!(RawSemaphore, lk) == 0, "semaphore.lk offset");
    assert!(core::mem::offset_of!(RawSemaphore, wait_queue) == 24, "semaphore.wait_queue offset");
    assert!(core::mem::offset_of!(RawSemaphore, value) == 72, "semaphore.value offset");
    assert!(core::mem::offset_of!(RawSemaphore, name) == 80, "semaphore.name offset");
};

impl RawSemaphore {
    /// Reinterpret the bindgen `*mut sem_t` as the native mirror.
    ///
    /// SAFETY: layout equivalence is proven at compile time by the
    /// assertions above; the caller provides a valid, non-dangling
    /// `*mut sem_t`.
    #[inline(always)]
    fn as_native(s: *mut sem_t) -> *mut RawSemaphore {
        s as *mut RawSemaphore
    }

    // -----------------------------------------------------------------
    // Helpers. Raw-pointer parameter kept (NOT `&self`): see the module
    // doc's freeze-noalias note (mirrors spinlock.rs's pilot conversion).
    // -----------------------------------------------------------------

    #[inline(always)]
    fn lk_ptr(s: *mut RawSemaphore) -> *mut spinlock_t {
        // SAFETY: caller passes a structurally-valid semaphore.
        u! { addr_of_mut!((*s).lk) as *mut spinlock_t }
    }
    #[inline(always)]
    fn wq_ptr(s: *mut RawSemaphore) -> *mut tq_t {
        // SAFETY: see `Self::lk_ptr`.
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
        Self::value_atomic(s).fetch_add(1, Ordering::SeqCst) + 1
    }
    #[inline(always)]
    fn value_dec(s: *mut RawSemaphore) -> c_int {
        Self::value_atomic(s).fetch_sub(1, Ordering::SeqCst) - 1
    }
    #[inline(always)]
    fn value_get(s: *mut RawSemaphore) -> c_int {
        Self::value_atomic(s).load(Ordering::SeqCst)
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
    /// `wait` — the body is the only `u! { ... }` block needed
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
    let status = RawSpinlock::is_holding(RawSemaphore::lk_ptr(sem));
    if status != 0 {
        RawSpinlock::unlock(RawSemaphore::lk_ptr(sem));
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
        RawSpinlock::lock(RawSemaphore::lk_ptr(sem));
    }
}}

impl RawSemaphore {
    // -----------------------------------------------------------------
    // Wake one waiter; mirrors C `__sem_do_post`. Caller holds `sem->lk`.
    // -----------------------------------------------------------------
    fn sem_do_post(s: *mut RawSemaphore) -> c_int {
        let val = Self::value_inc(s);
        if val <= 0 {
            // Null-queue path unreachable; empty valid queue -> `wakeup_one`
            // returns null, matching this default (former `tq_wakeup`).
            let t = TqRef::from_ptr(Self::wq_ptr(s)).map_or(core::ptr::null_mut(), |r| r.wakeup_one(0, 0));
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
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// N-R1 NOTE — signatures KEPT RAW `*mut sem_t` (same finding as
// `completion.rs`): `RawSemaphore` is `#[derive(Copy)]` + no `UnsafeCell`
// (embedded by value + required `Copy`), hence `Freeze`; a `&RawSemaphore`
// parameter would get LLVM `readonly`/`noalias` and the release optimiser
// would hoist the `value`/`done` reads out of the wait loops → hang. The
// sound typed-reference conversion needs the `UnsafeCell` (lock-owns-data)
// work deferred to wave 3d/N-R7. What DID change: the redundant whole-body
// `u!` wrappers are removed from the public entry points.
impl RawSemaphore {
    pub(crate) fn init(s: *mut sem_t, name: *const c_char, value: c_int) -> c_int {
        if s.is_null() { return -(EINVAL as c_int); }
        if value < 0 { return -(EINVAL as c_int); }
        let s = Self::as_native(s);
        let n = if name.is_null() {
            b"unnamed\0".as_ptr() as *const c_char
        } else {
            name
        };
        Self::set_name_value(s, n, value);
        spin_init(Self::lk_ptr(s), b"semaphore spinlock\0".as_ptr() as *const c_char);
        if let Some(r) = TqRef::from_ptr(Self::wq_ptr(s)) {
            r.init(b"semaphore wait queue\0".as_ptr() as *const c_char, Self::lk_ptr(s));
        }
        0
    }

    pub(crate) fn trywait(s: *mut sem_t) -> c_int {
        if s.is_null() { return -(EINVAL as c_int); }
        let s = Self::as_native(s);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(s)).lock();
        if Self::value_get(s) > 0 {
            Self::value_dec(s);
            return 0;
        }
        -(EAGAIN as c_int)
    }

    pub(crate) fn wait(s: *mut sem_t) -> c_int {
        if s.is_null() { return -(EINVAL as c_int); }
        let s = Self::as_native(s);
        let cur = machine::Riscv::current_thread_ptr();

        let _g = KSpinlock::from_bindings(Self::lk_ptr(s)).lock();
        let val = Self::value_dec(s);
        if val < -SEM_VALUE_MAX {
            Self::value_inc(s);
            return -(EOVERFLOW as c_int);
        }
        if val >= 0 { return 0; }
        machine::Riscv::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
        let ret = TqRef::from_ptr(Self::wq_ptr(s)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait(Self::lk_ptr(s), null_mut()));
        if ret != 0 {
            let wake_ret = Self::sem_do_post(s);
            if wake_ret != 0 && wake_ret != -(ENOENT as c_int) {
                Self::printf_post_failed(Self::name_of(s));
            }
        }
        ret
    }

    pub(crate) fn wait_interruptible(s: *mut sem_t) -> c_int {
        if s.is_null() { return -(EINVAL as c_int); }
        let cur = machine::Riscv::current_thread_ptr();
        loop {
            let ret = Self::trywait(s);
            if ret == 0 { return 0; }
            if ret != -(EAGAIN as c_int) { return ret; }
            if crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
            SchedTimer::sleep_ms(1);
        }
    }

    pub(crate) fn timedwait(s: *mut sem_t, timeout_ms: u64) -> c_int {
        if s.is_null() { return -(EINVAL as c_int); }
        let cur = machine::Riscv::current_thread_ptr();

        if timeout_ms == 0 {
            return if Self::trywait(s) == 0 { 0 } else { -(ETIMEDOUT as c_int) };
        }

        let s = Self::as_native(s);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(s)).lock();
        let val = Self::value_dec(s);
        if val < -SEM_VALUE_MAX {
            Self::value_inc(s);
            return -(EOVERFLOW as c_int);
        }
        if val >= 0 { return 0; }

        let timeout_ticks = machine::Riscv::ms_to_rawticks(timeout_ms);
        let start = machine::Riscv::read_time();
        let mut ctx = SemTimedCtx {
            sem: s,
            // SAFETY: zero-init of `timer_node` matches C `.timer = {0}`.
            timer: u! { core::mem::zeroed() },
            timeout_ms,
            timer_armed: false,
        };

        machine::Riscv::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
        let ret = TqRef::from_ptr(Self::wq_ptr(s)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait_cb(
            Some(sem_timed_sleep_cb),
            Some(sem_timed_wake_cb),
            &mut ctx as *mut _ as *mut c_void,
            null_mut(),
        ));

        if ret != 0 {
            let wake_ret = Self::sem_do_post(s);
            if wake_ret != 0 && wake_ret != -(ENOENT as c_int) {
                // SAFETY: variadic printf.
                u! {
                    crate::kprintln!(
                        "Failed to post semaphore '{}' after timed wait wakeup",
                        crate::printf::Cs(Self::name_of(s)));
                }
            }
            if crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
            if (machine::Riscv::read_time().wrapping_sub(start)) >= timeout_ticks {
                return -(ETIMEDOUT as c_int);
            }
        }
        ret
    }

    pub(crate) fn post(s: *mut sem_t) -> c_int {
        if s.is_null() { return -(EINVAL as c_int); }
        let s = Self::as_native(s);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(s)).lock();
        if Self::value_get(s) == SEM_VALUE_MAX { return -(EOVERFLOW as c_int); }
        let ret = Self::sem_do_post(s);
        if ret == -(ENOENT as c_int) { 0 } else { ret }
    }

    pub(crate) fn getvalue(s: *mut sem_t, value: *mut c_int) -> c_int {
        if s.is_null() || value.is_null() { return -(EINVAL as c_int); }
        let s = Self::as_native(s);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(s)).lock();
        let v = Self::value_get(s);
        // SAFETY: caller-supplied out-pointer.
        u! { *value = v; }
        0
    }
}

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

impl RawSemaphore {
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
        let r = u! { RawSemaphore::init(self.raw, name, value) };
        if r == 0 { Ok(()) } else { Err(RawSemaphore::map_sem_err(r)) }
    }

    /// Non-blocking acquire.
    #[inline]
    pub fn try_wait(self) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { RawSemaphore::trywait(self.raw) };
        if r == 0 { Ok(()) } else { Err(RawSemaphore::map_sem_err(r)) }
    }

    /// Blocking acquire (uninterruptible).
    #[inline]
    pub fn wait(self) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { RawSemaphore::wait(self.raw) };
        if r == 0 { Ok(()) } else { Err(RawSemaphore::map_sem_err(r)) }
    }

    /// Blocking acquire (interruptible).
    #[inline]
    pub fn wait_interruptible(self) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { RawSemaphore::wait_interruptible(self.raw) };
        if r == 0 { Ok(()) } else { Err(RawSemaphore::map_sem_err(r)) }
    }

    /// Timed acquire.
    #[inline]
    pub fn timed_wait(self, timeout_ms: u64) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { RawSemaphore::timedwait(self.raw, timeout_ms) };
        if r == 0 { Ok(()) } else { Err(RawSemaphore::map_sem_err(r)) }
    }

    /// Release one token.
    #[inline]
    pub fn post(self) -> Result<(), SemError> {
        // SAFETY: see `init`.
        let r = u! { RawSemaphore::post(self.raw) };
        if r == 0 { Ok(()) } else { Err(RawSemaphore::map_sem_err(r)) }
    }

    /// Read the current counter value.
    #[inline]
    pub fn value(self) -> Result<c_int, SemError> {
        let mut v: c_int = 0;
        // SAFETY: see `init`; `&mut v` is a valid out-pointer.
        let r = u! { RawSemaphore::getvalue(self.raw, &mut v) };
        if r == 0 { Ok(v) } else { Err(RawSemaphore::map_sem_err(r)) }
    }
}