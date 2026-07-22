//! virtio-mmio block device driver -- Rust port of `kernel/virtio_disk.c`
//! (Phase 2 Wave 28, sub-wave A -- see `docs/rustify/phase2_plan.md`; the
//! final porting wave). Drives qemu's `virtio-blk-device` over the
//! virtio-mmio transport (`qemu ... -device
//! virtio-mmio-bus.0,drive=x0` et al.).
//!
//! # DMA layout (hardware ABI)
//!
//! [`virtq_desc`]/[`virtq_avail`]/[`virtq_used`]/[`virtio_blk_req`]
//! resolve through the unchanged `crate::bindings` facade paths to the
//! NATIVE [`VirtqDesc`]/[`VirtqAvail`]/[`VirtqUsed`]/[`VirtioBlkReq`]
//! below (P3-4c; they were real `bindgen` types from
//! `kernel/inc/dev/virtio.h` when this port landed): the qemu device
//! reads/writes these structures directly over DMA, so the natives are
//! pinned byte-exact to that header by hardcoded asserts + the
//! toolchain-gcc probe (same rationale as Wave 25's
//! `x1_rx_desc`/`x1_tx_desc`).
//! [`layout_asserts`] pins `size_of`/`offset_of` for all four types at
//! compile time. `struct disk`'s *own* layout (this file's private
//! [`Disk`]) is **not** hardware ABI -- only the three ring pages
//! (`kalloc()`'d separately and handed to the device by physical
//! address) and the per-descriptor `virtio_blk_req` headers embedded in
//! [`Disk::ops`] are ever DMA-visible.
//!
//! # Virtqueue ordering protocol -- barrier accounting
//!
//! The C original has exactly **7** compiler/hardware barrier
//! intrinsics, every one preserved below at the identical call site:
//! 6 `__atomic_thread_fence(__ATOMIC_SEQ_CST)` (a real hardware fence)
//! -> [`core::sync::atomic::fence`]`(Ordering::SeqCst)`, and 1
//! `__atomic_signal_fence(__ATOMIC_SEQ_CST)` (compiler-reordering
//! barrier only, no hardware fence) -> [`core::sync::atomic::
//! compiler_fence`]`(Ordering::SeqCst)` in [`alloc3_desc`]. Sites:
//! [`free_desc`] (1, after clearing a descriptor, before checking the
//! wakeup threshold), [`alloc3_desc`] (1 signal fence, before the
//! allocation loop), [`virtio_disk_rw`] (2: after publishing
//! `avail->ring[..]`/before bumping `avail->idx`, and after bumping
//! `avail->idx`/before `QUEUE_NOTIFY`), [`virtio_disk_intr`] (3: after
//! acking the interrupt/before reading `used->idx`, at the top of each
//! loop iteration before reading a `used->ring[..]` entry, and after
//! advancing `used_idx`). This is the full virtqueue publish/consume
//! protocol from the virtio 1.1 spec (driver writes descriptors, fences,
//! publishes the avail index, fences, notifies; device fences, consumes,
//! fences, publishes the used index) -- removing or reordering any one
//! of these would be a genuine correctness bug (torn DMA reads on either
//! side), not a style choice, so every site is called out individually
//! rather than folded into a helper.
//!
//! # MMIO
//!
//! The C `#define R(n, r) ((volatile uint32 *)(__virtio_mmio_base[n] +
//! r))` macro (the file's one `volatile` site) is reimplemented as
//! [`reg`], read/written exclusively through
//! [`read_volatile`](core::ptr::read_volatile)/
//! [`write_volatile`](core::ptr::write_volatile) below -- same semantics
//! as the C `volatile uint32 *` dereference, compiler can never elide or
//! reorder an access.
//!
//! # Sleeping-completion protocol with the Rust bufcache/bio layer
//!
//! `virtio_disk_rw` is the [`BlkdevOps::submit_bio`] implementation
//! ([`virtio_disk_submit_bio`]): it programs the three-descriptor chain
//! and returns immediately (`Ok`, meaning "submitted") -- I/O completion
//! is entirely interrupt-driven, signalled to the waiting thread via
//! [`virtio_disk_intr`]'s [`bio_complete`] call (this file's local
//! reimplementation of `dev/bio.h`'s `static inline bio_complete`,
//! matching the established precedent -- see `kernel/dev/x1_sdhci.rs`'s
//! module doc). Callers (`kernel/bufcache.rs`, `kernel/vfs/xv6fs/
//! superblock.rs`) wait via their own local `bio_await`
//! reimplementation, unchanged by this port.
//!
//! # Deliberate simplifications (documented, no behavioural change)
//!
//! - `struct freelist` (`kernel/inc/freelist.h`, entirely `static
//!   inline`, used nowhere else in the tree -- grep-verified) is
//!   reimplemented as a self-contained [`Freelist`] that owns its
//!   `free`/`list` arrays internally, rather than `Disk` holding the
//!   backing arrays and handing borrowed pointers to a separate
//!   `freelist` field the way the C did (`disk->free`/`disk->free_list`
//!   passed into `freelist_init`) -- purely an internal representation
//!   change of a private, single-file data structure never touched by
//!   hardware or another translation unit; `freelist_is_free` (never
//!   called) is dropped, same precedent as Wave 1's dead-function drops.
//! - `__thread_state_set(current, THREAD_UNINTERRUPTIBLE)`
//!   (`kernel/inc/proc/thread.h`, `static inline`, no external linkage)
//!   is reimplemented natively in [`thread_state_set_uninterruptible`]:
//!   a raw `__ATOMIC_SEQ_CST` store to `thread->state`, matching the C
//!   body exactly (every `proc/*.rs` file that needs this logic already
//!   keeps its own private copy -- see e.g. `proc/thread.rs`'s
//!   `thread_state_set` -- this file follows the same per-file
//!   convention rather than reaching into `proc`'s private internals).
//! - `panic`/`assert` (`kernel/inc/printf.h` macros) drop the
//!   `PANIC %s:%d: In function '%s':\n` file/line/function preamble the
//!   C macro prepends, keeping only the formatted message + `\n` --
//!   same established simplification as `kernel/lock/spinlock.rs`'s
//!   `deadlock_panic`/`fixed_msg_panic` and `kernel/mm/vm.rs`'s
//!   `xv6_vm_assert_vm_write_held`.
//! - Feature negotiation (`__virtio_disk_init_one`) narrows the C's
//!   `uint64 features` local to `u32`: `VIRTIO_MMIO_DEVICE_FEATURES`/
//!   `VIRTIO_MMIO_DRIVER_FEATURES` are 32-bit MMIO registers and every
//!   feature bit this driver clears is `<= 29`, so the C's 64-bit
//!   widening never carried extra information -- the register write
//!   truncates to 32 bits either way. No behavioural change.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{compiler_fence, fence, AtomicU32, Ordering};

