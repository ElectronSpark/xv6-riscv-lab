//! Inter-processor interrupts (IPI) + per-CPU bring-up bookkeeping — Rust
//! port of `kernel/ipi/ipi.c` (Phase 2 Wave 9, see
//! `docs/rustify/phase2_plan.md`).
//!
//! IPIs are delivered as RISC-V supervisor software interrupts (`scause`
//! low bits == [`IRQ_S_SOFT`]). Sending is a two-step publish: OR a reason
//! bit into the target hart's [`IPI_PENDING`] slot, then trigger the
//! interrupt via `sbi_send_ipi` (`kernel/sbi.rs`, Wave 2). The receiving
//! hart's registered handler ([`ipi_irq_handler`]) drains all pending
//! reason bits in one atomic exchange and dispatches each.
//!
//! This file also owns [`cpus`] — the `struct cpu_local cpus[NCPU]`
//! array — because the C original put it here (`ipi_init`/`cpus_init`/
//! `mycpu_init` are all in `ipi.c`, sharing the file with the IPI
//! mailbox), not because it is conceptually IPI machinery. Four other
//! Rust files hold their own typed `extern "C" { #[link_name = "cpus"]
//! ... }` declarations of this exact symbol (`proc/rq.rs`, `lock/rcu.rs`,
//! `irq/trap.rs`, `mm/vm_pgtab.rs`, none touched by this wave) — the
//! storage below must keep the same symbol name, section, size, and
//! alignment or all four break.
//!
//! # Interrupt-context audit (Wave 9 deliverable)
//!
//! | Function | Context | Sleep/alloc? |
//! |---|---|---|
//! | [`ipi_init`] | boot hart, thread context, single-threaded (before any other hart is up) | no |
//! | [`ipi_send_single`]/[`ipi_send_mask`]/[`ipi_send_all_but_self`]/[`ipi_send_all`] | **sender-side, any context** (thread *or* interrupt, any hart) — called today from `proc/sched.rs`/`proc/signal.rs`/`proc/thread_group.rs` (thread context, holding rq locks) and `printf.rs`'s panic path (any context, including nested interrupt/panic) | no — pure atomic RMW + one SBI `ecall` |
//! | [`ipi_irq_handler`] | **receiver-side, hard interrupt context on the remote hart**, dispatched from `irq::irq_core::do_irq` with interrupts disabled and an `rcu_read_lock()` held (see that function's doc) | no — see per-case breakdown below |
//! | [`cpus_init`]/[`mycpu_init`] | boot-time only (`start_kernel.c`, both the boot hart's own path and every secondary hart's `__start_kernel_secondary_hart`), thread context | no |
//! | [`get_cpu_active_mask`] | any context (pure atomic load) | no |
//!
//! [`ipi_irq_handler`]'s five reason cases, individually:
//! * `IPI_REASON_CRASH` — `panic_msg_lock()`/`panic_msg_unlock()` are a
//!   spinlock (`kernel/printf.rs`), not a sleeping lock; `print_backtrace`
//!   is documented panic-safe/alloc-free (`kernel/backtrace.rs`); the case
//!   ends in an infinite `wfi()` loop and never returns — matches the C.
//! * `IPI_REASON_CALL_FUNC` — unimplemented in the C original (empty
//!   case), unchanged here.
//! * `IPI_REASON_RESCHEDULE` — calls `rq_flush_wake_list` (`proc/rq.rs`,
//!   documented there as IRQ-safe: it only walks an intrusive list under
//!   a spinlock and does not sleep/allocate) and sets a CPU flag bit; no
//!   sleep/alloc.
//! * `IPI_REASON_TLB_FLUSH` — empty case in the C (comment: xv6 flushes
//!   the TLB on the next return to user mode instead), unchanged here.
//! * `IPI_REASON_GENERIC` — empty case, unchanged here.
//!
//! No case sleeps, allocates, or takes a non-reentrant/blocking lock.
//!
//! # Atomic ordering map
//!
//! | Site | C (`__ATOMIC_*`) | Rust | Rationale |
//! |---|---|---|---|
//! | [`IPI_PENDING`] set (`ipi_send_single`/`_mask`/`_all`) | `atomic_or` = `SEQ_CST` | `Release` | Publication flag: the sender needs the reason bit (and, transitively, any data the reason handler reads, e.g. the wake-list state `IPI_REASON_RESCHEDULE`'s handler reads) to be visible to whichever hart later drains it. The RMW's own atomicity — no lost bits when two senders OR different reasons into the same slot concurrently — holds regardless of ordering; only the *visibility* half needs Release. Nothing downstream in the sender depends on the (discarded) old value, so no Acquire component is needed. |
//! | [`IPI_PENDING`] drain (`ipi_irq_handler`, `swap(0, ..)`) | `__atomic_exchange_n(.., ACQ_REL)` | `Acquire` | Must pair with the senders' `Release` to observe everything published before the bit was set. The store half (publishing "now empty") has no reader — no other hart inspects a target's `ipi_pending` — so the C's `Release` component is provably unnecessary; kept as `Acquire` only, the weakest correct choice. |
//! | [`IPI_PENDING`] boot-time clear (`ipi_init`) | `smp_store_release` | `Release`-equivalent `store` (kept, see note) | Provably unnecessary today: `ipi_init` runs once, on the boot hart, before any secondary hart is brought up (`start_kernel.c`'s init order) and before the handler is even registered — no concurrent reader exists yet. Kept at `Release` rather than downgraded to `Relaxed` purely for fidelity/documentation-of-intent (matches the array's general "this is a publication site" pattern used everywhere else in the file); a future caller of `ipi_init` from a hotter path would need to re-derive this, so the rationale is spelled out rather than silently assumed. |
//! | [`CPU_ACTIVE_MASK`] set (`mycpu_init`) | `atomic_or` = `SEQ_CST` | `Release` | Same publication-flag shape as `proc/rq.rs`'s `active_cpu_mask` (see that file's Package-B analysis, a different variable with an identical pattern): once a hart's bit is visible, other code (`get_cpu_active_mask`/`cpu_for_each_active`) assumes that hart's `cpu_local` slot and boot-time setup are complete. |
//! | [`CPU_ACTIVE_MASK`] read (`get_cpu_active_mask`) | `smp_load_acquire` | `Acquire` | Pairs with the `Release` above. |
//!
//! No site here needs `SeqCst`: every one is a one-directional
//! publish/observe pair (set-then-signal, or set-then-poll), never a
//! total-order requirement across multiple independent atomics.
//!
//! # Two latent C bugs found and neutralized (both dead code today)
//!
//! * `ipi_send_mask`'s per-hart loop indexed `ipi_pending[i +
//!   hart_mask_base]` with **no bound check on the sum** — passing a
//!   `hart_mask_base` that pushes `i + hart_mask_base >= NCPU` is an
//!   out-of-bounds write in the C (silent memory corruption, UB). Caller
//!   survey (full-tree grep, see the Wave 9 report): every live call site
//!   of `ipi_send_mask` — both of them, `ipi_send_all_but_self` and
//!   `ipi_send_all` below — passes `hart_mask_base = 0`, so this is
//!   unreachable in the current tree. The Rust port below uses
//!   `[AtomicU64].get(idx)` and silently skips out-of-range indices
//!   (extending the C doc comment's existing "bits out of bounds are
//!   ignored" contract to cover the shifted case too) rather than either
//!   reproducing the memory-corruption UB or introducing a new panic path
//!   for a value no live caller can ever produce.
//! * `ipi_send_all` OR'd `1UL << reason` into every hart's pending mask
//!   **before** validating `reason` (the validation only happens inside
//!   the `ipi_send_mask` call at the end) — a negative or
//!   out-of-range `reason` is a shift-by-invalid-amount (UB in C) on the
//!   direct-OR loop, still executed even though the trailing
//!   `ipi_send_mask` call would have rejected it. Caller survey: the
//!   single live call site (`printf.rs`'s panic path) always passes the
//!   compile-time constant `IPI_REASON_CRASH` (0). The Rust port
//!   validates `reason` up front, matching the sibling functions'
//!   already-established contract (`ipi_send_single`/`ipi_send_mask` both
//!   validate before mutating state).

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicU64, Ordering};

