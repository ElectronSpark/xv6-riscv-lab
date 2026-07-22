//! Rust port of `kernel/lock/spinlock.c`.
//!
//! The entry points (`RawSpinlock::{init,acquire,release,trylock,
//! is_holding,lock,unlock,lock_irqsave,unlock_irqrestore}`) keep the
//! exact C signatures (they still take a raw `*mut spinlock_t`, never
//! `&self`/`&RawSpinlock` -- see the freeze-noalias note below); every
//! consumer in the crate reaches them by crate path
//! (P3-D3c: the `#[no_mangle] extern "C"` C-ABI surface is gone).
//! NO-STANDALONE-FN sweep: every relocatable free fn in this file
//! (internal atomic-accessor helpers and the public C-style API alike)
//! is now an associated fn on `impl RawSpinlock`, keyed by raw-pointer
//! parameter, not by `&self`/`&T`: `RawSpinlock`/`spinlock_t` are
//! `Freeze` and read lock-free across harts inside spin loops, so
//! forming a `&RawSpinlock` would let LLVM's `noalias` hoist the
//! atomic/field load out of the loop -- a permanent hang. Every moved
//! body is byte-identical to its old free-fn form; only the wrapping
//! `impl` block, the fn name (leading `spin_` stripped from the public
//! API), and call-site qualification changed. `spin_sleep_cb`/
//! `spin_wake_cb` stay free fns (floor: address-taken
//! `extern "C" fn` pointers coerced into `Option<unsafe extern "C" fn>`
//! callback slots -- converting to an associated fn would break that
//! coercion).
//!
//! Centralised-unsafe pattern: every raw atomic access on
//! `spinlock_t::locked` and `spinlock_t::cpu` is encapsulated in one
//! of the tiny private associated fns at the top of the `impl`. Fn
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
use crate::machine::{self, CpuLocal, Riscv, CPU_FLAG_CRASHED, SIE_SSIE};
// P3-1C mesh sweep: printf.rs is in scope for this wave, so `panic_state`
// becomes a plain crate-path import instead of an `extern "C"` redeclaration.
use crate::printf::Printf;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3A, nativized in Wave P3-N2.
//
// `RawSpinlock` IS the kernel-wide Rust definition of
// `kernel/inc/lock/spinlock.h`'s `struct spinlock` now: `build.rs`
// blocklists the bindgen-generated `spinlock`/`spinlock_t` and injects
// `pub use crate::lock::spinlock::RawSpinlock as spinlock_t;` raw-line
// re-exports, so every remaining bindgen struct that embeds a spinlock
// by value (and every `crate::bindings::spinlock_t` path across the
// crate) resolves here. Field names/types stay exactly as bindgen had
// them so the static-initializer literals in `uart.rs`/`console.rs`/
// `sync/sync.rs` keep compiling unchanged.
//
// Note the C typedef's `__ALIGNED_CACHELINE` (`__attribute__((aligned(64)))`)
// is attached to the *typedef name* `spinlock_t`, not to `struct
// spinlock` itself — GCC gives the typedef alignment 64 but leaves
// sizeof at 24 (verified by cross-compiler probe: `_Alignof(spinlock_t)
// == 64`, `sizeof(spinlock_t) == 24`, `_Alignof(struct spinlock) == 8`).
// Rust cannot express size 24 / align 64 (size must be a multiple of
// alignment), and bindgen resolved this the only layout-sound way:
// the record type stays 24/8 and each *embedding* struct picks up its
// own `#[repr(align(64))]` from clang's real C layout (member offsets
// are unaffected because a `spinlock_t` member is only ever placed at
// offset 0 or an already-8-aligned offset with sizeof 24). This native
// mirror therefore reproduces the 24/8 record layout, NOT align(64).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct RawSpinlock {
    pub(crate) locked: u32,
    pub(crate) name: *mut c_char,
    pub(crate) cpu: *mut cpu_local,
}

// P3-N2 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// spinlock { locked: uint, name: *mut c_char, cpu: *mut cpu_local }`,
// no align attribute) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe against
// `kernel/inc/lock/spinlock.h` (size 24, align 8, offsets 0/8/16).
const _: () = {
    assert!(core::mem::size_of::<RawSpinlock>() == 24, "struct spinlock: u32 + pad + 2 pointers");
    assert!(core::mem::align_of::<RawSpinlock>() == 8, "struct spinlock: natural alignment (the aligned(64) rides the typedef only)");
    assert!(core::mem::offset_of!(RawSpinlock, locked) == 0, "spinlock.locked offset");
    assert!(core::mem::offset_of!(RawSpinlock, name) == 8, "spinlock.name offset");
    assert!(core::mem::offset_of!(RawSpinlock, cpu) == 16, "spinlock.cpu offset");
};

// ---------------------------------------------------------------------------
// Externs: the few C helpers we still rely on.
// ---------------------------------------------------------------------------
// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
// KERNEL-OO: reached via the `Printf` import above (`panic_state` moved
// to the same type).

unsafe extern "C" {
}


