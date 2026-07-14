//! Simple PCI-Express enumeration -- Rust port of `kernel/pci.c` (Phase 2
//! Wave 28, sub-wave B -- see `docs/rustify/phase2_plan.md`; the final
//! porting wave's network drivers). Only works for qemu and its e1000
//! card, per the C original's own header comment.
//!
//! # ECAM safety
//!
//! The PCIe ECAM (Enhanced Configuration Access Mechanism) region is
//! memory-mapped register space, not RAM -- every access must go
//! through `read_volatile`/`write_volatile` so the compiler can never
//! elide, reorder, or coalesce it (matches the C `volatile uint32
//! *base`/`volatile struct pci_common_confspace_header *dsc` -- **2**
//! `volatile`-qualified declarations in the C original, per the plan's
//! §1 volatile-count table; every field access through `dsc` inherits
//! that qualification in C, so every one of this port's field reads/
//! writes below is individually wrapped in `read_volatile`/
//! `write_volatile`, not just the two declaration sites). [`pci_common_
//! confspace_header`] (`crate::bindings`, generated from
//! `kernel/inc/dev/pci.h`, added to `build.rs`'s allowlist this wave) is
//! a real `bindgen` type -- the qemu ECAM region's byte layout is
//! defined by the PCI-E Base 4.0 Specification, so getting it from the
//! same header the size/offset invariants are written against is the
//! stronger guarantee, same rationale as every other hardware-ABI
//! struct this wave (`virtq_desc` &c., `tx_desc`/`rx_desc`).
//! [`layout_asserts`] pins its `size_of` at compile time in addition to
//! the C original's own runtime `sizeof(...) != 0x40` check (kept
//! verbatim below for line-for-line fidelity) -- belt and suspenders:
//! the compile-time assert catches a layout regression at build time,
//! before any hardware is ever touched, while the runtime check remains
//! exactly where the C put it.
//!
//! # Barrier accounting
//!
//! Exactly **2** `__atomic_thread_fence(__ATOMIC_SEQ_CST)` sites in the
//! C original, both in the e1000-BAR-probe branch of the enumeration
//! loop: one after writing the command register (`PCIE_CSCMD_IAE|MAE|
//! BME`), one inside the six-BAR size-probe loop (after writing
//! `0xffffffff` to a BAR, before restoring its original value). Both
//! preserved below as [`core::sync::atomic::fence`]`(Ordering::SeqCst)`
//! at the identical call site.
//!
//! # Address computation (word-indexed, not byte-indexed)
//!
//! `ecam` is a `uint32 *`/`*mut u32` in the C original, and `off` (`(bus
//! << 16) | (dev << 11) | (func << 8) | offset`) is added to it via
//! plain pointer arithmetic -- i.e. `off` is a **word** index (each unit
//! = 4 bytes), not the byte index the constant names might suggest.
//! Ported verbatim via `ecam.wrapping_add(off as usize)` (`u32`
//! pointer arithmetic in Rust is likewise word-scaled) -- not "fixed" to
//! byte-addressing, since this is this driver's own (deliberately
//! simplified, single-bus/single-function, qemu-only) addressing scheme,
//! not a bug.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{fence, Ordering};

use crate::bindings::pci_common_confspace_header;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.
    fn printf(fmt: *const c_char, ...) -> c_int;
    safe fn __panic_start();
    safe fn __panic_end() -> !;
}
// P3-1D mesh sweep: e1000.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::e1000::e1000_init;

// ===========================================================================
// PCI-E Configuration Space Command/Status/Header-Type bits --
// redeclared locally from `kernel/inc/dev/pci.h` (macro-only additions
// to the header; the struct itself is real bindgen -- see module doc).
// ===========================================================================

const PCIE_CSCMD_IAE: u16 = 1 << 0; // I/O Access Enable
const PCIE_CSCMD_MAE: u16 = 1 << 1; // Memory Access Enable
const PCIE_CSCMD_BME: u16 = 1 << 2; // Bus Master Enable

// ===========================================================================
// DMA/MMIO layout asserts (see module doc's "ECAM safety").
// ===========================================================================
#[allow(dead_code)]
mod layout_asserts {
    use super::*;
    const _SIZE_HEADER: () = assert!(core::mem::size_of::<pci_common_confspace_header>() == 0x40);
}

/// `uint64 __pcie_ecam_mmio_base = 0x30000000L;`.
// P3-1D mesh sweep: callers (`mm/vm_pgtab.rs`, `dev/fdt.rs`) now import
// this via crate-path `use` instead of an `extern` redeclaration --
// demoted.
pub(crate) static mut __pcie_ecam_mmio_base: u64 = 0x30000000;

