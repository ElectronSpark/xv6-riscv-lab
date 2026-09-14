//! Boot-time device discovery over checked, borrowed FDT bytes.
//!
//! `wire` contains the allocation-free parser and typed views. Only the boot
//! adapter below accepts firmware pointers or touches the early allocator and
//! platform globals. It runs on the boot hart before secondary harts, interrupts,
//! paging, or the normal allocator are enabled. The validated DTB is copied into
//! permanent early storage, keeping exported PCI register names alive after the
//! firmware's original buffer can be reclaimed.
//!
//! The public platform records retain their established C layout. No allocated
//! node tree, raw string scanning, hash chains, or rb-tree indexes are needed.

use core::ffi::{c_int, c_void, CStr};
use core::mem::{align_of, offset_of, size_of};
use core::ptr;

use crate::bindings::{EMAC_MAX, N_VIRTIO, PCIE_REG_MAX, SDHCI_MAX};
use crate::mm::early_allocator::early_alloc_align;
use crate::pci::__pcie_ecam_mmio_base;
use crate::virtio_disk::{__virtio_irqno, __virtio_mmio_base};

mod wire;
use wire::{CellConfig, DeviceTree, Error, Node, Property, Region};

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
/// platform description populated by [`Fdt::init`] from the device tree.
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

const fn platform_info_mem_regions() -> i32 { 8 }

impl PlatformInfo {
    const EMPTY: Self = Self {
        mem: [MemRegion { base: 0, size: 0 }; 8], mem_count: 0,
        reserved: ptr::null_mut(), reserved_count: 0,
        ramdisk_base: 0, ramdisk_size: 0, has_ramdisk: 0, total_mem: 0,
        uart_base: 0, uart_irq: 0, uart_clock: 0, uart_baud: 0,
        uart_reg_shift: 0, uart_reg_io_width: 0, plic_base: 0, plic_size: 0,
        has_pcie: 0, pcie_reg: [PlatformPcieReg { base: 0, size: 0, name: ptr::null() }; PCIE_REG_MAX as usize],
        pcie_reg_count: 0, has_virtio: 0, virtio_base: [0; 8], virtio_irq: [0; 8], virtio_count: 0,
        timebase_freq: 0, ncpu: 0, has_emac: 0,
        emac: [PlatformEmac { base: 0, size: 0, irq: 0, apmu_base: 0, ctrl_reg: 0,
            dline_reg: 0, reset_gpio: 0, tx_phase: 0, rx_phase: 0 }; EMAC_MAX as usize],
        emac_count: 0, has_sdhci: 0,
        sdhci: [PlatformSdhci { base: 0, size: 0, irq: 0, apmu_base: 0, apmu_offset: 0,
            apmu_axi_offset: 0, apbc_base: 0, is_emmc: 0, is_sdio: 0, bus_width: 0 }; SDHCI_MAX as usize],
        sdhci_count: 0,
    };
}

/// Published once by the boot hart; existing drivers consume this ABI record.
pub(crate) static mut platform: PlatformInfo = PlatformInfo::EMPTY;
static mut FDT_BLOB: Option<DeviceTree<'static>> = None;
static mut PCIE_CONFIG_BASE: Option<u64> = None;

struct ParsedPlatform<'a> {
    info: PlatformInfo,
    pcie_names: [Option<&'a CStr>; PCIE_REG_MAX as usize],
    pcie_config_base: Option<u64>,
}

fn first_reg(node: Node<'_>, cells: CellConfig) -> Result<Region, Error> {
    match node.property(c"reg") {
        Some(prop) => prop.regions(cells)?.next().ok_or(Error::Cells),
        None => Ok(Region::default()),
    }
}

fn irq(node: Node<'_>) -> Result<u32, Error> {
    node.property(c"interrupts").map_or(Ok(0), |prop| prop.cell(0))
}

