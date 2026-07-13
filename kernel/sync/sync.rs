//! Idiomatic Rust front-end for the kernel locking primitives.
//!
//! The C kernel exposes locks as `*mut spinlock_t` plus the
//! `spin_lock` / `spin_unlock` pair. Every mm submodule that touches a
//! lock today carries the same boilerplate:
//!
//! ```ignore
//! ffi::spin_lock(p.lock_ptr());
//! // ... critical section ...
//! ffi::spin_unlock(p.lock_ptr());
//! ```
//!
//! That pattern is error-prone (mismatched lock/unlock, missing
//! unlock on early return / panic, hand-rolled "two-lock order")
//! and visually noisy. This module wraps the same C ABI in a thin
//! RAII handle so callers can write:
//!
//! ```ignore
//! let _g = KSpinlock::from_ptr(p.lock_ptr()).lock();
//! // ... critical section, `_g` drops at scope end → spin_unlock ...
//! ```
//!
//! Properties:
//!   * Zero-cost — every method is `#[inline]` and lowers to the
//!     same `spin_lock` / `spin_unlock` calls hand-written today.
//!   * Drop-based release — early `return`, `?`, or panic-unwind
//!     paths cannot leak the lock.
//!   * `lock_two()` enforces address-order acquisition for two
//!     locks, replacing the hand-rolled `lock_pair` / `unlock_pair`
//!     helpers and the comment-only "always acquire low → high" rule.
//!
//! Soundness: the underlying lock storage is provided and pinned by
//! the C kernel; this wrapper only manipulates the raw pointer and
//! delegates to the existing `spin_lock` / `spin_unlock` C entry
//! points (themselves now defined in `kernel/lock/spinlock.rs`).
//! `KSpinlock` is `Copy + !Send + !Sync` deliberately: it is just a
//! handle, not an owner — copying it is fine, sending the *handle*
//! across harts is not (the lock itself is shared by design, but
//! the typed handle pretends to be CPU-local for ergonomics).

#![allow(dead_code)]

use core::marker::PhantomData;

use crate::mm::cffi::{raw, Spinlock};

// ===========================================================================
// Handle
// ===========================================================================

/// Thin typed handle to a kernel `spinlock_t`.
///
/// Construct via [`KSpinlock::from_ptr`]; the pointer must reference
/// an already-initialised `spinlock_t` whose storage outlives the
/// handle (true for every embedded mm lock, which is stored inside a
/// `static` or in a kernel-pinned page).
#[derive(Clone, Copy)]
pub struct KSpinlock {
    raw: *mut Spinlock,
    // Pinned to the calling hart for ergonomic safety; the lock itself
    // is shared by design but the *handle* should not be smuggled
    // across threads.
    _not_send_sync: PhantomData<*const ()>,
}

impl KSpinlock {
    /// Wrap a pre-initialised `spinlock_t` pointer.
    #[inline(always)]
    pub fn from_ptr(raw: *mut Spinlock) -> Self {
        Self { raw, _not_send_sync: PhantomData }
    }

    /// Wrap a `*mut spinlock_t` from `crate::bindings`. The bindgen
    /// `spinlock_t` and the hand-rolled `cffi::Spinlock` denote the
    /// same C type, so the cast is layout-compatible.
    #[inline(always)]
    pub fn from_bindings(p: *mut crate::bindings::spinlock_t) -> Self {
        Self::from_ptr(p as *mut Spinlock)
    }

    /// Initialise the underlying lock storage with `name`.
    ///
    /// Must be called exactly once per lock instance, before any
    /// thread can race to acquire it. Mirrors C `spin_init`.
    #[inline(always)]
    pub fn init(self, name: *const core::ffi::c_char) {
        raw::spin_init(self.raw, name);
    }

    /// Acquire the lock; returns a RAII guard that releases on drop.
    #[inline(always)]
    pub fn lock(self) -> KSpinGuard {
        raw::spin_lock(self.raw);
        KSpinGuard { raw: self.raw, _not_send_sync: PhantomData }
    }

    /// `true` if the current hart holds this lock.
    #[inline(always)]
    pub fn holding(self) -> bool {
        raw::spin_holding(self.raw) != 0
    }

    /// Borrow the raw pointer (escape hatch for the few call sites
    /// that still need to pass it to a C API).
    #[inline(always)]
    pub fn as_ptr(self) -> *mut Spinlock {
        self.raw
    }

