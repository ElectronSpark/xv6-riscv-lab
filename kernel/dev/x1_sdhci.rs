//! SpacemiT X1 SDHCI SD/eMMC controller driver -- Rust port of
//! `kernel/dev/x1_sdhci.c` (Phase 2 Wave 25, see
//! `docs/rustify/phase2_plan.md`; the last C in `kernel/dev/`, ported
//! together with `yt8531.rs`/`x1_emac.rs` in the same wave; the most
//! MMIO-heavy file in the tree, 15 `volatile` sites in the C original).
//!
//! Supports up to 3 SDHCI instances on the Orange Pi RV2 (SpacemiT X1
//! SoC): SDH0 (SD card slot, 4-bit), SDH1 (SDIO/WiFi, skipped -- not a
//! block device), SDH2 (eMMC, 8-bit, HS400-capable). Supports SDMA
//! (Simple DMA) for data transfers when the controller advertises the
//! capability; falls back to PIO otherwise.
//!
//! Key data flow: `bio` (file system) -> `blkdev` -> `submit_bio` ->
//! [`sdhci_rw_blocks`](sdhci_read_blocks)/[`sdhci_write_blocks`] ->
//! SDMA/PIO.
//!
//! # QEMU verifiability (Wave 25 charter)
//!
//! [`x1_sdhci_init`] -- the only entry point `start_kernel.c` calls --
//! early-returns when `platform.has_sdhci` is false, which it always is
//! on QEMU's generated device tree (Wave 23 finding: no SDHCI-compatible
//! node in `-machine virt`'s DTB). **Compile-verify + boot-no-regression
//! only**, flagged lower confidence per the plan's §3. Additionally --
//! per `RUST_REWRITE.md`'s Iteration 34 finding, reconfirmed here rather
//! than "fixed": the FDT compat/phandle hash tables `fdt.rs` builds are
//! *always empty* (a preserved C bug -- `fdt_node::fdt_type` is never
//! assigned `FDT_BEGIN_NODE`, so the indexing pass is dead code), which
//! means `platform.sdhci[i].apmu_base`/`apbc_base` can never be resolved
//! via the clock-controller-phandle path even on real hardware unless
//! some other probe path fills them in. This port does not touch that
//! logic -- preserving current (non-)behaviour, not fixing probing, is
//! explicitly this wave's charter. Functional verification of the whole
//! driver needs real Orange-Pi-RV2 hardware with an SD card or eMMC
//! wired up.
//!
//! # Diff-review attestation
//!
//! Every function below was reviewed line-by-line against
//! `kernel/dev/x1_sdhci.c` at port time: register field masks/shifts,
//! command/response handling (including the R2/136-bit response word
//! mapping comment block in [`sdhci_enumerate_sd`]/
//! [`sdhci_enumerate_emmc`]), timeout-loop bounds, SDMA boundary-
//! reprogramming logic, and log message text/argument order all match.
//! C `do { ... } while (cond)` loops (the ACMD41 and CMD1 polling loops)
//! are ported as `loop { ...; if cond_to_exit { break; } }` -- same
//! at-least-once execution and continuation predicate, just restated as
//! "break when done" instead of "continue while not done" (Rust has no
//! post-condition loop with an inverted test built in the same way C's
//! `do-while` reads, but the control-flow shape -- body, then test, then
//! maybe-loop -- is identical).
//!
//! # MMIO ordering
//!
//! `x1_sdhci.c` itself has 7 `__sync_synchronize()` calls: 1 in
//! [`sdhci_aib_enable`], 6 in [`sdhci_apmu_enable`] (one per clock/reset
//! configuration step that touches a register the DMA/clock hardware
//! also reads). Every one is preserved below as
//! `core::sync::atomic::fence(Ordering::SeqCst)` at the exact same call
//! site (this crate's established `__sync_synchronize()` ->
//! `fence(SeqCst)` mapping). Two more `fence(SeqCst)` calls appear in
//! this file's local reimplementation of `dev/bio.h`'s
//! `bio_start_io_acct`/`bio_end_io_acct` (one apiece) -- these mirror
//! that header's own `__atomic_thread_fence(__ATOMIC_SEQ_CST)` calls,
//! not an addition invented by this port; 9 `fence(SeqCst)` call sites
//! total in this file, from two distinct C sources (`x1_sdhci.c`'s 7,
//! `bio.h`'s 2). Every MMIO register access goes through
//! [`sdhci_readb`]/[`sdhci_readw`]/[`sdhci_readl`]/[`sdhci_writeb`]/
//! [`sdhci_writew`]/[`sdhci_writel`] or a direct `read_volatile`/
//! `write_volatile` on an already-dereferenced `apmu_reg`/`aib_reg`
//! pointer, so the compiler can never reorder or elide any of them,
//! matching the C `volatile uint8/16/32 *` semantics 1:1. The C
//! original's own comment about `SDHCI_SOFTWARE_RESET`/
//! `SDHCI_HOST_VERSION` needing byte/half-word-width accesses (not a
//! 4-byte `readl`, to avoid a RISC-V unaligned-MMIO load fault) is
//! preserved exactly -- [`sdhci_reset`] uses [`sdhci_readb`]/
//! [`sdhci_writeb`], never [`sdhci_wait_bit`] (which is `readl`-only),
//! and `sdhci_init_one`'s alive-probe uses [`sdhci_readw`] on
//! `SDHCI_HOST_VERSION` (offset `0xFE`, not 4-byte aligned).
//!
//! `dev/bio.h`'s several `static inline` helpers (`bio_iter_*`,
//! `bio_dir_write`, `bio_start_io_acct`/`bio_end_io_acct`, `bio_endio`,
//! `bio_complete`) have no external linkage in C; reimplemented natively
//! here (this file's only caller of them), matching the established
//! precedent (`kernel/bufcache.rs`'s `bio_await`, `kernel/dev/bio.rs`'s
//! module doc).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{fence, AtomicI32, Ordering};

use crate::bindings::{bio, bio_vec, blkdev_ops_t, blkdev_t, mode_t, mutex_t, page_t, platform_info};
use crate::machine;
use crate::sync::KMutex;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.
    fn printf(fmt: *const c_char, ...) -> c_int;
    // timer/sched_timer.rs.
    safe fn sleep_ms(ms: u64);
    // proc/sched.rs.
    safe fn scheduler_yield();
    // proc/thread.rs.
    safe fn kthread_create(
        name: *const c_char,
        entry: *mut c_void,
        arg1: u64,
        arg2: u64,
        stack_order: c_int,
    ) -> *mut crate::bindings::thread;
    safe fn wakeup(p: *mut crate::bindings::thread);
    // lock/mutex.rs.
    safe fn mutex_init(m: *mut mutex_t, name: *mut c_char);
    // string.rs.
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    // mm/page.rs.
    safe fn __page_to_pa(page: *mut page_t) -> u64;
    // lock/completion.rs.
    safe fn completion_reinit(c: *mut crate::bindings::completion_t);
    safe fn complete_all(c: *mut crate::bindings::completion_t);
    // dev/fdt.rs (Phase 2 Wave 23): boot-probed platform config. Kept
    // `#[no_mangle]` in `dev/fdt.rs` (P3-1D mesh sweep: widely-shared data
    // anchor, see that file's own comment) -- this extern stays valid.
    static mut platform: platform_info;
}
// P3-1D mesh sweep: dev/blkdev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use super::blkdev::blkdev_register;

// ===========================================================================
// Register/bit constants -- redeclared locally from `kernel/inc/dev/
// x1_sdhci.h` per this crate's established convention (see e.g.
// `fdt.rs`'s `FDT_MAGIC` et al.); only the subset this file actually
// uses (matching the C original's own usage, not the header's full set).
// ===========================================================================

// -- APMU clock/reset control bits (for SDH ctrl registers) --
const SDH_APMU_AXI_RST: u32 = 1 << 0;
const SDH_APMU_RST: u32 = 1 << 1;
const SDH_APMU_AXI_CLK_EN: u32 = 1 << 3;
const SDH_APMU_IO_CLK_EN: u32 = 1 << 4;
const SDH_APMU_MUX_SHIFT: u32 = 5;
const SDH_APMU_MUX_MASK: u32 = 0x7 << 5;
const SDH_APMU_DIV_SHIFT: u32 = 8;
const SDH_APMU_DIV_MASK: u32 = 0x7 << 8;
const SDH_APMU_CLK_FC: u32 = 1 << 11;
const SDH_MUX_PLL1_D6_409M: u32 = 0;

// -- AIB clock register offset within APBC region (required by SDH0) --
const APBC_AIB_CLK_RST: u32 = 0x3C;
const APBC_AIB_CLK_EN: u32 = 0x3;

// -- Standard SDHCI register offsets (SD Host Controller Spec v3.0) --
const SDHCI_DMA_ADDRESS: u32 = 0x00;
const SDHCI_BLOCK_SIZE: u32 = 0x04;
#[inline(always)]
const fn sdhci_make_blksz(dma_boundary: u16, blksz: u16) -> u16 {
    (dma_boundary << 12) | (blksz & 0x0FFF)
}
const SDHCI_BLOCK_COUNT: u32 = 0x06;
const SDHCI_ARGUMENT: u32 = 0x08;
const SDHCI_TRANSFER_MODE: u32 = 0x0C;
const SDHCI_TRNS_DMA: u16 = 1 << 0;
const SDHCI_TRNS_BLK_CNT_EN: u16 = 1 << 1;
const SDHCI_TRNS_AUTO_CMD12: u16 = 1 << 2;
const SDHCI_TRNS_READ: u16 = 1 << 4;
const SDHCI_TRNS_MULTI: u16 = 1 << 5;
const SDHCI_COMMAND: u32 = 0x0E;
const SDHCI_CMD_RESP_NONE: u16 = 0x00;
const SDHCI_CMD_RESP_136: u16 = 0x01;
const SDHCI_CMD_RESP_48: u16 = 0x02;
const SDHCI_CMD_RESP_48_BUSY: u16 = 0x03;
const SDHCI_CMD_CRC: u16 = 1 << 3;
const SDHCI_CMD_INDEX: u16 = 1 << 4;
const SDHCI_CMD_DATA: u16 = 1 << 5;
#[inline(always)]
const fn sdhci_make_cmd(idx: u32, flags: u16) -> u16 {
    ((idx as u16) << 8) | flags
}
const SDHCI_RESPONSE: u32 = 0x10;
const SDHCI_BUFFER: u32 = 0x20;
const SDHCI_PRESENT_STATE: u32 = 0x24;
const SDHCI_CMD_INHIBIT: u32 = 1 << 0;
const SDHCI_DATA_INHIBIT: u32 = 1 << 1;
const SDHCI_CARD_INSERTED: u32 = 1 << 16;
const SDHCI_HOST_CONTROL: u32 = 0x28;
const SDHCI_CTRL_4BITBUS: u8 = 1 << 1;
const SDHCI_CTRL_DMA_MASK: u8 = 0x18;
const SDHCI_CTRL_SDMA: u8 = 0x00;
const SDHCI_CTRL_8BITBUS: u8 = 1 << 5;
const SDHCI_POWER_CONTROL: u32 = 0x29;
const SDHCI_POWER_ON: u8 = 1 << 0;
const SDHCI_POWER_330: u8 = 0x0E;
const SDHCI_CLOCK_CONTROL: u32 = 0x2C;
const SDHCI_DIVIDER_SHIFT: u32 = 8;
const SDHCI_DIVIDER_HI_SHIFT: u32 = 6;
const SDHCI_DIV_MASK: u32 = 0xFF;
const SDHCI_CLOCK_INT_EN: u16 = 1 << 0;
const SDHCI_CLOCK_INT_STABLE: u16 = 1 << 1;
const SDHCI_CLOCK_CARD_EN: u16 = 1 << 2;
const SDHCI_TIMEOUT_CONTROL: u32 = 0x2E;
const SDHCI_TIMEOUT_DEFAULT: u8 = 0x0E;
const SDHCI_SOFTWARE_RESET: u32 = 0x2F;
const SDHCI_RESET_ALL: u8 = 0x01;
const SDHCI_RESET_CMD: u8 = 0x02;
const SDHCI_RESET_DATA: u8 = 0x04;
const SDHCI_INT_STATUS: u32 = 0x30;
const SDHCI_ERR_INT_STATUS: u32 = 0x32;
const SDHCI_INT_ENABLE: u32 = 0x34;
const SDHCI_ERR_INT_ENABLE: u32 = 0x36;
const SDHCI_SIGNAL_ENABLE: u32 = 0x38;
const SDHCI_ERR_SIGNAL_ENABLE: u32 = 0x3A;
const SDHCI_INT_RESPONSE: u16 = 1 << 0;
const SDHCI_INT_DATA_END: u16 = 1 << 1;
const SDHCI_INT_DMA_END: u16 = 1 << 3;
const SDHCI_INT_SPACE_AVAIL: u16 = 1 << 4;
const SDHCI_INT_DATA_AVAIL: u16 = 1 << 5;
const SDHCI_INT_ERROR: u16 = 1 << 15;
const SDHCI_ERR_ALL: u16 = 0x03FF;
const SDHCI_INT_ALL_MASK: u16 = 0xFFFF;
const SDHCI_HOST_VERSION: u32 = 0xFE;
const SDHCI_CAPABILITIES: u32 = 0x40;
const SDHCI_CAP_BASE_CLK_MASK: u32 = 0xFF;
const SDHCI_CAP_BASE_CLK_SHIFT: u32 = 8;
const SDHCI_CAP_SDMA: u32 = 1 << 22;

