//! Intel e1000 (82540EP/EM) Ethernet controller driver -- Rust port of
//! `kernel/e1000.c` (Phase 2 Wave 28, sub-wave B -- see
//! `docs/rustify/phase2_plan.md`; the final porting wave's network
//! drivers). Registered with the netdev abstraction layer
//! (`kernel/dev/netdev.rs`, Wave 24); driven by `kernel/pci.rs`'s BAR
//! probe (`pci_init` finds the qemu e1000 PCI function, programs BAR0,
//! calls [`e1000_init`]).
//!
//! # MMIO -- volatile accounting
//!
//! The C original has **7** `volatile`-qualified declaration sites (per
//! the plan's §1 table): the `regs` pointer itself (`STATIC volatile
//! uint32 *regs;`, the one hardware-register-bearing site) plus three
//! `volatile uint32 l, h;` local-variable pairs in
//! [`e1000_set_rcvaddr`]/[`e1000_set_transmission_descriptor_base`]/
//! [`e1000_set_receive_descriptor_base`]. The latter six are `volatile`-
//! qualified *stack locals*, not MMIO -- `volatile` on a plain
//! non-pointer, non-memory-mapped local has no hardware effect (the
//! compiler still owns the storage; only *access through a volatile
//! pointer/reference* is meaningful), so this port narrows those six to
//! plain `u32` locals, documented here as the one deliberate
//! simplification in this file. The single real hazard -- every access
//! through `regs` -- is preserved exactly via [`reg_read`]/[`reg_write`]
//! ([`core::ptr::read_volatile`]/[`write_volatile`]), used at every
//! register touch point below, matching the C `volatile uint32 *`
//! semantics 1:1.
//!
//! # DMA descriptor layout (hardware ABI)
//!
//! [`tx_desc`]/[`rx_desc`] resolve through the unchanged
//! `crate::bindings` facade paths to the NATIVE [`TxDesc`]/[`RxDesc`]
//! below (P3-4c; they were real `bindgen` types from
//! `kernel/inc/dev/e1000_dev.h` when this port landed, anchored
//! `^tx_desc$|^rx_desc$` to avoid matching the unrelated
//! `x1_tx_desc`/`x1_rx_desc`) -- the e1000's DMA
//! engine reads/writes these 16-byte entries directly, same rationale as
//! every other hardware-ABI struct this wave. [`layout_asserts`] pins
//! `size_of`/`offset_of` for both at compile time. [`TxRing`]/[`RxRing`]
//! keep the C's `__ALIGNED(16)` array attribute via a
//! `#[repr(C, align(16))]` wrapper newtype (Rust has no direct
//! `#[repr(align(N))]` on a `static` item, only on a type).
//!
//! # Barrier accounting
//!
//! Exactly **8** `__atomic_thread_fence(__ATOMIC_SEQ_CST)` sites in the
//! C original (a distinct count from the 7 `volatile` sites above --
//! two independent metrics): 1 in [`e1000_dev_reset`] (after the reset
//! sequence), 3 in [`e1000_set_rcvaddr`] (before the first `E1000_RA`
//! write, between the two `E1000_RA` writes, after the second), 2 in
//! [`e1000_set_transmission_descriptor_base`] (before/after the
//! `TDBAL`/`TDBAH`/`TDLEN`/`TDH`/`TDT` write group), 2 in
//! [`e1000_set_receive_descriptor_base`] (same shape). Every one
//! preserved below as [`core::sync::atomic::fence`]`(Ordering::SeqCst)`
//! at the identical call site.
//!
//! # Preserved-as-is (documented, not fixed -- matches this crate's
//! established practice for line-faithful ports)
//!
//! - [`e1000_transmit`]'s ring-index bound check is `index >
//!   TX_RING_SIZE`, not `>=` -- off-by-one in the C original that would
//!   let `index == TX_RING_SIZE` (one past the last valid descriptor)
//!   through to the `tx_ring[index]` dereference. In practice `E1000_TDT`
//!   is only ever written by this same file, always via `(regs[TDT]+1) %
//!   TX_RING_SIZE`, so the hardware register can never actually reach
//!   `TX_RING_SIZE` -- the C's own `>` bound is unreachable dead code,
//!   not a live bug, but ported verbatim (not silently upgraded to `>=`)
//!   per this wave's line-fidelity charter.
//! - [`e1000_recv`]'s `newbuf = Mbuf::alloc(0)` return is never null-
//!   checked before `newbuf->head` is dereferenced two lines later (an
//!   OOM would null-deref) -- same shape as the C original, which has
//!   the identical unchecked dereference. Preserved rather than
//!   "fixed" -- widening error handling here is out of this wave's
//!   scope (mirrors this crate's `timer_node_init`/`bio_release`
//!   documented-preserved-bug precedents).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::ptr;
use core::sync::atomic::{fence, Ordering};

use crate::bindings::{mbuf, netdev, rx_desc, tx_desc};
use crate::dev::netdev::NetdevOps;
use crate::irq::irq_core::{IrqCore, IrqDesc};
use crate::sync::SpinLock;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::Printf;

unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.

    // string.rs.
    fn strncpy(s: *mut c_char, t: *const c_char, n: usize) -> *mut c_char;
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
}
// P3-1D mesh sweep: net.rs/dev/netdev.rs are in scope for this wave;
// signatures are identical, so these become plain crate-path imports
// instead of `extern "C"` redeclarations.
use crate::net::{Mbuf, Net};
use crate::dev::netdev::Netdev;

// ===========================================================================
// Register offsets/bits -- redeclared locally from `kernel/inc/dev/
// e1000_dev.h` per this crate's established convention.
// ===========================================================================

/* Registers (word indices -- the header pre-divides byte offsets by 4).
 * `E1000_CTL` is `0x00000 / 4 == 0`, written as a literal `0` (rather
 * than `0x00000 / 4`) to satisfy clippy's `erasing_op` lint (deny-by-
 * default in this crate) -- every other offset below keeps the `/ 4`
 * form for direct diffability against the header's own `(byte_offset /
 * 4)` macros, since none of the others divides a zero numerator. */
const E1000_CTL: usize = 0;
const E1000_ICR: usize = 0x000C0 / 4;
const E1000_IMS: usize = 0x000D0 / 4;
const E1000_RCTL: usize = 0x00100 / 4;
const E1000_TCTL: usize = 0x00400 / 4;
const E1000_TIPG: usize = 0x00410 / 4;
const E1000_RDBAL: usize = 0x02800 / 4;
const E1000_RDBAH: usize = 0x02804 / 4;
const E1000_RDTR: usize = 0x02820 / 4;
const E1000_RADV: usize = 0x0282C / 4;
const E1000_RDH: usize = 0x02810 / 4;
const E1000_RDT: usize = 0x02818 / 4;
const E1000_RDLEN: usize = 0x02808 / 4;
const E1000_TDBAL: usize = 0x03800 / 4;
const E1000_TDBAH: usize = 0x03804 / 4;
const E1000_TDLEN: usize = 0x03808 / 4;
const E1000_TDH: usize = 0x03810 / 4;
const E1000_TDT: usize = 0x03818 / 4;
const E1000_MTA: usize = 0x05200 / 4;
const E1000_RA: usize = 0x05400 / 4;

/* Device Control. */
const E1000_CTL_RST: u32 = 0x04000000;

/* Transmit Control. */
const E1000_TCTL_EN: u32 = 0x00000002;
const E1000_TCTL_PSP: u32 = 0x00000008;
const E1000_TCTL_CT_SHIFT: u32 = 4;
const E1000_TCTL_COLD_SHIFT: u32 = 12;

/* Receive Control. */
const E1000_RCTL_EN: u32 = 0x00000002;
const E1000_RCTL_BAM: u32 = 0x00008000;
const E1000_RCTL_SZ_2048: u32 = 0x00000000;
const E1000_RCTL_SECRC: u32 = 0x04000000;

/* Transmit Descriptor command/status [E1000 3.3.3]. */
const E1000_TXD_CMD_EOP: u8 = 0x01;
const E1000_TXD_CMD_RS: u8 = 0x08;
const E1000_TXD_STAT_DD: u8 = 0x01;

/* Receive Descriptor status [E1000 3.2.3.1]. */
const E1000_RXD_STAT_DD: u8 = 0x01;

const TX_RING_SIZE: usize = 16;
const RX_RING_SIZE: usize = 16;

/// `kernel/inc/dev/netdev.h`: `#define NETDEV_NAME_MAX 16`.
const NETDEV_NAME_MAX: usize = 16;

// ===========================================================================
// Native `tx_desc`/`rx_desc` — P3-4c nativization (user directive:
// remove the C-compatible interfaces; DMA-exactness scrutiny class).
// `TxDesc`/`RxDesc` are the canonical definitions of
// `kernel/inc/dev/e1000_dev.h`'s `struct tx_desc`/`struct rx_desc`
// ([E1000 3.3.3]/[E1000 3.2.3]): `build.rs` blocklists the bindgen
// emissions (anchored `^tx_desc$|^rx_desc$`, matching the allowlist's
// own spelling — must not swallow `x1_tx_desc`/`x1_rx_desc`) and
// re-exports these types as `crate::bindings::tx_desc`/`rx_desc`
// (facade `pub use`, N2 pattern), so this file's ring types
// (`TxRing`/`RxRing`), `ZERO_*_DESC` field-literal constructors, and
// every pointer site resolve right back here.
//
// *** DEVICE-READ DMA LAYOUT — HANDLE WITH P3-4 SCRUTINY *** The e1000
// hardware itself walks these rings (TDBAL/RDBAL point at them); every
// field offset is the E1000 manual's wire format. Any drift is silent
// packet corruption. The driver's volatile/fence discipline (Wave-28
// exactness) is UNTOUCHED — only the type-definition source changes.
//
// DERIVE DECISIONS (P3-4c): Copy + Clone on both, exactly as the
// pre-nativization bindgen output derived (plain-int PODs).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + toolchain-gcc `_Static_assert` probe (rv64gc/lp64d —
// scratchpad p3_4c_dma_probe.c); both agree on every value asserted
// below.
// ===========================================================================

