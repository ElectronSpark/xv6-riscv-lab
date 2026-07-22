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

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};

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
// Lock-owns-data primitive — `SpinLock<T>` / `SpinLockGuard<'_, T>`
// ===========================================================================
//
// `KSpinlock` above is a *handle* to a lock the C kernel already owns
// (embedded in some `#[repr(C)]` struct, or a bare `spinlock_t` static):
// the lock and the data it protects are two separate things, so the
// guard it returns is data-less — callers still reach through a raw
// pointer for the protected fields.
//
// `SpinLock<T>` is the other idiomatic shape (`std::sync::Mutex<T>`'s):
// the lock *owns* its data. `lock()` returns a guard that `Deref`s /
// `DerefMut`s straight to `&T` / `&mut T`, so a correctly-used call
// site needs no `unsafe` at all -- the borrow checker enforces
// "only while holding the lock", the same way it enforces any other
// borrow.
//
// ```ignore
// static COUNTERS: SpinLock<[u32; 4]> = SpinLock::new(c"counters", [0; 4]);
//
// fn bump(i: usize) {
//     let mut g = COUNTERS.lock();   // acquires
//     g[i] += 1;                     // plain slice indexing, no unsafe
// }                                  // g drops here -> releases
// ```
//
// Implementation notes:
//   * The embedded `spinlock_t` is the *exact* compile-time-initialised
//     literal every other file in this crate already uses for
//     `SPINLOCK_INITIALIZED("...")` statics (see `uart.rs`, `console.rs`,
//     `sysnet.rs`, `irq/irq_core.rs`, ...) -- `{ locked: 0, name, cpu:
//     null }` *is* the initialised state (`spin_init` just writes those
//     same three fields), so `SpinLock::new` needs no runtime `spin_init`
//     call and is a `const fn`, usable directly in a `static`.
//   * `lock()`/`try_lock()` call straight through to this crate's one
//     canonical spinlock primitive (`crate::lock::spinlock::RawSpinlock::lock` /
//     `spin_trylock` / `spin_unlock`, from `kernel/lock/spinlock.rs`,
//     Wave P3-3A) -- the same `push_off`/`spin_acquire`/... machinery
//     `KSpinlock` above delegates to (via `crate::mm::cffi::raw`), just
//     without the extra opaque-pointer hop, since we already hold the
//     real `crate::bindings::spinlock_t`.
//   * Interrupt-disable semantics are unchanged: `spin_lock`/`spin_unlock`
//     already do `push_off`/`pop_off` internally, so holding a
//     `SpinLockGuard` implies interrupts are disabled on this hart,
//     exactly like holding a `KSpinGuard`.

/// A spinlock that owns the data it protects (`std::sync::Mutex<T>`'s
/// shape, `spinlock_t`'s locking semantics). See the module section
/// above for the full design rationale and a usage example.
#[repr(C)]
pub struct SpinLock<T> {
    raw: UnsafeCell<crate::bindings::spinlock_t>,
    data: UnsafeCell<T>,
}

// SAFETY: `SpinLock<T>` only ever exposes `&T`/`&mut T` to callers that
// are provably holding the underlying spinlock (via `SpinLockGuard`),
// and the lock guarantees at most one hart holds it at a time -- the
// same serialisation argument `std::sync::Mutex<T>`'s `Sync` impl
// relies on.
//
// Deliberately *no* `T: Send` bound (std's `Mutex<T>` has one, since
// the lock can hand `data` to a different OS thread than the one that
// created it). This kernel's globals overwhelmingly store raw pointers
// (`*mut Foo`, `!Send` by std's blanket-conservative default) inside
// their lock-protected storage -- and every existing per-file
// `unsafe impl Sync for FooCell<T>` in this crate (`SyncCell<T>` in
// `tty/ptmx.rs`/`kobject.rs`/`irq/irq_core.rs`/..., `PtyTableCell`,
// `mm/kalloc.rs`'s `Storage<T>`, ...) already makes exactly this same
// judgment call unconditionally, with the same underlying argument:
// hart-to-hart handoff of a `*mut Foo` is just handing over an address,
// no different from handing over a `u64` -- there is no OS-thread-local
// destructor/allocator state a raw pointer's `Send`-ness would need to
// protect here, unlike e.g. `Rc<T>`/`MutexGuard<T>` in a hosted
// program. A future caller instantiating `SpinLock<T>` with a type that
// *does* carry genuine thread-affinity state (rare in this crate) is
// still responsible for confirming that at the call site, same as
// every other `unsafe impl Sync` in this file already requires.
unsafe impl<T> Sync for SpinLock<T> {}