use crate::bindings::{
    bio, bio_vec, blkdev_t, device_t, mode_t, page_t, platform_info,
    thread, tq_t, virtio_blk_req, virtq_avail, virtq_desc, virtq_used, EIO, PGSIZE,
};
use crate::dev::blkdev::BlkdevOps;
use crate::kstd::KResult;
// N-R7g (lock-owns-data): the disk's *non-DMA* request-tracking state
// (descriptor free-map, `used_idx` consume cursor, descriptor-starvation
// wait queue) now lives in a data-owning `SpinLock<DiskInner>`; the DMA
// rings, the device-read `ops[]` headers, and the `info[]` array (whose
// `.status` byte is a device DMA-WRITE target -- see [`virtio_disk_rw`])
// stay raw. `tq_wait` is reached through the guard's `wait_on`.
use crate::sync::SpinLock;
use crate::irq::irq_core::{plic_irq, register_irq_handler, IrqDesc};
// P3-D3b: lock/completion.rs's entry points are plain safe Rust fns now
// that their `#[no_mangle]` exports are gone; reached by crate path.
use crate::lock::completion::RawCompletion;
// P3-D2a: proc/thread_queue.rs primitives, reached as plain crate-path
// items instead of `extern "C"` redeclarations.
// NO-STANDALONE-FN: the `tq_init`/`tq_wakeup_all` free-fn delegators were
// deleted; the two sites below build a `TqRef` handle via `from_ptr` and
// invoke the method. (`tq_wait` here is reached through the guard's
// `wait_on`, not called directly.)
use crate::proc::access::TqRef;
use crate::machine;
use crate::proc::proc_shims::xv6_current_thread;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
// N-R7g: the hand-rolled `spin_init`/`spin_lock`/`spin_unlock` facade
// wrappers are gone -- the disk's lock is now embedded in
// `SpinLock<DiskInner>`, so every acquire/release is the guard's `lock()`/
// `Drop`, the descriptor-starvation sleep is the guard's `wait_on`, and
// the diagnostic name is set once by `SpinLock::new`. (The DMA rings and
// the `info[]`/`ops[]` request buffers stay raw -- see the struct below.)

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.

    // lock/spinlock.rs -- raw calls (not `sync::KSpinlock` RAII): this
    // file's lock interacts with `tq_wait`'s internal release/reacquire
    // (`virtio_disk_rw`'s descriptor-starvation sleep), same precedent
    // as `kernel/vfs/xv6fs/log.rs`, which uses raw calls throughout the
    // whole file for the same reason (one consistent lock discipline
    // rather than mixing RAII and manual calls in the same module).
    // string.rs.
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;

}

// P3-D3c: `dev/fdt.rs`'s boot-probed platform config is a plain
// crate-path import now that its `#[no_mangle]` export is gone (same
// `platform_info` type, unchanged call sites -- reads of a `static mut`
// stay `unsafe` either way).
use crate::dev::fdt::platform;

// P3-D3a: `kalloc` (genuinely `unsafe fn` in `crate::mm::kalloc`; its
// call sites already sit in `unsafe` contexts) is reached as a
// crate-path item instead of the `extern "C"` redeclaration that used to
// sit in the block above. `__page_to_pa` is also genuinely `unsafe fn`
// (`crate::mm::page`), declared `safe` here (usual FFI facade) with the
// bindgen `page_t` view rather than page.rs's own `Page` struct (same
// layout, different Rust name); the thin wrapper preserves both.
// `kalloc` is now `impl Kmem` (kalloc.rs KERNEL-OO wave); called
// fully-qualified at its call sites below instead of `use`-imported.

/// SAFETY: `page` must be a live `Page`
/// (see [`crate::mm::page::__page_to_pa`]'s contract).
#[inline]
fn __page_to_pa(page: *mut page_t) -> u64 {
    unsafe { crate::mm::page::Page::__page_to_pa(page as *mut crate::mm::page::Page) }
}
// P3-1D mesh sweep: dev/blkdev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::dev::blkdev::Blkdev;

// ===========================================================================
// virtio-mmio register offsets/bits, virtqueue constants -- redeclared
// locally from `kernel/inc/dev/virtio.h` per this crate's established
// convention (macro-only headers are hand-copied, not bindgen'd).
// ===========================================================================

const N_VIRTIO_DISK: usize = 2;
/// `#define NUM 64` -- descriptor-ring size, must be a power of two.
const NUM: usize = 64;

const VIRTIO_MMIO_MAGIC_VALUE: usize = 0x000;
const VIRTIO_MMIO_VERSION: usize = 0x004;
const VIRTIO_MMIO_DEVICE_ID: usize = 0x008;
const VIRTIO_MMIO_VENDOR_ID: usize = 0x00c;
const VIRTIO_MMIO_DEVICE_FEATURES: usize = 0x010;
const VIRTIO_MMIO_DRIVER_FEATURES: usize = 0x020;
const VIRTIO_MMIO_QUEUE_SEL: usize = 0x030;
const VIRTIO_MMIO_QUEUE_NUM_MAX: usize = 0x034;
const VIRTIO_MMIO_QUEUE_NUM: usize = 0x038;
const VIRTIO_MMIO_QUEUE_READY: usize = 0x044;
const VIRTIO_MMIO_QUEUE_NOTIFY: usize = 0x050;
const VIRTIO_MMIO_INTERRUPT_STATUS: usize = 0x060;
const VIRTIO_MMIO_INTERRUPT_ACK: usize = 0x064;
const VIRTIO_MMIO_STATUS: usize = 0x070;
const VIRTIO_MMIO_QUEUE_DESC_LOW: usize = 0x080;
const VIRTIO_MMIO_QUEUE_DESC_HIGH: usize = 0x084;
const VIRTIO_MMIO_DRIVER_DESC_LOW: usize = 0x090;
const VIRTIO_MMIO_DRIVER_DESC_HIGH: usize = 0x094;
const VIRTIO_MMIO_DEVICE_DESC_LOW: usize = 0x0a0;
const VIRTIO_MMIO_DEVICE_DESC_HIGH: usize = 0x0a4;

const VIRTIO_CONFIG_S_ACKNOWLEDGE: u32 = 1;
const VIRTIO_CONFIG_S_DRIVER: u32 = 2;
const VIRTIO_CONFIG_S_DRIVER_OK: u32 = 4;
const VIRTIO_CONFIG_S_FEATURES_OK: u32 = 8;

const VIRTIO_BLK_F_RO: u32 = 5;
const VIRTIO_BLK_F_SCSI: u32 = 7;
const VIRTIO_BLK_F_CONFIG_WCE: u32 = 11;
const VIRTIO_BLK_F_MQ: u32 = 12;
const VIRTIO_F_ANY_LAYOUT: u32 = 27;
const VIRTIO_RING_F_INDIRECT_DESC: u32 = 28;
const VIRTIO_RING_F_EVENT_IDX: u32 = 29;

const VRING_DESC_F_NEXT: u16 = 1;
const VRING_DESC_F_WRITE: u16 = 2;

const VIRTIO_BLK_T_IN: u32 = 0;
const VIRTIO_BLK_T_OUT: u32 = 1;

/// `kernel/inc/vfs/xv6fs/ondisk.h`: `#define BSIZE 1024`.
const BSIZE: usize = 1024;
/// `kernel/inc/uabi/stat.h` `S_IFBLK`, same local copy as other `dev/*.rs`.
const S_IFBLK: u32 = 0o060_000;
const THREAD_UNINTERRUPTIBLE: u32 = 6;

// ===========================================================================
// Native virtqueue quintet — P3-4c nativization (user directive:
// remove the C-compatible interfaces; THE DMA-exactness scrutiny
// class, nativized LAST — block I/O is the filesystem's lifeline).
// `VirtqDesc`/`VirtqAvail`/`VirtqUsedElem`/`VirtqUsed`/`VirtioBlkReq`
// are the canonical definitions of `kernel/inc/dev/virtio.h`'s
// `struct virtq_desc`/`virtq_avail`/`virtq_used_elem`/`virtq_used`/
// `virtio_blk_req` (the virtio 1.x spec's split-virtqueue layout +
// the §5.2 block-request header): `build.rs` blocklists the bindgen
// emissions and re-exports these types under their C names (facade
// `pub use`, N2 pattern), so this file's ring pointers, `Disk::ops`
// embedded request headers, and every `.ring[..]`/`.idx` access
// resolve right back here.
//
// *** DEVICE-READ/WRITTEN DMA LAYOUT — HANDLE WITH P3-4 SCRUTINY ***
// The virtio-blk DEVICE dereferences these structures in guest RAM:
// QUEUE_DESC/DRIVER/DEVICE registers are programmed with their
// physical addresses, the device follows `desc.next` chains, reads
// `avail.idx`/`ring`, and writes `used.ring`/`idx`. EVERY disk block
// (including this kernel's own fs) moves through them — any drift is
// silent disk corruption or a hang at the first read. The driver's
// volatile/fence discipline (Wave-28 exactness) is UNTOUCHED — only
// the type-definition source changes. The ring lengths are tied to
// the same `NUM` const the driver indexes with (`% NUM`), so the
// const and the layout can never drift apart. No packed/align
// attributes: the pre-nativization bindgen emissions were plain
// `#[repr(C)]` (natural alignment), reproduced verbatim —
// `virtq_avail` is 134/2 with its trailing `unused` u16 (the
// avail-event slot) explicit, exactly as the header spells it.
//
// DERIVE DECISIONS (P3-4c): Copy + Clone on all five, exactly as the
// pre-nativization bindgen output derived (plain-int PODs;
// `VirtqUsed` embeds `[VirtqUsedElem; NUM]` by value).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + toolchain-gcc `_Static_assert` probe (rv64gc/lp64d —
// scratchpad p3_4c_dma_probe.c); both agree on every value asserted
// below.
// ===========================================================================