/// Native `struct tx_desc` (`kernel/inc/dev/e1000_dev.h`) — one e1000
/// legacy transmit descriptor [E1000 3.3.3].
#[repr(C)]
#[derive(Copy, Clone)]
pub struct TxDesc {
    /// Buffer physical address.
    pub addr: crate::bindings::uint64,
    /// Buffer length.
    pub length: crate::bindings::uint16,
    /// Checksum offset.
    pub cso: crate::bindings::uint8,
    /// Command byte (`E1000_TXD_CMD_*`).
    pub cmd: crate::bindings::uint8,
    /// Status byte (`E1000_TXD_STAT_DD`).
    pub status: crate::bindings::uint8,
    /// Checksum start.
    pub css: crate::bindings::uint8,
    pub special: crate::bindings::uint16,
}

/// Native `struct rx_desc` (`kernel/inc/dev/e1000_dev.h`) — one e1000
/// legacy receive descriptor [E1000 3.2.3].
#[repr(C)]
#[derive(Copy, Clone)]
pub struct RxDesc {
    /// Address of the descriptor's data buffer.
    pub addr: crate::bindings::uint64,
    /// Length of data DMAed into the data buffer.
    pub length: crate::bindings::uint16,
    /// Packet checksum.
    pub csum: crate::bindings::uint16,
    /// Descriptor status (`E1000_RXD_STAT_*`).
    pub status: crate::bindings::uint8,
    /// Descriptor errors.
    pub errors: crate::bindings::uint8,
    pub special: crate::bindings::uint16,
}

// P3-4c hardcoded layout proof — the e1000 wire format
// (`kernel/inc/dev/e1000_dev.h`), every field. Values captured from
// the pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the toolchain-gcc probe.
const _: () = {
    use core::mem::{align_of, offset_of, size_of};
    assert!(size_of::<TxDesc>() == 16, "tx_desc size (DEVICE-READ)");
    assert!(align_of::<TxDesc>() == 8, "tx_desc alignment");
    assert!(offset_of!(TxDesc, addr) == 0, "tx.addr offset (DEVICE-READ)");
    assert!(offset_of!(TxDesc, length) == 8, "tx.length offset (DEVICE-READ)");
    assert!(offset_of!(TxDesc, cso) == 10, "tx.cso offset (DEVICE-READ)");
    assert!(offset_of!(TxDesc, cmd) == 11, "tx.cmd offset (DEVICE-READ)");
    assert!(offset_of!(TxDesc, status) == 12, "tx.status offset (DEVICE-READ)");
    assert!(offset_of!(TxDesc, css) == 13, "tx.css offset (DEVICE-READ)");
    assert!(offset_of!(TxDesc, special) == 14, "tx.special offset (DEVICE-READ)");

    assert!(size_of::<RxDesc>() == 16, "rx_desc size (DEVICE-READ)");
    assert!(align_of::<RxDesc>() == 8, "rx_desc alignment");
    assert!(offset_of!(RxDesc, addr) == 0, "rx.addr offset (DEVICE-READ)");
    assert!(offset_of!(RxDesc, length) == 8, "rx.length offset (DEVICE-READ)");
    assert!(offset_of!(RxDesc, csum) == 10, "rx.csum offset (DEVICE-READ)");
    assert!(offset_of!(RxDesc, status) == 12, "rx.status offset (DEVICE-READ)");
    assert!(offset_of!(RxDesc, errors) == 13, "rx.errors offset (DEVICE-READ)");
    assert!(offset_of!(RxDesc, special) == 14, "rx.special offset (DEVICE-READ)");
};

// ===========================================================================
// DMA layout asserts (hardware ABI -- see module doc). Kept verbatim
// from the Wave-28 port (now redundant with the full P3-4c block above,
// but they are the port's own documented invariants).
// ===========================================================================
#[allow(dead_code)]
mod layout_asserts {
    use super::*;
    const _SIZE_TX: () = assert!(core::mem::size_of::<tx_desc>() == 16);
    const _SIZE_RX: () = assert!(core::mem::size_of::<rx_desc>() == 16);
    const _OFF_TX_LENGTH: () = assert!(core::mem::offset_of!(tx_desc, length) == 8);
    const _OFF_TX_STATUS: () = assert!(core::mem::offset_of!(tx_desc, status) == 12);
    const _OFF_RX_LENGTH: () = assert!(core::mem::offset_of!(rx_desc, length) == 8);
    const _OFF_RX_STATUS: () = assert!(core::mem::offset_of!(rx_desc, status) == 12);
}

// ===========================================================================
// Platform-probed MMIO base/IRQ.
// ===========================================================================

// P3-1D mesh sweep: caller (`mm/vm_pgtab.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) static mut __e1000_pci_mmio_base: u64 = 0x40000000;
// P3-1D mesh sweep: no caller anywhere outside this file -- demoted.
pub(crate) static mut __e1000_pci_irqno: u64 = 33;

// ===========================================================================
// Global driver state -- mirrors the C file's plain `STATIC`/file-scope
// globals 1:1 (all BSS-resident, zero-initialised, matching the C
// static-storage-duration originals).
// ===========================================================================

