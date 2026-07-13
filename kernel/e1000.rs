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
//! [`tx_desc`]/[`rx_desc`] (`crate::bindings`, generated from
//! `kernel/inc/dev/e1000_dev.h`, added to `build.rs`'s allowlist this
//! wave, anchored `^tx_desc$|^rx_desc$` to avoid matching the unrelated
//! `x1_tx_desc`/`x1_rx_desc`) are real `bindgen` types -- the e1000's DMA
//! engine reads/writes these 16-byte entries directly, same rationale as
//! every other hardware-ABI struct this wave. [`layout_asserts`] pins
//! `size_of`/`offset_of` for both at compile time. [`TX_RING`]/[`RX_RING`]
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
//! - [`e1000_recv`]'s `newbuf = mbufalloc(0)` return is never null-
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

use crate::bindings::{mbuf, netdev, netdev_ops, rx_desc, spinlock_t, tx_desc};
use crate::irq::irq_core::{plic_irq, register_irq_handler, IrqDesc};

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.
    fn printf(fmt: *const c_char, ...) -> c_int;
    safe fn __panic_start();
    safe fn __panic_end() -> !;

    // lock/spinlock.rs -- raw calls (matches this crate's convention for
    // straight-line critical sections with an early-return exit path).
    safe fn spin_lock(l: *mut spinlock_t);
    safe fn spin_unlock(l: *mut spinlock_t);

    // string.rs.
    fn strncpy(s: *mut c_char, t: *const c_char, n: usize) -> *mut c_char;
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

    // net.rs (this wave, same crate).
    fn mbufalloc(headroom: c_uint) -> *mut mbuf;
    fn mbuffree(m: *mut mbuf);
    fn net_rx(m: *mut mbuf);

    // dev/netdev.rs (Phase 2 Wave 24).
    fn netdev_register(dev: *mut netdev) -> c_int;
}

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
// DMA layout asserts (hardware ABI -- see module doc).
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
// Platform-probed MMIO base/IRQ. Keeps the exact C symbol names/types:
// `kernel/mm/vm_pgtab.rs` (`kvmmake`) externs `__e1000_pci_mmio_base` by
// name; `kernel/inc/dev/e1000_dev.h` declares both `extern`.
// ===========================================================================

#[no_mangle]
pub static mut __e1000_pci_mmio_base: u64 = 0x40000000;
#[no_mangle]
pub static mut __e1000_pci_irqno: u64 = 33;

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

static mut TX_RING: TxRing = TxRing([ZERO_TX_DESC; TX_RING_SIZE]);
static mut TX_MBUFS: [*mut mbuf; TX_RING_SIZE] = [ptr::null_mut(); TX_RING_SIZE];

static mut RX_RING: RxRing = RxRing([ZERO_RX_DESC; RX_RING_SIZE]);
static mut RX_MBUFS: [*mut mbuf; RX_RING_SIZE] = [ptr::null_mut(); RX_RING_SIZE];

/// Mirrors `STATIC volatile uint32 *regs;` -- remembers where the
/// e1000's registers live. Set exactly once, in [`e1000_init`], before
/// any other function in this file runs (registration order:
/// `pci_init` finds the device, programs BAR0, then calls
/// `e1000_init` -- no concurrent access is possible before that call
/// returns and `netdev_register` publishes this driver).
static mut REGS: *mut u32 = ptr::null_mut();

/// Mirrors `spinlock_t e1000_lock = SPINLOCK_INITIALIZED("e1000_lock");`
/// -- compile-time initialised, valid for locking from the moment
/// `.bss`/`.data` are live, no runtime `spin_init` call required (same
/// convention as `kernel/uart.rs`'s `UART_TX_LOCK`/`UART_RX_LOCK`).
static mut E1000_LOCK: spinlock_t = spinlock_t { locked: 0, name: c"e1000_lock".as_ptr() as *mut c_char, cpu: ptr::null_mut() };