// -- KY X1 vendor-specific register extensions (offset 0x100+) --
const SDHC_TX_CFG_REG: u32 = 0x11C;
const TX_INT_CLK_SEL: u32 = 0x40000000;
const SDHC_MMC_CTRL_REG: u32 = 0x114;
const MMC_CARD_MODE: u32 = 0x1000;

// -- SD/MMC command indices --
const MMC_GO_IDLE_STATE: u32 = 0;
const MMC_SEND_OP_COND: u32 = 1;
const MMC_ALL_SEND_CID: u32 = 2;
const MMC_SET_RELATIVE_ADDR: u32 = 3;
const SD_SWITCH_FUNC: u32 = 6;
const MMC_SELECT_CARD: u32 = 7;
const MMC_SEND_EXT_CSD: u32 = 8;
const SD_SEND_IF_COND: u32 = 8;
const MMC_SEND_CSD: u32 = 9;
const MMC_SET_BLOCKLEN: u32 = 16;
const MMC_READ_SINGLE_BLOCK: u32 = 17;
const MMC_READ_MULTIPLE_BLOCK: u32 = 18;
const MMC_WRITE_BLOCK: u32 = 24;
const MMC_WRITE_MULTIPLE_BLOCK: u32 = 25;
const SD_APP_OP_COND: u32 = 41;
const SD_APP_SET_BUS_WIDTH: u32 = 6;
const MMC_APP_CMD: u32 = 55;

// -- SD/MMC OCR (Operating Conditions Register) bits --
const MMC_OCR_HCS: u32 = 1 << 30;
const MMC_OCR_BUSY: u32 = 1 << 31;
const MMC_OCR_3V3: u32 = 0xFF8000;

// -- RCA (Relative Card Address) --
const MMC_RCA_DEFAULT: u32 = 0x0001;

// -- Driver constants --
const SDHCI_TIMEOUT_MS: c_int = 1000;
const SDHCI_CLK_TIMEOUT_MS: c_int = 200;
const SDHCI_BLOCK_SIZE_VAL: u32 = 512;
const SDHCI_SDMA_BOUNDARY: u32 = 512 * 1024;

// -- Card type constants (`kernel/dev/x1_sdhci.c`-local) --
const CARD_TYPE_SD: c_int = 0;
const CARD_TYPE_SDHC: c_int = 1;
const CARD_TYPE_EMMC: c_int = 2;

const MAX_SDH_INSTANCES: usize = 3;
const KERNEL_STACK_ORDER: c_int = 2;
const S_IFBLK: u32 = 0o060_000; // kernel/inc/uabi/stat.h -- same local copy as other dev/*.rs files.

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// ===========================================================================
// Per-instance driver state. Mirrors `struct sdhci_softc` field-for-
// field; purely internal, `#[repr(C)]` kept for diffability against the
// C original (see `x1_emac.rs`'s identical rationale).
// ===========================================================================
#[repr(C)]
struct SdhciSoftc {
    regs: *mut u8, // SDHCI MMIO base

    // APMU clock/reset register (per-instance).
    apmu_reg: *mut u32,
    // APMU shared AXI register (always at SDH0 offset 0x54).
    apmu_axi_reg: *mut u32,
    // APBC AIB clock register (required by SDH0).
    aib_reg: *mut u32,

    // Card state.
    rca: u32,               // Relative Card Address
    card_type: c_int,       // 0=SD, 1=SDHC/SDXC, 2=eMMC
    bus_width: c_int,       // 1, 4, or 8
    capacity_blocks: u64,   // total 512-byte sectors

    // Lock (mutex -- DMA/PIO path needs to sleep/yield).
    lock: mutex_t,

    // DMA.
    use_dma: c_int, // 1 if SDMA is available

    // IRQ / instance index (netdev-style; SDHCI itself is polled, not
    // interrupt-driven -- matches the C original, which never registers
    // an IRQ handler for these instances).
    irq: c_int,
    index: c_int,

    // Block device.
    bdev: blkdev_t,
    initialized: c_int, // 1 if card enumerated OK
}

unsafe impl Sync for SdhciSoftc {}

// ---------------------------------------------------------------------------
// Global storage. `SyncCell<T>` redefined locally per this crate's
// established convention (see `x1_emac.rs`'s identical copy).
// ---------------------------------------------------------------------------
#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get()
    }
}

static SDHCI_SC: SyncCell<[MaybeUninit<SdhciSoftc>; MAX_SDH_INSTANCES]> =
    SyncCell(UnsafeCell::new([const { MaybeUninit::uninit() }; MAX_SDH_INSTANCES]));

/// # Safety
/// `idx` must be `< MAX_SDH_INSTANCES`.
#[inline(always)]
unsafe fn sdhci_sc(idx: usize) -> *mut SdhciSoftc {
    // SAFETY: caller contract.
    unsafe { (SDHCI_SC.get() as *mut SdhciSoftc).add(idx) }
}

/// `static int sdhci_count` -- number of successfully initialised
/// instances. Only ever incremented (`__ATOMIC_SEQ_CST`); no reader
/// exists in this file, matching the C (dead telemetry, preserved as-is).
static SDHCI_COUNT: AtomicI32 = AtomicI32::new(0);

// ===========================================================================
// Low-level MMIO helpers. Mirrors `sdhci_read{b,w,l}`/`sdhci_write{b,w,l}`.
// ===========================================================================

/// # Safety
/// `sc` must be live; `(*sc).regs + off` must be valid MMIO space of the
/// accessed width.
#[inline(always)]
unsafe fn sdhci_readb(sc: *mut SdhciSoftc, off: u32) -> u8 {
    // SAFETY: caller contract.
    unsafe { ptr::read_volatile((*sc).regs.add(off as usize)) }
}
/// # Safety
/// Same as [`sdhci_readb`].
#[inline(always)]
unsafe fn sdhci_readw(sc: *mut SdhciSoftc, off: u32) -> u16 {
    // SAFETY: caller contract.
    unsafe { ptr::read_volatile((*sc).regs.add(off as usize) as *mut u16) }
}
/// # Safety
/// Same as [`sdhci_readb`].
#[inline(always)]
unsafe fn sdhci_readl(sc: *mut SdhciSoftc, off: u32) -> u32 {
    // SAFETY: caller contract.
    unsafe { ptr::read_volatile((*sc).regs.add(off as usize) as *mut u32) }
}
/// # Safety
/// Same as [`sdhci_readb`].
#[inline(always)]
unsafe fn sdhci_writeb(sc: *mut SdhciSoftc, off: u32, val: u8) {
    // SAFETY: caller contract.
    unsafe { ptr::write_volatile((*sc).regs.add(off as usize), val) };
}
/// # Safety
/// Same as [`sdhci_readb`].
#[inline(always)]
unsafe fn sdhci_writew(sc: *mut SdhciSoftc, off: u32, val: u16) {
    // SAFETY: caller contract.
    unsafe { ptr::write_volatile((*sc).regs.add(off as usize) as *mut u16, val) };
}
/// # Safety
/// Same as [`sdhci_readb`].
#[inline(always)]
unsafe fn sdhci_writel(sc: *mut SdhciSoftc, off: u32, val: u32) {
    // SAFETY: caller contract.
    unsafe { ptr::write_volatile((*sc).regs.add(off as usize) as *mut u32, val) };
}

// ===========================================================================
// DMA cache maintenance (Zicbom `cbo.clean`/`cbo.inval`). Local copy,
// same rationale as `x1_emac.rs`'s identical block.
// ===========================================================================
const CBOM_BLOCK_SIZE: usize = 64;

/// # Safety
/// See `x1_emac.rs`'s [`cbo_clean`](super::x1_emac) doc (identical
/// instruction, identical contract).
#[inline(always)]
unsafe fn cbo_clean(addr: usize) {
    unsafe {
        core::arch::asm!(".insn i 0x0F, 2, x0, {0}, 1", in(reg) addr);
    }
}
/// # Safety
/// Same as [`cbo_clean`].
#[inline(always)]
unsafe fn cbo_inval(addr: usize) {
    unsafe {
        core::arch::asm!(".insn i 0x0F, 2, x0, {0}, 0", in(reg) addr);
    }
}
/// # Safety
/// `addr .. addr + size` must be valid to read/write (rounded down to
/// `CBOM_BLOCK_SIZE`, matching the C original -- see `x1_emac.rs`'s
/// identical helper).
unsafe fn dma_cache_clean(addr: *mut c_void, size: usize) {
    let mut start = (addr as usize) & !(CBOM_BLOCK_SIZE - 1);
    let end = (addr as usize) + size;
    while start < end {
        // SAFETY: caller contract.
        unsafe { cbo_clean(start) };
        start += CBOM_BLOCK_SIZE;
    }
}
/// # Safety
/// Same as [`dma_cache_clean`]; dirty data in the touched range is lost.
unsafe fn dma_cache_inval(addr: *mut c_void, size: usize) {
    let mut start = (addr as usize) & !(CBOM_BLOCK_SIZE - 1);
    let end = (addr as usize) + size;
    while start < end {
        // SAFETY: caller contract.
        unsafe { cbo_inval(start) };
        start += CBOM_BLOCK_SIZE;
    }
}

