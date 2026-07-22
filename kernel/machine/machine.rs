//! Low-level primitives — the *only* place in the mm crate where
//! `unsafe { core::arch::asm!(...) }` and per-CPU raw-pointer access
//! are written out by hand. Every other module calls into this one
//! through plain safe function calls; the `unsafe` keyword stays
//! confined here.
//!
//! Soundness rationale
//! -------------------
//!
//! * The inline-asm primitives use `options(nomem, nostack,
//!   preserves_flags)` and target RISC-V CSRs / general-purpose
//!   registers that are *defined* to be visible to the kernel at
//!   every reachable program point. Reading them cannot trap and
//!   cannot violate memory safety, so the wrappers are exposed as
//!   ordinary safe `fn`s.
//! * `intr_off()` / `intr_on()` toggle SSTATUS.SIE. They have a
//!   logical contract (must be balanced under `push_off`/`pop_off`)
//!   but no memory-safety contract: the CSR write itself cannot
//!   produce UB. Safe to expose.
//! * `cpu_local_ptr()` returns the `*mut cpu_local` stored in the
//!   `tp` register. The kernel boot path initialises `tp` to point
//!   at a unique, 64-byte-aligned, statically-allocated `cpu_local`
//!   slot before any Rust code runs and the value never aliases.
//!   Returning the raw pointer is therefore safe — callers that
//!   want to read or mutate fields go through the typed accessors
//!   in [`CpuLocal`] below, which encapsulate the necessary deref.
//!
//! Host-test seam
//! ---------------
//!
//! This is the crate's single concentrated point of `core::arch::asm!`
//! (see `kernel/lib.rs`'s "Host-test seam" doc section for the full
//! picture). Every primitive that is *directly* backed by an asm block
//! below is split `#[cfg(target_arch = "riscv64")]` (the real body,
//! unchanged) / `#[cfg(not(target_arch = "riscv64"))]` (a host mock) so
//! that `cargo test --target x86_64-unknown-linux-gnu` can compile this
//! module too. Each mock is documented at its definition; none of them
//! attempt to emulate real per-CPU/interrupt semantics — they return
//! fixed, canned values (or are no-ops), which is sufficient because no
//! `cfg(test)` code in this crate today calls into `machine` at all (the
//! two suites that exist, `mm::bits` and `mm::early_allocator`, don't
//! touch per-CPU state). `smp_mb` is the one exception: a real host
//! memory fence is just as cheap to provide as a no-op and is strictly
//! more useful should a future host suite ever need it.
//!
//! Everything else in this file (`CpuLocal`, `ThreadRef`, `VfsFileRef`,
//! the `list_entry_*`/`rb_node_init` pointer-surgery helpers, the
//! `atomic_*`/`smp_*` helpers) is ordinary portable Rust — plain pointer
//! dereferences and `core::sync::atomic` operations, nothing
//! architecture-specific — so none of it needs gating.

#![allow(dead_code)]

use core::ffi::c_int;

use crate::bindings::cpu_local;

const SSTATUS_SIE: u64 = 1 << 1;
const PGSIZE: u64 = 4096;

/// Zero-sized type gathering the riscv64 CSR / CPU machine-layer
/// primitives as associated functions. No `&self` receiver on any
/// of them: several are `Freeze`-adjacent hot-path primitives read
/// lock-free across harts (spin/wait loops in `lock/`), and forming
/// a `&Riscv` borrow would let LLVM's `noalias` hoist a load out of
/// such a loop -- see the spinlock pilot's identical rationale
/// (`kernel/lock/spinlock.rs`).
pub struct Riscv;

// ===========================================================================
// Inline-asm primitives
// ===========================================================================