    /// Acquire `self`, run `f`, release. Equivalent to `let _g =
    /// self.lock(); f()` but reads slightly nicer at single-line
    /// call sites.
    #[inline(always)]
    pub fn with<R>(self, f: impl FnOnce() -> R) -> R {
        let _g = self.lock();
        f()
    }

    /// Acquire two locks in address order, returning a paired guard
    /// that releases them in reverse acquisition order on drop. If
    /// `a` and `b` alias, the lock is acquired exactly once.
    ///
    /// This replaces the hand-rolled `lock_pair` / `unlock_pair`
    /// helpers and the "always go low → high" comment-only rule.
    #[inline]
    pub fn lock_two(a: KSpinlock, b: KSpinlock) -> KSpinPairGuard {
        let pa = a.raw as usize;
        let pb = b.raw as usize;
        if pa == pb {
            raw::spin_lock(a.raw);
            KSpinPairGuard {
                first: a.raw,
                second: core::ptr::null_mut(),
                _not_send_sync: PhantomData,
            }
        } else if pa < pb {
            raw::spin_lock(a.raw);
            raw::spin_lock(b.raw);
            KSpinPairGuard {
                first: a.raw,
                second: b.raw,
                _not_send_sync: PhantomData,
            }
        } else {
            raw::spin_lock(b.raw);
            raw::spin_lock(a.raw);
            KSpinPairGuard {
                first: b.raw,
                second: a.raw,
                _not_send_sync: PhantomData,
            }
        }
    }
}

// ===========================================================================
// Single-lock guard
// ===========================================================================

/// RAII guard returned by [`KSpinlock::lock`].
///
/// Releases the underlying spinlock when dropped. Holding a guard
/// implies the current hart is in a critical section: `push_off`
/// nesting is already incremented by `spin_lock` and is decremented
/// by `spin_unlock` in `Drop`.
#[must_use = "spinlock is released immediately if the guard is dropped"]
pub struct KSpinGuard {
    raw: *mut Spinlock,
    _not_send_sync: PhantomData<*const ()>,
}

impl Drop for KSpinGuard {
    #[inline(always)]
    fn drop(&mut self) {
        raw::spin_unlock(self.raw);
    }
}

// ===========================================================================
// Two-lock guard
// ===========================================================================

/// RAII guard returned by [`KSpinlock::lock_two`].
///
/// Releases in reverse acquisition order on drop (the higher-address
/// lock first, then the lower). When the two input handles aliased
/// the same lock, the lock is held only once and released once.
#[must_use = "both spinlocks are released immediately if the guard is dropped"]
pub struct KSpinPairGuard {
    /// Lock acquired first (lower address). Released second.
    first: *mut Spinlock,
    /// Lock acquired second (higher address). Released first.
    /// `null_mut()` when the pair collapsed to a single lock.
    second: *mut Spinlock,
    _not_send_sync: PhantomData<*const ()>,
}

impl Drop for KSpinPairGuard {
    #[inline(always)]
    fn drop(&mut self) {
        if !self.second.is_null() {
            raw::spin_unlock(self.second);
        }
        raw::spin_unlock(self.first);
    }
}

// ===========================================================================
// Sleep-lock RAII facade
// ===========================================================================
//
// The C kernel exposes its sleep-style locks (mutex / rwsem /
// completion / semaphore / rwlock) through the symbols defined in the
// `kernel/lock/*.rs` files. Those symbols keep the historical C ABI
// (`#[no_mangle] pub unsafe extern "C" fn …`) for the rest of the
// kernel C tree.
//
// The handles below give Rust callers an idiomatic façade — typed
// `*mut` wrappers whose `lock` / `read` / `write` / `acquire` methods
// return RAII guards that release the lock when dropped. Pattern is
// uniform with `KSpinlock` above:
//
//   ```ignore
//   let m = KMutex::from_ptr(&mut my_mutex);
//   {
//       let _g = m.lock();        // mutex_lock
//       // critical section ...
//   }                             // _g drop → mutex_unlock
//   ```
//
// No new `unsafe` blocks are introduced — every C entry point is
// already declared `pub safe fn …` in the appropriate
// `unsafe extern "C" { … }` block below, so the call sites are
// plain safe Rust.

use core::ffi::{c_char, c_int};

use crate::bindings::{completion_t, mutex_t, rwlock as rwlock_t, rwsem_t, semaphore as sem_t};

