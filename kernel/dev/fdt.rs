//! Flattened Device Tree (FDT) parser -- Rust port of `kernel/dev/fdt.c`
//! (Phase 2 Wave 23, XL, see `docs/rustify/phase2_plan.md`).
//!
//! # Boot-order hazard
//!
//! This module runs at the **earliest** point in boot: `start_kernel.c`
//! calls [`fdt_early_scan_memory`] before the buddy allocator exists (to
//! size it), then [`fdt_init`] + [`fdt_apply_platform_config`] right
//! after `early_allocator_init`/`kobject_init`/`printfinit`, before
//! `sbi_init`, `ksyms_init`, `kinit` (the real page allocator), or
//! `kvminit`. Every allocation in this file therefore goes through the
//! bump-pointer `early_alloc_align` (never `slab`/`kalloc`), and nothing
//! here may take a lock (the C original never did either -- no
//! `spin_lock`/`rwsem` call anywhere in `kernel/dev/fdt.c`) or sleep.
//! [`fdt_apply_platform_config`] writes the globals every later subsystem
//! reads to find its hardware: `__physical_memory_{start,end}` /
//! `__physical_total_pages` (mm/page.rs's `kernbase`/`phystop`/
//! `totalpages`), `__uart0_*` (uart.rs), `__plic_mmio_base` (irq/plic.rs),
//! `__pcie_ecam_mmio_base` (pci.c, still C), `__virtio_mmio_base`/
//! `__virtio_irqno` (virtio_disk.c, still C), and `__jiff_ticks`
//! (timer/timer_core.rs). A bug here corrupts every one of those
//! subsystems' boot-time configuration -- see the module doc's
//! "Written-globals inventory" section in the Wave 23 report for the
//! full list with exact types.
//!
//! # Internal data structures -- not part of the public C ABI
//!
//! `struct fdt_node` / `struct fdt_blob_info` / `struct fdt_compat_link` /
//! the two hash-table node types are defined in full in
//! `kernel/inc/dev/fdt.h` (the header stays, unchanged, as this module's
//! public C ABI surface -- see the plan's "wrapper.h coverage" rule), but
//! a full-tree grep (verified before starting this port) shows **no**
//! other `.c`/`.rs` file ever touches their fields or constructs one --
//! only `struct platform_info platform` (the public output) and the
//! `fdt_*` function prototypes are live ABI. This module therefore
//! defines its own internal Rust types ([`FdtNode`], [`FdtBlobInfo`],
//! [`FdtCompatLink`], [`FdtCompatHashNode`], [`FdtPhandleHashNode`]) with
//! whatever layout is convenient -- they never cross the FFI boundary as
//! anything but an opaque pointer (the ten `#[no_mangle]` entry points
//! below match `kernel/inc/dev/fdt.h`'s prototypes exactly in
//! name/arity/calling convention; the pointee type is opaque on both
//! sides of that boundary, exactly like a C forward-declared-only
//! pointer). Two fields the C original carried are dropped outright as
//! provably dead (verified by grepping every use in the 1844-line
//! original): `fdt_blob_info::original_header`/`::boot_cpuid_phys` (write
//! -only) and `fdt_node::compat_links` (the C comment at
//! `__fdt_index_compat_string`'s call site says outright: "we don't add
//! link to fdt_node's compat_links here ... not used for now" -- and
//! grep confirms nothing ever reads it) and `fdt_node::truncated` (never
//! read or written anywhere).
//!
//! # Preserved pre-existing bugs (1:1 fidelity, Wave 23 charter)
//!
//! * **`fdt_node::fdt_type` is never assigned `FDT_BEGIN_NODE`.** The C
//!   original zero-initializes every node (`memset(node, 0, ...)`) and
//!   then never writes `->fdt_type` anywhere in the file; its one
//!   consumer, `__fdt_build_indexes`'s `if (node->fdt_type ==
//!   FDT_BEGIN_NODE)` guard, therefore always evaluates false (0 !=
//!   `FDT_BEGIN_NODE` = 1). This makes `__fdt_index_node_compat`/
//!   `__fdt_index_node_phandle` **dead code**: the compat/phandle hash
//!   tables are allocated but always end up empty, so
//!   [`fdt_compat_lookup`]/[`fdt_phandle_lookup`]/[`fdt_compat_next`]
//!   always return NULL, and the SDHCI clock-controller-phandle
//!   resolution in `fdt_extract_platform_info` (which calls
//!   `fdt_phandle_lookup`) never finds anything, leaving
//!   `platform.sdhci[i].apmu_base`/`apbc_base` at 0 whenever they'd
//!   otherwise come from that path. Preserved exactly -- the field, the
//!   dead guard, and the empty tables -- for 1:1 fidelity; not exercised
//!   on the QEMU boot path (SDHCI probes nothing on `virt`) so this has
//!   zero effect on the verified boot log.
//! * **The C `fdt_parse_namestring`'s `copy` parameter is always
//!   `false`.** All four call sites in the original pass `false`;
//!   verified by grep. This port therefore drops the dead `copy=true`
//!   branch (the `memcpy` into `node->name`) and the parameter itself --
//!   the real per-node name copy always happened via the separate
//!   explicit `memcpy` in `fdt_build_blob_info`'s node/prop-creation
//!   branches, which this port keeps.
//! * **`__fdt_node_first(NULL, "")` latent null-deref** in
//!   `fdt_extract_platform_info`'s root-node lookup (see that function's
//!   doc comment below) is preserved as-is; it is unreachable for any
//!   DTB whose root has at least one child (true of every real-world
//!   DTB, including QEMU's).
//!
//! # Fixed bug: `bswap64`/`fdt64_to_cpu`
//!
//! `kernel/inc/bits.h`'s `bswap64(x)` macro has two independent defects:
//! a `||` (logical OR) typo where every other term uses `|` (bitwise
//! OR), and a 17-hex-digit literal (`0x00FF0000000000000ULL`, one digit
//! too many) for the byte-6 mask. Because `|` binds tighter than `||` in
//! C, the whole macro body parses as `(five terms ORed together) ||
//! (three terms ORed together)` -- a boolean, not a byte-swapped value:
//! `bswap64(x)` returns exactly `0` or `1` for any input where enough
//! low bytes are set (verified empirically: `gcc` on
//! `x = 0x0000048000000000` returns `0x1`, not the swapped value). The
//! only two call sites (`fdt_build_blob_info`'s `/memreserve/` entry
//! parsing) feed this into `blob->reserved[i].{base,size}` ->
//! `platform.reserved[]` -> `mm/page.rs`'s buddy-init exclusion list, so
//! a real `/memreserve/` entry would have corrupted the physical
//! allocator's reserved-region list. **Verified dead on the QEMU
//! verification target**: this wave's pre-port baseline boot log's "Reserved
//! regions: 2" comes entirely from the `/reserved-memory` *node* path
//! (`__fdt_prop_u32`-based, unaffected by this macro) -- the header's
//! `/memreserve/` block has zero entries on `-machine virt`'s generated
//! DTB, confirmed by the loop that counts `mem_rsvmap` entries never
//! running (`rsv_count` stays 0). Fixing this cannot change the verified
//! boot log. [`fdt64_to_cpu`] below is implemented correctly
//! (`u64::swap_bytes`) rather than reproducing the boolean bug, since
//! leaving a "byte swap" that silently returns 0/1 in new Rust code
//! would be strictly worse than fixing it: the original C artifact that
//! would let a future reader notice the bug is gone once this file is
//! deleted.

#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};
use core::mem::{align_of, offset_of, size_of};
use core::ptr;

use crate::bindings::{
    list_node_t, mem_region, platform_info, rb_node, rb_root, rb_root_opts, EMAC_MAX, N_VIRTIO,
    PCIE_REG_MAX, SDHCI_MAX,
};
use crate::hlist::{Hlist, HlistEntry, HlistFunc, HtHash};
use crate::machine;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N6 (mm type family, allocator-POD slice).
//
// This IS the kernel-wide Rust definition of `kernel/inc/dev/fdt.h`'s
// `struct mem_region` now: `build.rs` blocklists the bindgen-generated
// form and injects a `pub use crate::dev::fdt::MemRegion as mem_region;`
// facade re-export (no `_t` typedef exists), so this file's `mem_region`
// import above resolves right back here. Field names/types and the
// `#[repr(C, packed)]` (the header's `__PACKED`) reproduce bindgen's
// exactly; derived Copy/Clone exactly as the pre-nativization bindgen
// output did — the still-bindgen `platform_info` embeds
// `[mem_region; 8]` BY VALUE and derives Copy/Clone itself, so the
// accurate Copy=Yes in build.rs's NativeTypeCallbacks is what keeps
// *its* derive line unchanged.
//
// Layout evidence (P3-N6): temporary in-tree `offset_of!` gate on the
// live bindgen form + cross-compiler `_Static_assert` probe (toolchain
// gcc, rv64gc/lp64d — scratchpad p3n6_static_assert_probe.c); the two
// agree on every value (16/1, base@0, size@8).
// ---------------------------------------------------------------------------

/// `struct mem_region` (`kernel/inc/dev/fdt.h`) — one physical memory
/// range parsed from the device tree (`__PACKED`: byte-aligned).
#[repr(C, packed)]
#[derive(Copy, Clone)]
pub struct MemRegion {
    pub base: crate::bindings::uint64,
    pub size: crate::bindings::uint64,
}

// P3-N6 hardcoded layout proof — gate + probe agree (see above).
const _: () = {
    assert!(size_of::<MemRegion>() == 16, "mem_region size");
    assert!(align_of::<MemRegion>() == 1, "mem_region align (__PACKED)");
    assert!(offset_of!(MemRegion, base) == 0, "mem_region.base");
    assert!(offset_of!(MemRegion, size) == 8, "mem_region.size");
};

// ===========================================================================
// Native `platform_info` — P3-4c nativization (user directive: remove
// the C-compatible interfaces; platform scrutiny class). `PlatformInfo`
// (plus the three named anonymous-struct shells below) is the canonical
// definition of `kernel/inc/dev/fdt.h`'s `struct platform_info`:
// `build.rs` blocklists the bindgen emissions
// (`platform_info(__bindgen_ty_[123])?`) and re-exports `PlatformInfo`
// as `crate::bindings::platform_info` (facade `pub use`, N2 pattern).
// The three inner shells replace bindgen's
// `platform_info__bindgen_ty_{1,2,3}` under proper names — every
// consumer (`mm/vm_pgtab.rs`, `dev/x1_emac.rs`, `dev/x1_sdhci.rs`)
// reaches them only through `platform.pcie_reg[i].base`-style FIELD
// access, so no consumer re-pointing is needed (verified: zero
// spelled-out `__bindgen_ty` type mentions outside the emission).
//
// *** BOOT-CRITICAL LAYOUT — HANDLE WITH P3-4 SCRUTINY *** `fdt_init`
// populates the `platform` static below from the device tree before
// the allocator even exists, and everything from `kvmmake`'s MMIO
// mappings to the virtio/uart/plic probes reads it. The array lengths
// are tied to the same header macros bindgen folded (`PCIE_REG_MAX`/
// `EMAC_MAX`/`SDHCI_MAX` via `crate::bindings`, `MAX_MEM_REGIONS` == 8
// asserted against `platform_info_mem_regions()` below) so the consts
// and the layout can never drift apart.
//
// DERIVE DECISIONS (P3-4c): Copy + Clone on all four, exactly as the
// pre-nativization bindgen output derived (POD scalars/pointers; the
// embedded `[MemRegion; 8]` answers Copy=Yes via its own P3-N6 entry).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + toolchain-gcc `_Static_assert` probe (rv64gc/lp64d —
// scratchpad p3_4c_dma_probe.c); both agree on every value asserted
// below.
// ===========================================================================

/// One PCIe MMIO region of [`PlatformInfo::pcie_reg`] (the header's
/// first anonymous struct; bindgen's `platform_info__bindgen_ty_1`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PlatformPcieReg {
    /// Region physical base address.
    pub base: crate::bindings::uint64,
    /// Region size (bytes).
    pub size: crate::bindings::uint64,
    /// Region name from `reg-names` (null if not available).
    pub name: *const core::ffi::c_char,
}

/// One EMAC instance of [`PlatformInfo::emac`] (the header's second
/// anonymous struct; bindgen's `platform_info__bindgen_ty_2`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PlatformEmac {
    /// MMIO register base.
    pub base: crate::bindings::uint64,
    /// MMIO region size.
    pub size: crate::bindings::uint64,
    /// Interrupt number.
    pub irq: crate::bindings::uint32,
    /// APMU register base (for ctrl/dline regs).
    pub apmu_base: crate::bindings::uint32,
    /// Ctrl register offset from `apmu_base`.
    pub ctrl_reg: crate::bindings::uint32,
    /// Delay-line register offset from `apmu_base`.
    pub dline_reg: crate::bindings::uint32,
    /// GPIO pin for PHY reset.
    pub reset_gpio: crate::bindings::uint32,
    /// RGMII TX clock phase.
    pub tx_phase: crate::bindings::uint32,
    /// RGMII RX clock phase.
    pub rx_phase: crate::bindings::uint32,
}

/// One SDHCI instance of [`PlatformInfo::sdhci`] (the header's third
/// anonymous struct; bindgen's `platform_info__bindgen_ty_3`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PlatformSdhci {
    /// MMIO register base.
    pub base: crate::bindings::uint64,
    /// MMIO region size.
    pub size: crate::bindings::uint64,
    /// Interrupt number.
    pub irq: crate::bindings::uint32,
    /// APMU base address (from clock-controller DT).
    pub apmu_base: crate::bindings::uint32,
    /// APMU register offset for this instance.
    pub apmu_offset: crate::bindings::uint32,
    /// APMU shared AXI register offset (SDH0's).
    pub apmu_axi_offset: crate::bindings::uint32,
    /// APBC base address (from clock-controller DT).
    pub apbc_base: crate::bindings::uint32,
    /// 1 if eMMC instance.
    pub is_emmc: c_int,
    /// 1 if SDIO instance (WiFi, skip).
    pub is_sdio: c_int,
    /// Max bus width (1, 4, or 8).
    pub bus_width: c_int,
}

/// Native `struct platform_info` (`kernel/inc/dev/fdt.h`) — the probed
/// platform description populated by `fdt_init` from the device tree.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PlatformInfo {
    /// Memory regions (may have multiple banks; `MAX_MEM_REGIONS`).
    pub mem: [MemRegion; 8],
    /// Number of valid entries in `mem`.
    pub mem_count: c_int,
    /// Reserved memory regions (from `/memreserve/`, `/reserved-memory`).
    pub reserved: *mut MemRegion,
    /// Number of entries at `reserved`.
    pub reserved_count: c_int,
    /// Ramdisk region base (pre-loaded filesystem image).
    pub ramdisk_base: crate::bindings::uint64,
    /// Ramdisk region size.
    pub ramdisk_size: crate::bindings::uint64,
    /// Non-zero if a ramdisk region was found.
    pub has_ramdisk: c_int,
    /// Total memory (sum of all regions).
    pub total_mem: crate::bindings::uint64,
    /// UART MMIO base.
    pub uart_base: crate::bindings::uint64,
    /// UART interrupt number.
    pub uart_irq: crate::bindings::uint32,
    /// UART clock frequency in Hz (0 = unknown, use default).
    pub uart_clock: crate::bindings::uint32,
    /// Desired baud rate (0 = default 115200).
    pub uart_baud: crate::bindings::uint32,
    /// Register spacing shift (0 = 1-byte, 2 = 4-byte).
    pub uart_reg_shift: crate::bindings::uint32,
    /// Register I/O width (1 = 8-bit, 4 = 32-bit).
    pub uart_reg_io_width: crate::bindings::uint32,
    /// PLIC MMIO base.
    pub plic_base: crate::bindings::uint64,
    /// PLIC MMIO region size.
    pub plic_size: crate::bindings::uint64,
    /// Non-zero if PCIe was found.
    pub has_pcie: c_int,
    /// PCIe MMIO regions (dbi/atu/config — `PCIE_REG_*` indices).
    pub pcie_reg: [PlatformPcieReg; crate::bindings::PCIE_REG_MAX as usize],
    /// Number of valid entries in `pcie_reg`.
    pub pcie_reg_count: c_int,
    /// Non-zero if virtio-mmio devices were found.
    pub has_virtio: c_int,
    /// Virtio-mmio device bases.
    pub virtio_base: [crate::bindings::uint64; 8],
    /// Virtio-mmio device IRQs.
    pub virtio_irq: [crate::bindings::uint32; 8],
    /// Number of valid virtio entries.
    pub virtio_count: c_int,
    /// Timebase frequency.
    pub timebase_freq: crate::bindings::uint64,
    /// Number of CPUs.
    pub ncpu: c_int,
    /// Non-zero if an EMAC was found.
    pub has_emac: c_int,
    /// EMAC instances (MMIO-based, e.g. SpacemiT X1 EMAC).
    pub emac: [PlatformEmac; crate::bindings::EMAC_MAX as usize],
    /// Number of valid entries in `emac`.
    pub emac_count: c_int,
    /// Non-zero if SDHCI controllers were found.
    pub has_sdhci: c_int,
    /// SDHCI SD/eMMC controllers (SpacemiT X1).
    pub sdhci: [PlatformSdhci; crate::bindings::SDHCI_MAX as usize],
    /// Number of valid entries in `sdhci`.
    pub sdhci_count: c_int,
}