static mut E1000_NDEV: netdev = netdev {
    name: [0; NETDEV_NAME_MAX],
    mac: [0; 6],
    ip: 0,
    mtu: 0,
    link_up: 0,
    speed: 0,
    full_duplex: 0,
    index: 0,
    ops: ptr::null_mut(),
    priv_: ptr::null_mut(),
    next: ptr::null_mut(),
    link_cb: None,
};

/// `#define R(n, r)`-equivalent word-indexed MMIO access -- see module
/// doc's "MMIO -- volatile accounting".
///
/// # Safety
/// `REGS` must have been set by [`e1000_init`]; `offset` must be a
/// valid e1000 register word-index.
#[inline(always)]
unsafe fn reg_read(offset: usize) -> u32 {
    // SAFETY: caller contract.
    unsafe { core::ptr::read_volatile(REGS.add(offset)) }
}
/// # Safety
/// Same as [`reg_read`].
#[inline(always)]
unsafe fn reg_write(offset: usize, val: u32) {
    // SAFETY: caller contract.
    unsafe { core::ptr::write_volatile(REGS.add(offset), val) }
}

#[cold]
#[inline(never)]
fn panic_fixed(msg: &core::ffi::CStr) -> ! {
    __panic_start();
    // SAFETY: `msg` is 'static, nul-terminated, no format args.
    unsafe { printf(msg.as_ptr()) };
    __panic_end()
}

// ===========================================================================
// Device programming.
// ===========================================================================