use crate::bindings::cpu_local;
use crate::machine;
// P3-5: `machine::CpuLocal` (the borrow-scoped accessor wrapper) is now
// referenced by qualified path — this file defines the native `struct
// cpu_local` under the same CamelCase name `CpuLocal` below (the N-wave
// naming convention), so the bare import would collide.
use crate::machine::{CPU_FLAG_CRASHED, SIE_SSIE};
// P3-1C mesh sweep: printf.rs is in scope for this wave; both are thin
// spinlock wrappers, unconditionally safe to call, so they become plain
// crate-path imports instead of `extern "C"` redeclarations.
use crate::printf::{panic_msg_lock, panic_msg_unlock};
// P3-1D mesh sweep: backtrace.rs/sbi.rs are in scope for this wave; both
// are thin/panic-safe calls, unconditionally safe or already-unsafe at
// their call sites here, so they become plain crate-path imports instead
// of `extern "C"` redeclarations.
use crate::backtrace::print_backtrace;
use crate::sbi::sbi_send_ipi;

// ===========================================================================
// Externs.
// ===========================================================================
//
// Per the wave's touch-scope rule, everything this file calls into that is
// owned by another (already-ported or off-limits) module is declared here
// rather than imported by path -- `sbi.rs` and `proc/rq.rs` are explicitly
// out of scope for edits in this wave.