/// Mirrors `struct tx_desc tx_ring[TX_RING_SIZE] __ALIGNED(16);` -- the
/// wrapper newtype carries the `align(16)` the C attribute requested
/// (Rust has no `#[repr(align(N))]` directly on a `static` item).
#[repr(C, align(16))]
struct TxRing([tx_desc; TX_RING_SIZE]);
/// Mirrors `struct rx_desc rx_ring[RX_RING_SIZE] __ALIGNED(16);`.
#[repr(C, align(16))]
struct RxRing([rx_desc; RX_RING_SIZE]);

const ZERO_TX_DESC: tx_desc = tx_desc { addr: 0, length: 0, cso: 0, cmd: 0, status: 0, css: 0, special: 0 };
const ZERO_RX_DESC: rx_desc = rx_desc { addr: 0, length: 0, csum: 0, status: 0, errors: 0, special: 0 };

/// Mirrors `struct tx_desc tx_ring[TX_RING_SIZE] __ALIGNED(16);` plus
/// `static struct mbuf *tx_mbufs[TX_RING_SIZE];` -- both were guarded by
/// `e1000_lock` (a bare `spinlock_t`, manually paired `spin_lock`/
/// `spin_unlock`) as two separate `static mut`s. Wave P3-8d: the lock
/// now owns the data it protects directly (`crate::sync::SpinLock`, same
/// P3-8b/8c precedent as `ramdisk.rs`'s `RAMDISK`/`bufcache.rs`'s
/// `BCACHE`), so [`e1000_transmit`]'s early-return-while-holding exit
/// path releases the lock for free via RAII instead of a hand-paired
/// `spin_unlock` before the `return`.
struct TxState {
    ring: TxRing,
    mbufs: [*mut mbuf; TX_RING_SIZE],
}

/// `spinlock_t e1000_lock = SPINLOCK_INITIALIZED("e1000_lock");` --
/// compile-time initialised, valid for locking from the moment
/// `.bss`/`.data` are live, no runtime `spin_init` call required (same
/// convention as `kernel/uart.rs`'s `UART_TX_LOCK`/`UART_RX_LOCK`).
static TX: SpinLock<TxState> = SpinLock::new(
    c"e1000_lock",
    TxState { ring: TxRing([ZERO_TX_DESC; TX_RING_SIZE]), mbufs: [ptr::null_mut(); TX_RING_SIZE] },
);

/// Mirrors `struct rx_desc rx_ring[RX_RING_SIZE] __ALIGNED(16);` plus
/// `static struct mbuf *rx_mbufs[RX_RING_SIZE];`. Unlike the TX ring,
/// these are **not** behind a lock in the C original either: they are
/// only ever touched from [`e1000_recv`], itself only ever called from
/// [`e1000_intr`] (the PLIC interrupt handler) -- a single-consumer
/// context with no concurrent caller, so no lock was ever needed here
/// (matches classic xv6's identical rx-path lock-freedom). Left as
/// plain `static mut`s -- not a SpinLock<T> candidate (nothing to
/// serialize against).
static mut RX_RING: RxRing = RxRing([ZERO_RX_DESC; RX_RING_SIZE]);
static mut RX_MBUFS: [*mut mbuf; RX_RING_SIZE] = [ptr::null_mut(); RX_RING_SIZE];

/// Mirrors `STATIC volatile uint32 *regs;` -- remembers where the
/// e1000's registers live. Set exactly once, in [`e1000_init`], before
/// any other function in this file runs (registration order:
/// `pci_init` finds the device, programs BAR0, then calls
/// `e1000_init` -- no concurrent access is possible before that call
/// returns and `netdev_register` publishes this driver).
static mut REGS: *mut u32 = ptr::null_mut();

static mut E1000_NDEV: netdev = netdev {
    name: [0; NETDEV_NAME_MAX],
    mac: [0; 6],
    ip: 0,
    mtu: 0,
    link_up: 0,
    speed: 0,
    full_duplex: 0,
    index: 0,
    ops: None,
    priv_: ptr::null_mut(),
    next: ptr::null_mut(),
    link_cb: None,
};

/// Zero-sized driver-state type for the e1000 (P3-10c precedent's ZST
/// shape) -- KERNEL-OO home for this file's free fns. Raw params kept,
/// NO `&self` receiver: `E1000`'s state (`REGS`/`TX`/the RX ring
/// globals) is `Freeze` and IRQ-shared cross-hart (see module doc's
/// "Barrier accounting") -- a `&self` would let LLVM hoist a read out
/// of a poll loop.
pub(crate) struct E1000;

