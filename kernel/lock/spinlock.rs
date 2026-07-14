//! Rust port of `kernel/lock/spinlock.c`.
//!
//! The exported functions keep the C ABI bit-for-bit (`#[no_mangle]
//! pub unsafe extern "C" fn spin_*`); both the original C call sites
//! and the bindgen-imported declarations in this crate resolve to the
//! symbols defined here.
//!
//! Centralised-unsafe pattern: every raw atomic access on
//! `spinlock_t::locked` and `spinlock_t::cpu` is encapsulated in one
//! of the tiny private helpers at the top of the file. Function
//! bodies below them contain *no* visible `unsafe` blocks.
//!
//! Memory ordering matches the C original:
//!   * `__atomic_test_and_set(.., __ATOMIC_ACQUIRE)`
//!       → `AtomicU32::swap(1, Acquire)`
//!   * `__atomic_clear(.., __ATOMIC_RELEASE)`
//!       → `AtomicU32::store(0, Release)`
//!   * `__atomic_store_n(cpu, _, __ATOMIC_RELAXED|RELEASE)`
//!       → `AtomicPtr::store(_, Relaxed|Release)`
//!   * `__atomic_load_n(cpu, __ATOMIC_ACQUIRE)`
//!       → `AtomicPtr::load(Acquire)`
//!   * `__atomic_signal_fence(__ATOMIC_ACQUIRE)`
//!       → `compiler_fence(Acquire)`

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{compiler_fence, AtomicPtr, AtomicU32, Ordering};

use crate::bindings::{cpu_local, spinlock_t};
use crate::machine::{
    self, cpu_relax, intr_off_save, intr_on, intr_restore, pop_off, push_off, wfi, write_sie,
    CpuLocal, CPU_FLAG_CRASHED, SIE_SSIE,
};

// ---------------------------------------------------------------------------
// Externs: the few C helpers we still rely on.
// ---------------------------------------------------------------------------
unsafe extern "C" {
    pub safe fn panic_state() -> c_int;
    pub safe fn __panic_start();
    pub safe fn __panic_end() -> !;
}

unsafe extern "C" {
    pub fn printf(fmt: *const c_char, ...) -> c_int;
}

// ---------------------------------------------------------------------------
// Centralised raw-pointer / atomic accessors on `spinlock_t`.
//
// Each helper performs exactly one field access through the raw `*mut
// spinlock_t` and confines the resulting `unsafe` block to one line.
// ---------------------------------------------------------------------------

#[inline(always)]
fn locked_atomic<'a>(lk: *mut spinlock_t) -> &'a AtomicU32 {
    // SAFETY: caller provides a valid `*mut spinlock_t`; the `locked`
    // field has the same layout and alignment as `AtomicU32`.
    unsafe { &*((&raw mut (*lk).locked) as *const AtomicU32) }
}

#[inline(always)]
fn cpu_atomic<'a>(lk: *mut spinlock_t) -> &'a AtomicPtr<cpu_local> {
    // SAFETY: see `locked_atomic`. `*mut cpu_local` has the same
    // layout as `AtomicPtr<cpu_local>`.
    unsafe { &*((&raw mut (*lk).cpu) as *const AtomicPtr<cpu_local>) }
}

#[inline(always)]
fn read_name(lk: *mut spinlock_t) -> *mut c_char {
    // SAFETY: caller provides a valid `*mut spinlock_t`.
    unsafe { (*lk).name }
}

#[inline(always)]
fn set_name(lk: *mut spinlock_t, name: *mut c_char) {
    // SAFETY: see `read_name`. Exclusive at init time.
    unsafe {
        (*lk).name = name;
    }
}

#[inline(always)]
fn set_locked_plain(lk: *mut spinlock_t, v: u32) {
    // SAFETY: init-time only; no concurrent access yet.
    unsafe {
        (*lk).locked = v;
    }
}

#[inline(always)]
fn set_cpu_plain(lk: *mut spinlock_t, p: *mut cpu_local) {
    // SAFETY: init-time only.
    unsafe {
        (*lk).cpu = p;
    }
}