// P3-D3c: `printf.rs`'s panic plumbing and `start_kernel.rs`'s
// FDT-configured kernel physical memory bounds
// (`kernel/inc/mm/memlayout.h`'s `KERNBASE`/`PHYSTOP` macros) are plain
// crate-path imports now that their `#[no_mangle]` exports are gone (the
// extern redeclarations that used to sit here are deleted). The bounds
// are `pub(crate) static mut`s; the read below sits in its own `unsafe`
// block with the usual boot-write-once justification.
use crate::printf::{__panic_end, __panic_start};
use crate::start_kernel::{__physical_memory_end, __physical_memory_start};

use crate::irq::irq_core::{register_irq_handler, IrqDesc};
// P3-D2a: `kernel/proc/rq.rs` (SECTION 19 of the former
// `proc_rust_shims.c`), now reached as a plain crate-path item instead
// of an `extern "C"` redeclaration. Documented there as IRQ-safe
// (spinlock-guarded intrusive-list splice, no sleep/alloc) -- see the
// module doc's interrupt-context audit table.
use crate::proc::rq_flush_wake_list;

// ===========================================================================
// Small ABI constants duplicated locally (same convention used throughout
// this crate for tiny values not worth a bindgen round-trip -- see e.g.
// `start.rs`'s `NCPU`/`KERNEL_STACK_SIZE`, `proc/rq.rs`'s `NCPU`).
// ===========================================================================

/// Mirrors `NCPU` in `kernel/inc/param.h`.
const NCPU: usize = 8;

/// `kernel/inc/smp/ipi.h`'s `IPI_REASON_*` / `NR_IPI_REASON`.
const IPI_REASON_CRASH: c_int = 0;
const IPI_REASON_CALL_FUNC: c_int = 1;
const IPI_REASON_RESCHEDULE: c_int = 2;
const IPI_REASON_TLB_FLUSH: c_int = 3;
const IPI_REASON_GENERIC: c_int = 4;
const NR_IPI_REASON: c_int = 5;

const _: () = assert!(NR_IPI_REASON <= 8, "ipi.h: BUILD_BUG_ON(NR_IPI_REASON > 8)");

/// IRQ number for the supervisor software interrupt (`kernel/ipi/ipi.c`'s
/// `#define IRQ_S_SOFT 1`).
const IRQ_S_SOFT: c_int = 1;

/// `kernel/inc/smp/percpu.h`'s `CPU_FLAG_NEEDS_RESCHED` bit. Only
/// `CPU_FLAG_CRASHED` is already public in `machine.rs`
/// ([`machine::CPU_FLAG_CRASHED`]); this one has no prior-wave home.
const CPU_FLAG_NEEDS_RESCHED: u64 = 1;

/// Mirrors `kernel/inc/types.h`'s `typedef uint64 cpumask_t;`.
type cpumask_t = u64;

#[inline]
fn neg(errno: u32) -> c_int {
    -(errno as c_int)
}

// ===========================================================================
// Small CSR/register primitives not covered by `machine.rs`. Local
// duplicates, same convention as `mm/vm_pgtab.rs`'s private `w_satp` and
// `printf.rs`'s private `read_fp` (see `RUST_REWRITE.md`'s Wave 3 note).
// ===========================================================================

/// `kernel/inc/riscv.h`'s `r_sip()`.
#[inline(always)]
fn read_sip() -> u64 {
    let x: u64;
    // SAFETY: `csrr sip` reads a supervisor CSR; no memory effect, cannot
    // trap.
    unsafe {
        core::arch::asm!("csrr {0}, sip", out(reg) x, options(nomem, nostack, preserves_flags));
    }
    x
}

/// `kernel/inc/riscv.h`'s `w_sip()`.
#[inline(always)]
fn write_sip(v: u64) {
    // SAFETY: `csrw sip` writes a supervisor CSR; the only bit this file
    // ever clears is `SIE_SSIE` (acknowledging the IPI we are currently
    // handling), which cannot itself violate memory safety.
    unsafe {
        core::arch::asm!("csrw sip, {0}", in(reg) v, options(nomem, nostack, preserves_flags));
    }
}

/// `kernel/inc/riscv.h`'s `r_fp()`. Same instruction/options as
/// `printf.rs`'s private `read_fp` -- duplicated per this crate's
/// established convention for tiny leaf CSR/register helpers.
#[inline(always)]
fn read_fp() -> u64 {
    let fp: u64;
    // SAFETY: `mv {0}, s0` reads a general-purpose register with no
    // memory effects and cannot trap.
    unsafe {
        core::arch::asm!("mv {0}, s0", out(reg) fp, options(nomem, nostack, preserves_flags));
    }
    fp
}

