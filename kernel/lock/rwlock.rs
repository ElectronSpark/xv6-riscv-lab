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
use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

use crate::bindings::rwlock as rwlock_t;
use crate::machine;
use crate::proc::thread_queue::TqWait;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3A.
//
// `crate::bindings::rwlock` (`kernel/inc/lock/rwlock_types.h`) has ~70
// non-`lock/` referents spread across mm/proc/tty/irq (`pid_lock` in
// particular is a plain `struct rwlock` read directly by
// `proc/proc_shims.rs` via `RawRwlock::rwl_try_update`/`RawRwlock::rwl_w_holding`),
// and nearly every fn in this file is itself reached directly by
// some out-of-scope caller (not just through the small no_mangle
// surface), so — unlike `mutex.rs`/`rwsem.rs`/`semaphore.rs`/
// `completion.rs`, which have a clean public/private split — every
// fn here keeps its `*mut rwlock_t` parameter unchanged (NO-STANDALONE-FN
// sweep only relocated every fn into `impl RawRwlock`, it did not change
// any parameter shape).
//
// P3-N2 nativization split: `Rwlock` below IS the kernel-wide Rust
// definition of `kernel/inc/lock/rwlock_types.h`'s `struct rwlock` now
// (`build.rs` blocklists the bindgen `rwlock` and re-exports
// `pub use crate::lock::rwlock::Rwlock as rwlock;`). It reproduces the
// bindgen record byte-for-byte, INCLUDING the plain (non-atomic)
// `u64`/`c_int` field types the C11 `_Atomic uint64`/`_Atomic int`
// members lowered to — `proc_shims.rs`'s `PROC_TABLE` static constructs
// it with a plain field literal (`rwlock { state: 0, .. }`), and the
// `pcache` bindgen struct embeds it by value, both of which require the
// plain-POD (Copy) shape. All atomic access keeps going through the
// separate `RawRwlock` view below, unchanged.
//
// `struct rwlock` carries `__ALIGNED_CACHELINE` on the *struct* itself
// (unlike `spinlock`, where it rides the typedef), so here the record
// type genuinely is size 64 / align 64.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct Rwlock {
    pub state: u64,
    pub w_holder: c_int,
    pub name: *const c_char,
}

// P3-N2 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `#[repr(C)]
// #[repr(align(64))] pub struct rwlock { state: uint64, w_holder:
// c_int, name: *const c_char }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe against
// `kernel/inc/lock/rwlock_types.h` (size 64, align 64, offsets 0/8/16).
const _: () = {
    assert!(core::mem::size_of::<Rwlock>() == 64, "struct rwlock: 24 payload bytes padded to one cacheline");
    assert!(core::mem::align_of::<Rwlock>() == 64, "struct rwlock: __ALIGNED_CACHELINE on the struct");
    assert!(core::mem::offset_of!(Rwlock, state) == 0, "rwlock.state offset");
    assert!(core::mem::offset_of!(Rwlock, w_holder) == 8, "rwlock.w_holder offset");
    assert!(core::mem::offset_of!(Rwlock, name) == 16, "rwlock.name offset");
};

// `RawRwlock` is still genuinely useful: unlike the C record type
// (whose `state`/`w_holder` fields are plain `uint64`/`c_int`, forcing
// every access through a pointer-cast-to-atomic reinterpretation), the
// native mirror declares those fields as real `AtomicU64`/`AtomicI32`.
// `as_native` below is the *only* unsafe cast in the file's
// atomic-access path; `state_atomic`/`holder_atomic` (private, zero
// external referents) become simple safe field projections on top of
// it, replacing the two independent raw pointer-to-atomic
// reinterpretations that existed previously.
#[repr(C, align(64))]
pub(crate) struct RawRwlock {
    pub(crate) state: core::sync::atomic::AtomicU64,
    pub(crate) w_holder: core::sync::atomic::AtomicI32,
    pub(crate) name: *const c_char,
}

const _: () = {
    assert!(core::mem::size_of::<RawRwlock>() == core::mem::size_of::<rwlock_t>());
    assert!(core::mem::align_of::<RawRwlock>() == core::mem::align_of::<rwlock_t>());
    assert!(core::mem::offset_of!(RawRwlock, state) == core::mem::offset_of!(rwlock_t, state));
    assert!(
        core::mem::offset_of!(RawRwlock, w_holder) == core::mem::offset_of!(rwlock_t, w_holder)
    );
    assert!(core::mem::offset_of!(RawRwlock, name) == core::mem::offset_of!(rwlock_t, name));
};

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

