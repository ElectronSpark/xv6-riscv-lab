//! SpacemiT X1 EMAC Ethernet MAC/DMA driver -- Rust port of
//! `kernel/dev/x1_emac.c` (Phase 2 Wave 25, see
//! `docs/rustify/phase2_plan.md`; the last C in `kernel/dev/`, ported
//! together with `yt8531.rs`/`x1_sdhci.rs` in the same wave).
//!
//! Supports dual EMAC instances on the Orange Pi RV2 (SpacemiT X1 SoC).
//! Each EMAC is connected to a YT8531 PHY over RGMII (see
//! [`super::yt8531`]). DMA is non-coherent on this platform, so Zicbom
//! cache management operations (`cbo.clean`/`cbo.inval`) are used around
//! DMA transfers -- see [`dma_cache_clean`]/[`dma_cache_inval`] below.
//!
//! Key data flow: TX: `net.c` -> `netdev_ops.transmit` ->
//! [`x1_emac_transmit`] -> DMA. RX: IRQ -> [`x1_emac_intr`] ->
//! [`x1_emac_recv`] -> `net_rx()`.
//!
//! # QEMU verifiability (Wave 25 charter)
//!
//! [`x1_emac_init`] -- the only entry point `start_kernel.c` calls --
//! early-returns when `platform.has_emac` is false, which it always is
//! on QEMU's generated device tree (Wave 23 finding: no EMAC-compatible
//! node in `-machine virt`'s DTB). **Compile-verify + boot-no-regression
//! only**, flagged lower confidence per the plan's §3; the DMA ring
//! logic, MDIO-via-EMAC-register plumbing, and interrupt handler are
//! entirely unexercised on the verification target. Functional
//! verification needs real Orange-Pi-RV2 hardware.
//!
//! # Diff-review attestation
//!
//! Every function below was reviewed line-by-line against
//! `kernel/dev/x1_emac.c` at port time: register field masks/shifts,
//! ring index arithmetic (`% X1_EMAC_{TX,RX}_RING_SIZE`), descriptor
//! ownership-bit protocol, DMA cache-maintenance call placement (clean
//! before handing a buffer to DMA, invalidate after DMA writes it), and
//! log message text/argument order all match. No behavioural changes
//! beyond what's individually documented below.
//!
//! # DMA descriptor layout (hardware ABI)
//!
//! [`x1_tx_desc`]/[`x1_rx_desc`] are real `bindgen` types generated from
//! `kernel/inc/dev/x1_emac.h` (added to `wrapper.h` this wave) rather
//! than a hand-rolled `#[repr(C)]` mirror -- the DMA engine reads/writes
//! these 64-byte, cache-line-padded structures directly, so getting the
//! layout from the same header the hardware's register map is documented
//! against (instead of retyping it by hand and hoping) is the stronger
//! guarantee. [`layout_asserts`] below additionally pins `size_of`/
//! `offset_of` for both descriptor types and [`phy_state`] at compile
//! time (8 assertions: 2 struct sizes, 1 alignment, 4 field offsets, 1
//! `phy_state` size), so any future header change that would silently
//! shift the ring's binary layout fails the build instead of corrupting
//! DMA in the field.
//!
//! # MMIO ordering
//!
//! The C original has 7 `__sync_synchronize()` calls (full compiler +
//! hardware fence): 3 in [`x1_emac_apmu_config`], 1 in
//! [`x1_emac_dline_config`], 1 in [`x1_emac_transmit`] (before the
//! descriptor's `OWN` bit hands it to DMA), and 2 in [`x1_emac_recv`]
//! (one per completion path, before re-arming a descriptor's `OWN` bit).
//! Every one is preserved below as `core::sync::atomic::fence(Ordering::
//! SeqCst)` at the exact same call site (this crate's established
//! `__sync_synchronize()` -> `fence(SeqCst)` mapping, see
//! `kernel/bufcache.rs`, `kernel/mm/vm.rs`). Every MMIO register access
//! goes through [`sc_rd`]/[`sc_wr`] (`read_volatile`/`write_volatile`),
//! so the compiler can never reorder or elide any of them, matching the
//! C `volatile uint32 *` semantics 1:1; `dma_cache_clean`/
//! `dma_cache_inval`'s inline `cbo.*` asm blocks carry no `nomem`/`pure`
//! option, so they are conservatively treated as full memory barriers by
//! the compiler too, matching the C `asm volatile(... : "memory")`
//! clobber.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{fence, AtomicI32, Ordering};

use crate::bindings::{mbuf, netdev, netdev_ops, phy_state, platform_info, spinlock_t, x1_rx_desc, x1_tx_desc};
use crate::irq::irq_core::{plic_irq, register_irq_handler, IrqDesc};
use crate::sync::KSpinlock;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
// P3-D3c: `timer/sched_timer.rs`'s `sleep_ms` is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone -- crate-path import.
use crate::timer::sched_timer::sleep_ms;
// P3-D3c: the spinlock primitives are genuinely `unsafe fn`s in
// `crate::lock::spinlock` now that their `#[no_mangle]` exports are gone;
// this file's original extern declarations asserted `safe fn` (usual
// FFI-facade convention). Thin wrappers preserve that safe facade for the
// unchanged call sites.
// (`name` cast: old redeclaration said `*const c_char`, real fn takes
// `*mut c_char`; the callee only reads it.)
/// SAFETY: see [`crate::lock::spinlock::spin_init`]'s contract.
fn spin_init(lk: *mut spinlock_t, name: *const c_char) {
    unsafe { crate::lock::spinlock::spin_init(lk, name as *mut c_char) }
}
unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.
    // string.rs.
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
}

// P3-D3c: `dev/fdt.rs`'s boot-probed platform config is a plain
// crate-path import now that its `#[no_mangle]` export is gone (same
// `platform_info` type, unchanged call sites -- reads of a `static mut`
// stay `unsafe` either way).
use crate::dev::fdt::platform;

// P3-D3a: `kalloc` is genuinely `unsafe fn` in `crate::mm::kalloc` now
// that its `#[no_mangle]` export is gone; this file's original extern
// declaration was a plain (non-`safe`) `fn`, so every call site already
// sits in an `unsafe` context — the plain `use` keeps them unchanged.
use crate::mm::kalloc;
// P3-1D mesh sweep: net.rs/dev/netdev.rs are in scope for this wave;
// signatures are identical, so these become plain crate-path imports
// instead of `extern "C"` redeclarations.
use crate::net::{mbufalloc, mbuffree, net_rx};
// P3-D2a: proc/sched.rs entry points, reached as plain crate-path items
// instead of `extern "C"` redeclarations.
// P3-D3b: `kthread_create` (proc/thread.rs) is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone; reached via the `crate::proc`
// glob re-export like its neighbors here.
use crate::proc::{kthread_create, scheduler_yield, wakeup};
use super::netdev::{netdev_register, netdev_set_link};

// ===========================================================================
// Register/bit constants -- redeclared locally from `kernel/inc/dev/
// x1_emac.h` per this crate's established convention.
// ===========================================================================

// -- APMU control register bits (ctrl_reg, relative to apmu_base) --
const EMAC_BUS_CLK_EN: u32 = 1 << 0;
const EMAC_RESET_DEASSERT: u32 = 1 << 1;
const PHY_INTF_RGMII: u32 = 1 << 2;
const AXI_SINGLE_ID: u32 = 1 << 13;

// -- Delay-line register bits (dline_reg, relative to apmu_base) --
const EMAC_RX_DLINE_EN: u32 = 1 << 0;
const EMAC_RX_DLINE_CODE_OFFSET: u32 = 8;
const EMAC_RX_DLINE_CODE_MASK: u32 = 0x0000_FF00;
const EMAC_TX_DLINE_EN: u32 = 1 << 16;
const EMAC_TX_DLINE_CODE_OFFSET: u32 = 24;
const EMAC_TX_DLINE_CODE_MASK: u32 = 0xFF00_0000;