impl E1000 {
/// `#define R(n, r)`-equivalent word-indexed MMIO access -- see module
/// doc's "MMIO -- volatile accounting".
///
/// # Safety
/// `REGS` must have been set by [`E1000::init`]; `offset` must be a
/// valid e1000 register word-index.
#[inline(always)]
unsafe fn reg_read(offset: usize) -> u32 {
    // SAFETY: caller contract.
    unsafe { core::ptr::read_volatile(REGS.add(offset)) }
}
/// # Safety
/// Same as [`Self::reg_read`].
#[inline(always)]
unsafe fn reg_write(offset: usize, val: u32) {
    // SAFETY: caller contract.
    unsafe { core::ptr::write_volatile(REGS.add(offset), val) }
}

#[cold]
#[inline(never)]
fn panic_fixed(msg: &core::ffi::CStr) -> ! {
    Printf::__panic_start();
    crate::kprint!("{}", crate::printf::Cs(msg.as_ptr()));
    Printf::__panic_end()
}

// ===========================================================================
// Device programming.
// ===========================================================================

/// Full reset of the device. Called by [`e1000_init`]. `REGS` must
/// already be set.
///
/// # Safety
/// `REGS` must be set (see [`reg_read`]/[`reg_write`]).
unsafe fn dev_reset() {
    // SAFETY: caller contract.
    unsafe {
        Self::reg_write(E1000_IMS, 0); // disable interrupts
        let ctl = Self::reg_read(E1000_CTL);
        Self::reg_write(E1000_CTL, ctl | E1000_CTL_RST);
        Self::reg_write(E1000_IMS, 0); // redisable interrupts
    }
    fence(Ordering::SeqCst);
}

/// Set receive MAC address: adds `mac` to the e1000's address filter
/// table at `index` (`< 16`). `as_type`: `0` = destination address
/// (required for normal mode), `1` = source address. Returns `0` on
/// success, `-1` on error.
///
/// # Safety
/// `mac` must be valid for 6 bytes; `REGS` must be set.
unsafe fn set_rcvaddr(mac: *const u8, as_type: u8, valid: bool, index: i32) -> i32 {
    if index >= 16 || index < 0 {
        return -1; // the receive address array of e1000 has < 16 entries
    }
    // SAFETY: caller contract (`mac` valid for 6 bytes); unaligned reads
    // match the C's `*(uint32*)mac`/`*(uint16*)&mac[4]` pointer-cast
    // dereferences (an unaligned access on RISC-V, worked in practice in
    // the C original -- `read_unaligned` gives the same bytes without
    // the alignment-UB the C cast technically carried).
    let l = unsafe { core::ptr::read_unaligned(mac as *const u32) };
    let mut h = unsafe { core::ptr::read_unaligned(mac.add(4) as *const u16) } as u32 | ((as_type as u32) << 16);
    if valid {
        h |= 1 << 31;
    }
    fence(Ordering::SeqCst);
    // SAFETY: caller contract; `index` bounds-checked above.
    unsafe { Self::reg_write(E1000_RA + 2 * index as usize, l) };
    fence(Ordering::SeqCst);
    // SAFETY: same as above.
    unsafe { Self::reg_write(E1000_RA + 2 * index as usize + 1, h) };
    fence(Ordering::SeqCst);
    0
}

/// Initialise the transmit descriptor circular buffer. `physical_base`
/// must be 16-byte aligned; `size` must be a multiple of 128 bytes and
/// equal `mbufs_ptr_arr_base`'s element count times `size_of::<tx_desc>()`.
/// Returns `0` on success, `-1` on error.
///
/// # Safety
/// `virtual_base`/`mbufs_ptr_arr_base` must be valid for `size /
/// size_of::<tx_desc>()` entries; `REGS` must be set.
unsafe fn set_transmission_descriptor_base(
    virtual_base: *mut tx_desc,
    physical_base: u64,
    mbufs_ptr_arr_base: *mut *mut mbuf,
    size: u32,
) -> i32 {
    let rd_arr_size = size as usize / core::mem::size_of::<tx_desc>();

    if physical_base == 0 || virtual_base.is_null() {
        return -1; // null address
    }
    if physical_base & 15 != 0 {
        return -1; // TD base address must be 16 bytes aligned
    }
    if size % 128 != 0 {
        return -1; // the size of TD must be a multiple of 128 bytes
    }

    // SAFETY: caller contract; `virtual_base`/`mbufs_ptr_arr_base` valid
    // for `rd_arr_size` entries.
    unsafe {
        for i in 0..rd_arr_size {
            (*virtual_base.add(i)) = ZERO_TX_DESC;
            (*virtual_base.add(i)).status = E1000_TXD_STAT_DD;
            *mbufs_ptr_arr_base.add(i) = ptr::null_mut();
        }
    }

    let l = (physical_base << 32) >> 32;
    let h = physical_base >> 32;
    fence(Ordering::SeqCst);
    // SAFETY: caller contract.
    unsafe {
        Self::reg_write(E1000_TDBAL, l as u32);
        Self::reg_write(E1000_TDBAH, h as u32);
        Self::reg_write(E1000_TDLEN, size);
        Self::reg_write(E1000_TDH, 0);
        Self::reg_write(E1000_TDT, 0);
    }
    fence(Ordering::SeqCst);
    0
}

/// Initialise the receive descriptor circular buffer. Same contract as
/// [`e1000_set_transmission_descriptor_base`], and additionally
/// allocates an `mbuf` per entry via `Mbuf::alloc(0)`.
///
/// # Safety
/// Same as [`e1000_set_transmission_descriptor_base`].
unsafe fn set_receive_descriptor_base(
    virtual_base: *mut rx_desc,
    physical_base: u64,
    mbufs_ptr_arr_base: *mut *mut mbuf,
    size: u32,
) -> i32 {
    let rd_arr_size = size as usize / core::mem::size_of::<rx_desc>();

    if physical_base == 0 || virtual_base.is_null() {
        return -1; // null address
    }
    if physical_base & 15 != 0 {
        return -1; // RD base address must be 16 bytes aligned
    }
    if size % 128 != 0 {
        return -1; // the size of RD must be a multiple of 128 bytes
    }

    let l = (physical_base << 32) >> 32;
    let h = physical_base >> 32;

    // SAFETY: caller contract; `virtual_base`/`mbufs_ptr_arr_base` valid
    // for `rd_arr_size` entries.
    unsafe {
        for i in 0..rd_arr_size {
            *virtual_base.add(i) = ZERO_RX_DESC;
            let mb = Mbuf::alloc(0);
            *mbufs_ptr_arr_base.add(i) = mb;
            if mb.is_null() {
                return -1;
            }
            (*virtual_base.add(i)).addr = (*mb).head as u64;
        }
    }
    fence(Ordering::SeqCst);
    // SAFETY: caller contract.
    unsafe {
        Self::reg_write(E1000_RDBAL, l as u32);
        Self::reg_write(E1000_RDBAH, h as u32);
        Self::reg_write(E1000_RDLEN, size);
        Self::reg_write(E1000_RDH, 0);
        Self::reg_write(E1000_RDT, (rd_arr_size - 1) as u32);
    }
    fence(Ordering::SeqCst);
    0
}

/// Called by [`crate::pci::pci_init`]. `xregs` is the memory address at
/// which the e1000's registers are mapped.
// P3-1D mesh sweep: caller (`pci.rs`) reaches this via a crate-path
// `use` of `E1000::init` (not an `extern` redeclaration).
pub(crate) extern "C" fn init(xregs: *mut u32) {
    let default_mac_address: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];