// P3-4c hardcoded layout proof — the boot-critical platform record
// (`kernel/inc/dev/fdt.h` `struct platform_info` + its three anonymous
// structs), every field. Values captured from the pre-nativization
// bindgen output via the temporary in-tree `offset_of!` gate and
// cross-checked by the toolchain-gcc probe.
const _: () = {
    assert!(size_of::<PlatformPcieReg>() == 24, "pcie_reg elem size");
    assert!(align_of::<PlatformPcieReg>() == 8, "pcie_reg elem align");
    assert!(offset_of!(PlatformPcieReg, base) == 0, "pcie_reg.base");
    assert!(offset_of!(PlatformPcieReg, size) == 8, "pcie_reg.size");
    assert!(offset_of!(PlatformPcieReg, name) == 16, "pcie_reg.name");

    assert!(size_of::<PlatformEmac>() == 48, "emac elem size");
    assert!(align_of::<PlatformEmac>() == 8, "emac elem align");
    assert!(offset_of!(PlatformEmac, base) == 0, "emac.base");
    assert!(offset_of!(PlatformEmac, size) == 8, "emac.size");
    assert!(offset_of!(PlatformEmac, irq) == 16, "emac.irq");
    assert!(offset_of!(PlatformEmac, apmu_base) == 20, "emac.apmu_base");
    assert!(offset_of!(PlatformEmac, ctrl_reg) == 24, "emac.ctrl_reg");
    assert!(offset_of!(PlatformEmac, dline_reg) == 28, "emac.dline_reg");
    assert!(offset_of!(PlatformEmac, reset_gpio) == 32, "emac.reset_gpio");
    assert!(offset_of!(PlatformEmac, tx_phase) == 36, "emac.tx_phase");
    assert!(offset_of!(PlatformEmac, rx_phase) == 40, "emac.rx_phase");

    assert!(size_of::<PlatformSdhci>() == 48, "sdhci elem size");
    assert!(align_of::<PlatformSdhci>() == 8, "sdhci elem align");
    assert!(offset_of!(PlatformSdhci, base) == 0, "sdhci.base");
    assert!(offset_of!(PlatformSdhci, size) == 8, "sdhci.size");
    assert!(offset_of!(PlatformSdhci, irq) == 16, "sdhci.irq");
    assert!(offset_of!(PlatformSdhci, apmu_base) == 20, "sdhci.apmu_base");
    assert!(offset_of!(PlatformSdhci, apmu_offset) == 24, "sdhci.apmu_offset");
    assert!(offset_of!(PlatformSdhci, apmu_axi_offset) == 28, "sdhci.apmu_axi_offset");
    assert!(offset_of!(PlatformSdhci, apbc_base) == 32, "sdhci.apbc_base");
    assert!(offset_of!(PlatformSdhci, is_emmc) == 36, "sdhci.is_emmc");
    assert!(offset_of!(PlatformSdhci, is_sdio) == 40, "sdhci.is_sdio");
    assert!(offset_of!(PlatformSdhci, bus_width) == 44, "sdhci.bus_width");

    assert!(size_of::<PlatformInfo>() == 816, "platform_info size");
    assert!(align_of::<PlatformInfo>() == 8, "platform_info align");
    assert!(offset_of!(PlatformInfo, mem) == 0, "pi.mem");
    assert!(platform_info_mem_regions() == 8, "pi.mem length == MAX_MEM_REGIONS");
    assert!(offset_of!(PlatformInfo, mem_count) == 128, "pi.mem_count");
    assert!(offset_of!(PlatformInfo, reserved) == 136, "pi.reserved");
    assert!(offset_of!(PlatformInfo, reserved_count) == 144, "pi.reserved_count");
    assert!(offset_of!(PlatformInfo, ramdisk_base) == 152, "pi.ramdisk_base");
    assert!(offset_of!(PlatformInfo, ramdisk_size) == 160, "pi.ramdisk_size");
    assert!(offset_of!(PlatformInfo, has_ramdisk) == 168, "pi.has_ramdisk");
    assert!(offset_of!(PlatformInfo, total_mem) == 176, "pi.total_mem");
    assert!(offset_of!(PlatformInfo, uart_base) == 184, "pi.uart_base");
    assert!(offset_of!(PlatformInfo, uart_irq) == 192, "pi.uart_irq");
    assert!(offset_of!(PlatformInfo, uart_clock) == 196, "pi.uart_clock");
    assert!(offset_of!(PlatformInfo, uart_baud) == 200, "pi.uart_baud");
    assert!(offset_of!(PlatformInfo, uart_reg_shift) == 204, "pi.uart_reg_shift");
    assert!(offset_of!(PlatformInfo, uart_reg_io_width) == 208, "pi.uart_reg_io_width");
    assert!(offset_of!(PlatformInfo, plic_base) == 216, "pi.plic_base");
    assert!(offset_of!(PlatformInfo, plic_size) == 224, "pi.plic_size");
    assert!(offset_of!(PlatformInfo, has_pcie) == 232, "pi.has_pcie");
    assert!(offset_of!(PlatformInfo, pcie_reg) == 240, "pi.pcie_reg");
    assert!(offset_of!(PlatformInfo, pcie_reg_count) == 432, "pi.pcie_reg_count");
    assert!(offset_of!(PlatformInfo, has_virtio) == 436, "pi.has_virtio");
    assert!(offset_of!(PlatformInfo, virtio_base) == 440, "pi.virtio_base");
    assert!(offset_of!(PlatformInfo, virtio_irq) == 504, "pi.virtio_irq");
    assert!(offset_of!(PlatformInfo, virtio_count) == 536, "pi.virtio_count");
    assert!(offset_of!(PlatformInfo, timebase_freq) == 544, "pi.timebase_freq");
    assert!(offset_of!(PlatformInfo, ncpu) == 552, "pi.ncpu");
    assert!(offset_of!(PlatformInfo, has_emac) == 556, "pi.has_emac");
    assert!(offset_of!(PlatformInfo, emac) == 560, "pi.emac");
    assert!(offset_of!(PlatformInfo, emac_count) == 656, "pi.emac_count");
    assert!(offset_of!(PlatformInfo, has_sdhci) == 660, "pi.has_sdhci");
    assert!(offset_of!(PlatformInfo, sdhci) == 664, "pi.sdhci");
    assert!(offset_of!(PlatformInfo, sdhci_count) == 808, "pi.sdhci_count");
};

// ===========================================================================
// Externs -- printf/allocator/string primitives (declared locally per this
// crate's established convention: every file redeclares the C-ABI symbols
// it needs rather than reaching into another module's private items).
// ===========================================================================
// P3-D3c: `early_alloc_align` (`mm/early_allocator.rs`; the only allocator
// live at the point `fdt_init`/`fdt_early_scan_memory` run) is a plain
// crate-path import instead of an `extern "C"` redeclaration (demoted from
// `#[no_mangle]` in the same wave). Same for the boot-configured globals
// this file writes (see the write sites in `fdt_apply_platform_config`):
// `__physical_memory_*` (`start_kernel.rs`), `__plic_mmio_base`
// (`irq/plic.rs`), `__jiff_ticks` (`timer/timer_core.rs`).
use crate::mm::early_allocator::early_alloc_align;
unsafe extern "C" {
    /// Variadic; see `printf.rs`'s module doc for the ABI rationale.

    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    fn strcmp(p: *const c_char, q: *const c_char) -> c_int;
    fn strncmp(p: *const c_char, q: *const c_char, n: usize) -> c_int;
    fn strlen(s: *const c_char) -> usize;
    fn strnlen(s: *const c_char, maxlen: usize) -> usize;
}
// P3-1D mesh sweep: pci.rs/virtio_disk.rs are in scope for this wave;
// default-initialized nonzero, this file only overwrites them when the
// matching FDT node is actually probed. These become plain crate-path
// imports instead of `extern "C"` redeclarations (both demoted from
// `#[no_mangle]` in the same wave, each with only one other reader,
// `mm/vm_pgtab.rs`, also fixed up).
use crate::pci::__pcie_ecam_mmio_base;
use crate::virtio_disk::{__virtio_irqno, __virtio_mmio_base};

/// `HZ` (`kernel/inc/timer/timer.h`) -- simple integer macro, redeclared
/// locally per this crate's established convention (see `printf.rs`'s
/// `PAGE_SHIFT`).
const HZ: u64 = 1000;

// ===========================================================================
// Global platform info -- populated by `fdt_init`, consumed by
// `mm/{page,vm_pgtab}.rs`, `mm/kalloc` (transitively), and the driver
// ports (`x1_emac.rs`, `x1_sdhci.rs`, `virtio_disk.rs`, `ramdisk.rs`).
// Typed via the unchanged `crate::bindings::platform_info` facade path
// (the native [`PlatformInfo`] above since P3-4c) -- exact same struct
// every reader already expects.
// ===========================================================================
// P3-D3c: `#[no_mangle]` dropped -- every reader (`mm/page.rs`,
// `mm/vm_pgtab.rs`, `dev/x1_emac.rs`, `dev/x1_sdhci.rs`, `ramdisk.rs`,
// `virtio_disk.rs`) now reaches it by crate path (directly or through a
// thin accessor in its own `ffi` module) instead of an
// `extern`/`bindings::` redeclaration of the symbol.
pub(crate) static mut platform: platform_info = unsafe { core::mem::zeroed() };

// ===========================================================================
// FDT token/header constants (`kernel/inc/dev/fdt.h`).
// ===========================================================================
const FDT_MAGIC: u32 = 0xd00d_feed;
const FDT_BEGIN_NODE: u32 = 0x0000_0001;
const FDT_END_NODE: u32 = 0x0000_0002;
const FDT_PROP: u32 = 0x0000_0003;
const FDT_NOP: u32 = 0x0000_0004;
const FDT_END: u32 = 0x0000_0009;

const FDT_MAX_DEPTH: usize = 32;
const FDT_COMPAT_HASH_BUCKETS: u64 = 64;
const FDT_PHANDLE_HASH_BUCKETS: u64 = 32;

// `fdt_header` field byte offsets (`kernel/inc/dev/fdt.h`'s `__PACKED`
// struct); read via `read_be32` below rather than a Rust struct
// definition, matching the C original's own `Fdt::fdt_get_header(dtb, off)`
// macro approach exactly.
const FDT_HDR_MAGIC: usize = 0;
const FDT_HDR_TOTALSIZE: usize = 4;
const FDT_HDR_OFF_DT_STRUCT: usize = 8;
const FDT_HDR_OFF_DT_STRINGS: usize = 12;
const FDT_HDR_OFF_MEM_RSVMAP: usize = 16;

// ===========================================================================
// Fdt -- N-OO wave (KERNEL-OO) marker/namespace type. Zero-sized, never
// instantiated: it exists purely so the raw-blob-level helpers and the
// no-receiver orchestration entry points (`fdt_init`,
// `fdt_early_scan_memory`, `fdt_apply_platform_config`, `fdt_dump`/
// `fdt_walk`, plus the generic byte-swap/hash/list utilities) become
// associated functions instead of free-standing top-level fns. Mirrors
// the `Scheduler`/`sched.rs` marker precedent (`baf650b`): raw pointer
// params are kept exactly as the C original had them ("keeping raw
// params -- namespacing only, zero behavior risk"), so every body below
// is byte-identical to its pre-wave free-fn form modulo the `Type::`
// qualification at call sites.
// ===========================================================================
pub(crate) struct Fdt;

// ===========================================================================
// Byte-swap + unaligned-read helpers. FDT structure-block tokens/props are
// only 4-byte aligned (not 8), so every multi-byte read below goes through
// `ptr::read_unaligned` -- never a direct `*(T*)p` deref.
// ===========================================================================

impl Fdt {
/// Mirrors `fdt32_to_cpu`/`bswap32` -- correct in the C original (plain
/// `|`, well-formed literals); `u32::swap_bytes` is definitionally the
/// same byte reversal.
#[inline(always)]
fn fdt32_to_cpu(x: u32) -> u32 {
    x.swap_bytes()
}
}

impl Fdt {
/// Mirrors `fdt64_to_cpu`/`bswap64` -- **fixed**, see this module's doc
/// comment ("Fixed bug: bswap64/fdt64_to_cpu") for the discovered C bug
/// and why fixing it cannot change the verified boot log.
#[inline(always)]
fn fdt64_to_cpu(x: u64) -> u64 {
    x.swap_bytes()
}
}

impl Fdt {
/// Read a big-endian `u32` at a possibly-unaligned byte offset from `base`.
///
/// # Safety
/// `base + off .. base + off + 4` must be readable memory.
#[inline(always)]
unsafe fn read_be32(base: *const u8, off: usize) -> u32 {
    // SAFETY: caller contract.
    Fdt::fdt32_to_cpu(unsafe { ptr::read_unaligned(base.add(off) as *const u32) })
}
}

impl Fdt {
/// Read a big-endian `u64` at a possibly-unaligned byte offset from `base`.
///
/// # Safety
/// `base + off .. base + off + 8` must be readable memory.
#[inline(always)]
unsafe fn read_be64(base: *const u8, off: usize) -> u64 {
    // SAFETY: caller contract.
    Fdt::fdt64_to_cpu(unsafe { ptr::read_unaligned(base.add(off) as *const u64) })
}
}

impl Fdt {
/// Mirrors the C `Fdt::fdt_get_header(dtb, offset)` macro.
///
/// # Safety
/// `dtb` must be a valid FDT blob pointer with at least `offset + 4`
/// readable bytes.
#[inline(always)]
unsafe fn fdt_get_header(dtb: *const u8, offset: usize) -> u32 {
    // SAFETY: caller contract.
    unsafe { Fdt::read_be32(dtb, offset) }
}
}

impl Fdt {
/// Mirrors the C `Fdt::fdt_valid(dtb)` macro. Not `#[no_mangle]`: the C header
/// declares `int Fdt::fdt_valid(void *dtb);` as a real function prototype, but
/// `kernel/dev/fdt.c` only ever defines it as this same-named macro
/// (shadowing the prototype within that one translation unit) -- grep
/// confirms no other `.c` file in the tree ever calls `Fdt::fdt_valid()` as an
/// actual function, so no such symbol was ever linkable, and none needs
/// to be exported here either.
///
/// # Safety
/// `dtb` must be null or point to at least 4 readable bytes.
#[inline(always)]
unsafe fn fdt_valid(dtb: *mut c_void) -> bool {
    !dtb.is_null() && unsafe { Fdt::fdt_get_header(dtb as *const u8, FDT_HDR_MAGIC) } == FDT_MAGIC
}
}

impl Fdt {
/// Mirrors the C `Fdt::fdt_totalsize(dtb)` macro (same never-a-real-symbol
/// situation as [`fdt_valid`] above).
///
/// # Safety
/// `dtb` must point to at least 8 readable bytes.
#[inline(always)]
unsafe fn fdt_totalsize(dtb: *mut c_void) -> u32 {
    // SAFETY: caller contract.
    unsafe { Fdt::fdt_get_header(dtb as *const u8, FDT_HDR_TOTALSIZE) }
}
}

/// Mirrors the C `fdt_align(x)` macro: round up to a 4-byte boundary.
#[inline(always)]
const fn fdt_align(x: usize) -> usize {
    (x + 3) & !3
}

impl Fdt {
/// Mirrors `Fdt::fdt_get_string()`.
///
/// # Safety
/// `dtb` must be a valid FDT blob; `offset` must be a valid offset into
/// its strings block.
unsafe fn fdt_get_string(dtb: *const u8, offset: u32) -> *const c_char {
    // SAFETY: caller contract.
    let str_off = unsafe { Fdt::fdt_get_header(dtb, FDT_HDR_OFF_DT_STRINGS) };
    unsafe { dtb.add(str_off as usize + offset as usize) as *const c_char }
}
}

// ===========================================================================
// fdt_early_scan_memory -- lightweight linear scan, no allocation, no tree.
// ===========================================================================

