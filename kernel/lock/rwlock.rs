//! Rust port of `kernel/lock/rwlock.c`.
//!
//! All inline atomic CAS primitives from `rwlock.h` (and the
//! `_Atomic uint64 state` / `_Atomic int w_holder` fields) are reached
//! through the small C trampolines in `kernel/lock/rwlock_shim.c`. This
//! keeps the atomic ABI fully on the C side while the Rust code drives the
//! spin policy and IRQ wrappers.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};

use crate::bindings::rwlock as rwlock_t;

unsafe extern "C" {
    // From kernel/lock/rwlock_shim.c.
    pub safe fn __rwl_try_rlock(rw: *mut rwlock_t) -> bool;
    pub safe fn __rwl_try_wlock(rw: *mut rwlock_t, expedite: bool) -> bool;
    pub safe fn __rwl_state(rw: *mut rwlock_t) -> u64;
    pub safe fn __rwl_w_holding(rw: *mut rwlock_t) -> bool;
    pub safe fn __rwl_state_r_count(state: u64) -> u64;
    pub safe fn __rwl_atomic_sub_reader(rw: *mut rwlock_t);
    pub safe fn __rwl_store_unlocked(rw: *mut rwlock_t);
    pub safe fn __rwl_store_holder_none(rw: *mut rwlock_t);
    pub safe fn __rwl_cpu_relax();
    pub safe fn __rwl_r_time() -> u64;
    pub safe fn __rwl_expedite_threshold() -> u64;
    pub safe fn __rwl_intr_off_save() -> c_int;
    pub safe fn __rwl_intr_restore(state: c_int);

    pub safe fn __rwl_push_off();
    pub safe fn __rwl_pop_off();
}

// ---------------------------------------------------------------------------
// Centralised-unsafe accessor for the `name` field.
// ---------------------------------------------------------------------------

#[inline(always)]
fn set_name(rw: *mut rwlock_t, name: *const c_char) {
    // SAFETY: caller has exclusive access during init.
    unsafe { (*rw).name = name; }
}

// ---------------------------------------------------------------------------
// Safe inner helpers — all logic is here. The `#[no_mangle] pub unsafe
// extern "C" fn` wrappers below are thin trampolines that exist only to
// hand back the C-ABI symbol; they delegate to these functions so that
// intra-module recursion does not need `unsafe { ... }` blocks.
// ---------------------------------------------------------------------------

fn init_inner(rw: *mut rwlock_t, name: *const c_char) {
    if rw.is_null() { return; }
    __rwl_store_unlocked(rw);
    __rwl_store_holder_none(rw);
    let fallback = b"unnamed\0".as_ptr() as *const c_char;
    let n = if name.is_null() { fallback } else { name };
    set_name(rw, n);
}

fn racquire_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    while !__rwl_try_rlock(rw) {
        __rwl_cpu_relax();
    }
}

fn rrelease_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    __rwl_atomic_sub_reader(rw);
}

fn wacquire_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    let start = __rwl_r_time();
    let threshold = __rwl_expedite_threshold();
    let mut expedite = false;
    while !__rwl_try_wlock(rw, expedite) {
        __rwl_cpu_relax();
        if !expedite && __rwl_r_time().wrapping_sub(start) >= threshold {
            expedite = true;
        }
    }
}

fn wacquire_expedited_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    while !__rwl_try_wlock(rw, true) {
        __rwl_cpu_relax();
    }
}

fn graceful_wacquire_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    while !__rwl_try_wlock(rw, false) {
        __rwl_cpu_relax();
    }
}

fn writer_release_inner(rw: *mut rwlock_t) {
    if rw.is_null() { return; }
    __rwl_store_holder_none(rw);
    __rwl_store_unlocked(rw);
}

fn rlock_inner(rw: *mut rwlock_t) {
    __rwl_push_off();
    racquire_inner(rw);
}

fn runlock_inner(rw: *mut rwlock_t) {
    rrelease_inner(rw);
    __rwl_pop_off();
}

fn wlock_inner(rw: *mut rwlock_t) {
    __rwl_push_off();
    wacquire_inner(rw);
}

fn wunlock_inner(rw: *mut rwlock_t) {
    writer_release_inner(rw);
    __rwl_pop_off();
}