// ===========================================================================
// APMU clock / reset configuration.
// ===========================================================================

/// Wait for Frequency Change (FC) bit to self-clear.
///
/// # Safety
/// `reg` must be a live MMIO register pointer.
unsafe fn sdhci_apmu_wait_fc(reg: *mut u32, timeout_us: c_int) -> c_int {
    let deadline = machine::read_time() + machine::tick_s() * timeout_us as u64 / 1_000_000;
    while machine::read_time() < deadline {
        // SAFETY: caller contract.
        if unsafe { ptr::read_volatile(reg) } & SDH_APMU_CLK_FC == 0 {
            return 0;
        }
        scheduler_yield();
    }
    -1
}

/// Enable the AIB clock via APBC (required by SDH0). APBC base and AIB
/// offset come from the clock-controller's device tree `reg` property.
/// Bits `[1:0] = 0x3` enable the clock.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_aib_enable(sc: *mut SdhciSoftc) {
    // SAFETY: caller contract.
    let aib_reg = unsafe { (*sc).aib_reg };
    if aib_reg.is_null() {
        return;
    }
    // SAFETY: `aib_reg` non-null, live MMIO per caller contract.
    let mut val = unsafe { ptr::read_volatile(aib_reg) };
    val |= APBC_AIB_CLK_EN;
    // SAFETY: see above.
    unsafe { ptr::write_volatile(aib_reg, val) };
    fence(Ordering::SeqCst);
}

/// Enable clocks and deassert reset for an SDHCI instance. Must be
/// called before any SDHCI register access.
///
/// Preserves the MUX/DIV clock source configuration U-Boot/firmware
/// already set up (it booted from SD/eMMC, so the clocks must already be
/// configured) -- only ensures the gate and reset bits are asserted;
/// only reconfigures MUX/DIV if the IO clock is off AND both fields are
/// at reset defaults.
///
/// # Safety
/// `sc` must point to a live, exclusively-owned (not-yet-published)
/// `SdhciSoftc`.
unsafe fn sdhci_apmu_enable(sc: *mut SdhciSoftc) {
    // SAFETY: caller contract.
    let apmu_reg = unsafe { (*sc).apmu_reg };
    if apmu_reg.is_null() {
        // SAFETY: format string matches its one argument.
        unsafe {
            printf(c"x1_sdhci%d: no APMU register, skipping clock enable\n".as_ptr(), (*sc).index)
        };
        return;
    }

    // Step 1: Enable AIB clock (needed for SDH bus fabric).
    // SAFETY: caller contract.
    unsafe { sdhci_aib_enable(sc) };

    // Read current APMU register -- U-Boot should have configured MUX/DIV.
    // SAFETY: `apmu_reg` non-null, live per caller contract.
    let mut val = unsafe { ptr::read_volatile(apmu_reg) };
    // SAFETY: format string matches its two arguments.
    unsafe { printf(c"x1_sdhci%d: APMU initial value = 0x%x\n".as_ptr(), (*sc).index, val) };

    // Configure clock source if not already set up by firmware. Only
    // reconfigure if IO clock is off AND MUX/DIV are at reset defaults.
    if (val & SDH_APMU_IO_CLK_EN == 0) || ((val & SDH_APMU_MUX_MASK) == 0 && (val & SDH_APMU_DIV_MASK) == 0) {
        // SAFETY: caller contract.
        unsafe {
            printf(c"x1_sdhci%d: configuring clock (MUX=0 PLL1_D6, DIV=1)\n".as_ptr(), (*sc).index)
        };
        val &= !(SDH_APMU_MUX_MASK | SDH_APMU_DIV_MASK);
        val |= SDH_MUX_PLL1_D6_409M << SDH_APMU_MUX_SHIFT;
        val |= 1 << SDH_APMU_DIV_SHIFT;
        // SAFETY: `apmu_reg` live.
        unsafe { ptr::write_volatile(apmu_reg, val) };
        fence(Ordering::SeqCst);

        // Trigger frequency change and wait for self-clear.
        val |= SDH_APMU_CLK_FC;
        // SAFETY: `apmu_reg` live.
        unsafe { ptr::write_volatile(apmu_reg, val) };
        fence(Ordering::SeqCst);
        // SAFETY: `apmu_reg` live.
        if unsafe { sdhci_apmu_wait_fc(apmu_reg, 20000) } < 0 {
            // SAFETY: format string matches its one argument.
            unsafe { printf(c"x1_sdhci%d: APMU FC timeout\n".as_ptr(), (*sc).index) };
        }
        // SAFETY: `apmu_reg` live.
        val = unsafe { ptr::read_volatile(apmu_reg) };
    }

    // Step 2: Enable shared AXI bus clock (via SDH0 register).
    // SAFETY: caller contract.
    let apmu_axi_reg = unsafe { (*sc).apmu_axi_reg };
    if !apmu_axi_reg.is_null() {
        // SAFETY: `apmu_axi_reg` non-null, live per caller contract.
        let mut axi = unsafe { ptr::read_volatile(apmu_axi_reg) };
        if axi & SDH_APMU_AXI_CLK_EN == 0 {
            axi |= SDH_APMU_AXI_CLK_EN;
            // SAFETY: see above.
            unsafe { ptr::write_volatile(apmu_axi_reg, axi) };
            fence(Ordering::SeqCst);
        }
    }

    // Step 3: Enable per-instance AXI + IO clock gates.
    // SAFETY: `apmu_reg` live.
    val = unsafe { ptr::read_volatile(apmu_reg) };
    if val & SDH_APMU_AXI_CLK_EN == 0 {
        val |= SDH_APMU_AXI_CLK_EN;
    }
    if val & SDH_APMU_IO_CLK_EN == 0 {
        val |= SDH_APMU_IO_CLK_EN;
    }
    // SAFETY: `apmu_reg` live.
    unsafe { ptr::write_volatile(apmu_reg, val) };
    fence(Ordering::SeqCst);

    // Step 4: Deassert shared AXI bus reset (via SDH0 register).
    if !apmu_axi_reg.is_null() {
        // SAFETY: `apmu_axi_reg` non-null, live.
        let mut axi = unsafe { ptr::read_volatile(apmu_axi_reg) };
        if axi & SDH_APMU_AXI_RST == 0 {
            axi |= SDH_APMU_AXI_RST;
            // SAFETY: see above.
            unsafe { ptr::write_volatile(apmu_axi_reg, axi) };
            fence(Ordering::SeqCst);
        }
    }

    // Step 5: Deassert per-instance AXI + function resets.
    // SAFETY: `apmu_reg` live.
    val = unsafe { ptr::read_volatile(apmu_reg) };
    if val & SDH_APMU_AXI_RST == 0 {
        val |= SDH_APMU_AXI_RST;
    }
    if val & SDH_APMU_RST == 0 {
        val |= SDH_APMU_RST;
    }
    // SAFETY: `apmu_reg` live.
    unsafe { ptr::write_volatile(apmu_reg, val) };
    fence(Ordering::SeqCst);

    sleep_ms(1); // let clocks and reset stabilize

    // SAFETY: caller contract; `apmu_reg`/`apmu_axi_reg` live (or null,
    // handled by the `0` fallback matching the C ternary).
    unsafe {
        printf(
            c"x1_sdhci%d: APMU enabled (reg=0x%x, axi=0x%x)\n".as_ptr(),
            (*sc).index,
            ptr::read_volatile(apmu_reg),
            if apmu_axi_reg.is_null() { 0 } else { ptr::read_volatile(apmu_axi_reg) },
        )
    };
}

// ===========================================================================
// SDHCI controller reset & init.
// ===========================================================================

/// Wait for a register bit to be set or cleared.
///
/// # Safety
/// `sc` must be live; `reg` a valid 4-byte-aligned register offset
/// (this helper always uses `sdhci_readl`).
unsafe fn sdhci_wait_bit(sc: *mut SdhciSoftc, reg: u32, mask: u32, set: bool, timeout_ms: c_int) -> c_int {
    let deadline = machine::read_time() + machine::tick_s() * timeout_ms as u64 / 1000;
    while machine::read_time() < deadline {
        // SAFETY: caller contract.
        let val = unsafe { sdhci_readl(sc, reg) };
        if set {
            if val & mask != 0 {
                return 0;
            }
        } else if val & mask == 0 {
            return 0;
        }
        scheduler_yield();
    }
    -1
}

/// Software reset. `mask` is one of `SDHCI_RESET_{ALL,CMD,DATA}`.
///
/// `SDHCI_SOFTWARE_RESET` is an 8-bit register at offset `0x2F` (not
/// 4-byte aligned), so this uses [`sdhci_readb`] -- not [`sdhci_wait_bit`]
/// (`readl`-only) -- to avoid a load access fault on RISC-V MMIO.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_reset(sc: *mut SdhciSoftc, mask: u8) -> c_int {
    // SAFETY: caller contract.
    unsafe { sdhci_writeb(sc, SDHCI_SOFTWARE_RESET, mask) };

    let deadline = machine::read_time() + machine::tick_s() * 100 / 1000;
    while machine::read_time() < deadline {
        // SAFETY: caller contract.
        if unsafe { sdhci_readb(sc, SDHCI_SOFTWARE_RESET) } & mask == 0 {
            return 0;
        }
        scheduler_yield();
    }
    // SAFETY: format string matches its two arguments.
    unsafe { printf(c"x1_sdhci%d: reset timeout (mask=0x%x)\n".as_ptr(), (*sc).index, mask as c_int) };
    -1
}

/// Set SD clock to a target frequency. Uses the 10-bit divided clock
/// mode from SDHCI spec v3.0.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_set_clock(sc: *mut SdhciSoftc, target_hz: u32) {
    // Stop clock.
    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_CLOCK_CONTROL, 0) };

    if target_hz == 0 {
        return;
    }

    // Read base clock from capabilities.
    // SAFETY: caller contract.
    let caps = unsafe { sdhci_readl(sc, SDHCI_CAPABILITIES) };
    let mut base_clk_mhz = (caps >> SDHCI_CAP_BASE_CLK_SHIFT) & SDHCI_CAP_BASE_CLK_MASK;
    if base_clk_mhz == 0 {
        base_clk_mhz = 200; // fallback, typical for X1
    }

    let base_clk_hz = base_clk_mhz * 1_000_000;

    // Calculate divisor: find smallest divisor that gives <= target_hz.
    let div: u32 = if base_clk_hz <= target_hz {
        0 // no division needed
    } else {
        let mut d = 1;
        while d < 2046 {
            if (base_clk_hz / (2 * d)) <= target_hz {
                break;
            }
            d += 1;
        }
        d
    };

    // Set divisor and enable internal clock.
    let mut clk: u16 =
        (((div & SDHCI_DIV_MASK) as u16) << SDHCI_DIVIDER_SHIFT) | ((((div >> 8) & 0x3) as u16) << SDHCI_DIVIDER_HI_SHIFT) | SDHCI_CLOCK_INT_EN;
    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_CLOCK_CONTROL, clk) };

    // Wait for internal clock stable.
    // SAFETY: caller contract.
    if unsafe { sdhci_wait_bit(sc, SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_STABLE as u32, true, SDHCI_CLK_TIMEOUT_MS) } < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: internal clock not stable\n".as_ptr(), (*sc).index) };
        return;
    }

    // Enable clock to card.
    clk |= SDHCI_CLOCK_CARD_EN;
    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_CLOCK_CONTROL, clk) };

    scheduler_yield(); // brief settling time after clock enable
}