// -- DMA register set (offset from EMAC base) --
const DMA_CONFIGURATION: u32 = 0x0000;
const DMA_CONTROL: u32 = 0x0004;
const DMA_STATUS_IRQ: u32 = 0x0008;
const DMA_INTERRUPT_ENABLE: u32 = 0x000C;
const DMA_TRANSMIT_AUTO_POLL_COUNTER: u32 = 0x0010;
const DMA_TRANSMIT_POLL_DEMAND: u32 = 0x0014;
const DMA_RECEIVE_POLL_DEMAND: u32 = 0x0018;
const DMA_TRANSMIT_BASE_ADDRESS: u32 = 0x001C;
const DMA_RECEIVE_BASE_ADDRESS: u32 = 0x0020;

// -- DMA_CONFIGURATION bits --
const DMA_CFG_SW_RESET: u32 = 1 << 0;
const DMA_CFG_BURST_16WORD: u32 = 1 << 5;
const DMA_CFG_STRICT_BURST: u32 = 1 << 17;
const DMA_CFG_64BIT_MODE: u32 = 1 << 18;

// -- DMA_CONTROL bits --
const DMA_CTRL_START_TX: u32 = 1 << 0;
const DMA_CTRL_START_RX: u32 = 1 << 1;

// -- DMA_STATUS_IRQ bits --
const DMA_IRQ_TX_DONE: u32 = 1 << 0;
const DMA_IRQ_TX_DESC_UNAVAIL: u32 = 1 << 1;
const DMA_IRQ_TX_STOPPED: u32 = 1 << 2;
const DMA_IRQ_RX_DONE: u32 = 1 << 4;
const DMA_IRQ_RX_DESC_UNAVAIL: u32 = 1 << 5;
const DMA_IRQ_RX_STOPPED: u32 = 1 << 6;

// -- DMA_INTERRUPT_ENABLE bits --
const DMA_IE_TX_DONE: u32 = 1 << 0;
const DMA_IE_TX_STOPPED: u32 = 1 << 2;
const DMA_IE_RX_DONE: u32 = 1 << 4;
const DMA_IE_RX_DESC_UNAVAIL: u32 = 1 << 5;
const DMA_IE_RX_STOPPED: u32 = 1 << 6;

// -- MAC register set (offset from EMAC base) --
const MAC_GLOBAL_CONTROL: u32 = 0x0100;
const MAC_TRANSMIT_CONTROL: u32 = 0x0104;
const MAC_RECEIVE_CONTROL: u32 = 0x0108;
const MAC_ADDRESS_CONTROL: u32 = 0x0118;
const MAC_ADDRESS1_HIGH: u32 = 0x0120;
const MAC_ADDRESS1_MED: u32 = 0x0124;
const MAC_ADDRESS1_LOW: u32 = 0x0128;
const MAC_MULTICAST_HASH_TABLE1: u32 = 0x0150;
const MAC_MULTICAST_HASH_TABLE2: u32 = 0x0154;
const MAC_MULTICAST_HASH_TABLE3: u32 = 0x0158;
const MAC_MULTICAST_HASH_TABLE4: u32 = 0x015C;
const MAC_FC_CONTROL: u32 = 0x0160;
const MAC_TX_FIFO_ALMOST_FULL: u32 = 0x01C0;
const MAC_TX_PKT_START_THRESHOLD: u32 = 0x01C4;
const MAC_RX_PKT_START_THRESHOLD: u32 = 0x01C8;
const MAC_INTERRUPT_ENABLE: u32 = 0x01E4;

// -- MAC_GLOBAL_CONTROL bits --
const MAC_GC_SPEED_MASK: u32 = 0x03;
const MAC_GC_SPEED_10M: u32 = 0x00;
const MAC_GC_SPEED_100M: u32 = 0x01;
const MAC_GC_SPEED_1000M: u32 = 0x02;
const MAC_GC_FULL_DUPLEX: u32 = 1 << 2;
const MAC_GC_RESET_RX_STATS: u32 = 1 << 3;
const MAC_GC_RESET_TX_STATS: u32 = 1 << 4;

// -- MAC_TRANSMIT_CONTROL bits --
const MAC_TC_TX_ENABLE: u32 = 1 << 0;
const MAC_TC_TX_AUTO_RETRY: u32 = 1 << 3;

// -- MAC_RECEIVE_CONTROL bits --
const MAC_RC_RX_ENABLE: u32 = 1 << 0;
const MAC_RC_STORE_FORWARD: u32 = 1 << 3;

// -- MAC_ADDRESS_CONTROL bits --
const MAC_AC_ADDR1_ENABLE: u32 = 1 << 0;

// -- MAC_FC_CONTROL bits --
const MAC_FC_DECODE_ENABLE: u32 = 1 << 0;

// -- RX frame status bits (descriptor word 0, bits [28:14]) --
const RX_FRAME_ALIGN_ERR: u32 = 1 << 0;
const RX_FRAME_RUNT: u32 = 1 << 1;
const RX_FRAME_CRC_ERR: u32 = 1 << 6;
const RX_FRAME_MAX_LEN_ERR: u32 = 1 << 7;
const RX_FRAME_JABBER_ERR: u32 = 1 << 8;
const RX_FRAME_LENGTH_ERR: u32 = 1 << 9;
const RX_FRAME_ERROR_MASK: u32 =
    RX_FRAME_ALIGN_ERR | RX_FRAME_RUNT | RX_FRAME_CRC_ERR | RX_FRAME_MAX_LEN_ERR | RX_FRAME_JABBER_ERR | RX_FRAME_LENGTH_ERR;

// -- DMA descriptor structures --
const DESC_CHAINED: u32 = 1 << 25; // SecondAddressChained in word1

// -- RX descriptor word0 field accessors --
#[inline(always)]
const fn rx_desc_frame_len(w0: u32) -> u32 {
    w0 & 0x3FFF
}
#[inline(always)]
const fn rx_desc_app_status(w0: u32) -> u32 {
    (w0 >> 14) & 0x7FFF
}
const RX_DESC_OWN: u32 = 1 << 31;

// -- RX descriptor word1 field setters --
#[inline(always)]
const fn rx_desc_buf1_size(sz: u32) -> u32 {
    sz & 0xFFF
}

// -- TX descriptor word0 --
const TX_DESC_OWN: u32 = 1 << 31;

// -- TX descriptor word1 field setters --
#[inline(always)]
const fn tx_desc_buf1_size(sz: u32) -> u32 {
    sz & 0xFFF
}
const TX_DESC_FIRST_SEG: u32 = 1 << 29;
const TX_DESC_LAST_SEG: u32 = 1 << 30;
const TX_DESC_IOC: u32 = 1 << 31;

// -- Driver constants --
const X1_EMAC_DEFAULT_TX_THRESH: u32 = 0x5EE; // store-forward mode (matches U-Boot)
const X1_EMAC_DEFAULT_RX_THRESH: u32 = 12;
const X1_EMAC_MAX_FRAME_SIZE: u32 = 1518;
const X1_EMAC_FCS_SIZE: u32 = 4;
const X1_EMAC_TX_RING_SIZE: usize = 64;
const X1_EMAC_RX_RING_SIZE: usize = 64;
const MBUF_SIZE: u32 = 2048; // kernel/inc/dev/net.h

const MAX_EMAC_INSTANCES: usize = 2;
const KERNEL_STACK_ORDER: c_int = 2;

// ===========================================================================
// DMA descriptor / phy_state layout asserts (hardware ABI). See module
// doc's "DMA descriptor layout" section.
// ===========================================================================
mod layout_asserts {
    use super::{phy_state, x1_rx_desc, x1_tx_desc};
    const _SIZE_RX: () = assert!(core::mem::size_of::<x1_rx_desc>() == 64);
    const _SIZE_TX: () = assert!(core::mem::size_of::<x1_tx_desc>() == 64);
    const _ALIGN_RX: () = assert!(core::mem::align_of::<x1_rx_desc>() == core::mem::align_of::<u32>());
    const _OFF_RX_WORD1: () = assert!(core::mem::offset_of!(x1_rx_desc, word1) == 4);
    const _OFF_RX_BUF_ADDR: () = assert!(core::mem::offset_of!(x1_rx_desc, buf_addr) == 8);
    const _OFF_RX_BUF_ADDR2: () = assert!(core::mem::offset_of!(x1_rx_desc, buf_addr2) == 12);
    const _OFF_TX_BUF_ADDR2: () = assert!(core::mem::offset_of!(x1_tx_desc, buf_addr2) == 12);
    const _SIZE_PHY_STATE: () = assert!(core::mem::size_of::<phy_state>() == 12);
}