// ---------------------------------------------------------------------------
// Panic-message strings — keep the panic-message formatting strings
// here so they live alongside the call sites.
// ---------------------------------------------------------------------------
static MSG_REENTRY: &[u8] = b"spin_lock reentry\0";
static MSG_UNLOCK: &[u8] = b"spin_unlock\0";

impl RawSpinlock {
    // -----------------------------------------------------------------
    // Centralised raw-pointer / atomic accessors on `spinlock_t`.
    //
    // Each helper performs exactly one field access through the raw
    // `*mut RawSpinlock` (the native mirror of `spinlock_t`) and
    // confines the resulting `unsafe` block to one line. Every public
    // entry point below casts its incoming `*mut spinlock_t` to
    // `*mut RawSpinlock` exactly once, so these helpers themselves
    // need no further casting. Raw-pointer parameter kept (NOT
    // `&self`): see the module doc's freeze-noalias note.
    // -----------------------------------------------------------------

    #[inline(always)]
    fn locked_atomic<'a>(lk: *mut RawSpinlock) -> &'a AtomicU32 {
        // SAFETY: caller provides a valid `*mut RawSpinlock`; the `locked`
        // field has the same layout and alignment as `AtomicU32`.
        unsafe { &*((&raw mut (*lk).locked) as *const AtomicU32) }
    }

    #[inline(always)]
    fn cpu_atomic<'a>(lk: *mut RawSpinlock) -> &'a AtomicPtr<cpu_local> {
        // SAFETY: see `Self::locked_atomic`. `*mut cpu_local` has the same
        // layout as `AtomicPtr<cpu_local>`.
        unsafe { &*((&raw mut (*lk).cpu) as *const AtomicPtr<cpu_local>) }
    }

    #[inline(always)]
    fn read_name(lk: *mut RawSpinlock) -> *mut c_char {
        // SAFETY: caller provides a valid `*mut RawSpinlock`.
        unsafe { (*lk).name }
    }

    #[inline(always)]
    fn set_name(lk: *mut RawSpinlock, name: *mut c_char) {
        // SAFETY: see `Self::read_name`. Exclusive at init time.
        unsafe {
            (*lk).name = name;
        }
    }

    #[inline(always)]
    fn set_locked_plain(lk: *mut RawSpinlock, v: u32) {
        // SAFETY: init-time only; no concurrent access yet.
        unsafe {
            (*lk).locked = v;
        }
    }

    #[inline(always)]
    fn set_cpu_plain(lk: *mut RawSpinlock, p: *mut cpu_local) {
        // SAFETY: init-time only.
        unsafe {
            (*lk).cpu = p;
        }
    }

    /// Reinterpret the bindgen `*mut spinlock_t` as the native mirror.
    ///
    /// SAFETY: layout equivalence is proven at compile time by the
    /// assertions above; the caller provides a valid, non-dangling
    /// `*mut spinlock_t`.
    #[inline(always)]
    unsafe fn as_native(lk: *mut spinlock_t) -> *mut RawSpinlock {
        lk as *mut RawSpinlock
    }

    #[inline(never)]
    #[cold]
    fn deadlock_panic(name: *mut c_char) -> ! {
        Printf::__panic_start();
        crate::kprintln!(
            "spin_acquire: deadlock detected on lock {}",
            crate::printf::Cs(name)
        );
        Printf::__panic_end()
    }

    #[inline(never)]
    #[cold]
    fn fixed_msg_panic(msg: &'static [u8]) -> ! {
        Printf::__panic_start();
        crate::kprintln!("{}", crate::printf::Cs(msg.as_ptr() as *const c_char));
        Printf::__panic_end()
    }

    // -----------------------------------------------------------------
    // Internal: is the current hart already the recorded owner of `lk`?
    // -----------------------------------------------------------------
    #[inline]
    fn holding(lk: *mut RawSpinlock) -> bool {
        Self::cpu_atomic(lk).load(Ordering::Acquire) == CpuLocal::current().as_ptr()
    }

    // -----------------------------------------------------------------
    // Slow path of `acquire`: contention loop with panic-state checks.
    // -----------------------------------------------------------------
    #[inline(never)]
    fn acquire_contended(lk: *mut RawSpinlock) {
        let locked = Self::locked_atomic(lk);
        let mut count: u64 = 0;
        // `TICK_S` is `__timebase_frequency` in the C original; the
        // deadlock threshold is `TICK_S * 100`. The volatile read is
        // centralised in `machine::tick_s`.
        let deadlock_threshold = machine::Riscv::tick_s().saturating_mul(100);

        loop {
            if locked.swap(1, Ordering::Acquire) == 0 {
                return;
            }
            count = count.wrapping_add(1);
            if count >= 10 {
                Riscv::cpu_relax();
            }
            if (count & 0xFFFF) == 0 {
                let mut cpu = CpuLocal::current();
                if (cpu.flags() & CPU_FLAG_CRASHED) == 0 && Printf::panic_state() != 0 {
                    cpu.flags_or(CPU_FLAG_CRASHED);
                    Riscv::write_sie(SIE_SSIE);
                    Riscv::intr_on();
                    loop {
                        Riscv::wfi();
                    }
                }
            }
            if count >= deadlock_threshold {
                let cpu = CpuLocal::current();
                if (cpu.flags() & CPU_FLAG_CRASHED) == 0 {
                    Self::deadlock_panic(Self::read_name(lk));
                }
            }
        }
    }