impl Riscv {
    /// Read the `tp` (thread pointer) register.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn read_tp() -> u64 {
        let tp: u64;
        // SAFETY: `mv {0}, tp` reads a general-purpose register with no
        // memory effects and cannot trap.
        unsafe {
            core::arch::asm!("mv {0}, tp", out(reg) tp,
                options(nomem, nostack, preserves_flags));
        }
        tp
    }

    /// Host mock of [`read_tp`]: there is no riscv `tp`/per-CPU register on
    /// the host, and no `cfg(test)` code in this crate constructs a
    /// [`CpuLocal`] today, so this is a canned `0`. Documented here rather
    /// than silently guessed at, per the host-test seam's mocking policy.
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn read_tp() -> u64 {
        0
    }

    /// Read the `sp` (stack pointer) register.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn read_sp() -> u64 {
        let sp: u64;
        // SAFETY: `mv {0}, sp` reads a general-purpose register with no
        // memory effects and cannot trap.
        unsafe {
            core::arch::asm!("mv {0}, sp", out(reg) sp,
                options(nomem, nostack, preserves_flags));
        }
        sp
    }

    /// Host mock of [`read_sp`]: canned `0` (see [`read_tp`]'s mock doc — no
    /// host test reads this value today).
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn read_sp() -> u64 {
        0
    }

    /// Read SSTATUS.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn read_sstatus() -> u64 {
        let v: u64;
        // SAFETY: `csrr sstatus` reads a supervisor CSR that is always
        // readable at supervisor privilege; no memory effect, cannot trap.
        unsafe {
            core::arch::asm!("csrr {0}, sstatus", out(reg) v,
                options(nomem, nostack, preserves_flags));
        }
        v
    }

    /// Host mock of [`read_sstatus`]: canned `0` (SSTATUS.SIE reads as
    /// clear, i.e. [`intr_get`] reports "interrupts off" by default on the
    /// host — consistent with [`intr_off`]/[`intr_on`] below being no-ops
    /// that never actually flip a bit anywhere).
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn read_sstatus() -> u64 {
        0
    }

    /// Clear SSTATUS.SIE (disable supervisor interrupts).
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn intr_off() {
        // SAFETY: `csrc sstatus, x` clears bits in a supervisor CSR. The
        // operation itself cannot violate memory safety; correctness of
        // interrupt nesting is a logical (not memory) contract.
        unsafe {
            core::arch::asm!("csrc sstatus, {0}", in(reg) SSTATUS_SIE,
                options(nomem, nostack, preserves_flags));
        }
    }

    /// Host mock of [`intr_off`]: no-op. There is no interrupt-enable CSR on
    /// the host; combined with [`read_sstatus`]'s canned `0`, "interrupts"
    /// are simply always modeled as off.
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn intr_off() {}

    /// Set SSTATUS.SIE (enable supervisor interrupts).
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn intr_on() {
        // SAFETY: see `intr_off`.
        unsafe {
            core::arch::asm!("csrs sstatus, {0}", in(reg) SSTATUS_SIE,
                options(nomem, nostack, preserves_flags));
        }
    }

    /// Host mock of [`intr_on`]: no-op (see [`intr_off`]'s mock doc).
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn intr_on() {}

    /// `true` if SSTATUS.SIE is currently set.
    #[inline(always)]
    pub fn intr_get() -> bool {
        (Self::read_sstatus() & SSTATUS_SIE) != 0
    }

    /// Read the SIE bit, then disable interrupts if they were enabled.
    /// Mirrors the C `intr_off_save()` static inline.
    #[inline(always)]
    pub fn intr_off_save() -> core::ffi::c_int {
        let enabled = Self::intr_get();
        if enabled {
            Self::intr_off();
        }
        if enabled {
            1
        } else {
            0
        }
    }

    /// Re-enable interrupts if `enabled` is non-zero.
    #[inline(always)]
    pub fn intr_restore(enabled: core::ffi::c_int) {
        if enabled != 0 {
            Self::intr_on();
        }
    }

    /// Issue a `wfi` (wait-for-interrupt) instruction.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn wfi() {
        // SAFETY: `wfi` either stalls until an interrupt arrives or is
        // treated as a nop on harts that don't implement it; no memory
        // effect.
        unsafe {
            core::arch::asm!("wfi", options(nomem, nostack, preserves_flags));
        }
    }

    /// Host mock of [`wfi`]: no-op. There is no hart to idle on the host,
    /// and a real "block until interrupt" wait would just hang a test
    /// process forever with nothing to ever wake it.
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn wfi() {}
}

// ===========================================================================
// Per-CPU access
// ===========================================================================

impl Riscv {
    /// Return the address of the current hart's `cpu_local` slot.
    ///
    /// `tp` is initialised once per hart, before any Rust code runs, to
    /// the address of a unique slot inside a static array. Reading it is
    /// therefore safe; the returned pointer is always non-null and is
    /// exclusively owned by the current hart.
    #[inline(always)]
    pub fn cpu_local_ptr() -> *mut cpu_local {
        Self::read_tp() as *mut cpu_local
    }

    /// Compute the current hart's CPU index from `tp`, mirroring the C
    /// `cpuid()` macro: `((tp & PAGE_MASK) / sizeof(cpu_local))`.
    #[inline(always)]
    pub fn cpuid() -> c_int {
        let off = Self::read_tp() & (PGSIZE - 1);
        (off / core::mem::size_of::<cpu_local>() as u64) as c_int
    }
}

/// Typed reference to the current hart's `cpu_local` slot.
///
/// Construct via [`CpuLocal::current`]; the lifetime is bounded by
/// the borrow, so other code cannot smuggle the reference past a
/// context switch. The accessors below encapsulate the only field
/// dereferences the mm crate needs.
pub struct CpuLocal<'a> {
    raw: *mut cpu_local,
    _marker: core::marker::PhantomData<&'a mut cpu_local>,
}

impl<'a> CpuLocal<'a> {
    /// Borrow the current hart's per-CPU slot.
    ///
    /// Returned reference is valid for the rest of the current call
    /// frame: per-CPU storage is pinned for the kernel's lifetime, and
    /// the borrow tracks single-threaded access on this hart.
    #[inline(always)]
    pub fn current() -> Self {
        Self {
            raw: Riscv::cpu_local_ptr(),
            _marker: core::marker::PhantomData,
        }
    }

    /// Raw pointer escape hatch (kept for the few call sites that hand
    /// the pointer back to C).
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut cpu_local {
        self.raw
    }

    /// Nesting depth of `push_off` on this hart.
    #[inline(always)]
    pub fn noff(&self) -> c_int {
        // SAFETY: `raw` points at this hart's `cpu_local`, which is
        // pinned for the kernel's lifetime; the borrow on `self`
        // serialises access on this hart.
        unsafe { (*self.raw).noff }
    }