/// Set bus power to 3.3V.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_set_power(sc: *mut SdhciSoftc) {
    // SAFETY: caller contract.
    unsafe { sdhci_writeb(sc, SDHCI_POWER_CONTROL, SDHCI_POWER_330 | SDHCI_POWER_ON) };
    sleep_ms(10);
}

/// Apply vendor-specific (KY X1) quirks after reset. All instances start
/// in internal-clock (bypass PHY) mode for the 400 kHz identification
/// clock -- the PHY PLL cannot lock at such a low frequency, so
/// `PHY_FUNC_EN` must NOT be enabled during init. For eMMC, additionally
/// sets `MMC_CARD_MODE`.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_x1_post_reset(sc: *mut SdhciSoftc) {
    // SAFETY: caller contract.
    let mut reg = unsafe { sdhci_readl(sc, SDHC_TX_CFG_REG) };
    reg |= TX_INT_CLK_SEL;
    // SAFETY: caller contract.
    unsafe { sdhci_writel(sc, SDHC_TX_CFG_REG, reg) };

    // SAFETY: caller contract.
    if unsafe { (*sc).index } >= 2 {
        // eMMC: set MMC card mode.
        // SAFETY: caller contract.
        let mut reg = unsafe { sdhci_readl(sc, SDHC_MMC_CTRL_REG) };
        reg |= MMC_CARD_MODE;
        // SAFETY: caller contract.
        unsafe { sdhci_writel(sc, SDHC_MMC_CTRL_REG, reg) };
    }
}

/// Full controller initialisation: reset, power, clock, vendor quirks.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_hw_init(sc: *mut SdhciSoftc) -> c_int {
    // Full reset.
    // SAFETY: caller contract.
    if unsafe { sdhci_reset(sc, SDHCI_RESET_ALL) } < 0 {
        return -1;
    }

    // Apply X1 vendor quirks.
    // SAFETY: caller contract.
    unsafe { sdhci_x1_post_reset(sc) };

    // Set power to 3.3V.
    // SAFETY: caller contract.
    unsafe { sdhci_set_power(sc) };

    // Set initial clock to 400 kHz (identification mode).
    // SAFETY: caller contract.
    unsafe { sdhci_set_clock(sc, 400_000) };

    // Set timeout.
    // SAFETY: caller contract.
    unsafe { sdhci_writeb(sc, SDHCI_TIMEOUT_CONTROL, SDHCI_TIMEOUT_DEFAULT) };

    // Enable all normal + error interrupt status (for polling).
    // SAFETY: caller contract.
    unsafe {
        sdhci_writew(sc, SDHCI_INT_ENABLE, SDHCI_INT_ALL_MASK);
        sdhci_writew(sc, SDHCI_ERR_INT_ENABLE, SDHCI_ERR_ALL);

        // Disable interrupt signals (we use polling, not IRQ).
        sdhci_writew(sc, SDHCI_SIGNAL_ENABLE, 0);
        sdhci_writew(sc, SDHCI_ERR_SIGNAL_ENABLE, 0);
    }

    // Check SDMA capability and configure host control.
    // SAFETY: caller contract.
    let caps = unsafe { sdhci_readl(sc, SDHCI_CAPABILITIES) };
    if caps & SDHCI_CAP_SDMA != 0 {
        // SAFETY: caller contract.
        unsafe { (*sc).use_dma = 1 };
        // Select SDMA mode in Host Control register (bits [4:3] = 00).
        // SAFETY: caller contract.
        let mut ctrl = unsafe { sdhci_readb(sc, SDHCI_HOST_CONTROL) };
        ctrl &= !SDHCI_CTRL_DMA_MASK;
        ctrl |= SDHCI_CTRL_SDMA;
        // SAFETY: caller contract.
        unsafe {
            sdhci_writeb(sc, SDHCI_HOST_CONTROL, ctrl);
            printf(c"x1_sdhci%d: SDMA enabled\n".as_ptr(), (*sc).index);
        }
    } else {
        // SAFETY: caller contract.
        unsafe {
            (*sc).use_dma = 0;
            printf(c"x1_sdhci%d: no SDMA capability, using PIO\n".as_ptr(), (*sc).index);
        }
    }

    0
}

// ===========================================================================
// Command engine.
// ===========================================================================

/// Send a command and wait for completion (polled), with an explicit
/// timeout. Returns `0` on success, a negative errno on error.
///
/// # Safety
/// `sc` must be live; `resp`, if non-null, must point to at least 4
/// writable `u32`s.
unsafe fn sdhci_send_cmd_to(sc: *mut SdhciSoftc, cmd_idx: u32, arg: u32, flags: u16, resp: *mut u32, timeout_ms: c_int) -> c_int {
    let mut mask = SDHCI_CMD_INHIBIT;
    if flags & SDHCI_CMD_DATA != 0 {
        mask |= SDHCI_DATA_INHIBIT;
    }

    // Wait for command line to be free.
    // SAFETY: caller contract.
    if unsafe { sdhci_wait_bit(sc, SDHCI_PRESENT_STATE, mask, false, timeout_ms) } < 0 {
        // SAFETY: format string matches its two arguments.
        unsafe { printf(c"x1_sdhci%d: CMD%d inhibit timeout\n".as_ptr(), (*sc).index, cmd_idx) };
        return neg(crate::bindings::ETIMEDOUT);
    }

    // SAFETY: caller contract for the whole block below.
    unsafe {
        // Clear pending interrupts.
        sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_ALL_MASK);
        sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);

        // Write argument.
        sdhci_writel(sc, SDHCI_ARGUMENT, arg);

        // Write command register (triggers command issue).
        let cmd_reg = sdhci_make_cmd(cmd_idx, flags);
        sdhci_writew(sc, SDHCI_COMMAND, cmd_reg);
    }

    // Wait for command complete.
    // SAFETY: caller contract.
    if unsafe { sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_RESPONSE as u32, true, timeout_ms) } < 0 {
        // SAFETY: caller contract.
        let err = unsafe { sdhci_readw(sc, SDHCI_ERR_INT_STATUS) };
        if cmd_idx != MMC_GO_IDLE_STATE && cmd_idx != SD_SEND_IF_COND && cmd_idx != MMC_SEND_OP_COND {
            // SAFETY: format string matches its three arguments.
            unsafe {
                printf(
                    c"x1_sdhci%d: CMD%d timeout (err=0x%x)\n".as_ptr(),
                    (*sc).index,
                    cmd_idx,
                    err as c_int,
                )
            };
        }
        // SAFETY: caller contract.
        unsafe { sdhci_reset(sc, SDHCI_RESET_CMD) };
        return neg(crate::bindings::ETIMEDOUT);
    }

    // Check for errors.
    // SAFETY: caller contract.
    let int_status = unsafe { sdhci_readw(sc, SDHCI_INT_STATUS) };
    if int_status & SDHCI_INT_ERROR != 0 {
        // SAFETY: caller contract.
        let err = unsafe { sdhci_readw(sc, SDHCI_ERR_INT_STATUS) };
        // SAFETY: format string matches its four arguments.
        unsafe {
            printf(
                c"x1_sdhci%d: CMD%d error (int=0x%x, err=0x%x)\n".as_ptr(),
                (*sc).index,
                cmd_idx,
                int_status as c_int,
                err as c_int,
            );
            sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);
            sdhci_reset(sc, SDHCI_RESET_CMD);
        }
        return neg(crate::bindings::EIO);
    }

    // Clear command complete.
    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_RESPONSE) };

    // Read response.
    if !resp.is_null() {
        // SAFETY: caller contract; `resp` valid for 4 `u32`s per caller
        // contract when `flags & 0x03 == SDHCI_CMD_RESP_136`, else 1.
        unsafe {
            if flags & 0x03 == SDHCI_CMD_RESP_136 {
                // R2: 128-bit response across four registers.
                *resp.add(0) = sdhci_readl(sc, SDHCI_RESPONSE + 0);
                *resp.add(1) = sdhci_readl(sc, SDHCI_RESPONSE + 4);
                *resp.add(2) = sdhci_readl(sc, SDHCI_RESPONSE + 8);
                *resp.add(3) = sdhci_readl(sc, SDHCI_RESPONSE + 12);
            } else {
                *resp.add(0) = sdhci_readl(sc, SDHCI_RESPONSE);
            }
        }
    }

    0
}

/// Send a command with the default timeout (`SDHCI_TIMEOUT_MS`).
///
/// # Safety
/// Same as [`sdhci_send_cmd_to`].
unsafe fn sdhci_send_cmd(sc: *mut SdhciSoftc, cmd_idx: u32, arg: u32, flags: u16, resp: *mut u32) -> c_int {
    // SAFETY: caller contract.
    unsafe { sdhci_send_cmd_to(sc, cmd_idx, arg, flags, resp, SDHCI_TIMEOUT_MS) }
}

/// Send an application-specific command (ACMD): CMD55 (APP_CMD) first,
/// then the actual command.
///
/// # Safety
/// Same as [`sdhci_send_cmd_to`].
unsafe fn sdhci_send_acmd(sc: *mut SdhciSoftc, cmd_idx: u32, arg: u32, flags: u16, resp: *mut u32) -> c_int {
    // SAFETY: caller contract.
    let rca = unsafe { (*sc).rca };
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_cmd(
            sc,
            MMC_APP_CMD,
            rca << 16,
            SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
            ptr::null_mut(),
        )
    };
    if ret < 0 {
        return ret;
    }
    // SAFETY: caller contract.
    unsafe { sdhci_send_cmd(sc, cmd_idx, arg, flags, resp) }
}

// ===========================================================================
// SDMA data transfer.
// ===========================================================================