// ===========================================================================
// DMA cache maintenance (Zicbom `cbo.clean`/`cbo.inval`). Mirrors
// `kernel/inc/cache.h`'s static inlines; reimplemented locally per this
// crate's established convention for macro-only/static-inline-only
// headers (this file needs `dma_cache_clean`/`dma_cache_inval` only --
// `dma_cache_flush` has no caller in the C original and is dropped).
// ===========================================================================
const CBOM_BLOCK_SIZE: usize = 64;

/// `cbo.clean (rs1)` -- write back dirty cache lines without invalidating.
///
/// # Safety
/// No memory-safety precondition beyond a valid address in `rs1`
/// (matches the C `asm volatile` -- this is a cache-maintenance
/// instruction, not a memory access in the Rust aliasing sense). No
/// `nomem`/`pure`/`readonly` option is set, so this asm block is
/// conservatively treated by the compiler as reading and writing
/// arbitrary memory -- the same guarantee the C `"memory"` clobber gives.
#[inline(always)]
unsafe fn cbo_clean(addr: usize) {
    unsafe {
        core::arch::asm!(".insn i 0x0F, 2, x0, {0}, 1", in(reg) addr);
    }
}

/// `cbo.inval (rs1)` -- invalidate cache lines (drops dirty data).
///
/// # Safety
/// Same as [`cbo_clean`].
#[inline(always)]
unsafe fn cbo_inval(addr: usize) {
    unsafe {
        core::arch::asm!(".insn i 0x0F, 2, x0, {0}, 0", in(reg) addr);
    }
}

/// Clean (write back) a range -- use before DMA TX (CPU -> device).
///
/// # Safety
/// `addr .. addr + size` must be valid to read/write cache-line-aligned
/// (the loop below rounds `addr` down to `CBOM_BLOCK_SIZE`, matching the
/// C original exactly, so the actual touched range can start up to 63
/// bytes before `addr`).
unsafe fn dma_cache_clean(addr: *mut c_void, size: usize) {
    let mut start = (addr as usize) & !(CBOM_BLOCK_SIZE - 1);
    let end = (addr as usize) + size;
    while start < end {
        // SAFETY: caller contract.
        unsafe { cbo_clean(start) };
        start += CBOM_BLOCK_SIZE;
    }
}

/// Invalidate a range -- use after DMA RX (device -> CPU).
///
/// # Safety
/// Same as [`dma_cache_clean`]; additionally, per the C original's own
/// warning, any dirty (not-yet-written-back) data in the touched cache
/// lines is lost -- only safe when the range is not shared with other
/// live data.
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
// Per-instance driver state. Mirrors `struct x1_emac_softc` field-for-
// field; purely internal (never crosses the FFI boundary as anything but
// an opaque `void *` handed to `register_irq_handler`/netdev's
// `priv_`), so `#[repr(C)]` is kept only for straightforward diffing
// against the C original, not because any external ABI depends on it.
// ===========================================================================
#[repr(C)]
struct X1EmacSoftc {
    regs: *mut u32, // EMAC MMIO base

    // APMU registers (identity-mapped virtual = physical).
    ctrl_reg: *mut u32,  // APMU ctrl register
    dline_reg: *mut u32, // APMU delay-line register

    // PHY.
    phy_addr: c_int, // MDIO address of the PHY
    phy: phy_state,  // current link state

    // Clock tuning.
    tx_phase: u32,
    rx_phase: u32,

    // GPIO for PHY reset.
    reset_gpio: c_int,

    // TX ring.
    tx_ring: *mut x1_tx_desc,
    tx_mbufs: [*mut mbuf; X1_EMAC_TX_RING_SIZE],
    tx_head: u32, // next to transmit (producer)
    tx_tail: u32, // next to reclaim (consumer)

    // RX ring.
    rx_ring: *mut x1_rx_desc,
    rx_mbufs: [*mut mbuf; X1_EMAC_RX_RING_SIZE],
    rx_head: u32, // next to receive (consumer)

    // Lock.
    lock: spinlock_t,

    // Netdev.
    ndev: netdev,

    // IRQ number (raw PLIC hwirq, before PLIC_IRQ offset).
    irq: c_int,

    // Instance index (0 or 1).
    index: c_int,
}

// SAFETY: shared only through `EMAC_SC`'s single-hart-init-then-
// documented-concurrent-access pattern (see the storage comment below);
// never implicitly aliased as `&mut` across harts.
unsafe impl Sync for X1EmacSoftc {}

// ---------------------------------------------------------------------------
// Global storage. `SyncCell<T>` is redefined locally per this crate's
// established convention (see `kernel/dev/dev.rs`, `kernel/kobject.rs`,
// `mm/pcache.rs`). Each `X1EmacSoftc` is zeroed by an explicit `memset`
// in `x1_emac_init_one` before any field is read (mirrors the C: BSS
// zero-init followed by a redundant defensive `memset`; this port keeps
// only the explicit `memset`, achieving the same "always zeroed before
// use" property without relying on Rust statics being BSS-placed).
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

static EMAC_SC: SyncCell<[MaybeUninit<X1EmacSoftc>; MAX_EMAC_INSTANCES]> =
    SyncCell(UnsafeCell::new([const { MaybeUninit::uninit() }; MAX_EMAC_INSTANCES]));

/// # Safety
/// `idx` must be `< MAX_EMAC_INSTANCES`.
#[inline(always)]
unsafe fn emac_sc(idx: usize) -> *mut X1EmacSoftc {
    // SAFETY: caller contract; `EMAC_SC` is a live, `MAX_EMAC_INSTANCES`-
    // element array.
    unsafe { (EMAC_SC.get() as *mut X1EmacSoftc).add(idx) }
}

/// `static volatile int emac_init_done[MAX_EMAC_INSTANCES]` -- `1` = ok,
/// `-1` = fail, `0` = not yet finished. Plain `AtomicI32` array (mirrors
/// the C `__atomic_*` builtins used at every access site 1:1 -- see
/// module doc's ordering accounting).
static EMAC_INIT_DONE: [AtomicI32; MAX_EMAC_INSTANCES] = [const { AtomicI32::new(0) }; MAX_EMAC_INSTANCES];

/// `static int emac_count` -- number of successfully initialised
/// instances. Only ever incremented (`__ATOMIC_SEQ_CST` in the C
/// original); no reader exists in this file, matching the C (dead
/// telemetry, preserved as-is).
static EMAC_COUNT: AtomicI32 = AtomicI32::new(0);

extern "C" fn x1_emac_intr(_irq: c_int, data: *mut c_void, _dev: *mut c_void) {
    // SAFETY: `data` is the `X1EmacSoftc*` this instance's
    // `x1_emac_init_one` registered via `register_irq_handler` below;
    // live for the process lifetime (never freed).
    let sc = data as *mut X1EmacSoftc;

    // Read and acknowledge DMA status.
    // SAFETY: `sc` live per above.
    let status = unsafe { sc_rd(sc, DMA_STATUS_IRQ) };
    // Acknowledge all bits by writing them back.
    // SAFETY: see above.
    unsafe { sc_wr(sc, DMA_STATUS_IRQ, status) };

    if status & DMA_IRQ_RX_DONE != 0 {
        // SAFETY: `sc` live.
        unsafe { x1_emac_recv(sc) };
    }

    if status & DMA_IRQ_TX_DONE != 0 {
        // SAFETY: `sc` live.
        unsafe { x1_emac_tx_reclaim(sc) };
    }

    if status & DMA_IRQ_RX_DESC_UNAVAIL != 0 {
        // RX ring ran out of descriptors. Kick RX DMA to resume.
        // SAFETY: `sc` live.
        unsafe { sc_wr(sc, DMA_RECEIVE_POLL_DEMAND, 0xFF) };
    }

    if status & (DMA_IRQ_TX_STOPPED | DMA_IRQ_RX_STOPPED) != 0 {
        // SAFETY: `sc` live; format string matches its two arguments.
        crate::kprintln!(
            "x1_emac{}: DMA stopped! status=0x{:x}",
            unsafe { (*sc).index },
            ((status as i32 as i64) as u64),
        );
    }
}