impl<T> SpinLock<T> {
    /// Construct an already-initialised lock around `data`. `name` is
    /// used verbatim as the lock's diagnostic name (deadlock-panic
    /// messages -- see `deadlock_panic` in `kernel/lock/spinlock.rs`),
    /// matching every other named `spinlock_t` in this crate.
    ///
    /// `const fn`: see the module section above for why no runtime
    /// `spin_init` call is needed.
    #[inline]
    pub const fn new(name: &'static core::ffi::CStr, data: T) -> Self {
        Self {
            raw: UnsafeCell::new(crate::bindings::spinlock_t {
                locked: 0,
                name: name.as_ptr() as *mut core::ffi::c_char,
                cpu: core::ptr::null_mut(),
            }),
            data: UnsafeCell::new(data),
        }
    }

    /// Acquire the lock, blocking until it is free. Returns a RAII
    /// guard that `Deref`s/`DerefMut`s to the protected data and
    /// releases the lock when dropped.
    #[must_use]
    #[inline]
    pub fn lock(&self) -> SpinLockGuard<'_, T> {
        // SAFETY: `self.raw.get()` is a valid, non-dangling
        // `*mut spinlock_t` for at least the lifetime of `&self` (it is
        // storage embedded in `self`, never freed while `self` is
        // alive) -- the same contract every other `spin_lock` call site
        // in this crate relies on.
        unsafe { crate::lock::spinlock::RawSpinlock::lock(self.raw.get()) };
        SpinLockGuard { lock: self, _not_send_sync: PhantomData }
    }

    /// Re-run `spin_init` on the embedded lock storage, preserving the
    /// diagnostic `name` set by [`new`](Self::new).
    ///
    /// Optional: [`new`](Self::new) already const-initialises the exact
    /// three fields `spin_init` writes (`locked = 0`, `name`, `cpu =
    /// null`), so a `SpinLock` is fully usable straight out of a
    /// `static` with no runtime call. This method exists only for call
    /// sites that historically had an explicit `spin_init(&LOCK)` at
    /// subsystem init and want to keep that init point verbatim; it is
    /// idempotent (writes the same fields `new` did). Must be called
    /// before any hart races to acquire the lock.
    #[inline]
    pub fn init(&self) {
        let raw = self.raw.get();
        // SAFETY: `raw` is the lock storage embedded in `self` (never
        // freed while `self` is alive); this runs at subsystem init
        // before any concurrent acquirer exists. `spin_init` only
        // writes the three lock fields; we pass back the `name` already
        // stored by `new` so the diagnostic name is preserved.
        unsafe {
            let name = (*raw).name;
            crate::lock::spinlock::RawSpinlock::init(raw, name);
        }
    }

    /// Non-blocking variant of [`lock`](Self::lock): returns `None`
    /// immediately, without side effects, if the lock is currently held
    /// (by this hart or another).
    #[must_use]
    #[inline]
    pub fn try_lock(&self) -> Option<SpinLockGuard<'_, T>> {
        // SAFETY: see `lock`.
        let acquired = unsafe { crate::lock::spinlock::RawSpinlock::trylock(self.raw.get()) } != 0;
        if acquired {
            Some(SpinLockGuard { lock: self, _not_send_sync: PhantomData })
        } else {
            None
        }
    }

    /// Raw pointer to the protected data, **bypassing the lock**.
    ///
    /// This is the `UnsafeCell::get`-style escape hatch: it returns
    /// `*mut T` without acquiring the spinlock and without borrowing
    /// `self` beyond the call, so it is the tool for the (rare) call
    /// sites that protect the data with an *external* mutual-exclusion
    /// protocol rather than this lock. The canonical such site is the
    /// xv6fs log committer (`kernel/vfs/xv6fs/log.rs`): once `end_op`
    /// has set `committing = 1` under the lock and released it, exactly
    /// one hart runs `__xv6fs_commit`, which streams the 964-byte log
    /// header to disk across many sleeping `bread`/`bwrite` calls -- it
    /// therefore *cannot* hold the spinlock (spinlocks disable
    /// interrupts and must not be held across a yield), yet the
    /// `committing` flag guarantees no other accessor touches the data
    /// meanwhile. The lock release/reacquire around the commit provides
    /// the release/acquire barrier that publishes the committer's writes
    /// to the next lock holder.
    ///
    /// **Caller discipline:** producing the pointer is safe (it is just
    /// `UnsafeCell::get`), but dereferencing it is `unsafe` and sound
    /// only while some protocol *other* than this spinlock guarantees no
    /// other hart -- and no live [`SpinLockGuard`] on this lock --
    /// accesses the data (for the log, the `committing` flag). Misuse
    /// (deref while another accessor is live) is a data race.
    #[inline]
    pub fn data_ptr(&self) -> *mut T {
        self.data.get()
    }
}