/// Native `struct virtq_desc` (`kernel/inc/dev/virtio.h`) — a single
/// descriptor, from the virtio spec.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtqDesc {
    /// Buffer physical address.
    pub addr: crate::bindings::uint64,
    /// Buffer length.
    pub len: crate::bindings::uint32,
    /// `VRING_DESC_F_NEXT` / `VRING_DESC_F_WRITE`.
    pub flags: crate::bindings::uint16,
    /// Next descriptor index (if `VRING_DESC_F_NEXT`).
    pub next: crate::bindings::uint16,
}

/// Native `struct virtq_avail` (`kernel/inc/dev/virtio.h`) — the
/// (entire) avail ring, from the virtio spec.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtqAvail {
    /// Always zero.
    pub flags: crate::bindings::uint16,
    /// Driver will write `ring[idx]` next.
    pub idx: crate::bindings::uint16,
    /// Descriptor numbers of chain heads.
    pub ring: [crate::bindings::uint16; NUM],
    /// Trailing avail-event slot (unused).
    pub unused: crate::bindings::uint16,
}

/// Native `struct virtq_used_elem` (`kernel/inc/dev/virtio.h`) — one
/// entry in the used ring: the device tells the driver about a
/// completed request.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtqUsedElem {
    /// Index of start of completed descriptor chain.
    pub id: crate::bindings::uint32,
    /// Bytes written.
    pub len: crate::bindings::uint32,
}

/// Native `struct virtq_used` (`kernel/inc/dev/virtio.h`) — the
/// (entire) used ring.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtqUsed {
    /// Always zero.
    pub flags: crate::bindings::uint16,
    /// Device increments when it adds a `ring[]` entry.
    pub idx: crate::bindings::uint16,
    /// Completed chains.
    pub ring: [VirtqUsedElem; NUM],
}

/// Native `struct virtio_blk_req` (`kernel/inc/dev/virtio.h`) — the
/// first descriptor of a disk request (virtio spec §5.2), followed by
/// the block and a one-byte status.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtioBlkReq {
    /// `VIRTIO_BLK_T_IN` or `VIRTIO_BLK_T_OUT` (C field `type`).
    pub type_: crate::bindings::uint32,
    /// Reserved (zero).
    pub reserved: crate::bindings::uint32,
    /// Sector number (512-byte units).
    pub sector: crate::bindings::uint64,
}

// P3-4c hardcoded layout proof — the virtio 1.x split-virtqueue byte
// contract (`kernel/inc/dev/virtio.h`), every field of all five
// records. Values captured from the pre-nativization bindgen output
// via the temporary in-tree `offset_of!` gate and cross-checked by the
// toolchain-gcc probe.
const _: () = {
    use core::mem::{align_of, offset_of, size_of};
    assert!(size_of::<VirtqDesc>() == 16, "virtq_desc size (DEVICE-READ)");
    assert!(align_of::<VirtqDesc>() == 8, "virtq_desc alignment");
    assert!(offset_of!(VirtqDesc, addr) == 0, "vd.addr offset (DEVICE-READ)");
    assert!(offset_of!(VirtqDesc, len) == 8, "vd.len offset (DEVICE-READ)");
    assert!(offset_of!(VirtqDesc, flags) == 12, "vd.flags offset (DEVICE-READ)");
    assert!(offset_of!(VirtqDesc, next) == 14, "vd.next offset (DEVICE-READ)");

    assert!(size_of::<VirtqAvail>() == 134, "virtq_avail size (DEVICE-READ)");
    assert!(align_of::<VirtqAvail>() == 2, "virtq_avail alignment");
    assert!(offset_of!(VirtqAvail, flags) == 0, "va.flags offset (DEVICE-READ)");
    assert!(offset_of!(VirtqAvail, idx) == 2, "va.idx offset (DEVICE-READ)");
    assert!(offset_of!(VirtqAvail, ring) == 4, "va.ring offset (DEVICE-READ)");
    assert!(offset_of!(VirtqAvail, unused) == 132, "va.unused offset");
    assert!(NUM == 64, "ring lengths == NUM (DEVICE-READ)");

    assert!(size_of::<VirtqUsedElem>() == 8, "virtq_used_elem size (DEVICE-WRITTEN)");
    assert!(align_of::<VirtqUsedElem>() == 4, "virtq_used_elem alignment");
    assert!(offset_of!(VirtqUsedElem, id) == 0, "vue.id offset (DEVICE-WRITTEN)");
    assert!(offset_of!(VirtqUsedElem, len) == 4, "vue.len offset (DEVICE-WRITTEN)");

    assert!(size_of::<VirtqUsed>() == 516, "virtq_used size (DEVICE-WRITTEN)");
    assert!(align_of::<VirtqUsed>() == 4, "virtq_used alignment");
    assert!(offset_of!(VirtqUsed, flags) == 0, "vu.flags offset (DEVICE-WRITTEN)");
    assert!(offset_of!(VirtqUsed, idx) == 2, "vu.idx offset (DEVICE-WRITTEN)");
    assert!(offset_of!(VirtqUsed, ring) == 4, "vu.ring offset (DEVICE-WRITTEN)");

    assert!(size_of::<VirtioBlkReq>() == 16, "virtio_blk_req size (DEVICE-READ)");
    assert!(align_of::<VirtioBlkReq>() == 8, "virtio_blk_req alignment");
    assert!(offset_of!(VirtioBlkReq, type_) == 0, "br.type offset (DEVICE-READ)");
    assert!(offset_of!(VirtioBlkReq, reserved) == 4, "br.reserved offset (DEVICE-READ)");
    assert!(offset_of!(VirtioBlkReq, sector) == 8, "br.sector offset (DEVICE-READ)");
};

// ===========================================================================
// DMA layout asserts (hardware ABI -- see module doc). Kept verbatim
// from the Wave-28 port (now redundant with the full P3-4c block above,
// but they are the port's own documented invariants).
// ===========================================================================
#[allow(dead_code)]
mod layout_asserts {
    use super::*;
    const _SIZE_DESC: () = assert!(core::mem::size_of::<virtq_desc>() == 16);
    const _SIZE_BLK_REQ: () = assert!(core::mem::size_of::<virtio_blk_req>() == 16);
    const _OFF_DESC_LEN: () = assert!(core::mem::offset_of!(virtq_desc, len) == 8);
    const _OFF_DESC_FLAGS: () = assert!(core::mem::offset_of!(virtq_desc, flags) == 12);
    const _OFF_DESC_NEXT: () = assert!(core::mem::offset_of!(virtq_desc, next) == 14);
    const _OFF_AVAIL_IDX: () = assert!(core::mem::offset_of!(virtq_avail, idx) == 2);
    const _OFF_AVAIL_RING: () = assert!(core::mem::offset_of!(virtq_avail, ring) == 4);
    const _OFF_USED_IDX: () = assert!(core::mem::offset_of!(virtq_used, idx) == 2);
    const _OFF_USED_RING: () = assert!(core::mem::offset_of!(virtq_used, ring) == 4);
    const _OFF_BLKREQ_RESERVED: () = assert!(core::mem::offset_of!(virtio_blk_req, reserved) == 4);
    const _OFF_BLKREQ_SECTOR: () = assert!(core::mem::offset_of!(virtio_blk_req, sector) == 8);
}