impl Fdt {
/// Rust port of `Fdt::fdt_early_scan_memory()`.
///
/// # Safety
/// `dtb` must be null or a valid FDT blob pointer; `base_out`/`size_out`
/// must be null or valid `*mut u64` out-params.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn fdt_early_scan_memory(
    dtb: *mut c_void,
    base_out: *mut u64,
    size_out: *mut u64,
) -> c_int {
    // SAFETY: caller contract.
    if !unsafe { Fdt::fdt_valid(dtb) } || base_out.is_null() || size_out.is_null() {
        return -1;
    }

    let base = dtb as *const u8;
    // SAFETY: `dtb` valid per the check above.
    let mut p = unsafe { Fdt::fdt_get_header(base, FDT_HDR_OFF_DT_STRUCT) } as usize;
    let mut depth: i32 = 0;
    let mut in_memory_node = false;
    let mut root_addr_cells: i32 = 2;
    let mut root_size_cells: i32 = 1;

    loop {
        // SAFETY: FDT structure-block tokens are always readable up to
        // `FDT_END`; the original C has no bounds check here either.
        let token = unsafe { Fdt::read_be32(base, p) };
        p += 4;

        match token {
            FDT_BEGIN_NODE => {
                let name = unsafe { base.add(p) as *const c_char };
                // SAFETY: `name` points into the FDT structure block,
                // NUL-terminated by the DTB format.
                let namelen = unsafe { strlen(name) };
                p += fdt_align(namelen + 1);

                depth += 1;
                if depth == 1 && namelen >= 6 {
                    // SAFETY: `name` valid per above; comparing 6 bytes is
                    // in-bounds since `namelen >= 6`.
                    if unsafe { strncmp(name, c"memory".as_ptr(), 6) } == 0 {
                        in_memory_node = true;
                    }
                }
            }
            FDT_END_NODE => {
                if in_memory_node && depth == 1 {
                    in_memory_node = false;
                }
                depth -= 1;
            }
            FDT_PROP => {
                // SAFETY: see the token read above.
                let len = unsafe { Fdt::read_be32(base, p) };
                p += 4;
                let nameoff = unsafe { Fdt::read_be32(base, p) };
                p += 4;
                // SAFETY: `nameoff` comes from the DTB's own prop header.
                let propname = unsafe { Fdt::fdt_get_string(base, nameoff) };
                let data = unsafe { base.add(p) };
                p += fdt_align(len as usize);

                if depth == 1 {
                    // SAFETY: `propname`/`data` valid per above; the length
                    // guard matches the C `len >= 4` check before reading a
                    // cell.
                    if len >= 4 && unsafe { strcmp(propname, c"#address-cells".as_ptr()) } == 0 {
                        root_addr_cells = unsafe { Fdt::read_be32(data, 0) } as i32;
                    } else if len >= 4
                        // SAFETY: see above.
                        && unsafe { strcmp(propname, c"#size-cells".as_ptr()) } == 0
                    {
                        root_size_cells = unsafe { Fdt::read_be32(data, 0) } as i32;
                    }
                }

                // SAFETY: see above.
                if in_memory_node && unsafe { strcmp(propname, c"reg".as_ptr()) } == 0 {
                    let base_val: u64;
                    // SAFETY: `data` points at the "reg" property's raw
                    // cell data, at least `len` bytes.
                    unsafe {
                        if root_addr_cells == 2 {
                            base_val =
                                ((Fdt::read_be32(data, 0) as u64) << 32) | Fdt::read_be32(data, 4) as u64;
                        } else {
                            base_val = Fdt::read_be32(data, 0) as u64;
                        }

                        let size_off = root_addr_cells as usize * 4;
                        let size_val: u64 = if root_size_cells == 2 {
                            ((Fdt::read_be32(data, size_off) as u64) << 32)
                                | Fdt::read_be32(data, size_off + 4) as u64
                        } else {
                            Fdt::read_be32(data, size_off) as u64
                        };

                        *base_out = base_val;
                        *size_out = size_val;
                    }
                    return 0;
                }
            }
            FDT_NOP => {}
            FDT_END => return -1,
            _ => return -1,
        }
    }
}
}

// ===========================================================================
// FdtNode -- internal tree node. Header fields followed by `data_size`
// bytes of property data, then `name_size + 1` bytes of NUL-terminated
// name, all in one `early_alloc_align`ed allocation (mirrors the C
// `struct fdt_node { ...; uint32 data[0]; }` flexible-array-member
// layout; Rust has no FAM, so the trailing bytes are addressed via raw
// offset arithmetic from `size_of::<FdtNode>()` instead of a `data[0]`
// field -- see `fdt_data_ptr`).
//
// `rb_entry` is the *first* field, so `*mut rb_node -> *mut FdtNode` is an
// offset-0 reinterpretation (mirrors the C `container_of(node, struct
// fdt_node, rb_entry)`, which is exactly that on this layout).
// ===========================================================================
#[repr(C)]
struct FdtNode {
    rb_entry: rb_node,
    list_entry: list_node_t,
    children: rb_root,
    name: *const c_char,
    hash: HtHash,
    addr: u64,
    data_size: u32,
    name_size: u32,
    child_count: i32,
    layer: u8,
    /// Preserved dead field -- see module doc's "Preserved pre-existing
    /// bugs": never written to `FDT_BEGIN_NODE`, so
    /// `fdt_type == FDT_BEGIN_NODE` is always false.
    fdt_type: u8,
    has_addr: bool,
    has_phandle: bool,
    phandle: u32,
}

impl FdtNode {
    /// # Safety
    /// `self` must be a live `FdtNode` allocated by [`FdtNode::fdt_create_node`]
    /// (i.e. with `data_size` bytes of property data immediately following
    /// the header). N-OO: true `&self` method (goal #1) -- every call site
    /// dereferences an already-non-null-checked `*mut FdtNode`, so forming
    /// `&self` here never derefs a null pointer.
    #[inline(always)]
    unsafe fn data_ptr(&self) -> *mut u8 {
        (self as *const FdtNode as *mut u8).wrapping_add(size_of::<FdtNode>())
    }
}

// ===========================================================================
// N-METH iterators (goal #2) -- the repetitive device-tree walks recast as
// `Iterator`s. Each keeps the blob/tree reads behind ONE bounds-/null-
// checked `next()` instead of the scattered offset/`rb_next_node`
// arithmetic the C walks open-coded. Every walk site in this file runs at
// boot, single-threaded, over a tree/blob that is immutable for the
// duration of the walk (see module doc), so the lazy successor is stable
// and the rewrite is behaviour-identical.
// ===========================================================================

/// Iterator over an [`FdtNode`]'s direct children (the rb-tree walk
/// `rb_first_node`/`rb_next_node` open-coded at every device/reserved-
/// memory/dump site). Yields each child as `*mut FdtNode`; `rb_entry` is
/// `FdtNode`'s first field so the `rb_node* -> FdtNode*` cast is offset-0
/// (mirrors the C `container_of`). The tree is not mutated during any
/// walk, so pre-advancing `cur` in `next()` is order-equivalent to the C
/// `node = rb; rb = rb_next_node(rb); <body>` shape.
struct FdtChildIter {
    cur: *mut rb_node,
}

impl Iterator for FdtChildIter {
    type Item = *mut FdtNode;

    fn next(&mut self) -> Option<*mut FdtNode> {
        if self.cur.is_null() {
            return None;
        }
        let node = self.cur as *mut FdtNode;
        // SAFETY: `self.cur` is a live rb_node in the tree (seeded by
        // `rb_first_node`, only ever advanced by `rb_next_node`); the tree
        // is immutable for the walk's duration (boot-time, single hart).
        self.cur = unsafe { crate::bintree::rb_next_node(self.cur) };
        Some(node)
    }
}

/// Iterator over the NUL-separated C strings packed in a property's data
/// block (the `while pos < data_size { strnlen; pos += len + 1 }` walk in
/// `fdt_prop_compat`/`fdt_index_node_compat`). Yields `(ptr, len)` per
/// string, `len` excluding the terminator.
struct FdtStrList {
    base: *const u8,
    len: usize,
    pos: usize,
}

impl Iterator for FdtStrList {
    type Item = (*const c_char, usize);

    fn next(&mut self) -> Option<(*const c_char, usize)> {
        if self.pos >= self.len {
            return None;
        }
        // SAFETY: `pos < len`, so `base + pos` is in-bounds of the
        // property's `len`-byte data block; `strnlen` is bounded by the
        // remaining `len - pos` bytes.
        let str_ptr = unsafe { self.base.add(self.pos) as *const c_char };
        let str_len = unsafe { strnlen(str_ptr, self.len - self.pos) };
        self.pos += str_len + 1;
        Some((str_ptr, str_len))
    }
}

impl FdtNode {
/// Iterator over a parent's children sharing `first`'s exact name (the
/// `fdt_node_first` + `fdt_node_next_same_name` walk used for `/cpus/cpu*`
/// and `/memory*`). Seeded with `first` (may be null -> empty). Uses
/// `core::iter::successors` (the established loop->iterator idiom in this
/// crate, cf. `vfs/file.rs`, `mm/pcache.rs`).
///
/// # Safety
/// `parent` must point to a live `FdtNode` outliving the iterator; `first`
/// must be null or a live child of `parent`.
unsafe fn fdt_same_name_iter(
    parent: *mut FdtNode,
    first: *mut FdtNode,
) -> impl Iterator<Item = *mut FdtNode> {
    core::iter::successors((!first.is_null()).then_some(first), move |&cur| {
        // SAFETY: `cur` is a live node just yielded; `parent` live for the
        // whole walk (caller contract); the tree is immutable at boot.
        let next = unsafe { FdtNode::fdt_node_next_same_name(parent, cur) };
        (!next.is_null()).then_some(next)
    })
}
}

impl FdtNode {
    /// Iterator over this node's direct children in rb-tree order
    /// (goal #1 method wrapping the [`FdtChildIter`] rb-tree walk).
    fn children(&self) -> FdtChildIter {
        // SAFETY: `&self.children` is a live `rb_root`; `rb_first_node`
        // only reads it. The `*const -> *mut` cast matches the C
        // `rb_first_node` signature (the walk never mutates the tree).
        let cur =
            unsafe { crate::bintree::rb_first_node(&self.children as *const rb_root as *mut rb_root) };
        FdtChildIter { cur }
    }

    /// Iterator over the NUL-separated strings packed in this (property)
    /// node's data block (goal #1 method wrapping [`FdtStrList`]).
    fn string_list(&self) -> FdtStrList {
        // SAFETY: `self` is a live `FdtNode`; `data_size` bytes of data
        // immediately follow the header (see [`fdt_data_ptr`]).
        let base = unsafe { self.data_ptr() } as *const u8;
        FdtStrList {
            base,
            len: self.data_size as usize,
            pos: 0,
        }
    }
}

impl Fdt {
/// Mirrors `Fdt::strtoul()` (`kernel/inc/string.h`, `static inline`, no
/// external linkage -- reimplemented locally per this crate's
/// established convention, e.g. `backtrace.rs`'s own copy). Only ever
/// called with `base = 16` in this file.
///
/// # Safety
/// `nptr` must be a valid NUL-terminated C string; `endptr`, if non-null,
/// must be a valid `*mut *mut c_char` out-param.
unsafe fn strtoul(nptr: *const c_char, endptr: *mut *mut c_char, base: i32) -> u64 {
    let mut result: u64 = 0;
    let mut p = nptr;
    loop {
        // SAFETY: `p` starts at `nptr` (caller contract) and only ever
        // advances past characters just read as non-NUL below.
        let c = unsafe { *p } as u8;
        if c == 0 {
            break;
        }
        let digit = if c.is_ascii_digit() {
            (c - b'0') as i32
        } else if (b'a'..=b'f').contains(&c) {
            (c - b'a' + 10) as i32
        } else if (b'A'..=b'F').contains(&c) {
            (c - b'A' + 10) as i32
        } else {
            break;
        };
        if digit >= base {
            break;
        }
        result = result.wrapping_mul(base as u64).wrapping_add(digit as u64);
        p = unsafe { p.add(1) };
    }
    if !endptr.is_null() {
        // SAFETY: caller contract.
        unsafe { *endptr = p as *mut c_char };
    }
    result
}
}

impl FdtNode {
    /// Rust port of `fdt_parse_namestring()` (goal #1 method: the receiver
    /// is the node being written). The C `copy` parameter is dropped --
    /// see module doc's "Preserved pre-existing bugs": every call site in
    /// the original passes `copy = false`, making the `memcpy` branch dead.
    /// Only `name_size`/`hash`/`has_addr`/`addr`/`name` are written.
    ///
    /// # Safety
    /// `namestring` must be a valid NUL-terminated C string outliving
    /// `self`.
    unsafe fn parse_namestring(&mut self, namestring: *const c_char, ignore_addr: bool) {
        // SAFETY: `namestring` valid per caller contract.
        let full_len = unsafe { strlen(namestring) };
        let mut len = 0usize;
        // SAFETY: `namestring` valid, NUL-terminated; this loop stops at
        // the terminator at the latest.
        while unsafe { *namestring.add(len) } != 0
            && unsafe { *namestring.add(len) } != b'@' as c_char
        {
            len += 1;
        }

        self.name_size = len as u32;
        // SAFETY: `namestring` valid; `len` bytes readable.
        self.hash = unsafe { Fdt::hlist_hash_str(namestring, len) };

        // SAFETY: `namestring` valid; `len <= full_len`.
        let at_terminator = unsafe { *namestring.add(len) } == 0;
        if at_terminator || ignore_addr {
            self.has_addr = false;
            self.addr = 0;
            self.name = namestring;
            return;
        }

        self.has_addr = true;

        let mut endptr: *mut c_char = ptr::null_mut();
        // SAFETY: `namestring + len + 1` is in-bounds (there's a '@' at
        // `len`, so at least a NUL follows).
        let addr_str = unsafe { namestring.add(len + 1) };
        let parsed = unsafe { Fdt::strtoul(addr_str, &raw mut endptr, 16) };

        // SAFETY: `addr_str`/`endptr` valid per above.
        if unsafe { endptr as *const c_char == addr_str || *endptr != 0 } {
            // SAFETY: `addr_str` valid; `full_len - len - 1` bytes readable.
            self.addr = unsafe { Fdt::hlist_hash_str(addr_str, full_len - len - 1) };
        } else {
            self.addr = parsed;
        }
    }
}

impl Fdt {
/// Mirrors `Fdt::hlist_hash_str()` (`kernel/inc/hlist.h`, `static inline`, no
/// external linkage) -- reimplemented locally per this crate's
/// established precedent (`vfs/tmpfs/inode.rs`'s identical copy; see that
/// file's doc comment for why `ptr::read_unaligned` replaces the C's
/// raw `*(ht_hash_t*)(str+i)` cast-and-deref).
///
/// # Safety
/// `s` must point to at least `len` readable bytes.
unsafe fn hlist_hash_str(s: *const c_char, len: usize) -> HtHash {
    const GOLDEN_RATIO_PRIME_64: u64 = 0x9e37_ffff_fffc_0001;
    // SAFETY: caller contract.
    unsafe {
        let bytes = s as *const u8;
        let mut ret = GOLDEN_RATIO_PRIME_64.wrapping_mul(len as u64);
        let tail_size = len % size_of::<u64>();
        let mut i = 0usize;
        while i < len - tail_size {
            let word = ptr::read_unaligned(bytes.add(i) as *const u64);
            ret ^= word.wrapping_mul(GOLDEN_RATIO_PRIME_64);
            i += size_of::<u64>();
        }
        if tail_size > 0 {
            let mut tail: u64 = 0;
            for j in (len - tail_size)..len {
                tail = (tail << 8) | u64::from(*bytes.add(j));
            }
            ret ^= tail.wrapping_mul(GOLDEN_RATIO_PRIME_64);
        }
        if ret == 0 {
            ret = GOLDEN_RATIO_PRIME_64;
        }
        ret
    }
}
}

// ===========================================================================
// fdt_rb_opts -- shared rb_root_opts for every `FdtNode::children` tree
// AND `FdtBlobInfo::root` (mirrors the C original using one static
// `fdt_rb_opts` everywhere).
// ===========================================================================

/// Mirrors `__fdt_rb_compare()`.
unsafe extern "C" fn fdt_rb_compare(a: u64, b: u64) -> c_int {
    let node_a = a as *mut FdtNode;
    let node_b = b as *mut FdtNode;
    // SAFETY: both keys are live `FdtNode*` (the only producer of tree
    // keys is `fdt_rb_get_key` below, called only on already-linked
    // nodes, or a caller-supplied dummy in the lookup helpers).
    unsafe {
        if (*node_a).hash > (*node_b).hash {
            return 1;
        }
        if (*node_a).hash < (*node_b).hash {
            return -1;
        }
        let str_cmp = strcmp((*node_a).name, (*node_b).name);
        if str_cmp != 0 {
            return str_cmp;
        }
        if (*node_a).has_addr == (*node_b).has_addr {
            if (*node_a).has_addr {
                return if (*node_a).addr > (*node_b).addr {
                    1
                } else if (*node_a).addr < (*node_b).addr {
                    -1
                } else {
                    0
                };
            }
            return 0;
        }
        if (*node_a).has_addr {
            1
        } else {
            -1
        }
    }
}

/// Mirrors `__fdt_rb_get_key()`. `rb_entry` is `FdtNode`'s first field,
/// so `container_of(node, FdtNode, rb_entry)` is the identity cast.
unsafe extern "C" fn fdt_rb_get_key(node: *mut rb_node) -> u64 {
    node as u64
}

static FDT_RB_OPTS: rb_root_opts = rb_root_opts {
    keys_cmp_fun: Some(fdt_rb_compare),
    get_key_fun: Some(fdt_rb_get_key),
};

