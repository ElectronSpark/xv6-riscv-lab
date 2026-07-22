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
//! confspace_header`] resolves through the unchanged `crate::bindings`
//! facade path to the NATIVE [`PciCommonConfspaceHeader`] below (P3-4c;
//! it was a real `bindgen` type from `kernel/inc/dev/pci.h` when this
//! port landed) -- the qemu ECAM region's byte layout is defined by the
//! PCI-E Base 4.0 Specification, pinned byte-exact by the hardcoded
//! asserts + the toolchain-gcc probe, same rationale as every other
//! hardware-ABI struct this wave (`virtq_desc` &c.,
//! `tx_desc`/`rx_desc`).
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
// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::Printf;

unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.
}
// P3-1D mesh sweep: e1000.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::e1000::E1000;

// ===========================================================================
// PCI-E Configuration Space Command/Status/Header-Type bits --
// redeclared locally from `kernel/inc/dev/pci.h` (macro-only additions
// to the header; the struct itself is real bindgen -- see module doc).
// ===========================================================================

const PCIE_CSCMD_IAE: u16 = 1 << 0; // I/O Access Enable
const PCIE_CSCMD_MAE: u16 = 1 << 1; // Memory Access Enable
const PCIE_CSCMD_BME: u16 = 1 << 2; // Bus Master Enable

// ===========================================================================
// Native `pci_common_confspace_header` — P3-4c nativization (user
// directive: remove the C-compatible interfaces; MMIO-layout scrutiny
// class). `PciCommonConfspaceHeader` (plus the named union and its
// three struct arms below) is the canonical definition of
// `kernel/inc/dev/pci.h`'s `struct pci_common_confspace_header`:
// `build.rs` blocklists the bindgen emissions
// (`pci_common_confspace_header` + its `__bindgen_ty_*` shells) and
// re-exports this type as `crate::bindings::
// pci_common_confspace_header` (facade `pub use`, N2 pattern).
//
// *** HARDWARE MMIO LAYOUT — HANDLE WITH P3-4 SCRUTINY *** This is the
// PCI-E Base 4.0 spec's Common Configuration Space Header, overlaid on
// the qemu ECAM region (`pci_init` casts `ecam + off` straight to
// `*mut PciCommonConfspaceHeader` and reads/writes fields through
// volatile MMIO). Any layout drift silently probes the wrong config
// registers. The byte-exact asserts below pin every field of every arm.
//
// Two anonymous-member translations (both byte-identical):
// * The C header's anonymous bitfield struct `{ uint32 revision_id:8;
//   uint32 class_code:24; }` (bindgen: `__bindgen_anon_1`, a 4-byte
//   `__BindgenBitfieldUnit` with `get(0,8)`/`get(8,24)` LSB-first
//   accessors) becomes the plain `rev_class: uint32` word —
//   `revision_id` is bits [7:0], `class_code` bits [31:8]. ZERO
//   consumers used the bitfield accessors (verified crate-wide), so no
//   accessor shims are reproduced.
// * The anonymous union (bindgen: `__bindgen_anon_2`) becomes the
//   named field `type_spec: PciHeaderTypeSpec`; this file's five
//   `header_type_0.base_addr` BAR-probe sites (the union's only
//   consumers crate-wide) are re-pointed in place.
//
// DERIVE DECISIONS (P3-4c): Copy + Clone on all five types, exactly as
// the pre-nativization bindgen output derived (POD scalars/arrays; a
// Rust union requires Copy arms anyway).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + toolchain-gcc `_Static_assert` probe (rv64gc/lp64d —
// scratchpad p3_4c_dma_probe.c); both agree on every value asserted
// below.
// ===========================================================================

/// Header-type-agnostic tail of [`PciHeaderTypeSpec`] (the header's
/// `header_type_common` arm) — only the fields every header type
/// shares, at their common offsets.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PciHeaderTypeCommon {
    pub header_type_spec_0: [crate::bindings::uint8; 36],
    /// Capabilities pointer.
    pub caps_ptr: crate::bindings::uint8,
    pub header_type_spec_1: [crate::bindings::uint8; 7],
    /// Interrupt line.
    pub intr_line: crate::bindings::uint8,
    /// Interrupt pin.
    pub intr_pin: crate::bindings::uint8,
    pub header_type_spec_2: [crate::bindings::uint8; 2],
}