/// Full reset of the device. Called by [`e1000_init`]. `REGS` must
/// already be set.
///
/// # Safety
/// `REGS` must be set (see [`reg_read`]/[`reg_write`]).
unsafe fn e1000_dev_reset() {
    // SAFETY: caller contract.
    unsafe {
        reg_write(E1000_IMS, 0); // disable interrupts
        let ctl = reg_read(E1000_CTL);
        reg_write(E1000_CTL, ctl | E1000_CTL_RST);
        reg_write(E1000_IMS, 0); // redisable interrupts
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
unsafe fn e1000_set_rcvaddr(mac: *const u8, as_type: u8, valid: bool, index: i32) -> i32 {
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
    unsafe { reg_write(E1000_RA + 2 * index as usize, l) };
    fence(Ordering::SeqCst);
    // SAFETY: same as above.
    unsafe { reg_write(E1000_RA + 2 * index as usize + 1, h) };
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
unsafe fn e1000_set_transmission_descriptor_base(
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
        reg_write(E1000_TDBAL, l as u32);
        reg_write(E1000_TDBAH, h as u32);
        reg_write(E1000_TDLEN, size);
        reg_write(E1000_TDH, 0);
        reg_write(E1000_TDT, 0);
    }
    fence(Ordering::SeqCst);
    0
}

/// Initialise the receive descriptor circular buffer. Same contract as
/// [`e1000_set_transmission_descriptor_base`], and additionally
/// allocates an `mbuf` per entry via `mbufalloc(0)`.
///
/// # Safety
/// Same as [`e1000_set_transmission_descriptor_base`].
unsafe fn e1000_set_receive_descriptor_base(
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
            let mb = mbufalloc(0);
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
        reg_write(E1000_RDBAL, l as u32);
        reg_write(E1000_RDBAH, h as u32);
        reg_write(E1000_RDLEN, size);
        reg_write(E1000_RDH, 0);
        reg_write(E1000_RDT, (rd_arr_size - 1) as u32);
    }
    fence(Ordering::SeqCst);
    0
}

/// Called by [`crate::pci::pci_init`]. `xregs` is the memory address at
/// which the e1000's registers are mapped.
#[no_mangle]
pub extern "C" fn e1000_init(xregs: *mut u32) {
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
            IrqDesc { handler: Some(e1000_intr), data: ptr::null_mut(), dev: ptr::null_mut(), irq: 0, count: 0, rcu_head: core::mem::zeroed() };
        register_irq_handler(plic_irq(__e1000_pci_irqno as c_int), &raw mut e1000_irq)
    };
    if ret != 0 {
        panic_fixed(c"e1000_init: failed to register irq handler");
    }

    // Reset the device.
    // SAFETY: `REGS` just set above.
    unsafe { e1000_dev_reset() };

    // [E1000 14.5] Transmit initialization.
    // SAFETY: `TX_RING`/`TX_MBUFS` are `'static`, exclusively accessed
    // here (single-threaded init, same as `REGS` above).
    let (tx_ring_ptr, tx_mbufs_ptr) = unsafe { (&raw mut TX_RING.0 as *mut tx_desc, &raw mut TX_MBUFS as *mut *mut mbuf) };
    // SAFETY: `TX_RING`/`TX_MBUFS` are `'static`, exclusively accessed
    // here (single-threaded init).
    if unsafe {
        e1000_set_transmission_descriptor_base(
            tx_ring_ptr,
            tx_ring_ptr as u64,
            tx_mbufs_ptr,
            (TX_RING_SIZE * core::mem::size_of::<tx_desc>()) as u32,
        )
    } != 0
    {
        panic_fixed(c"e1000");
    }

    // [E1000 14.4] Receive initialization.
    // SAFETY: same as the TX ring above.
    let (rx_ring_ptr, rx_mbufs_ptr) = unsafe { (&raw mut RX_RING.0 as *mut rx_desc, &raw mut RX_MBUFS as *mut *mut mbuf) };
    // SAFETY: same as the TX ring above.
    if unsafe {
        e1000_set_receive_descriptor_base(
            rx_ring_ptr,
            rx_ring_ptr as u64,
            rx_mbufs_ptr,
            (RX_RING_SIZE * core::mem::size_of::<rx_desc>()) as u32,
        )
    } != 0
    {
        panic_fixed(c"e1000");
    }

    // Filter by qemu's MAC address, 52:54:00:12:34:56.
    // SAFETY: `default_mac_address` valid for 6 bytes; `REGS` set.
    if unsafe { e1000_set_rcvaddr(default_mac_address.as_ptr(), 0, true, 0) } != 0 {
        panic_fixed(c"e1000_init: MAC address");
    }

    // SAFETY: `REGS` set; every write below is a plain MMIO register write.
    unsafe {
        // Multicast table.
        for i in 0..(4096 / 32) {
            reg_write(E1000_MTA + i, 0);
        }

        // Transmitter control bits.
        reg_write(
            E1000_TCTL,
            E1000_TCTL_EN // enable
                | E1000_TCTL_PSP // pad short packets
                | (0x0F << E1000_TCTL_CT_SHIFT) // max retransmissions on collision
                | (0x40 << E1000_TCTL_COLD_SHIFT), // collision distance
        );
        reg_write(E1000_TIPG, 10 | (8 << 10) | (6 << 20)); // inter-pkt gap

        // Receiver control bits.
        reg_write(
            E1000_RCTL,
            E1000_RCTL_EN // enable receiver
                | E1000_RCTL_BAM // enable broadcast
                | E1000_RCTL_SZ_2048 // 2048-byte rx buffers
                | E1000_RCTL_SECRC, // strip CRC
        );

        // Ask e1000 for receive interrupts. Instead of RDTR/RADV, use
        // the Interrupt Throttling Register (ITR) if a delay is needed.
        reg_write(E1000_RDTR, 0); // interrupt after every received packet (no timer)
        reg_write(E1000_RADV, 0); // interrupt after every packet (no timer)
        reg_write(E1000_IMS, 1 << 7); // RXDW -- Receiver Descriptor Write Back

        // Register with the netdev abstraction layer.
        strncpy(E1000_NDEV.name.as_mut_ptr(), c"e1000".as_ptr(), NETDEV_NAME_MAX);
        memmove(E1000_NDEV.mac.as_mut_ptr() as *mut c_void, default_mac_address.as_ptr() as *const c_void, 6);
        E1000_NDEV.mtu = 1500;
        E1000_NDEV.link_up = 1;
        E1000_NDEV.speed = 1000;
        E1000_NDEV.full_duplex = 1;
        E1000_NDEV.ops = &E1000_NETDEV_OPS as *const netdev_ops as *mut netdev_ops;
        E1000_NDEV.priv_ = REGS as *mut c_void;
        netdev_register(&raw mut E1000_NDEV);
    }
}