    #[inline(always)]
    pub fn set_noff(&mut self, n: c_int) {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).noff = n; }
    }

    /// Saved SIE bit captured by the outermost `push_off`.
    #[inline(always)]
    pub fn intena(&self) -> c_int {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).intena }
    }

    #[inline(always)]
    pub fn set_intena(&mut self, v: c_int) {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).intena = v; }
    }

    /// Pointer to the currently running thread on this hart.
    #[inline(always)]
    pub fn proc_(&self) -> *mut crate::bindings::thread {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).proc_ }
    }

    /// Read the current spinlock-nesting depth.
    #[inline(always)]
    pub fn spin_depth(&self) -> c_int {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).spin_depth }
    }

    /// Add `delta` to the spinlock-nesting depth.
    #[inline(always)]
    pub fn spin_depth_add(&mut self, delta: c_int) {
        // SAFETY: see `noff`.
        unsafe {
            (*self.raw).spin_depth += delta;
        }
    }

    /// Read the CPU flag bitmap.
    #[inline(always)]
    pub fn flags(&self) -> u64 {
        // SAFETY: see `noff`.
        unsafe { (*self.raw).flags }
    }

    /// OR `bits` into the CPU flag bitmap.
    #[inline(always)]
    pub fn flags_or(&mut self, bits: u64) {
        // SAFETY: see `noff`.
        unsafe {
            (*self.raw).flags |= bits;
        }
    }
}

impl Riscv {
    /// Hint to the CPU that we are in a busy-wait loop. Emits a `nop`
    /// with `"memory"` clobber, matching the C `cpu_relax()` macro.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn cpu_relax() {
        // SAFETY: `nop` has no side effects beyond the instruction stream.
        unsafe {
            core::arch::asm!("nop", options(nostack, preserves_flags));
        }
    }

    /// Host mock of [`cpu_relax`]: no-op. It is purely a CPU hint on riscv
    /// too (the `options(nostack, preserves_flags)` real body has no
    /// observable effect beyond a compiler memory-ordering barrier), so a
    /// plain no-op is behaviorally equivalent for anything that could run
    /// on the host.
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn cpu_relax() {}

    /// Full memory barrier, matching the kernel's `smp_mb()` macro.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn smp_mb() {
        // SAFETY: `fence rw, rw` orders memory operations; it cannot violate
        // Rust memory safety by itself.
        unsafe {
            core::arch::asm!("fence rw, rw", options(nostack, preserves_flags));
        }
    }

    /// Host mock of [`smp_mb`]: unlike the other mocks in this file, this
    /// one is a *real* implementation, not a canned value — a full
    /// `SeqCst` fence is the host's direct equivalent of riscv's `fence rw,
    /// rw`, is just as cheap to write as a no-op, and remains correct
    /// (indeed useful) for any future host test that exercises concurrency.
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn smp_mb() {
        core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
    }

    /// Write the `sie` (supervisor interrupt-enable) CSR.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn write_sie(v: u64) {
        // SAFETY: `csrw sie` writes a supervisor CSR; cannot violate
        // memory safety.
        unsafe {
            core::arch::asm!("csrw sie, {0}", in(reg) v,
                options(nomem, nostack, preserves_flags));
        }
    }

    /// Host mock of [`write_sie`]: no-op (there is no `sie` CSR on the
    /// host; see [`read_sie`]'s matching canned-`0` mock).
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn write_sie(_v: u64) {}

    /// Read the `sie` (supervisor interrupt-enable) CSR. Mirrors the C
    /// `r_sie()` inline.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn read_sie() -> u64 {
        let v: u64;
        // SAFETY: `csrr sie` reads a supervisor CSR; no memory effect,
        // cannot trap.
        unsafe {
            core::arch::asm!("csrr {0}, sie", out(reg) v,
                options(nomem, nostack, preserves_flags));
        }
        v
    }

    /// Host mock of [`read_sie`]: canned `0` (see [`write_sie`]'s mock doc).
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn read_sie() -> u64 {
        0
    }
}

/// `CPU_FLAG_CRASHED` bit, mirroring `kernel/inc/smp/percpu.h`.
pub const CPU_FLAG_CRASHED: u64 = 8;
/// `SIE_SSIE` bit (software), mirroring `kernel/inc/riscv.h`.
pub const SIE_SSIE: u64 = 1 << 1;
/// `SIE_STIE` bit (timer), mirroring `kernel/inc/riscv.h`.
pub const SIE_STIE: u64 = 1 << 5;
/// `SIE_SEIE` bit (external), mirroring `kernel/inc/riscv.h`.
pub const SIE_SEIE: u64 = 1 << 9;