// ===========================================================================
// Platform-probed MMIO base/IRQ tables. Keeps the exact C symbol
// names/types: `kernel/dev/fdt.rs` (`fdt_apply_platform_config`) and
// `kernel/mm/vm_pgtab.rs` (`kvmmake`) extern these by name and populate
// them before/around this driver's own init.
// ===========================================================================

// P3-1D mesh sweep: caller (`mm/vm_pgtab.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) static mut __virtio_mmio_base: [u64; 3] = [0x10001000, 0x10002000, 0x10003000];
// P3-1D mesh sweep: no caller anywhere outside this file (in-scope writer
// is `dev/fdt.rs`, itself in scope this wave and already using a
// crate-path `use`) -- demoted.
pub(crate) static mut __virtio_irqno: [u64; 3] = [1, 2, 3];

/// `#define R(n, r) ((volatile uint32 *)(__virtio_mmio_base[n] + (r)))`.
///
/// # Safety
/// `diskno` must be `< __virtio_mmio_base.len()`.
#[inline(always)]
unsafe fn reg(diskno: usize, offset: usize) -> *mut u32 {
    // SAFETY: caller contract; `__virtio_mmio_base` is populated by
    // `fdt_apply_platform_config` before `virtio_disk_init` ever runs
    // (boot-order invariant, `start_kernel.rs`).
    unsafe { (__virtio_mmio_base[diskno] + offset as u64) as *mut u32 }
}

// ===========================================================================
// Freelist -- private reimplementation of `kernel/inc/freelist.h` (see
// module doc's "Deliberate simplifications").
// ===========================================================================

struct Freelist {
    free: [bool; NUM],
    list: [u16; NUM],
    idx: u16,
}

impl Freelist {
    const fn new() -> Self {
        Freelist { free: [false; NUM], list: [0u16; NUM], idx: 0 }
    }

    /// All `NUM` descriptors start out free (mirrors `freelist_init`).
    fn init(&mut self) {
        // N-METH: zip the two backing arrays and `enumerate` instead of a
        // `for i in 0..NUM` index loop (drops the per-slot bounds checks;
        // `free[i]=true, list[i]=i` verbatim).
        for (i, (free, slot)) in self.free.iter_mut().zip(self.list.iter_mut()).enumerate() {
            *free = true;
            *slot = i as u16;
        }
        self.idx = NUM as u16;
    }

    /// Mirrors `freelist_alloc`: returns the item index, or `-1` if empty.
    fn alloc(&mut self) -> i32 {
        if self.idx == 0 {
            return -1;
        }
        self.idx -= 1;
        let idx = self.list[self.idx as usize] as usize;
        self.free[idx] = false;
        idx as i32
    }

    /// Mirrors `freelist_free`: returns `0` on success, `-1` on error
    /// (out-of-range index, or double-free).
    fn free_item(&mut self, i: i32) -> i32 {
        if i < 0 || i as usize >= NUM {
            return -1;
        }
        let iu = i as usize;
        if self.free[iu] {
            return -1;
        }
        self.free[iu] = true;
        self.list[self.idx as usize] = i as u16;
        self.idx += 1;
        if self.idx as usize > NUM {
            return -1;
        }
        0
    }

    /// Mirrors `freelist_available`.
    fn available(&self) -> i32 {
        self.idx as i32
    }
}

// ===========================================================================
// Per-disk state. `#[repr(C)]` kept for diffability against the C
// original (`struct disk`), though this layout is never itself DMA
// visible -- only `desc`/`avail`/`used` (separately `kalloc()`'d pages)
// and `ops[]` (embedded `virtio_blk_req` headers) are.
// ===========================================================================

struct DiskInfo {
    bio: *mut bio,
    done: bool,
    /// **Device DMA-WRITE target** (see [`virtio_disk_rw`]'s descriptor-2
    /// setup: descriptor `idx[2]`'s buffer address is
    /// `&info[idx[0]].status`, flagged `VRING_DESC_F_WRITE`). The device
    /// writes `0` here on success. This is why the whole `info[]` array
    /// CANNOT move into the lock-owns-data `SpinLock` (N-R7g): a
    /// `SpinLockGuard`'s `&mut` would claim exclusive/`noalias` access to
    /// a byte the device mutates asynchronously by physical address. The
    /// module doc's "`struct disk` ... is not hardware ABI" line omits
    /// this field; the code (and its `// device writes the status`
    /// comment) is authoritative. `info[]` therefore stays a raw `Disk`
    /// field, serialised by the same lock but not owned by it.
    status: u8,
}

/// The lock-protected **non-DMA** request-tracking state (N-R7g). Exactly
/// the fields the device never touches by physical address:
/// * `desc_freelist` -- the descriptor free-map (`freelist_alloc`/`_free`
///   bookkeeping; pure CPU-side).
/// * `used_idx` -- the driver's *own* copy of how far it has consumed the
///   used ring. The device has its **own** `used->idx` in the DMA `used`
///   page (read raw in [`virtio_disk_intr`]); this cursor is never DMA.
/// * `desc_wait_queue` -- the descriptor-starvation wait queue, `tq_init`'d
///   against this lock so a later [`SpinLockGuard::wait_on`] releases and
///   reacquires *this* same lock.
///
/// Everything the device reads or writes by physical address stays a raw
/// [`Disk`] field: the `desc`/`avail`/`used` ring pointers, the
/// device-read `ops[]` request headers, and the `info[]` array (its
/// `.status` byte is a device DMA-write target -- see [`DiskInfo`]).
struct DiskInner {
    used_idx: u16,
    desc_freelist: Freelist,
    desc_wait_queue: tq_t,
}

#[repr(C)]
struct Disk {
    desc: *mut virtq_desc,
    avail: *mut virtq_avail,
    used: *mut virtq_used,

    info: [DiskInfo; NUM],
    ops: [virtio_blk_req; NUM],

    /// N-R7g: lock-owns-data for the non-DMA tracking state. Replaces the
    /// former `vdisk_lock: spinlock_t` + bare `used_idx`/`desc_freelist`/
    /// `desc_wait_queue` fields. The lock still serialises the raw `info[]`
    /// / ring accesses in `virtio_disk_rw`/`virtio_disk_intr` (they run
    /// while this guard is held), it just no longer *owns* them.
    inner: SpinLock<DiskInner>,
}

// ---------------------------------------------------------------------------
// Global storage. `SyncCell<T>` redefined locally per this crate's
// established convention (see `kernel/dev/x1_sdhci.rs`'s identical copy).
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

static DISKS: SyncCell<[MaybeUninit<Disk>; N_VIRTIO_DISK]> =
    SyncCell(UnsafeCell::new([const { MaybeUninit::uninit() }; N_VIRTIO_DISK]));
static VIRTIO_DISK_DEVS: SyncCell<[MaybeUninit<blkdev_t>; N_VIRTIO_DISK]> =
    SyncCell(UnsafeCell::new([const { MaybeUninit::uninit() }; N_VIRTIO_DISK]));

const DISK_NAMES: [&core::ffi::CStr; N_VIRTIO_DISK] = [c"disk0", c"disk1"];