    // SAFETY: single-threaded boot-time init: `pci_init` (this driver's
    // only caller) runs during sequential, single-hart boot-time PCI
    // enumeration, strictly before `netdev_register` (at the end of this
    // function) ever publishes this driver to a concurrent reader.
    unsafe { REGS = xregs };

    // SAFETY: `e1000_irq` is a live, fully-initialised `IrqDesc` for the
    // duration of this call.
    let ret = unsafe {
        let mut e1000_irq =
            IrqDesc { handler: Some(&E1000_IRQ_HANDLER), data: ptr::null_mut(), dev: ptr::null_mut(), irq: 0, count: 0, rcu_head: core::mem::zeroed() };
        IrqCore::register_irq_handler(IrqCore::plic_irq(__e1000_pci_irqno as c_int), &raw mut e1000_irq)
    };
    if ret != 0 {
        Self::panic_fixed(c"e1000_init: failed to register irq handler");
    }

    // Reset the device.
    // SAFETY: `REGS` just set above.
    unsafe { Self::dev_reset() };

    // [E1000 14.5] Transmit initialization.
    // Single-threaded boot-time init (same as `REGS` above) -- taking
    // `TX`'s lock here isn't required for exclusivity, but keeps the
    // lock/unlock discipline uniform with every later `e1000_transmit`
    // access (same precedent as `ramdisk.rs::ramdisk_init`'s identical
    // `RAMDISK.lock()` call before publication).
    let (tx_ring_ptr, tx_mbufs_ptr) = {
        let mut tx = TX.lock();
        (&raw mut tx.ring.0 as *mut tx_desc, &raw mut tx.mbufs as *mut *mut mbuf)
    };
    // SAFETY: `tx_ring_ptr`/`tx_mbufs_ptr` point into `TX`'s `'static`
    // storage (valid for the process lifetime regardless of the guard
    // above having since dropped); still single-threaded init, so no
    // concurrent access is possible here.
    if unsafe {
        Self::set_transmission_descriptor_base(
            tx_ring_ptr,
            tx_ring_ptr as u64,
            tx_mbufs_ptr,
            (TX_RING_SIZE * core::mem::size_of::<tx_desc>()) as u32,
        )
    } != 0
    {
        Self::panic_fixed(c"e1000");
    }

    // [E1000 14.4] Receive initialization.
    // SAFETY: same as the TX ring above.
    let (rx_ring_ptr, rx_mbufs_ptr) = unsafe { (&raw mut RX_RING.0 as *mut rx_desc, &raw mut RX_MBUFS as *mut *mut mbuf) };
    // SAFETY: same as the TX ring above.
    if unsafe {
        Self::set_receive_descriptor_base(
            rx_ring_ptr,
            rx_ring_ptr as u64,
            rx_mbufs_ptr,
            (RX_RING_SIZE * core::mem::size_of::<rx_desc>()) as u32,
        )
    } != 0
    {
        Self::panic_fixed(c"e1000");
    }

    // Filter by qemu's MAC address, 52:54:00:12:34:56.
    // SAFETY: `default_mac_address` valid for 6 bytes; `REGS` set.
    if unsafe { Self::set_rcvaddr(default_mac_address.as_ptr(), 0, true, 0) } != 0 {
        Self::panic_fixed(c"e1000_init: MAC address");
    }