impl Riscv {
    /// Write the `satp` (supervisor address translation & protection) CSR.
    /// Mirrors the C `w_satp()` inline. Writing `0` disables paging.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn write_satp(v: u64) {
        // SAFETY: writing `satp` cannot itself violate Rust memory safety;
        // it changes how subsequent loads/stores are translated, which is
        // why (matching the historical C site in `kernel/mm/vm_pgtab.rs`'s
        // private `w_satp`) this omits `nomem`: the compiler must not
        // reorder memory accesses across the mode switch as if the
        // instruction had no effect on memory semantics.
        unsafe {
            core::arch::asm!("csrw satp, {0}", in(reg) v,
                options(nostack, preserves_flags));
        }
    }

    /// Host mock of [`write_satp`]: no-op. There is no MMU/`satp` CSR on
    /// the host to program.
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn write_satp(_v: u64) {}

    /// Write the `stimecmp` CSR (deadline for the next supervisor-timer
    /// interrupt). Mirrors the C `w_stimecmp()` inline, which uses the raw
    /// CSR number `0x14d` because the mnemonic `stimecmp` is not recognised
    /// by the assembler on this toolchain.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn write_stimecmp(v: u64) {
        // SAFETY: `csrw 0x14d` writes the supervisor timer-compare CSR
        // (sstc extension, already enabled by firmware before S-mode code
        // runs); no memory effect, cannot trap.
        unsafe {
            core::arch::asm!("csrw 0x14d, {0}", in(reg) v,
                options(nomem, nostack, preserves_flags));
        }
    }

    /// Host mock of [`write_stimecmp`]: no-op (no timer CSR on the host).
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn write_stimecmp(_v: u64) {}

    /// Read the `time` CSR (mtime mirror). Mirrors the C `r_time()` inline.
    #[cfg(target_arch = "riscv64")]
    #[inline(always)]
    pub fn read_time() -> u64 {
        let v: u64;
        // SAFETY: `csrr time` reads an unprivileged-readable CSR; no
        // memory effect, cannot trap.
        unsafe {
            core::arch::asm!("csrr {0}, time", out(reg) v,
                options(nomem, nostack, preserves_flags));
        }
        v
    }

    /// Host mock of [`read_time`]: canned `0`. A real host time source
    /// (`std::time::Instant`, say) is deliberately not wired in here: `#
    /// ![no_std]` is only lifted for `cfg(test)` builds, and no test today
    /// calls this, so returning a fixed value keeps this mock's contract
    /// (and every timing computation built on top of it, e.g. `tick_ms`)
    /// simple and documented rather than silently time-dependent.
    #[cfg(not(target_arch = "riscv64"))]
    #[inline(always)]
    pub fn read_time() -> u64 {
        0
    }

    /// Sequentially-consistent store to a `thread`'s `state` field.
    /// Mirrors the C `__thread_state_set(p, state)` static inline.
    ///
    /// `state` is `thread_state` (a C enum, 4-byte). `core::sync::atomic`
    /// requires us to view the field as a 4-byte aligned integer.
    #[inline(always)]
    pub fn thread_state_set(p: *mut crate::bindings::thread,
                            state: crate::bindings::thread_state) {
        if p.is_null() { return; }
        // SAFETY: `p` is a valid (non-null) thread pointer; the `state`
        // field is naturally aligned (4 bytes). The cast to `AtomicU32`
        // is sound for a `c_uint`-sized field. The store uses SeqCst,
        // matching the C `__atomic_store_n(..., __ATOMIC_SEQ_CST)`.
        unsafe {
            let sp = core::ptr::addr_of_mut!((*p).state)
                as *mut core::sync::atomic::AtomicU32;
            (*sp).store(state as u32, core::sync::atomic::Ordering::SeqCst);
        }
    }
}

// ===========================================================================
// Typed pointer wrappers for FFI structs
// ===========================================================================
//
// These wrap a non-null `*mut T` coming back from the C kernel and
// expose just the fields the mm crate needs. Each accessor pays the
// `unsafe { (*p).field }` cost *once*, inside the wrapper, instead
// of at every call site.
//
// The lifetime parameter (`'a`) is a phantom borrow tying the
// reference to the local scope: callers cannot smuggle the wrapper
// past the function frame.

use core::marker::PhantomData;
use core::ptr::NonNull;

/// Borrow a C-supplied `*mut thread` for the duration of a call frame.
pub struct ThreadRef<'a> {
    raw: NonNull<crate::bindings::thread>,
    _marker: PhantomData<&'a mut crate::bindings::thread>,
}

impl<'a> ThreadRef<'a> {
    /// `None` if the FFI pointer is null.
    ///
    /// SAFETY contract (paid by the C caller): the pointer is either
    /// null or valid + exclusively owned for the duration of the call
    /// frame; no concurrent mutation occurs from other harts.
    #[inline(always)]
    pub fn from_raw(p: *mut crate::bindings::thread) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _marker: PhantomData })
    }

    #[inline(always)]
    pub fn as_ptr(&self) -> *mut crate::bindings::thread {
        self.raw.as_ptr()
    }

    #[inline(always)]
    pub fn trapframe(&self) -> *mut crate::bindings::utrapframe {
        // SAFETY: validity is the FFI contract documented on `from_raw`.
        unsafe { (*self.raw.as_ptr()).trapframe }
    }

    #[inline(always)]
    pub fn vm_ptr(&self) -> *mut core::ffi::c_void {
        // SAFETY: see `trapframe`.
        unsafe { (*self.raw.as_ptr()).vm as *mut core::ffi::c_void }
    }

    #[inline(always)]
    pub fn fdtable(&self) -> *mut core::ffi::c_void {
        // SAFETY: see `trapframe`.
        unsafe { (*self.raw.as_ptr()).fdtable as *mut core::ffi::c_void }
    }
}

impl Riscv {
    /// Resolve the currently running thread on this hart, honouring the
    /// interrupt-disable invariant the C `__current_thread()` macro relies
    /// on. Returns the raw pointer (callers usually wrap it in a
    /// [`ThreadRef`] right away).
    #[inline]
    pub fn current_thread_ptr() -> *mut crate::bindings::thread {
        let old = Self::intr_get();
        if old {
            Self::intr_off();
        }
        let cpu = CpuLocal::current();
        let cur = cpu.proc_();
        if old {
            Self::intr_on();
        }
        cur
    }