extern "C" fn x1_emac_transmit_op(ndev: *mut netdev, m: *mut mbuf) -> c_int {
    // SAFETY: `ndev->priv_` is the `X1EmacSoftc*` `x1_emac_init_one`
    // stashed there before `netdev_register`; `ndev` is caller-provided
    // and live for the duration of this call (established netdev.rs
    // contract).
    let sc = unsafe { (*ndev).priv_ } as *mut X1EmacSoftc;
    // SAFETY: `sc`/`m` live per caller contract.
    unsafe { x1_emac_transmit(sc, m) }
}

static X1_EMAC_NETDEV_OPS: netdev_ops = netdev_ops { transmit: Some(x1_emac_transmit_op) };

// ===========================================================================
// Low-level MMIO helpers. Mirrors `sc_rd`/`sc_wr` static inlines.
// ===========================================================================

/// # Safety
/// `sc` must be live; `(*sc).regs + off .. +4` must be valid MMIO space.
#[inline(always)]
unsafe fn sc_rd(sc: *mut X1EmacSoftc, off: u32) -> u32 {
    // SAFETY: caller contract.
    unsafe { ptr::read_volatile(((*sc).regs as *mut u8).add(off as usize) as *mut u32) }
}

/// # Safety
/// Same as [`sc_rd`].
#[inline(always)]
unsafe fn sc_wr(sc: *mut X1EmacSoftc, off: u32, val: u32) {
    // SAFETY: caller contract.
    unsafe { ptr::write_volatile(((*sc).regs as *mut u8).add(off as usize) as *mut u32, val) };
}

// ===========================================================================
// APMU / clock / delay-line configuration.
// ===========================================================================

/// Configure the APMU control register for RGMII mode and AXI single-ID.
///
/// # Safety
/// `sc` must point to a live, exclusively-owned (not-yet-published)
/// `X1EmacSoftc`.
unsafe fn x1_emac_apmu_config(sc: *mut X1EmacSoftc) {
    // SAFETY: caller contract.
    let ctrl_reg = unsafe { (*sc).ctrl_reg };
    if ctrl_reg.is_null() {
        // SAFETY: caller contract; format string matches its one argument.
        crate::kprintln!("x1_emac{}: no ctrl_reg, skipping APMU config", unsafe { (*sc).index });
        return;
    }

    // SAFETY: `ctrl_reg` non-null, a live MMIO register per caller contract.
    let mut val = unsafe { ptr::read_volatile(ctrl_reg) };

    // Enable bus clock gate -- required before any EMAC register access.
    val |= EMAC_BUS_CLK_EN;
    // SAFETY: see above.
    unsafe { ptr::write_volatile(ctrl_reg, val) };
    fence(Ordering::SeqCst);

    // Deassert reset (take EMAC out of reset).
    val |= EMAC_RESET_DEASSERT;
    // SAFETY: see above.
    unsafe { ptr::write_volatile(ctrl_reg, val) };
    fence(Ordering::SeqCst);
    scheduler_yield(); // allow reset to propagate

    // Set RGMII interface mode + AXI single ID.
    val |= PHY_INTF_RGMII;
    val |= AXI_SINGLE_ID;
    // SAFETY: see above.
    unsafe { ptr::write_volatile(ctrl_reg, val) };
    fence(Ordering::SeqCst);
}

/// Program RGMII TX and RX delay-lines using the APMU dline register
/// (`CLK_TUNING_BY_DLINE` method from the Linux driver).
///
/// # Safety
/// Same as [`x1_emac_apmu_config`].
unsafe fn x1_emac_dline_config(sc: *mut X1EmacSoftc) {
    // SAFETY: caller contract.
    let dline_reg = unsafe { (*sc).dline_reg };
    if dline_reg.is_null() {
        // SAFETY: caller contract; format string matches its one argument.
        crate::kprintln!("x1_emac{}: no dline_reg, skipping delay-line config", unsafe { (*sc).index });
        return;
    }

    // SAFETY: `dline_reg` non-null, live MMIO per caller contract.
    let mut val = unsafe { ptr::read_volatile(dline_reg) };

    // TX delay line.
    val &= !EMAC_TX_DLINE_CODE_MASK;
    // SAFETY: caller contract.
    val |= unsafe { (*sc).tx_phase } << EMAC_TX_DLINE_CODE_OFFSET;
    val |= EMAC_TX_DLINE_EN;

    // RX delay line.
    val &= !EMAC_RX_DLINE_CODE_MASK;
    // SAFETY: caller contract.
    val |= unsafe { (*sc).rx_phase } << EMAC_RX_DLINE_CODE_OFFSET;
    val |= EMAC_RX_DLINE_EN;

    // SAFETY: see above.
    unsafe { ptr::write_volatile(dline_reg, val) };
    fence(Ordering::SeqCst);

    // SAFETY: caller contract; format string matches its three arguments.
    crate::kprintln!(
        "x1_emac{}: delay-line tx={} rx={}",
        unsafe { (*sc).index },
        unsafe { (*sc).tx_phase },
        unsafe { (*sc).rx_phase },
    );
}

// ===========================================================================
// DMA reset.
// ===========================================================================

/// # Safety
/// `sc` must be live.
unsafe fn x1_emac_dma_reset(sc: *mut X1EmacSoftc) -> c_int {
    // Stop DMA.
    // SAFETY: caller contract.
    unsafe { sc_wr(sc, DMA_CONTROL, 0) };

    // Assert software reset.
    // SAFETY: caller contract.
    unsafe {
        sc_wr(sc, DMA_CONFIGURATION, DMA_CFG_SW_RESET);
    }
    sleep_ms(10);
    // SAFETY: caller contract.
    unsafe { sc_wr(sc, DMA_CONFIGURATION, 0) };
    sleep_ms(10);

    0
}

// ===========================================================================
// MAC initialisation (mirrors `emac_init_hw` in Linux).
// ===========================================================================

/// # Safety
/// `sc` must be live; `addr` must point to 6 readable bytes.
unsafe fn x1_emac_set_mac_addr(sc: *mut X1EmacSoftc, addr: *const u8) {
    // SAFETY: caller contract.
    unsafe {
        sc_wr(sc, MAC_ADDRESS1_HIGH, ((*addr.add(1) as u32) << 8) | *addr.add(0) as u32);
        sc_wr(sc, MAC_ADDRESS1_MED, ((*addr.add(3) as u32) << 8) | *addr.add(2) as u32);
        sc_wr(sc, MAC_ADDRESS1_LOW, ((*addr.add(5) as u32) << 8) | *addr.add(4) as u32);
    }
}

/// # Safety
/// `sc` must be live.
unsafe fn x1_emac_init_hw(sc: *mut X1EmacSoftc) {
    // SAFETY: caller contract for the whole function body below.
    unsafe {
        // Disable TX and RX.
        sc_wr(sc, MAC_RECEIVE_CONTROL, 0);
        sc_wr(sc, MAC_TRANSMIT_CONTROL, 0);

        // Reset MAC + statistic counters (matches U-Boot emac_reset_hw).
        sc_wr(sc, MAC_GLOBAL_CONTROL, MAC_GC_RESET_RX_STATS | MAC_GC_RESET_TX_STATS);
        sc_wr(sc, MAC_GLOBAL_CONTROL, 0);

        // Enable MAC address 1 filtering.
        sc_wr(sc, MAC_ADDRESS_CONTROL, MAC_AC_ADDR1_ENABLE);

        // Zero the multicast hash table.
        sc_wr(sc, MAC_MULTICAST_HASH_TABLE1, 0);
        sc_wr(sc, MAC_MULTICAST_HASH_TABLE2, 0);
        sc_wr(sc, MAC_MULTICAST_HASH_TABLE3, 0);
        sc_wr(sc, MAC_MULTICAST_HASH_TABLE4, 0);

        // TX FIFO almost full threshold.
        sc_wr(sc, MAC_TX_FIFO_ALMOST_FULL, 0x1f8);

        // TX packet start threshold.
        sc_wr(sc, MAC_TX_PKT_START_THRESHOLD, X1_EMAC_DEFAULT_TX_THRESH);

        // RX packet start threshold.
        sc_wr(sc, MAC_RX_PKT_START_THRESHOLD, X1_EMAC_DEFAULT_RX_THRESH);

        // Flow control: decode enable.
        sc_wr(sc, MAC_FC_CONTROL, MAC_FC_DECODE_ENABLE);

        // DMA reset.
        x1_emac_dma_reset(sc);

        // DMA configuration: strict burst + 64-bit + 16-word burst length.
        // Matches the vendor Linux driver's register value (0x00060020).
        // dma-burst-len=5 in DT -> bit 5 -> BURST_16WORD.
        let val = DMA_CFG_STRICT_BURST | DMA_CFG_64BIT_MODE | DMA_CFG_BURST_16WORD;
        sc_wr(sc, DMA_CONFIGURATION, val);

        // Disable all interrupts initially.
        sc_wr(sc, MAC_INTERRUPT_ENABLE, 0);
        sc_wr(sc, DMA_INTERRUPT_ENABLE, 0);
    }
}