/// `kernel/inc/riscv.h`'s `w_tp()`. Writes the `tp` register directly --
/// after this call, `machine::read_tp`/`machine::cpuid`/`CpuLocal::current`
/// on this hart resolve against the new value.
#[inline(always)]
fn write_tp(v: u64) {
    // SAFETY: writes a general-purpose register with no memory access of
    // its own; it changes what `mycpu()` resolves to from this point on,
    // which is exactly `mycpu_init`'s documented purpose.
    unsafe {
        core::arch::asm!("mv tp, {0}", in(reg) v, options(nomem, nostack, preserves_flags));
    }
}

/// Mirrors `kernel/inc/bits.h`'s `bits_ctz8(x)`: trailing-zero count of the
/// low byte, or `-1` for a zero input (the macro's own zero-guard). Not a
/// ported `.c` file -- `bits.h` is header-only (macros/static-inline, no
/// `bits.c`) -- reimplemented locally like this crate's other tiny leaf
/// helpers.
#[inline]
fn ctz8(x: u8) -> i32 {
    if x == 0 {
        -1
    } else {
        x.trailing_zeros() as i32
    }
}

// ===========================================================================
// `cpus[]` -- kernel global array of per-CPU records.
//
// C: `__SECTION(cpu_local_sec) __ALIGNED_PAGE struct cpu_local cpus[NCPU]
// = {0};`. The explicit page alignment + dedicated linker-input section
// (`kernel.ld.in`'s `.trampoline.cpu_local` output section, itself
// page-aligned and asserted `<= 0x1000`) let `mm/vm_pgtab.rs::kvmmake`
// (not touched by this wave) `kvmmap` this exact physical page to the
// `TRAMPOLINE_CPULOCAL` virtual address computed in `mycpu_init` below --
// NCPU (8) * sizeof(cpu_local) (64, `#[repr(align(64))]` per the bindgen
// struct) = 512 bytes, comfortably under one page.
// ===========================================================================

// ===========================================================================
// Native layout — Wave P3-5 (the asm-offset-locked trio, part 3/3).
//
// This IS the kernel-wide Rust definition of `struct cpu_local`
// (`kernel/inc/smp/percpu_types.h`) now: `build.rs` blocklists the
// bindgen form and injects `pub use crate::ipi::CpuLocal as cpu_local;`,
// so every `crate::bindings::cpu_local` path across the crate (including
// this file's own import and `machine.rs`'s accessor wrapper) resolves
// here. NOTE the deliberate name split: `crate::machine::CpuLocal` is
// the borrow-scoped per-hart ACCESSOR wrapper; `crate::ipi::CpuLocal`
// (this type) is the per-CPU DATA record it points at — this file, the
// only place both are in scope, qualifies the accessor as
// `machine::CpuLocal`.
//
// DANGER ZONE — ASM CONTRACT: kernel/irq/kernelvec.S addresses this
// struct through `tp` using the `CPU_LOCAL_FLAGS`/`CPU_LOCAL_INTR_SP`
// macros of asm-offsets.h (interrupt-stack switch on trap entry); the
// per-hart `tp` spacing itself is `sizeof(struct cpu_local)` (`mycpu_
// init` below, and `machine::cpuid()`'s divide). The const asserts below
// cite the consumers; any drift fails the target build before an image
// can be linked.
//
// FIELD-NAME NOTE: the C field `proc` is spelled `proc_` here — bindgen
// renamed it (`proc` is on bindgen's reserved list) and every Rust
// consumer already reads `.proc_`; the asm-offsets.h define stays
// `CPU_LOCAL_PROC` (no .S consumer).
//
// Layout evidence (three-way agreement): the golden gcc-generated
// asm-offsets.h CPU_LOCAL_* values + the riscv64-unknown-elf-gcc
// `_Static_assert` probe (rv64gc/lp64d, scratchpad
// p3_5_static_assert_probe.c) + the pre-nativization bindgen output
// (`#[repr(C)] #[repr(align(64))]`, derives Copy/Clone). C attribute
// `__ALIGNED_CACHELINE` == align(64); fields end at 64, no tail pad.
//
// Derive fidelity: bindgen derived Copy/Clone; the native keeps them
// (the `ZERO_CPU_LOCAL` array-repeat seed below depends on Copy), and
// build.rs's `NativeTypeCallbacks` answers Copy=Yes (nothing
// still-bindgen embeds it by value — the only by-value context is the
// `CpuLocalArray` wrapper below, hand-written in this file).
// ===========================================================================