    // ===================================================================
    // Public ABI — matches `kernel/inc/defs.h` prototypes.
    // ===================================================================

    /// Initialise a spinlock. Mirrors C `spin_init`.
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now (directly, via `cffi::raw`'s typed safe wrappers, or
    // via per-file safe-facade adapters).
    pub unsafe fn init(lk: *mut spinlock_t, name: *mut c_char) {
        let lk = Self::as_native(lk);
        Self::set_name(lk, name);
        Self::set_locked_plain(lk, 0);
        Self::set_cpu_plain(lk, core::ptr::null_mut());
    }

    /// Spin until `lk` is acquired by the current hart. Caller must have
    /// interrupts disabled (the public entry point `lock` does that).
    pub(crate) unsafe fn acquire(lk: *mut spinlock_t) {
        if lk.is_null() || Self::holding(Self::as_native(lk)) {
            Self::fixed_msg_panic(MSG_REENTRY);
        }
        let lk = Self::as_native(lk);
        let locked = Self::locked_atomic(lk);
        // Fast path: try once before entering the contention loop.
        if locked.swap(1, Ordering::Acquire) != 0 {
            Self::acquire_contended(lk);
        }
        Self::cpu_atomic(lk).store(CpuLocal::current().as_ptr(), Ordering::Relaxed);
        CpuLocal::current().spin_depth_add(1);
        compiler_fence(Ordering::Acquire);
    }

    /// Release a spinlock previously acquired by this hart.
    pub(crate) unsafe fn release(lk: *mut spinlock_t) {
        if lk.is_null() || !Self::holding(Self::as_native(lk)) {
            Self::fixed_msg_panic(MSG_UNLOCK);
        }
        let lk = Self::as_native(lk);
        Self::cpu_atomic(lk).store(core::ptr::null_mut(), Ordering::Release);
        Self::locked_atomic(lk).store(0, Ordering::Release);
        CpuLocal::current().spin_depth_add(-1);
    }

    /// Try-acquire variant that disables interrupts internally.
    pub(crate) unsafe fn trylock(lk: *mut spinlock_t) -> c_int {
        Riscv::push_off();
        let lk = Self::as_native(lk);
        if Self::holding(lk) {
            Riscv::pop_off();
            return 0;
        }
        if Self::locked_atomic(lk).swap(1, Ordering::Acquire) != 0 {
            Riscv::pop_off();
            return 0;
        }
        Self::cpu_atomic(lk).store(CpuLocal::current().as_ptr(), Ordering::Relaxed);
        CpuLocal::current().spin_depth_add(1);
        compiler_fence(Ordering::Acquire);
        1
    }

    /// Returns 1 if the current hart holds `lk`, 0 otherwise. Interrupts
    /// must be disabled by the caller (the per-hart check uses `tp`).
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now (directly, via `cffi::raw`'s typed safe wrappers, or
    // via per-file safe-facade adapters).
    pub unsafe fn is_holding(lk: *mut spinlock_t) -> c_int {
        if Self::holding(Self::as_native(lk)) {
            1
        } else {
            0
        }
    }

    /// Default `spin_lock`: disables interrupts and acquires.
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now (directly, via `cffi::raw`'s typed safe wrappers, or
    // via per-file safe-facade adapters).
    pub unsafe fn lock(lk: *mut spinlock_t) {
        Riscv::push_off();
        Self::acquire(lk);
    }

    /// Default `spin_unlock`: releases and re-enables interrupts.
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now (directly, via `cffi::raw`'s typed safe wrappers, or
    // via per-file safe-facade adapters).
    pub unsafe fn unlock(lk: *mut spinlock_t) {
        Self::release(lk);
        Riscv::pop_off();
    }

    /// `lock` variant that saves the interrupt-enable flag.
    pub(crate) unsafe fn lock_irqsave(lk: *mut spinlock_t) -> c_int {
        let intena = Riscv::intr_off_save();
        Self::acquire(lk);
        intena
    }

    /// Companion of `lock_irqsave`.
    pub(crate) unsafe fn unlock_irqrestore(lk: *mut spinlock_t, intena: c_int) {
        Self::release(lk);
        Riscv::intr_restore(intena);
    }
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
    let status = RawSpinlock::is_holding(lk);
    if status != 0 {
        RawSpinlock::unlock(lk);
    }
    status
}

pub(crate) unsafe extern "C" fn spin_wake_cb(data: *mut c_void, sleep_cb_status: c_int) {
    if !data.is_null() && sleep_cb_status != 0 {
        let lk = data as *mut spinlock_t;
        RawSpinlock::lock(lk);
    }
}

// Re-export `machine::*` we touch so `#![allow(unused_imports)]` is not
// needed when the module set grows.
#[allow(dead_code)]
fn _link_ll_helpers() {
    let _ = Riscv::intr_get;
}
