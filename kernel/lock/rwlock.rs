//! Rust port of `kernel/lock/rwlock.c`.
//!
//! The state-word CAS primitives that used to live as `static inline`
//! functions in the C header `kernel/inc/lock/rwlock.h` — reached via the
//! non-inline trampolines in the now-deleted `kernel/lock/rwlock_shim.c` —
//! are implemented natively here on top of `core::sync::atomic`. The C
//! header still defines the `struct rwlock` layout (`rwlock_types.h`) for
//! the handful of C call sites that only need the type, but no C code
//! implements any of the locking logic anymore.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

use crate::bindings::rwlock as rwlock_t;
use crate::machine;

// ---------------------------------------------------------------------------
// State-word layout constants — mirror `kernel/inc/lock/rwlock.h` (the
// `RWLOCK_STATE_*` macros) byte-for-byte. See that header for the full
// bit-layout narrative.
// ---------------------------------------------------------------------------

const RWLOCK_STATE_UNLOCKED: u64 = 0;
const RWLOCK_STATE_WRITER_WAITING: u64 = 1 << 8;
const RWLOCK_STATE_WRITER_HOLDING: u64 = (1 << 8) - 1;
const RWLOCK_STATE_WRITER_MASK: u64 = RWLOCK_STATE_WRITER_WAITING | RWLOCK_STATE_WRITER_HOLDING;
const RWLOCK_STATE_READER_BIAS_SHIFT: u32 = 9;
const RWLOCK_STATE_READER_BIAS: u64 = 1 << RWLOCK_STATE_READER_BIAS_SHIFT;
const RWLOCK_NONE_HOLDER: c_int = -1;

/// Writer-starvation-prevention threshold (mirrors `RWLOCK_EXPEDITE_THRESHOLD`
/// = `TICK_MS << 2`, i.e. 4ms of raw ticks).
#[inline(always)]
fn expedite_threshold() -> u64 {
    machine::tick_ms() << 2
}

// ---------------------------------------------------------------------------
// Centralised-unsafe field accessors
// ---------------------------------------------------------------------------

#[inline(always)]
fn state_atomic<'a>(rw: *mut rwlock_t) -> &'a AtomicU64 {
    // SAFETY: `state` is an `_Atomic uint64` (8-byte aligned) field of a
    // live `struct rwlock`; every caller in this module passes a valid,
    // non-dangling pointer supplied by the kernel.
    unsafe { &*(addr_of_mut!((*rw).state) as *const AtomicU64) }
}

#[inline(always)]
fn holder_atomic<'a>(rw: *mut rwlock_t) -> &'a AtomicI32 {
    // SAFETY: `w_holder` is an `_Atomic int` field of a live `struct rwlock`.
    unsafe { &*(addr_of_mut!((*rw).w_holder) as *const AtomicI32) }
}

#[inline(always)]
fn set_name(rw: *mut rwlock_t, name: *const c_char) {
    // SAFETY: caller has exclusive access during init.
    unsafe { (*rw).name = name; }
}

// ---------------------------------------------------------------------------
// State-machine predicates and CAS primitives — native Rust replacement for
// the `static inline` functions previously in `kernel/inc/lock/rwlock.h`.
// ---------------------------------------------------------------------------

/// Mirrors `rwlock_can_rlock`.
fn can_rlock(rw: *mut rwlock_t, state: u64) -> bool {
    if state & RWLOCK_STATE_WRITER_MASK != 0 {
        w_holding(rw)
    } else {
        true // No writers — readers can acquire.
    }
}

/// Mirrors `rwlock_try_rlock`: single CAS attempt, reader count += bias.
fn try_rlock(rw: *mut rwlock_t) -> bool {
    let a = state_atomic(rw);
    let mut val = a.load(Ordering::Acquire);
    while can_rlock(rw, val) {
        match a.compare_exchange(
            val,
            val + RWLOCK_STATE_READER_BIAS,
            Ordering::SeqCst,
            Ordering::SeqCst,
        ) {
            Ok(_) => return true,
            Err(cur) => val = cur,
        }
    }
    false
}