/// Native `struct cpu_local` (`kernel/inc/smp/percpu_types.h`) — the
/// per-CPU state record each hart's `tp` points at.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct CpuLocal {
    pub proc_: *mut crate::bindings::thread,
    pub idle_thread: *mut crate::bindings::thread,
    pub intr_stacks: *mut *mut c_void,
    pub intr_sp: u64,
    pub intr_depth: c_int,
    pub noff: c_int,
    pub spin_depth: c_int,
    pub intena: c_int,
    pub flags: u64,
    pub rcu_timestamp: u64,
}

// P3-5 hardcoded layout proof — every value equals the golden
// gcc-generated asm-offsets.h CPU_LOCAL_* define named in the message,
// independently confirmed by the toolchain-gcc `_Static_assert` probe
// and the pre-nativization bindgen output. `CPU_LOCAL_FLAGS` and
// `CPU_LOCAL_INTR_SP` are live kernelvec.S loads; the size assert
// guards the per-hart `tp` stride (kernelvec.S's `(tp)` addressing +
// `machine::cpuid()`); the rest are pinned for the static asm-offsets.h.
const _: () = {
    assert!(
        core::mem::size_of::<CpuLocal>() == 64,
        "CPU_LOCAL_SIZE (per-hart tp stride: kernelvec.S `(tp)` addressing, machine::cpuid())"
    );
    assert!(core::mem::align_of::<CpuLocal>() == 64, "cpu_local alignment (__ALIGNED_CACHELINE)");
    assert!(core::mem::offset_of!(CpuLocal, proc_) == 0, "CPU_LOCAL_PROC (C field `proc`)");
    assert!(core::mem::offset_of!(CpuLocal, idle_thread) == 8, "CPU_LOCAL_IDLE_THREAD");
    assert!(core::mem::offset_of!(CpuLocal, intr_stacks) == 16, "CPU_LOCAL_INTR_STACKS");
    assert!(
        core::mem::offset_of!(CpuLocal, intr_sp) == 24,
        "CPU_LOCAL_INTR_SP (kernelvec.S `ld a0, CPU_LOCAL_INTR_SP(tp)`)"
    );
    assert!(core::mem::offset_of!(CpuLocal, intr_depth) == 32, "CPU_LOCAL_INTR_DEPTH");
    assert!(core::mem::offset_of!(CpuLocal, noff) == 36, "CPU_LOCAL_NOFF");
    assert!(core::mem::offset_of!(CpuLocal, spin_depth) == 40, "CPU_LOCAL_SPIN_DEPTH");
    assert!(core::mem::offset_of!(CpuLocal, intena) == 44, "CPU_LOCAL_INTENA");
    assert!(
        core::mem::offset_of!(CpuLocal, flags) == 48,
        "CPU_LOCAL_FLAGS (kernelvec.S `ld a0, CPU_LOCAL_FLAGS(tp)`)"
    );
    assert!(core::mem::offset_of!(CpuLocal, rcu_timestamp) == 56, "CPU_LOCAL_RCU_TIMESTAMP");
};

/// Alignment-carrying wrapper -- `#[repr(align(N))]` cannot be attached
/// directly to a `static`'s array type, same reason `start.rs` wraps
/// `stack0` in a named `Stack0` struct.
#[repr(C, align(4096))]
pub struct CpuLocalArray([cpu_local; NCPU]);

/// All-zero `cpu_local`: every field is a pointer or plain integer, so the
/// all-zero bit pattern is a valid value (matches the C `= {0}`
/// initializer). `cpu_local` derives `Copy` (bindgen), so this can seed an
/// array-repeat expression.
const ZERO_CPU_LOCAL: cpu_local = unsafe { core::mem::zeroed() };

/// `struct cpu_local cpus[NCPU]` (see module doc: read by `proc/rq.rs`,
/// `lock/rcu.rs`, `irq/trap.rs`, `mm/vm_pgtab.rs` -- P3-D3c: all four now
/// reach it by crate path with their own locally-typed raw-pointer casts,
/// replacing their `extern`/`#[link_name]` redeclarations, so the
/// `#[no_mangle]` anchor is gone. The `#[link_section]` placement is what
/// the trampoline per-CPU mapping actually depends on and is unchanged).
#[link_section = "cpu_local_sec"]
pub(crate) static mut cpus: CpuLocalArray = CpuLocalArray([ZERO_CPU_LOCAL; NCPU]);

/// `kernel/ipi/ipi.c`'s `static cpumask_t cpu_active_mask = 0;` -- private
/// to this file both in C (never `extern`-declared anywhere) and here.
static CPU_ACTIVE_MASK: AtomicU64 = AtomicU64::new(0);