    // SAFETY: `REGS` set; every write below is a plain MMIO register write.
    unsafe {
        // Multicast table.
        for i in 0..(4096 / 32) {
            Self::reg_write(E1000_MTA + i, 0);
        }

        // Transmitter control bits.
        Self::reg_write(
            E1000_TCTL,
            E1000_TCTL_EN // enable
                | E1000_TCTL_PSP // pad short packets
                | (0x0F << E1000_TCTL_CT_SHIFT) // max retransmissions on collision
                | (0x40 << E1000_TCTL_COLD_SHIFT), // collision distance
        );
        Self::reg_write(E1000_TIPG, 10 | (8 << 10) | (6 << 20)); // inter-pkt gap

        // Receiver control bits.
        Self::reg_write(
            E1000_RCTL,
            E1000_RCTL_EN // enable receiver
                | E1000_RCTL_BAM // enable broadcast
                | E1000_RCTL_SZ_2048 // 2048-byte rx buffers
                | E1000_RCTL_SECRC, // strip CRC
        );

        // Ask e1000 for receive interrupts. Instead of RDTR/RADV, use
        // the Interrupt Throttling Register (ITR) if a delay is needed.
        Self::reg_write(E1000_RDTR, 0); // interrupt after every received packet (no timer)
        Self::reg_write(E1000_RADV, 0); // interrupt after every packet (no timer)
        Self::reg_write(E1000_IMS, 1 << 7); // RXDW -- Receiver Descriptor Write Back

        // Register with the netdev abstraction layer.
        strncpy(E1000_NDEV.name.as_mut_ptr(), c"e1000".as_ptr(), NETDEV_NAME_MAX);
        memmove(E1000_NDEV.mac.as_mut_ptr() as *mut c_void, default_mac_address.as_ptr() as *const c_void, 6);
        E1000_NDEV.mtu = 1500;
        E1000_NDEV.link_up = 1;
        E1000_NDEV.speed = 1000;
        E1000_NDEV.full_duplex = 1;
        E1000_NDEV.ops = Some(&E1000_NETDEV_OPS);
        E1000_NDEV.priv_ = REGS as *mut c_void;
        Netdev::register(&raw mut E1000_NDEV);
    }
}

/// The mbuf contains an ethernet frame; program it into the TX
/// descriptor ring so the e1000 sends it. Stashes a pointer so it can
/// be freed after sending. Returns `0` on success, `-1` if the ring is
/// full (the next descriptor hasn't finished transmitting yet).
// P3-1D mesh sweep: called by name from `E1000NetdevOps::transmit`
// below (the actual `NetdevOps` trait-object dispatch target -- see
// that impl's own doc comment), so this one is a plain crate-path item,
// not itself address-taken -- convertible to an `E1000` associated fn.
// `extern "C"` (the calling-convention marker) is kept for ABI parity
// even though nothing takes this particular fn's address directly.
pub(crate) extern "C" fn transmit(m: *mut mbuf) -> c_int {
    // `TX.lock()` (RAII): the sole early-return-while-holding exit path
    // below now releases for free when `tx` drops, instead of a
    // hand-paired `spin_unlock` before the `return` (Wave P3-8d; matches
    // `kernel/ramdisk.rs`'s identical `submit_bio` upgrade).
    let mut tx = TX.lock();

    // Get the current tail pointer of the transmission ring buffer.
    // SAFETY: `REGS` set by `e1000_init`, which always runs before any
    // netdev consumer can reach this function (publication order via
    // `netdev_register`).
    let index = unsafe { Self::reg_read(E1000_TDT) };
    if index > TX_RING_SIZE as u32 {
        Self::panic_fixed(c"e1000 transmit: ring overflow");
    }
    // SAFETY: `index <= TX_RING_SIZE` (see module doc's "Preserved-as-
    // is" note for the C's own off-by-one-tolerant bound, kept verbatim).
    let desc = unsafe { (&raw mut tx.ring.0 as *mut tx_desc).add(index as usize) };
    // SAFETY: `desc` computed above, points at a live ring entry.
    if unsafe { (*desc).status } & E1000_TXD_STAT_DD == 0 {
        // Descriptor not finished; report error. `tx` drops here (RAII).
        return -1;
    }
    // SAFETY: `index` bounds as above.
    let slot = unsafe { (&raw mut tx.mbufs as *mut *mut mbuf).add(index as usize) };
    // SAFETY: `slot` live.
    if !unsafe { *slot }.is_null() {
        // Free the buffer containing the already-transmitted data.
        // SAFETY: `*slot` non-null, previously stashed by an earlier
        // `e1000_transmit` call whose descriptor has now completed
        // (checked above).
        unsafe { Mbuf::free(*slot) };
    }
    // Pass packet information into the transmit descriptor.
    // SAFETY: `m` is caller-owned and live (the `NetdevOps::transmit`
    // contract); `desc`/`slot` live as established above.
    unsafe {
        (*desc).addr = (*m).head as u64;
        (*desc).length = (*m).len as u16;
        // - let the controller report status so a later call can check
        //   whether this descriptor has finished transmission;
        // - tell the controller this is the last descriptor of the
        //   packet.
        (*desc).cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
        *slot = m;
        // Move the tail pointer of the transmission ring buffer forward.
        Self::reg_write(E1000_TDT, (Self::reg_read(E1000_TDT) + 1) % TX_RING_SIZE as u32);
    }
    // `tx` drops here (RAII) -- no manual `spin_unlock` needed.
    0
}