/// `void pci_init(void)`.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn pci_init() {
    // We'll place the e1000 registers at this address. vm_pgtab.rs
    // (kvmmake) maps this range.
    let e1000_regs: u64 = 0x40000000;

    // qemu -machine virt puts PCIe config space here. vm_pgtab.rs
    // (kvmmake) maps this range.
    let ecam: *mut u32 = 0x30000000u64 as *mut u32;

    if core::mem::size_of::<pci_common_confspace_header>() != 0x40 {
        __panic_start();
        // SAFETY: format string matches its one argument.
        unsafe {
            printf(
                c"sizeof pci_common_confspace_header: %lx\n".as_ptr(),
                core::mem::size_of::<pci_common_confspace_header>() as u64,
            )
        };
        // SAFETY: fixed message, no format args.
        unsafe {
            printf(
                c"The size of PCI-E Common Configuration Space Header Structure is not 0x40 Bytes!\n"
                    .as_ptr(),
            )
        };
        __panic_end();
    }

    // Look at each possible PCI device on bus 0.
    for dev in 0u32..32 {
        let bus: u32 = 0;
        let func: u32 = 0;
        let offset: u32 = 0;
        let off = (bus << 16) | (dev << 11) | (func << 8) | offset;
        // SAFETY: `off` is bounds-derived from `dev < 32` and the fixed
        // `bus`/`func`/`offset` above, landing within the qemu ECAM
        // region `vm_pgtab.rs`'s `kvmmake` maps (`platform.pcie_reg[]`,
        // aligned/sized to cover this whole enumeration range).
        let dsc: *mut pci_common_confspace_header = unsafe { ecam.wrapping_add(off as usize) as *mut pci_common_confspace_header };

        // SAFETY: `dsc` points into the mapped ECAM region (see above);
        // every field access is a volatile MMIO read/write.
        let (vendor_id, device_id) =
            unsafe { (core::ptr::read_volatile(&raw const (*dsc).vendor_id), core::ptr::read_volatile(&raw const (*dsc).device_id)) };

        if dev < 8 {
            // SAFETY: format string matches its four arguments.
            unsafe {
                printf(
                    c"PCI device %d:%d:%d - vendor ID: 0x%x, device ID: 0x%x\n".as_ptr(),
                    bus as c_int,
                    dev as c_int,
                    func as c_int,
                    vendor_id as c_int,
                    device_id as c_int,
                )
            };
        }

        // 100e:8086 is an e1000.
        if device_id == 0x100e && vendor_id == 0x8086 {
            // SAFETY: fixed message, no format args.
            unsafe { printf(c"E1000 Ethernet Controller detected.\n".as_ptr()) };

            // Command and status register:
            //   bit 0: I/O access enable
            //   bit 1: memory access enable
            //   bit 2: enable mastering
            // SAFETY: `dsc` live (see above); volatile MMIO write.
            unsafe { core::ptr::write_volatile(&raw mut (*dsc).command, PCIE_CSCMD_IAE | PCIE_CSCMD_MAE | PCIE_CSCMD_BME) };
            fence(Ordering::SeqCst);

            for i in 0..6usize {
                // SAFETY: `dsc` live; `__bindgen_anon_2.header_type_0`
                // is the active union member for a type-0 (non-bridge)
                // header, which every PCI function this loop probes is
                // (qemu's e1000 device, the only match this branch
                // reaches); volatile MMIO read/write.
                let old = unsafe { core::ptr::read_volatile(&raw const (*dsc).__bindgen_anon_2.header_type_0.base_addr[i]) };

                // Writing all 1's to the BAR causes it to be replaced
                // with its size.
                // SAFETY: same as above.
                unsafe { core::ptr::write_volatile(&raw mut (*dsc).__bindgen_anon_2.header_type_0.base_addr[i], 0xffffffff) };
                fence(Ordering::SeqCst);

                // SAFETY: same as above.
                unsafe { core::ptr::write_volatile(&raw mut (*dsc).__bindgen_anon_2.header_type_0.base_addr[i], old) };
            }

            // Tell the e1000 to reveal its registers at physical address
            // 0x40000000.
            // SAFETY: `dsc` live; volatile MMIO write.
            unsafe { core::ptr::write_volatile(&raw mut (*dsc).__bindgen_anon_2.header_type_0.base_addr[0], e1000_regs as u32) };

            // `e1000_regs` is the physical address just programmed into
            // BAR0 above, mapped by `vm_pgtab.rs`'s `kvmmake` (see the
            // module doc's "We'll place..." comment).
            e1000_init(e1000_regs as *mut u32);
        }
    }
}