/// `kernel/ipi/ipi.c`'s `void cpus_init(void)`.
// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn cpus_init() {
    // SAFETY: `memset`s the whole `cpus` array to zero. Sound under the
    // same contract the C relied on: `start_kernel.c` calls this only
    // once, on the boot hart, before any hart (including the boot hart
    // itself) has written its `tp` to point into this array -- so there
    // is no concurrent reader/writer at the time of this call.
    unsafe {
        core::ptr::write_bytes(
            (&raw mut cpus) as *mut u8,
            0,
            core::mem::size_of::<CpuLocalArray>(),
        );
    }
}

/// Replicates the C `assert(expr, fmt, arg)` expansion for `mycpu_init`'s
/// one dynamic-argument assert. Same special-casing precedent as
/// `irq/trap.rs`'s `enter_irq` (its doc comment explains why a fixed
/// `kassert!`-style macro can't carry a runtime `%d`).
fn assert_hartid_in_range(hartid: u64) {
    if hartid < NCPU as u64 {
        return;
    }
    __panic_start();
    crate::kprintln!(
        "ASSERTION_FAILURE {}:{}: In function '{}':",
        "kernel/ipi.rs",
        line!() as c_int,
        "mycpu_init",
    );
    crate::kprintln!("mycpu_init: invalid hartid {}", hartid);
    __panic_end();
}

/// `kernel/ipi/ipi.c`'s `void mycpu_init(uint64 hartid, bool trampoline)`.
// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn mycpu_init(hartid: u64, trampoline: bool) {
    assert_hartid_in_range(hartid);
    // SEQ_CST fetch-or in the C (`atomic_or`); weakest-correct is
    // `Release` -- see the module doc's ordering-map table.
    CPU_ACTIVE_MASK.fetch_or(1u64 << hartid, Ordering::Release);

    // SAFETY: `cpus` is `#[repr(C)]` with the `[cpu_local; NCPU]` array as
    // its sole field at offset 0, so casting the whole static's address to
    // `*mut cpu_local` and indexing by `hartid` (just bounds-checked
    // above) is exactly `&cpus[hartid]`. Taking the address does not
    // dereference it.
    let cpu_ptr = unsafe { (&raw mut cpus as *mut cpu_local).add(hartid as usize) };

    if trampoline {
        // Convert physical address to virtual address in the trampoline
        // region: keep the in-page offset, but rebase onto
        // TRAMPOLINE_CPULOCAL.
        let offset = (cpu_ptr as u64) & PAGE_MASK;
        let c = TRAMPOLINE_CPULOCAL + offset;
        write_tp(c);
        crate::kprintln!(
            "hart {} mycpu_init: setting tp to {} - {}",
            hartid,
            crate::printf::Ptr(c),
            crate::printf::Ptr(c + core::mem::size_of::<cpu_local>() as u64),
        );
    } else {
        write_tp(cpu_ptr as u64);
        crate::kprintln!(
            "hart {} mycpu_init: setting tp to {} - {}",
            hartid,
            crate::printf::Ptr(cpu_ptr as u64),
            crate::printf::Ptr(cpu_ptr as u64 + core::mem::size_of::<cpu_local>() as u64),
        );
    }
}

/// `kernel/ipi/ipi.c`'s `cpumask_t get_cpu_active_mask(void)`. No live
/// caller anywhere in the tree today (C or Rust) -- `cpu_for_each_active`
/// (`kernel/inc/smp/percpu.h`) is unused too -- preserved for API
/// completeness / future use, matching the C original's own dead-code
/// status.
// P3-1B: demoted from `#[no_mangle]`; `#[allow(dead_code)]` documents the
// gap (matches this wave's `goldfish_rtc_init`/`sched_timer_add`
// precedent) instead of silently deleting still-plausible public API.
#[allow(dead_code)]
pub(crate) extern "C" fn get_cpu_active_mask() -> cpumask_t {
    CPU_ACTIVE_MASK.load(Ordering::Acquire)
}

// PGSIZE/TRAMPOLINE_CPULOCAL -- mirrors `kernel/inc/mm/memlayout.h`'s
// `TRAMPOLINE`/`TRAMPOLINE_CPULOCAL` macros. Local duplicate, same
// convention (and same derivation) as `mm/vm_pgtab.rs`'s own private
// `TRAMPOLINE_CPULOCAL` -- not imported from there since `mm/` is out of
// this wave's touch scope.
const PGSIZE: u64 = crate::bindings::PGSIZE as u64;
const PAGE_MASK: u64 = PGSIZE - 1;
const TRAMPOLINE_CPULOCAL: u64 = crate::bindings::MAXVA - PGSIZE * 3;