    /// Read `(*p).pid`, returning `-1` for a null pointer.
    ///
    /// Centralises the `unsafe { (*thread).pid }` access used by the
    /// lock primitives (mutex / rwsem / completion). Treats `IS_ERR`
    /// pointers (top-bit-set, magnitude ≤ 4095) as `-1` to mirror the
    /// C wakeup paths that defensively skip error returns.
    #[inline]
    pub fn thread_pid(p: *mut crate::bindings::thread) -> core::ffi::c_int {
        if p.is_null() {
            return -1;
        }
        let n = p as isize;
        if n < 0 && ((-n) as usize) <= 4095 {
            return -1;
        }
        // SAFETY: validated non-null, non-error thread pointer; `pid` is
        // a plain `c_int` field of a structurally-valid thread.
        unsafe { (*p).pid }
    }
}

// ===========================================================================
// Kernel timebase
// ===========================================================================

// Runtime-initialised timebase frequency (Hz). Set once at boot before
// any SMP activity; never changes afterwards. P3-D3c: plain crate-path
// import instead of an `extern "C"` redeclaration (demoted from
// `#[no_mangle]` in the same wave).
use crate::timer::timer_core::__timebase_frequency;

impl Riscv {
    /// `__timebase_frequency / 1000` — number of raw ticks in one
    /// millisecond. Centralises the volatile read so the lock primitives
    /// do not each carry their own copy.
    #[inline]
    pub fn tick_ms() -> u64 {
        // SAFETY: kernel-owned constant, read-only after boot.
        let f = unsafe { core::ptr::read_volatile(&raw const __timebase_frequency) };
        f / 1000
    }

    /// Raw timebase frequency (ticks per second). Spinlock's deadlock
    /// detector uses this directly; everything else should prefer
    /// [`tick_ms`] or [`ms_to_rawticks`].
    #[inline]
    pub fn tick_s() -> u64 {
        // SAFETY: see `tick_ms`.
        unsafe { core::ptr::read_volatile(&raw const __timebase_frequency) }
    }

    /// Convert a millisecond count into raw ticks, saturating on
    /// overflow (mirrors the C `MS_TO_RAWTICKS` macro).
    #[inline]
    pub fn ms_to_rawticks(ms: u64) -> u64 {
        ms.saturating_mul(Self::tick_ms())
    }
}

// ===========================================================================
// Intrusive doubly-linked-list primitives
// ===========================================================================
//
// The C kernel uses a single `list_node_t { prev, next }` struct
// embedded inside larger payloads. Every traversal in vm.rs / page.rs
// touches these two raw pointer fields. Wrapping the field access
// here pays the `unsafe { (*p).field }` cost once.

use crate::bindings::list_node_t;

impl Riscv {
    /// Read the `next` link of an intrusive list entry.
    #[inline(always)]
    pub fn list_entry_next(entry: *mut list_node_t) -> *mut list_node_t {
        // SAFETY: caller passes a valid FFI entry pointer (the entry is
        // embedded in a live payload owned by the caller).
        unsafe { (*entry).next }
    }

    /// Read the `prev` link of an intrusive list entry.
    #[inline(always)]
    pub fn list_entry_prev(entry: *mut list_node_t) -> *mut list_node_t {
        // SAFETY: see `list_entry_next`.
        unsafe { (*entry).prev }
    }

    /// Self-loop initialise a list entry so it satisfies the C
    /// `list_entry_init()` post-condition.
    #[inline]
    pub fn list_entry_init(entry: *mut list_node_t) {
        // SAFETY: entry is exclusively owned during init.
        unsafe {
            (*entry).next = entry;
            (*entry).prev = entry;
        }
    }

    /// Insert `new` immediately after `prev`. Mirrors the C `list.h`
    /// inline of the same name.
    #[inline]
    pub fn list_entry_insert_after(prev: *mut list_node_t, new: *mut list_node_t) {
        // SAFETY: caller holds the list's protecting lock; `prev`, `new`,
        // and `prev->next` are all valid for the duration of this call.
        unsafe {
            let nxt = (*prev).next;
            (*new).next = nxt;
            (*new).prev = prev;
            (*nxt).prev = new;
            (*prev).next = new;
        }
    }

    /// Detach `entry` from whichever list contains it and reinitialise
    /// it to a self-loop.
    #[inline]
    pub fn list_entry_detach(entry: *mut list_node_t) {
        // SAFETY: caller holds the protecting lock; `entry`, `entry->prev`
        // and `entry->next` are all valid.
        unsafe {
            let p = (*entry).prev;
            let n = (*entry).next;
            (*p).next = n;
            (*n).prev = p;
            (*entry).next = entry;
            (*entry).prev = entry;
        }
    }

    /// `true` if `entry` is in a self-loop (detached) state.
    #[inline]
    pub fn list_entry_is_detached(entry: *const list_node_t) -> bool {
        // SAFETY: entry is a valid FFI pointer.
        unsafe { (*entry).next as *const _ == entry && (*entry).prev as *const _ == entry }
    }