/// Type-0 (endpoint) arm of [`PciHeaderTypeSpec`] (the header's
/// `header_type_0` arm).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PciHeaderType0 {
    /// Base address registers (BAR0-BAR5).
    pub base_addr: [crate::bindings::uint32; 6],
    pub card_bus_cis_ptr: crate::bindings::uint32,
    /// Subsystem vendor ID.
    pub subsys_vendor_id: crate::bindings::uint16,
    /// Subsystem ID.
    pub subsys_id: crate::bindings::uint16,
    /// Expansion ROM base address.
    pub eprom_base_addr: crate::bindings::uint32,
    /// Capabilities pointer.
    pub caps_ptr: crate::bindings::uint8,
    /// Reserved.
    pub rsvd: [crate::bindings::uint8; 7],
    /// Interrupt line.
    pub intr_line: crate::bindings::uint8,
    /// Interrupt pin.
    pub intr_pin: crate::bindings::uint8,
    pub min_gnt: crate::bindings::uint8,
    pub min_lat: crate::bindings::uint8,
}

/// Type-1 (PCI-PCI bridge) arm of [`PciHeaderTypeSpec`] (the header's
/// `header_type_1` arm).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PciHeaderType1 {
    /// Base address register 0.
    pub base_addr_reg0: crate::bindings::uint32,
    /// Base address register 1.
    pub base_addr_reg1: crate::bindings::uint32,
    /// Primary bus number.
    pub pri_bus_no: crate::bindings::uint8,
    /// Secondary bus number.
    pub sec_bus_no: crate::bindings::uint8,
    /// Subordinate bus number.
    pub sub_bus_no: crate::bindings::uint8,
    /// Secondary latency timer.
    pub sec_lat_timer: crate::bindings::uint8,
    /// I/O base lower 8 bits.
    pub io_base: crate::bindings::uint8,
    /// I/O limit lower 8 bits.
    pub io_limit: crate::bindings::uint8,
    /// Secondary status.
    pub sstatus: crate::bindings::uint16,
    /// Memory base.
    pub mem_base: crate::bindings::uint16,
    /// Memory limit.
    pub mem_limit: crate::bindings::uint16,
    /// Prefetchable memory base lower 16 bits.
    pub pmem_base: crate::bindings::uint16,
    /// Prefetchable memory limit lower 16 bits.
    pub pmem_limit: crate::bindings::uint16,
    pub pmem_base_upper: crate::bindings::uint32,
    pub pmem_limit_upper: crate::bindings::uint32,
    pub io_base_upper: crate::bindings::uint16,
    pub io_limit_upper: crate::bindings::uint16,
    /// Capabilities pointer.
    pub caps_ptr: crate::bindings::uint8,
    /// Reserved.
    pub rsvd: [crate::bindings::uint8; 7],
    /// Interrupt line.
    pub intr_line: crate::bindings::uint8,
    /// Interrupt pin.
    pub intr_pin: crate::bindings::uint8,
    /// Bridge control.
    pub bridge_ctl: crate::bindings::uint16,
}

/// The header-type-specific tail (bytes 16..64) of
/// [`PciCommonConfspaceHeader`] — the header's anonymous union
/// (bindgen's `pci_common_confspace_header__bindgen_ty_2`).
#[repr(C)]
#[derive(Copy, Clone)]
pub union PciHeaderTypeSpec {
    /// Raw dword view.
    pub header_type_spec: [crate::bindings::uint32; 12],
    /// Header-type-agnostic common fields.
    pub header_type_common: PciHeaderTypeCommon,
    /// Type-0 (endpoint) view — the active arm for every device
    /// `pci_init` probes.
    pub header_type_0: PciHeaderType0,
    /// Type-1 (bridge) view.
    pub header_type_1: PciHeaderType1,
}

/// Native `struct pci_common_confspace_header`
/// (`kernel/inc/dev/pci.h`) — the PCI-E Common Configuration Space
/// Header, overlaid on the ECAM MMIO region.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PciCommonConfspaceHeader {
    pub vendor_id: crate::bindings::uint16,
    pub device_id: crate::bindings::uint16,
    pub command: crate::bindings::uint16,
    pub status: crate::bindings::uint16,
    /// Revision ID (bits [7:0]) | class code (bits [31:8]) — the C
    /// header's anonymous `revision_id:8`/`class_code:24` bitfield
    /// word (see the nativization note above).
    pub rev_class: crate::bindings::uint32,
    pub cache_line_size: crate::bindings::uint8,
    pub master_latency_timer: crate::bindings::uint8,
    pub header_type: crate::bindings::uint8,
    pub bist: crate::bindings::uint8,
    /// Header-type-specific tail (the C header's anonymous union;
    /// bindgen's `__bindgen_anon_2`).
    pub type_spec: PciHeaderTypeSpec,
}