// ===========================================================================
// IPI mailbox + send-side API.
// ===========================================================================

/// `kernel/ipi/ipi.c`'s `uint64 ipi_pending[NCPU]`. Never `extern`-declared
/// anywhere (full-tree grep, see Wave 9 report) -- private to this file in
/// both languages.
static IPI_PENDING: [AtomicU64; NCPU] = [const { AtomicU64::new(0) }; NCPU];

/// `kernel/ipi/ipi.c`'s `int ipi_send_single(int hartid, int reason)`.
// P3-1B: only callers are `proc/sched.rs`, `proc/thread_group.rs`, and
// `proc/signal.rs` (all crate-path `use`, not `extern` redeclarations) --
// demoted.
pub(crate) extern "C" fn ipi_send_single(hartid: c_int, reason: c_int) -> c_int {
    if hartid < 0 || hartid >= NCPU as c_int {
        return neg(crate::bindings::EINVAL);
    }
    if reason < 0 || reason >= NR_IPI_REASON {
        return neg(crate::bindings::EINVAL);
    }
    // SEQ_CST in the C (`atomic_or`); weakest-correct `Release` -- see
    // module doc's ordering-map table.
    IPI_PENDING[hartid as usize].fetch_or(1u64 << reason, Ordering::Release);
    let hart_mask = 1u64 << hartid;
    sbi_send_ipi(hart_mask, 0) as c_int
}

/// `kernel/ipi/ipi.c`'s `int ipi_send_mask(unsigned long hart_mask,
/// unsigned long hart_mask_base, int reason)`.
///
/// See the module doc's "two latent C bugs" section: the C indexed
/// `ipi_pending[i + hart_mask_base]` with no bound check on the sum
/// (out-of-bounds write, UB, for any `hart_mask_base` that pushes the
/// index past `NCPU`). Every live caller passes `hart_mask_base = 0`
/// (verified by full-tree grep); this port uses checked indexing and
/// silently skips any index that would be out of range, extending the
/// existing "bits out of bounds are ignored" doc contract rather than
/// reproducing the memory-corruption UB or introducing a new panic path.
// P3-1B: no caller anywhere outside this file (`ipi_send_all_but_self`/
// `ipi_send_all`, both below) -- demoted.
pub(crate) extern "C" fn ipi_send_mask(hart_mask: u64, hart_mask_base: u64, reason: c_int) -> c_int {
    if reason < 0 || reason >= NR_IPI_REASON {
        return neg(crate::bindings::EINVAL);
    }

    for i in 0..NCPU {
        if hart_mask & (1u64 << i) == 0 {
            continue;
        }
        let idx = i + hart_mask_base as usize;
        if let Some(slot) = IPI_PENDING.get(idx) {
            slot.fetch_or(1u64 << reason, Ordering::Release);
        }
    }

    sbi_send_ipi(hart_mask, hart_mask_base) as c_int
}

/// `kernel/ipi/ipi.c`'s `int ipi_send_all_but_self(int reason)`.
// P3-1B: no caller anywhere outside this file (`ipi_irq_handler`'s
// `IPI_REASON_CRASH` case, above) -- demoted.
pub(crate) extern "C" fn ipi_send_all_but_self(reason: c_int) -> c_int {
    let hart_mask: cpumask_t = ((1u64 << NCPU) - 1) & !(1u64 << (machine::cpuid() as u64));
    ipi_send_mask(hart_mask, 0, reason)
}

/// `kernel/ipi/ipi.c`'s `int ipi_send_all(int reason)`.
///
/// See the module doc's "two latent C bugs" section: the C OR'd the
/// reason bit into every hart's pending mask *before* validating `reason`
/// (validation only happened inside the trailing `ipi_send_mask` call),
/// so an out-of-range `reason` was a shift-by-invalid-amount (UB) on the
/// direct-OR loop even though the call would ultimately have rejected it.
/// This port validates up front, matching `ipi_send_single`/
/// `ipi_send_mask`'s already-established contract. The single live
/// caller (`printf.rs`'s panic path) always passes the compile-time
/// constant `IPI_REASON_CRASH`, so this is behavior-preserving for the
/// entire live call graph.
// P3-1D mesh sweep: caller (`printf.rs`) now imports this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn ipi_send_all(reason: c_int) -> c_int {
    if reason < 0 || reason >= NR_IPI_REASON {
        return neg(crate::bindings::EINVAL);
    }

    let hart_mask: cpumask_t = (1u64 << NCPU) - 1;
    for i in 0..NCPU {
        // SEQ_CST in the C; weakest-correct `Release` -- see module doc.
        IPI_PENDING[i].fetch_or(1u64 << reason, Ordering::Release);
    }

    ipi_send_mask(hart_mask, 0, reason)
}