    /// `container_of`: subtract the member byte-offset from `entry` to
    /// recover a pointer to the containing payload of type `T`.
    #[inline(always)]
    pub fn list_container_of<T>(entry: *mut list_node_t, member_offset: usize) -> *mut T {
        (entry as *mut u8).wrapping_sub(member_offset) as *mut T
    }
}

// ===========================================================================
// rb_node initialisation
// ===========================================================================

impl Riscv {
    /// Self-colour an rb-tree node, mirroring the C `rb_node_init()`
    /// static inline: `node->__parent_color = (unsigned long)node`.
    #[inline]
    pub fn rb_node_init(node: *mut crate::bindings::rb_node) {
        // SAFETY: node is exclusively owned during init.
        unsafe {
            (*node).__parent_color = node as u64;
        }
    }
}

// ===========================================================================
// VFS-file accessors
// ===========================================================================

/// Borrow a `*mut vfs_file` for safe accessor methods.
pub struct VfsFileRef<'a> {
    raw: NonNull<crate::bindings::vfs_file>,
    _marker: PhantomData<&'a mut crate::bindings::vfs_file>,
}

impl<'a> VfsFileRef<'a> {
    #[inline(always)]
    pub fn from_raw(p: *mut crate::bindings::vfs_file) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _marker: PhantomData })
    }

    /// Address of the embedded `inode` slot, suitable for handing to
    /// the C `vfs_inode_deref()` helper.
    #[inline(always)]
    pub fn inode_slot_ptr(&self) -> *mut crate::bindings::vfs_inode_ref {
        // SAFETY: validity from FFI contract.
        unsafe { &raw mut (*self.raw.as_ptr()).inode }
    }
}

impl Riscv {
    /// Read `inode->mode` through a non-null inode pointer.
    #[inline(always)]
    pub fn vfs_inode_mode(p: *mut crate::bindings::vfs_inode) -> u32 {
        // SAFETY: caller has just retrieved this pointer from the FFI
        // and verified it is non-null.
        unsafe { (*p).mode as u32 }
    }

    /// Read `inode->size`. Signed loff_t in C.
    #[inline(always)]
    pub fn vfs_inode_size(p: *mut crate::bindings::vfs_inode) -> i64 {
        // SAFETY: caller verified non-null.
        unsafe { (*p).size as i64 }
    }

    /// Pointer to the embedded `pcache` inside a `vfs_inode`.
    #[inline(always)]
    pub fn vfs_inode_pcache_ptr(p: *mut crate::bindings::vfs_inode) -> *mut crate::bindings::pcache {
        // SAFETY: caller verified non-null.
        unsafe { &raw mut (*p).i_data }
    }
}

// P3-10a: the `vfs_file_ops(p)` accessor (read `file->ops` as a raw
// table pointer) was deleted with the table type itself — `file.ops` is
// `Option<&'static dyn crate::vfs::file::FileOps>` now and its (two)
// dispatch sites read the field directly. The accessor had no callers.

// ===========================================================================
// `vma` field reads (cheap inline accessors)
// ===========================================================================

impl Riscv {
    #[inline(always)]
    pub fn vma_start(v: *mut crate::bindings::vma) -> u64 { unsafe { (*v).start } }

    #[inline(always)]
    pub fn vma_end(v: *mut crate::bindings::vma) -> u64 { unsafe { (*v).end } }

    #[inline(always)]
    pub fn vma_flags(v: *mut crate::bindings::vma) -> u64 { unsafe { (*v).flags } }

    #[inline(always)]
    pub fn vma_file(v: *mut crate::bindings::vma) -> *mut crate::bindings::vfs_file {
        unsafe { (*v).file as *mut crate::bindings::vfs_file }
    }

    #[inline(always)]
    pub fn vma_pgoff(v: *mut crate::bindings::vma) -> u64 { unsafe { (*v).pgoff } }
}

// ===========================================================================
// PTE raw read / write
// ===========================================================================

impl Riscv {
    #[inline(always)]
    pub fn pte_read(p: *const u64) -> u64 { unsafe { *p } }

    #[inline(always)]
    pub fn pte_write(p: *mut u64, val: u64) { unsafe { *p = val; } }
}

// ===========================================================================
// `vm` / `vma` field-address accessors
// ===========================================================================
//
// Almost every list/rb-tree operation in vm.rs needs the *address* of
// a field embedded in a `*mut vm` or `*mut vma` (e.g.
// `&raw mut (*v).list_entry`). Computing the address only requires a
// valid base pointer (the field need not yet be initialised), so the
// helpers below collapse each `&raw mut (*p).field` into a single
// safe-callable accessor.

impl Riscv {
    #[inline(always)]
    pub fn vma_list_entry_ptr(v: *mut crate::bindings::vma) -> *mut list_node_t {
        // SAFETY: FFI in pointer is non-null and points at a `vma`.
        unsafe { &raw mut (*v).list_entry }
    }

    #[inline(always)]
    pub fn vma_free_list_entry_ptr(v: *mut crate::bindings::vma) -> *mut list_node_t {
        // SAFETY: see `vma_list_entry_ptr`.
        unsafe { &raw mut (*v).free_list_entry }
    }

    #[inline(always)]
    pub fn vma_rb_entry_ptr(v: *mut crate::bindings::vma) -> *mut crate::bindings::rb_node {
        // SAFETY: see `vma_list_entry_ptr`.
        unsafe { &raw mut (*v).rb_entry }
    }