/// Mirrors `rwlock_can_wlock`.
fn can_wlock(state: u64, expedite: bool) -> bool {
    if (state >> RWLOCK_STATE_READER_BIAS_SHIFT) > 0 {
        return false; // Readers present — can't acquire write lock.
    }
    if state & RWLOCK_STATE_WRITER_HOLDING != 0 {
        return false; // Another writer holds the lock.
    }
    if state & RWLOCK_STATE_WRITER_WAITING != 0 && !expedite {
        return false; // Another writer is waiting and we're not expediting.
    }
    true
}

/// Mirrors `rwlock_try_wlock`, folding in the `__rwlock_expedite_hook`
/// CAS-failure hook that sets the WRITER_WAITING soft-priority bit.
fn try_wlock(rw: *mut rwlock_t, expedite: bool) -> bool {
    let a = state_atomic(rw);
    let mut val = a.load(Ordering::Acquire);
    loop {
        if !can_wlock(val, expedite) {
            return false;
        }
        match a.compare_exchange(
            val,
            RWLOCK_STATE_WRITER_HOLDING,
            Ordering::SeqCst,
            Ordering::SeqCst,
        ) {
            Ok(_) => {
                holder_atomic(rw).store(machine::cpuid(), Ordering::Release);
                return true;
            }
            Err(cur) => {
                val = cur;
                if expedite && val & RWLOCK_STATE_WRITER_WAITING == 0 {
                    a.fetch_or(RWLOCK_STATE_WRITER_WAITING, Ordering::SeqCst);
                }
            }
        }
    }
}

/// Mirrors `rwlock_can_update`.
fn can_update(state: u64) -> bool {
    if state & RWLOCK_STATE_WRITER_HOLDING != 0 {
        return false; // A writer holds the lock (includes write→read→update).
    }
    let r_count = state >> RWLOCK_STATE_READER_BIAS_SHIFT;
    if r_count != 1 || state & RWLOCK_STATE_WRITER_WAITING != 0 {
        return false; // Not the sole reader, or another writer is waiting.
    }
    true
}

/// Mirrors `rwlock_try_update`: non-blocking read → write upgrade.
fn try_update(rw: *mut rwlock_t) -> bool {
    let a = state_atomic(rw);
    let mut val = a.load(Ordering::Acquire);
    while can_update(val) {
        match a.compare_exchange(
            val,
            RWLOCK_STATE_WRITER_HOLDING,
            Ordering::SeqCst,
            Ordering::SeqCst,
        ) {
            Ok(_) => {
                holder_atomic(rw).store(machine::cpuid(), Ordering::Release);
                return true;
            }
            Err(cur) => val = cur,
        }
    }
    false
}

/// Mirrors `RWLOCK_W_HOLDING`: true if the *calling* CPU holds the writer.
/// Brackets the read in push_off/pop_off so `cpuid()` cannot change out
/// from under the comparison.
fn w_holding(rw: *mut rwlock_t) -> bool {
    machine::push_off();
    let ret = machine::cpuid() == holder_atomic(rw).load(Ordering::Acquire);
    machine::pop_off();
    ret
}

/// Mirrors `atomic_sub(&rw->state, READER_BIAS)`.
fn atomic_sub_reader(rw: *mut rwlock_t) {
    state_atomic(rw).fetch_sub(RWLOCK_STATE_READER_BIAS, Ordering::SeqCst);
}

fn store_unlocked(rw: *mut rwlock_t) {
    state_atomic(rw).store(RWLOCK_STATE_UNLOCKED, Ordering::Release);
}

fn store_holder_none(rw: *mut rwlock_t) {
    holder_atomic(rw).store(RWLOCK_NONE_HOLDER, Ordering::Release);
}