/// Set MAC speed/duplex based on PHY link state.
///
/// # Safety
/// `sc` must be live.
unsafe fn x1_emac_adjust_link(sc: *mut X1EmacSoftc) {
    // SAFETY: caller contract.
    let mut ctrl = unsafe { sc_rd(sc, MAC_GLOBAL_CONTROL) };

    ctrl &= !MAC_GC_SPEED_MASK;
    // SAFETY: caller contract.
    let speed = unsafe { (*sc).phy.speed };
    ctrl |= match speed {
        1000 => MAC_GC_SPEED_1000M,
        100 => MAC_GC_SPEED_100M,
        _ => MAC_GC_SPEED_10M,
    };

    // SAFETY: caller contract.
    if unsafe { (*sc).phy.full_duplex } != 0 {
        ctrl |= MAC_GC_FULL_DUPLEX;
    } else {
        ctrl &= !MAC_GC_FULL_DUPLEX;
    }

    // SAFETY: caller contract.
    unsafe { sc_wr(sc, MAC_GLOBAL_CONTROL, ctrl) };
}

// ===========================================================================
// TX ring setup.
// ===========================================================================

/// # Safety
/// `sc` must point to a live, exclusively-owned `X1EmacSoftc`.
unsafe fn x1_emac_tx_ring_init(sc: *mut X1EmacSoftc) -> c_int {
    // Allocate descriptor ring (needs to be page-aligned for DMA): 64
    // entries x 64 bytes = 4096 = exactly 1 page.
    let ring_size = X1_EMAC_TX_RING_SIZE * core::mem::size_of::<x1_tx_desc>();

    // Use kalloc pages -- identity mapped, so VA == PA.
    // SAFETY: `kalloc` returns a fresh page or null.
    let ring = unsafe { kalloc() } as *mut x1_tx_desc;
    if ring.is_null() {
        return -1;
    }
    // SAFETY: caller contract; `ring` is a fresh `ring_size`-byte allocation.
    unsafe {
        (*sc).tx_ring = ring;
        memset(ring as *mut c_void, 0, ring_size);
        memset((*sc).tx_mbufs.as_mut_ptr() as *mut c_void, 0, core::mem::size_of_val(&(*sc).tx_mbufs));
        (*sc).tx_head = 0;
        (*sc).tx_tail = 0;
    }

    // Set up chained descriptor pointers. Each descriptor's `buf_addr2`
    // points to the next descriptor; the last wraps back to the first.
    for i in 0..X1_EMAC_TX_RING_SIZE {
        let next = (i + 1) % X1_EMAC_TX_RING_SIZE;
        // SAFETY: `ring` has `X1_EMAC_TX_RING_SIZE` live entries per
        // the allocation above; `i`/`next` are both in range.
        unsafe {
            let d = ring.add(i);
            (*d).buf_addr2 = ring.add(next) as u64 as u32;
            (*d).word1 = DESC_CHAINED; // SecondAddressChained
        }
    }

    // Flush descriptors to memory.
    // SAFETY: `ring` valid for `ring_size` bytes.
    unsafe { dma_cache_clean(ring as *mut c_void, ring_size) };

    0
}

// ===========================================================================
// RX ring setup.
// ===========================================================================

/// # Safety
/// Same as [`x1_emac_tx_ring_init`].
unsafe fn x1_emac_rx_ring_init(sc: *mut X1EmacSoftc) -> c_int {
    // 64 entries x 64 bytes = 4096 = exactly 1 page.
    let ring_size = X1_EMAC_RX_RING_SIZE * core::mem::size_of::<x1_rx_desc>();

    // SAFETY: `kalloc` returns a fresh page or null.
    let ring = unsafe { kalloc() } as *mut x1_rx_desc;
    if ring.is_null() {
        return -1;
    }
    // SAFETY: caller contract; `ring` fresh `ring_size`-byte allocation.
    unsafe {
        (*sc).rx_ring = ring;
        memset(ring as *mut c_void, 0, ring_size);
        memset((*sc).rx_mbufs.as_mut_ptr() as *mut c_void, 0, core::mem::size_of_val(&(*sc).rx_mbufs));
        (*sc).rx_head = 0;
    }

    // Allocate mbufs and set up each descriptor in chained mode.
    for i in 0..X1_EMAC_RX_RING_SIZE {
        // `mbufalloc` returns a fresh mbuf or null.
        let m = mbufalloc(0);
        if m.is_null() {
            // SAFETY: caller contract; format string matches its two arguments.
            crate::kprintln!("x1_emac{}: failed to alloc rx mbuf {}", unsafe { (*sc).index }, i as c_int);
            return -1;
        }
        // SAFETY: caller contract; `i` in range.
        unsafe { (*sc).rx_mbufs[i] = m };

        let next = (i + 1) % X1_EMAC_RX_RING_SIZE;
        // SAFETY: `ring`/`m` live; `i`/`next` in range.
        unsafe {
            let d = ring.add(i);
            (*d).buf_addr = (*m).head as u64 as u32;
            (*d).buf_addr2 = ring.add(next) as u64 as u32;
            (*d).word1 = rx_desc_buf1_size(MBUF_SIZE) | DESC_CHAINED;
            (*d).word0 = RX_DESC_OWN; // give to DMA

            // Flush the mbuf buffer so DMA can write into it.
            dma_cache_clean((*m).head as *mut c_void, MBUF_SIZE as usize);
        }
    }

    // Flush all descriptors to memory.
    // SAFETY: `ring` valid for `ring_size` bytes.
    unsafe { dma_cache_clean(ring as *mut c_void, ring_size) };

    0
}

// ===========================================================================
// Transmit.
// ===========================================================================