// P3-4c hardcoded layout proof — the PCI-E spec byte contract
// (`kernel/inc/dev/pci.h`), every field of every arm. Values captured
// from the pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the toolchain-gcc probe.
const _: () = {
    use core::mem::{align_of, offset_of, size_of};
    assert!(size_of::<PciCommonConfspaceHeader>() == 0x40, "pci hdr size (MMIO)");
    assert!(align_of::<PciCommonConfspaceHeader>() == 4, "pci hdr alignment");
    assert!(offset_of!(PciCommonConfspaceHeader, vendor_id) == 0, "pci.vendor_id (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, device_id) == 2, "pci.device_id (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, command) == 4, "pci.command (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, status) == 6, "pci.status (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, rev_class) == 8, "pci.rev_class (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, cache_line_size) == 12, "pci.cache_line_size (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, master_latency_timer) == 13, "pci.mlt (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, header_type) == 14, "pci.header_type (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, bist) == 15, "pci.bist (MMIO)");
    assert!(offset_of!(PciCommonConfspaceHeader, type_spec) == 16, "pci.type_spec (MMIO)");

    assert!(size_of::<PciHeaderTypeSpec>() == 48, "pci union size (MMIO)");
    assert!(align_of::<PciHeaderTypeSpec>() == 4, "pci union alignment");

    assert!(size_of::<PciHeaderTypeCommon>() == 48, "pci common arm size");
    assert!(align_of::<PciHeaderTypeCommon>() == 1, "pci common arm alignment");
    assert!(offset_of!(PciHeaderTypeCommon, header_type_spec_0) == 0, "common.spec0");
    assert!(offset_of!(PciHeaderTypeCommon, caps_ptr) == 36, "common.caps_ptr");
    assert!(offset_of!(PciHeaderTypeCommon, header_type_spec_1) == 37, "common.spec1");
    assert!(offset_of!(PciHeaderTypeCommon, intr_line) == 44, "common.intr_line");
    assert!(offset_of!(PciHeaderTypeCommon, intr_pin) == 45, "common.intr_pin");
    assert!(offset_of!(PciHeaderTypeCommon, header_type_spec_2) == 46, "common.spec2");

    assert!(size_of::<PciHeaderType0>() == 48, "pci type-0 arm size");
    assert!(align_of::<PciHeaderType0>() == 4, "pci type-0 arm alignment");
    assert!(offset_of!(PciHeaderType0, base_addr) == 0, "ht0.base_addr (BARs, MMIO)");
    assert!(offset_of!(PciHeaderType0, card_bus_cis_ptr) == 24, "ht0.card_bus_cis_ptr");
    assert!(offset_of!(PciHeaderType0, subsys_vendor_id) == 28, "ht0.subsys_vendor_id");
    assert!(offset_of!(PciHeaderType0, subsys_id) == 30, "ht0.subsys_id");
    assert!(offset_of!(PciHeaderType0, eprom_base_addr) == 32, "ht0.eprom_base_addr");
    assert!(offset_of!(PciHeaderType0, caps_ptr) == 36, "ht0.caps_ptr");
    assert!(offset_of!(PciHeaderType0, rsvd) == 37, "ht0.rsvd");
    assert!(offset_of!(PciHeaderType0, intr_line) == 44, "ht0.intr_line");
    assert!(offset_of!(PciHeaderType0, intr_pin) == 45, "ht0.intr_pin");
    assert!(offset_of!(PciHeaderType0, min_gnt) == 46, "ht0.min_gnt");
    assert!(offset_of!(PciHeaderType0, min_lat) == 47, "ht0.min_lat");

    assert!(size_of::<PciHeaderType1>() == 48, "pci type-1 arm size");
    assert!(align_of::<PciHeaderType1>() == 4, "pci type-1 arm alignment");
    assert!(offset_of!(PciHeaderType1, base_addr_reg0) == 0, "ht1.bar0");
    assert!(offset_of!(PciHeaderType1, base_addr_reg1) == 4, "ht1.bar1");
    assert!(offset_of!(PciHeaderType1, pri_bus_no) == 8, "ht1.pri_bus_no");
    assert!(offset_of!(PciHeaderType1, sec_bus_no) == 9, "ht1.sec_bus_no");
    assert!(offset_of!(PciHeaderType1, sub_bus_no) == 10, "ht1.sub_bus_no");
    assert!(offset_of!(PciHeaderType1, sec_lat_timer) == 11, "ht1.sec_lat_timer");
    assert!(offset_of!(PciHeaderType1, io_base) == 12, "ht1.io_base");
    assert!(offset_of!(PciHeaderType1, io_limit) == 13, "ht1.io_limit");
    assert!(offset_of!(PciHeaderType1, sstatus) == 14, "ht1.sstatus");
    assert!(offset_of!(PciHeaderType1, mem_base) == 16, "ht1.mem_base");
    assert!(offset_of!(PciHeaderType1, mem_limit) == 18, "ht1.mem_limit");
    assert!(offset_of!(PciHeaderType1, pmem_base) == 20, "ht1.pmem_base");
    assert!(offset_of!(PciHeaderType1, pmem_limit) == 22, "ht1.pmem_limit");
    assert!(offset_of!(PciHeaderType1, pmem_base_upper) == 24, "ht1.pmem_base_upper");
    assert!(offset_of!(PciHeaderType1, pmem_limit_upper) == 28, "ht1.pmem_limit_upper");
    assert!(offset_of!(PciHeaderType1, io_base_upper) == 32, "ht1.io_base_upper");
    assert!(offset_of!(PciHeaderType1, io_limit_upper) == 34, "ht1.io_limit_upper");
    assert!(offset_of!(PciHeaderType1, caps_ptr) == 36, "ht1.caps_ptr");
    assert!(offset_of!(PciHeaderType1, rsvd) == 37, "ht1.rsvd");
    assert!(offset_of!(PciHeaderType1, intr_line) == 44, "ht1.intr_line");
    assert!(offset_of!(PciHeaderType1, intr_pin) == 45, "ht1.intr_pin");
    assert!(offset_of!(PciHeaderType1, bridge_ctl) == 46, "ht1.bridge_ctl");
};

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
        Printf::__panic_start();
        // SAFETY: format string matches its one argument.
        unsafe {
            crate::kprintln!(
                "sizeof pci_common_confspace_header: {:x}",
                core::mem::size_of::<pci_common_confspace_header>() as u64,
            )
        };
        // SAFETY: fixed message, no format args.
        unsafe {
            crate::kprintln!(
                "The size of PCI-E Common Configuration Space Header Structure is not 0x40 Bytes!",
            )
        };
        Printf::__panic_end();
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
                crate::kprintln!(
                    "PCI device {}:{}:{} - vendor ID: 0x{:x}, device ID: 0x{:x}",
                    bus as c_int,
                    dev as c_int,
                    func as c_int,
                    ((vendor_id as c_int) as i32 as i64) as u64,
                    ((device_id as c_int) as i32 as i64) as u64,
                )
            };
        }

        // 100e:8086 is an e1000.
        if device_id == 0x100e && vendor_id == 0x8086 {
            // SAFETY: fixed message, no format args.
            unsafe { crate::kprintln!("E1000 Ethernet Controller detected.") };

            // Command and status register:
            //   bit 0: I/O access enable
            //   bit 1: memory access enable
            //   bit 2: enable mastering
            // SAFETY: `dsc` live (see above); volatile MMIO write.
            unsafe { core::ptr::write_volatile(&raw mut (*dsc).command, PCIE_CSCMD_IAE | PCIE_CSCMD_MAE | PCIE_CSCMD_BME) };
            fence(Ordering::SeqCst);

            for i in 0..6usize {
                // SAFETY: `dsc` live; `type_spec.header_type_0`
                // is the active union member for a type-0 (non-bridge)
                // header, which every PCI function this loop probes is
                // (qemu's e1000 device, the only match this branch
                // reaches); volatile MMIO read/write.
                let old = unsafe { core::ptr::read_volatile(&raw const (*dsc).type_spec.header_type_0.base_addr[i]) };

                // Writing all 1's to the BAR causes it to be replaced
                // with its size.
                // SAFETY: same as above.
                unsafe { core::ptr::write_volatile(&raw mut (*dsc).type_spec.header_type_0.base_addr[i], 0xffffffff) };
                fence(Ordering::SeqCst);

                // SAFETY: same as above.
                unsafe { core::ptr::write_volatile(&raw mut (*dsc).type_spec.header_type_0.base_addr[i], old) };
            }

            // Tell the e1000 to reveal its registers at physical address
            // 0x40000000.
            // SAFETY: `dsc` live; volatile MMIO write.
            unsafe { core::ptr::write_volatile(&raw mut (*dsc).type_spec.header_type_0.base_addr[0], e1000_regs as u32) };

            // `e1000_regs` is the physical address just programmed into
            // BAR0 above, mapped by `vm_pgtab.rs`'s `kvmmake` (see the
            // module doc's "We'll place..." comment).
            E1000::init(e1000_regs as *mut u32);
        }
    }
}