// NO-STANDALONE-FN sweep: every relocatable free fn in this file (internal
// state/CAS/field helpers and the public C-style API alike) is now an
// associated fn on `impl RawRwlock`, keyed by raw-pointer parameter, not by
// `&self`/`&RawRwlock`. `RawRwlock` is read lock-free across harts inside
// the spin-wait loops below (`racquire_inner`/`wacquire_inner`/...); forming
// a `&RawRwlock` receiver would let LLVM's `noalias` hoist the atomic load
// out of the loop -- a permanent hang (see `lock/spinlock.rs`'s identical
// pilot note). Every moved body is byte-identical to its old free-fn form;
// only the wrapping `impl` block, the fn name (leading `rwlock_` stripped
// from the public API), and call-site `Self::`/`RawRwlock::` qualification
// changed.
impl RawRwlock {
    /// Reinterpret the bindgen `*mut rwlock_t` as the native mirror.
    ///
    /// SAFETY: layout equivalence is proven at compile time by the
    /// assertions above; the caller provides a valid, non-dangling
    /// `*mut rwlock_t`.
    #[inline(always)]
    fn as_native<'a>(rw: *mut rwlock_t) -> &'a RawRwlock {
        // SAFETY: see doc comment; `AtomicU64`/`AtomicI32` tolerate shared
        // access from multiple hosts (that is the whole point of atomics).
        unsafe { &*(rw as *const RawRwlock) }
    }

    /// Writer-starvation-prevention threshold (mirrors
    /// `RWLOCK_EXPEDITE_THRESHOLD` = `TICK_MS << 2`, i.e. 4ms of raw ticks).
    #[inline(always)]
    fn expedite_threshold() -> u64 {
        machine::Riscv::tick_ms() << 2
    }

    // -----------------------------------------------------------------
    // Centralised-unsafe field accessors
    // -----------------------------------------------------------------

    #[inline(always)]
    fn state_atomic<'a>(rw: *mut rwlock_t) -> &'a AtomicU64 {
        &Self::as_native(rw).state
    }

    #[inline(always)]
    fn holder_atomic<'a>(rw: *mut rwlock_t) -> &'a AtomicI32 {
        &Self::as_native(rw).w_holder
    }

    #[inline(always)]
    fn set_name(rw: *mut rwlock_t, name: *const c_char) {
        // SAFETY: caller has exclusive access during init; writing through
        // a shared `&RawRwlock` to a plain (non-atomic) field is sound
        // here because init happens before the lock is published to any
        // other hart (same precondition the C original relied on).
        unsafe {
            let p = core::ptr::addr_of!(Self::as_native(rw).name) as *mut *const c_char;
            p.write(name);
        }
    }

    // -----------------------------------------------------------------
    // State-machine predicates and CAS primitives — native Rust replacement
    // for the `static inline` functions previously in
    // `kernel/inc/lock/rwlock.h`.
    // -----------------------------------------------------------------

    /// Mirrors `rwlock_can_rlock`.
    fn can_rlock(rw: *mut rwlock_t, state: u64) -> bool {
        if state & RWLOCK_STATE_WRITER_MASK != 0 {
            Self::w_holding(rw)
        } else {
            true // No writers — readers can acquire.
        }
    }