/// Wait for an SDMA transfer to complete, handling boundary interrupts.
/// SDMA transfers in fixed-size chunks (512 KB boundary); when the
/// controller reaches a boundary it pauses and sets `SDHCI_INT_DMA_END`
/// -- we re-program `SDHCI_DMA_ADDRESS` with the next boundary-aligned
/// address to resume, until `SDHCI_INT_DATA_END` signals completion.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_sdma_wait(sc: *mut SdhciSoftc, dma_addr_in: u64, is_write: bool) -> c_int {
    let mut dma_addr = dma_addr_in;
    let deadline = machine::read_time() + machine::tick_s() * SDHCI_TIMEOUT_MS as u64 / 1000;

    while machine::read_time() < deadline {
        // SAFETY: caller contract.
        let int_st = unsafe { sdhci_readw(sc, SDHCI_INT_STATUS) };

        // Check for errors.
        if int_st & SDHCI_INT_ERROR != 0 {
            // SAFETY: caller contract.
            let err = unsafe { sdhci_readw(sc, SDHCI_ERR_INT_STATUS) };
            // SAFETY: caller contract; format string matches its four arguments.
            unsafe {
                printf(
                    c"x1_sdhci%d: DMA %s error (int=0x%x, err=0x%x)\n".as_ptr(),
                    (*sc).index,
                    if is_write { c"write".as_ptr() } else { c"read".as_ptr() },
                    int_st as c_int,
                    err as c_int,
                );
                sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);
                sdhci_reset(sc, SDHCI_RESET_DATA);
            }
            return neg(crate::bindings::EIO);
        }

        // Transfer complete -- all data moved.
        if int_st & SDHCI_INT_DATA_END != 0 {
            // SAFETY: caller contract.
            unsafe { sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_END) };
            return 0;
        }

        // SDMA boundary reached -- reprogram address and continue.
        if int_st & SDHCI_INT_DMA_END != 0 {
            // SAFETY: caller contract.
            unsafe { sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DMA_END) };
            // Advance to next boundary.
            dma_addr &= !(SDHCI_SDMA_BOUNDARY as u64 - 1);
            dma_addr += SDHCI_SDMA_BOUNDARY as u64;
            // SAFETY: caller contract.
            unsafe { sdhci_writel(sc, SDHCI_DMA_ADDRESS, dma_addr as u32) };
        }

        scheduler_yield();
    }

    // SAFETY: caller contract; format string matches its two arguments.
    unsafe {
        printf(
            c"x1_sdhci%d: DMA %s timeout\n".as_ptr(),
            (*sc).index,
            if is_write { c"write".as_ptr() } else { c"read".as_ptr() },
        );
        sdhci_reset(sc, SDHCI_RESET_DATA);
    }
    neg(crate::bindings::ETIMEDOUT)
}

// ===========================================================================
// PIO data transfer.
// ===========================================================================

/// Read a single 512-byte block from the card using PIO.
///
/// # Safety
/// `sc` must be live; `buf` must point to `SDHCI_BLOCK_SIZE_VAL` (512)
/// writable bytes, 4-byte aligned.
unsafe fn sdhci_pio_read_block(sc: *mut SdhciSoftc, buf: *mut c_void) -> c_int {
    let p = buf as *mut u32;

    // Wait for data available.
    // SAFETY: caller contract.
    if unsafe { sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL as u32, true, SDHCI_TIMEOUT_MS) } < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: PIO read data timeout\n".as_ptr(), (*sc).index) };
        return neg(crate::bindings::ETIMEDOUT);
    }

    // Clear the data available interrupt.
    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL) };

    // Read 512 / 4 = 128 words from data buffer port.
    for i in 0..(SDHCI_BLOCK_SIZE_VAL / 4) as usize {
        // SAFETY: caller contract; `p` valid for 128 `u32`s.
        unsafe { *p.add(i) = sdhci_readl(sc, SDHCI_BUFFER) };
    }

    0
}

/// Write a single 512-byte block to the card using PIO.
///
/// # Safety
/// `sc` must be live; `buf` must point to `SDHCI_BLOCK_SIZE_VAL` (512)
/// readable bytes, 4-byte aligned.
unsafe fn sdhci_pio_write_block(sc: *mut SdhciSoftc, buf: *const c_void) -> c_int {
    let p = buf as *const u32;

    // Wait for space available.
    // SAFETY: caller contract.
    if unsafe { sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_SPACE_AVAIL as u32, true, SDHCI_TIMEOUT_MS) } < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: PIO write space timeout\n".as_ptr(), (*sc).index) };
        return neg(crate::bindings::ETIMEDOUT);
    }

    // Clear the space available interrupt.
    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_SPACE_AVAIL) };

    // Write 128 words to data buffer port.
    for i in 0..(SDHCI_BLOCK_SIZE_VAL / 4) as usize {
        // SAFETY: caller contract; `p` valid for 128 `u32`s.
        unsafe { sdhci_writel(sc, SDHCI_BUFFER, *p.add(i)) };
    }

    0
}

/// Wait for transfer complete after all blocks transferred.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_wait_xfer_done(sc: *mut SdhciSoftc) -> c_int {
    // SAFETY: caller contract.
    if unsafe { sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_END as u32, true, SDHCI_TIMEOUT_MS) } < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: transfer complete timeout\n".as_ptr(), (*sc).index) };
        return neg(crate::bindings::ETIMEDOUT);
    }

    // Check for errors.
    // SAFETY: caller contract.
    let err = unsafe { sdhci_readw(sc, SDHCI_ERR_INT_STATUS) };
    if err != 0 {
        // SAFETY: caller contract; format string matches its two arguments.
        unsafe {
            printf(c"x1_sdhci%d: transfer error 0x%x\n".as_ptr(), (*sc).index, err as c_int);
            sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);
            sdhci_reset(sc, SDHCI_RESET_DATA);
        }
        return neg(crate::bindings::EIO);
    }

    // Clear transfer complete.
    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_END) };
    0
}

// ===========================================================================
// Block read/write (single + multi-block).
// ===========================================================================

/// Read `nblocks` 512-byte blocks starting at `lba` into `buf`. Uses
/// SDMA if available, otherwise falls back to PIO. `buf` must point to a
/// physically-contiguous buffer (physical address usable as both VA and
/// PA due to identity mapping).
///
/// # Safety
/// `sc` must be live; `buf` must point to `nblocks * 512` writable bytes.
unsafe fn sdhci_read_blocks(sc: *mut SdhciSoftc, lba: u32, nblocks: u32, buf: *mut c_void) -> c_int {
    let total = nblocks * SDHCI_BLOCK_SIZE_VAL;

    // SDHC/SDXC/eMMC uses block addressing; SD uses byte addressing.
    // SAFETY: caller contract.
    let card_addr = if unsafe { (*sc).card_type } == CARD_TYPE_SD { lba * 512 } else { lba };

    // Set block size and count.
    // SAFETY: caller contract.
    unsafe {
        sdhci_writew(sc, SDHCI_BLOCK_SIZE, sdhci_make_blksz(7, SDHCI_BLOCK_SIZE_VAL as u16));
        sdhci_writew(sc, SDHCI_BLOCK_COUNT, nblocks as u16);
    }

    // Transfer mode: read direction, block count enable.
    let mut mode = SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN;
    if nblocks > 1 {
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
    }

    // SAFETY: caller contract.
    let use_dma = unsafe { (*sc).use_dma } != 0;
    if use_dma {
        // Invalidate cache before device writes to memory.
        // SAFETY: caller contract.
        unsafe { dma_cache_inval(buf, total as usize) };

        // Program SDMA system address.
        // SAFETY: caller contract.
        unsafe { sdhci_writel(sc, SDHCI_DMA_ADDRESS, buf as u64 as u32) };
        mode |= SDHCI_TRNS_DMA;
    }

    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_TRANSFER_MODE, mode) };

    // Command.
    let cmd_idx = if nblocks == 1 { MMC_READ_SINGLE_BLOCK } else { MMC_READ_MULTIPLE_BLOCK };
    let cmd_flags = SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA;

    // SAFETY: caller contract.
    let ret = unsafe { sdhci_send_cmd(sc, cmd_idx, card_addr, cmd_flags, ptr::null_mut()) };
    if ret < 0 {
        return ret;
    }

    if use_dma {
        // SAFETY: caller contract.
        let ret = unsafe { sdhci_sdma_wait(sc, buf as u64, false) };
        if ret < 0 {
            return ret;
        }
        // Invalidate again to ensure CPU sees DMA-written data.
        // SAFETY: caller contract.
        unsafe { dma_cache_inval(buf, total as usize) };
        0
    } else {
        // SAFETY: caller contract.
        unsafe { printf(c"sdhci_read_blocks: fallback to bio.\n".as_ptr()) };
        // Read blocks via PIO.
        for i in 0..nblocks {
            // SAFETY: `buf` valid for `total` bytes; `i < nblocks`.
            let ret = unsafe { sdhci_pio_read_block(sc, (buf as *mut u8).add((i * SDHCI_BLOCK_SIZE_VAL) as usize) as *mut c_void) };
            if ret < 0 {
                return ret;
            }
        }
        // SAFETY: caller contract.
        unsafe { sdhci_wait_xfer_done(sc) }
    }
}

/// Write `nblocks` 512-byte blocks starting at `lba` from `buf`. Uses
/// SDMA if available, otherwise falls back to PIO.
///
/// # Safety
/// `sc` must be live; `buf` must point to `nblocks * 512` readable bytes.
unsafe fn sdhci_write_blocks(sc: *mut SdhciSoftc, lba: u32, nblocks: u32, buf: *const c_void) -> c_int {
    let total = nblocks * SDHCI_BLOCK_SIZE_VAL;

    // SAFETY: caller contract.
    let card_addr = if unsafe { (*sc).card_type } == CARD_TYPE_SD { lba * 512 } else { lba };

    // SAFETY: caller contract.
    unsafe {
        sdhci_writew(sc, SDHCI_BLOCK_SIZE, sdhci_make_blksz(7, SDHCI_BLOCK_SIZE_VAL as u16));
        sdhci_writew(sc, SDHCI_BLOCK_COUNT, nblocks as u16);
    }

    // Transfer mode: write direction, block count enable (no
    // SDHCI_TRNS_READ = write).
    let mut mode = SDHCI_TRNS_BLK_CNT_EN;
    if nblocks > 1 {
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
    }

    // SAFETY: caller contract.
    let use_dma = unsafe { (*sc).use_dma } != 0;
    if use_dma {
        // Clean cache so device reads CPU's latest data.
        // SAFETY: caller contract.
        unsafe { dma_cache_clean(buf as *mut c_void, total as usize) };

        // Program SDMA system address.
        // SAFETY: caller contract.
        unsafe { sdhci_writel(sc, SDHCI_DMA_ADDRESS, buf as u64 as u32) };
        mode |= SDHCI_TRNS_DMA;
    }

    // SAFETY: caller contract.
    unsafe { sdhci_writew(sc, SDHCI_TRANSFER_MODE, mode) };

    let cmd_idx = if nblocks == 1 { MMC_WRITE_BLOCK } else { MMC_WRITE_MULTIPLE_BLOCK };
    let cmd_flags = SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA;

    // SAFETY: caller contract.
    let ret = unsafe { sdhci_send_cmd(sc, cmd_idx, card_addr, cmd_flags, ptr::null_mut()) };
    if ret < 0 {
        return ret;
    }

    if use_dma {
        // SAFETY: caller contract.
        unsafe { sdhci_sdma_wait(sc, buf as u64, true) }
    } else {
        // SAFETY: caller contract.
        unsafe { printf(c"sdhci_write_blocks: fallback to bio.\n".as_ptr()) };
        // Write blocks via PIO.
        for i in 0..nblocks {
            // SAFETY: `buf` valid for `total` bytes; `i < nblocks`.
            let ret = unsafe {
                sdhci_pio_write_block(sc, (buf as *const u8).add((i * SDHCI_BLOCK_SIZE_VAL) as usize) as *const c_void)
            };
            if ret < 0 {
                return ret;
            }
        }
        // SAFETY: caller contract.
        unsafe { sdhci_wait_xfer_done(sc) }
    }
}

// ===========================================================================
// SD card enumeration.
// ===========================================================================

/// Check for card presence using the Present State register.
///
/// # Safety
/// `sc` must be live.
unsafe fn sdhci_card_present(sc: *mut SdhciSoftc) -> bool {
    // SAFETY: caller contract.
    let state = unsafe { sdhci_readl(sc, SDHCI_PRESENT_STATE) };
    state & SDHCI_CARD_INSERTED != 0
}

