//! Intel e1000 Ethernet controller driver.
//!
//! Descriptor arrays retain their hardware layout and live in UnsafeCell so
//! DMA writes do not alias Rust references. Locks serialize CPU ring access;
//! volatile descriptor accesses and fences delimit device ownership transfers.
//! Failed receive allocations reuse the completed buffer without dereferencing
//! null or leaving the device with freed storage.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_int, c_uint, c_void};
use core::ptr;

use crate::bindings::{mbuf, netdev, rx_desc, tx_desc};
use crate::dev::netdev::NetdevOps;
use crate::irq::irq_core::{IrqCore, IrqDesc};
use crate::sync::SpinLock;

use crate::printf::Printf;

use crate::net::{Mbuf, Net, MBUF_SIZE};
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
const E1000_RXD_STAT_EOP: u8 = 0x02;

const TX_RING_SIZE: usize = 16;
const RX_RING_SIZE: usize = 16;

/// `kernel/inc/dev/netdev.h`: `#define NETDEV_NAME_MAX 16`.
const NETDEV_NAME_MAX: usize = 16;

// The NIC reads and writes these exact legacy DMA descriptor layouts.
// Compile-time assertions below pin every hardware-visible field offset.

/// Native `struct tx_desc` (`kernel/inc/dev/e1000_dev.h`) — one e1000
/// legacy transmit descriptor [E1000 3.3.3].
#[repr(C)]
#[derive(Copy, Clone)]
pub struct TxDesc {
    /// Buffer physical address.
    pub addr: u64,
    /// Buffer length.
    pub length: u16,
    /// Checksum offset.
    pub cso: u8,
    /// Command byte (`E1000_TXD_CMD_*`).
    pub cmd: u8,
    /// Status byte (`E1000_TXD_STAT_DD`).
    pub status: u8,
    /// Checksum start.
    pub css: u8,
    pub special: u16,
}

/// Native `struct rx_desc` (`kernel/inc/dev/e1000_dev.h`) — one e1000
/// legacy receive descriptor [E1000 3.2.3].
#[repr(C)]
#[derive(Copy, Clone)]
pub struct RxDesc {
    /// Address of the descriptor's data buffer.
    pub addr: u64,
    /// Length of data DMAed into the data buffer.
    pub length: u16,
    /// Packet checksum.
    pub csum: u16,
    /// Descriptor status (`E1000_RXD_STAT_*`).
    pub status: u8,
    /// Descriptor errors.
    pub errors: u8,
    pub special: u16,
}

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
struct TxRing(UnsafeCell<[tx_desc; TX_RING_SIZE]>);
/// Mirrors `struct rx_desc rx_ring[RX_RING_SIZE] __ALIGNED(16);`.
#[repr(C, align(16))]
struct RxRing(UnsafeCell<[rx_desc; RX_RING_SIZE]>);

const _: () = {
    assert!(core::mem::align_of::<TxRing>() >= 16);
    assert!(core::mem::align_of::<RxRing>() >= 16);
    assert!(core::mem::size_of::<TxRing>() % 128 == 0);
    assert!(core::mem::size_of::<RxRing>() % 128 == 0);
};

const ZERO_TX_DESC: tx_desc = tx_desc { addr: 0, length: 0, cso: 0, cmd: 0, status: 0, css: 0, special: 0 };
const ZERO_RX_DESC: rx_desc = rx_desc { addr: 0, length: 0, csum: 0, status: 0, errors: 0, special: 0 };

/// Descriptor ownership and retained packets, serialized by the TX lock.
struct TxState {
    ring: TxRing,
    mbufs: [*mut mbuf; TX_RING_SIZE],
}

// SAFETY: each pointer is an owned packet retained until its DMA descriptor
// completes. TX's SpinLock serializes all CPU access to this state; moving the
// state does not dereference those pointers or transfer access to the device.
unsafe impl Send for TxState {}

/// `spinlock_t e1000_lock = SPINLOCK_INITIALIZED("e1000_lock");` --
/// compile-time initialised, valid for locking from the moment
/// `.bss`/`.data` are live, no runtime `spin_init` call required (same
/// convention as `kernel/uart.rs`'s `UART_TX_LOCK`/`UART_RX_LOCK`).
static TX: SpinLock<TxState> = SpinLock::new(
    c"e1000_lock",
    TxState { ring: TxRing(UnsafeCell::new([ZERO_TX_DESC; TX_RING_SIZE])), mbufs: [ptr::null_mut(); TX_RING_SIZE] },
);