/// # Safety
/// `diskno` must be `< N_VIRTIO_DISK`.
#[inline(always)]
unsafe fn disk_ptr(diskno: usize) -> *mut Disk {
    // SAFETY: caller contract; `DISKS` storage is 'static.
    unsafe { (*DISKS.get())[diskno].as_mut_ptr() }
}
/// # Safety
/// `diskno` must be `< N_VIRTIO_DISK`.
#[inline(always)]
unsafe fn blkdev_ptr(diskno: usize) -> *mut blkdev_t {
    // SAFETY: caller contract; `VIRTIO_DISK_DEVS` storage is 'static.
    unsafe { (*VIRTIO_DISK_DEVS.get())[diskno].as_mut_ptr() }
}

// ===========================================================================
// `dev/bio.h`'s static-inline helpers, reimplemented natively (same
// precedent as `kernel/dev/x1_sdhci.rs`, `kernel/bufcache.rs`).
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
        it.blkno = (*bio_ptr).blkno
            + (((*bio_ptr).done_size as u64) >> (BLK_SIZE_SHIFT + (*bio_ptr).block_shift as u32));
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
    unsafe { (*bio_ptr).flags.rw() != 0 }
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_start_io_acct(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    unsafe {
        (*bio_ptr).flags.set_done(0);
        (*bio_ptr).done_size = 0;
        (*bio_ptr).error = 0;
        RawCompletion::reinit(&raw mut (*bio_ptr).io_completion);
    }
    fence(Ordering::SeqCst);
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_end_io_acct(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    unsafe { (*bio_ptr).flags.set_done(1) };
    fence(Ordering::SeqCst);
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_endio(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    if let Some(cb) = unsafe { (*bio_ptr).end_io } {
        // SAFETY: `cb` is the bio owner's completion callback.
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
        RawCompletion::complete_all(&raw mut (*bio_ptr).io_completion);
    }
}

// ===========================================================================
// Panic helpers -- see module doc's "Deliberate simplifications".
// ===========================================================================

// P3-2 (zero-C wave): `panic_fmt_i32` used to take its format string as a
// runtime `&CStr` argument (each call site's literal embedding a `%d`) --
// `core::fmt`'s `format_args!` requires a literal format string, so it
// became a macro (format captured as `:literal` at each call site) instead
// of a plain function. `panic_fixed` never had a dynamic format piece (no
// vararg), so it stays a plain function, just taking `&str` instead of
// `&CStr`.
macro_rules! panic_fmt_i32 {
    ($fmt:literal, $val:expr) => {{
        __panic_start();
        crate::kprintln!($fmt, $val);
        __panic_end()
    }};
}

#[cold]
#[inline(never)]
fn panic_fixed(msg: &str) -> ! {
    __panic_start();
    crate::kprintln!("{}", msg);
    __panic_end()
}

#[cold]
#[inline(never)]
fn panic_intr_status(id: c_int, status: c_int, bio_ptr: *mut c_void, blockno: u64) -> ! {
    // Matches the C original's
    // `printf("ERROR: id=%d status=%d buf=%p blockno=0x%lx\n", ...)`
    // immediately preceding its `panic("virtio_disk_intr status: %d", status)`.
    crate::kprintln!(
        "ERROR: id={} status={} buf={} blockno=0x{:x}",
        id, status, crate::printf::Ptr(bio_ptr as u64), blockno,
    );
    __panic_start();
    crate::kprintln!("virtio_disk_intr status: {}", status);
    __panic_end()
}

/// Mirrors `__thread_state_set(p, THREAD_UNINTERRUPTIBLE)`
/// (`kernel/inc/proc/thread.h`, `static inline` -- no external linkage).
/// See module doc's "Deliberate simplifications".
///
/// # Safety
/// `p` must be null or a live `*mut thread`.
#[inline(always)]
unsafe fn thread_state_set_uninterruptible(p: *mut thread) {
    if p.is_null() {
        return;
    }
    // SAFETY: caller contract; `state` is a plain 4-byte field of `p`.
    unsafe {
        let state_ptr = core::ptr::addr_of_mut!((*p).state) as *mut u32;
        AtomicU32::from_ptr(state_ptr).store(THREAD_UNINTERRUPTIBLE, Ordering::SeqCst);
    }
}

// ===========================================================================
// Descriptor allocation.
// ===========================================================================

// N-METH: the four descriptor-management free fns `fn X(inner: &mut
// DiskInner, ...)` become `impl DiskInner { fn X(&mut self, ...) }`. The
// receiver was already the lock-owned [`DiskInner`] (the held guard's
// `&mut`), so these are its natural methods -- the `&mut self` is the same
// exclusive borrow the guard's `DerefMut` hands out (no `*mut -> &`, no
// freeze-noalias hazard); the raw DMA descriptor ring stays a `desc: *mut
// virtq_desc` parameter (device-shared page, unchanged). Same lock-owns-
// data-methodization precedent as `tty.rs` (da51c6c).
impl DiskInner {
    /// Mirrors `alloc_desc`: find a free descriptor, mark it non-free,
    /// return its index. Operates on the lock-owned free-map (`self` is
    /// the held guard's `&mut`), so no `unsafe` -- the freelist is pure
    /// CPU-side state.
    fn alloc_desc(&mut self) -> i32 {
        self.desc_freelist.alloc()
    }

    /// Mirrors `free_desc`: mark a descriptor as free, wake waiters if
    /// enough descriptors are now available. The free-map + wait queue
    /// come from `self` (the held guard); `desc` is the raw DMA descriptor
    /// ring (stays raw -- the device-shared page), cleared here as before.
    ///
    /// # Safety
    /// `desc` must be the disk's live descriptor ring; caller must hold
    /// the disk's `inner` lock (evidenced by the `&mut self`).
    unsafe fn free_desc(&mut self, desc: *mut virtq_desc, i: i32) {
        if self.desc_freelist.free_item(i) != 0 {
            panic_fixed("free_desc: invalid free");
        }

        // SAFETY: caller contract; `i` just validated as a real descriptor
        // index by `free_item` above. `desc` is the raw device-shared ring.
        unsafe {
            let d = desc.add(i as usize);
            (*d).addr = 0;
            (*d).len = 0;
            (*d).flags = 0;
            (*d).next = 0;
        }

        fence(Ordering::SeqCst);
        // Wake waiters if enough descriptors are free. Already holding the
        // lock (the `&mut self` proves it), so wake directly.
        if self.desc_freelist.available() >= 3 {
            // `desc_wait_queue` is a live, initialised `tq_t` in the guarded
            // state; `wakeup_all` is a safe, null-tolerant primitive.
            let _ = TqRef::from_ptr(&raw mut self.desc_wait_queue).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wakeup_all(0, 0));
        }
    }

    /// Mirrors `free_chain`: free a chain of descriptors.
    ///
    /// # Safety
    /// `desc` must be the disk's live descriptor ring; caller must hold
    /// the disk's `inner` lock; `i` must be the head of a valid descriptor
    /// chain.
    unsafe fn free_chain(&mut self, desc: *mut virtq_desc, mut i: i32) {
        loop {
            // SAFETY: caller contract; `desc` is the raw device-shared ring.
            let (flag, nxt) = unsafe {
                let d = desc.add(i as usize);
                ((*d).flags, (*d).next)
            };
            // SAFETY: caller contract.
            unsafe { self.free_desc(desc, i) };
            if flag & VRING_DESC_F_NEXT != 0 {
                i = nxt as i32;
            } else {
                break;
            }
        }
    }

    /// Mirrors `alloc3_desc`: allocate three descriptors (need not be
    /// contiguous); disk transfers always use three descriptors.
    ///
    /// # Safety
    /// `desc` must be the disk's live descriptor ring; caller must hold
    /// the disk's `inner` lock.
    unsafe fn alloc3_desc(&mut self, desc: *mut virtq_desc, idx: &mut [i32; 3]) -> i32 {
        // `__atomic_signal_fence(__ATOMIC_SEQ_CST)` -- compiler-reordering
        // barrier only, no hardware fence (see module doc's barrier
        // accounting).
        compiler_fence(Ordering::SeqCst);
        for i in 0..3 {
            idx[i] = self.alloc_desc();
            if idx[i] < 0 {
                // `idx[0..i)` were successfully allocated above -- iterate
                // that slice to roll them back (N-METH: slice iterator in
                // place of the `for j in 0..i` index loop).
                for &d in &idx[..i] {
                    // SAFETY: caller contract; `desc` live.
                    unsafe { self.free_desc(desc, d) };
                }
                return -1;
            }
        }
        0
    }
}

// ===========================================================================
// I/O submission + completion.
// ===========================================================================

/// Mirrors `virtio_disk_rw`.
///
/// # Safety
/// `diskno` must be `< N_VIRTIO_DISK`; `bio_ptr` must be live; `buf` must
/// be a valid physical-address pointer for `size` bytes, page-backed for
/// the duration of the I/O (caller -- `virtio_disk_submit_bio` --
/// guarantees this via the `bio`'s pinned pages).
unsafe fn virtio_disk_rw(diskno: usize, bio_ptr: *mut bio, sector: u64, buf: *mut c_void, size: usize, write: bool) {
    // SAFETY: caller contract.
    let disk = unsafe { disk_ptr(diskno) };

    if size != BSIZE {
        panic_fixed("virtio_disk_rw: size must be BSIZE");
    }
    if buf.is_null() {
        panic_fixed("virtio_disk_rw: buf is NULL");
    }

    // The raw DMA descriptor ring pointer, copied out before the lock:
    // the ring page is device-shared and stays raw; the free-map that
    // hands out indices into it is the lock-owned state.
    // SAFETY: `disk` live.
    let desc = unsafe { (*disk).desc };

    // N-R7g: acquire the lock-owns-data guard. `d` `DerefMut`s to the
    // guarded [`DiskInner`] (free-map, `used_idx`, wait queue); it holds
    // the *same* `spinlock_t` the C `vdisk_lock` was, so it still
    // serialises the raw `info[]`/ring accesses below (they run while `d`
    // is held) and releases on every exit (RAII) -- replacing the manual
    // `spin_lock`/`spin_unlock` pair.
    // SAFETY: `disk` live.
    let mut d = unsafe { (*disk).inner.lock() };

    // The spec's Section 5.2 says legacy block operations use three
    // descriptors: one for type/reserved/sector, one for the data, one
    // for a 1-byte status result.
    let mut idx = [0i32; 3];
    loop {
        // SAFETY: `desc` live, lock held via `d`.
        if unsafe { d.alloc3_desc(desc, &mut idx) } == 0 {
            break;
        }
        // No free descriptors; wait on the per-disk queue.
        // SAFETY: `xv6_current_thread()` returns the live current thread
        // (or null, handled internally).
        unsafe { thread_state_set_uninterruptible(xv6_current_thread()) };
        // Wait on the descriptor-starvation queue: `wait_on` atomically
        // releases THIS guard's lock, blocks on the queue, and reacquires
        // the same lock before returning (the `tq_wait(q, &lk)` protocol,
        // no lost-wakeup window). The raw queue pointer is formed first so
        // no borrow of `d` is retained across the `&mut self` call.
        let wq = &raw mut d.desc_wait_queue;
        d.wait_on(wq, ptr::null_mut());
    }

    // Format the three descriptors. qemu's virtio-blk.c reads them.
    // SAFETY: `idx[0..3)` were just exclusively allocated above.
    unsafe {
        let buf0 = (*disk).ops.as_mut_ptr().add(idx[0] as usize);
        (*buf0).type_ = if write { VIRTIO_BLK_T_OUT } else { VIRTIO_BLK_T_IN };
        (*buf0).reserved = 0;
        (*buf0).sector = sector;

        let d0 = (*disk).desc.add(idx[0] as usize);
        (*d0).addr = buf0 as u64;
        (*d0).len = core::mem::size_of::<virtio_blk_req>() as u32;
        (*d0).flags = VRING_DESC_F_NEXT;
        (*d0).next = idx[1] as u16;

        let d1 = (*disk).desc.add(idx[1] as usize);
        (*d1).addr = buf as u64;
        (*d1).len = size as u32;
        (*d1).flags = if write { 0 } else { VRING_DESC_F_WRITE }; // device reads b->data / writes b->data
        (*d1).flags |= VRING_DESC_F_NEXT;
        (*d1).next = idx[2] as u16;

        (*disk).info[idx[0] as usize].status = 0xff; // device writes 0 on success
        let d2 = (*disk).desc.add(idx[2] as usize);
        (*d2).addr = (&raw mut (*disk).info[idx[0] as usize].status) as u64;
        (*d2).len = 1;
        (*d2).flags = VRING_DESC_F_WRITE; // device writes the status
        (*d2).next = 0;

        // Record `bio` for `virtio_disk_intr()`.
        (*bio_ptr).private_data = ptr::null_mut();
        (*disk).info[idx[0] as usize].bio = bio_ptr;

        // Tell the device the first index in our chain of descriptors.
        let avail_idx = (*(*disk).avail).idx;
        (*(*disk).avail).ring[(avail_idx as usize) % NUM] = idx[0] as u16;
    }

    fence(Ordering::SeqCst);

    // Tell the device another avail ring entry is available.
    // SAFETY: `disk` live.
    unsafe { (*(*disk).avail).idx = (*(*disk).avail).idx.wrapping_add(1) }; // not % NUM ...

    fence(Ordering::SeqCst);

    debug_assert!(!machine::intr_get(), "virtio_disk_rw: interrupts enabled");
    // SAFETY: `diskno` valid; value is the queue number (0).
    unsafe { core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_QUEUE_NOTIFY), 0) };

    // Submit only -- completion is handled by `virtio_disk_intr()`,
    // which frees the descriptor chain and signals the bio via
    // `bio_complete()`. Callers wait for I/O via `bio_await()`.
    // SAFETY: `disk` live; `info[]` is a raw field (DMA-contaminated via
    // `.status`), serialised by the held guard `d`.
    unsafe { (*disk).info[idx[0] as usize].done = false };
    // `d` drops here -> releases the lock (was the explicit `spin_unlock`).
    drop(d);
}