/// RAII guard returned by [`SpinLock::lock`] / [`SpinLock::try_lock`].
///
/// `Deref`s/`DerefMut`s to the protected `T`; releases the underlying
/// spinlock when dropped. `!Send`/`!Sync` for the same reason
/// [`KSpinGuard`] is: dropping (releasing) from a hart other than the
/// one that acquired it would fail `spin_unlock`'s "does this hart hold
/// the lock" check in `kernel/lock/spinlock.rs`.
#[must_use = "the lock is released immediately if the guard is dropped"]
pub struct SpinLockGuard<'a, T> {
    lock: &'a SpinLock<T>,
    _not_send_sync: PhantomData<*const ()>,
}

impl<'a, T> Deref for SpinLockGuard<'a, T> {
    type Target = T;
    #[inline]
    fn deref(&self) -> &T {
        // SAFETY: holding a `SpinLockGuard` proves this hart holds the
        // spinlock, so no other hart can concurrently access `data`
        // through another guard -- the lock-owns-data invariant that
        // licenses this `UnsafeCell` projection (same argument
        // `std::sync::Mutex<T>`'s `Deref` relies on).
        unsafe { &*self.lock.data.get() }
    }
}

impl<'a, T> DerefMut for SpinLockGuard<'a, T> {
    #[inline]
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: see `Deref`; taking `&mut self` here additionally
        // proves no other `&T`/`&mut T` borrowed from this same guard
        // is live, so the exclusive borrow is sound.
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<'a, T> Drop for SpinLockGuard<'a, T> {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: this guard's existence proves the lock is held by
        // this hart (established in `SpinLock::lock`/`try_lock`, never
        // forged elsewhere -- `lock`/`_not_send_sync` are private
        // fields).
        unsafe { crate::lock::spinlock::RawSpinlock::unlock(self.lock.raw.get()) };
    }
}

impl<'a, T> SpinLockGuard<'a, T> {
    /// Sleep on `chan`, atomically releasing this guard's lock and
    /// reacquiring the **same** lock before returning -- the kernel
    /// `sleep(chan, lk)` protocol.
    ///
    /// This mirrors the C idiom `sleep(chan, &lk)` exactly:
    /// [`crate::proc::sleep_on_chan`] performs the release-and-block as
    /// one step with respect to `wakeup(chan)`, so a wakeup racing the
    /// sleep is never lost (no lost-wakeup window). The lock is re-held
    /// on return, so this guard is still valid afterward and the
    /// protected data is borrowable again -- typical use is to re-test
    /// the wait condition in a loop.
    ///
    /// Takes `&mut self` deliberately: it forces any outstanding
    /// `Deref`/`DerefMut` borrow of the protected data to end before
    /// the lock is released, so no `&T`/`&mut T` can be held across the
    /// sleep window (during which another hart may hold the lock and
    /// mutate the data). This is the borrow-checker analogue of the C
    /// rule "don't cache pointers into lock-protected state across a
    /// `sleep`".
    #[inline]
    pub fn sleep_on(&mut self, chan: *mut core::ffi::c_void) {
        // SAFETY: `self.lock.raw.get()` is the exact lock this guard
        // holds. `sleep_on_chan` releases it, blocks until woken on
        // `chan`, then reacquires it before returning -- restoring this
        // guard's "lock is held by this hart" invariant, which every
        // other method (`Deref`, `Drop`, ...) relies on.
        unsafe { crate::proc::sleep_on_chan(chan, self.lock.raw.get()) };
    }