// ===========================================================================
// Receive side: registered handler for IRQ_S_SOFT.
// ===========================================================================

/// `kernel/ipi/ipi.c`'s `static void __ipi_irq_handler(int irq, void
/// *data, device_t *dev)`. Runs in hard interrupt context on the
/// receiving hart -- see the module doc's interrupt-context audit table
/// for the no-sleep/no-alloc justification of every case below.
///
/// # Safety
/// Must only be invoked by `irq::irq_core::do_irq`'s dispatch (the
/// `register_irq_handler` call in [`ipi_init`] is this function's only
/// registration site), which guarantees interrupts are disabled and this
/// runs on the hart the interrupt actually targeted.
unsafe extern "C" fn ipi_irq_handler(_irq: c_int, _data: *mut c_void, _dev: *mut c_void) {
    // Acknowledge: clear SIP.SSIE.
    write_sip(read_sip() & !SIE_SSIE);

    let hartid = machine::cpuid();

    // ACQ_REL in the C; weakest-correct `Acquire` -- see module doc's
    // ordering-map table (no reader ever observes the "now empty" store
    // half, so the C's Release component is provably unneeded).
    let mut pending = IPI_PENDING[hartid as usize].swap(0, Ordering::Acquire);

    while pending != 0 {
        let reason = ctz8(pending as u8);
        if reason < 0 || reason >= NR_IPI_REASON {
            break;
        }
        pending &= !(1u64 << reason);

        match reason {
            IPI_REASON_CRASH => {
                machine::CpuLocal::current().flags_or(CPU_FLAG_CRASHED);
                panic_msg_lock();
                crate::kprintln!("[Core: {}] Received IPI_REASON_CRASH, crashing...", hartid);
                // SAFETY: same as the removed `printf` call above; the
                // physical-memory bounds are written only during
                // single-hart early boot (start_kernel/fdt), read-only by
                // the time any IPI can run.
                unsafe {
                    print_backtrace(read_fp(), __physical_memory_start, __physical_memory_end);
                }
                panic_msg_unlock();
                ipi_send_all_but_self(IPI_REASON_CRASH);
                loop {
                    machine::wfi();
                }
            }
            IPI_REASON_CALL_FUNC => {
                // Request to call a function -- not implemented yet
                // (matches the C original's empty case).
            }
            IPI_REASON_RESCHEDULE => {
                // SAFETY: `rq_flush_wake_list` is documented IRQ-safe
                // (see the `extern` block above and the module doc's
                // interrupt-context audit).
                unsafe { rq_flush_wake_list(hartid) };
                machine::CpuLocal::current().flags_or(CPU_FLAG_NEEDS_RESCHED);
            }
            IPI_REASON_TLB_FLUSH => {
                // xv6 uses separate kernel/user page tables, so the TLB
                // is flushed on the next return to user mode instead
                // (matches the C original's empty case + comment).
            }
            IPI_REASON_GENERIC => {
                // Generic IPI -- no specific action (matches the C).
            }
            _ => {
                // Unknown reason (matches the C's default case).
            }
        }
    }
}

/// `kernel/ipi/ipi.c`'s `void ipi_init(void)`.
// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn ipi_init() {
    // Boot-time, single-threaded, before the handler below is even
    // registered -- see the module doc's ordering-map table entry for why
    // this store's ordering is provably unnecessary but kept for fidelity.
    for slot in IPI_PENDING.iter() {
        slot.store(0, Ordering::Release);
    }

    let mut ipi_desc = IrqDesc {
        handler: Some(ipi_irq_handler),
        data: core::ptr::null_mut(),
        dev: core::ptr::null_mut(),
        irq: 0,
        count: 0,
        // SAFETY: `rcu_head` is plain-old-data; `register_irq_handler`
        // overwrites it unconditionally (see `irq/irq_core.rs`).
        rcu_head: unsafe { core::mem::zeroed() },
    };
    // SAFETY: `ipi_desc` is a live, fully-initialized `IrqDesc` for the
    // duration of this call.
    let ret = unsafe { register_irq_handler(IRQ_S_SOFT, &raw mut ipi_desc) };
    if ret != 0 {
        __panic_start();
        crate::kprintln!(
            "ASSERTION_FAILURE {}:{}: In function '{}':",
            "kernel/ipi.rs",
            line!() as c_int,
            "ipi_init",
        );
        crate::kprintln!("ipi_init: failed to register IPI handler: {}", ret);
        __panic_end();
    }
    crate::kprintln!("ipi_init: IPI subsystem initialized (IRQ {})", IRQ_S_SOFT);
}