// ===========================================================================
// Public C-ABI surface — thin trampolines around the safe inner fns.
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn rwlock_init(rw: *mut rwlock_t, name: *const c_char) {
    init_inner(rw, name);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_racquire(rw: *mut rwlock_t) { racquire_inner(rw); }

#[no_mangle]
pub unsafe extern "C" fn rwlock_rrelease(rw: *mut rwlock_t) { rrelease_inner(rw); }

#[no_mangle]
pub unsafe extern "C" fn rwlock_wacquire(rw: *mut rwlock_t) { wacquire_inner(rw); }

#[no_mangle]
pub unsafe extern "C" fn rwlock_wacquire_expedited(rw: *mut rwlock_t) {
    wacquire_expedited_inner(rw);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_graceful_wacquire(rw: *mut rwlock_t) {
    graceful_wacquire_inner(rw);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_writer_release(rw: *mut rwlock_t) {
    writer_release_inner(rw);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_rlock(rw: *mut rwlock_t) { rlock_inner(rw); }

#[no_mangle]
pub unsafe extern "C" fn rwlock_runlock(rw: *mut rwlock_t) { runlock_inner(rw); }

#[no_mangle]
pub unsafe extern "C" fn rwlock_wlock(rw: *mut rwlock_t) { wlock_inner(rw); }

#[no_mangle]
pub unsafe extern "C" fn rwlock_wlock_expedited(rw: *mut rwlock_t) {
    __rwl_push_off();
    wacquire_expedited_inner(rw);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_graceful_wlock(rw: *mut rwlock_t) {
    __rwl_push_off();
    graceful_wacquire_inner(rw);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_wunlock(rw: *mut rwlock_t) { wunlock_inner(rw); }

// ---------------------------------------------------------------------------
// irqsave / irqrestore wrappers
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn rwlock_rlock_irqsave(rw: *mut rwlock_t) -> c_int {
    let intena = __rwl_intr_off_save();
    racquire_inner(rw);
    intena
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_runlock_irqrestore(rw: *mut rwlock_t, intena: c_int) {
    rrelease_inner(rw);
    __rwl_intr_restore(intena);
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_wlock_irqsave(rw: *mut rwlock_t) -> c_int {
    let intena = __rwl_intr_off_save();
    wacquire_inner(rw);
    intena
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_wlock_expedited_irqsave(rw: *mut rwlock_t) -> c_int {
    let intena = __rwl_intr_off_save();
    wacquire_expedited_inner(rw);
    intena
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_graceful_wlock_irqsave(rw: *mut rwlock_t) -> c_int {
    let intena = __rwl_intr_off_save();
    graceful_wacquire_inner(rw);
    intena
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_wunlock_irqrestore(rw: *mut rwlock_t, intena: c_int) {
    writer_release_inner(rw);
    __rwl_intr_restore(intena);
}

// ---------------------------------------------------------------------------
// Sleep / wakeup callbacks for rwlock-protected thread-queue waits
// ---------------------------------------------------------------------------

const RW_CB_STATUS_READER: c_int = 1;
const RW_CB_STATUS_WRITER: c_int = 2;

#[no_mangle]
pub unsafe extern "C" fn rwlock_r_sleep_cb(data: *mut c_void) -> c_int {
    if data.is_null() { return 0; }
    let rw = data as *mut rwlock_t;
    runlock_inner(rw);
    let mut status = RW_CB_STATUS_READER;
    if __rwl_w_holding(rw) {
        // The reader may also hold the write lock (write→read recursion).
        wunlock_inner(rw);
        status |= RW_CB_STATUS_WRITER;
    }
    status
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_r_wake_cb(data: *mut c_void, status: c_int) {
    if data.is_null() { return; }
    let rw = data as *mut rwlock_t;
    if (status & RW_CB_STATUS_WRITER) != 0 {
        wlock_inner(rw);
    }
    if (status & RW_CB_STATUS_READER) != 0 {
        rlock_inner(rw);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_w_sleep_cb(data: *mut c_void) -> c_int {
    if !data.is_null() {
        let rw = data as *mut rwlock_t;
        wunlock_inner(rw);
    }
    RW_CB_STATUS_WRITER
}

#[no_mangle]
pub unsafe extern "C" fn rwlock_w_wake_cb(data: *mut c_void, status: c_int) {
    if data.is_null() { return; }
    if (status & RW_CB_STATUS_WRITER) != 0 {
        let rw = data as *mut rwlock_t;
        wlock_inner(rw);
    }
}

// Reference unused helper to keep symbols alive in release builds where
// the public API may not exercise every shim path.
#[allow(dead_code)]
fn _keep_alive() -> u64 {
    __rwl_state_r_count(0)
}

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