// ---------------------------------------------------------------------------
// Safe inner helpers — spin policy and IRQ wrappers. The `#[no_mangle] pub
// unsafe extern "C" fn` wrappers below are thin trampolines that exist only
// to hand back the C-ABI symbol; they delegate to these functions so that
// intra-module recursion does not need `unsafe { ... }` blocks.
// ---------------------------------------------------------------------------

fn init_inner(rw: *mut rwlock_t, name: *const c_char) {
    if rw.is_null() { return; }
    store_unlocked(rw);
    store_holder_none(rw);
    let fallback = b"unnamed\0".as_ptr() as *const c_char;
    let n = if name.is_null() { fallback } else { name };
    set_name(rw, n);
}

fn racquire_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    while !try_rlock(rw) {
        machine::cpu_relax();
    }
}

fn rrelease_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    atomic_sub_reader(rw);
}

fn wacquire_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    let start = machine::read_time();
    let threshold = expedite_threshold();
    let mut expedite = false;
    while !try_wlock(rw, expedite) {
        machine::cpu_relax();
        if !expedite && machine::read_time().wrapping_sub(start) >= threshold {
            expedite = true;
        }
    }
}

fn wacquire_expedited_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    while !try_wlock(rw, true) {
        machine::cpu_relax();
    }
}

fn graceful_wacquire_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    while !try_wlock(rw, false) {
        machine::cpu_relax();
    }
}

fn writer_release_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    store_holder_none(rw);
    store_unlocked(rw);
}

fn rlock_inner(rw: *mut rwlock_t) {
    machine::push_off();
    racquire_inner(rw);
}

fn runlock_inner(rw: *mut rwlock_t) {
    rrelease_inner(rw);
    machine::pop_off();
}

fn wlock_inner(rw: *mut rwlock_t) {
    machine::push_off();
    wacquire_inner(rw);
}

fn wunlock_inner(rw: *mut rwlock_t) {
    writer_release_inner(rw);
    machine::pop_off();
}

// ===========================================================================
// Public C-ABI surface — thin trampolines around the safe inner fns.
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn rwlock_init(rw: *mut rwlock_t, name: *const c_char) {
    init_inner(rw, name);
}

pub(crate) unsafe fn rwlock_racquire(rw: *mut rwlock_t) { racquire_inner(rw); }

pub(crate) unsafe fn rwlock_rrelease(rw: *mut rwlock_t) { rrelease_inner(rw); }

pub(crate) unsafe fn rwlock_wacquire(rw: *mut rwlock_t) { wacquire_inner(rw); }

pub(crate) unsafe fn rwlock_wacquire_expedited(rw: *mut rwlock_t) {
    wacquire_expedited_inner(rw);
}

pub(crate) unsafe fn rwlock_graceful_wacquire(rw: *mut rwlock_t) {
    graceful_wacquire_inner(rw);
}