/// Decode supported devices into a local record. A malformed used property
/// aborts discovery before any global platform configuration is published.
fn parse_platform(tree: DeviceTree<'_>) -> Result<ParsedPlatform<'_>, Error> {
    let mut parsed = ParsedPlatform { info: PlatformInfo::EMPTY,
        pcie_names: [None; PCIE_REG_MAX as usize], pcie_config_base: None };
    let info = &mut parsed.info;
    let root = tree.root();
    let root_cells = CellConfig::for_node(root, CellConfig::ROOT_DEFAULT)?;
    if let Some(cpus) = root.child(c"cpus") {
        info.timebase_freq = cpus.property(c"timebase-frequency").map_or(Ok(0), Property::integer)?;
        info.ncpu = c_int::try_from(cpus.children_named(b"cpu").count()).map_err(|_| Error::Range)?;
    }
    info.ncpu = info.ncpu.max(1);
    for node in root.children_named(b"memory") {
        if let Some(prop) = node.property(c"reg") {
            for region in prop.regions(root_cells)? {
                if region.size == 0 { continue; }
                if info.mem_count as usize == info.mem.len() { break; }
                info.mem[info.mem_count as usize] = MemRegion { base: region.base, size: region.size };
                info.total_mem = info.total_mem.checked_add(region.size).ok_or(Error::Range)?;
                info.mem_count += 1;
            }
        }
    }
    if let Some(chosen) = root.child(c"chosen") {
        let start = chosen.property(c"linux,initrd-start").map_or(Ok(0), Property::integer)?;
        let end = chosen.property(c"linux,initrd-end").map_or(Ok(0), Property::integer)?;
        if start != 0 && end > start {
            info.ramdisk_base = start;
            info.ramdisk_size = end - start;
            info.has_ramdisk = 1;
        }
    }

    let parent = root.child(c"soc").unwrap_or(root);
    let cells = CellConfig::for_node(parent, root_cells)?;
    for node in parent.ordered_children() {
        if info.uart_base == 0 && node.compatible(&[c"ns16550a", c"ns16550", c"snps,dw-apb-uart",
            c"ti,omap3-uart", c"xlnx,xuartps", c"ky,pxa-uart", c"arm,sbsa-uart"])? {
            let reg = first_reg(node, cells)?;
            info.uart_base = reg.base;
            info.uart_irq = irq(node)?;
            info.uart_clock = node.u32_or(c"clock-frequency", 0)?;
            info.uart_baud = node.u32_or(c"current-speed", 0)?;
            info.uart_reg_shift = node.u32_or(c"reg-shift", 0)?;
            info.uart_reg_io_width = node.u32_or(c"reg-io-width", 1)?;
            if reg.base != 0 { wire::uart_window(reg, info.uart_reg_shift, info.uart_reg_io_width)?; }
        }
        if info.plic_base == 0 && node.compatible(&[c"riscv,plic0", c"sifive,plic-1.0.0",
            c"thead,c900-plic", c"andestech,nceplic100"])? {
            let reg = first_reg(node, cells)?;
            info.plic_base = reg.base;
            info.plic_size = reg.size;
        }
        if info.has_pcie == 0 && node.compatible(&[c"pci-host-ecam-generic", c"pci-host-cam-generic",
            c"x1,dwc-pcie", c"spacemit,k1-pcie"])? {
            if let Some(prop) = node.property(c"reg") {
                let mut names = node.property(c"reg-names").map(Property::strings).transpose()?;
                info.has_pcie = 1;
                for (index, region) in prop.regions(cells)?.take(info.pcie_reg.len()).enumerate() {
                    let name = names.as_mut().and_then(Iterator::next);
                    info.pcie_reg[index] = PlatformPcieReg { base: region.base, size: region.size,
                        name: name.map_or(ptr::null(), CStr::as_ptr) };
                    parsed.pcie_names[index] = name;
                    if parsed.pcie_config_base.is_none() && name.is_some_and(|name| name.to_bytes().starts_with(b"config")) {
                        parsed.pcie_config_base = Some(region.base);
                    }
                    info.pcie_reg_count += 1;
                }
            }
        }
        if node.compatible(&[c"virtio,mmio"])? && (info.virtio_count as usize) < info.virtio_base.len() {
            let index = info.virtio_count as usize;
            info.has_virtio = 1;
            info.virtio_base[index] = first_reg(node, cells)?.base;
            info.virtio_irq[index] = irq(node)?;
            info.virtio_count += 1;
        }
        if node.compatible(&[c"ky,x1-emac", c"spacemit,k1-emac"])? && (info.emac_count as usize) < info.emac.len() {
            let emac = &mut info.emac[info.emac_count as usize];
            let reg = first_reg(node, cells)?;
            emac.base = reg.base;
            emac.size = reg.size;
            emac.irq = irq(node)?;
            emac.apmu_base = node.u32_or(c"x1,apmu-base-reg", 0xD428_2800)?;
            emac.ctrl_reg = node.u32_or(c"ctrl-reg", 0)?;
            emac.dline_reg = node.u32_or(c"dline-reg", 0)?;
            emac.tx_phase = node.u32_or(c"tx-phase", 0)?;
            emac.rx_phase = node.u32_or(c"rx-phase", 0)?;
            emac.reset_gpio = node.property(c"emac,reset-gpio").map_or(Ok(0), |prop| prop.cell(1))?;
            info.has_emac = 1;
            info.emac_count += 1;
        }
        if node.compatible(&[c"ky,x1-sdhci", c"spacemit,k1-sdhci", c"marvell,sdhci-pxa1928"])?
            && (info.sdhci_count as usize) < info.sdhci.len() {
            let sdhci = &mut info.sdhci[info.sdhci_count as usize];
            let reg = first_reg(node, cells)?;
            sdhci.base = reg.base;
            sdhci.size = reg.size;
            sdhci.irq = irq(node)?;
            if let Some(clocks) = node.property(c"clocks") {
                if let Some((controller, bus)) = tree.find_phandle(clocks.cell(0)?)? {
                    if let (Some(reg), Some(names)) = (controller.property(c"reg"), controller.property(c"reg-names")) {
                        let clock_cells = CellConfig::for_node(bus, root_cells)?;
                        for (region, name) in reg.regions(clock_cells)?.zip(names.strings()?) {
                            if name == c"apmu" { sdhci.apmu_base = u32::try_from(region.base).map_err(|_| Error::Range)?; }
                            if name == c"apbc" { sdhci.apbc_base = u32::try_from(region.base).map_err(|_| Error::Range)?; }
                        }
                    }
                }
            }
            sdhci.apmu_offset = match sdhci.base { 0xD428_0000 => 0x54, 0xD428_0800 => 0x58, 0xD428_1000 => 0xE0, _ => 0 };
            sdhci.apmu_axi_offset = 0x54;
            sdhci.bus_width = c_int::try_from(node.u32_or(c"bus-width", 4)?).map_err(|_| Error::Range)?;
            sdhci.is_emmc = i32::from(node.property(c"mmc-hs400-1_8v").is_some() || sdhci.bus_width == 8);
            sdhci.is_sdio = i32::from(node.property(c"non-removable").is_some() && sdhci.is_emmc == 0 && sdhci.bus_width == 4);
            info.has_sdhci = 1;
            info.sdhci_count += 1;
        }
    }
    Ok(parsed)
}

fn visit_reserved(tree: DeviceTree<'_>, mut visit: impl FnMut(Region) -> Result<(), Error>) -> Result<(), Error> {
    for region in tree.reservations() { visit(region)?; }
    let root = tree.root();
    if let Some(reserved) = root.child(c"reserved-memory") {
        let cells = CellConfig::for_node(root, CellConfig::ROOT_DEFAULT)?;
        let cells = CellConfig::for_node(reserved, cells)?;
        for node in reserved.ordered_children() {
            if let Some(prop) = node.property(c"reg") {
                // Every tuple reserves memory, including second/subsequent
                // ranges that the old first-register-only helper discarded.
                for region in prop.regions(cells)? { visit(region)?; }
            }
        }
    }
    Ok(())
}

/// # Safety
/// The non-null firmware pointer must be mapped and readable for HEADER_LEN
/// bytes and then for its declared total size, all in one allocation. Those
/// bytes must remain immutable for `'a`. Header checks cannot prove this raw
/// mapping contract; they only validate the contents within that extent.
unsafe fn firmware_tree<'a>(dtb: *const c_void) -> Result<DeviceTree<'a>, Error> {
    if dtb.is_null() || (dtb as usize).checked_add(wire::HEADER_LEN).is_none() { return Err(Error::Bounds); }
    // SAFETY: fixed header readability is part of the caller's contract.
    let header = unsafe { core::slice::from_raw_parts(dtb.cast::<u8>(), wire::HEADER_LEN) };
    let len = wire::total_size(header)?;
    (dtb as usize).checked_add(len).ok_or(Error::Bounds)?;
    // SAFETY: caller guarantees the entire declared extent and its lifetime.
    DeviceTree::parse(unsafe { core::slice::from_raw_parts(dtb.cast::<u8>(), len) })
}

/// # Safety
/// Single boot hart with initialized early allocator and no concurrent users.
/// Its arena must be disjoint from `tree`, whose bytes are copied before return.
unsafe fn own_tree(tree: DeviceTree<'_>) -> Result<DeviceTree<'static>, Error> {
    let bytes = tree.bytes();
    // SAFETY: exclusive early allocator access, permanent allocated storage.
    let storage = unsafe { early_alloc_align(bytes.len(), align_of::<u64>()) }.cast::<u8>();
    if storage.is_null() { return Err(Error::Allocation); }
    // SAFETY: fresh disjoint allocation, fully initialized before borrowing.
    unsafe { ptr::copy_nonoverlapping(bytes.as_ptr(), storage, bytes.len()); }
    // SAFETY: permanently owned immutable early storage, initialized above.
    DeviceTree::parse(unsafe { core::slice::from_raw_parts(storage, bytes.len()) })
}

/// # Safety
/// Same exclusive early-allocator contract as `own_tree`. The returned slice
/// owns fresh permanent storage and must not be aliased while being populated.
unsafe fn reserved_storage(count: usize) -> Result<&'static mut [MemRegion], Error> {
    if count == 0 { return Ok(&mut []); }
    let bytes = count.checked_mul(size_of::<MemRegion>()).filter(|n| *n <= isize::MAX as usize).ok_or(Error::Bounds)?;
    // SAFETY: boot-only allocator, requested size/alignment checked above.
    let storage = unsafe { early_alloc_align(bytes, align_of::<u64>()) }.cast::<MemRegion>();
    if storage.is_null() { return Err(Error::Allocation); }
    // SAFETY: the allocation is exclusive; all-zero is a valid MemRegion. No
    // reference to uninitialized integer fields is formed.
    unsafe { ptr::write_bytes(storage, 0, count); }
    // SAFETY: initialized, aligned, exclusive storage, live for the whole boot.
    Ok(unsafe { core::slice::from_raw_parts_mut(storage, count) })
}

pub(crate) struct Fdt;

impl Fdt {
    /// Allocation-free memory discovery before the early allocator exists.
    ///
    /// # Safety
    /// `dtb` satisfies `firmware_tree`'s mapped, immutable extent contract.
    pub(crate) unsafe fn early_memory(dtb: *const c_void) -> Result<Option<Region>, Error> {
        // SAFETY: forwarded firmware mapping contract; no allocation needed.
        unsafe { firmware_tree(dtb) }.and_then(wire::first_memory)
    }

    /// The checked source range lets boot setup exclude the firmware blob from
    /// the early allocator before any allocation can overwrite its bytes.
    ///
    /// # Safety
    /// `dtb` satisfies `firmware_tree`'s mapped, immutable extent contract.
    pub(crate) unsafe fn source_extent(dtb: *const c_void) -> Result<core::ops::Range<u64>, Error> {
        // SAFETY: caller provides the mapped immutable firmware bytes.
        let tree = unsafe { firmware_tree(dtb)? };
        let start = dtb as u64;
        let end = start.checked_add(tree.bytes().len() as u64).ok_or(Error::Bounds)?;
        Ok(start..end)
    }

    /// # Safety
    /// Run once on the boot hart after early allocator initialization, before
    /// concurrent readers/interrupts. `dtb` satisfies `firmware_tree` and lies
    /// outside the early allocator's allocation area.
    pub(crate) unsafe fn init(dtb: *const c_void) -> Result<(), Error> {
        crate::kprintln!("fdt: checking DTB at {}", crate::printf::Ptr(dtb as u64));
        // SAFETY: caller establishes the firmware extent and boot ownership.
        unsafe { Self::initialize(dtb) }
    }

    /// # Safety
    /// Same boot/mapping/allocation contracts as `init`.
    unsafe fn initialize(dtb: *const c_void) -> Result<(), Error> {
        // SAFETY: boot mapping and exclusive allocator are guaranteed by caller.
        let tree = unsafe { firmware_tree(dtb)? };
        // Validate the used properties and allocation count before copying.
        parse_platform(tree)?;
        let mut count = 0usize;
        visit_reserved(tree, |_| { count = count.checked_add(1).ok_or(Error::Range)?; Ok(()) })?;
        let reserved_count = c_int::try_from(count).map_err(|_| Error::Range)?;
        // SAFETY: exclusive boot-time allocation, source DTB disjoint from arena.
        let tree = unsafe { own_tree(tree)? };
        let mut parsed = parse_platform(tree)?;
        // SAFETY: fresh initialized allocation exclusively owned until publish.
        let reserved = unsafe { reserved_storage(count)? };
        let mut slots = reserved.iter_mut();
        visit_reserved(tree, |region| {
            let slot = slots.next().ok_or(Error::Bounds)?;
            *slot = MemRegion { base: region.base, size: region.size };
            Ok(())
        })?;
        parsed.info.reserved = reserved.as_mut_ptr();
        if reserved.is_empty() { parsed.info.reserved = ptr::null_mut(); }
        parsed.info.reserved_count = reserved_count;
        crate::kprintln!("fdt: using DTB at {} (size {} bytes)", crate::printf::Ptr(dtb as u64), tree.bytes().len());
        crate::kprintln!("fdt: parsed {} nodes", tree.node_property_count());
        print_platform(&parsed, reserved);
        // SAFETY: sole boot-time publication, completed before any consumers;
        // all pointer fields refer to permanent initialized early storage.
        unsafe {
            platform = parsed.info;
            PCIE_CONFIG_BASE = parsed.pcie_config_base;
            FDT_BLOB = Some(tree);
        }
        Ok(())
    }

    /// # Safety
    /// Run once on the boot hart after initialization and before any consumers
    /// run concurrently. All records were checked before being published.
    pub(crate) unsafe fn fdt_apply_platform_config() {
        // SAFETY: exclusive boot configuration; this is the only global-write
        // boundary. Parsing, selection, names, and counts are already checked.
        unsafe {
            let info = platform;
            if info.mem_count > 0 && info.mem[0].size > 0 {
                crate::start_kernel::__physical_memory_start = info.mem[0].base;
                crate::start_kernel::__physical_memory_end = info.mem[0].base + info.mem[0].size;
                crate::start_kernel::__physical_total_pages = info.mem[0].size >> 12;
            }
            if info.uart_base != 0 {
                crate::uart::__uart0_mmio_base = info.uart_base;
                crate::uart::__uart0_irqno = info.uart_irq as u64;
                crate::uart::__uart0_clock = info.uart_clock;
                crate::uart::__uart0_baud = info.uart_baud;
                crate::uart::__uart0_reg_shift = info.uart_reg_shift;
                crate::uart::__uart0_reg_io_width = info.uart_reg_io_width;
            }
            if info.plic_base != 0 { crate::irq::plic::__plic_mmio_base = info.plic_base; }
            if let Some(base) = PCIE_CONFIG_BASE { __pcie_ecam_mmio_base = base; }
            if info.has_pcie != 0 && __pcie_ecam_mmio_base == 0 && info.pcie_reg_count > 0 {
                __pcie_ecam_mmio_base = info.pcie_reg[0].base;
            }
            if info.has_virtio != 0 {
                for index in 0..(info.virtio_count as usize).min(N_VIRTIO as usize) {
                    __virtio_mmio_base[index] = info.virtio_base[index];
                    __virtio_irqno[index] = info.virtio_irq[index] as u64;
                }
            }
            if info.timebase_freq != 0 { crate::timer::timer_core::__jiff_ticks = info.timebase_freq / 1000; }
        }
    }

    /// # Safety
    /// The firmware pointer must satisfy `firmware_tree`'s extent contract.
    #[allow(dead_code)]
    pub(crate) unsafe fn fdt_dump(dtb: *const c_void) {
        // SAFETY: caller provides the mapped immutable firmware bytes.
        match unsafe { firmware_tree(dtb) } {
            Ok(tree) => { crate::kprintln!("FDT: {} bytes, {} nodes/properties", tree.bytes().len(), tree.node_property_count()); print_node(tree.root(), 0); }
            Err(error) => crate::kprintln!("fdt_dump: invalid DTB: {:?}", error),
        }
    }

    /// # Safety
    /// Initialization must have finished; no concurrent reinitialization.
    #[allow(dead_code)]
    pub(crate) unsafe fn fdt_walk(_dtb: *const c_void) {
        // SAFETY: snapshot of the once-published immutable borrowed view.
        if let Some(tree) = unsafe { FDT_BLOB } { print_node(tree.root(), 0); }
    }
}

fn print_node(node: Node<'_>, depth: usize) {
    crate::kprintln!("{:width$}{} {{", "", node.name().to_str().unwrap_or("<non-UTF8>"), width = depth * 2);
    for prop in node.properties() {
        crate::kprintln!("{:width$}{}: {:x?}", "", prop.name().to_str().unwrap_or("<non-UTF8>"), prop.bytes(), width = (depth + 1) * 2);
    }
    for child in node.ordered_children() { print_node(child, depth + 1); }
    crate::kprintln!("{:width$}}}", "", width = depth * 2);
}

fn print_platform(parsed: &ParsedPlatform<'_>, reserved: &[MemRegion]) {
    let info = &parsed.info;
    if info.uart_base != 0 {
        crate::kprintln!("fdt: found UART at 0x{:x}, IRQ {}, clock {} Hz, baud {}, reg-shift {}, io-width {}",
            info.uart_base, info.uart_irq, info.uart_clock, info.uart_baud, info.uart_reg_shift, info.uart_reg_io_width);
    }
    for (index, emac) in info.emac[..info.emac_count as usize].iter().enumerate() {
        crate::kprintln!("fdt: found EMAC{} at 0x{:x} size 0x{:x} IRQ {} gpio {} tx-phase {} rx-phase {}",
            index, emac.base, emac.size, emac.irq, emac.reset_gpio, emac.tx_phase, emac.rx_phase);
    }
    for (index, sdhci) in info.sdhci[..info.sdhci_count as usize].iter().enumerate() {
        crate::kprintln!("fdt: found SDHCI{} at 0x{:x} size 0x{:x} IRQ {} bus-width {} apmu 0x{:x}+0x{:x} apbc 0x{:x} {}{}",
            index, sdhci.base, sdhci.size, sdhci.irq, sdhci.bus_width, sdhci.apmu_base, sdhci.apmu_offset,
            sdhci.apbc_base, if sdhci.is_emmc != 0 { "(eMMC)" } else { "" }, if sdhci.is_sdio != 0 { "(SDIO)" } else { "" });
    }
    crate::kprintln!("fdt: probed platform info:");
    crate::kprintln!("  Memory regions: {} (total {} MB)", info.mem_count, info.total_mem / (1024 * 1024));
    for (index, region) in info.mem[..info.mem_count as usize].iter().enumerate() {
        let (base, size) = (region.base, region.size);
        crate::kprintln!("    [{}] 0x{:x} - 0x{:x} ({} MB)", index, base, base + size, size / (1024 * 1024));
    }
    crate::kprintln!("  Reserved regions: {}", reserved.len());
    for (index, region) in reserved.iter().enumerate() {
        let (base, size) = (region.base, region.size);
        crate::kprintln!("    [{}] 0x{:x} - 0x{:x} ({} KB)", index, base, base + size, size / 1024);
    }
    if info.has_ramdisk != 0 {
        crate::kprintln!("  Ramdisk: 0x{:x} - 0x{:x} ({} KB)", info.ramdisk_base,
            info.ramdisk_base + info.ramdisk_size, info.ramdisk_size / 1024);
    }
    crate::kprint!("  UART: 0x{:x}, IRQ {}", info.uart_base, info.uart_irq);
    if info.uart_clock != 0 || info.uart_baud != 0 {
        crate::kprint!(", clock {} Hz, baud {}", if info.uart_clock != 0 { info.uart_clock } else { 1_843_200 },
            if info.uart_baud != 0 { info.uart_baud } else { 115_200 });
    }
    crate::kprintln!();
    crate::kprintln!("  PLIC: 0x{:x} (size 0x{:x})", info.plic_base, info.plic_size);
    if info.has_pcie != 0 {
        crate::kprintln!("  PCIe regions: {}", info.pcie_reg_count);
        for (index, region) in info.pcie_reg[..info.pcie_reg_count as usize].iter().enumerate() {
            if let Some(name) = parsed.pcie_names[index] {
                crate::kprintln!("    [{}] {}: 0x{:x} (size 0x{:x})", index, name.to_str().unwrap_or("<non-UTF8>"), region.base, region.size);
            } else {
                crate::kprintln!("    [{}] 0x{:x} (size 0x{:x})", index, region.base, region.size);
            }
        }
    }
    crate::kprintln!("  CPUs: {}, timebase: {} Hz", info.ncpu, info.timebase_freq);
    if info.has_virtio != 0 {
        crate::kprintln!("  VirtIO devices: {}", info.virtio_count);
        for index in 0..info.virtio_count as usize {
            crate::kprintln!("    [{}] 0x{:x}, IRQ {}", index, info.virtio_base[index], info.virtio_irq[index]);
        }
    }
}
