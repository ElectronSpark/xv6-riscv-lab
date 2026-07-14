//! RISC-V Platform Level Interrupt Controller (PLIC) driver.
//!
//! Rust port of `kernel/irq/plic.c`. Only the S-mode registers/macros this
//! file's C original actually used are reproduced (`kernel/inc/dev/plic.h`
//! also defines M-mode variants and a `bits_test_and_set/clear/test_bit32`
//! pending/enable-test macro family that have no live caller anywhere in
//! the tree -- confirmed by grep -- so they are not ported).
//!
//! # MMIO
//!
//! Every register access below is a single `read_volatile`/`write_volatile`
//! at an address computed from the FDT-configured [`__plic_mmio_base`].

use core::ffi::c_int;

use crate::machine::cpuid;

/// PLIC MMIO base address. Resolved from the FDT at boot
/// (`dev/fdt.c::fdt_apply_platform_config` writes it through the `extern
/// uint64 __plic_mmio_base` declaration in `kernel/inc/dev/plic.h`,
/// unchanged by this port); defaults to the QEMU `virt` machine's PLIC
/// address, matching the C initializer. Same "FDT-configured global becomes
/// `#[no_mangle] pub static mut`, still written by `dev/fdt.c`" pattern as
/// `uart.rs`'s `__uart0_*` statics (Wave 4).
#[no_mangle]
pub static mut __plic_mmio_base: u64 = 0x0c00_0000;

const PLIC_ENABLE_BASE: u64 = 0x2000;
const PLIC_PRIORITY_THRESH_BASE: u64 = 0x20_0000;
const PLIC_CLAIM_BASE: u64 = 0x20_0004;

#[inline(always)]
fn plic_base() -> u64 {
    // SAFETY: `__plic_mmio_base` has a single boot-time writer
    // (`dev/fdt.c`, before secondary harts start or any interrupt is
    // enabled) and is read-only for the rest of the kernel's life -- same
    // invariant as `uart.rs`'s `__uart0_mmio_base`.
    unsafe { __plic_mmio_base }
}

/// `PLIC_SPRIORITY_THRESH(hart)`: this hart's S-mode priority threshold
/// register.
#[inline(always)]
fn s_priority_thresh(hart: u32) -> *mut u32 {
    (plic_base() + PLIC_PRIORITY_THRESH_BASE + ((hart as u64) << 13) + 0x1000) as *mut u32
}

/// `PLIC_SCLAIM(hart)`: this hart's S-mode claim/complete register.
#[inline(always)]
fn s_claim(hart: u32) -> *mut u32 {
    (plic_base() + PLIC_CLAIM_BASE + ((hart as u64) << 13) + 0x1000) as *mut u32
}

/// `PLIC_PRIORITY(irq)`: the priority register for PLIC source `irq`.
#[inline(always)]
fn priority(irq: u32) -> *mut u32 {
    (plic_base() + ((irq as u64) << 2)) as *mut u32
}

/// `PLIC_SENABLE(hart)`: base of this hart's S-mode IRQ-enable bitmap (one
/// bit per PLIC source, packed 32 bits/word).
#[inline(always)]
fn s_enable_base(hart: u32) -> *mut u32 {
    (plic_base() + PLIC_ENABLE_BASE + ((hart as u64) << 8) + 0x80) as *mut u32
}

/// Core PLIC initialization. Device-specific IRQ priorities are set by
/// each device's init function via [`plic_enable_irq`].
// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn plicinit() {}

/// Set this hart's S-mode priority threshold to 0 (accept all priorities).
/// Device-specific IRQ enables are done by each device's init function via
/// [`plic_enable_irq`].
// P3-1B: only callers are `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn plicinithart() {
    let hart = cpuid() as u32;
    // SAFETY: `s_priority_thresh` computes an address inside the PLIC MMIO
    // window, mapped uncached by `kvmmake` (`mm/vm_pgtab.rs`) for the
    // kernel's whole post-`kvminithart` lifetime; `hart` is this CPU's own
    // id, so this hart is the sole writer of this specific register.
    unsafe {
        core::ptr::write_volatile(s_priority_thresh(hart), 0);
    }
}

/// Ask the PLIC what interrupt we should serve. Returns 0 if none is
/// pending (spurious interrupt) -- reading this register also claims the
/// interrupt, so the read must not be reordered/elided.
// P3-1B: only caller is `irq/irq_core.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn plic_claim() -> c_int {
    let hart = cpuid() as u32;
    // SAFETY: see `plicinithart`; safe to call from any context (including
    // interrupt context) on the owning hart.
    unsafe { core::ptr::read_volatile(s_claim(hart)) as c_int }
}

/// Tell the PLIC we've finished servicing `irq`.
// P3-1B: only caller is `irq/irq_core.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn plic_complete(irq: c_int) {
    let hart = cpuid() as u32;
    // SAFETY: see `plic_claim`.
    unsafe {
        core::ptr::write_volatile(s_claim(hart), irq as u32);
    }
}

/// Enable PLIC source `irq` (priority 1) for every hart's S-mode context.
/// Inlines the one bitmap-bit-set operation this file's C original needed
/// (`bits_test_and_set_bit32`, see module doc) rather than porting the
/// wider, otherwise-unused bitmap-helper macro family.
// P3-1B: only caller is `irq/irq_core.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn plic_enable_irq(irq: c_int) {
    let irq = irq as u32;
    let word_index = (irq >> 5) as usize;
    let mask = 1u32 << (irq & 31);
    // SAFETY: `priority(irq)` and, for each hart, `s_enable_base(hart)` are
    // all distinct PLIC MMIO registers/words (see `plicinithart`); each
    // hart's enable bitmap is written here only, one word at a time.
    unsafe {
        core::ptr::write_volatile(priority(irq), 1);
        for hart in 0..crate::bindings::NCPU {
            let word = s_enable_base(hart).add(word_index);
            let prev = core::ptr::read_volatile(word);
            core::ptr::write_volatile(word, prev | mask);
        }
    }
}