impl Fdt {
/// Mirrors `kernel/inc/bintree.h`'s `rb_root_init()` static inline (no
/// external linkage in C, reimplemented locally -- same precedent as
/// `timer/timer_core.rs`'s identical helper).
///
/// # Safety
/// `root` must point to caller-owned, writable `rb_root` storage.
unsafe fn fdt_rb_root_init(root: *mut rb_root) {
    // SAFETY: caller contract; `FDT_RB_OPTS` is a live 'static.
    unsafe {
        (*root).node = ptr::null_mut();
        (*root).opts = &raw const FDT_RB_OPTS as *mut rb_root_opts;
    }
}
}

impl FdtNode {
/// Mirrors `__fdt_create_node()`.
///
/// # Safety
/// None beyond normal FFI-allocation caveats -- this is a fresh
/// allocation with no aliasing concerns.
unsafe fn fdt_create_node(name_size: u32, data_size: u32) -> *mut FdtNode {
    let header_size = size_of::<FdtNode>();
    let total_size = header_size + data_size as usize + name_size as usize + 1;
    // SAFETY: `early_alloc_align` is the only allocator live at this
    // point in boot (see module doc).
    let raw = unsafe { early_alloc_align(total_size, align_of::<u64>()) } as *mut u8;
    if raw.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `raw` is a fresh `total_size`-byte allocation.
    unsafe { memset(raw as *mut c_void, 0, total_size) };
    let node = raw as *mut FdtNode;
    // SAFETY: `node` is exclusively owned here (fresh allocation).
    unsafe {
        (*node).data_size = data_size;
        (*node).name_size = name_size;
        (*node).name = raw.add(header_size + data_size as usize) as *const c_char;
    }
    node
}
}

impl FdtNode {
    /// Mirrors `fdt_node_lookup()`. N-OO: true `&self` method (goal #1) --
    /// the C original never null-checks `parent` (doc contract: "must
    /// point to a live, initialized `FdtNode`"), so `&self` never derefs a
    /// null pointer here (unlike the nullable-receiver `fdt_get_prop`
    /// family below, which stay raw-pointer associated fns).
    ///
    /// # Safety
    /// `name` must be a valid NUL-terminated C string; `addr` must be null
    /// or point to a readable `u64`.
    // P3-1D mesh sweep: no live caller anywhere in the tree today, in C or
    // Rust (full-tree grep) -- demoted; `#[allow(dead_code)]` documents the
    // gap rather than deleting still-plausible public API.
    #[allow(dead_code)]
    pub(crate) unsafe fn lookup(&self, name: *const c_char, addr: *mut u64) -> *mut FdtNode {
        if self.child_count == 0 {
            return ptr::null_mut();
        }
        let mut dummy: FdtNode = unsafe { core::mem::zeroed() };
        // SAFETY: `dummy` is a local, exclusively owned; `name` valid per
        // caller contract.
        unsafe { dummy.parse_namestring(name, !addr.is_null()) };
        if !addr.is_null() {
            // SAFETY: `addr` valid per caller contract.
            dummy.addr = unsafe { *addr };
        }
        // SAFETY: `dummy` local and live for the duration of this call;
        // the `&self.children as *const -> *mut` cast matches the
        // established `first_child_named`/`children` precedent (lookup
        // does not mutate the tree).
        let node = unsafe {
            crate::bintree::rb_find_key(&self.children as *const rb_root as *mut rb_root, &raw mut dummy as u64)
        };
        if node.is_null() {
            return ptr::null_mut();
        }
        node as *mut FdtNode
    }
}

impl FdtNode {
    /// Mirrors `__fdt_node_first()` -- the first child matching `name`
    /// (goal #1 method; receiver is the `parent`). Matches the C
    /// original's own lack of a null check on `parent`.
    ///
    /// # Safety
    /// `name` must be a valid NUL-terminated C string.
    unsafe fn first_child_named(&self, name: *const c_char) -> *mut FdtNode {
        if self.child_count == 0 {
            return ptr::null_mut();
        }
        let mut dummy: FdtNode = unsafe { core::mem::zeroed() };
        // SAFETY: `dummy` local and live; `name` valid per caller contract.
        unsafe { dummy.parse_namestring(name, true) };
        dummy.has_addr = false;

        // SAFETY: `&self.children` live; `dummy` local and live. The
        // `*const -> *mut` cast matches `rb_find_key_rup`'s signature
        // (lookup does not mutate the tree).
        let node = unsafe {
            crate::bintree::rb_find_key_rup(
                &self.children as *const rb_root as *mut rb_root,
                &raw mut dummy as u64,
            )
        };
        if node.is_null() {
            return ptr::null_mut();
        }
        let fdt = node as *mut FdtNode;
        // SAFETY: `fdt`/`dummy` live.
        unsafe {
            if (*fdt).hash != dummy.hash
                || strncmp((*fdt).name, dummy.name, dummy.name_size as usize) != 0
                || *(*fdt).name.add(dummy.name_size as usize) != 0
            {
                return ptr::null_mut();
            }
        }
        fdt
    }
}

impl FdtNode {
/// Mirrors `__fdt_node_next_same_name()`.
///
/// # Safety
/// `parent`/`current` must be null or point to live `FdtNode`s.
unsafe fn fdt_node_next_same_name(parent: *mut FdtNode, current: *mut FdtNode) -> *mut FdtNode {
    if parent.is_null() || current.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `current` live per caller contract.
    let next = unsafe { crate::bintree::rb_next_node(&raw mut (*current).rb_entry) };
    if next.is_null() {
        return ptr::null_mut();
    }
    let next_fdt = next as *mut FdtNode;
    // SAFETY: `next_fdt`/`current` live.
    unsafe {
        if (*next_fdt).hash != (*current).hash || strcmp((*next_fdt).name, (*current).name) != 0 {
            return ptr::null_mut();
        }
    }
    next_fdt
}
}

// ===========================================================================
// FdtBlobInfo -- parsed FDT tree + reserved-memory list + lookup indexes.
// ===========================================================================
#[repr(C)]
struct FdtBlobInfo {
    root: rb_root,
    all_nodes: list_node_t,
    n_nodes: i32,
    reserved: *mut mem_region,
    reserved_count: i32,
    compat_table: *mut Hlist,
    phandle_table: *mut Hlist,
}

impl FdtBlobInfo {
/// Mirrors `FdtBlobInfo::fdt_path_lookup()`.
///
/// # Safety
/// `blob` must be null or point to a live `FdtBlobInfo`; `path` must be
/// null or a valid NUL-terminated C string.
// P3-1D mesh sweep: no live caller anywhere in the tree today -- demoted;
// `#[allow(dead_code)]` documents the gap, same precedent as
// `fdt_node_lookup` above.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn fdt_path_lookup(blob: *mut FdtBlobInfo, path: *const c_char) -> *mut FdtNode {
    if blob.is_null() || path.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `blob` live per caller contract.
    if unsafe { (*blob).root.node }.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: see above; `rb_entry` is `FdtNode`'s first field.
    let mut current = unsafe { (*blob).root.node } as *mut FdtNode;

    // SAFETY: `path` valid per caller contract.
    unsafe {
        if *path == b'/' as c_char && *path.add(1) == 0 {
            return current;
        }
    }

    let mut p = path;
    // SAFETY: `path` valid.
    unsafe {
        if *p == b'/' as c_char {
            p = p.add(1);
        }
    }

    // SAFETY: `p` walks within `path`'s NUL-terminated extent throughout.
    unsafe {
        while *p != 0 {
            let mut end = p;
            while *end != 0 && *end != b'/' as c_char {
                end = end.add(1);
            }
            let comp_len = end.offset_from(p) as usize;
            if comp_len == 0 {
                p = end.add(1);
                continue;
            }

            let mut at = p;
            while at < end && *at != b'@' as c_char {
                at = at.add(1);
            }

            let child;
            if at < end {
                let name_len = at.offset_from(p) as usize;
                let addr = Fdt::strtoul(at.add(1), ptr::null_mut(), 16);

                let mut dummy: FdtNode = core::mem::zeroed();
                dummy.name = p;
                dummy.name_size = name_len as u32;
                dummy.hash = Fdt::hlist_hash_str(p, name_len);
                dummy.has_addr = true;
                dummy.addr = addr;

                let node =
                    crate::bintree::rb_find_key(&raw mut (*current).children, &raw mut dummy as u64);
                child = if node.is_null() { ptr::null_mut() } else { node as *mut FdtNode };
            } else {
                let mut name_buf = [0u8; 64];
                let copy_len = if comp_len < 63 { comp_len } else { 63 };
                memcpy(
                    name_buf.as_mut_ptr() as *mut c_void,
                    p as *const c_void,
                    copy_len,
                );
                name_buf[copy_len] = 0;
                child = (*current).first_child_named(name_buf.as_ptr() as *const c_char);
            }

            if child.is_null() {
                return ptr::null_mut();
            }
            current = child;

            if *end == b'/' as c_char {
                p = end.add(1);
            } else {
                break;
            }
        }
    }

    current
}
}

impl FdtBlobInfo {
    /// Mirrors `__fdt_insert_node()`. N-OO: true `&mut self` method (goal
    /// #1) -- `blob` is never null-checked in the C original (caller
    /// contract: live, caller-owned data), and every write goes through
    /// `self` (`n_nodes`, `all_nodes`) or the separate `parent`/`new_node`
    /// raw-pointer params (unaffected by the receiver change).
    ///
    /// # Safety
    /// `parent`/`new_node` must point to live, caller-owned data;
    /// `new_node` must not already be linked into any tree.
    unsafe fn insert_node(&mut self, parent: *mut FdtNode, new_node: *mut FdtNode) -> bool {
        // SAFETY: `new_node` caller-owned, detached per caller contract.
        unsafe { machine::rb_node_init(&raw mut (*new_node).rb_entry) };
        // SAFETY: `parent`/`new_node` live per caller contract.
        let inserted =
            unsafe { crate::rbtree::rb_insert_color(&raw mut (*parent).children, &raw mut (*new_node).rb_entry) };
        if inserted != unsafe { &raw mut (*new_node).rb_entry } {
            return false;
        }
        // SAFETY: `parent`/`new_node` live.
        unsafe {
            (*parent).child_count += 1;
            self.n_nodes += 1;
            Fdt::list_push_back(&raw mut self.all_nodes, &raw mut (*new_node).list_entry);
        }
        true
    }
}

impl Fdt {
/// Mirrors `list_entry_push_back()` (`kernel/inc/list.h`, `static
/// inline`, reimplemented locally on top of `machine::list_entry_*`).
///
/// # Safety
/// `head` must point to a live list-head sentinel; `entry` must be
/// detached.
#[inline(always)]
unsafe fn list_push_back(head: *mut list_node_t, entry: *mut list_node_t) {
    // SAFETY: caller contract.
    unsafe {
        let last = machine::list_entry_prev(head);
        machine::list_entry_insert_after(last, entry);
    }
}
}

impl Fdt {
/// Mirrors `list_entry_push_front()`.
///
/// # Safety
/// Same as [`list_push_back`].
#[inline(always)]
unsafe fn list_push_front(head: *mut list_node_t, entry: *mut list_node_t) {
    // SAFETY: caller contract.
    unsafe { machine::list_entry_insert_after(head, entry) };
}
}

impl FdtBlobInfo {
/// Mirrors `fdt_build_blob_info()`. N-OO: associated constructor fn (goal
/// #1) -- there is no `self` to receive yet (this function builds the
/// value), so it stays a `Self`-returning associated fn rather than a
/// method, matching the `first_child_named`/`children` precedent's own
/// constructor style.
///
/// # Safety
/// `dtb` must be a valid, [`Fdt::fdt_valid`] FDT blob pointer.
unsafe fn build(dtb: *mut c_void) -> *mut FdtBlobInfo {
    // SAFETY: caller contract.
    if !unsafe { Fdt::fdt_valid(dtb) } {
        crate::kprintln!("fdt: invalid magic");
        return ptr::null_mut();
    }

    // SAFETY: fresh allocation.
    let blob = unsafe { early_alloc_align(size_of::<FdtBlobInfo>(), align_of::<u64>()) } as *mut FdtBlobInfo;
    if blob.is_null() {
        crate::kprintln!("fdt: alloc blob failed");
        return ptr::null_mut();
    }
    // SAFETY: `blob` freshly allocated.
    unsafe { memset(blob as *mut c_void, 0, size_of::<FdtBlobInfo>()) };

    let base = dtb as *const u8;
    // SAFETY: `dtb` valid per the `fdt_valid` check above.
    unsafe {
        Fdt::fdt_rb_root_init(&raw mut (*blob).root);
        machine::list_entry_init(&raw mut (*blob).all_nodes);
    }

    // Parse memory reservation map (`/memreserve/`).
    // SAFETY: `dtb` valid.
    let rsvmap_off = unsafe { Fdt::fdt_get_header(base, FDT_HDR_OFF_MEM_RSVMAP) } as usize;
    let mut rsv_count: usize = 0;
    // SAFETY: the DTB's `mem_rsvmap` block is terminated by a {0,0}
    // sentinel entry per the FDT spec; this mirrors the C scan exactly
    // (raw big-endian compare against 0 is endian-invariant).
    unsafe {
        let mut cur = rsvmap_off;
        loop {
            let b = ptr::read_unaligned(base.add(cur) as *const u64);
            let s = ptr::read_unaligned(base.add(cur + 8) as *const u64);
            if b == 0 && s == 0 {
                break;
            }
            rsv_count += 1;
            cur += size_of::<mem_region>();
        }
    }

    if rsv_count > 0 {
        // SAFETY: allocator call.
        let reserved = unsafe {
            early_alloc_align(rsv_count * size_of::<mem_region>(), align_of::<u64>())
        } as *mut mem_region;
        if reserved.is_null() {
            crate::kprintln!("fdt: alloc reserved failed");
            return ptr::null_mut();
        }
        // SAFETY: `reserved` sized for `rsv_count` entries above; `blob`
        // live.
        unsafe {
            (*blob).reserved = reserved;
            (*blob).reserved_count = rsv_count as i32;
            for i in 0..rsv_count {
                let off = rsvmap_off + i * size_of::<mem_region>();
                (*reserved.add(i)).base = Fdt::read_be64(base, off);
                (*reserved.add(i)).size = Fdt::read_be64(base, off + 8);
            }
        }
    }

    // Parse the FDT structure block and build the tree.
    // SAFETY: `dtb` valid.
    let struct_start = unsafe { Fdt::fdt_get_header(base, FDT_HDR_OFF_DT_STRUCT) } as usize;
    let mut p = struct_start;

    let mut node_stack: [*mut FdtNode; FDT_MAX_DEPTH] = [ptr::null_mut(); FDT_MAX_DEPTH];
    let mut depth: usize = 0;

    // SAFETY: fresh node.
    let virtual_root = unsafe { FdtNode::fdt_create_node(0, 0) };
    if virtual_root.is_null() {
        crate::kprintln!("fdt: alloc virtual root failed");
        return ptr::null_mut();
    }
    // SAFETY: `virtual_root` freshly allocated, exclusively owned.
    unsafe { Fdt::fdt_rb_root_init(&raw mut (*virtual_root).children) };
    node_stack[0] = virtual_root;

    loop {
        // SAFETY: see the equivalent read in `fdt_early_scan_memory`.
        let token = unsafe { Fdt::read_be32(base, p) };
        p += 4;

        match token {
            FDT_BEGIN_NODE => {
                let name = unsafe { base.add(p) as *const c_char };
                // SAFETY: `name` NUL-terminated per the DTB format.
                let namelen = unsafe { strlen(name) };
                p += fdt_align(namelen + 1);

                let mut dummy: FdtNode = unsafe { core::mem::zeroed() };
                // SAFETY: `dummy` local; `name` valid.
                unsafe { dummy.parse_namestring(name, false) };

                // SAFETY: allocator call.
                let new_node = unsafe { FdtNode::fdt_create_node(dummy.name_size, 0) };
                if new_node.is_null() {
                    crate::kprintln!("fdt: alloc node '{}' failed", crate::printf::Cs(name));
                    return ptr::null_mut();
                }

                // SAFETY: `new_node` freshly allocated with
                // `dummy.name_size + 1` trailing bytes for the name.
                unsafe {
                    memcpy(
                        (*new_node).name as *mut c_void,
                        name as *const c_void,
                        dummy.name_size as usize,
                    );
                    *((*new_node).name as *mut c_char).add(dummy.name_size as usize) = 0;

                    (*new_node).hash = dummy.hash;
                    (*new_node).has_addr = dummy.has_addr;
                    (*new_node).addr = dummy.addr;
                    (*new_node).layer = depth as u8;

                    Fdt::fdt_rb_root_init(&raw mut (*new_node).children);

                    let parent = node_stack[depth];
                    if !(*blob).insert_node(parent, new_node) {
                        crate::kprintln!("fdt: insert node '{}' failed (dup?)", crate::printf::Cs(name));
                        return ptr::null_mut();
                    }
                }

                depth += 1;
                if depth >= FDT_MAX_DEPTH {
                    crate::kprintln!("fdt: tree too deep");
                    return ptr::null_mut();
                }
                node_stack[depth] = new_node;
            }

            FDT_END_NODE => {
                if depth > 0 {
                    depth -= 1;
                }
            }

            FDT_PROP => {
                // SAFETY: see `fdt_early_scan_memory`.
                let len = unsafe { Fdt::read_be32(base, p) };
                p += 4;
                let nameoff = unsafe { Fdt::read_be32(base, p) };
                p += 4;
                // SAFETY: `nameoff` from the DTB's own prop header.
                let propname = unsafe { Fdt::fdt_get_string(base, nameoff) };
                let data = unsafe { base.add(p) };
                p += fdt_align(len as usize);

                let parent = node_stack[depth];
                if parent.is_null() {
                    continue;
                }

                // SAFETY: `propname` valid.
                if len >= 4
                    && (unsafe { strcmp(propname, c"phandle".as_ptr()) } == 0
                        || unsafe { strcmp(propname, c"linux,phandle".as_ptr()) } == 0)
                {
                    // SAFETY: `parent`/`data` live.
                    unsafe {
                        (*parent).phandle = Fdt::read_be32(data, 0);
                        (*parent).has_phandle = true;
                    }
                    continue;
                } else if unsafe { strcmp(propname, c"phandle".as_ptr()) } == 0
                    || unsafe { strcmp(propname, c"linux,phandle".as_ptr()) } == 0
                {
                    // `len < 4`: matches the C original, which still
                    // `continue`s without creating a property node for a
                    // too-short phandle property.
                    continue;
                }

                let mut dummy: FdtNode = unsafe { core::mem::zeroed() };
                // SAFETY: `dummy` local; `propname` valid.
                unsafe { dummy.parse_namestring(propname, true) };

                // SAFETY: allocator call.
                let prop_node = unsafe { FdtNode::fdt_create_node(dummy.name_size, len) };
                if prop_node.is_null() {
                    crate::kprintln!("fdt: alloc prop '{}' failed", crate::printf::Cs(propname));
                    return ptr::null_mut();
                }

                // SAFETY: `prop_node` freshly allocated with `len` bytes
                // of data storage followed by `dummy.name_size + 1`
                // bytes of name storage.
                unsafe {
                    if len > 0 {
                        memcpy((*prop_node).data_ptr() as *mut c_void, data as *const c_void, len as usize);
                    }
                    memcpy(
                        (*prop_node).name as *mut c_void,
                        propname as *const c_void,
                        dummy.name_size as usize,
                    );
                    *((*prop_node).name as *mut c_char).add(dummy.name_size as usize) = 0;

                    (*prop_node).hash = dummy.hash;
                    (*prop_node).has_addr = false;
                    (*prop_node).layer = (depth + 1) as u8;

                    Fdt::fdt_rb_root_init(&raw mut (*prop_node).children);

                    if !(*blob).insert_node(parent, prop_node) {
                        crate::kprintln!(
                            "fdt: insert prop '{}' in '{}' failed",
                            crate::printf::Cs(propname),
                            crate::printf::Cs((*parent).name),
                        );
                        return ptr::null_mut();
                    }
                }
            }

            FDT_NOP => {}

            FDT_END => {
                // SAFETY: `blob`/`virtual_root` live.
                unsafe { (*blob).root = (*virtual_root).children };
                return blob;
            }

            _ => {
                crate::kprintln!("fdt: unknown token 0x{:x}", ((token as c_int as i32 as i64) as u64));
                return ptr::null_mut();
            }
        }
    }
}
}