// ---------------------------------------------------------------------------
// Panic helpers — keep the panic-message formatting strings here so
// they live alongside the call sites.
// ---------------------------------------------------------------------------
static FMT_DEADLOCK: &[u8] = b"spin_acquire: deadlock detected on lock %s\n\0";
static MSG_REENTRY: &[u8] = b"spin_lock reentry\0";
static MSG_UNLOCK: &[u8] = b"spin_unlock\0";

#[inline(never)]
#[cold]
fn deadlock_panic(name: *mut c_char) -> ! {
    __panic_start();
    // SAFETY: variadic call; FMT_DEADLOCK is a nul-terminated string.
    unsafe {
        printf(FMT_DEADLOCK.as_ptr() as *const c_char, name);
    }
    __panic_end()
}

#[inline(never)]
#[cold]
fn fixed_msg_panic(msg: &'static [u8]) -> ! {
    static FMT: &[u8] = b"%s\n\0";
    __panic_start();
    // SAFETY: variadic call; `msg` and FMT are nul-terminated.
    unsafe {
        printf(FMT.as_ptr() as *const c_char, msg.as_ptr() as *const c_char);
    }
    __panic_end()
}

// ---------------------------------------------------------------------------
// Internal: is the current hart already the recorded owner of `lk`?
// ---------------------------------------------------------------------------
#[inline]
fn holding(lk: *mut spinlock_t) -> bool {
    cpu_atomic(lk).load(Ordering::Acquire) == CpuLocal::current().as_ptr()
}

// ---------------------------------------------------------------------------
// Slow path of `spin_acquire`: contention loop with panic-state checks.
// ---------------------------------------------------------------------------
#[inline(never)]
fn acquire_contended(lk: *mut spinlock_t) {
    let locked = locked_atomic(lk);
    let mut count: u64 = 0;
    // `TICK_S` is `__timebase_frequency` in the C original; the
    // deadlock threshold is `TICK_S * 100`. The volatile read is
    // centralised in `machine::tick_s`.
    let deadlock_threshold = machine::tick_s().saturating_mul(100);

    loop {
        if locked.swap(1, Ordering::Acquire) == 0 {
            return;
        }
        count = count.wrapping_add(1);
        if count >= 10 {
            cpu_relax();
        }
        if (count & 0xFFFF) == 0 {
            let mut cpu = CpuLocal::current();
            if (cpu.flags() & CPU_FLAG_CRASHED) == 0 && panic_state() != 0 {
                cpu.flags_or(CPU_FLAG_CRASHED);
                write_sie(SIE_SSIE);
                intr_on();
                loop {
                    wfi();
                }
            }
        }
        if count >= deadlock_threshold {
            let cpu = CpuLocal::current();
            if (cpu.flags() & CPU_FLAG_CRASHED) == 0 {
                deadlock_panic(read_name(lk));
            }
        }
    }
}

// ===========================================================================
// Public ABI — matches `kernel/inc/defs.h` prototypes.
// ===========================================================================

/// Initialise a spinlock. Mirrors C `spin_init`.
#[no_mangle]
pub unsafe extern "C" fn spin_init(lk: *mut spinlock_t, name: *mut c_char) {
    set_name(lk, name);
    set_locked_plain(lk, 0);
    set_cpu_plain(lk, core::ptr::null_mut());
}

/// Spin until `lk` is acquired by the current hart. Caller must have
/// interrupts disabled (the public entry point `spin_lock` does that).
pub(crate) unsafe fn spin_acquire(lk: *mut spinlock_t) {
    if lk.is_null() || holding(lk) {
        fixed_msg_panic(MSG_REENTRY);
    }
    let locked = locked_atomic(lk);
    // Fast path: try once before entering the contention loop.
    if locked.swap(1, Ordering::Acquire) != 0 {
        acquire_contended(lk);
    }
    cpu_atomic(lk).store(CpuLocal::current().as_ptr(), Ordering::Relaxed);
    CpuLocal::current().spin_depth_add(1);
    compiler_fence(Ordering::Acquire);
}