/// # Safety
/// `sc`/`m` must be live for the duration of this call; `m` is
/// transferred to the ring on success (queued for DMA) or freed on
/// reclaim of a previous occupant of the slot -- matches the C's
/// ownership contract (`net.c` never touches `m` again after this call).
unsafe fn x1_emac_transmit(sc: *mut X1EmacSoftc, m: *mut mbuf) -> c_int {
    // SAFETY: caller contract; `(*sc).lock` is a live, initialised
    // `spinlock_t` embedded in `sc` (set up by `x1_emac_init_one`).
    let _guard = KSpinlock::from_bindings(unsafe { &raw mut (*sc).lock }).lock();

    // SAFETY: caller contract.
    let idx = unsafe { (*sc).tx_head } as usize;
    // SAFETY: `idx < X1_EMAC_TX_RING_SIZE` (produced by `% RING_SIZE`
    // arithmetic below/at ring init).
    let d = unsafe { (*sc).tx_ring.add(idx) };

    // Invalidate descriptor to read current state from memory.
    // SAFETY: `d` a live ring entry.
    unsafe { dma_cache_inval(d as *mut c_void, core::mem::size_of::<x1_tx_desc>()) };

    // Check if descriptor is still owned by DMA.
    // SAFETY: `d` live.
    if unsafe { (*d).word0 } & TX_DESC_OWN != 0 {
        return -1; // ring full
    }

    // Free any previously transmitted mbuf.
    // SAFETY: caller contract.
    unsafe {
        if !(*sc).tx_mbufs[idx].is_null() {
            mbuffree((*sc).tx_mbufs[idx]);
            (*sc).tx_mbufs[idx] = ptr::null_mut();
        }
    }

    // Set up the descriptor -- chained mode: `buf_addr2` already points
    // to the next descriptor (set during ring init, preserved here).
    // SAFETY: `d`/`m` live.
    unsafe {
        (*d).buf_addr = (*m).head as u64 as u32;
        (*d).word1 = tx_desc_buf1_size((*m).len) | TX_DESC_FIRST_SEG | TX_DESC_LAST_SEG | TX_DESC_IOC | DESC_CHAINED;

        (*sc).tx_mbufs[idx] = m;

        // Flush the packet data.
        dma_cache_clean((*m).head as *mut c_void, (*m).len as usize);
    }

    // Write barrier before setting OWN bit.
    fence(Ordering::SeqCst);
    // SAFETY: `d` live.
    unsafe { (*d).word0 = TX_DESC_OWN };

    // Flush the descriptor.
    // SAFETY: `d` live.
    unsafe { dma_cache_clean(d as *mut c_void, core::mem::size_of::<x1_tx_desc>()) };

    // Advance head.
    // SAFETY: caller contract.
    unsafe { (*sc).tx_head = (idx as u32 + 1) % X1_EMAC_TX_RING_SIZE as u32 };

    // Kick DMA transmit poll.
    // SAFETY: caller contract.
    unsafe { sc_wr(sc, DMA_TRANSMIT_POLL_DEMAND, 0xFF) };

    0
}

// ===========================================================================
// Receive.
// ===========================================================================

/// RX budget: limit how many packets we process per interrupt to avoid
/// holding the CPU in ISR context for too long and consuming too many
/// mbufs before the network stack gets a chance to process them.
const X1_EMAC_RX_BUDGET: i32 = 32;

/// Give a descriptor back to DMA with its existing buffer (chained mode
/// -- `buf_addr`/`buf_addr2` untouched). Mirrors the C `recycle:` label,
/// which both of [`x1_emac_recv`]'s error branches `goto` into -- Rust
/// has no `goto`, so this shared helper reproduces the same "exactly one
/// source-level copy of the recycle sequence, reached from two call
/// sites" shape instead of duplicating the sequence inline at each
/// branch (which would double this function's `fence` count relative to
/// the C original -- see module doc's ordering accounting: exactly 2
/// `fence(SeqCst)` call sites in this file's `x1_emac_recv`, matching
/// the C's 2 `__sync_synchronize()` sites at lines 540/551).
///
/// # Safety
/// `sc`/`d` must be live; `d` must be the ring entry at `idx`.
unsafe fn rx_recycle(sc: *mut X1EmacSoftc, d: *mut x1_rx_desc, idx: usize) {
    // SAFETY: caller contract.
    unsafe {
        (*d).word1 = rx_desc_buf1_size(MBUF_SIZE) | DESC_CHAINED;
        // buf_addr2 (next-desc ptr) already set from init.
        fence(Ordering::SeqCst);
        (*d).word0 = RX_DESC_OWN;
        dma_cache_clean(d as *mut c_void, core::mem::size_of::<x1_rx_desc>());
        (*sc).rx_head = (idx as u32 + 1) % X1_EMAC_RX_RING_SIZE as u32;
    }
}

/// # Safety
/// `sc` must be live.
unsafe fn x1_emac_recv(sc: *mut X1EmacSoftc) {
    let mut budget = X1_EMAC_RX_BUDGET;

    loop {
        budget -= 1;
        if budget < 0 {
            break;
        }

        // SAFETY: caller contract.
        let idx = unsafe { (*sc).rx_head } as usize;
        // SAFETY: `idx < X1_EMAC_RX_RING_SIZE`.
        let d = unsafe { (*sc).rx_ring.add(idx) };

        // Invalidate descriptor to read from memory.
        // SAFETY: `d` a live ring entry.
        unsafe { dma_cache_inval(d as *mut c_void, core::mem::size_of::<x1_rx_desc>()) };

        // Is this descriptor owned by the CPU?
        // SAFETY: `d` live.
        let word0 = unsafe { (*d).word0 };
        if word0 & RX_DESC_OWN != 0 {
            break;
        }

        // Check for errors.
        let app_status = rx_desc_app_status(word0);
        if app_status & RX_FRAME_ERROR_MASK != 0 {
            // Error frame -- recycle the buffer.
            // SAFETY: `d`/`sc` live.
            unsafe { rx_recycle(sc, d, idx) };
            continue;
        }

        // Get frame length (includes FCS).
        let mut frame_len = rx_desc_frame_len(word0);
        if frame_len < 14 || frame_len > X1_EMAC_MAX_FRAME_SIZE + X1_EMAC_FCS_SIZE {
            // Error/oversize frame -- recycle the buffer.
            // SAFETY: `d`/`sc` live.
            unsafe { rx_recycle(sc, d, idx) };
            continue;
        }

        // Strip FCS (4 bytes).
        frame_len -= X1_EMAC_FCS_SIZE;

        // Invalidate the received data from cache.
        // SAFETY: caller contract.
        let m = unsafe { (*sc).rx_mbufs[idx] };
        // SAFETY: `m` live; `frame_len` bounded above.
        unsafe { dma_cache_inval((*m).head as *mut c_void, frame_len as usize) };

        // Set the mbuf length.
        // SAFETY: `m` live.
        unsafe { (*m).len = frame_len };

        // Deliver to network stack. `m` live, ownership transferred to
        // `net_rx` (matches the C original's contract -- `net_rx` is
        // responsible for `m`'s lifetime from here).
        net_rx(m);

        // Allocate a replacement mbuf. `mbufalloc` returns a fresh mbuf
        // or null.
        let mut newm = mbufalloc(0);
        if newm.is_null() {
            // SAFETY: caller contract; format string matches its one argument.
            crate::kprintln!("x1_emac{}: rx mbuf alloc failed", unsafe { (*sc).index });
            // Reuse the old mbuf (we already gave it to net_rx, so
            // allocate or we'll have a dangling pointer). In practice
            // this shouldn't happen on xv6.
            newm = mbufalloc(0);
            if newm.is_null() {
                panic!("x1_emac: out of mbufs");
            }
        }
        // SAFETY: caller contract; `newm` live.
        unsafe {
            (*sc).rx_mbufs[idx] = newm;
            dma_cache_clean((*newm).head as *mut c_void, MBUF_SIZE as usize);

            // Set up descriptor with new buffer -- chained mode preserved.
            (*d).buf_addr = (*newm).head as u64 as u32;
            (*d).word1 = rx_desc_buf1_size(MBUF_SIZE) | DESC_CHAINED;
            // buf_addr2 already contains next-descriptor pointer from init.
            fence(Ordering::SeqCst);
            (*d).word0 = RX_DESC_OWN;
            dma_cache_clean(d as *mut c_void, core::mem::size_of::<x1_rx_desc>());

            (*sc).rx_head = (idx as u32 + 1) % X1_EMAC_RX_RING_SIZE as u32;
        }
    }

    // If we hit the budget limit, there may be more packets pending.
    // Kick DMA poll to trigger another interrupt so we process the rest.
    if budget < 0 {
        // SAFETY: caller contract.
        unsafe { sc_wr(sc, DMA_RECEIVE_POLL_DEMAND, 0xFF) };
    }
}

// ===========================================================================
// TX reclaim (clean completed TX descriptors).
// ===========================================================================