/// Mirrors `virtio_disk_intr`.
extern "C" fn virtio_disk_intr(_irq: c_int, data: *mut c_void, _dev: *mut c_void) {
    let diskno = data as usize;
    // SAFETY: `diskno` was registered with a valid disk index in
    // `virtio_blkdev_init` below.
    let disk = unsafe { disk_ptr(diskno) };

    // Raw DMA ring pointers, copied out before the lock (device-shared
    // pages, stay raw). `used_idx` -- the driver's *own* consume cursor --
    // is the lock-owned state and is read/written through the guard below.
    // SAFETY: `disk` live.
    let (desc, used) = unsafe { ((*disk).desc, (*disk).used) };

    // N-R7g: acquire the lock-owns-data guard (same `spinlock_t` as the C
    // `vdisk_lock`); releases on scope exit (RAII), replacing the manual
    // `spin_lock`/`spin_unlock`.
    // SAFETY: `disk` live.
    let mut d = unsafe { (*disk).inner.lock() };

    // The device won't raise another interrupt until we tell it we've
    // seen this interrupt, which the following does. This may race with
    // the device writing new entries to the "used" ring, in which case
    // we may process the new completion entries in this interrupt and
    // have nothing to do in the next interrupt, which is harmless.
    // SAFETY: `diskno` valid.
    unsafe {
        let status = core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_INTERRUPT_STATUS)) & 0x3;
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_INTERRUPT_ACK), status);
    }

    fence(Ordering::SeqCst);

    // The device increments disk->used->idx when it adds an entry to
    // the used ring.
    loop {
        // `used_idx` from the guard (lock-owned cursor); `dev_idx` is the
        // device's own index in the raw DMA `used` page.
        // SAFETY: `used` live DMA ring.
        let (used_idx, dev_idx) = (d.used_idx, unsafe { (*used).idx });
        if used_idx == dev_idx {
            break;
        }
        fence(Ordering::SeqCst);
        // SAFETY: `used` live DMA ring.
        let id = unsafe { (*used).ring[(used_idx as usize) % NUM].id } as usize;

        // SAFETY: `id` is a device-reported descriptor index, `< NUM`
        // (the used ring only ever reports indices this driver itself
        // published, all `< NUM`). `info[]` is a raw `Disk` field
        // (DMA-contaminated via `.status`), serialised by the held guard.
        let (status, bio_ptr) = unsafe { ((*disk).info[id].status, (*disk).info[id].bio) };

        if status != 0 {
            panic_intr_status(id as c_int, status as c_int, bio_ptr as *mut c_void, if bio_ptr.is_null() {
                0
            } else {
                // SAFETY: `bio_ptr` non-null here.
                unsafe { (*bio_ptr).blkno }
            });
        }

        // SAFETY: `disk` live; `info[]` raw field, serialised by `d`.
        if unsafe { (*disk).info[id].done } {
            panic_fixed("virtio_disk_intr: already done");
        }
        // SAFETY: `disk` live.
        unsafe { (*disk).info[id].done = true };

        // Clean up descriptor chain and info slot.
        // SAFETY: `disk` live.
        unsafe { (*disk).info[id].bio = ptr::null_mut() };
        // SAFETY: `desc` live, lock held via `d`, `id` a valid chain head.
        unsafe { d.free_chain(desc, id as i32) };

        // Signal bio completion -- wakes any thread in `bio_await()`.
        if !bio_ptr.is_null() {
            // SAFETY: `bio_ptr` non-null and live (owned by the caller
            // that submitted it, still waiting in `bio_await`).
            unsafe {
                (*bio_ptr).error = if status != 0 { -(EIO as c_int) } else { 0 };
                bio_complete(bio_ptr);
            }
        }

        d.used_idx = d.used_idx.wrapping_add(1);
        fence(Ordering::SeqCst);
    }

    // `d` drops here -> releases the lock (was the explicit `spin_unlock`).
    drop(d);
}