unsafe extern "C" {
    // mutex.rs exports
    pub safe fn mutex_init(m: *mut mutex_t, name: *mut c_char);
    pub safe fn mutex_lock(m: *mut mutex_t);
    pub safe fn mutex_trylock(m: *mut mutex_t) -> c_int;
    pub safe fn mutex_unlock(m: *mut mutex_t);
    pub safe fn mutex_lock_interruptible(m: *mut mutex_t) -> c_int;
    pub safe fn mutex_lock_timed(m: *mut mutex_t, timeout_ms: u64) -> c_int;
    pub safe fn holding_mutex(m: *mut mutex_t) -> c_int;

    // rwsem.rs exports
    pub safe fn rwsem_init(l: *mut rwsem_t, flags: u64, name: *const c_char) -> c_int;
    pub safe fn rwsem_acquire_read(l: *mut rwsem_t) -> c_int;
    pub safe fn rwsem_try_acquire_read(l: *mut rwsem_t) -> c_int;
    pub safe fn rwsem_acquire_read_interruptible(l: *mut rwsem_t) -> c_int;
    pub safe fn rwsem_acquire_read_timed(l: *mut rwsem_t, timeout_ms: u64) -> c_int;
    pub safe fn rwsem_acquire_write(l: *mut rwsem_t) -> c_int;
    pub safe fn rwsem_try_acquire_write(l: *mut rwsem_t) -> c_int;
    pub safe fn rwsem_acquire_write_interruptible(l: *mut rwsem_t) -> c_int;
    pub safe fn rwsem_acquire_write_timed(l: *mut rwsem_t, timeout_ms: u64) -> c_int;
    pub safe fn rwsem_release(l: *mut rwsem_t);
    pub safe fn rwsem_is_write_holding(l: *mut rwsem_t) -> bool;

    // completion.rs exports
    pub safe fn completion_init(c: *mut completion_t);
    pub safe fn completion_reinit(c: *mut completion_t);
    pub safe fn try_wait_for_completion(c: *mut completion_t) -> bool;
    pub safe fn wait_for_completion(c: *mut completion_t);
    pub safe fn wait_for_completion_interruptible(c: *mut completion_t) -> c_int;
    pub safe fn wait_for_completion_timed(c: *mut completion_t, timeout_ms: u64) -> c_int;
    pub safe fn complete(c: *mut completion_t);
    pub safe fn complete_all(c: *mut completion_t);
    pub safe fn completion_done(c: *mut completion_t) -> bool;

    // semaphore.rs exports
    pub safe fn sem_init(s: *mut sem_t, name: *const c_char, value: c_int) -> c_int;
    pub safe fn sem_trywait(s: *mut sem_t) -> c_int;
    pub safe fn sem_wait(s: *mut sem_t) -> c_int;
    pub safe fn sem_wait_interruptible(s: *mut sem_t) -> c_int;
    pub safe fn sem_timedwait(s: *mut sem_t, timeout_ms: u64) -> c_int;
    pub safe fn sem_post(s: *mut sem_t) -> c_int;
    pub safe fn sem_getvalue(s: *mut sem_t, value: *mut c_int) -> c_int;

    // rwlock.rs exports
    pub safe fn rwlock_init(rw: *mut rwlock_t, name: *const c_char);
    pub safe fn rwlock_rlock(rw: *mut rwlock_t);
    pub safe fn rwlock_runlock(rw: *mut rwlock_t);
    pub safe fn rwlock_wlock(rw: *mut rwlock_t);
    pub safe fn rwlock_wlock_expedited(rw: *mut rwlock_t);
    pub safe fn rwlock_graceful_wlock(rw: *mut rwlock_t);
    pub safe fn rwlock_wunlock(rw: *mut rwlock_t);
    pub safe fn rwlock_rlock_irqsave(rw: *mut rwlock_t) -> c_int;
    pub safe fn rwlock_runlock_irqrestore(rw: *mut rwlock_t, intena: c_int);
    pub safe fn rwlock_wlock_irqsave(rw: *mut rwlock_t) -> c_int;
    pub safe fn rwlock_wunlock_irqrestore(rw: *mut rwlock_t, intena: c_int);
}

// ---------------------------------------------------------------------------
// KMutex — typed handle + RAII guard around `mutex_t`.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
pub struct KMutex {
    raw: *mut mutex_t,
    _not_send_sync: PhantomData<*const ()>,
}

