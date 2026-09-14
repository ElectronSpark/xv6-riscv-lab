//! Data-owning spinlock: safe access is tied to a borrowed guard.

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};

#[cfg(not(test))]
use crate::lock::spinlock::RawSpinlock;
#[cfg(test)]
use host_backend::RawSpinlock;

#[cfg(not(test))]
const fn new_raw_lock(name: &'static core::ffi::CStr) -> RawSpinlock {
    RawSpinlock {
        locked: 0,
        name: name.as_ptr() as *mut core::ffi::c_char,
        cpu: core::ptr::null_mut(),
    }
}

#[cfg(test)]
const fn new_raw_lock(_name: &'static core::ffi::CStr) -> RawSpinlock {
    RawSpinlock::new()
}

/// A spinlock that owns its protected data. The guard is the only safe
/// route to shared or mutable borrows; dropping it releases the lock.
/// Acquisition disables local interrupts until the guard is dropped.
///
/// Sharing the lock requires `T: Send`, since another hart may replace or
/// take ownership of the contents. A type with only hart-local ownership,
/// such as `Rc`, cannot become shareable by putting it inside this lock.
///
/// `new` fully initializes the raw lock at compile time. There is no safe
/// reinitialization operation: resetting a lock while a guard exists would
/// allow a second guard to manufacture an aliased `&mut T`.
#[repr(C)]
pub struct SpinLock<T> {
    raw: UnsafeCell<RawSpinlock>,
    data: UnsafeCell<T>,
}

// SAFETY: a guard serializes access to `T`, and `T: Send` permits handing
// ownership of its contents from one hart to another. `T: Sync` is not
// required because two harts can never borrow the data simultaneously.
unsafe impl<T: Send> Sync for SpinLock<T> {}
// SAFETY: moving an unborrowed lock also moves its owned `T`. A live guard
// borrows the lock, preventing safe code from moving it while acquired.
unsafe impl<T: Send> Send for SpinLock<T> {}

impl<T> SpinLock<T> {
    /// Construct an already-initialised lock around `data`. `name` is
    /// used verbatim as the lock's diagnostic name (deadlock-panic
    /// messages -- see `deadlock_panic` in `kernel/lock/spinlock.rs`),
    /// matching every other named `spinlock_t` in this crate.
    ///
    /// This initializes the same unlocked state as the raw backend's
    /// runtime initializer, so it can be used directly in a static.
    #[inline]
    pub const fn new(name: &'static core::ffi::CStr, data: T) -> Self {
        Self {
            raw: UnsafeCell::new(new_raw_lock(name)),
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
        unsafe { RawSpinlock::lock(self.raw.get()) };
        SpinLockGuard { lock: self, _not_send_sync: PhantomData }
    }

    /// Non-blocking variant of [`lock`](Self::lock): returns `None`
    /// immediately, without side effects, if the lock is currently held
    /// (by this hart or another).
    #[must_use]
    #[inline]
    pub fn try_lock(&self) -> Option<SpinLockGuard<'_, T>> {
        // SAFETY: see `lock`.
        let acquired = unsafe { RawSpinlock::trylock(self.raw.get()) } != 0;
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
/// raw spinlock guards are: releasing from a hart other than the
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
        unsafe { RawSpinlock::unlock(self.lock.raw.get()) };
    }
}

#[cfg(not(test))]
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
    pub fn lock_ptr(&self) -> *mut RawSpinlock {
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
    /// # Safety
    /// `q` must point to a live `tq_t` (typically
    /// embedded in this lock's own guarded data, e.g.
    /// `&raw mut inner.wait_queue`) that was `tq_init`'d against *this*
    /// lock; `rdata` must be null or a valid writable `*mut u64`.
    /// `tq_wait` atomically releases this guard's lock, blocks, and
    /// reacquires it before returning, so the guard stays valid across
    /// the wait (same contract as `sleep_on`).
    #[inline]
    pub unsafe fn wait_on(
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

// The host seam substitutes only acquisition/release. Tests below exercise
// the production ownership, borrowing, auto-trait bounds, and guard Drop.
#[cfg(test)]
mod host_backend {
    use core::sync::atomic::{AtomicBool, Ordering};

    pub struct RawSpinlock {
        held: AtomicBool,
    }

    impl RawSpinlock {
        pub const fn new() -> Self {
            Self { held: AtomicBool::new(false) }
        }

        pub unsafe fn lock(raw: *mut Self) {
            // SAFETY: the production wrapper provides its live lock storage.
            while unsafe { Self::trylock(raw) } == 0 {
                std::thread::yield_now();
            }
        }

        pub unsafe fn trylock(raw: *mut Self) -> i32 {
            // SAFETY: the production wrapper provides its live lock storage.
            unsafe { &*raw }.held.compare_exchange(
                false, true, Ordering::Acquire, Ordering::Relaxed,
            ).is_ok() as i32
        }

        pub unsafe fn unlock(raw: *mut Self) {
            // SAFETY: a live guard owns this acquire and releases it once.
            assert!(unsafe { &*raw }.held.swap(false, Ordering::Release));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{SpinLock, SpinLockGuard};
    use std::cell::Cell;
    use std::rc::Rc;

    #[test]
    fn handoff_requires_send_data_and_guards_stay_local() {
        fn assert_send_sync<T: Send + Sync>() {}
        // Cell is Send but !Sync: exclusive access suffices for sharing it.
        assert_send_sync::<SpinLock<Cell<usize>>>();

        // Two candidate implementations make type inference ambiguous if the
        // forbidden auto trait is present. This fails to compile if the lock
        // ever regains an unconditional Sync/Send implementation.
        trait AmbiguousIfSync<A> { fn check() {} }
        impl<T: ?Sized> AmbiguousIfSync<()> for T {}
        impl<T: ?Sized + Sync> AmbiguousIfSync<u8> for T {}
        let _ = <SpinLock<Rc<()>> as AmbiguousIfSync<_>>::check;
        let _ = <SpinLockGuard<'_, usize> as AmbiguousIfSync<_>>::check;

        trait AmbiguousIfSend<A> { fn check() {} }
        impl<T: ?Sized> AmbiguousIfSend<()> for T {}
        impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}
        let _ = <SpinLock<Rc<()>> as AmbiguousIfSend<_>>::check;
        let _ = <SpinLockGuard<'_, usize> as AmbiguousIfSend<_>>::check;
    }

    #[test]
    fn serializes_concurrent_mutations() {
        let lock = SpinLock::new(c"test", Cell::new(0usize));
        std::thread::scope(|threads| {
            for _ in 0..4 {
                threads.spawn(|| {
                    for _ in 0..2_000 {
                        let guard = lock.lock();
                        guard.set(guard.get() + 1);
                    }
                });
            }
        });
        assert_eq!(lock.lock().get(), 8_000);
    }

    #[test]
    fn try_lock_and_drop_preserve_exclusivity() {
        let lock = SpinLock::new(c"test", 3);
        let mut guard = lock.lock();
        assert!(lock.try_lock().is_none());
        *guard += 4;
        drop(guard);
        assert_eq!(*lock.try_lock().unwrap(), 7);
    }

    #[test]
    fn unwinding_drops_the_guard() {
        let lock = SpinLock::new(c"test", 0);
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let mut guard = lock.lock();
            *guard = 9;
            panic!("leave critical section");
        }));
        assert!(result.is_err());
        assert_eq!(*lock.try_lock().unwrap(), 9);
    }
}