/// The mbuf contains an ethernet frame; program it into the TX
/// descriptor ring so the e1000 sends it. Stashes a pointer so it can
/// be freed after sending. Returns `0` on success, `-1` if the ring is
/// full (the next descriptor hasn't finished transmitting yet).
#[no_mangle]
pub extern "C" fn e1000_transmit(m: *mut mbuf) -> c_int {
    // Raw spin_lock/spin_unlock (not RAII): early-return-while-holding
    // exit path below (matches this crate's convention, see
    // `kernel/virtio_disk.rs`/`kernel/ramdisk.rs`'s identical rationale).
    spin_lock(unsafe { &raw mut E1000_LOCK });

    // Get the current tail pointer of the transmission ring buffer.
    // SAFETY: `REGS` set by `e1000_init`, which always runs before any
    // netdev consumer can reach this function (publication order via
    // `netdev_register`).
    let index = unsafe { reg_read(E1000_TDT) };
    if index > TX_RING_SIZE as u32 {
        panic_fixed(c"e1000 transmit: ring overflow");
    }
    // SAFETY: `index <= TX_RING_SIZE` (see module doc's "Preserved-as-
    // is" note for the C's own off-by-one-tolerant bound, kept verbatim).
    let desc = unsafe { (&raw mut TX_RING.0 as *mut tx_desc).add(index as usize) };
    // SAFETY: `desc` computed above, points at a live ring entry.
    if unsafe { (*desc).status } & E1000_TXD_STAT_DD == 0 {
        // Descriptor not finished; report error.
        spin_unlock(unsafe { &raw mut E1000_LOCK });
        return -1;
    }
    // SAFETY: `index` bounds as above.
    let slot = unsafe { (&raw mut TX_MBUFS as *mut *mut mbuf).add(index as usize) };
    // SAFETY: `slot` live.
    if !unsafe { *slot }.is_null() {
        // Free the buffer containing the already-transmitted data.
        // SAFETY: `*slot` non-null, previously stashed by an earlier
        // `e1000_transmit` call whose descriptor has now completed
        // (checked above).
        unsafe { mbuffree(*slot) };
    }
    // Pass packet information into the transmit descriptor.
    // SAFETY: `m` is caller-owned and live (the `netdev_ops.transmit`
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
        reg_write(E1000_TDT, (reg_read(E1000_TDT) + 1) % TX_RING_SIZE as u32);
    }
    spin_unlock(unsafe { &raw mut E1000_LOCK });
    0
}

/// Check for packets that have arrived from the e1000; deliver each to
/// the network stack via `net_rx()`.
///
/// # Safety
/// `REGS` must be set.
unsafe fn e1000_recv() {
    loop {
        // SAFETY: caller contract.
        let index = (unsafe { reg_read(E1000_RDT) } + 1) % RX_RING_SIZE as u32;
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
        // SAFETY: `buf` live; `net_rx` takes ownership (frees or
        // forwards it, matching every other netdev driver's contract).
        unsafe { net_rx(buf) };
        // Allocate a new buffer for this slot.
        // SAFETY: no preconditions.
        let newbuf = unsafe { mbufalloc(0) };
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
            reg_write(E1000_RDT, index);
        }
    }
}

/// Tell the e1000 we've seen this interrupt (without this it won't
/// raise any further interrupts), then drain received packets.
extern "C" fn e1000_intr(_irq: c_int, _data: *mut c_void, _dev: *mut c_void) {
    // SAFETY: `REGS` set by `e1000_init`, which registers this handler
    // (via `register_irq_handler`) only after setting `REGS`.
    unsafe { reg_write(E1000_ICR, 0xffffffff) };
    // SAFETY: same as above.
    unsafe { e1000_recv() };
}

// ===========================================================================
// netdev glue.
// ===========================================================================

extern "C" fn e1000_netdev_transmit(_ndev: *mut netdev, m: *mut mbuf) -> c_int {
    e1000_transmit(m)
}

static E1000_NETDEV_OPS: netdev_ops = netdev_ops { transmit: Some(e1000_netdev_transmit) };