    /// Read `vma->vm`. Returns null if the vma has already been detached.
    #[inline(always)]
    pub fn vma_vm_ptr(v: *mut crate::bindings::vma) -> *mut crate::bindings::vm {
        // SAFETY: see `vma_list_entry_ptr`.
        unsafe { (*v).vm as *mut crate::bindings::vm }
    }

    #[inline(always)]
    pub fn vm_list_ptr(v: *mut crate::bindings::vm) -> *mut list_node_t {
        // SAFETY: FFI in pointer is non-null and points at a `vm`.
        unsafe { &raw mut (*v).vm_list }
    }

    #[inline(always)]
    pub fn vm_free_list_ptr(v: *mut crate::bindings::vm) -> *mut list_node_t {
        // SAFETY: see `vm_list_ptr`.
        unsafe { &raw mut (*v).vm_free_list }
    }

    #[inline(always)]
    pub fn vm_rw_lock_ptr(v: *mut crate::bindings::vm) -> *mut crate::bindings::rwsem_t {
        // SAFETY: see `vm_list_ptr`.
        unsafe { &raw mut (*v).rw_lock }
    }
}

// ===========================================================================
// push_off / pop_off — duplicate of the C macros, written without any
// visible `unsafe` keyword in the body.
// ===========================================================================
impl Riscv {
    /// Disable interrupts and increment the nesting depth, mirroring the
    /// C `push_off()` macro byte-for-byte. Safe-callable.
    #[inline]
    pub fn push_off() {
        let old = Self::intr_get();
        if old {
            Self::intr_off();
        }
        let mut c = CpuLocal::current();
        let n = c.noff();
        if n == 0 {
            c.set_intena(if old { 1 } else { 0 });
        }
        c.set_noff(n + 1);
    }

    /// Decrement the nesting depth and re-enable interrupts when the
    /// outermost frame pops, mirroring the C `pop_off()` macro.
    #[inline]
    pub fn pop_off() {
        let mut c = CpuLocal::current();
        let n = c.noff() - 1;
        c.set_noff(n);
        if n == 0 && c.intena() != 0 {
            Self::intr_on();
        }
    }
}

// ===========================================================================
// PreemptGuard — RAII scope for push_off / pop_off
// ===========================================================================

/// RAII wrapper around [`push_off`] / [`pop_off`]. Construction disables
/// preemption (and interrupts on the outermost frame); the matching
/// `pop_off` runs on `Drop`, so every control-flow path that leaves the
/// guard's scope — early returns, `?`, panics — restores the previous
/// state automatically.
///
/// The type is `!Send` and `!Sync` because the push/pop counter lives
/// in the originating CPU's per-cpu area; smuggling the guard to
/// another hart would underflow that CPU's counter and overflow the
/// other's.
#[must_use = "PreemptGuard re-enables preemption when dropped — bind it to a name"]
pub struct PreemptGuard {
    _not_send: core::marker::PhantomData<*const ()>,
}

impl PreemptGuard {
    /// Disable preemption for the lifetime of the returned guard.
    #[inline]
    pub fn new() -> Self {
        Riscv::push_off();
        Self { _not_send: core::marker::PhantomData }
    }

    /// CPU id, valid only while this guard is alive (preemption off).
    #[inline]
    pub fn cpuid(&self) -> usize {
        Riscv::cpuid() as usize
    }
}

impl Drop for PreemptGuard {
    #[inline]
    fn drop(&mut self) {
        Riscv::pop_off();
    }
}

// ===========================================================================
// Atomic and SMP operations
// ===========================================================================

impl Riscv {
    #[inline(always)]
    pub fn atomic_load_acquire_i32(p: *const core::ffi::c_int) -> core::ffi::c_int {
        // SAFETY: caller provides a valid, aligned `*const c_int`; `c_int`
        // (4-byte, 4-byte-aligned on this target) has the same size and
        // alignment as `AtomicI32`, so the reinterpret cast is layout-sound
        // (cf. `lock/spinlock.rs`'s `locked_atomic`/`cpu_atomic` helpers).
        unsafe { (*(p as *const core::sync::atomic::AtomicI32)).load(core::sync::atomic::Ordering::Acquire) }
    }

    #[inline(always)]
    pub fn atomic_store_release_i32(p: *mut core::ffi::c_int, v: core::ffi::c_int) {
        // SAFETY: caller provides a valid, aligned `*mut c_int`; same
        // layout justification as `atomic_load_acquire_i32`.
        unsafe { (*(p as *mut core::sync::atomic::AtomicI32)).store(v, core::sync::atomic::Ordering::Release) }
    }

    #[inline(always)]
    pub fn atomic_inc_i32(p: *mut core::ffi::c_int) {
        // SAFETY: caller provides a valid, aligned `*mut c_int`; same
        // layout justification as `atomic_load_acquire_i32`.
        unsafe { (*(p as *mut core::sync::atomic::AtomicI32)).fetch_add(1, core::sync::atomic::Ordering::AcqRel); }
    }