// ===========================================================================
// Property-value accessors.
// ===========================================================================

impl FdtNode {
/// Mirrors `__fdt_get_prop()`.
///
/// # Safety
/// `node` must be null or point to a live `FdtNode`; `name` must be a
/// valid NUL-terminated C string.
unsafe fn fdt_get_prop(node: *mut FdtNode, name: *const c_char) -> *mut FdtNode {
    if node.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: caller contract.
    unsafe { (*node).lookup(name, ptr::null_mut()) }
}
}

impl FdtNode {
/// Mirrors `__fdt_prop_u32()`.
///
/// # Safety
/// `prop` must be null or point to a live `FdtNode` whose data is at
/// least `data_size` readable bytes.
unsafe fn fdt_prop_u32(prop: *mut FdtNode, index: i32) -> u32 {
    if prop.is_null() {
        return 0;
    }
    // SAFETY: caller contract.
    let data_size = unsafe { (*prop).data_size };
    if data_size < ((index + 1) as u32) * 4 {
        return 0;
    }
    // SAFETY: bounds-checked above.
    unsafe { Fdt::read_be32((*prop).data_ptr(), index as usize * 4) }
}
}

impl FdtNode {
/// Mirrors `__fdt_prop_u64()`.
///
/// # Safety
/// Same as [`fdt_prop_u32`].
unsafe fn fdt_prop_u64(prop: *mut FdtNode, index: i32) -> u64 {
    if prop.is_null() {
        return 0;
    }
    // SAFETY: caller contract.
    let data_size = unsafe { (*prop).data_size };
    if data_size < ((index + 1) as u32) * 8 {
        return 0;
    }
    // SAFETY: bounds-checked above; two adjacent big-endian u32 cells
    // compose the 64-bit value (matches the C original, which does NOT
    // use `fdt64_to_cpu`/`bswap64` here -- unaffected by that macro's
    // bug).
    unsafe {
        let hi = Fdt::read_be32((*prop).data_ptr(), index as usize * 8);
        let lo = Fdt::read_be32((*prop).data_ptr(), index as usize * 8 + 4);
        ((hi as u64) << 32) | lo as u64
    }
}
}

impl FdtNode {
/// Mirrors `__fdt_prop_compat()`.
///
/// # Safety
/// `prop` must be null or point to a live `FdtNode`; `compat` must be
/// null or a valid NUL-terminated C string.
unsafe fn fdt_prop_compat(prop: *mut FdtNode, compat: *const c_char) -> bool {
    if prop.is_null() || compat.is_null() {
        return false;
    }
    // SAFETY: `compat` valid per caller contract.
    let compat_len = unsafe { strlen(compat) };
    // SAFETY: `prop` non-null past the guard above; `string_list` yields
    // each NUL-terminated string in its `data_size`-byte data block.
    for (str_ptr, str_len) in unsafe { (*prop).string_list() } {
        if str_len >= compat_len {
            let mut i = 0usize;
            while i <= str_len - compat_len {
                // SAFETY: `str_ptr + i` in-bounds; comparing `compat_len`
                // bytes stays within the string since
                // `i + compat_len <= str_len`.
                if unsafe { strncmp(str_ptr.add(i), compat, compat_len) } == 0 {
                    return true;
                }
                i += 1;
            }
        }
    }
    false
}
}

impl FdtNode {
/// Mirrors `__fdt_prop_compat_list()`.
///
/// # Safety
/// `prop` must be null or point to a live `FdtNode`; `compat_list` must
/// be null or a NULL-terminated array of valid NUL-terminated C strings.
unsafe fn fdt_prop_compat_list(prop: *mut FdtNode, compat_list: &[*const c_char]) -> bool {
    if prop.is_null() {
        return false;
    }
    for &compat in compat_list {
        // SAFETY: caller contract.
        if unsafe { FdtNode::fdt_prop_compat(prop, compat) } {
            return true;
        }
    }
    false
}
}

impl FdtNode {
/// Mirrors `__fdt_parse_reg_prop()`.
///
/// # Safety
/// `prop` must be null or point to a live `FdtNode`; `base_out`/`size_out`
/// must be null or valid `*mut u64` out-params.
unsafe fn fdt_parse_reg_prop(
    prop: *mut FdtNode,
    addr_cells: i32,
    size_cells: i32,
    base_out: *mut u64,
    size_out: *mut u64,
) {
    if prop.is_null() || base_out.is_null() {
        return;
    }
    // SAFETY: caller contract throughout.
    unsafe {
        if addr_cells == 2 {
            *base_out = FdtNode::fdt_prop_u64(prop, 0);
        } else {
            *base_out = FdtNode::fdt_prop_u32(prop, 0) as u64;
        }

        if !size_out.is_null() {
            if size_cells == 2 {
                let hi = FdtNode::fdt_prop_u32(prop, addr_cells);
                let lo = FdtNode::fdt_prop_u32(prop, addr_cells + 1);
                *size_out = ((hi as u64) << 32) | lo as u64;
            } else {
                *size_out = FdtNode::fdt_prop_u32(prop, addr_cells) as u64;
            }
        }
    }
}
}

// ===========================================================================
// Compatible-string / phandle hash tables.
//
// NOTE: because of the preserved `fdt_type` bug (see module doc), the
// indexing loop below (`fdt_build_indexes`) never actually calls
// `fdt_index_node_compat`/`fdt_index_node_phandle` on any real node --
// both tables always end up allocated-but-empty. Kept in full (not
// special-cased away) so the C bug stays visible/auditable in the port
// rather than silently vanishing.
// ===========================================================================

#[repr(C)]
struct FdtCompatHashNode {
    hash_entry: HlistEntry,
    compat: *const c_char,
    compat_len: usize,
    nodes: list_node_t,
    count: i32,
}

#[repr(C)]
struct FdtCompatLink {
    list_entry: list_node_t,
    hash_node: *mut FdtCompatHashNode,
    fdt_node: *mut FdtNode,
}

#[repr(C)]
struct FdtPhandleHashNode {
    hash_entry: HlistEntry,
    phandle: u32,
    fdt_node: *mut FdtNode,
}

impl Fdt {
/// Mirrors `Fdt::hlist_entry_init()` (`kernel/inc/hlist.h`, `static inline`).
///
/// # Safety
/// `entry` must be null or point to caller-owned, writable storage.
unsafe fn hlist_entry_init(entry: *mut HlistEntry) {
    if entry.is_null() {
        return;
    }
    // SAFETY: caller contract.
    unsafe {
        (*entry).bucket = ptr::null_mut();
        machine::list_entry_init(&raw mut (*entry).list_entry);
    }
}
}

extern "C" fn fdt_compat_hash_func(data: *mut c_void) -> HtHash {
    let node = data as *mut FdtCompatHashNode;
    // SAFETY: `data` is a live `FdtCompatHashNode*` (hlist callback
    // contract).
    unsafe { Fdt::hlist_hash_str((*node).compat, (*node).compat_len) }
}

extern "C" fn fdt_compat_cmp_func(_hlist: *mut Hlist, node: *mut c_void, key: *mut c_void) -> c_int {
    let a = node as *mut FdtCompatHashNode;
    let b = key as *mut FdtCompatHashNode;
    // SAFETY: `node`/`key` are live `FdtCompatHashNode*` (hlist callback
    // contract).
    unsafe {
        let min_len = core::cmp::min((*a).compat_len, (*b).compat_len);
        let cmp = strncmp((*a).compat, (*b).compat, min_len);
        if cmp != 0 {
            return cmp;
        }
        if (*a).compat_len > (*b).compat_len {
            1
        } else if (*a).compat_len < (*b).compat_len {
            -1
        } else {
            0
        }
    }
}

extern "C" fn fdt_compat_get_node_func(entry: *mut HlistEntry) -> *mut c_void {
    // `hash_entry` is `FdtCompatHashNode`'s first field: offset-0 cast.
    entry as *mut c_void
}

extern "C" fn fdt_compat_get_entry_func(node: *mut c_void) -> *mut HlistEntry {
    node as *mut HlistEntry
}

static FDT_COMPAT_HLIST_FUNCS: HlistFunc = HlistFunc {
    hash: Some(fdt_compat_hash_func),
    get_node: Some(fdt_compat_get_node_func),
    get_entry: Some(fdt_compat_get_entry_func),
    cmp_node: Some(fdt_compat_cmp_func),
};

extern "C" fn fdt_phandle_hash_func(data: *mut c_void) -> HtHash {
    let node = data as *mut FdtPhandleHashNode;
    // SAFETY: `data` is a live `FdtPhandleHashNode*`.
    unsafe { (*node).phandle as HtHash }
}

extern "C" fn fdt_phandle_cmp_func(_hlist: *mut Hlist, node: *mut c_void, key: *mut c_void) -> c_int {
    let a = node as *mut FdtPhandleHashNode;
    let b = key as *mut FdtPhandleHashNode;
    // SAFETY: `node`/`key` are live `FdtPhandleHashNode*`.
    unsafe {
        if (*a).phandle > (*b).phandle {
            1
        } else if (*a).phandle < (*b).phandle {
            -1
        } else {
            0
        }
    }
}

extern "C" fn fdt_phandle_get_node_func(entry: *mut HlistEntry) -> *mut c_void {
    entry as *mut c_void
}

extern "C" fn fdt_phandle_get_entry_func(node: *mut c_void) -> *mut HlistEntry {
    node as *mut HlistEntry
}

static FDT_PHANDLE_HLIST_FUNCS: HlistFunc = HlistFunc {
    hash: Some(fdt_phandle_hash_func),
    get_node: Some(fdt_phandle_get_node_func),
    get_entry: Some(fdt_phandle_get_entry_func),
    cmp_node: Some(fdt_phandle_cmp_func),
};

impl FdtCompatHashNode {
/// Mirrors `__fdt_alloc_compat_hash_node()`.
unsafe fn fdt_alloc_compat_hash_node(compat: *const c_char, len: usize) -> *mut FdtCompatHashNode {
    // SAFETY: allocator call.
    let node = unsafe { early_alloc_align(size_of::<FdtCompatHashNode>(), align_of::<u64>()) }
        as *mut FdtCompatHashNode;
    if node.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: freshly allocated.
    unsafe {
        memset(node as *mut c_void, 0, size_of::<FdtCompatHashNode>());
        (*node).compat = compat;
        (*node).compat_len = len;
        machine::list_entry_init(&raw mut (*node).nodes);
        Fdt::hlist_entry_init(&raw mut (*node).hash_entry);
    }
    node
}
}

impl FdtCompatLink {
/// Mirrors `__fdt_alloc_compat_link()`.
unsafe fn fdt_alloc_compat_link(fdt_node: *mut FdtNode, hash_node: *mut FdtCompatHashNode) -> *mut FdtCompatLink {
    // SAFETY: allocator call.
    let link = unsafe { early_alloc_align(size_of::<FdtCompatLink>(), align_of::<u64>()) } as *mut FdtCompatLink;
    if link.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: freshly allocated.
    unsafe {
        memset(link as *mut c_void, 0, size_of::<FdtCompatLink>());
        (*link).fdt_node = fdt_node;
        (*link).hash_node = hash_node;
    }
    link
}
}

impl FdtPhandleHashNode {
/// Mirrors `__fdt_alloc_phandle_hash_node()`.
unsafe fn fdt_alloc_phandle_hash_node(phandle: u32, fdt_node: *mut FdtNode) -> *mut FdtPhandleHashNode {
    // SAFETY: allocator call.
    let node = unsafe { early_alloc_align(size_of::<FdtPhandleHashNode>(), align_of::<u64>()) }
        as *mut FdtPhandleHashNode;
    if node.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: freshly allocated.
    unsafe {
        memset(node as *mut c_void, 0, size_of::<FdtPhandleHashNode>());
        (*node).phandle = phandle;
        (*node).fdt_node = fdt_node;
        Fdt::hlist_entry_init(&raw mut (*node).hash_entry);
    }
    node
}
}