/// Check for packets that have arrived from the e1000; deliver each to
/// the network stack via `Net::net_rx()`.
///
/// # Safety
/// `REGS` must be set.
unsafe fn recv() {
    loop {
        // SAFETY: caller contract.
        let index = (unsafe { Self::reg_read(E1000_RDT) } + 1) % RX_RING_SIZE as u32;
        // SAFETY: `index < RX_RING_SIZE` (mod above).
        let desc = unsafe { (&raw mut RX_RING.0 as *mut rx_desc).add(index as usize) };
        // Keep processing RX descriptors until an unfinished one is hit.
        // SAFETY: `desc` live.
        if unsafe { (*desc).status } & E1000_RXD_STAT_DD == 0 {
            return;
        }
        // SAFETY: `index` bounds as above; `RX_MBUFS[index]` was
        // installed by `e1000_set_receive_descriptor_base` (initial
        // fill) or a prior iteration of this loop, always non-null on
        // success (see module doc's "Preserved-as-is" note for the one
        // OOM path that would already have null-deref'd before reaching
        // here, same shape as the C original).
        let slot = unsafe { (&raw mut RX_MBUFS as *mut *mut mbuf).add(index as usize) };
        let buf = unsafe { *slot };
        // Start processing the received packet.
        // SAFETY: `buf`/`desc` live.
        unsafe { (*buf).len = (*desc).length as c_uint };
        // `buf` live; `net_rx` takes ownership (frees or forwards it,
        // matching every other netdev driver's contract).
        Net::net_rx(buf);
        // Allocate a new buffer for this slot. No preconditions.
        let newbuf = Mbuf::alloc(0);
        // Let the descriptor point to the newly allocated buffer and
        // tell the controller we've finished processing this packet.
        // SAFETY: `slot`/`desc` live; `newbuf` may be null on OOM (see
        // module doc's "Preserved-as-is" note -- ported verbatim,
        // unchecked, matching the C original's own unchecked
        // `newbuf->head` dereference).
        unsafe {
            *slot = newbuf;
            (*desc).addr = (*newbuf).head as u64;
            (*desc).status = 0;
            Self::reg_write(E1000_RDT, index);
        }
    }
}
} // impl E1000 (reg_read/reg_write/panic_fixed/dev_reset/set_rcvaddr/
  // set_transmission_descriptor_base/set_receive_descriptor_base/init/
  // transmit/recv)

/// Tell the e1000 we've seen this interrupt (without this it won't
/// raise any further interrupts), then drain received packets.
///
/// FLOOR: address-taken IRQ handler slot (`Some(e1000_intr)` in
/// [`E1000::init`]'s `IrqDesc.handler` below) -- kept a free fn per this
/// crate's established floor convention (matches `virtio_disk.rs`'s
/// `virtio_disk_intr`, `uart.rs`'s `uartintr`), body updated to call the
/// relocated `E1000` associated fns.
/// [`IrqHandler`] implementor for the e1000 (fn-pointer-callback-slot ->
/// trait-dispatch campaign; was the free fn `e1000_intr` +
/// `Some(e1000_intr)` in [`E1000::init`]'s `IrqDesc.handler`, now
/// folded into this ZST + trait impl, matching the `E1000NetdevOps`
/// implementor precedent below). `irq`/`data`/`dev` are unused --
/// forwarded only for trait-signature uniformity with the registered
/// `IrqDesc`.
struct E1000IrqHandler;

impl crate::irq::irq_core::IrqHandler for E1000IrqHandler {
    unsafe fn handle(&self, _irq: c_int, _data: *mut c_void, _dev: *mut c_void) {
        // SAFETY: `REGS` set by `e1000_init`, which registers this handler
        // (via `register_irq_handler`) only after setting `REGS`.
        unsafe { E1000::reg_write(E1000_ICR, 0xffffffff) };
        // SAFETY: same as above.
        unsafe { E1000::recv() };
    }
}

static E1000_IRQ_HANDLER: E1000IrqHandler = E1000IrqHandler;

// ===========================================================================
// netdev glue.
// ===========================================================================

/// [`NetdevOps`] implementor for the e1000 (fn-pointer-ops-table ->
/// trait dispatch campaign; was the free fn `e1000_netdev_transmit` +
/// `netdev_ops { transmit: Some(..) }` table literal, both now folded
/// into this ZST + trait impl, matching the `CdevOps`/`PcacheOps`
/// implementor precedent).
struct E1000NetdevOps;

impl NetdevOps for E1000NetdevOps {
    unsafe fn transmit(&self, _dev: *mut netdev, m: *mut mbuf) -> c_int {
        E1000::transmit(m)
    }
}

static E1000_NETDEV_OPS: E1000NetdevOps = E1000NetdevOps;