    #[inline]
    pub fn atomic_dec_unless_i32(p: *mut core::ffi::c_int, unless: core::ffi::c_int) -> bool {
        // SAFETY: caller provides a valid, aligned `*mut c_int`; same
        // layout justification as `atomic_load_acquire_i32`. The resulting
        // reference is used only for the atomic ops below, for the
        // duration of this call.
        let a = unsafe { &*(p as *mut core::sync::atomic::AtomicI32) };
        let mut cur = a.load(core::sync::atomic::Ordering::Acquire);
        loop {
            if cur == unless { return false; }
            match a.compare_exchange_weak(cur, cur - 1, core::sync::atomic::Ordering::AcqRel, core::sync::atomic::Ordering::Acquire) {
                Ok(_) => return true,
                Err(v) => cur = v,
            }
        }
    }

    #[inline(always)]
    pub fn atomic_store_release_u64(p: *mut u64, v: u64) {
        // SAFETY: caller provides a valid, aligned `*mut u64`; `u64` has
        // the same size and alignment as `AtomicU64`, so the reinterpret
        // cast is layout-sound (cf. `lock/spinlock.rs`'s `locked_atomic`).
        unsafe { (*(p as *mut core::sync::atomic::AtomicU64)).store(v, core::sync::atomic::Ordering::Release) }
    }

    #[inline(always)]
    pub fn smp_load_acquire_i32(p: *const core::ffi::c_int) -> core::ffi::c_int {
        Self::atomic_load_acquire_i32(p)
    }

    #[inline(always)]
    pub fn smp_store_release_i32(p: *mut core::ffi::c_int, v: core::ffi::c_int) {
        Self::atomic_store_release_i32(p, v)
    }

    #[inline(always)]
    pub fn smp_load_acquire_state(p: *const crate::bindings::thread_state) -> crate::bindings::thread_state {
        Self::atomic_load_acquire_i32(p as *const core::ffi::c_int) as crate::bindings::thread_state
    }

    #[inline(always)]
    pub fn smp_store_release_state(p: *mut crate::bindings::thread_state, v: crate::bindings::thread_state) {
        Self::atomic_store_release_i32(p as *mut core::ffi::c_int, v as core::ffi::c_int)
    }

    #[inline(always)]
    pub fn smp_store_release_u64(p: *mut u64, v: u64) {
        Self::atomic_store_release_u64(p, v)
    }

    #[inline(always)]
    pub fn smp_load_acquire_u64(p: *const u64) -> u64 {
        // SAFETY: caller provides a valid, aligned `*const u64`; same
        // layout justification as `atomic_store_release_u64`.
        unsafe { (*(p as *const core::sync::atomic::AtomicU64)).load(core::sync::atomic::Ordering::Acquire) }
    }

    #[inline(always)]
    pub fn smp_rmb() {
        core::sync::atomic::fence(core::sync::atomic::Ordering::Acquire);
    }
}

// ===========================================================================
// Host-test seam smoke tests
// ===========================================================================
//
// These exist to prove the `cfg(target_arch)` seam above actually compiles
// and behaves as documented on a host build -- they are deliberately narrow
// (canned-value/no-op checks only) and do not attempt to exercise
// `CpuLocal`/`ThreadRef`/the per-CPU accessors: those dereference whatever
// `read_tp()` returns, which the host mock defines as `0` (a null pointer,
// not a real `cpu_local`) precisely because no `cfg(test)` suite needs a
// working per-CPU slot today (see the module doc's "Host-test seam"
// section). Exercising the real per-CPU accessors on the host is left to
// whichever future suite actually needs it (a later phase).
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_mock_register_reads_return_the_documented_canned_zero() {
        assert_eq!(Riscv::read_tp(), 0);
        assert_eq!(Riscv::read_sp(), 0);
        assert_eq!(Riscv::read_sstatus(), 0);
        assert_eq!(Riscv::read_sie(), 0);
        assert_eq!(Riscv::read_time(), 0);
    }

    #[test]
    fn host_mock_intr_toggle_is_a_noop_and_intr_get_reports_off() {
        // Arrange/Act: flip both ways: since the host mock never sets any
        // real bit, `intr_get()` must report "off" no matter what order
        // these are called in.
        Riscv::intr_on();
        Riscv::intr_off();
        assert!(!Riscv::intr_get());

        Riscv::intr_off();
        Riscv::intr_on();
        assert!(!Riscv::intr_get());
    }

    #[test]
    fn host_mock_intr_off_save_and_restore_do_not_panic() {
        let saved = Riscv::intr_off_save();
        Riscv::intr_restore(saved);
    }

    #[test]
    fn host_mock_wfi_and_cpu_relax_return_immediately() {
        // Regression guard: on riscv, `wfi` can legitimately block until an
        // interrupt arrives. The host mock must be a plain no-op, or this
        // test would hang forever instead of completing.
        Riscv::wfi();
        Riscv::cpu_relax();
    }

    #[test]
    fn host_mock_smp_mb_is_a_real_fence_and_does_not_panic() {
        // `smp_mb`'s host mock is the one exception that is a genuine
        // implementation rather than a canned value (see its doc) --
        // there's no single-threaded observable to assert on beyond "it
        // runs to completion".
        Riscv::smp_mb();
    }

    #[test]
    fn host_mock_csr_writes_accept_any_value_and_are_noops() {
        Riscv::write_sie(SIE_SSIE | SIE_STIE | SIE_SEIE);
        Riscv::write_satp(0);
        Riscv::write_stimecmp(u64::MAX);
        // Nothing to assert beyond "compiles and returns" -- these are
        // documented no-ops with no host-observable CSR to read back.
    }
}