impl FdtBlobInfo {
/// Mirrors `__fdt_index_compat_string()`. N-OO: true `&self` method (goal
/// #1) -- `blob` is never null-checked (live per caller contract) and
/// only `self.compat_table` (a pointer to a separately-allocated `Hlist`)
/// is read, never a `self` field written, so `&self` suffices.
unsafe fn index_compat_string(&self, fdt_node: *mut FdtNode, compat: *const c_char, len: usize) -> i32 {
    let mut key: FdtCompatHashNode = unsafe { core::mem::zeroed() };
    key.compat = compat;
    key.compat_len = len;
    // SAFETY: `self` live; `key` local.
    let mut hash_node =
        unsafe { crate::hlist::hlist_get(self.compat_table as *mut c_void as *mut Hlist, &raw mut key as *mut c_void) }
            as *mut FdtCompatHashNode;

    if hash_node.is_null() {
        // SAFETY: allocator call.
        hash_node = unsafe { FdtCompatHashNode::fdt_alloc_compat_hash_node(compat, len) };
        if hash_node.is_null() {
            return -1;
        }
        // SAFETY: `self`/`hash_node` live.
        let ret = unsafe {
            crate::hlist::hlist_put(self.compat_table, hash_node as *mut c_void, false)
        } as *mut FdtCompatHashNode;
        if ret != hash_node && !ret.is_null() {
            hash_node = ret;
        }
    }

    // SAFETY: `fdt_node`/`hash_node` live.
    let link = unsafe { FdtCompatLink::fdt_alloc_compat_link(fdt_node, hash_node) };
    if link.is_null() {
        return -1;
    }

    // SAFETY: `hash_node`/`link` live.
    unsafe {
        Fdt::list_push_front(&raw mut (*hash_node).nodes, &raw mut (*link).list_entry);
        (*hash_node).count += 1;
    }
    0
}

/// Mirrors `__fdt_index_node_compat()`. N-OO: true `&self` method (goal
/// #1), same rationale as [`FdtBlobInfo::index_compat_string`].
unsafe fn index_node_compat(&self, fdt_node: *mut FdtNode) {
    // SAFETY: `fdt_node` live.
    let compat_prop = unsafe { (*fdt_node).lookup(c"compatible".as_ptr(), ptr::null_mut()) };
    if compat_prop.is_null() || unsafe { (*compat_prop).data_size } == 0 {
        return;
    }

    // SAFETY: `compat_prop` non-null with `data_size > 0` past the guard;
    // `string_list` yields each NUL-terminated string in its data block.
    for (str_ptr, str_len) in unsafe { (*compat_prop).string_list() } {
        if str_len > 0 {
            unsafe { self.index_compat_string(fdt_node, str_ptr, str_len) };
        }
    }
}

/// Mirrors `__fdt_index_node_phandle()`. N-OO: true `&self` method (goal
/// #1), same rationale as [`FdtBlobInfo::index_compat_string`].
unsafe fn index_node_phandle(&self, fdt_node: *mut FdtNode) {
    // SAFETY: `fdt_node` live.
    if !unsafe { (*fdt_node).has_phandle } || unsafe { (*fdt_node).phandle } == 0 {
        return;
    }
    // SAFETY: allocator call.
    let hash_node = unsafe { FdtPhandleHashNode::fdt_alloc_phandle_hash_node((*fdt_node).phandle, fdt_node) };
    if hash_node.is_null() {
        return;
    }
    // SAFETY: `self`/`hash_node` live.
    unsafe { crate::hlist::hlist_put(self.phandle_table, hash_node as *mut c_void, false) };
}
}

impl Fdt {
/// Mirrors `__fdt_alloc_hlist()`.
unsafe fn fdt_alloc_hlist(bucket_cnt: u64, func: *const HlistFunc) -> *mut Hlist {
    let header_size = size_of::<Hlist>();
    let size = header_size + bucket_cnt as usize * size_of::<list_node_t>();
    // SAFETY: allocator call.
    let hlist = unsafe { early_alloc_align(size, align_of::<u64>()) } as *mut Hlist;
    if hlist.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: freshly allocated.
    unsafe { memset(hlist as *mut c_void, 0, size) };

    // SAFETY: `hlist` sized for `bucket_cnt` buckets above; `func` is a
    // live 'static.
    let ret = unsafe { crate::hlist::hlist_init(hlist, bucket_cnt, func as *mut HlistFunc) };
    if ret != 0 {
        return ptr::null_mut();
    }
    hlist
}
}

impl FdtBlobInfo {
/// Mirrors `__fdt_build_indexes()`. N-OO: true `&mut self` method (goal
/// #1) -- `blob` is never null-checked (live per caller contract) and
/// writes `self.compat_table`/`self.phandle_table` directly.
unsafe fn build_indexes(&mut self) {
    // SAFETY: `self` live.
    unsafe {
        self.compat_table = Fdt::fdt_alloc_hlist(FDT_COMPAT_HASH_BUCKETS, &raw const FDT_COMPAT_HLIST_FUNCS);
    }
    if self.compat_table.is_null() {
        crate::kprintln!("fdt: failed to alloc compat hash table");
        return;
    }

    // SAFETY: `self` live.
    unsafe {
        self.phandle_table = Fdt::fdt_alloc_hlist(FDT_PHANDLE_HASH_BUCKETS, &raw const FDT_PHANDLE_HLIST_FUNCS);
    }
    if self.phandle_table.is_null() {
        crate::kprintln!("fdt: failed to alloc phandle hash table");
        return;
    }

    // SAFETY: `self` live; walks `all_nodes`, a well-formed list per
    // `FdtBlobInfo::build`'s construction. The `successors` iterator
    // reproduces the C `pos = next(head); while pos != head { ..; pos =
    // next(pos) }` walk exactly (the established loop->iterator idiom in
    // this crate, cf. `mm/pcache.rs`).
    unsafe {
        let head = &raw mut self.all_nodes;
        let all_nodes = core::iter::successors(
            Some(machine::list_entry_next(head)).filter(|&p| p != head),
            move |&p| {
                let next = machine::list_entry_next(p);
                (next != head).then_some(next)
            },
        );
        for pos in all_nodes {
            let node = machine::list_container_of::<FdtNode>(pos, offset_of!(FdtNode, list_entry));
            // Preserved dead branch -- see module doc: `fdt_type` is
            // never `FDT_BEGIN_NODE`, so this never runs.
            if (*node).fdt_type as u32 == FDT_BEGIN_NODE {
                self.index_node_compat(node);
                self.index_node_phandle(node);
            }
        }
    }
}
}

impl FdtBlobInfo {
/// Mirrors `FdtBlobInfo::fdt_compat_lookup()`.
///
/// # Safety
/// `blob` must be null or point to a live `FdtBlobInfo`; `compat` must be
/// null or a valid NUL-terminated C string.
// P3-1D mesh sweep: no live caller anywhere in the tree today -- demoted;
// `#[allow(dead_code)]` documents the gap, same precedent as
// `fdt_node_lookup` above.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn fdt_compat_lookup(blob: *mut FdtBlobInfo, compat: *const c_char) -> *mut FdtNode {
    if blob.is_null() || compat.is_null() || unsafe { (*blob).compat_table }.is_null() {
        return ptr::null_mut();
    }
    let mut key: FdtCompatHashNode = unsafe { core::mem::zeroed() };
    key.compat = compat;
    key.compat_len = unsafe { strlen(compat) };
    // SAFETY: `blob` live; `key` local.
    let hash_node =
        unsafe { crate::hlist::hlist_get((*blob).compat_table, &raw mut key as *mut c_void) } as *mut FdtCompatHashNode;
    if hash_node.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `hash_node` live.
    let head = unsafe { &raw mut (*hash_node).nodes };
    let first = unsafe { machine::list_entry_next(head) };
    if first == head {
        return ptr::null_mut();
    }
    let link = unsafe { machine::list_container_of::<FdtCompatLink>(first, offset_of!(FdtCompatLink, list_entry)) };
    unsafe { (*link).fdt_node }
}
}

impl FdtBlobInfo {
/// Mirrors `FdtBlobInfo::fdt_compat_next()`.
///
/// # Safety
/// `link` must be null or point to a live `*mut FdtCompatLink` (which may
/// itself be null).
// P3-1D mesh sweep: no live caller anywhere in the tree today -- demoted;
// `#[allow(dead_code)]` documents the gap, same precedent as
// `fdt_node_lookup` above.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn fdt_compat_next(link: *mut *mut FdtCompatLink) -> *mut FdtNode {
    if link.is_null() || unsafe { *link }.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `*link` live per the check above.
    let cur = unsafe { *link };
    let hash_node = unsafe { (*cur).hash_node };
    let next = unsafe { machine::list_entry_next(&raw mut (*cur).list_entry) };
    let head = unsafe { &raw mut (*hash_node).nodes };
    if next == head {
        // SAFETY: `link` valid per caller contract.
        unsafe { *link = ptr::null_mut() };
        return ptr::null_mut();
    }
    let next_link = unsafe { machine::list_container_of::<FdtCompatLink>(next, offset_of!(FdtCompatLink, list_entry)) };
    unsafe { *link = next_link };
    unsafe { (*next_link).fdt_node }
}
}

impl FdtBlobInfo {
/// Mirrors `FdtBlobInfo::fdt_phandle_lookup()`.
///
/// # Safety
/// `blob` must be null or point to a live `FdtBlobInfo`.
// P3-1D mesh sweep: no live caller anywhere in the tree today -- demoted;
// `#[allow(dead_code)]` documents the gap, same precedent as
// `fdt_node_lookup` above.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn fdt_phandle_lookup(blob: *mut FdtBlobInfo, phandle: u32) -> *mut FdtNode {
    if blob.is_null() || phandle == 0 || unsafe { (*blob).phandle_table }.is_null() {
        return ptr::null_mut();
    }
    let mut key: FdtPhandleHashNode = unsafe { core::mem::zeroed() };
    key.phandle = phandle;
    // SAFETY: `blob` live; `key` local.
    let hash_node =
        unsafe { crate::hlist::hlist_get((*blob).phandle_table, &raw mut key as *mut c_void) } as *mut FdtPhandleHashNode;
    if hash_node.is_null() {
        return ptr::null_mut();
    }
    unsafe { (*hash_node).fdt_node }
}
}

// ===========================================================================
// fdt_init / fdt_extract_platform_info / fdt_apply_platform_config.
// ===========================================================================

/// Mirrors the C `static struct fdt_blob_info *fdt_blob;`. Written once
/// by `fdt_init` (boot hart, single-threaded), read only by `fdt_walk`
/// (never called from `start_kernel.c` -- see that function's doc
/// comment).
static mut FDT_BLOB: *mut FdtBlobInfo = ptr::null_mut();