    /// Interruptible analogue of [`sleep_on`](Self::sleep_on): sleep on
    /// `chan`, atomically releasing this guard's lock and reacquiring the
    /// **same** lock before returning, but returning early with `-EINTR`
    /// if a signal is delivered while blocked.
    ///
    /// Uses [`crate::proc::sleep_on_chan_interruptible`], whose
    /// `sleep_on_chan_common` releases this guard's lock, blocks on
    /// `chan`, then reacquires it before returning on **both** the woken
    /// (`0`) and the signalled (`-EINTR`) paths (see `proc/sched.rs`). The
    /// lock is therefore re-held on return in either case, so this guard
    /// is still valid afterward and the protected data is borrowable
    /// again -- the typical use is to re-test the wait condition in a loop
    /// and bail out (dropping the guard) when the return is nonzero.
    ///
    /// Takes `&mut self` deliberately, for the same two reasons as
    /// [`sleep_on`](Self::sleep_on): it ends any outstanding
    /// `Deref`/`DerefMut` borrow of the protected data before the lock is
    /// released, and it bars LLVM from hoisting the re-test of the wait
    /// condition out of the loop (the freeze/noalias defense). The return
    /// value is `sleep_on_chan_interruptible`'s verbatim -- `0` when woken,
    /// `-EINTR` when a signal interrupted the wait.
    #[inline]
    pub fn sleep_on_interruptible(&mut self, chan: *mut core::ffi::c_void) -> core::ffi::c_int {
        // SAFETY: `self.lock_ptr()` is the exact lock this guard holds.
        // `sleep_on_chan_interruptible` releases it, blocks until woken on
        // `chan` or interrupted by a signal, then reacquires it before
        // returning on both paths -- restoring this guard's "lock is held
        // by this hart" invariant, which every other method (`Deref`,
        // `Drop`, ...) relies on.
        unsafe { crate::proc::sleep_on_chan_interruptible(chan, self.lock_ptr()) }
    }

    /// The `*mut spinlock_t` embedded in this guard's [`SpinLock`] -- the
    /// exact lock this guard holds.
    ///
    /// Exposed for the (few) call sites that must hand the underlying
    /// lock to a C-ABI primitive that expects a `*mut spinlock_t` -- e.g.
    /// wiring a `tq_t` wait queue to this lock at init time
    /// (`tq_init(q, name, guard.lock_ptr())`), so that a later
    /// [`wait_on`](Self::wait_on) on that queue releases and reacquires
    /// *this* same lock. The pointer references storage embedded in the
    /// `SpinLock` (never freed while the lock is alive) and is valid for
    /// at least the guard's lifetime.
    #[inline]
    pub fn lock_ptr(&self) -> *mut crate::bindings::spinlock_t {
        self.lock.raw.get()
    }

    /// Wait on the thread queue `q`, atomically releasing this guard's
    /// lock and reacquiring the **same** lock before returning -- the
    /// `tq_t` analogue of [`sleep_on`](Self::sleep_on).
    ///
    /// [`crate::proc::tq_wait`] enqueues the caller on `q`, then releases
    /// this guard's spinlock and blocks as one step with respect to a
    /// `tq_wakeup*` on `q`, so a wakeup racing the wait is never lost
    /// (no lost-wakeup window -- same contract as `sleep(chan, &lk)`).
    /// The lock is re-held on return, so this guard is still valid
    /// afterward and the protected data is borrowable again; the typical
    /// use is to re-test the wait condition in a loop. The return value
    /// is `tq_wait`'s verbatim -- `-EINTR` when a signal interrupted an
    /// interruptible wait, `0` otherwise -- so the caller keeps the
    /// interruptible early-return path.
    ///
    /// Takes `&mut self` deliberately, for the same two reasons as
    /// [`sleep_on`](Self::sleep_on): it ends any outstanding
    /// `Deref`/`DerefMut` borrow of the protected data before the lock
    /// is released, and it bars LLVM from hoisting the re-test of the
    /// wait condition out of the loop (the freeze/noalias defense).
    ///
    /// **Caller discipline:** `q` must point to a live `tq_t` (typically
    /// embedded in this lock's own guarded data, e.g.
    /// `&raw mut inner.wait_queue`) that was `tq_init`'d against *this*
    /// lock; `rdata` must be null or a valid writable `*mut u64`.
    /// `tq_wait` atomically releases this guard's lock, blocks, and
    /// reacquires it before returning, so the guard stays valid across
    /// the wait (same contract as `sleep_on`).
    #[inline]
    pub fn wait_on(
        &mut self,
        q: *mut crate::bindings::tq_t,
        rdata: *mut u64,
    ) -> core::ffi::c_int {
        // `self.lock_ptr()` is the exact lock this guard holds; `TqRef::wait`
        // (itself a safe, null-tolerant primitive) releases it, blocks on
        // `q`, then reacquires it before returning -- restoring this
        // guard's "lock held by this hart" invariant.
        // NO-STANDALONE-FN: former `tq_wait` delegator deleted; build the
        // handle and invoke the method (null `q` maps to -EINVAL as before).
        crate::proc::access::TqRef::from_ptr(q)
            .map_or(-(crate::bindings::EINVAL as core::ffi::c_int), |r| r.wait(self.lock_ptr(), rdata))
    }
}

