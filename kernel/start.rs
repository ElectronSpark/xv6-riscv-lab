//! Boot-time hart entry point — Rust port of `kernel/start.c` (Phase 2
//! Wave 3, see `docs/rustify/phase2_plan.md`).
//!
//! `entry.S` sets up a per-hart stack in [`stack0`], then does `call
//! start` with the hart id in `a0` (already mirrored into `tp`) and the
//! FDT pointer in `a1`. Boot has already reached S-mode via OpenSBI, so
//! there is no machine-mode delegation/PMP setup to do here — [`start`]
//! only disables paging, unmasks the three supervisor interrupt-enable
//! bits, arms the first timer interrupt, and falls through to
//! `start_kernel()` (Rust, `kernel/start_kernel.rs`; Wave 27).

#![allow(non_upper_case_globals)]

use core::ffi::{c_int, c_void};
use core::sync::atomic::{AtomicI32, Ordering};

use crate::machine;

// ===========================================================================
// stack0 — one boot-time kernel stack per hart.
// ===========================================================================
//
// `entry.S` (`_skip_bss_clear:`) computes
//     sp = &stack0 + (hartid + 1) * KERNEL_STACK_SIZE - 4096
// (the last 4096 bytes of each hart's slice are reserved for its PCB)
// *before* calling `start()`, and `idle_thread_init`
// (`kernel/proc/thread.rs`) later recovers a hart's stack base by
// masking the live `sp` down to a `KERNEL_STACK_SIZE` boundary. Both of
// those depend on this symbol's exact name, total size, and alignment —
// keep all three in lockstep with `entry.S` and `thread.rs` if either
// ever changes.
//
// Placement: the C original had no explicit `section` attribute, only
// `aligned(KERNEL_STACK_SIZE)`; being a global array with no
// initialiser, it landed in `.bss` by default. `entry.S` clears `.bss`
// (guarded by `__bss_cleared_flag`, published with `fence w, rw`)
// strictly *before* the `la sp, stack0` sequence that first touches this
// memory, so `.bss` placement is safe: no hart reads or writes `stack0`
// until after the clear/fence has completed. The Rust definition below
// is an all-zero static, which rustc/LLVM place in `.bss` for the same
// reason (confirmed post-build with `nm -S` / `objdump -t`, see the
// Wave 3 report).

const PAGE_SHIFT: u32 = 12;
const KERNEL_STACK_ORDER: u32 = 2;
/// Mirrors `KERNEL_STACK_SIZE` in `kernel/inc/param.h`
/// (`1UL << (PAGE_SHIFT + KERNEL_STACK_ORDER)`). Not exposed via
/// bindgen (`build.rs`'s `allowlist_var` doesn't cover it), so it is
/// duplicated here — same convention already used for `KERNEL_STACK_SIZE`
/// in `kernel/proc/thread.rs` and for `PGSIZE` in
/// `kernel/machine/machine.rs`.
const KERNEL_STACK_SIZE: usize = 1usize << (PAGE_SHIFT + KERNEL_STACK_ORDER);
/// Mirrors `NCPU` in `kernel/inc/param.h`. Hardcoded locally rather than
/// read from `crate::bindings::NCPU`, matching the existing convention
/// in `kernel/mm/page.rs` / `kernel/proc/thread.rs`.
const NCPU: usize = 8;

const STACK0_LEN: usize = KERNEL_STACK_SIZE * NCPU;

/// Alignment-carrying wrapper: `#[repr(align(N))]` requires a literal,
/// so the `16384` below is pinned equal to `KERNEL_STACK_SIZE` by the
/// `const _` assertion that follows it.
#[repr(C, align(16384))]
pub struct Stack0([u8; STACK0_LEN]);

const _: () = assert!(
    16384 == KERNEL_STACK_SIZE,
    "Stack0's #[repr(align(..))] literal must equal KERNEL_STACK_SIZE"
);

/// `entry.S`'s per-hart boot stack (see the module-level comment above
/// for the full contract). `#[no_mangle]` plus the exact type layout
/// keep the C-visible symbol name, size (`KERNEL_STACK_SIZE * NCPU` =
/// 128 KiB), and alignment (`KERNEL_STACK_SIZE` = 16 KiB) identical to
/// the original
/// `char stack0[KERNEL_STACK_SIZE * NCPU] __attribute__((aligned(KERNEL_STACK_SIZE)))`.
#[no_mangle]
pub static mut stack0: Stack0 = Stack0([0u8; STACK0_LEN]);