    /// Mirrors `rwlock_try_rlock`: single CAS attempt, reader count += bias.
    fn try_rlock(rw: *mut rwlock_t) -> bool {
        let a = Self::state_atomic(rw);
        let mut val = a.load(Ordering::Acquire);
        while Self::can_rlock(rw, val) {
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
        let a = Self::state_atomic(rw);
        let mut val = a.load(Ordering::Acquire);
        loop {
            if !Self::can_wlock(val, expedite) {
                return false;
            }
            match a.compare_exchange(
                val,
                RWLOCK_STATE_WRITER_HOLDING,
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(_) => {
                    Self::holder_atomic(rw).store(machine::Riscv::cpuid(), Ordering::Release);
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
        let a = Self::state_atomic(rw);
        let mut val = a.load(Ordering::Acquire);
        while Self::can_update(val) {
            match a.compare_exchange(
                val,
                RWLOCK_STATE_WRITER_HOLDING,
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(_) => {
                    Self::holder_atomic(rw).store(machine::Riscv::cpuid(), Ordering::Release);
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
        machine::Riscv::push_off();
        let ret = machine::Riscv::cpuid() == Self::holder_atomic(rw).load(Ordering::Acquire);
        machine::Riscv::pop_off();
        ret
    }

    /// Mirrors `atomic_sub(&rw->state, READER_BIAS)`.
    fn atomic_sub_reader(rw: *mut rwlock_t) {
        Self::state_atomic(rw).fetch_sub(RWLOCK_STATE_READER_BIAS, Ordering::SeqCst);
    }

    fn store_unlocked(rw: *mut rwlock_t) {
        Self::state_atomic(rw).store(RWLOCK_STATE_UNLOCKED, Ordering::Release);
    }

    fn store_holder_none(rw: *mut rwlock_t) {
        Self::holder_atomic(rw).store(RWLOCK_NONE_HOLDER, Ordering::Release);
    }

    // -----------------------------------------------------------------
    // Safe inner helpers — spin policy and IRQ wrappers. The `pub(crate)
    // unsafe fn` wrappers below are thin trampolines that hand back the
    // historical `rwlock_*` names (now stripped of the prefix); they
    // delegate to these functions so that intra-module recursion does not
    // need `unsafe { ... }` blocks.
    // -----------------------------------------------------------------

    fn init_inner(rw: *mut rwlock_t, name: *const c_char) {
        if rw.is_null() { return; }
        Self::store_unlocked(rw);
        Self::store_holder_none(rw);
        let fallback = b"unnamed\0".as_ptr() as *const c_char;
        let n = if name.is_null() { fallback } else { name };
        Self::set_name(rw, n);
    }

    fn racquire_inner(rw: *mut rwlock_t) {
        if rw.is_null() { return; }
        while !Self::try_rlock(rw) {
            machine::Riscv::cpu_relax();
        }
    }

    fn rrelease_inner(rw: *mut rwlock_t) {
        if rw.is_null() { return; }
        Self::atomic_sub_reader(rw);
    }

    fn wacquire_inner(rw: *mut rwlock_t) {
        if rw.is_null() { return; }
        let start = machine::Riscv::read_time();
        let threshold = Self::expedite_threshold();
        let mut expedite = false;
        while !Self::try_wlock(rw, expedite) {
            machine::Riscv::cpu_relax();
            if !expedite && machine::Riscv::read_time().wrapping_sub(start) >= threshold {
                expedite = true;
            }
        }
    }

    fn wacquire_expedited_inner(rw: *mut rwlock_t) {
        if rw.is_null() { return; }
        while !Self::try_wlock(rw, true) {
            machine::Riscv::cpu_relax();
        }
    }

    fn graceful_wacquire_inner(rw: *mut rwlock_t) {
        if rw.is_null() { return; }
        while !Self::try_wlock(rw, false) {
            machine::Riscv::cpu_relax();
        }
    }

    fn writer_release_inner(rw: *mut rwlock_t) {
        if rw.is_null() { return; }
        Self::store_holder_none(rw);
        Self::store_unlocked(rw);
    }

    fn rlock_inner(rw: *mut rwlock_t) {
        machine::Riscv::push_off();
        Self::racquire_inner(rw);
    }

    fn runlock_inner(rw: *mut rwlock_t) {
        Self::rrelease_inner(rw);
        machine::Riscv::pop_off();
    }

    fn wlock_inner(rw: *mut rwlock_t) {
        machine::Riscv::push_off();
        Self::wacquire_inner(rw);
    }

    fn wunlock_inner(rw: *mut rwlock_t) {
        Self::writer_release_inner(rw);
        machine::Riscv::pop_off();
    }

    // ===================================================================
    // Cross-module surface — thin trampolines around the safe inner fns.
    // Formerly `#[no_mangle] pub unsafe extern "C" fn` C-ABI exports; all
    // consumers are Rust (sync/sync.rs, mm/pcache.rs, proc/proc_shims.rs)
    // and reach these by crate path now, so they are plain associated fns.
    // ===================================================================

    pub(crate) unsafe fn init(rw: *mut rwlock_t, name: *const c_char) {
        Self::init_inner(rw, name);
    }

    pub(crate) unsafe fn racquire(rw: *mut rwlock_t) { Self::racquire_inner(rw); }

    pub(crate) unsafe fn rrelease(rw: *mut rwlock_t) { Self::rrelease_inner(rw); }

    pub(crate) unsafe fn wacquire(rw: *mut rwlock_t) { Self::wacquire_inner(rw); }

    pub(crate) unsafe fn wacquire_expedited(rw: *mut rwlock_t) {
        Self::wacquire_expedited_inner(rw);
    }

    pub(crate) unsafe fn graceful_wacquire(rw: *mut rwlock_t) {
        Self::graceful_wacquire_inner(rw);
    }

    pub(crate) unsafe fn writer_release(rw: *mut rwlock_t) {
        Self::writer_release_inner(rw);
    }

    pub(crate) unsafe fn rlock(rw: *mut rwlock_t) { Self::rlock_inner(rw); }

    pub(crate) unsafe fn runlock(rw: *mut rwlock_t) { Self::runlock_inner(rw); }

    pub(crate) unsafe fn wlock(rw: *mut rwlock_t) { Self::wlock_inner(rw); }

    pub(crate) unsafe fn wlock_expedited(rw: *mut rwlock_t) {
        machine::Riscv::push_off();
        Self::wacquire_expedited_inner(rw);
    }

    pub(crate) unsafe fn graceful_wlock(rw: *mut rwlock_t) {
        machine::Riscv::push_off();
        Self::graceful_wacquire_inner(rw);
    }

    pub(crate) unsafe fn wunlock(rw: *mut rwlock_t) { Self::wunlock_inner(rw); }

    // -----------------------------------------------------------------
    // irqsave / irqrestore wrappers
    // -----------------------------------------------------------------

    pub(crate) unsafe fn rlock_irqsave(rw: *mut rwlock_t) -> c_int {
        let intena = machine::Riscv::intr_off_save();
        Self::racquire_inner(rw);
        intena
    }

    pub(crate) unsafe fn runlock_irqrestore(rw: *mut rwlock_t, intena: c_int) {
        Self::rrelease_inner(rw);
        machine::Riscv::intr_restore(intena);
    }

    pub(crate) unsafe fn wlock_irqsave(rw: *mut rwlock_t) -> c_int {
        let intena = machine::Riscv::intr_off_save();
        Self::wacquire_inner(rw);
        intena
    }

    pub(crate) unsafe fn wlock_expedited_irqsave(rw: *mut rwlock_t) -> c_int {
        let intena = machine::Riscv::intr_off_save();
        Self::wacquire_expedited_inner(rw);
        intena
    }

    pub(crate) unsafe fn graceful_wlock_irqsave(rw: *mut rwlock_t) -> c_int {
        let intena = machine::Riscv::intr_off_save();
        Self::graceful_wacquire_inner(rw);
        intena
    }

    pub(crate) unsafe fn wunlock_irqrestore(rw: *mut rwlock_t, intena: c_int) {
        Self::writer_release_inner(rw);
        machine::Riscv::intr_restore(intena);
    }

    // -----------------------------------------------------------------
    // Raw state-word dispatchers for `kernel/proc/proc_shims.rs`, which
    // calls these two directly on `pid_lock` (a plain `struct rwlock`)
    // rather than going through the higher-level API. They used to be
    // defined in `kernel/lock/rwlock_shim.c` and were later re-exported as
    // C-ABI symbols; proc_shims.rs now imports them by crate path, so they
    // are plain associated fns. Kept `pub(crate)` and their historical
    // `__rwl_*` names (test-hook / raw-primitive convention), just moved
    // into the `impl` and renamed off the module-level namespace.
    // -----------------------------------------------------------------

    pub(crate) unsafe fn rwl_try_update(rw: *mut rwlock_t) -> bool { Self::try_update(rw) }

    pub(crate) unsafe fn rwl_w_holding(rw: *mut rwlock_t) -> bool { Self::w_holding(rw) }
}

// ---------------------------------------------------------------------------
// Sleep / wakeup callbacks for rwlock-protected thread-queue waits
//
// TRAIT-OPS (final wave): `rwlock_r_sleep_cb`/`rwlock_r_wake_cb` used to be
// an address-taken `extern "C" fn` pair; `mm/pcache.rs`'s sole call site
// (`Pcache::io_wait`) passed them *by value* as `Option<unsafe extern "C"
// fn(...)>` arguments to `TqRef::wait_cb` -- same fn-pointer-type-coercion
// reasoning as `lock::spinlock`'s retired `spin_sleep_cb`/`spin_wake_cb`.
// That call site now passes a single `&'static dyn TqWait`
// (`&RWLOCK_R_TQ_WAIT`) instead, so the pair collapses into one ZST.
// `rwlock_w_sleep_cb`/`rwlock_w_wake_cb` had zero in-tree callers already
// (pre-existing dead code -- they were plain `unsafe fn`, never coerced to
// an `extern "C"` fn-pointer slot); converted anyway for consistency, so
// no bare fn-ptr slot remains in this family. Bodies moved verbatim.
// ---------------------------------------------------------------------------

const RW_CB_STATUS_READER: c_int = 1;
const RW_CB_STATUS_WRITER: c_int = 2;

/// TRAIT-OPS: the rwlock-reader [`TqWait`] impl -- installed as
/// `&'static RWLOCK_R_TQ_WAIT` at `mm/pcache.rs`'s `io_wait` call site.
pub(crate) struct RwlockRTqWait;

impl TqWait for RwlockRTqWait {
    unsafe fn sleep(&self, data: *mut c_void) -> c_int {
        if data.is_null() { return 0; }
        let rw = data as *mut rwlock_t;
        RawRwlock::runlock_inner(rw);
        let mut status = RW_CB_STATUS_READER;
        if RawRwlock::w_holding(rw) {
            // The reader may also hold the write lock (write→read recursion).
            RawRwlock::wunlock_inner(rw);
            status |= RW_CB_STATUS_WRITER;
        }
        status
    }

    unsafe fn wake(&self, data: *mut c_void, status: c_int) {
        if data.is_null() { return; }
        let rw = data as *mut rwlock_t;
        if (status & RW_CB_STATUS_WRITER) != 0 {
            RawRwlock::wlock_inner(rw);
        }
        if (status & RW_CB_STATUS_READER) != 0 {
            RawRwlock::rlock_inner(rw);
        }
    }
}

pub(crate) static RWLOCK_R_TQ_WAIT: RwlockRTqWait = RwlockRTqWait;

/// TRAIT-OPS: the rwlock-writer [`TqWait`] impl. No in-tree installer
/// (pre-existing dead code, see the module note above); kept as a
/// `pub(crate)` ZST + static so a future writer-side timed wait can adopt
/// it the same way the reader side does.
#[allow(dead_code)]
pub(crate) struct RwlockWTqWait;

impl TqWait for RwlockWTqWait {
    unsafe fn sleep(&self, data: *mut c_void) -> c_int {
        if !data.is_null() {
            let rw = data as *mut rwlock_t;
            RawRwlock::wunlock_inner(rw);
        }
        RW_CB_STATUS_WRITER
    }

    unsafe fn wake(&self, data: *mut c_void, status: c_int) {
        if data.is_null() { return; }
        if (status & RW_CB_STATUS_WRITER) != 0 {
            let rw = data as *mut rwlock_t;
            RawRwlock::wlock_inner(rw);
        }
    }
}

#[allow(dead_code)]
pub(crate) static RWLOCK_W_TQ_WAIT: RwlockWTqWait = RwlockWTqWait;

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
        unsafe { RawRwlock::init(self.raw, name); }
    }

    /// Acquire shared (reader).
    #[inline]
    pub fn read(self) -> KRwLockReadGuard {
        // SAFETY: see `init`.
        unsafe { RawRwlock::rlock(self.raw); }
        KRwLockReadGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Acquire exclusive (writer, fair variant).
    #[inline]
    pub fn write(self) -> KRwLockWriteGuard {
        // SAFETY: see `init`.
        unsafe { RawRwlock::wlock(self.raw); }
        KRwLockWriteGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Acquire exclusive with priority-boost (expedited).
    #[inline]
    pub fn write_expedited(self) -> KRwLockWriteGuard {
        // SAFETY: see `init`.
        unsafe { RawRwlock::wlock_expedited(self.raw); }
        KRwLockWriteGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Acquire exclusive cooperatively (graceful).
    #[inline]
    pub fn write_graceful(self) -> KRwLockWriteGuard {
        // SAFETY: see `init`.
        unsafe { RawRwlock::graceful_wlock(self.raw); }
        KRwLockWriteGuard { raw: self.raw, _ns: PhantomData }
    }

    /// Acquire shared, saving the interrupt-enable flag. The guard's
    /// `Drop` restores it via `rwlock_runlock_irqrestore`.
    #[inline]
    pub fn read_irqsave(self) -> KRwLockReadIrqGuard {
        // SAFETY: see `init`.
        let intena = unsafe { RawRwlock::rlock_irqsave(self.raw) };
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
        unsafe { RawRwlock::runlock(self.raw); }
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
        unsafe { RawRwlock::wunlock(self.raw); }
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
        unsafe { RawRwlock::runlock_irqrestore(self.raw, self.intena); }
    }
}