/// Release a spinlock previously acquired by this hart.
pub(crate) unsafe fn spin_release(lk: *mut spinlock_t) {
    if lk.is_null() || !holding(lk) {
        fixed_msg_panic(MSG_UNLOCK);
    }
    cpu_atomic(lk).store(core::ptr::null_mut(), Ordering::Release);
    locked_atomic(lk).store(0, Ordering::Release);
    CpuLocal::current().spin_depth_add(-1);
}

/// Try-acquire variant that disables interrupts internally.
pub(crate) unsafe fn spin_trylock(lk: *mut spinlock_t) -> c_int {
    push_off();
    if holding(lk) {
        pop_off();
        return 0;
    }
    if locked_atomic(lk).swap(1, Ordering::Acquire) != 0 {
        pop_off();
        return 0;
    }
    cpu_atomic(lk).store(CpuLocal::current().as_ptr(), Ordering::Relaxed);
    CpuLocal::current().spin_depth_add(1);
    compiler_fence(Ordering::Acquire);
    1
}

/// Returns 1 if the current hart holds `lk`, 0 otherwise. Interrupts
/// must be disabled by the caller (the per-hart check uses `tp`).
#[no_mangle]
pub unsafe extern "C" fn spin_holding(lk: *mut spinlock_t) -> c_int {
    if holding(lk) {
        1
    } else {
        0
    }
}

/// Default `spin_lock`: disables interrupts and acquires.
#[no_mangle]
pub unsafe extern "C" fn spin_lock(lk: *mut spinlock_t) {
    push_off();
    spin_acquire(lk);
}

/// Default `spin_unlock`: releases and re-enables interrupts.
#[no_mangle]
pub unsafe extern "C" fn spin_unlock(lk: *mut spinlock_t) {
    spin_release(lk);
    pop_off();
}

/// `spin_lock` variant that saves the interrupt-enable flag.
pub(crate) unsafe fn spin_lock_irqsave(lk: *mut spinlock_t) -> c_int {
    let intena = intr_off_save();
    spin_acquire(lk);
    intena
}

/// Companion of `spin_lock_irqsave`.
pub(crate) unsafe fn spin_unlock_irqrestore(lk: *mut spinlock_t, intena: c_int) {
    spin_release(lk);
    intr_restore(intena);
}

// ---------------------------------------------------------------------------
// Sleep/wake callbacks used by `tq_wait()` / `ttree_wait()`. Their
// declarations live in `kernel/inc/lock/spinlock.h`.
//
// NOT demoted to a plain Rust fn like their neighbors: `proc/thread_queue.rs`
// (out of this wave's scope) passes these *by value* as
// `Option<unsafe extern "C" fn(...)>` callback arguments to
// `tq_wait_cb_impl`/`ttree_wait_cb_impl` (function-pointer coercion, not a
// symbol-table lookup) — the `extern "C"` calling convention is therefore
// part of the function's *type*, not just its link-time symbol name, and
// must stay for the coercion to type-check. `#[no_mangle]` is still dropped:
// nothing resolves these by C symbol name (no out-of-scope extern-block
// declares them), only by the Rust-level fn-pointer value.
// ---------------------------------------------------------------------------

pub(crate) unsafe extern "C" fn spin_sleep_cb(data: *mut c_void) -> c_int {
    if data.is_null() {
        return 0;
    }
    let lk = data as *mut spinlock_t;
    let status = spin_holding(lk);
    if status != 0 {
        spin_unlock(lk);
    }
    status
}

pub(crate) unsafe extern "C" fn spin_wake_cb(data: *mut c_void, sleep_cb_status: c_int) {
    if !data.is_null() && sleep_cb_status != 0 {
        let lk = data as *mut spinlock_t;
        spin_lock(lk);
    }
}

// Re-export `machine::*` we touch so `#![allow(unused_imports)]` is not
// needed when the module set grows.
#[allow(dead_code)]
fn _link_ll_helpers() {
    let _ = machine::intr_get;
}