// ===========================================================================
// Sleep-lock RAII facade
// ===========================================================================
//
// The kernel exposes its sleep-style locks (mutex / rwsem /
// completion / semaphore / rwlock) through the functions defined in the
// `kernel/lock/*.rs` files. As of P3-D3b those keep only their
// historical *names* (`pub(crate) fn mutex_lock`, …) — the
// `#[no_mangle] extern "C"` export surface is dismantled and every
// consumer reaches them by crate path.
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

pub(crate) use crate::lock::completion::{complete, complete_all, completion_done, completion_init, completion_reinit, try_wait_for_completion, wait_for_completion, wait_for_completion_interruptible, wait_for_completion_timed};
pub(crate) use crate::lock::mutex::{holding_mutex, mutex_init, mutex_lock, mutex_lock_interruptible, mutex_lock_timed, mutex_trylock, mutex_unlock};
// `crate::lock::rwlock::rwlock_*` are genuinely `unsafe fn`; this file's
// original extern declaration asserted `pub safe fn` (this crate's usual
// FFI-facade convention — every method below already documents its own
// pointer-validity contract via `KRwlock::from_ptr`'s doc). Thin safe
// wrappers preserve that facade instead of pushing `unsafe {}` onto every
// call site below.
macro_rules! safe_rwlock_wrapper {
    ($name:ident($($arg:ident: $ty:ty),*) $(-> $ret:ty)?) => {
        #[inline(always)]
        fn $name($($arg: $ty),*) $(-> $ret)? {
            // SAFETY: see `crate::lock::rwlock::$name`'s contract; every
            // call site here goes through `KRwlock`/`KRwlockGuard`, which
            // only ever hold a pointer obtained from `KRwlock::from_ptr`.
            unsafe { crate::lock::rwlock::$name($($arg),*) }
        }
    };
}
safe_rwlock_wrapper!(rwlock_init(l: *mut rwlock_t, name: *const c_char));
safe_rwlock_wrapper!(rwlock_rlock(l: *mut rwlock_t));
safe_rwlock_wrapper!(rwlock_wlock(l: *mut rwlock_t));
safe_rwlock_wrapper!(rwlock_wlock_expedited(l: *mut rwlock_t));
safe_rwlock_wrapper!(rwlock_graceful_wlock(l: *mut rwlock_t));
safe_rwlock_wrapper!(rwlock_rlock_irqsave(l: *mut rwlock_t) -> c_int);
safe_rwlock_wrapper!(rwlock_wlock_irqsave(l: *mut rwlock_t) -> c_int);
safe_rwlock_wrapper!(rwlock_runlock(l: *mut rwlock_t));
safe_rwlock_wrapper!(rwlock_wunlock(l: *mut rwlock_t));
safe_rwlock_wrapper!(rwlock_runlock_irqrestore(l: *mut rwlock_t, intena: c_int));
safe_rwlock_wrapper!(rwlock_wunlock_irqrestore(l: *mut rwlock_t, intena: c_int));
pub(crate) use crate::lock::rwsem::{rwsem_acquire_read, rwsem_acquire_read_interruptible, rwsem_acquire_read_timed, rwsem_acquire_write, rwsem_acquire_write_interruptible, rwsem_acquire_write_timed, rwsem_init, rwsem_is_write_holding, rwsem_release, rwsem_try_acquire_read, rwsem_try_acquire_write};
pub(crate) use crate::lock::semaphore::{sem_getvalue, sem_init, sem_post, sem_timedwait, sem_trywait, sem_wait, sem_wait_interruptible};

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