impl KMutex {
    #[inline(always)]
    pub fn from_ptr(raw: *mut mutex_t) -> Self {
        Self { raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut mutex_t { self.raw }
    #[inline(always)]
    pub fn init(self, name: *mut c_char) { mutex_init(self.raw, name); }
    #[inline(always)]
    pub fn holding(self) -> bool { holding_mutex(self.raw) != 0 }

    /// Block until the mutex is acquired; returns a guard that
    /// releases on drop.
    #[inline(always)]
    pub fn lock(self) -> KMutexGuard {
        mutex_lock(self.raw);
        KMutexGuard { raw: self.raw, _not_send_sync: PhantomData }
    }

    /// Try once. `Some(guard)` if acquired, `None` otherwise.
    #[inline(always)]
    pub fn try_lock(self) -> Option<KMutexGuard> {
        if mutex_trylock(self.raw) != 0 {
            Some(KMutexGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { None }
    }

    /// Interruptible acquire. `Ok(guard)` on success, `Err(errno)` on
    /// signal / null pointer.
    #[inline(always)]
    pub fn lock_interruptible(self) -> Result<KMutexGuard, c_int> {
        let r = mutex_lock_interruptible(self.raw);
        if r == 0 {
            Ok(KMutexGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { Err(r) }
    }

    /// Acquire with a timeout in milliseconds.
    #[inline(always)]
    pub fn lock_timed(self, timeout_ms: u64) -> Result<KMutexGuard, c_int> {
        let r = mutex_lock_timed(self.raw, timeout_ms);
        if r == 0 {
            Ok(KMutexGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { Err(r) }
    }

    /// Run `f` under the mutex and return its result.
    #[inline]
    pub fn with<R>(self, f: impl FnOnce() -> R) -> R {
        let _g = self.lock();
        f()
    }
}

#[must_use = "mutex is released when the guard is dropped"]
pub struct KMutexGuard {
    raw: *mut mutex_t,
    _not_send_sync: PhantomData<*const ()>,
}

impl Drop for KMutexGuard {
    #[inline(always)]
    fn drop(&mut self) { mutex_unlock(self.raw); }
}

// ---------------------------------------------------------------------------
// KRwsem — typed handle + read / write RAII guards around `rwsem_t`.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
pub struct KRwsem {
    raw: *mut rwsem_t,
    _not_send_sync: PhantomData<*const ()>,
}

impl KRwsem {
    #[inline(always)]
    pub fn from_ptr(raw: *mut rwsem_t) -> Self {
        Self { raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut rwsem_t { self.raw }
    #[inline(always)]
    pub fn init(self, flags: u64, name: *const c_char) -> c_int {
        rwsem_init(self.raw, flags, name)
    }
    #[inline(always)]
    pub fn holding_write(self) -> bool { rwsem_is_write_holding(self.raw) }

    #[inline(always)]
    pub fn read(self) -> Result<KRwsemReadGuard, c_int> {
        let r = rwsem_acquire_read(self.raw);
        if r == 0 {
            Ok(KRwsemReadGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { Err(r) }
    }
    #[inline(always)]
    pub fn try_read(self) -> Option<KRwsemReadGuard> {
        if rwsem_try_acquire_read(self.raw) == 0 {
            Some(KRwsemReadGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { None }
    }
    #[inline(always)]
    pub fn read_interruptible(self) -> Result<KRwsemReadGuard, c_int> {
        let r = rwsem_acquire_read_interruptible(self.raw);
        if r == 0 {
            Ok(KRwsemReadGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { Err(r) }
    }
    #[inline(always)]
    pub fn read_timed(self, timeout_ms: u64) -> Result<KRwsemReadGuard, c_int> {
        let r = rwsem_acquire_read_timed(self.raw, timeout_ms);
        if r == 0 {
            Ok(KRwsemReadGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { Err(r) }
    }
    #[inline(always)]
    pub fn write(self) -> Result<KRwsemWriteGuard, c_int> {
        let r = rwsem_acquire_write(self.raw);
        if r == 0 {
            Ok(KRwsemWriteGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { Err(r) }
    }
    #[inline(always)]
    pub fn try_write(self) -> Option<KRwsemWriteGuard> {
        if rwsem_try_acquire_write(self.raw) == 0 {
            Some(KRwsemWriteGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { None }
    }
    #[inline(always)]
    pub fn write_interruptible(self) -> Result<KRwsemWriteGuard, c_int> {
        let r = rwsem_acquire_write_interruptible(self.raw);
        if r == 0 {
            Ok(KRwsemWriteGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { Err(r) }
    }
    #[inline(always)]
    pub fn write_timed(self, timeout_ms: u64) -> Result<KRwsemWriteGuard, c_int> {
        let r = rwsem_acquire_write_timed(self.raw, timeout_ms);
        if r == 0 {
            Ok(KRwsemWriteGuard { raw: self.raw, _not_send_sync: PhantomData })
        } else { Err(r) }
    }
}

#[must_use = "rwsem read lock is released when the guard is dropped"]
pub struct KRwsemReadGuard {
    raw: *mut rwsem_t,
    _not_send_sync: PhantomData<*const ()>,
}
impl Drop for KRwsemReadGuard {
    #[inline(always)]
    fn drop(&mut self) { rwsem_release(self.raw); }
}

#[must_use = "rwsem write lock is released when the guard is dropped"]
pub struct KRwsemWriteGuard {
    raw: *mut rwsem_t,
    _not_send_sync: PhantomData<*const ()>,
}
impl Drop for KRwsemWriteGuard {
    #[inline(always)]
    fn drop(&mut self) { rwsem_release(self.raw); }
}

// ---------------------------------------------------------------------------
// KCompletion — typed handle around `completion_t`. No RAII guard
// (the primitive is intrinsically asymmetric: `wait` consumes, `complete`
// posts; there is nothing to release on drop).
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
pub struct KCompletion {
    raw: *mut completion_t,
    _not_send_sync: PhantomData<*const ()>,
}

impl KCompletion {
    #[inline(always)]
    pub fn from_ptr(raw: *mut completion_t) -> Self {
        Self { raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut completion_t { self.raw }
    #[inline(always)]
    pub fn init(self) { completion_init(self.raw); }
    #[inline(always)]
    pub fn reinit(self) { completion_reinit(self.raw); }
    #[inline(always)]
    pub fn try_wait(self) -> bool { try_wait_for_completion(self.raw) }
    #[inline(always)]
    pub fn wait(self) { wait_for_completion(self.raw); }
    #[inline(always)]
    pub fn wait_interruptible(self) -> Result<(), c_int> {
        match wait_for_completion_interruptible(self.raw) {
            0 => Ok(()), e => Err(e),
        }
    }
    #[inline(always)]
    pub fn wait_timed(self, timeout_ms: u64) -> Result<(), c_int> {
        match wait_for_completion_timed(self.raw, timeout_ms) {
            0 => Ok(()), e => Err(e),
        }
    }
    #[inline(always)]
    pub fn complete(self) { complete(self.raw); }
    #[inline(always)]
    pub fn complete_all(self) { complete_all(self.raw); }
    #[inline(always)]
    pub fn is_done(self) -> bool { completion_done(self.raw) }
}

// ---------------------------------------------------------------------------
// KSemaphore — typed handle + RAII `permit` around `semaphore`.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
pub struct KSemaphore {
    raw: *mut sem_t,
    _not_send_sync: PhantomData<*const ()>,
}

impl KSemaphore {
    #[inline(always)]
    pub fn from_ptr(raw: *mut sem_t) -> Self {
        Self { raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut sem_t { self.raw }
    #[inline(always)]
    pub fn init(self, name: *const c_char, value: c_int) -> c_int {
        sem_init(self.raw, name, value)
    }
    #[inline(always)]
    pub fn value(self) -> Result<c_int, c_int> {
        let mut v: c_int = 0;
        match sem_getvalue(self.raw, &mut v as *mut c_int) {
            0 => Ok(v), e => Err(e),
        }
    }
    /// Try to decrement once.
    #[inline(always)]
    pub fn try_acquire(self) -> Option<KSemPermit> {
        if sem_trywait(self.raw) == 0 {
            Some(KSemPermit { raw: self.raw, _not_send_sync: PhantomData })
        } else { None }
    }
    /// Decrement, blocking if the count is zero.
    #[inline(always)]
    pub fn acquire(self) -> Result<KSemPermit, c_int> {
        match sem_wait(self.raw) {
            0 => Ok(KSemPermit { raw: self.raw, _not_send_sync: PhantomData }),
            e => Err(e),
        }
    }
    #[inline(always)]
    pub fn acquire_interruptible(self) -> Result<KSemPermit, c_int> {
        match sem_wait_interruptible(self.raw) {
            0 => Ok(KSemPermit { raw: self.raw, _not_send_sync: PhantomData }),
            e => Err(e),
        }
    }
    #[inline(always)]
    pub fn acquire_timed(self, timeout_ms: u64) -> Result<KSemPermit, c_int> {
        match sem_timedwait(self.raw, timeout_ms) {
            0 => Ok(KSemPermit { raw: self.raw, _not_send_sync: PhantomData }),
            e => Err(e),
        }
    }
    /// Release one count manually (no guard).
    #[inline(always)]
    pub fn post(self) -> c_int { sem_post(self.raw) }
}

/// Permit returned from a successful semaphore `acquire`. Dropping
/// the permit calls `sem_post`, mirroring `Semaphore::acquire` /
/// `OwnedSemaphorePermit` in std-like APIs.
pub struct KSemPermit {
    raw: *mut sem_t,
    _not_send_sync: PhantomData<*const ()>,
}

impl KSemPermit {
    /// Consume the permit without posting (caller will post manually).
    #[inline(always)]
    pub fn forget(self) { core::mem::forget(self); }
}

impl Drop for KSemPermit {
    #[inline(always)]
    fn drop(&mut self) { let _ = sem_post(self.raw); }
}

// ---------------------------------------------------------------------------
// KRwlock — typed handle + RAII guards around the spin rwlock.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
pub struct KRwlock {
    raw: *mut rwlock_t,
    _not_send_sync: PhantomData<*const ()>,
}

impl KRwlock {
    #[inline(always)]
    pub fn from_ptr(raw: *mut rwlock_t) -> Self {
        Self { raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut rwlock_t { self.raw }
    #[inline(always)]
    pub fn init(self, name: *const c_char) { rwlock_init(self.raw, name); }

    #[inline(always)]
    pub fn read(self) -> KRwlockReadGuard {
        rwlock_rlock(self.raw);
        KRwlockReadGuard { raw: self.raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn write(self) -> KRwlockWriteGuard {
        rwlock_wlock(self.raw);
        KRwlockWriteGuard { raw: self.raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn write_expedited(self) -> KRwlockWriteGuard {
        rwlock_wlock_expedited(self.raw);
        KRwlockWriteGuard { raw: self.raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn write_graceful(self) -> KRwlockWriteGuard {
        rwlock_graceful_wlock(self.raw);
        KRwlockWriteGuard { raw: self.raw, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn read_irqsave(self) -> KRwlockReadIrqGuard {
        let intena = rwlock_rlock_irqsave(self.raw);
        KRwlockReadIrqGuard { raw: self.raw, intena, _not_send_sync: PhantomData }
    }
    #[inline(always)]
    pub fn write_irqsave(self) -> KRwlockWriteIrqGuard {
        let intena = rwlock_wlock_irqsave(self.raw);
        KRwlockWriteIrqGuard { raw: self.raw, intena, _not_send_sync: PhantomData }
    }
}

#[must_use = "rwlock read lock is released when the guard is dropped"]
pub struct KRwlockReadGuard {
    raw: *mut rwlock_t,
    _not_send_sync: PhantomData<*const ()>,
}
impl Drop for KRwlockReadGuard {
    #[inline(always)]
    fn drop(&mut self) { rwlock_runlock(self.raw); }
}

#[must_use = "rwlock write lock is released when the guard is dropped"]
pub struct KRwlockWriteGuard {
    raw: *mut rwlock_t,
    _not_send_sync: PhantomData<*const ()>,
}
impl Drop for KRwlockWriteGuard {
    #[inline(always)]
    fn drop(&mut self) { rwlock_wunlock(self.raw); }
}

#[must_use = "rwlock read lock is released when the guard is dropped"]
pub struct KRwlockReadIrqGuard {
    raw: *mut rwlock_t,
    intena: c_int,
    _not_send_sync: PhantomData<*const ()>,
}
impl Drop for KRwlockReadIrqGuard {
    #[inline(always)]
    fn drop(&mut self) { rwlock_runlock_irqrestore(self.raw, self.intena); }
}

#[must_use = "rwlock write lock is released when the guard is dropped"]
pub struct KRwlockWriteIrqGuard {
    raw: *mut rwlock_t,
    intena: c_int,
    _not_send_sync: PhantomData<*const ()>,
}
impl Drop for KRwlockWriteIrqGuard {
    #[inline(always)]
    fn drop(&mut self) { rwlock_wunlock_irqrestore(self.raw, self.intena); }
}