impl FdtBlobInfo {
/// Mirrors `fdt_extract_platform_info()`. N-OO: true `&self` method (goal
/// #1) -- `blob` is never null-checked (live per caller contract) and
/// only reads `self.root`/`self.reserved`/`self.reserved_count`, never
/// writes a `self` field.
///
/// The root-node lookup below reproduces a convoluted, provably-mostly-
/// dead C original 1:1 (see module doc's "Preserved pre-existing bugs"):
/// `__fdt_node_first(root_or_null, "")` never actually finds anything
/// (no child of `/` is ever named `""`), so `root` is always recovered
/// via the `blob->root.node != NULL` fallback for every real DTB.
///
/// # Safety
/// `self` must be a live, fully-built `FdtBlobInfo`.
unsafe fn extract_platform_info(&self) {
    // SAFETY: caller contract.
    let blob_root_node = unsafe { self.root.node };
    let mut root: *mut FdtNode = ptr::null_mut();
    if !blob_root_node.is_null() {
        let candidate = blob_root_node as *mut FdtNode;
        // SAFETY: `candidate` live (offset-0 cast of a live `rb_node`).
        let first_arg = if !unsafe { (*candidate).children.node }.is_null() {
            candidate
        } else {
            ptr::null_mut()
        };
        if !first_arg.is_null() {
            // SAFETY: `first_arg` non-null and live.
            root = unsafe { (*first_arg).first_child_named(c"".as_ptr()) };
        }
    }
    if root.is_null() && !blob_root_node.is_null() {
        root = blob_root_node as *mut FdtNode;
    }
    if root.is_null() {
        return;
    }

    let mut root_addr_cells: i32 = 2;
    let mut root_size_cells: i32 = 1;
    // SAFETY: `root` live.
    let mut prop = unsafe { FdtNode::fdt_get_prop(root, c"#address-cells".as_ptr()) };
    if !prop.is_null() {
        root_addr_cells = unsafe { FdtNode::fdt_prop_u32(prop, 0) } as i32;
    }
    prop = unsafe { FdtNode::fdt_get_prop(root, c"#size-cells".as_ptr()) };
    if !prop.is_null() {
        root_size_cells = unsafe { FdtNode::fdt_prop_u32(prop, 0) } as i32;
    }

    // /cpus
    let cpus = unsafe { (*root).lookup(c"cpus".as_ptr(), ptr::null_mut()) };
    if !cpus.is_null() {
        prop = unsafe { FdtNode::fdt_get_prop(cpus, c"timebase-frequency".as_ptr()) };
        if !prop.is_null() {
            // SAFETY: `prop` live.
            let data_size = unsafe { (*prop).data_size };
            unsafe {
                if data_size == 4 {
                    platform.timebase_freq = FdtNode::fdt_prop_u32(prop, 0) as u64;
                } else if data_size == 8 {
                    platform.timebase_freq = FdtNode::fdt_prop_u64(prop, 0);
                }
            }
        }

        // SAFETY: `cpus` live; `first_child_named`/`fdt_same_name_iter`
        // walk its immutable child tree (boot-time, single hart).
        let cpus_iter =
            unsafe { FdtNode::fdt_same_name_iter(cpus, (*cpus).first_child_named(c"cpu".as_ptr())) };
        for _cpu in cpus_iter {
            unsafe { platform.ncpu += 1 };
        }
    }
    if unsafe { platform.ncpu } == 0 {
        unsafe { platform.ncpu = 1 };
    }

    // /memory (possibly multiple banks)
    // SAFETY: `root` live; the same-name walk reads its immutable child
    // tree (boot-time, single hart).
    let memory_iter =
        unsafe { FdtNode::fdt_same_name_iter(root, (*root).first_child_named(c"memory".as_ptr())) };
    for memory in memory_iter {
        if unsafe { platform.mem_count } >= platform_info_mem_regions() {
            break;
        }
        prop = unsafe { FdtNode::fdt_get_prop(memory, c"reg".as_ptr()) };
        if !prop.is_null() {
            let cells_per_entry = root_addr_cells + root_size_cells;
            // SAFETY: `prop` live.
            let num_entries = (unsafe { (*prop).data_size } as i32 / 4) / cells_per_entry;

            let mut i = 0;
            while i < num_entries && unsafe { platform.mem_count } < platform_info_mem_regions() {
                let offset = i * cells_per_entry;
                let base_val: u64;
                let size_val: u64;
                unsafe {
                    if root_addr_cells == 2 {
                        base_val = ((FdtNode::fdt_prop_u32(prop, offset) as u64) << 32)
                            | FdtNode::fdt_prop_u32(prop, offset + 1) as u64;
                    } else {
                        base_val = FdtNode::fdt_prop_u32(prop, offset) as u64;
                    }
                    if root_size_cells == 2 {
                        let hi = FdtNode::fdt_prop_u32(prop, offset + root_addr_cells);
                        let lo = FdtNode::fdt_prop_u32(prop, offset + root_addr_cells + 1);
                        size_val = ((hi as u64) << 32) | lo as u64;
                    } else {
                        size_val = FdtNode::fdt_prop_u32(prop, offset + root_addr_cells) as u64;
                    }

                    let idx = platform.mem_count as usize;
                    platform.mem[idx].base = base_val;
                    platform.mem[idx].size = size_val;
                    platform.mem_count += 1;
                    platform.total_mem += size_val;
                }
                i += 1;
            }
        }
    }

    // /chosen (ramdisk)
    let chosen = unsafe { (*root).lookup(c"chosen".as_ptr(), ptr::null_mut()) };
    if !chosen.is_null() {
        prop = unsafe { FdtNode::fdt_get_prop(chosen, c"linux,initrd-start".as_ptr()) };
        if !prop.is_null() {
            unsafe {
                platform.ramdisk_base = if (*prop).data_size == 8 {
                    FdtNode::fdt_prop_u64(prop, 0)
                } else {
                    FdtNode::fdt_prop_u32(prop, 0) as u64
                };
            }
        }
        prop = unsafe { FdtNode::fdt_get_prop(chosen, c"linux,initrd-end".as_ptr()) };
        if !prop.is_null() {
            unsafe {
                let end = if (*prop).data_size == 8 {
                    FdtNode::fdt_prop_u64(prop, 0)
                } else {
                    FdtNode::fdt_prop_u32(prop, 0) as u64
                };
                if platform.ramdisk_base != 0 && end > platform.ramdisk_base {
                    platform.ramdisk_size = end - platform.ramdisk_base;
                    platform.has_ramdisk = 1;
                }
            }
        }
    }

    // Compatible-string lists for device detection.
    let uart_compat: [*const c_char; 8] = [
        c"ns16550a".as_ptr(),
        c"ns16550".as_ptr(),
        c"snps,dw-apb-uart".as_ptr(),
        c"ti,omap3-uart".as_ptr(),
        c"xlnx,xuartps".as_ptr(),
        c"ky,pxa-uart".as_ptr(),
        c"arm,sbsa-uart".as_ptr(),
        ptr::null(),
    ];
    let plic_compat: [*const c_char; 5] = [
        c"riscv,plic0".as_ptr(),
        c"sifive,plic-1.0.0".as_ptr(),
        c"thead,c900-plic".as_ptr(),
        c"andestech,nceplic100".as_ptr(),
        ptr::null(),
    ];
    let pcie_compat: [*const c_char; 5] = [
        c"pci-host-ecam-generic".as_ptr(),
        c"pci-host-cam-generic".as_ptr(),
        c"x1,dwc-pcie".as_ptr(),
        c"spacemit,k1-pcie".as_ptr(),
        ptr::null(),
    ];
    let virtio_compat: [*const c_char; 2] = [c"virtio,mmio".as_ptr(), ptr::null()];
    let emac_compat: [*const c_char; 3] =
        [c"ky,x1-emac".as_ptr(), c"spacemit,k1-emac".as_ptr(), ptr::null()];
    let sdhci_compat: [*const c_char; 4] = [
        c"ky,x1-sdhci".as_ptr(),
        c"spacemit,k1-sdhci".as_ptr(),
        c"marvell,sdhci-pxa1928".as_ptr(),
        ptr::null(),
    ];

    let soc = unsafe { (*root).lookup(c"soc".as_ptr(), ptr::null_mut()) };
    let device_parent = if soc.is_null() { root } else { soc };

    let mut soc_addr_cells = root_addr_cells;
    let mut soc_size_cells = root_size_cells;
    if !soc.is_null() {
        prop = unsafe { FdtNode::fdt_get_prop(soc, c"#address-cells".as_ptr()) };
        if !prop.is_null() {
            soc_addr_cells = unsafe { FdtNode::fdt_prop_u32(prop, 0) } as i32;
        }
        prop = unsafe { FdtNode::fdt_get_prop(soc, c"#size-cells".as_ptr()) };
        if !prop.is_null() {
            soc_size_cells = unsafe { FdtNode::fdt_prop_u32(prop, 0) } as i32;
        }
    }

    // SAFETY: `device_parent` live; its child tree is immutable during
    // the walk (boot-time, single hart).
    for node in unsafe { (*device_parent).children() } {

        let compat = unsafe { FdtNode::fdt_get_prop(node, c"compatible".as_ptr()) };
        let reg = unsafe { FdtNode::fdt_get_prop(node, c"reg".as_ptr()) };
        let interrupts = unsafe { FdtNode::fdt_get_prop(node, c"interrupts".as_ptr()) };

        // UART detection.
        if unsafe { platform.uart_base } == 0 && unsafe { FdtNode::fdt_prop_compat_list(compat, &uart_compat) } {
            let mut uart_addr: u64 = 0;
            if !reg.is_null() {
                unsafe { FdtNode::fdt_parse_reg_prop(reg, soc_addr_cells, soc_size_cells, &raw mut uart_addr, ptr::null_mut()) };
            }
            unsafe {
                platform.uart_base = uart_addr;
                if !interrupts.is_null() {
                    platform.uart_irq = FdtNode::fdt_prop_u32(interrupts, 0);
                }
                let clock_freq = FdtNode::fdt_get_prop(node, c"clock-frequency".as_ptr());
                if !clock_freq.is_null() {
                    platform.uart_clock = FdtNode::fdt_prop_u32(clock_freq, 0);
                }
                let baud = FdtNode::fdt_get_prop(node, c"current-speed".as_ptr());
                if !baud.is_null() {
                    platform.uart_baud = FdtNode::fdt_prop_u32(baud, 0);
                }
                let reg_shift = FdtNode::fdt_get_prop(node, c"reg-shift".as_ptr());
                platform.uart_reg_shift = if !reg_shift.is_null() { FdtNode::fdt_prop_u32(reg_shift, 0) } else { 0 };
                let reg_io_width = FdtNode::fdt_get_prop(node, c"reg-io-width".as_ptr());
                platform.uart_reg_io_width =
                    if !reg_io_width.is_null() { FdtNode::fdt_prop_u32(reg_io_width, 0) } else { 1 };
                crate::kprintln!(
                    "fdt: found UART at 0x{:x}, IRQ {}, clock {} Hz, baud {}, reg-shift {}, io-width {}",
                    (platform.uart_base as i64) as u64,
                    platform.uart_irq as c_int,
                    ((platform.uart_clock as c_int) as i32 as i64) as u64,
                    ((platform.uart_baud as c_int) as i32 as i64) as u64,
                    ((platform.uart_reg_shift as c_int) as i32 as i64) as u64,
                    ((platform.uart_reg_io_width as c_int) as i32 as i64) as u64,
                );
            }
        }

        // PLIC detection.
        if unsafe { platform.plic_base } == 0 && unsafe { FdtNode::fdt_prop_compat_list(compat, &plic_compat) } {
            if !reg.is_null() {
                unsafe {
                    FdtNode::fdt_parse_reg_prop(reg, soc_addr_cells, soc_size_cells, &raw mut platform.plic_base, &raw mut platform.plic_size);
                }
            }
        }

        // PCIe detection -- parse all reg entries.
        if unsafe { platform.has_pcie } == 0 && unsafe { FdtNode::fdt_prop_compat_list(compat, &pcie_compat) } {
            if !reg.is_null() {
                unsafe { platform.has_pcie = 1 };
                let entry_size = (soc_addr_cells + soc_size_cells) as u32 * size_of::<u32>() as u32;
                // SAFETY: `reg` live.
                let mut num_entries = (unsafe { (*reg).data_size } / entry_size) as i32;
                if num_entries > PCIE_REG_MAX as i32 {
                    num_entries = PCIE_REG_MAX as i32;
                }

                let reg_names = unsafe { FdtNode::fdt_get_prop(node, c"reg-names".as_ptr()) };
                let (names_data, names_len): (*const c_char, usize) = if !reg_names.is_null() {
                    (unsafe { (*reg_names).data_ptr() as *const c_char }, unsafe { (*reg_names).data_size } as usize)
                } else {
                    (ptr::null(), 0)
                };
                let mut name_pos = 0usize;

                let mut i = 0;
                while i < num_entries {
                    let offset = i * (soc_addr_cells + soc_size_cells);
                    unsafe {
                        let idx = i as usize;
                        if soc_addr_cells == 2 {
                            platform.pcie_reg[idx].base = FdtNode::fdt_prop_u64(reg, offset);
                        } else {
                            platform.pcie_reg[idx].base = FdtNode::fdt_prop_u32(reg, offset) as u64;
                        }
                        if soc_size_cells == 2 {
                            let hi = FdtNode::fdt_prop_u32(reg, offset + soc_addr_cells);
                            let lo = FdtNode::fdt_prop_u32(reg, offset + soc_addr_cells + 1);
                            platform.pcie_reg[idx].size = ((hi as u64) << 32) | lo as u64;
                        } else {
                            platform.pcie_reg[idx].size = FdtNode::fdt_prop_u32(reg, offset + soc_addr_cells) as u64;
                        }

                        if !names_data.is_null() && name_pos < names_len {
                            platform.pcie_reg[idx].name = names_data.add(name_pos);
                            name_pos += strnlen(names_data.add(name_pos), names_len - name_pos) + 1;
                        } else {
                            platform.pcie_reg[idx].name = ptr::null();
                        }
                        platform.pcie_reg_count += 1;
                    }
                    i += 1;
                }
            }
        }

        // VirtIO detection.
        if unsafe { FdtNode::fdt_prop_compat_list(compat, &virtio_compat) } && unsafe { platform.virtio_count } < 8 {
            unsafe { platform.has_virtio = 1 };
            if !reg.is_null() {
                unsafe {
                    let idx = platform.virtio_count as usize;
                    FdtNode::fdt_parse_reg_prop(reg, soc_addr_cells, soc_size_cells, &raw mut platform.virtio_base[idx], ptr::null_mut());
                }
            }
            if !interrupts.is_null() {
                unsafe {
                    let idx = platform.virtio_count as usize;
                    platform.virtio_irq[idx] = FdtNode::fdt_prop_u32(interrupts, 0);
                }
            }
            unsafe { platform.virtio_count += 1 };
        }

        // EMAC detection.
        if unsafe { FdtNode::fdt_prop_compat_list(compat, &emac_compat) } && unsafe { platform.emac_count } < EMAC_MAX as i32 {
            let idx = unsafe { platform.emac_count } as usize;
            unsafe { platform.has_emac = 1 };
            if !reg.is_null() {
                unsafe {
                    FdtNode::fdt_parse_reg_prop(
                        reg,
                        soc_addr_cells,
                        soc_size_cells,
                        &raw mut platform.emac[idx].base,
                        &raw mut platform.emac[idx].size,
                    );
                }
            }
            if !interrupts.is_null() {
                unsafe { platform.emac[idx].irq = FdtNode::fdt_prop_u32(interrupts, 0) };
            }
            unsafe {
                let p = FdtNode::fdt_get_prop(node, c"x1,apmu-base-reg".as_ptr());
                platform.emac[idx].apmu_base = if !p.is_null() { FdtNode::fdt_prop_u32(p, 0) } else { 0xD428_2800 };
                let p = FdtNode::fdt_get_prop(node, c"ctrl-reg".as_ptr());
                platform.emac[idx].ctrl_reg = if !p.is_null() { FdtNode::fdt_prop_u32(p, 0) } else { 0 };
                let p = FdtNode::fdt_get_prop(node, c"dline-reg".as_ptr());
                platform.emac[idx].dline_reg = if !p.is_null() { FdtNode::fdt_prop_u32(p, 0) } else { 0 };
                let p = FdtNode::fdt_get_prop(node, c"tx-phase".as_ptr());
                platform.emac[idx].tx_phase = if !p.is_null() { FdtNode::fdt_prop_u32(p, 0) } else { 0 };
                let p = FdtNode::fdt_get_prop(node, c"rx-phase".as_ptr());
                platform.emac[idx].rx_phase = if !p.is_null() { FdtNode::fdt_prop_u32(p, 0) } else { 0 };
                let p = FdtNode::fdt_get_prop(node, c"emac,reset-gpio".as_ptr());
                if !p.is_null() && (*p).data_size >= 8 {
                    platform.emac[idx].reset_gpio = FdtNode::fdt_prop_u32(p, 1);
                }
                crate::kprintln!(
                    "fdt: found EMAC{} at 0x{:x} size 0x{:x} IRQ {} gpio {} tx-phase {} rx-phase {}",
                    idx as c_int,
                    (platform.emac[idx].base as i64) as u64,
                    (platform.emac[idx].size as i64) as u64,
                    platform.emac[idx].irq as c_int,
                    platform.emac[idx].reset_gpio as c_int,
                    platform.emac[idx].tx_phase as c_int,
                    platform.emac[idx].rx_phase as c_int,
                );
                platform.emac_count += 1;
            }
        }

        // SDHCI detection.
        if unsafe { FdtNode::fdt_prop_compat_list(compat, &sdhci_compat) } && unsafe { platform.sdhci_count } < SDHCI_MAX as i32 {
            let idx = unsafe { platform.sdhci_count } as usize;
            unsafe { platform.has_sdhci = 1 };
            if !reg.is_null() {
                unsafe {
                    FdtNode::fdt_parse_reg_prop(
                        reg,
                        soc_addr_cells,
                        soc_size_cells,
                        &raw mut platform.sdhci[idx].base,
                        &raw mut platform.sdhci[idx].size,
                    );
                }
            }
            if !interrupts.is_null() {
                unsafe { platform.sdhci[idx].irq = FdtNode::fdt_prop_u32(interrupts, 0) };
            }

            let clks = unsafe { FdtNode::fdt_get_prop(node, c"clocks".as_ptr()) };
            if !clks.is_null() && unsafe { (*clks).data_size } >= 4 {
                let clk_phandle = unsafe { FdtNode::fdt_prop_u32(clks, 0) };
                let clk_ctrl = unsafe { FdtBlobInfo::fdt_phandle_lookup(self as *const FdtBlobInfo as *mut FdtBlobInfo, clk_phandle) };
                if !clk_ctrl.is_null() {
                    let cc_reg = unsafe { FdtNode::fdt_get_prop(clk_ctrl, c"reg".as_ptr()) };
                    let cc_names = unsafe { FdtNode::fdt_get_prop(clk_ctrl, c"reg-names".as_ptr()) };
                    if !cc_reg.is_null() && !cc_names.is_null() {
                        let entry_cells = soc_addr_cells + soc_size_cells;
                        // SAFETY: `cc_reg` live.
                        let n_entries =
                            unsafe { (*cc_reg).data_size } as i32 / (entry_cells * size_of::<u32>() as i32);
                        let ndata = unsafe { (*cc_names).data_ptr() as *const c_char };
                        let nlen = unsafe { (*cc_names).data_size } as usize;
                        let mut npos = 0usize;
                        let mut e = 0;
                        while e < n_entries {
                            let rn: *const c_char =
                                if npos < nlen { unsafe { ndata.add(npos) } } else { ptr::null() };
                            let off = e * entry_cells;
                            let rbase: u64;
                            unsafe {
                                if soc_addr_cells == 2 {
                                    rbase = ((FdtNode::fdt_prop_u32(cc_reg, off) as u64) << 32) | FdtNode::fdt_prop_u32(cc_reg, off + 1) as u64;
                                } else {
                                    rbase = FdtNode::fdt_prop_u32(cc_reg, off) as u64;
                                }

                                if !rn.is_null() && strncmp(rn, c"apmu".as_ptr(), 5) == 0 {
                                    platform.sdhci[idx].apmu_base = rbase as u32;
                                } else if !rn.is_null() && strncmp(rn, c"apbc".as_ptr(), 5) == 0 {
                                    platform.sdhci[idx].apbc_base = rbase as u32;
                                }

                                if npos < nlen {
                                    npos += strnlen(ndata.add(npos), nlen - npos) + 1;
                                }
                            }
                            e += 1;
                        }
                    }
                }
            }

            unsafe {
                if platform.sdhci[idx].base == 0xD428_0000 {
                    platform.sdhci[idx].apmu_offset = 0x54;
                } else if platform.sdhci[idx].base == 0xD428_0800 {
                    platform.sdhci[idx].apmu_offset = 0x58;
                } else if platform.sdhci[idx].base == 0xD428_1000 {
                    platform.sdhci[idx].apmu_offset = 0xE0;
                }
                platform.sdhci[idx].apmu_axi_offset = 0x54;

                let bw = FdtNode::fdt_get_prop(node, c"bus-width".as_ptr());
                platform.sdhci[idx].bus_width = if !bw.is_null() { FdtNode::fdt_prop_u32(bw, 0) as i32 } else { 4 };

                let hs400 = FdtNode::fdt_get_prop(node, c"mmc-hs400-1_8v".as_ptr());
                if !hs400.is_null() || platform.sdhci[idx].bus_width == 8 {
                    platform.sdhci[idx].is_emmc = 1;
                }
                let nr = FdtNode::fdt_get_prop(node, c"non-removable".as_ptr());
                if !nr.is_null() && platform.sdhci[idx].is_emmc == 0 && platform.sdhci[idx].bus_width == 4 {
                    platform.sdhci[idx].is_sdio = 1;
                }

                crate::kprintln!(
                    "fdt: found SDHCI{} at 0x{:x} size 0x{:x} IRQ {} bus-width {} apmu 0x{:x}+0x{:x} apbc 0x{:x} {}{}",
                    idx as c_int,
                    (platform.sdhci[idx].base as i64) as u64,
                    (platform.sdhci[idx].size as i64) as u64,
                    platform.sdhci[idx].irq as c_int,
                    platform.sdhci[idx].bus_width as c_int,
                    ((platform.sdhci[idx].apmu_base as c_int) as i32 as i64) as u64,
                    ((platform.sdhci[idx].apmu_offset as c_int) as i32 as i64) as u64,
                    ((platform.sdhci[idx].apbc_base as c_int) as i32 as i64) as u64,
                    crate::printf::Cs(if platform.sdhci[idx].is_emmc != 0 { c"(eMMC)".as_ptr() } else { c"".as_ptr() }),
                    crate::printf::Cs(if platform.sdhci[idx].is_sdio != 0 { c"(SDIO)".as_ptr() } else { c"".as_ptr() }),
                );
                platform.sdhci_count += 1;
            }
        }
    }

    // Reserved memory: copy `blob->reserved` (memreserve block), then
    // augment with the `/reserved-memory` node's children (if any).
    unsafe {
        platform.reserved = self.reserved;
        platform.reserved_count = self.reserved_count;
    }

    let rsvmem = unsafe { (*root).lookup(c"reserved-memory".as_ptr(), ptr::null_mut()) };
    if !rsvmem.is_null() {
        let mut rsv_addr_cells = root_addr_cells;
        let mut rsv_size_cells = root_size_cells;
        prop = unsafe { FdtNode::fdt_get_prop(rsvmem, c"#address-cells".as_ptr()) };
        if !prop.is_null() {
            rsv_addr_cells = unsafe { FdtNode::fdt_prop_u32(prop, 0) } as i32;
        }
        prop = unsafe { FdtNode::fdt_get_prop(rsvmem, c"#size-cells".as_ptr()) };
        if !prop.is_null() {
            rsv_size_cells = unsafe { FdtNode::fdt_prop_u32(prop, 0) } as i32;
        }

        let mut rsv_child_count = 0i32;
        // SAFETY: `rsvmem` live; its child tree is immutable during the
        // walk (boot-time, single hart).
        for rsv_node in unsafe { (*rsvmem).children() } {
            if !unsafe { FdtNode::fdt_get_prop(rsv_node, c"reg".as_ptr()) }.is_null() {
                rsv_child_count += 1;
            }
        }

        if rsv_child_count > 0 {
            let total_count = unsafe { platform.reserved_count } + rsv_child_count;
            // SAFETY: allocator call.
            let new_reserved = unsafe {
                early_alloc_align(total_count as usize * size_of::<mem_region>(), align_of::<u64>())
            } as *mut mem_region;
            if !new_reserved.is_null() {
                unsafe {
                    for i in 0..platform.reserved_count {
                        *new_reserved.add(i as usize) = *platform.reserved.offset(i as isize);
                    }
                }
                let mut idx = unsafe { platform.reserved_count };
                // SAFETY: `rsvmem` live; child tree immutable during walk.
                for rsv_node in unsafe { (*rsvmem).children() } {
                    if idx >= total_count {
                        break;
                    }
                    prop = unsafe { FdtNode::fdt_get_prop(rsv_node, c"reg".as_ptr()) };
                    if !prop.is_null() {
                        unsafe {
                            let base_val: u64 = if rsv_addr_cells == 2 {
                                ((FdtNode::fdt_prop_u32(prop, 0) as u64) << 32) | FdtNode::fdt_prop_u32(prop, 1) as u64
                            } else {
                                FdtNode::fdt_prop_u32(prop, 0) as u64
                            };
                            let size_val: u64 = if rsv_size_cells == 2 {
                                ((FdtNode::fdt_prop_u32(prop, rsv_addr_cells) as u64) << 32)
                                    | FdtNode::fdt_prop_u32(prop, rsv_addr_cells + 1) as u64
                            } else {
                                FdtNode::fdt_prop_u32(prop, rsv_addr_cells) as u64
                            };
                            (*new_reserved.add(idx as usize)).base = base_val;
                            (*new_reserved.add(idx as usize)).size = size_val;
                        }
                        idx += 1;
                    }
                }
                unsafe {
                    platform.reserved = new_reserved;
                    platform.reserved_count = idx;
                }
            }
        }
    }
}
}