pub(crate) unsafe fn rwlock_writer_release(rw: *mut rwlock_t) {
    writer_release_inner(rw);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_rlock(rw: *mut rwlock_t) { rlock_inner(rw); }

#[no_mangle]
pub unsafe extern "C" fn rwlock_runlock(rw: *mut rwlock_t) { runlock_inner(rw); }

#[no_mangle]
pub unsafe extern "C" fn rwlock_wlock(rw: *mut rwlock_t) { wlock_inner(rw); }

pub(crate) unsafe fn rwlock_wlock_expedited(rw: *mut rwlock_t) {
    machine::push_off();
    wacquire_expedited_inner(rw);
}

pub(crate) unsafe fn rwlock_graceful_wlock(rw: *mut rwlock_t) {
    machine::push_off();
    graceful_wacquire_inner(rw);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_wunlock(rw: *mut rwlock_t) { wunlock_inner(rw); }

// ---------------------------------------------------------------------------
// irqsave / irqrestore wrappers
// ---------------------------------------------------------------------------

pub(crate) unsafe fn rwlock_rlock_irqsave(rw: *mut rwlock_t) -> c_int {
    let intena = machine::intr_off_save();
    racquire_inner(rw);
    intena
}

pub(crate) unsafe fn rwlock_runlock_irqrestore(rw: *mut rwlock_t, intena: c_int) {
    rrelease_inner(rw);
    machine::intr_restore(intena);
}

pub(crate) unsafe fn rwlock_wlock_irqsave(rw: *mut rwlock_t) -> c_int {
    let intena = machine::intr_off_save();
    wacquire_inner(rw);
    intena
}

pub(crate) unsafe fn rwlock_wlock_expedited_irqsave(rw: *mut rwlock_t) -> c_int {
    let intena = machine::intr_off_save();
    wacquire_expedited_inner(rw);
    intena
}

pub(crate) unsafe fn rwlock_graceful_wlock_irqsave(rw: *mut rwlock_t) -> c_int {
    let intena = machine::intr_off_save();
    graceful_wacquire_inner(rw);
    intena
}

pub(crate) unsafe fn rwlock_wunlock_irqrestore(rw: *mut rwlock_t, intena: c_int) {
    writer_release_inner(rw);
    machine::intr_restore(intena);
}

// ---------------------------------------------------------------------------
// Sleep / wakeup callbacks for rwlock-protected thread-queue waits
// ---------------------------------------------------------------------------

const RW_CB_STATUS_READER: c_int = 1;
const RW_CB_STATUS_WRITER: c_int = 2;

// Kept `extern "C"` (not demoted to a plain Rust fn like their neighbors):
// `mm/pcache.rs` (out of this file, in-scope but a separate module) passes
// these *by value* as `Option<unsafe extern "C" fn(...)>` callback
// arguments to `tq_wait_cb` — same fn-pointer-type-coercion reasoning as
// `lock::spinlock::spin_sleep_cb`/`spin_wake_cb`'s identical note.
// `#[no_mangle]` is still dropped: nothing resolves these by C symbol
// name.
pub(crate) unsafe extern "C" fn rwlock_r_sleep_cb(data: *mut c_void) -> c_int {
    if data.is_null() { return 0; }
    let rw = data as *mut rwlock_t;
    runlock_inner(rw);
    let mut status = RW_CB_STATUS_READER;
    if w_holding(rw) {
        // The reader may also hold the write lock (write→read recursion).
        wunlock_inner(rw);
        status |= RW_CB_STATUS_WRITER;
    }
    status
}

pub(crate) unsafe extern "C" fn rwlock_r_wake_cb(data: *mut c_void, status: c_int) {
    if data.is_null() { return; }
    let rw = data as *mut rwlock_t;
    if (status & RW_CB_STATUS_WRITER) != 0 {
        wlock_inner(rw);
    }
    if (status & RW_CB_STATUS_READER) != 0 {
        rlock_inner(rw);
    }
}

pub(crate) unsafe fn rwlock_w_sleep_cb(data: *mut c_void) -> c_int {
    if !data.is_null() {
        let rw = data as *mut rwlock_t;
        wunlock_inner(rw);
    }
    RW_CB_STATUS_WRITER
}

pub(crate) unsafe fn rwlock_w_wake_cb(data: *mut c_void, status: c_int) {
    if data.is_null() { return; }
    if (status & RW_CB_STATUS_WRITER) != 0 {
        let rw = data as *mut rwlock_t;
        wlock_inner(rw);
    }
}

// ---------------------------------------------------------------------------
// Extra C-ABI symbols kept for non-C callers.
//
// `kernel/proc/proc_shims.rs` declares its own `unsafe extern "C" { fn
// __rwl_try_update(...); fn __rwl_w_holding(...); }` block and calls these
// two symbols directly on `pid_lock` (a plain `struct rwlock`) rather than
// going through the higher-level `rwlock_*` API. They used to be defined in
// `kernel/lock/rwlock_shim.c`; now they are thin dispatchers onto the native
// primitives above so that call site keeps working unmodified.
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn __rwl_try_update(rw: *mut rwlock_t) -> bool { try_update(rw) }

#[no_mangle]
pub unsafe extern "C" fn __rwl_w_holding(rw: *mut rwlock_t) -> bool { w_holding(rw) }

// ===========================================================================
// Rust-native typed handle
// ===========================================================================
//
// `KRwLock` wraps the spin-based reader/writer lock. Acquisition
// returns a RAII guard whose `Drop` calls the matching `runlock` /
// `wunlock`. Three writer flavours mirror the C ABI: the default
// fair acquire (`write`), the priority-boosted `write_expedited`,
// and the cooperative `write_graceful`.

use core::marker::PhantomData;

/// Typed handle to a kernel `rwlock_t`.
#[derive(Clone, Copy)]
pub struct KRwLock {
    raw: *mut rwlock_t,
    _ns: PhantomData<*const ()>,
}

impl KRwLock {
    #[inline(always)]
    pub fn from_ptr(raw: *mut rwlock_t) -> Self {
        Self { raw, _ns: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut rwlock_t { self.raw }

    #[inline]
    pub fn init(self, name: *const c_char) {
        // SAFETY: handle wraps live storage.
        unsafe { rwlock_init(self.raw, name); }
    }

    /// Acquire shared (reader).
    #[inline]
    pub fn read(self) -> KRwLockReadGuard {
        // SAFETY: see `init`.
        unsafe { rwlock_rlock(self.raw); }
        KRwLockReadGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Acquire exclusive (writer, fair variant).
    #[inline]
    pub fn write(self) -> KRwLockWriteGuard {
        // SAFETY: see `init`.
        unsafe { rwlock_wlock(self.raw); }
        KRwLockWriteGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Acquire exclusive with priority-boost (expedited).
    #[inline]
    pub fn write_expedited(self) -> KRwLockWriteGuard {
        // SAFETY: see `init`.
        unsafe { rwlock_wlock_expedited(self.raw); }
        KRwLockWriteGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Acquire exclusive cooperatively (graceful).
    #[inline]
    pub fn write_graceful(self) -> KRwLockWriteGuard {
        // SAFETY: see `init`.
        unsafe { rwlock_graceful_wlock(self.raw); }
        KRwLockWriteGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Acquire shared, saving the interrupt-enable flag. The guard's
    /// `Drop` restores it via `rwlock_runlock_irqrestore`.
    #[inline]
    pub fn read_irqsave(self) -> KRwLockReadIrqGuard {
        // SAFETY: see `init`.
        let intena = unsafe { rwlock_rlock_irqsave(self.raw) };
        KRwLockReadIrqGuard { raw: self.raw, intena, _ns: PhantomData }
    }
}

/// Reader guard for [`KRwLock`].
#[must_use = "rwlock read lock is released when the guard is dropped"]
pub struct KRwLockReadGuard {
    raw: *mut rwlock_t,
    _ns: PhantomData<*const ()>,
}
impl Drop for KRwLockReadGuard {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: guard tracks a successful acquire.
        unsafe { rwlock_runlock(self.raw); }
    }
}

/// Writer guard for [`KRwLock`].
#[must_use = "rwlock write lock is released when the guard is dropped"]
pub struct KRwLockWriteGuard {
    raw: *mut rwlock_t,
    _ns: PhantomData<*const ()>,
}
impl Drop for KRwLockWriteGuard {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: guard tracks a successful acquire.
        unsafe { rwlock_wunlock(self.raw); }
    }
}

/// Reader guard that restores the saved IE flag on drop.
#[must_use = "rwlock read lock is released when the guard is dropped"]
pub struct KRwLockReadIrqGuard {
    raw: *mut rwlock_t,
    intena: c_int,
    _ns: PhantomData<*const ()>,
}
impl Drop for KRwLockReadIrqGuard {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: guard tracks a successful acquire.
        unsafe { rwlock_runlock_irqrestore(self.raw, self.intena); }
    }
}