/// Enumerate an SD card: CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9
/// -> CMD7 -> CMD16. Returns `0` on success.
///
/// # Safety
/// `sc` must point to a live, exclusively-owned `SdhciSoftc`.
unsafe fn sdhci_enumerate_sd(sc: *mut SdhciSoftc) -> c_int {
    let mut resp = [0u32; 4];

    // SAFETY: caller contract.
    unsafe {
        (*sc).rca = 0;
        (*sc).card_type = CARD_TYPE_SD;
    }

    // CMD0: Go idle.
    // SAFETY: caller contract.
    unsafe { sdhci_send_cmd(sc, MMC_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE, ptr::null_mut()) };
    sleep_ms(10);

    // CMD8: Send IF Condition (voltage check for SD 2.0+).
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_cmd(sc, SD_SEND_IF_COND, 0x1AA, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, resp.as_mut_ptr())
    };
    if ret < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: CMD8 failed -- legacy SD v1.x card?\n".as_ptr(), (*sc).index) };
        // Might be SD v1.x -- try ACMD41 anyway.
    } else if resp[0] & 0xFF != 0xAA {
        // SAFETY: format string matches its two arguments.
        unsafe { printf(c"x1_sdhci%d: CMD8 bad echo: 0x%x\n".as_ptr(), (*sc).index, resp[0]) };
    }

    // ACMD41: SD_APP_OP_COND -- negotiate operating conditions. Requests
    // 3.3V range and HCS (high capacity support).
    let mut tries = 0;
    loop {
        // SAFETY: caller contract.
        let ret = unsafe { sdhci_send_acmd(sc, SD_APP_OP_COND, MMC_OCR_HCS | MMC_OCR_3V3, SDHCI_CMD_RESP_48, resp.as_mut_ptr()) };
        if ret < 0 {
            // SAFETY: format string matches its one argument.
            unsafe { printf(c"x1_sdhci%d: ACMD41 failed\n".as_ptr(), (*sc).index) };
            return ret;
        }
        tries += 1;
        if tries > 100 {
            // SAFETY: format string matches its one argument.
            unsafe { printf(c"x1_sdhci%d: ACMD41 timeout -- card not ready\n".as_ptr(), (*sc).index) };
            return neg(crate::bindings::ETIMEDOUT);
        }
        sleep_ms(10);
        if resp[0] & MMC_OCR_BUSY != 0 {
            break;
        }
    }

    // Check if card is SDHC/SDXC.
    if resp[0] & MMC_OCR_HCS != 0 {
        // SAFETY: caller contract.
        unsafe { (*sc).card_type = CARD_TYPE_SDHC };
    }

    // SAFETY: caller contract; format string matches its two arguments.
    unsafe {
        printf(
            c"x1_sdhci%d: card type: %s\n".as_ptr(),
            (*sc).index,
            if (*sc).card_type == CARD_TYPE_SDHC { c"SDHC/SDXC".as_ptr() } else { c"SD".as_ptr() },
        )
    };

    // CMD2: ALL_SEND_CID -- get card identification.
    // SAFETY: caller contract.
    let ret = unsafe { sdhci_send_cmd(sc, MMC_ALL_SEND_CID, 0, SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp.as_mut_ptr()) };
    if ret < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: CMD2 failed\n".as_ptr(), (*sc).index) };
        return ret;
    }

    // CMD3: SEND_RELATIVE_ADDR -- get RCA.
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_cmd(sc, MMC_SET_RELATIVE_ADDR, 0, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, resp.as_mut_ptr())
    };
    if ret < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: CMD3 failed\n".as_ptr(), (*sc).index) };
        return ret;
    }
    // SAFETY: caller contract.
    unsafe { (*sc).rca = (resp[0] >> 16) & 0xFFFF };
    // SAFETY: format string matches its two arguments.
    unsafe { printf(c"x1_sdhci%d: RCA = 0x%x\n".as_ptr(), (*sc).index, (*sc).rca) };

    // CMD9: SEND_CSD -- get card-specific data (read capacity etc.).
    // SAFETY: caller contract.
    let rca = unsafe { (*sc).rca };
    // SAFETY: caller contract.
    let ret = unsafe { sdhci_send_cmd(sc, MMC_SEND_CSD, rca << 16, SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp.as_mut_ptr()) };
    if ret < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: CMD9 failed\n".as_ptr(), (*sc).index) };
        return ret;
    }

    // Parse CSD to get capacity.
    //
    // SDHCI R2 response mapping: the controller strips CRC7+end bit, so
    // card CSD bit X maps to SDHCI response bit (X - 8).
    //   resp[0] (offset 0x10) = CSD bits [39:8]
    //   resp[1] (offset 0x14) = CSD bits [71:40]
    //   resp[2] (offset 0x18) = CSD bits [103:72]
    //   resp[3] (offset 0x1C) = CSD bits [127:104]
    // SAFETY: caller contract.
    let capacity_blocks: u64 = if unsafe { (*sc).card_type } == CARD_TYPE_SDHC {
        // CSD v2 (SDHC/SDXC): C_SIZE is CSD bits [69:48] (22 bits) ->
        // SDHCI bits [61:40] -> resp[1] bits [29:8].
        let c_size = (resp[1] >> 8) & 0x3FFFFF;
        (c_size as u64 + 1) * 1024
    } else {
        // CSD v1: READ_BL_LEN = CSD[83:80] -> resp[2] bits [11:8];
        //         C_SIZE      = CSD[73:62] -> resp[2][1:0]:resp[1][31:22];
        //         C_SIZE_MULT = CSD[49:47] -> resp[1] bits [9:7].
        let read_bl_len = (resp[2] >> 8) & 0xF;
        let c_size = ((resp[2] & 0x3) << 10) | ((resp[1] >> 22) & 0x3FF);
        let c_size_mult = (resp[1] >> 7) & 0x7;
        let mult = 1u32 << (c_size_mult + 2);
        let blocknr = (c_size + 1) * mult;
        let block_len = 1u32 << read_bl_len;
        (blocknr as u64) * (block_len as u64) / 512
    };
    // SAFETY: caller contract.
    unsafe { (*sc).capacity_blocks = capacity_blocks };
    // SAFETY: format string matches its three arguments.
    unsafe {
        printf(
            c"x1_sdhci%d: capacity = %ld sectors (%ld MB)\n".as_ptr(),
            (*sc).index,
            capacity_blocks,
            capacity_blocks / 2048,
        )
    };

    // CMD7: SELECT_CARD -- move card to transfer state.
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_cmd(
            sc,
            MMC_SELECT_CARD,
            rca << 16,
            SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
            resp.as_mut_ptr(),
        )
    };
    if ret < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: CMD7 failed\n".as_ptr(), (*sc).index) };
        return ret;
    }

    // CMD16: SET_BLOCKLEN to 512 (for non-SDHC cards).
    // SAFETY: caller contract.
    if unsafe { (*sc).card_type } != CARD_TYPE_SDHC {
        // SAFETY: caller contract.
        let ret = unsafe {
            sdhci_send_cmd(
                sc,
                MMC_SET_BLOCKLEN,
                SDHCI_BLOCK_SIZE_VAL,
                SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                resp.as_mut_ptr(),
            )
        };
        if ret < 0 {
            // SAFETY: format string matches its one argument.
            unsafe { printf(c"x1_sdhci%d: CMD16 failed\n".as_ptr(), (*sc).index) };
            return ret;
        }
    }

    // Switch to 4-bit bus width.
    // SAFETY: caller contract.
    unsafe { (*sc).bus_width = 4 };
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_acmd(sc, SD_APP_SET_BUS_WIDTH, 2, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, ptr::null_mut())
    };
    if ret < 0 {
        // SAFETY: caller contract; format string matches its one argument.
        unsafe {
            printf(c"x1_sdhci%d: ACMD6 (set 4-bit) failed, using 1-bit\n".as_ptr(), (*sc).index);
            (*sc).bus_width = 1;
        }
    } else {
        // Tell the host controller to use 4-bit mode.
        // SAFETY: caller contract.
        let mut ctrl = unsafe { sdhci_readb(sc, SDHCI_HOST_CONTROL) };
        ctrl |= SDHCI_CTRL_4BITBUS;
        // SAFETY: caller contract.
        unsafe { sdhci_writeb(sc, SDHCI_HOST_CONTROL, ctrl) };
    }

    // Increase clock to 25 MHz (SD default speed).
    // SAFETY: caller contract.
    unsafe { sdhci_set_clock(sc, 25_000_000) };

    0
}