/// # Safety
/// `sc` must be live.
unsafe fn x1_emac_tx_reclaim(sc: *mut X1EmacSoftc) {
    // SAFETY: caller contract.
    while unsafe { (*sc).tx_tail } != unsafe { (*sc).tx_head } {
        // SAFETY: caller contract.
        let idx = unsafe { (*sc).tx_tail } as usize;
        // SAFETY: `idx < X1_EMAC_TX_RING_SIZE`.
        let d = unsafe { (*sc).tx_ring.add(idx) };

        // SAFETY: `d` a live ring entry.
        unsafe { dma_cache_inval(d as *mut c_void, core::mem::size_of::<x1_tx_desc>()) };
        // SAFETY: `d` live.
        if unsafe { (*d).word0 } & TX_DESC_OWN != 0 {
            break; // still owned by DMA
        }

        // SAFETY: caller contract.
        unsafe {
            if !(*sc).tx_mbufs[idx].is_null() {
                mbuffree((*sc).tx_mbufs[idx]);
                (*sc).tx_mbufs[idx] = ptr::null_mut();
            }
            (*sc).tx_tail = (idx as u32 + 1) % X1_EMAC_TX_RING_SIZE as u32;
        }
    }
}

// ===========================================================================
// Start TX/RX DMA.
// ===========================================================================

/// # Safety
/// `sc` must be live, with the TX/RX rings already initialised.
unsafe fn x1_emac_start(sc: *mut X1EmacSoftc) {
    // SAFETY: caller contract for the whole function body below.
    unsafe {
        // Set TX base address.
        sc_wr(sc, DMA_TRANSMIT_BASE_ADDRESS, (*sc).tx_ring as u64 as u32);

        // Enable TX.
        let mut val = sc_rd(sc, MAC_TRANSMIT_CONTROL);
        val |= MAC_TC_TX_ENABLE | MAC_TC_TX_AUTO_RETRY;
        sc_wr(sc, MAC_TRANSMIT_CONTROL, val);

        // Disable auto-poll (we use poll demand on transmit).
        sc_wr(sc, DMA_TRANSMIT_AUTO_POLL_COUNTER, 0);

        // Start TX DMA.
        val = sc_rd(sc, DMA_CONTROL);
        val |= DMA_CTRL_START_TX;
        sc_wr(sc, DMA_CONTROL, val);

        // Set RX base address.
        sc_wr(sc, DMA_RECEIVE_BASE_ADDRESS, (*sc).rx_ring as u64 as u32);

        // Enable RX: receive + store-and-forward.
        val = sc_rd(sc, MAC_RECEIVE_CONTROL);
        val |= MAC_RC_RX_ENABLE | MAC_RC_STORE_FORWARD;
        sc_wr(sc, MAC_RECEIVE_CONTROL, val);

        // Start RX DMA.
        val = sc_rd(sc, DMA_CONTROL);
        val |= DMA_CTRL_START_RX;
        sc_wr(sc, DMA_CONTROL, val);

        // Enable DMA interrupts.
        val = DMA_IE_TX_DONE | DMA_IE_RX_DONE | DMA_IE_RX_DESC_UNAVAIL | DMA_IE_TX_STOPPED | DMA_IE_RX_STOPPED;
        sc_wr(sc, DMA_INTERRUPT_ENABLE, val);
    }
}

// ===========================================================================
// Instance initialisation.
// ===========================================================================

/// `x1_emac_init_one` -- initialise a single EMAC instance
/// (`idx` indexes `platform.emac[]`, 0 or 1). Returns `0` on success,
/// `-1` on failure.
///
/// # Safety
/// `idx` must be `< MAX_EMAC_INSTANCES`; must be called at most once per
/// `idx`, before any other function in this file can observe `emac_sc
/// (idx)` (single-hart-per-instance-kthread contract, see
/// [`x1_emac_kthread`]).
unsafe fn x1_emac_init_one(idx: usize) -> c_int {
    // SAFETY: caller contract.
    let sc = unsafe { emac_sc(idx) };

    // SAFETY: `sc` points at `size_of::<X1EmacSoftc>()` bytes of storage
    // this call exclusively owns (caller contract).
    unsafe { memset(sc as *mut c_void, 0, core::mem::size_of::<X1EmacSoftc>()) };
    // SAFETY: `sc` zeroed above, exclusively owned.
    unsafe {
        spin_init(&raw mut (*sc).lock, c"x1_emac".as_ptr());
        (*sc).index = idx as c_int;
        (*sc).irq = platform.emac[idx].irq as c_int;

        // Map EMAC registers (already identity-mapped by vm.c).
        (*sc).regs = platform.emac[idx].base as *mut u32;
        crate::kprintln!(
            "x1_emac{}: MMIO base 0x{:x}, IRQ {}",
            idx as c_int,
            platform.emac[idx].base as u64,
            platform.emac[idx].irq,
        );

        // Map APMU ctrl and dline registers.
        if platform.emac[idx].apmu_base != 0 && platform.emac[idx].ctrl_reg != 0 {
            (*sc).ctrl_reg = (platform.emac[idx].apmu_base as u64 + platform.emac[idx].ctrl_reg as u64) as *mut u32;
            crate::kprintln!(
                "x1_emac{}: ctrl_reg @ 0x{:x}",
                idx as c_int,
                platform.emac[idx].apmu_base as u64 + platform.emac[idx].ctrl_reg as u64,
            );
        }
        if platform.emac[idx].apmu_base != 0 && platform.emac[idx].dline_reg != 0 {
            (*sc).dline_reg = (platform.emac[idx].apmu_base as u64 + platform.emac[idx].dline_reg as u64) as *mut u32;
            crate::kprintln!(
                "x1_emac{}: dline_reg @ 0x{:x}",
                idx as c_int,
                platform.emac[idx].apmu_base as u64 + platform.emac[idx].dline_reg as u64,
            );
        }

        (*sc).tx_phase = platform.emac[idx].tx_phase;
        (*sc).rx_phase = platform.emac[idx].rx_phase;
        (*sc).reset_gpio = platform.emac[idx].reset_gpio as c_int;
        (*sc).phy_addr = -1; // auto-detect
    }

    // Configure APMU: RGMII mode + AXI single ID.
    // SAFETY: `sc` live, exclusively owned.
    unsafe { x1_emac_apmu_config(sc) };

    // Configure RGMII delay lines.
    // SAFETY: see above.
    unsafe { x1_emac_dline_config(sc) };

    // Init MAC hardware.
    // SAFETY: see above.
    unsafe { x1_emac_init_hw(sc) };

    // Init PHY via MDIO.
    // SAFETY: `(*sc).regs` a live EMAC MMIO base; `(*sc).phy_addr` == -1
    // requests auto-detect (see `yt8531::yt8531_init`).
    let phy_addr = unsafe { super::yt8531::yt8531_init((*sc).regs, (*sc).phy_addr) };
    if phy_addr < 0 {
        crate::kprintln!("x1_emac{}: PHY init failed", idx as c_int);
        return -1;
    }
    // SAFETY: `sc` live.
    unsafe { (*sc).phy_addr = phy_addr }; // use the auto-detected PHY address

    // Wait for auto-negotiation (up to 5 seconds).
    // SAFETY: `sc` live.
    if unsafe { super::yt8531::yt8531_wait_autoneg((*sc).regs, (*sc).phy_addr, &raw mut (*sc).phy, 5000) } == 0 {
        // SAFETY: format string matches its three arguments.
        crate::kprintln!(
            "x1_emac{}: link up {}Mbps {}-duplex",
            idx as c_int,
            unsafe { (*sc).phy.speed },
            if unsafe { (*sc).phy.full_duplex } != 0 { "full" } else { "half" },
        );
    } else {
        crate::kprintln!("x1_emac{}: no link", idx as c_int);
        // Continue anyway -- link may come up later.
        // SAFETY: `sc` live.
        unsafe {
            (*sc).phy.speed = 1000;
            (*sc).phy.full_duplex = 1;
        }
    }

    // Set MAC speed/duplex to match PHY.
    // SAFETY: `sc` live.
    unsafe { x1_emac_adjust_link(sc) };

    // Set MAC address for this instance. Generate a locally-administered
    // MAC based on instance index.
    let mac: [u8; 6] = [0x02, 0x00, 0x00, 0x00, 0x00, (idx as u8) + 1];
    // SAFETY: `sc` live; `mac` a live 6-byte array.
    unsafe { x1_emac_set_mac_addr(sc, mac.as_ptr()) };

    // Init TX ring.
    // SAFETY: `sc` live.
    if unsafe { x1_emac_tx_ring_init(sc) } < 0 {
        crate::kprintln!("x1_emac{}: TX ring init failed", idx as c_int);
        return -1;
    }

    // Init RX ring.
    // SAFETY: `sc` live.
    if unsafe { x1_emac_rx_ring_init(sc) } < 0 {
        crate::kprintln!("x1_emac{}: RX ring init failed", idx as c_int);
        return -1;
    }

    // Register IRQ handler.
    // SAFETY: caller contract; `emac_irq` is a live, fully-initialised
    // `IrqDesc` for the duration of this call.
    let ret = unsafe {
        let mut emac_irq = IrqDesc {
            handler: Some(x1_emac_intr),
            data: sc as *mut c_void,
            dev: ptr::null_mut(),
            irq: 0,
            count: 0,
            rcu_head: core::mem::zeroed(),
        };
        register_irq_handler(plic_irq((*sc).irq), &raw mut emac_irq)
    };
    if ret != 0 {
        crate::kprintln!("x1_emac{}: failed to register IRQ {}", idx as c_int, unsafe { (*sc).irq });
        return -1;
    }

    // Start DMA.
    // SAFETY: `sc` live, rings initialised above.
    unsafe { x1_emac_start(sc) };

    // Register with netdev layer.
    // SAFETY: `sc` live, exclusively owned.
    unsafe {
        (*sc).ndev.name[0] = b'e' as c_char;
        (*sc).ndev.name[1] = b't' as c_char;
        (*sc).ndev.name[2] = b'h' as c_char;
        (*sc).ndev.name[3] = b'0' as c_char + idx as c_char;
        (*sc).ndev.name[4] = 0;
        memmove((*sc).ndev.mac.as_mut_ptr() as *mut c_void, mac.as_ptr() as *const c_void, 6);
        (*sc).ndev.mtu = 1500;
        (*sc).ndev.link_up = (*sc).phy.link_up;
        (*sc).ndev.speed = (*sc).phy.speed;
        (*sc).ndev.full_duplex = (*sc).phy.full_duplex;
        (*sc).ndev.ops = &X1_EMAC_NETDEV_OPS as *const netdev_ops as *mut netdev_ops;
        (*sc).ndev.priv_ = sc as *mut c_void;
        netdev_register(&raw mut (*sc).ndev);

        crate::kprintln!(
            "x1_emac{}: initialised as {} (MAC {:x}:{:x}:{:x}:{:x}:{:x}:{:x})",
            idx as c_int,
            crate::printf::Cs((*sc).ndev.name.as_ptr()),
            ((mac[0] as c_int as i64) as u64),
            ((mac[1] as c_int as i64) as u64),
            ((mac[2] as c_int as i64) as u64),
            ((mac[3] as c_int as i64) as u64),
            ((mac[4] as c_int as i64) as u64),
            ((mac[5] as c_int as i64) as u64),
        );
    }

    EMAC_COUNT.fetch_add(1, Ordering::SeqCst);
    0
}