/// CPU access is serialized even when interrupts migrate between harts.
struct RxState {
    ring: RxRing,
    mbufs: [*mut mbuf; RX_RING_SIZE],
    discarding: bool,
}

// SAFETY: the ring owns its packet pointers. The RX lock serializes CPU
// ownership changes; DMA-visible descriptor bytes are behind UnsafeCell.
unsafe impl Send for RxState {}

static RX: SpinLock<RxState> = SpinLock::new(
    c"e1000_rx",
    RxState {
        ring: RxRing(UnsafeCell::new([ZERO_RX_DESC; RX_RING_SIZE])),
        mbufs: [ptr::null_mut(); RX_RING_SIZE],
        discarding: false,
    },
);

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

/// Driver entry points. MMIO and DMA state live in stable kernel storage.
pub(crate) struct E1000;

impl E1000 {
/// Order descriptor memory and MMIO together. Rust atomic fences only order
/// normal memory; the NIC's tail registers are device-output accesses.
#[inline]
fn io_fence() {
    // SAFETY: FENCE only orders this hart's memory/device accesses and does
    // not touch registers, pointers or stack storage. Omitting nomem keeps
    // the compiler barrier as well as the RISC-V I/O and memory barrier.
    unsafe { core::arch::asm!("fence iorw, iorw", options(nostack, preserves_flags)) };
}

/// Read a word-indexed MMIO register.
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

/// Full reset of the device. Called by [`Self::init`]. `REGS` must
/// already be set.
///
/// # Safety
/// `REGS` must be set (see [`Self::reg_read`]/[`Self::reg_write`]).
unsafe fn dev_reset() {
    // SAFETY: caller contract.
    unsafe {
        Self::reg_write(E1000_IMS, 0); // disable interrupts
        let ctl = Self::reg_read(E1000_CTL);
        Self::reg_write(E1000_CTL, ctl | E1000_CTL_RST);
        Self::reg_write(E1000_IMS, 0); // redisable interrupts
    }
    Self::io_fence();
}

/// Set receive MAC address: adds `mac` to the e1000's address filter
/// table at `index` (`< 16`). `as_type`: `0` = destination address
/// (required for normal mode), `1` = source address. Returns `0` on
/// success, `-1` on error.
///
/// # Safety
/// `REGS` must be set.
unsafe fn set_rcvaddr(mac: &[u8; 6], as_type: u8, valid: bool, index: usize) -> i32 {
    if index >= 16 {
        return -1; // the receive address array of e1000 has < 16 entries
    }
    let l = u32::from_le_bytes([mac[0], mac[1], mac[2], mac[3]]);
    let mut h = u32::from(u16::from_le_bytes([mac[4], mac[5]])) | (u32::from(as_type) << 16);
    if valid {
        h |= 1 << 31;
    }
    Self::io_fence();
    // SAFETY: caller contract; `index` bounds-checked above.
    unsafe { Self::reg_write(E1000_RA + 2 * index, l) };
    Self::io_fence();
    // SAFETY: same as above.
    unsafe { Self::reg_write(E1000_RA + 2 * index + 1, h) };
    Self::io_fence();
    0
}

/// Initialize the fixed, aligned TX ring before the device is enabled.
///
/// # Safety
/// REGS must be initialized and DMA must not access the ring yet. `state`
/// must be the locked TX static, whose address remains stable after return.
unsafe fn initialize_transmit(state: &mut TxState) {
    let base = state.ring.0.get() as u64;
    // SAFETY: DMA is disabled and the caller exclusively owns the ring.
    let descriptors = unsafe { &mut *state.ring.0.get() };
    descriptors.fill(tx_desc { status: E1000_TXD_STAT_DD, ..ZERO_TX_DESC });
    state.mbufs.fill(ptr::null_mut());
    Self::io_fence();
    // SAFETY: initialized registers; the static ring has the required layout.
    unsafe {
        Self::reg_write(E1000_TDBAL, base as u32);
        Self::reg_write(E1000_TDBAH, (base >> 32) as u32);
        Self::reg_write(E1000_TDLEN, core::mem::size_of::<TxRing>() as u32);
        Self::reg_write(E1000_TDH, 0);
        Self::reg_write(E1000_TDT, 0);
    }
    Self::io_fence();
}

/// Initialize the fixed RX ring, releasing earlier allocations on failure.
///
/// # Safety
/// REGS must be initialized and DMA must not access the ring yet. `state`
/// must be the locked RX static, whose address remains stable after return.
unsafe fn initialize_receive(state: &mut RxState) -> Result<(), ()> {
    let base = state.ring.0.get() as u64;
    // SAFETY: DMA is disabled and the caller exclusively owns the ring.
    let descriptors = unsafe { &mut *state.ring.0.get() };
    descriptors.fill(ZERO_RX_DESC);
    for index in 0..RX_RING_SIZE {
        let packet = Mbuf::alloc(0);
        if packet.is_null() {
            for allocated in &mut state.mbufs[..index] {
                // SAFETY: these allocations were never published to the NIC.
                unsafe { Mbuf::free(*allocated) };
                *allocated = ptr::null_mut();
            }
            return Err(());
        }
        state.mbufs[index] = packet;
        // SAFETY: the newly allocated packet is initialized and exclusively owned.
        descriptors[index].addr = unsafe { (*packet).head as u64 };
    }
    Self::io_fence();
    // SAFETY: initialized registers; every descriptor now owns a valid buffer.
    unsafe {
        Self::reg_write(E1000_RDBAL, base as u32);
        Self::reg_write(E1000_RDBAH, (base >> 32) as u32);
        Self::reg_write(E1000_RDLEN, core::mem::size_of::<RxRing>() as u32);
        Self::reg_write(E1000_RDH, 0);
        Self::reg_write(E1000_RDT, (RX_RING_SIZE - 1) as u32);
    }
    Self::io_fence();
    Ok(())
}

/// Called by [`crate::pci::pci_init`]. `xregs` is the memory address at
/// which the e1000's registers are mapped.
///
/// # Safety
/// `xregs` must map the e1000's full register aperture. Initialization must
/// run once, before any packet submission or device interrupt is possible.
pub(crate) unsafe fn init(xregs: *mut u32) {
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

    // SAFETY: initialized MMIO, disabled DMA, and guards borrowing the stable
    // static ring storage. Ring alignment/size are checked at compile time.
    unsafe { Self::initialize_transmit(&mut TX.lock()) };
    // SAFETY: same initialization and ownership contract as the TX ring.
    if unsafe { Self::initialize_receive(&mut RX.lock()) }.is_err() {
        Self::panic_fixed(c"e1000: receive buffer allocation");
    }

    // Filter by qemu's MAC address, 52:54:00:12:34:56.
    // SAFETY: `default_mac_address` valid for 6 bytes; `REGS` set.
    if unsafe { Self::set_rcvaddr(&default_mac_address, 0, true, 0) } != 0 {
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
        E1000_NDEV.name = [0; NETDEV_NAME_MAX];
        for (destination, source) in E1000_NDEV.name.iter_mut().zip(b"e1000") {
            *destination = *source as core::ffi::c_char;
        }
        E1000_NDEV.mac = default_mac_address;
        E1000_NDEV.mtu = 1500;
        E1000_NDEV.link_up = 1;
        E1000_NDEV.speed = 1000;
        E1000_NDEV.full_duplex = 1;
        E1000_NDEV.ops = Some(&E1000_NETDEV_OPS);
        E1000_NDEV.priv_ = REGS as *mut c_void;
        Netdev::register(&raw mut E1000_NDEV);
    }
}

/// Submit one frame, taking ownership on success and leaving it with the
/// caller when the ring is full.
///
/// # Safety
/// `packet` must be a live, exclusively owned packet; REGS must be initialized.
unsafe fn transmit(packet: *mut mbuf) -> c_int {
    let mut tx = TX.lock();
    // SAFETY: initialization precedes netdev publication.
    let index = unsafe { Self::reg_read(E1000_TDT) } as usize;
    if index >= TX_RING_SIZE {
        Self::panic_fixed(c"e1000 transmit: ring overflow");
    }
    // SAFETY: the checked index selects a descriptor in stable static storage.
    let descriptor = unsafe { tx.ring.0.get().cast::<tx_desc>().add(index) };
    // SAFETY: the status byte is DMA-shared and must be read with volatility.
    if unsafe { ptr::read_volatile(&raw const (*descriptor).status) } & E1000_TXD_STAT_DD == 0 {
        return -1;
    }
    Self::io_fence();
    let completed = core::mem::replace(&mut tx.mbufs[index], packet);
    if !completed.is_null() {
        // SAFETY: DD confirms that DMA no longer reads the previous packet.
        unsafe { Mbuf::free(completed) };
    }
    // SAFETY: the caller transfers exclusive ownership; DD grants the CPU
    // descriptor ownership. Reset status as well as address/length: retaining
    // the old DD bit would allow freeing a packet while DMA still reads it.
    unsafe {
        ptr::write_volatile(descriptor, tx_desc {
            addr: (*packet).head as u64,
            length: (*packet).len as u16,
            cmd: E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS,
            ..ZERO_TX_DESC
        });
    }
    Self::io_fence();
    // SAFETY: initialized register; descriptor is complete before publication.
    unsafe { Self::reg_write(E1000_TDT, ((index + 1) % TX_RING_SIZE) as u32) };
    0
}

/// Drain completed receive descriptors, recycling a buffer on allocation
/// failure and rejecting invalid lengths, hardware errors and split frames.
///
/// # Safety
/// REGS must be initialized and the receive ring must be configured.
unsafe fn recv() {
    loop {
        let completed = {
            let mut rx = RX.lock();
            // SAFETY: initialized register; wrapping avoids overflow if a
            // faulty register value is observed before taking the ring modulo.
            let index = unsafe { Self::reg_read(E1000_RDT) }.wrapping_add(1) as usize % RX_RING_SIZE;
            // SAFETY: index is within the stable, DMA-visible ring allocation.
            let descriptor = unsafe { rx.ring.0.get().cast::<rx_desc>().add(index) };
            // SAFETY: the device writes this status byte asynchronously.
            let status = unsafe { ptr::read_volatile(&raw const (*descriptor).status) };
            if status & E1000_RXD_STAT_DD == 0 {
                return;
            }
            Self::io_fence();
            // SAFETY: DD transferred this descriptor to the CPU, and the lock
            // excludes other CPU consumers until it is rearmed below.
            let (length, errors) = unsafe {
                (ptr::read_volatile(&raw const (*descriptor).length),
                 ptr::read_volatile(&raw const (*descriptor).errors))
            };
            let end_of_packet = status & E1000_RXD_STAT_EOP != 0;
            let valid = end_of_packet && !rx.discarding && errors == 0
                && (14..=MBUF_SIZE).contains(&usize::from(length));
            rx.discarding = !end_of_packet;
            let old = rx.mbufs[index];
            // Allocate before surrendering old to the stack. With no spare
            // page, drop this frame and give its existing buffer back to DMA.
            let replacement = if valid { Mbuf::alloc(0) } else { ptr::null_mut() };
            let completed = if replacement.is_null() {
                None
            } else {
                rx.mbufs[index] = replacement;
                // SAFETY: completed buffer is CPU-owned and length was checked.
                unsafe { (*old).len = c_uint::from(length) };
                Some(old)
            };
            let active = rx.mbufs[index];
            // SAFETY: active is either the nonnull existing buffer or a fresh
            // allocation; the completed descriptor belongs to this locked CPU.
            unsafe {
                ptr::write_volatile(descriptor, rx_desc { addr: (*active).head as u64, ..ZERO_RX_DESC });
            }
            Self::io_fence();
            // SAFETY: a valid, fully initialized buffer is ready for the NIC.
            unsafe { Self::reg_write(E1000_RDT, index as u32) };
            completed
        };
        if let Some(packet) = completed {
            // SAFETY: the rearmed descriptor points at a different buffer and
            // this completed allocation is now exclusively owned by the stack.
            unsafe { Net::net_rx(packet) };
        }
    }
}
}

/// Acknowledge device interrupts, then drain completed receive descriptors.
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

/// Network-device trait implementation transfers packets to the TX ring.
struct E1000NetdevOps;

impl NetdevOps for E1000NetdevOps {
    unsafe fn transmit(&self, _dev: *mut netdev, m: *mut mbuf) -> c_int {
        // SAFETY: the NetdevOps caller transfers a live DMA-ready packet.
        unsafe { E1000::transmit(m) }
    }
}

static E1000_NETDEV_OPS: E1000NetdevOps = E1000NetdevOps;