/// Enumerate an eMMC device: CMD0 -> CMD1 -> CMD2 -> CMD3 -> CMD9 ->
/// CMD7 -> CMD16. Returns `0` on success.
///
/// # Safety
/// Same as [`sdhci_enumerate_sd`].
unsafe fn sdhci_enumerate_emmc(sc: *mut SdhciSoftc) -> c_int {
    let mut resp = [0u32; 4];

    // SAFETY: caller contract.
    unsafe {
        (*sc).rca = MMC_RCA_DEFAULT;
        (*sc).card_type = CARD_TYPE_EMMC;
    }

    // CMD0: Go idle.
    // SAFETY: caller contract.
    unsafe { sdhci_send_cmd(sc, MMC_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE, ptr::null_mut()) };
    sleep_ms(10);

    // CMD1: SEND_OP_COND -- negotiate operating conditions. Requests
    // sector addressing mode + 3.3V. eMMC may take time to power up, so
    // retry on timeout, with a short 100ms command timeout to avoid long
    // waits.
    let mut tries = 0;
    let mut cmd1_ok = false;
    loop {
        // SAFETY: caller contract.
        let ret = unsafe { sdhci_send_cmd_to(sc, MMC_SEND_OP_COND, 0x40FF_8080, SDHCI_CMD_RESP_48, resp.as_mut_ptr(), 100) };
        if ret == 0 {
            cmd1_ok = true;
            if resp[0] & MMC_OCR_BUSY != 0 {
                break; // card is ready
            }
        }
        // Retry on timeout -- eMMC may still be powering up.
        tries += 1;
        if tries > 30 {
            // SAFETY: caller contract; format string matches its two arguments.
            unsafe {
                printf(
                    c"x1_sdhci%d: CMD1 no response after %d attempts -- eMMC not present or not powered\n".as_ptr(),
                    (*sc).index,
                    tries,
                )
            };
            return neg(crate::bindings::ETIMEDOUT);
        }
        sleep_ms(10);
    }

    if !cmd1_ok {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: CMD1 never succeeded\n".as_ptr(), (*sc).index) };
        return neg(crate::bindings::EIO);
    }

    // SAFETY: caller contract; format string matches its two arguments.
    unsafe { printf(c"x1_sdhci%d: eMMC OCR = 0x%x\n".as_ptr(), (*sc).index, resp[0]) };

    // CMD2: ALL_SEND_CID.
    // SAFETY: caller contract.
    let ret = unsafe { sdhci_send_cmd(sc, MMC_ALL_SEND_CID, 0, SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp.as_mut_ptr()) };
    if ret < 0 {
        return ret;
    }

    // CMD3: SET_RELATIVE_ADDR -- eMMC: we assign the RCA.
    // SAFETY: caller contract.
    let rca = unsafe { (*sc).rca };
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_cmd(sc, MMC_SET_RELATIVE_ADDR, rca << 16, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, resp.as_mut_ptr())
    };
    if ret < 0 {
        return ret;
    }

    // CMD9: SEND_CSD -- get card specific data.
    // SAFETY: caller contract.
    let ret = unsafe { sdhci_send_cmd(sc, MMC_SEND_CSD, rca << 16, SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp.as_mut_ptr()) };
    if ret < 0 {
        return ret;
    }

    // eMMC CSD: fields use the same SDHCI R2 bit mapping as SD. For
    // >2GB eMMC, C_SIZE=0xFFF -> read SEC_COUNT from EXT_CSD.
    // READ_BL_LEN = CSD[83:80] -> resp[2] bits [11:8].
    let read_bl_len = (resp[2] >> 8) & 0xF;
    // C_SIZE = CSD[73:62] -> resp[2][1:0]:resp[1][31:22].
    let c_size = ((resp[2] & 0x3) << 10) | ((resp[1] >> 22) & 0x3FF);
    // C_SIZE_MULT = CSD[49:47] -> resp[1] bits [9:7].
    let c_size_mult = (resp[1] >> 7) & 0x7;
    let mut capacity_blocks: u64 = if c_size == 0xFFF {
        // Capacity is in EXT_CSD -- we'll read it after SELECT.
        0
    } else {
        let mult = 1u32 << (c_size_mult + 2);
        let blocknr = (c_size + 1) * mult;
        let block_len = 1u32 << read_bl_len;
        (blocknr as u64) * (block_len as u64) / 512
    };
    // SAFETY: caller contract.
    unsafe { (*sc).capacity_blocks = capacity_blocks };

    // CMD7: SELECT_CARD.
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_cmd(sc, MMC_SELECT_CARD, rca << 16, SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, resp.as_mut_ptr())
    };
    if ret < 0 {
        return ret;
    }

    // CMD16: SET_BLOCKLEN to 512.
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_cmd(sc, MMC_SET_BLOCKLEN, SDHCI_BLOCK_SIZE_VAL, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, resp.as_mut_ptr())
    };
    if ret < 0 {
        return ret;
    }

    // Read EXT_CSD to get true capacity (CMD8 for eMMC).
    if capacity_blocks == 0 {
        let mut ext_csd = [0u8; 512];
        // SAFETY: caller contract.
        unsafe {
            sdhci_writew(sc, SDHCI_BLOCK_SIZE, sdhci_make_blksz(7, SDHCI_BLOCK_SIZE_VAL as u16));
            sdhci_writew(sc, SDHCI_BLOCK_COUNT, 1);
            sdhci_writew(sc, SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN);
        }

        // SAFETY: caller contract.
        let ret = unsafe {
            sdhci_send_cmd(
                sc,
                MMC_SEND_EXT_CSD,
                0,
                SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA,
                resp.as_mut_ptr(),
            )
        };
        if ret == 0 {
            // SAFETY: caller contract; `ext_csd` a live 512-byte buffer.
            let ret = unsafe { sdhci_pio_read_block(sc, ext_csd.as_mut_ptr() as *mut c_void) };
            if ret == 0 {
                // SAFETY: caller contract.
                unsafe { sdhci_wait_xfer_done(sc) };
                // SEC_COUNT is at EXT_CSD bytes 212-215 (little-endian).
                capacity_blocks = ext_csd[212] as u64
                    | ((ext_csd[213] as u64) << 8)
                    | ((ext_csd[214] as u64) << 16)
                    | ((ext_csd[215] as u64) << 24);
                // SAFETY: caller contract.
                unsafe { (*sc).capacity_blocks = capacity_blocks };
            }
        }
    }

    // SAFETY: format string matches its three arguments.
    unsafe {
        printf(
            c"x1_sdhci%d: eMMC capacity = %ld sectors (%ld MB)\n".as_ptr(),
            (*sc).index,
            capacity_blocks,
            capacity_blocks / 2048,
        )
    };

    // Switch to 8-bit bus width via CMD6 (SWITCH): Access=Write Byte
    // (0x03), Index=183 (BUS_WIDTH), Value=2 (8-bit).
    // SAFETY: caller contract.
    let ret = unsafe {
        sdhci_send_cmd(
            sc,
            SD_SWITCH_FUNC,
            (3 << 24) | (183 << 16) | (2 << 8),
            SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
            resp.as_mut_ptr(),
        )
    };
    if ret == 0 {
        // SAFETY: caller contract.
        let mut ctrl = unsafe { sdhci_readb(sc, SDHCI_HOST_CONTROL) };
        ctrl |= SDHCI_CTRL_8BITBUS;
        // SAFETY: caller contract.
        unsafe {
            sdhci_writeb(sc, SDHCI_HOST_CONTROL, ctrl);
            (*sc).bus_width = 8;
        }
        sleep_ms(1);
    } else {
        // SAFETY: caller contract; format string matches its one argument.
        unsafe {
            (*sc).bus_width = 1;
            printf(c"x1_sdhci%d: 8-bit bus switch failed, using 1-bit\n".as_ptr(), (*sc).index);
        }
    }

    // Increase clock to 26 MHz (eMMC backward-compatible speed).
    // SAFETY: caller contract.
    unsafe { sdhci_set_clock(sc, 26_000_000) };

    0
}

// ===========================================================================
// `dev/bio.h`'s static-inline helpers, reimplemented natively (this
// file's only consumer -- see module doc).
// ===========================================================================

struct BioIter {
    blkno: u64,
    size: u16,
    size_done: u16,
    bvec_idx: i16,
}