// ===========================================================================
// Top-level init -- spawns a kthread for post-init probing.
// ===========================================================================

/// Per-instance init kthread: initialises one EMAC and signals completion.
extern "C" fn x1_emac_init_kthread(idx: u64, _arg2: u64) {
    // SAFETY: `idx` is one of the `0..n` values `x1_emac_kthread` below
    // spawned this thread with, `n <= MAX_EMAC_INSTANCES`; each `idx` is
    // only ever handed to one kthread (see that function).
    let ret = unsafe { x1_emac_init_one(idx as usize) };
    EMAC_INIT_DONE[idx as usize].store(if ret < 0 { -1 } else { 1 }, Ordering::Release);
    if ret < 0 {
        crate::kprintln!("x1_emac{}: init failed, skipping", idx as c_int);
    }
}

// `is_err_or_null`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::is_err_or_null;

/// Main EMAC kthread: spawns per-instance init threads in parallel,
/// waits for all to complete, then enters the link-polling loop.
extern "C" fn x1_emac_kthread(_arg1: u64, _arg2: u64) {
    // SAFETY: `platform` is boot-time-populated, read-only from this
    // point on (fdt.rs's documented contract).
    let mut n = unsafe { platform.emac_count } as usize;
    if n > MAX_EMAC_INSTANCES {
        n = MAX_EMAC_INSTANCES;
    }

    crate::kprintln!("x1_emac: found {} instance(s)", n as c_int);

    // Spawn one init kthread per instance so they probe in parallel.
    for i in 0..n {
        let name: [u8; 6] = [b'e', b'm', b'a', b'c', b'0' + i as u8, 0];
        let t = kthread_create(
            name.as_ptr() as *const c_char,
            x1_emac_init_kthread as *mut c_void,
            i as u64,
            0,
            KERNEL_STACK_ORDER,
        );
        if is_err_or_null(t) {
            crate::kprintln!("x1_emac{}: failed to create init kthread", i as c_int);
            EMAC_INIT_DONE[i].store(-1, Ordering::Release);
        } else {
            wakeup(t);
        }
    }

    // Wait for all init threads to finish.
    for done in EMAC_INIT_DONE.iter().take(n) {
        while done.load(Ordering::Acquire) == 0 {
            sleep_ms(50);
        }
    }

    // ---- Link-state polling loop ----
    // PHY link-change interrupts are unreliable on many SoCs, so we poll
    // the PHY status register every 2 seconds. When the link state
    // changes we update the netdev and notify upper layers (e.g. lwIP)
    // via the link-change callback.
    loop {
        sleep_ms(2000);

        for i in 0..n {
            if EMAC_INIT_DONE[i].load(Ordering::Acquire) != 1 {
                continue; // init failed or incomplete
            }
            // SAFETY: `i < MAX_EMAC_INSTANCES`; `EMAC_INIT_DONE[i] == 1`
            // means `x1_emac_init_one(i)` completed successfully, so
            // `emac_sc(i)` is fully initialised.
            let sc = unsafe { emac_sc(i) };
            // SAFETY: `sc` live.
            if unsafe { (*sc).phy_addr } < 0 {
                continue; // PHY init failed for this instance
            }

            let mut ps: phy_state = phy_state { link_up: 0, speed: 0, full_duplex: 0 };
            // SAFETY: `sc` live; `ps` local and live.
            if unsafe { super::yt8531::yt8531_poll_link((*sc).regs, (*sc).phy_addr, &raw mut ps) } != 0 {
                continue;
            }

            // SAFETY: `sc` live.
            let old_link = unsafe { (*sc).ndev.link_up };
            let new_link = ps.link_up;

            if old_link != new_link {
                // SAFETY: `sc` live.
                unsafe {
                    (*sc).phy = ps;
                    (*sc).ndev.speed = ps.speed;
                    (*sc).ndev.full_duplex = ps.full_duplex;
                }

                if new_link != 0 {
                    // SAFETY: `sc` live.
                    unsafe { x1_emac_adjust_link(sc) };
                    // SAFETY: format string matches its three arguments.
                    crate::kprintln!(
                        "x1_emac{}: link up {}Mbps {}-duplex",
                        unsafe { (*sc).index },
                        ps.speed,
                        if ps.full_duplex != 0 { "full" } else { "half" },
                    );
                } else {
                    crate::kprintln!("x1_emac{}: link down", unsafe { (*sc).index });
                }

                // SAFETY: `sc` live.
                unsafe { netdev_set_link(&raw mut (*sc).ndev, new_link) };
            }
        }
    }
}

/// `void x1_emac_init(void)` -- schedule EMAC probing as a post-init
/// kthread. Called from `start_kernel`. The actual hardware probing, PHY
/// init, and auto-negotiation run in a dedicated kernel thread so that:
/// 1. The scheduler is already running -> `sleep_ms()` works.
/// 2. Boot proceeds without blocking on slow PHY negotiation.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn x1_emac_init() {
    // SAFETY: `platform` is boot-time-populated by `fdt.rs` before
    // `start_kernel.c` calls this (documented boot-order contract).
    if !(unsafe { platform.has_emac } != 0) {
        return;
    }

    let t = kthread_create(c"x1_emac".as_ptr(), x1_emac_kthread as *mut c_void, 0, 0, KERNEL_STACK_ORDER);
    if is_err_or_null(t) {
        crate::kprintln!("x1_emac: failed to create init kthread");
        return;
    }
    wakeup(t);
}