// ===========================================================================
// Block device interface.
// ===========================================================================

/// Zero-sized [`BlkdevOps`] implementor for the virtio disks (P3-10c;
/// was the `static blkdev_ops_t VIRTIO_DISK_OPS` fn-pointer table --
/// the three former `extern "C"` callbacks are trait methods now).
struct VirtioDiskOps;

/// The single shared instance `virtio_blkdev_init` installs.
static VIRTIO_DISK_OPS: VirtioDiskOps = VirtioDiskOps;

impl BlkdevOps for VirtioDiskOps {
    unsafe fn open(&self, _blkdev: *mut blkdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn release(&self, _blkdev: *mut blkdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn submit_bio(&self, blkdev: *mut blkdev_t, bio_ptr: *mut bio) -> KResult<()> {
        virtio_disk_submit_bio(blkdev, bio_ptr);
        // The old callback unconditionally returned 0 -- I/O completion
        // (including per-bio errors) is signalled asynchronously via
        // `bio_complete` from `virtio_disk_intr`.
        Ok(())
    }
}

/// `submit_bio` -- process a block I/O request. Iterates over bio
/// segments and hands each one to [`virtio_disk_rw`]; completion is
/// asynchronous (see module doc).
fn virtio_disk_submit_bio(blkdev: *mut blkdev_t, bio_ptr: *mut bio) {
    // SAFETY: `blkdev` is one of `VIRTIO_DISK_DEVS`'s entries, registered
    // in `virtio_blkdev_init` below with `minor` = diskno + 1.
    let diskno = (unsafe { (*blkdev).dev.minor } - 1) as usize; // minor 1 -> disk 0, minor 2 -> disk 1

    // SAFETY: `bio_ptr` live (blkdev_submit_bio's contract).
    unsafe { bio_start_io_acct(bio_ptr) };

    let mut iter = BioIter { blkno: 0, size: 0, size_done: 0, bvec_idx: 0 };
    // SAFETY: `bio_ptr` live.
    unsafe { bio_iter_start(bio_ptr, &mut iter) };
    let mut bvec: bio_vec = bio_vec { bv_page: ptr::null_mut(), len: 0, offset: 0 };
    // SAFETY: `bio_ptr` live; `bvec` local and live.
    while unsafe { bio_iter_copy_bvec(bio_ptr, &iter, &raw mut bvec) } {
        let sector = iter.blkno;
        let page: *mut page_t = bvec.bv_page;
        if page.is_null() {
            panic_fixed("virtio_disk_submit_bio: page is NULL");
        }
        let pa = __page_to_pa(page) as *mut c_void;
        if pa.is_null() {
            panic_fixed("virtio_disk_submit_bio: page has no physical address");
        }
        // SAFETY: `bio_ptr` live.
        let write = unsafe { bio_dir_write(bio_ptr) };
        // SAFETY: `diskno` valid (checked above); `bio_ptr` live; `pa +
        // bvec.offset` is a valid physical-memory pointer for
        // `bvec.len` bytes (the page this segment was built against).
        unsafe {
            virtio_disk_rw(
                diskno,
                bio_ptr,
                sector,
                (pa as u64 + bvec.offset as u64) as *mut c_void,
                bvec.len as usize,
                write,
            );
        }

        // Matches the C original's own (dead -- `size_done` is never
        // read again in this function) manual accumulation, kept for
        // line-for-line fidelity alongside `bio_iter_next_seg`'s own
        // internal (differently-staggered) `size_done` update.
        iter.size_done += bvec.len;
        // SAFETY: `bio_ptr` live.
        unsafe { bio_iter_next_seg(bio_ptr, &mut iter) };
    }

    // I/O has been submitted to the device. Completion (`bio_complete`)
    // is signalled by `virtio_disk_intr` when the device finishes.
    // Callers wait via `bio_await()`.
}

fn virtio_blkdev_init(diskno: usize) {
    // SAFETY: `diskno < N_VIRTIO_DISK`, storage 'static.
    let dev = unsafe { blkdev_ptr(diskno) };
    // SAFETY: `dev` is exclusively owned at this point (not yet
    // registered/published); zero it first (matches the C static
    // initializer's implicit zero-fill for every field the designated
    // initializer doesn't list, e.g. `.dev.kobj`/`.dev.type_`/
    // `.dev.unregistering`/`.dev.ops`), same precedent as
    // `kernel/dev/x1_sdhci.rs`'s `sdhci_init_one`.
    unsafe {
        memset(dev as *mut c_void, 0, core::mem::size_of::<blkdev_t>());
        (*dev).dev.major = 2;
        (*dev).dev.minor = diskno as c_int + 1;
        (*dev).dev.devname = DISK_NAMES[diskno].as_ptr();
        (*dev).dev.devmode = (S_IFBLK | 0o600) as mode_t;
        (*dev).flags.set_readable(1);
        (*dev).flags.set_writable(1);
        (*dev).block_shift = 0; // 2^0 * 512 = 512 bytes per block
        (*dev).ops = Some(&VIRTIO_DISK_OPS);
    }

    // `dev` fully initialised above.
    let errno = Blkdev::register(dev);
    if errno != 0 {
        panic_fmt_i32!("virtio_blkdev_init: blkdev_register failed: {}", errno);
    }

    // SAFETY: `dev` live; `virtio_irq` is a live, fully-initialised
    // `IrqDesc` for the duration of this call.
    let ret = unsafe {
        let mut virtio_irq = IrqDesc {
            handler: Some(virtio_disk_intr),
            data: diskno as *mut c_void,
            dev: &raw mut (*dev).dev as *mut device_t,
            irq: 0,
            count: 0,
            rcu_head: core::mem::zeroed(),
        };
        // `VIRTIO0_IRQ + diskno` -- matches the C original exactly:
        // always `__virtio_irqno[0]` (not `[diskno]`) plus the disk
        // offset, not a per-index IRQ-table lookup.
        register_irq_handler(plic_irq((__virtio_irqno[0] + diskno as u64) as c_int), &raw mut virtio_irq)
    };
    if ret != 0 {
        panic_fmt_i32!("virtio_blkdev_init: register_irq_handler failed: {}", ret);
    }
}

fn virtio_disk_init_one(diskno: usize) {
    // SAFETY: `diskno < N_VIRTIO_DISK`, storage 'static.
    let disk = unsafe { disk_ptr(diskno) };
    let mut status: u32 = 0;

    // SAFETY: `disk` exclusively owned at this point; zero the whole
    // struct first (matches the C static array's implicit zero-init),
    // then initialise every field explicitly below.
    unsafe { memset(disk as *mut c_void, 0, core::mem::size_of::<Disk>()) };

    // N-R7g: install the lock-owns-data inner state. The `memset` above
    // zeroed the `inner` slot too (which would leave a null-named lock);
    // `ptr::write` overwrites it with a properly-named, const-initialised
    // `SpinLock` (`SpinLock::new` writes the exact `{locked:0, name, cpu:
    // null}` state `spin_init` would, so no runtime `spin_init` is
    // needed). The `MaybeUninit` slot has no prior value to drop, and this
    // runs before the disk is registered (single-threaded), so the raw
    // write is sound. `desc_freelist`/`desc_wait_queue` are zero-initial
    // here (`Freelist::new`/zeroed `tq_t`) and fully set up below.
    // SAFETY: `disk` exclusively owned; `inner` slot is uninitialised
    // MaybeUninit memory being initialised for the first time.
    unsafe {
        core::ptr::write(
            &raw mut (*disk).inner,
            SpinLock::new(
                c"virtio_disk",
                DiskInner {
                    used_idx: 0,
                    desc_freelist: Freelist::new(),
                    desc_wait_queue: core::mem::zeroed(),
                },
            ),
        );
    }

    // SAFETY: `diskno` valid.
    let magic_ok = unsafe {
        core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_MAGIC_VALUE)) == 0x74726976
            && core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_VERSION)) == 2
            && core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_DEVICE_ID)) == 2
            && core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_VENDOR_ID)) == 0x554d4551
    };
    if !magic_ok {
        panic_fmt_i32!("could not find virtio disk {}", diskno as c_int);
    }

    // SAFETY: `diskno` valid throughout this function.
    unsafe {
        // reset device
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_STATUS), status);

        // set ACKNOWLEDGE status bit
        status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_STATUS), status);

        // set DRIVER status bit
        status |= VIRTIO_CONFIG_S_DRIVER;
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_STATUS), status);

        // negotiate features (32-bit -- see module doc's "Deliberate
        // simplifications" for why this narrows the C's `uint64`).
        let mut features = core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_DEVICE_FEATURES));
        features &= !(1u32 << VIRTIO_BLK_F_RO);
        features &= !(1u32 << VIRTIO_BLK_F_SCSI);
        features &= !(1u32 << VIRTIO_BLK_F_CONFIG_WCE);
        features &= !(1u32 << VIRTIO_BLK_F_MQ);
        features &= !(1u32 << VIRTIO_F_ANY_LAYOUT);
        features &= !(1u32 << VIRTIO_RING_F_EVENT_IDX);
        features &= !(1u32 << VIRTIO_RING_F_INDIRECT_DESC);
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_DRIVER_FEATURES), features);

        // tell device that feature negotiation is complete.
        status |= VIRTIO_CONFIG_S_FEATURES_OK;
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_STATUS), status);

        // re-read status to ensure FEATURES_OK is set.
        status = core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_STATUS));
        if status & VIRTIO_CONFIG_S_FEATURES_OK == 0 {
            panic_fmt_i32!("virtio disk {} FEATURES_OK unset", diskno as c_int);
        }

        // initialize queue 0.
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_QUEUE_SEL), 0);

        // ensure queue 0 is not in use.
        if core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_QUEUE_READY)) != 0 {
            panic_fmt_i32!("virtio disk {} should not be ready", diskno as c_int);
        }

        // check maximum queue size.
        let max = core::ptr::read_volatile(reg(diskno, VIRTIO_MMIO_QUEUE_NUM_MAX));
        if max == 0 {
            panic_fmt_i32!("virtio disk {} has no queue 0", diskno as c_int);
        }
        if (max as usize) < NUM {
            panic_fmt_i32!("virtio disk {} max queue too short", diskno as c_int);
        }

        // allocate and zero queue memory.
        (*disk).desc = crate::mm::kalloc::Kmem::kalloc() as *mut virtq_desc;
        (*disk).avail = crate::mm::kalloc::Kmem::kalloc() as *mut virtq_avail;
        (*disk).used = crate::mm::kalloc::Kmem::kalloc() as *mut virtq_used;
        if (*disk).desc.is_null() || (*disk).avail.is_null() || (*disk).used.is_null() {
            panic_fmt_i32!("virtio disk {} kalloc", diskno as c_int);
        }
        memset((*disk).desc as *mut c_void, 0, PGSIZE as usize);
        memset((*disk).avail as *mut c_void, 0, PGSIZE as usize);
        memset((*disk).used as *mut c_void, 0, PGSIZE as usize);

        // set queue size.
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_QUEUE_NUM), NUM as u32);

        // write physical addresses.
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_QUEUE_DESC_LOW), (*disk).desc as u64 as u32);
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_QUEUE_DESC_HIGH), ((*disk).desc as u64 >> 32) as u32);
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_DRIVER_DESC_LOW), (*disk).avail as u64 as u32);
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_DRIVER_DESC_HIGH), ((*disk).avail as u64 >> 32) as u32);
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_DEVICE_DESC_LOW), (*disk).used as u64 as u32);
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_DEVICE_DESC_HIGH), ((*disk).used as u64 >> 32) as u32);

        // queue is ready.
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_QUEUE_READY), 1);

        // tell device we're completely ready.
        status |= VIRTIO_CONFIG_S_DRIVER_OK;
        core::ptr::write_volatile(reg(diskno, VIRTIO_MMIO_STATUS), status);
    }

    // all NUM descriptors start out unused; wire the descriptor-starvation
    // queue to the disk's lock. N-R7g: the free-map + wait queue are now
    // lock-owned, so this init goes through a guard -- `tq_init(q, name,
    // guard.lock_ptr())` wires the queue to *this* lock, so a later
    // `wait_on` releases/reacquires it (the exact `lock_ptr()`/`tq_init`
    // use its doc calls out). Init-time and single-threaded, so acquiring
    // the guard here is uncontended.
    {
        // SAFETY: `disk` live; `inner` was initialised above.
        let mut d = unsafe { (*disk).inner.lock() };
        d.desc_freelist.init();
        let wq = &raw mut d.desc_wait_queue;
        let lk = d.lock_ptr();
        if let Some(r) = TqRef::from_ptr(wq) { r.init(c"virtio_desc_wait".as_ptr(), lk); }
    }

    virtio_blkdev_init(diskno);
    // plic.rs and trap.rs arrange for interrupts from VIRTIO IRQs.
}

/// `void virtio_disk_init(void)`.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn virtio_disk_init() {
    // SAFETY: `platform` is populated by `fdt_apply_platform_config`
    // before this runs (boot-hart-main-init, strictly before
    // `start_kernel_post_init`, which is where this driver's init runs).
    let (has_virtio, virtio_count) = unsafe { (platform.has_virtio, platform.virtio_count) };
    if has_virtio == 0 || virtio_count == 0 {
        return;
    }

    let mut num_disks = virtio_count as usize;
    if num_disks > N_VIRTIO_DISK {
        num_disks = N_VIRTIO_DISK;
    }

    for i in 0..num_disks {
        virtio_disk_init_one(i);
    }
}