const BLK_SIZE_SHIFT: u32 = 9;

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_iter_start(bio_ptr: *mut bio, it: &mut BioIter) {
    // SAFETY: caller contract.
    unsafe {
        it.blkno = (*bio_ptr).blkno;
        it.bvec_idx = 0;
        if (*bio_ptr).vec_length > 0 {
            it.size = (*(*bio_ptr).bvecs.as_ptr()).len;
            it.size_done = 0;
        } else {
            it.size = 0;
            it.size_done = 0;
        }
    }
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_iter_next_seg(bio_ptr: *mut bio, it: &mut BioIter) {
    let bvec_idx = it.bvec_idx + 1;
    // SAFETY: caller contract.
    if bvec_idx > unsafe { (*bio_ptr).vec_length } || bvec_idx < 0 {
        return;
    }
    // SAFETY: caller contract; `bvec_idx` bounds-checked above.
    unsafe {
        let len = (*(*bio_ptr).bvecs.as_ptr().add(bvec_idx as usize)).len;
        it.size -= len;
        it.size_done += len;
        (*bio_ptr).done_size += len;
        it.blkno = (*bio_ptr).blkno + (((*bio_ptr).done_size as u64) >> (BLK_SIZE_SHIFT + (*bio_ptr).block_shift as u32));
        it.bvec_idx = bvec_idx;
    }
}

/// # Safety
/// `bio_ptr` must be live; `bvec` must be a live, writable `bio_vec`.
unsafe fn bio_iter_copy_bvec(bio_ptr: *mut bio, it: &BioIter, bvec: *mut bio_vec) -> bool {
    // SAFETY: caller contract.
    if it.bvec_idx >= unsafe { (*bio_ptr).vec_length } {
        return false;
    }
    // SAFETY: caller contract; `it.bvec_idx` bounds-checked above.
    unsafe { *bvec = *(*bio_ptr).bvecs.as_ptr().add(it.bvec_idx as usize) };
    true
}

/// # Safety
/// `bio_ptr` must be live.
#[inline(always)]
unsafe fn bio_dir_write(bio_ptr: *mut bio) -> bool {
    // SAFETY: caller contract.
    unsafe { (*bio_ptr).__bindgen_anon_1.rw() != 0 }
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_start_io_acct(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    unsafe {
        (*bio_ptr).__bindgen_anon_1.set_done(0);
        (*bio_ptr).done_size = 0;
        (*bio_ptr).error = 0;
        completion_reinit(&raw mut (*bio_ptr).io_completion);
    }
    fence(Ordering::SeqCst);
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_end_io_acct(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    unsafe { (*bio_ptr).__bindgen_anon_1.set_done(1) };
    fence(Ordering::SeqCst);
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_endio(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    if let Some(cb) = unsafe { (*bio_ptr).end_io } {
        // SAFETY: `cb` is the bio owner's completion callback, invoked
        // with the same `bio_ptr` per the C `end_io(bio)` contract.
        unsafe { cb(bio_ptr) };
    }
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_complete(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    unsafe {
        bio_end_io_acct(bio_ptr);
        bio_endio(bio_ptr);
        complete_all(&raw mut (*bio_ptr).io_completion);
    }
}

// ===========================================================================
// Block device interface.
// ===========================================================================

extern "C" fn sdhci_blk_open(_blkdev: *mut blkdev_t) -> c_int {
    0
}
extern "C" fn sdhci_blk_release(_blkdev: *mut blkdev_t) -> c_int {
    0
}

/// `submit_bio` -- process a block I/O request. Iterates over bio
/// segments and reads/writes blocks via SDMA/PIO.
extern "C" fn sdhci_submit_bio(blkdev: *mut blkdev_t, bio_ptr: *mut bio) -> c_int {
    // SAFETY: `blkdev` is `(*sc).bdev`'s address for the `sc` that
    // registered it (`sdhci_init_one`'s `blkdev_register(&(*sc).bdev)`
    // call, mirrored below) -- `bdev` is `SdhciSoftc`'s field, so this
    // is the same "reverse the embedding" computation as the C
    // `container_of` macro.
    let sc = crate::mm::cffi::container_of::<SdhciSoftc, blkdev_t>(blkdev, core::mem::offset_of!(SdhciSoftc, bdev));

    let mut ret: c_int = 0;

    // SAFETY: `(*sc).lock` a live, initialised `mutex_t`.
    let _guard = KMutex::from_ptr(unsafe { &raw mut (*sc).lock }).lock();
    // SAFETY: `bio_ptr` live (caller/blkdev_submit_bio contract).
    unsafe { bio_start_io_acct(bio_ptr) };

    let mut iter = BioIter { blkno: 0, size: 0, size_done: 0, bvec_idx: 0 };
    // SAFETY: `bio_ptr` live.
    unsafe { bio_iter_start(bio_ptr, &mut iter) };
    let mut bvec: bio_vec = bio_vec { bv_page: ptr::null_mut(), len: 0, offset: 0 };
    // SAFETY: `bio_ptr` live; `bvec` local and live.
    while unsafe { bio_iter_copy_bvec(bio_ptr, &iter, &raw mut bvec) } {
        let sector = iter.blkno;
        let page = bvec.bv_page;

        if page.is_null() {
            ret = neg(crate::bindings::EINVAL);
            break;
        }

        let pa = __page_to_pa(page) as *mut c_void;
        if pa.is_null() {
            ret = neg(crate::bindings::EINVAL);
            break;
        }

        let data = (pa as u64 + bvec.offset as u64) as *mut c_void;
        let nblocks = bvec.len as u32 / SDHCI_BLOCK_SIZE_VAL;

        if nblocks == 0 {
            ret = neg(crate::bindings::EINVAL);
            break;
        }

        // SAFETY: `bio_ptr` live.
        ret = if unsafe { bio_dir_write(bio_ptr) } {
            // SAFETY: `sc` live, exclusively accessed under `_guard`;
            // `data` valid for `nblocks * 512` bytes per `bvec`'s
            // contract (validated by `bio_validate` before submission).
            unsafe { sdhci_write_blocks(sc, sector as u32, nblocks, data) }
        } else {
            // SAFETY: same as above.
            unsafe { sdhci_read_blocks(sc, sector as u32, nblocks, data) }
        };

        if ret < 0 {
            break;
        }

        iter.size_done += bvec.len;
    }

    drop(_guard);

    // SAFETY: `bio_ptr` live.
    unsafe {
        (*bio_ptr).error = ret;
        bio_complete(bio_ptr);
    }
    ret
}

static SDHCI_BLK_OPS: blkdev_ops_t =
    blkdev_ops_t { open: Some(sdhci_blk_open), release: Some(sdhci_blk_release), submit_bio: Some(sdhci_submit_bio) };

// ===========================================================================
// Instance initialisation.
// ===========================================================================

const EMMC_NAMES: [&core::ffi::CStr; MAX_SDH_INSTANCES] = [c"mmc0", c"mmc1", c"mmc2"];
const SD_NAMES: [&core::ffi::CStr; MAX_SDH_INSTANCES] = [c"sd0", c"sd1", c"sd2"];

/// Initialise a single SDHCI instance (`idx` indexes `platform.sdhci[]`,
/// 0/1/2; `is_emmc` non-zero for the eMMC instance). Returns `0` on
/// success.
///
/// # Safety
/// `idx` must be `< MAX_SDH_INSTANCES`; must be called at most once per
/// `idx` before any other function in this file observes `sdhci_sc(idx)`.
unsafe fn sdhci_init_one(idx: usize, is_emmc: bool) -> c_int {
    // SAFETY: caller contract.
    let sc = unsafe { sdhci_sc(idx) };

    // SAFETY: `sc` points at `size_of::<SdhciSoftc>()` bytes this call
    // exclusively owns.
    unsafe { memset(sc as *mut c_void, 0, core::mem::size_of::<SdhciSoftc>()) };
    // SAFETY: `sc` zeroed above, exclusively owned.
    unsafe {
        mutex_init(&raw mut (*sc).lock, c"x1_sdhci".as_ptr() as *mut c_char);
        (*sc).index = idx as c_int;
        (*sc).irq = platform.sdhci[idx].irq as c_int;

        // Map SDHCI registers (already identity-mapped by vm.c).
        (*sc).regs = platform.sdhci[idx].base as *mut u8;
        printf(
            c"x1_sdhci%d: MMIO base 0x%lx, IRQ %d\n".as_ptr(),
            idx as c_int,
            platform.sdhci[idx].base,
            platform.sdhci[idx].irq,
        );

        // Map APMU register (per-instance).
        if platform.sdhci[idx].apmu_base != 0 && platform.sdhci[idx].apmu_offset != 0 {
            (*sc).apmu_reg = (platform.sdhci[idx].apmu_base as u64 + platform.sdhci[idx].apmu_offset as u64) as *mut u32;
            printf(
                c"x1_sdhci%d: APMU ctrl @ 0x%lx\n".as_ptr(),
                idx as c_int,
                platform.sdhci[idx].apmu_base as u64 + platform.sdhci[idx].apmu_offset as u64,
            );
        }

        // Map shared AXI register (always at SDH0 offset in APMU); the
        // shared AXI offset is parsed from the FDT (apmu_axi_offset).
        if platform.sdhci[idx].apmu_base != 0 && platform.sdhci[idx].apmu_axi_offset != 0 {
            (*sc).apmu_axi_reg =
                (platform.sdhci[idx].apmu_base as u64 + platform.sdhci[idx].apmu_axi_offset as u64) as *mut u32;
        }

        // Map AIB clock register (APBC base + AIB offset, from FDT).
        if platform.sdhci[idx].apbc_base != 0 {
            (*sc).aib_reg = (platform.sdhci[idx].apbc_base as u64 + APBC_AIB_CLK_RST as u64) as *mut u32;
        }
    }

    // Enable clocks and deassert reset via APMU.
    // SAFETY: `sc` live, exclusively owned.
    unsafe { sdhci_apmu_enable(sc) };

    // Probe read: verify controller is alive after clock enable.
    // `SDHCI_HOST_VERSION` is a 16-bit register at offset 0xFE -- must
    // use `sdhci_readw` (not `sdhci_readl`) to avoid unaligned MMIO access.
    // SAFETY: `sc` live.
    let ver = unsafe { sdhci_readw(sc, SDHCI_HOST_VERSION) };
    if ver == 0xFFFF || ver == 0x0000 {
        // SAFETY: format string matches its two arguments.
        unsafe {
            printf(
                c"x1_sdhci%d: controller not responding (version=0x%x), aborting\n".as_ptr(),
                idx as c_int,
                ver as c_int,
            )
        };
        return -1;
    }
    // SAFETY: format string matches its two arguments.
    unsafe { printf(c"x1_sdhci%d: controller alive (version=0x%x)\n".as_ptr(), idx as c_int, ver as c_int) };

    // Initialise SDHCI controller hardware.
    // SAFETY: `sc` live.
    let ret = unsafe { sdhci_hw_init(sc) };
    if ret < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: hardware init failed\n".as_ptr(), idx as c_int) };
        return -1;
    }

    // Check card presence (for SD card slots).
    // SAFETY: `sc` live.
    if !is_emmc && !unsafe { sdhci_card_present(sc) } {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: no card detected\n".as_ptr(), idx as c_int) };
        // Not a fatal error -- slot is empty.
        return 0;
    }

    // Enumerate the card/device.
    // SAFETY: `sc` live.
    let ret = if is_emmc { unsafe { sdhci_enumerate_emmc(sc) } } else { unsafe { sdhci_enumerate_sd(sc) } };

    if ret < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: card enumeration failed\n".as_ptr(), idx as c_int) };
        return -1;
    }

    // SAFETY: `sc` live.
    unsafe { (*sc).initialized = 1 };

    // Register as a block device.
    // SAFETY: `sc` live, exclusively owned.
    unsafe {
        (*sc).bdev.dev.major = 4; // new major for SD/eMMC
        (*sc).bdev.dev.minor = idx as c_int + 1;
        (*sc).bdev.dev.devmode = (S_IFBLK | 0o600) as mode_t;
        (*sc).bdev.__bindgen_anon_1.set_readable(1);
        (*sc).bdev.__bindgen_anon_1.set_writable(1);
        (*sc).bdev.block_shift = 0; // 512 bytes per sector
        (*sc).bdev.ops = SDHCI_BLK_OPS;

        (*sc).bdev.dev.devname = if is_emmc { EMMC_NAMES[idx].as_ptr() } else { SD_NAMES[idx].as_ptr() };
    }

    // SAFETY: `sc` live.
    let ret = unsafe { blkdev_register(&raw mut (*sc).bdev) };
    if ret != 0 {
        // SAFETY: format string matches its two arguments.
        unsafe { printf(c"x1_sdhci%d: blkdev_register failed: %d\n".as_ptr(), idx as c_int, ret) };
        return -1;
    }

    // SAFETY: format string matches its three arguments.
    unsafe {
        printf(
            c"x1_sdhci%d: registered as %s (%ld MB, %d-bit bus)\n".as_ptr(),
            idx as c_int,
            (*sc).bdev.dev.devname,
            (*sc).capacity_blocks / 2048,
            (*sc).bus_width,
        )
    };

    SDHCI_COUNT.fetch_add(1, Ordering::SeqCst);
    0
}

// ===========================================================================
// Top-level init -- spawns a kthread for post-init probing.
// ===========================================================================

/// Mirror of `IS_ERR_OR_NULL` from `kernel/inc/errno.h`.
#[inline]
fn is_err_or_null<T>(p: *mut T) -> bool {
    if p.is_null() {
        return true;
    }
    let n = p as usize;
    (n as isize) < 0 && (n.wrapping_neg()) <= 4095
}

/// Per-instance init kthread: initialises one SDHCI slot.
extern "C" fn x1_sdhci_init_one_kthread(idx: u64, _arg2: u64) {
    let i = idx as usize;

    // SAFETY: `platform` is boot-time-populated, read-only from this point.
    if unsafe { platform.sdhci[i].base } == 0 {
        return;
    }

    // Skip SDIO (sdhci1) -- WiFi, not a block device.
    // SAFETY: see above.
    if unsafe { platform.sdhci[i].is_sdio } != 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: SDIO instance, skipping\n".as_ptr(), i as c_int) };
        return;
    }

    // SAFETY: see above.
    let is_emmc = unsafe { platform.sdhci[i].is_emmc } != 0;
    // SAFETY: `i < MAX_SDH_INSTANCES` (caller contract, see
    // `x1_sdhci_kthread`); called at most once per `i`.
    if unsafe { sdhci_init_one(i, is_emmc) } < 0 {
        // SAFETY: format string matches its one argument.
        unsafe { printf(c"x1_sdhci%d: init failed, skipping\n".as_ptr(), i as c_int) };
    }
}

/// Kernel thread entry point: spawns per-instance init kthreads so that
/// all SDHCI slots probe in parallel. Running in kthread context allows
/// `sleep_ms()` (scheduler-based timer sleep) instead of busy-waiting,
/// friendlier to the system during potentially slow SD/eMMC enumeration.
extern "C" fn x1_sdhci_kthread(_arg1: u64, _arg2: u64) {
    // SAFETY: `platform` boot-time-populated, read-only from this point.
    let mut n = unsafe { platform.sdhci_count } as usize;
    if n > MAX_SDH_INSTANCES {
        n = MAX_SDH_INSTANCES;
    }

    // SAFETY: format string matches its one argument.
    unsafe { printf(c"x1_sdhci: found %d instance(s)\n".as_ptr(), n as c_int) };

    for i in 0..n {
        let name: [u8; 5] = [b's', b'd', b'h', b'0' + i as u8, 0];
        let t = kthread_create(
            name.as_ptr() as *const c_char,
            x1_sdhci_init_one_kthread as *mut c_void,
            i as u64,
            0,
            KERNEL_STACK_ORDER,
        );
        if is_err_or_null(t) {
            // SAFETY: format string matches its one argument.
            unsafe { printf(c"x1_sdhci%d: failed to create init kthread\n".as_ptr(), i as c_int) };
        } else {
            wakeup(t);
        }
    }

    // kthread exits -- per-instance threads run independently.
}

/// `void x1_sdhci_init(void)` -- schedule SDHCI probing as a post-init
/// kthread. Called from `start_kernel`. The actual hardware probing and
/// card enumeration run in a dedicated kernel thread so that:
/// 1. The scheduler is already running -> `sleep_ms()` works.
/// 2. Boot proceeds without blocking on slow card init.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn x1_sdhci_init() {
    // SAFETY: `platform` boot-time-populated by `fdt.rs` before
    // `start_kernel.c` calls this (documented boot-order contract).
    if !(unsafe { platform.has_sdhci } != 0) {
        return;
    }

    let t = kthread_create(c"x1_sdhci".as_ptr(), x1_sdhci_kthread as *mut c_void, 0, 0, KERNEL_STACK_ORDER);
    if is_err_or_null(t) {
        unsafe { printf(c"x1_sdhci: failed to create init kthread\n".as_ptr()) };
        return;
    }
    wakeup(t);
}