/// `MAX_MEM_REGIONS` (`kernel/inc/dev/fdt.h`) -- matches
/// `platform_info::mem`'s bindgen array length exactly (both derive from
/// the same header).
#[inline(always)]
const fn platform_info_mem_regions() -> i32 {
    8
}

impl Fdt {
/// Rust port of `Fdt::fdt_init()`.
///
/// # Safety
/// `dtb` must be null or a valid FDT blob pointer.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn fdt_init(dtb: *mut c_void) -> c_int {
    crate::kprintln!("fdt: checking DTB at {}", crate::printf::Ptr(dtb as u64));

    // SAFETY: caller contract.
    if !unsafe { Fdt::fdt_valid(dtb) } {
        crate::kprintln!("fdt: no valid DTB found!");
        return -1;
    }

    // SAFETY: `dtb` valid per the check above.
    crate::kprintln!("fdt: using DTB at {} (size {} bytes)", crate::printf::Ptr(dtb as u64), unsafe { Fdt::fdt_totalsize(dtb) } as c_int);

    // SAFETY: `platform` is exclusively written here at single-threaded
    // boot, before any other hart or interrupt is live (see module doc).
    unsafe { memset(&raw mut platform as *mut c_void, 0, size_of::<platform_info>()) };

    // SAFETY: `dtb` valid.
    let blob = unsafe { FdtBlobInfo::build(dtb) };
    if blob.is_null() {
        crate::kprintln!("fdt: failed to build blob info!");
        return -1;
    }
    unsafe { FDT_BLOB = blob };

    crate::kprintln!("fdt: parsed {} nodes", unsafe { (*blob).n_nodes });

    // SAFETY: `blob` live.
    unsafe { (*blob).build_indexes() };
    unsafe { (*blob).extract_platform_info() };

    crate::kprintln!("fdt: probed platform info:");
    unsafe {
        crate::kprintln!(
            "  Memory regions: {} (total {} MB)",
            platform.mem_count,
            (platform.total_mem / (1024 * 1024)) as i64,
        );
        for i in 0..platform.mem_count {
            let idx = i as usize;
            crate::kprintln!(
                "    [{}] 0x{:x} - 0x{:x} ({} MB)",
                i,
                (platform.mem[idx].base as i64) as u64,
                ((platform.mem[idx].base + platform.mem[idx].size) as i64) as u64,
                (platform.mem[idx].size / (1024 * 1024)) as i64,
            );
        }
        crate::kprintln!("  Reserved regions: {}", platform.reserved_count);
        for i in 0..platform.reserved_count {
            let r = *platform.reserved.offset(i as isize);
            crate::kprintln!(
                "    [{}] 0x{:x} - 0x{:x} ({} KB)",
                i,
                (r.base as i64) as u64,
                ((r.base + r.size) as i64) as u64,
                (r.size / 1024) as i64,
            );
        }
        if platform.has_ramdisk != 0 {
            crate::kprintln!(
                "  Ramdisk: 0x{:x} - 0x{:x} ({} KB)",
                (platform.ramdisk_base as i64) as u64,
                ((platform.ramdisk_base + platform.ramdisk_size) as i64) as u64,
                (platform.ramdisk_size / 1024) as i64,
            );
        }
        crate::kprint!("  UART: 0x{:x}, IRQ {}", (platform.uart_base as i64) as u64, platform.uart_irq as c_int);
        if platform.uart_clock != 0 || platform.uart_baud != 0 {
            crate::kprint!(
                ", clock {} Hz, baud {}",
                (((if platform.uart_clock != 0 { platform.uart_clock } else { 1_843_200 }) as c_int) as i32 as i64) as u64,
                (((if platform.uart_baud != 0 { platform.uart_baud } else { 115_200 }) as c_int) as i32 as i64) as u64,
            );
        }
        crate::kprintln!();
        crate::kprintln!("  PLIC: 0x{:x} (size 0x{:x})", (platform.plic_base as i64) as u64, (platform.plic_size as i64) as u64);
        if platform.has_pcie != 0 {
            crate::kprintln!("  PCIe regions: {}", platform.pcie_reg_count);
            for i in 0..platform.pcie_reg_count {
                let idx = i as usize;
                if !platform.pcie_reg[idx].name.is_null() {
                    crate::kprintln!(
                        "    [{}] {}: 0x{:x} (size 0x{:x})",
                        i,
                        crate::printf::Cs(platform.pcie_reg[idx].name),
                        (platform.pcie_reg[idx].base as i64) as u64,
                        (platform.pcie_reg[idx].size as i64) as u64,
                    );
                } else {
                    crate::kprintln!(
                        "    [{}] 0x{:x} (size 0x{:x})",
                        i,
                        (platform.pcie_reg[idx].base as i64) as u64,
                        (platform.pcie_reg[idx].size as i64) as u64,
                    );
                }
            }
        }
        crate::kprintln!("  CPUs: {}, timebase: {} Hz", platform.ncpu, platform.timebase_freq as i64);

        if platform.has_virtio != 0 {
            crate::kprintln!("  VirtIO devices: {}", platform.virtio_count);
            for i in 0..platform.virtio_count {
                let idx = i as usize;
                crate::kprintln!(
                    "    [{}] 0x{:x}, IRQ {}",
                    i,
                    (platform.virtio_base[idx] as i64) as u64,
                    platform.virtio_irq[idx] as c_int,
                );
            }
        }
    }

    0
}
}

// ===========================================================================
// Debug dump/walk -- never called from `start_kernel.c` (the one call
// site, `Fdt::fdt_walk(fdt_base)`, is commented out there), kept for ABI
// parity with `kernel/inc/dev/fdt.h`.
// ===========================================================================

impl Fdt {
/// Rust port of `Fdt::fdt_dump()`.
///
/// # Safety
/// `dtb` must be null or a valid FDT blob pointer.
// P3-1D mesh sweep: no live caller anywhere in the tree today -- demoted;
// `#[allow(dead_code)]` documents the gap, same precedent as
// `fdt_node_lookup` above.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn fdt_dump(dtb: *mut c_void) {
    // SAFETY: caller contract.
    if !unsafe { Fdt::fdt_valid(dtb) } {
        crate::kprintln!("fdt_dump: invalid DTB");
        return;
    }
    let base = dtb as *const u8;
    crate::kprintln!("FDT at {}:", crate::printf::Ptr(dtb as u64));
    // SAFETY: `dtb` valid per the check above.
    unsafe {
        crate::kprintln!("  magic: 0x{:x}", ((Fdt::read_be32(base, FDT_HDR_MAGIC) as c_int) as i32 as i64) as u64);
        crate::kprintln!("  totalsize: {}", Fdt::read_be32(base, FDT_HDR_TOTALSIZE) as c_int);
        crate::kprintln!("  off_dt_struct: 0x{:x}", ((Fdt::read_be32(base, FDT_HDR_OFF_DT_STRUCT) as c_int) as i32 as i64) as u64);
        crate::kprintln!("  off_dt_strings: 0x{:x}", ((Fdt::read_be32(base, FDT_HDR_OFF_DT_STRINGS) as c_int) as i32 as i64) as u64);
        crate::kprintln!("  version: {}", Fdt::read_be32(base, 20) as c_int);
    }
}
}

impl Fdt {
fn fdt_print_indent(depth: i32) {
    for _ in 0..depth {
        crate::kprint!("  ");
    }
}
}

impl Fdt {
/// Mirrors `Fdt::fdt_print_prop_value()`.
///
/// # Safety
/// `data` must point to at least `len` readable bytes.
unsafe fn fdt_print_prop_value(data: *const u8, len: u32) {
    if len == 0 {
        crate::kprintln!("(empty)");
        return;
    }

    let mut is_string = true;
    let mut null_count = 0u32;
    // SAFETY: caller contract.
    unsafe {
        for i in 0..len {
            let c = *data.add(i as usize);
            if c == 0 {
                null_count += 1;
            } else if c < 0x20 || c > 0x7e {
                is_string = false;
                break;
            }
        }
    }

    // SAFETY: `len > 0` here (checked above), so `len - 1` is in-bounds.
    if is_string && null_count > 0 && unsafe { *data.add(len as usize - 1) } == 0 {
        let end = unsafe { data.add(len as usize) };
        let mut s = data;
        let mut first = true;
        while s < end {
            // SAFETY: `s < end`, in-bounds.
            if unsafe { *s } != 0 {
                if !first {
                    crate::kprint!(", ");
                }
                crate::kprint!("\"{}\"", crate::printf::Cs(s as *const c_char));
                first = false;
                while s < end && unsafe { *s } != 0 {
                    s = unsafe { s.add(1) };
                }
            }
            s = unsafe { s.add(1) };
        }
        crate::kprintln!();
        return;
    }

    if len % 4 == 0 {
        crate::kprint!("<");
        let ncells = len / 4;
        for i in 0..ncells {
            // SAFETY: caller contract; `i*4+4 <= len`.
            let cell = unsafe { Fdt::read_be32(data, i as usize * 4) };
            crate::kprint!("0x{:x}", ((cell as c_int) as i32 as i64) as u64);
            if i < ncells - 1 {
                crate::kprint!(" ");
            }
        }
        crate::kprintln!(">");
        return;
    }

    crate::kprint!("[");
    let show = if len < 32 { len } else { 32 };
    for i in 0..show {
        // SAFETY: caller contract; `i < len`.
        let b = unsafe { *data.add(i as usize) };
        // NOTE: preserved bug -- legacy printf_rust never zero-pads a
        // non-left-aligned width (see migration spec); `%02x` is really
        // just `%x` in the original's actual behavior.
        crate::kprint!("{:x}", ((b as c_int) as i32 as i64) as u64);
        if i < len - 1 {
            crate::kprint!(" ");
        }
    }
    if len > 32 {
        crate::kprint!(" ...");
    }
    crate::kprintln!("]");
}
}

impl FdtNode {
/// Mirrors `__fdt_walk_node()`.
///
/// # Safety
/// `node` must be null or point to a live `FdtNode`.
unsafe fn fdt_walk_node(node: *mut FdtNode, depth: i32) {
    if node.is_null() {
        return;
    }
    Fdt::fdt_print_indent(depth);

    // SAFETY: caller contract.
    let data_size = unsafe { (*node).data_size };
    if data_size > 0 {
        unsafe { crate::kprint!("{} = ", crate::printf::Cs((*node).name)) };
        unsafe { Fdt::fdt_print_prop_value((*node).data_ptr(), data_size) };
    } else {
        unsafe {
            if *(*node).name == 0 {
                crate::kprintln!("/");
            } else if (*node).has_addr {
                crate::kprintln!("{}@{:x}/", crate::printf::Cs((*node).name), ((*node).addr as i64) as u64);
            } else {
                crate::kprintln!("{}/", crate::printf::Cs((*node).name));
            }

            if (*node).has_phandle {
                Fdt::fdt_print_indent(depth + 1);
                crate::kprintln!("phandle = <0x{:x}>", (((*node).phandle as c_int) as i32 as i64) as u64);
            }

            for child in (*node).children() {
                FdtNode::fdt_walk_node(child, depth + 1);
            }
        }
    }
}
}

impl Fdt {
/// Rust port of `Fdt::fdt_walk()`.
///
/// # Safety
/// No preconditions on `dtb` (unused -- matches the C original, which
/// walks the already-parsed tree instead).
// P3-1D mesh sweep: no live caller anywhere in the tree today -- the one
// potential call site (`start_kernel.rs`) is commented out (`// fdt_walk
// (fdt_base);`) -- demoted; `#[allow(dead_code)]` documents the gap,
// same precedent as `fdt_node_lookup` above.
#[allow(dead_code)]
pub(crate) unsafe extern "C" fn fdt_walk(_dtb: *mut c_void) {
    // SAFETY: `FDT_BLOB` is written once (single-threaded boot) by
    // `fdt_init` before this could ever run.
    let blob = unsafe { FDT_BLOB };
    if blob.is_null() {
        crate::kprintln!("fdt_walk: no parsed FDT tree available");
        return;
    }

    crate::kprintln!("=== FDT Walk (from parsed tree) ===");
    crate::kprintln!("Parsed {} nodes\n", unsafe { (*blob).n_nodes });

    // SAFETY: `blob` live.
    let root_rb = unsafe { (*blob).root.node };
    if !root_rb.is_null() {
        unsafe { FdtNode::fdt_walk_node(root_rb as *mut FdtNode, 0) };
    }

    crate::kprintln!("\n=== End of FDT ===");
}
}

impl Fdt {
/// Rust port of `Fdt::fdt_apply_platform_config()`.
///
/// # Safety
/// Must run exactly once, on the boot hart, after [`fdt_init`] and before
/// any other hart or interrupt is live (matches the C original's
/// `start_kernel.c` call site and every global it writes -- see this
/// module's doc comment for the full inventory).
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn fdt_apply_platform_config() {
    // SAFETY: single boot-time writer, see module doc + this fn's own
    // Safety section.
    unsafe {
        if platform.mem_count > 0 && platform.mem[0].size > 0 {
            // P3-D3c: single boot-time writer sequence (`start_kernel.rs`
            // first, this file refines second), both on the boot hart
            // before any other hart or interrupt is live; written via
            // crate path instead of `extern "C"` redeclarations.
            crate::start_kernel::__physical_memory_start = platform.mem[0].base;
            crate::start_kernel::__physical_memory_end = platform.mem[0].base + platform.mem[0].size;
            crate::start_kernel::__physical_total_pages = platform.mem[0].size >> 12;
        }

        if platform.uart_base != 0 {
            // P3-1C mesh sweep: `uart.rs` is in scope for this wave, so its
            // `__uart0_*` globals are written via crate path instead of an
            // `extern "C"` redeclaration.
            crate::uart::__uart0_mmio_base = platform.uart_base;
            crate::uart::__uart0_irqno = platform.uart_irq as u64;
            crate::uart::__uart0_clock = platform.uart_clock;
            crate::uart::__uart0_baud = platform.uart_baud;
            crate::uart::__uart0_reg_shift = platform.uart_reg_shift;
            crate::uart::__uart0_reg_io_width = platform.uart_reg_io_width;
        }
        if platform.plic_base != 0 {
            // P3-D3c: `irq/plic.rs` global, single boot-time writer (this
            // file); written via crate path.
            crate::irq::plic::__plic_mmio_base = platform.plic_base;
        }

        if platform.has_pcie != 0 {
            let mut found = false;
            for i in 0..platform.pcie_reg_count {
                let idx = i as usize;
                if !platform.pcie_reg[idx].name.is_null()
                    && strncmp(platform.pcie_reg[idx].name, c"config".as_ptr(), 6) == 0
                {
                    __pcie_ecam_mmio_base = platform.pcie_reg[idx].base;
                    found = true;
                    break;
                }
            }
            let _ = found;
            if __pcie_ecam_mmio_base == 0 && platform.pcie_reg_count > 0 {
                __pcie_ecam_mmio_base = platform.pcie_reg[0].base;
            }
        }

        if platform.has_virtio != 0 {
            let n = core::cmp::min(platform.virtio_count, N_VIRTIO as i32);
            for i in 0..n {
                let idx = i as usize;
                __virtio_mmio_base[idx] = platform.virtio_base[idx];
                __virtio_irqno[idx] = platform.virtio_irq[idx] as u64;
            }
        }

        if platform.timebase_freq != 0 {
            // P3-D3c: `timer/timer_core.rs` global, boot-time writer
            // sequence (`start.rs`'s `timerinit()` first, this file
            // refines second); written via crate path.
            crate::timer::timer_core::__jiff_ticks = platform.timebase_freq / HZ;
        }
    }
}
}