// ===========================================================================
// boot_hartid — CAS'd once, by whichever hart reaches `start()` first.
// ===========================================================================

/// Mirrors the C `_Atomic int boot_hartid = -1;` — private to
/// `start.c`/this module; no other file reads it.
static BOOT_HARTID: AtomicI32 = AtomicI32::new(-1);

// ===========================================================================
// start_kernel — Rust (`kernel/start_kernel.rs`, Wave 27). P3-D3c: plain
// crate-path import instead of an `extern "C"` redeclaration (demoted from
// `#[no_mangle]` in the same wave).
// ===========================================================================

use crate::start_kernel::start_kernel;

/// `entry.S` jumps here in supervisor mode, on `stack0`, once per hart.
///
/// When booting from OpenSBI: we're already in S-mode, `hartid` arrives
/// in `a0` (already mirrored into `tp` by `entry.S`), and the DTB
/// pointer arrives in `a1`. Ported from `kernel/start.c::start()`.
#[no_mangle]
pub extern "C" fn start(hartid: c_int, fdt_base: *mut c_void) {
    // The first hart to get here becomes the boot hart.
    //
    // `SeqCst`/`SeqCst` (stronger than the `Acquire`/`Relaxed` this
    // standalone CAS would need on its own) is a deliberate 1:1 port of
    // the C `atomic_cas()` macro (`kernel/inc/smp/atomic.h`), which
    // hardcodes `__ATOMIC_SEQ_CST` for both the success and failure
    // orderings and is used unweakened by every other Rust port of an
    // `atomic_cas()` call site in this crate (e.g. `kobject.rs`,
    // `proc/access.rs`). Runs at most `NCPU` (<=8) times in the kernel's
    // entire lifetime, so the extra fence has no measurable cost; keeping
    // it exactly as strong as the audited C avoids introducing a new
    // boot-race risk for a memory-order micro-optimisation.
    let is_boot_hart = BOOT_HARTID
        .compare_exchange(-1, hartid, Ordering::SeqCst, Ordering::SeqCst)
        .is_ok();

    // Disable paging for now.
    machine::write_satp(0);

    // Enable supervisor-mode interrupts.
    machine::write_sie(
        machine::read_sie() | machine::SIE_SEIE | machine::SIE_STIE | machine::SIE_SSIE,
    );

    // Ask for clock interrupts.
    timerinit();

    // Jump to main(). `start_kernel` is the (safe, Rust) boot-continuation
    // entry point; `hartid`/`fdt_base` are forwarded unmodified from
    // `entry.S`'s `a0`/`a1`, and `is_boot_hart` reflects the CAS above.
    // `entry.S` guarantees a valid stack for this call (a `stack0` slice,
    // cleared and fenced before use).
    start_kernel(hartid, fdt_base, is_boot_hart);
}

// ===========================================================================
// timerinit — ask this hart to generate timer interrupts.
// ===========================================================================

// Owned by `kernel/timer/timer_core.rs`; this function and the platform
// probe in `kernel/dev/fdt.rs` are its only writers before the scheduler
// starts. P3-D3c: plain crate-path import instead of an `extern "C"`
// redeclaration (demoted from `#[no_mangle]` in the same wave).
use crate::timer::timer_core::__jiff_ticks;

/// Ask this hart to generate timer interrupts. When using OpenSBI, the
/// firmware has already configured the `menvcfg.STCE` bit for the sstc
/// extension and `mcounteren` for `stimecmp`/`time` access — this only
/// arms the first timer interrupt.
///
/// Private to [`start`], exactly like the C `timerinit()` it replaces
/// (note: distinct from the unrelated `timer_init()` in
/// `kernel/timer/timer.c`).
fn timerinit() {
    // Calculate jiffy ticks — one-time calculation, so no optimisation
    // needed. `HZ` (`kernel/inc/timer/timer.h`) is a fixed `1000`, so
    // `TIMEBASE_FREQUENCY / HZ` is exactly `machine::tick_ms()`.
    // SAFETY: single-writer at this boot stage (see the `extern` doc
    // above) — no other hart or interrupt handler observes
    // `__jiff_ticks` yet.
    unsafe {
        __jiff_ticks = machine::tick_ms();
    }

    // Ask for the very first timer interrupt.
    // SAFETY: reads the value just stored above.
    let jiff_ticks = unsafe { __jiff_ticks };
    machine::write_stimecmp(machine::read_time() + jiff_ticks);
}
