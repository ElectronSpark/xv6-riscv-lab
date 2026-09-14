//! VirtIO MMIO block driver with checked descriptor ownership.
//!
//! `transport` owns the three DMA pages and the volatile register/ring
//! boundary. `queue` allocates complete descriptor chains without sentinel
//! integers. The disk lock owns CPU request metadata; device-written status
//! bytes and device-read headers live separately inside `UnsafeCell`.
//!
//! Descriptor publication, interrupt consumption and descriptor release keep
//! their hardware fences. DMA memory is never borrowed as a Rust reference.
//! A pending request retains its `BioPart` until device completion, so a
//! multi-segment bio completes only after every segment and submission finish.

#![deny(unsafe_op_in_unsafe_fn)]

use core::cell::UnsafeCell;
use core::ffi::{c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{compiler_fence, fence, AtomicBool, AtomicU32, Ordering};

use crate::bindings::{bio, blkdev_t, device_t, mode_t, tq_t};
use crate::dev::bio::{Bio, BioPart};
use crate::dev::blkdev::{Blkdev, BlkdevOps};
use crate::dev::fdt::platform;
use crate::irq::irq_core::{IrqCore, IrqDesc, IrqHandler};
use crate::kstd::{Errno, KResult};
use crate::machine::Riscv;
use crate::printf::Printf;
use crate::proc::access::TqRef;
use crate::proc::proc_shims::xv6_current_thread;
use crate::sync::SpinLock;

mod queue;
#[cfg(feature = "bio_test")]
mod runtime_test;
mod transport;
use queue::{DescriptorChain, DescriptorIndex, DescriptorPool, NUM};
use transport::{Mmio, QueueMemory, Register};

const N_VIRTIO_DISK: usize = 2;
const S_IFBLK: u32 = 0o060_000;
const THREAD_UNINTERRUPTIBLE: u32 = 6;
const STATUS_ACKNOWLEDGE: u32 = 1;
const STATUS_DRIVER: u32 = 2;
const STATUS_DRIVER_OK: u32 = 4;
const STATUS_FEATURES_OK: u32 = 8;
const DESC_NEXT: u16 = 1;
const DESC_WRITE: u16 = 2;

// Preserve the existing feature negotiation: RO, SCSI, CONFIG_WCE, MQ,
// ANY_LAYOUT, INDIRECT_DESC and EVENT_IDX are not supported by this driver.
const UNSUPPORTED_FEATURES: u32 =
    (1 << 5) | (1 << 7) | (1 << 11) | (1 << 12) | (1 << 27) | (1 << 28) | (1 << 29);

/// Native `struct virtq_desc` (`kernel/inc/dev/virtio.h`) — a single
/// descriptor, from the virtio spec.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtqDesc {
    /// Buffer physical address.
    pub addr: u64,
    /// Buffer length.
    pub len: u32,
    /// `VRING_DESC_F_NEXT` / `VRING_DESC_F_WRITE`.
    pub flags: u16,
    /// Next descriptor index (if `VRING_DESC_F_NEXT`).
    pub next: u16,
}

/// Native `struct virtq_avail` (`kernel/inc/dev/virtio.h`) — the
/// (entire) avail ring, from the virtio spec.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtqAvail {
    /// Always zero.
    pub flags: u16,
    /// Driver will write `ring[idx]` next.
    pub idx: u16,
    /// Descriptor numbers of chain heads.
    pub ring: [u16; NUM],
    /// Trailing avail-event slot (unused).
    pub unused: u16,
}

/// Native `struct virtq_used_elem` (`kernel/inc/dev/virtio.h`) — one
/// entry in the used ring: the device tells the driver about a
/// completed request.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtqUsedElem {
    /// Index of start of completed descriptor chain.
    pub id: u32,
    /// Bytes written.
    pub len: u32,
}

/// Native `struct virtq_used` (`kernel/inc/dev/virtio.h`) — the
/// (entire) used ring.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct VirtqUsed {
    /// Always zero.
    pub flags: u16,
    /// Device increments when it adds a `ring[]` entry.
    pub idx: u16,
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
    pub type_: u32,
    /// Reserved (zero).
    pub reserved: u32,
    /// Sector number (512-byte units).
    pub sector: u64,
}

// P3-4c hardcoded layout proof — the virtio 1.x split-virtqueue byte
// contract (`kernel/inc/dev/virtio.h`), every field of all five
// records. Values captured from the pre-nativization bindgen output
// via the temporary in-tree `offset_of!` gate and cross-checked by the
// toolchain-gcc probe.
const _: () = {
    use core::mem::{align_of, offset_of, size_of};
    assert!(
        size_of::<VirtqDesc>() == 16,
        "virtq_desc size (DEVICE-READ)"
    );
    assert!(align_of::<VirtqDesc>() == 8, "virtq_desc alignment");
    assert!(
        offset_of!(VirtqDesc, addr) == 0,
        "vd.addr offset (DEVICE-READ)"
    );
    assert!(
        offset_of!(VirtqDesc, len) == 8,
        "vd.len offset (DEVICE-READ)"
    );
    assert!(
        offset_of!(VirtqDesc, flags) == 12,
        "vd.flags offset (DEVICE-READ)"
    );
    assert!(
        offset_of!(VirtqDesc, next) == 14,
        "vd.next offset (DEVICE-READ)"
    );

    assert!(
        size_of::<VirtqAvail>() == 134,
        "virtq_avail size (DEVICE-READ)"
    );
    assert!(align_of::<VirtqAvail>() == 2, "virtq_avail alignment");
    assert!(
        offset_of!(VirtqAvail, flags) == 0,
        "va.flags offset (DEVICE-READ)"
    );
    assert!(
        offset_of!(VirtqAvail, idx) == 2,
        "va.idx offset (DEVICE-READ)"
    );
    assert!(
        offset_of!(VirtqAvail, ring) == 4,
        "va.ring offset (DEVICE-READ)"
    );
    assert!(offset_of!(VirtqAvail, unused) == 132, "va.unused offset");
    assert!(NUM == 64, "ring lengths == NUM (DEVICE-READ)");

    assert!(
        size_of::<VirtqUsedElem>() == 8,
        "virtq_used_elem size (DEVICE-WRITTEN)"
    );
    assert!(
        align_of::<VirtqUsedElem>() == 4,
        "virtq_used_elem alignment"
    );
    assert!(
        offset_of!(VirtqUsedElem, id) == 0,
        "vue.id offset (DEVICE-WRITTEN)"
    );
    assert!(
        offset_of!(VirtqUsedElem, len) == 4,
        "vue.len offset (DEVICE-WRITTEN)"
    );

    assert!(
        size_of::<VirtqUsed>() == 516,
        "virtq_used size (DEVICE-WRITTEN)"
    );
    assert!(align_of::<VirtqUsed>() == 4, "virtq_used alignment");
    assert!(
        offset_of!(VirtqUsed, flags) == 0,
        "vu.flags offset (DEVICE-WRITTEN)"
    );
    assert!(
        offset_of!(VirtqUsed, idx) == 2,
        "vu.idx offset (DEVICE-WRITTEN)"
    );
    assert!(
        offset_of!(VirtqUsed, ring) == 4,
        "vu.ring offset (DEVICE-WRITTEN)"
    );

    assert!(
        size_of::<VirtioBlkReq>() == 16,
        "virtio_blk_req size (DEVICE-READ)"
    );
    assert!(align_of::<VirtioBlkReq>() == 8, "virtio_blk_req alignment");
    assert!(
        offset_of!(VirtioBlkReq, type_) == 0,
        "br.type offset (DEVICE-READ)"
    );
    assert!(
        offset_of!(VirtioBlkReq, reserved) == 4,
        "br.reserved offset (DEVICE-READ)"
    );
    assert!(
        offset_of!(VirtioBlkReq, sector) == 8,
        "br.sector offset (DEVICE-READ)"
    );
};

// Boot probing writes these tables before VM mapping and disk initialization.
pub(crate) static mut __virtio_mmio_base: [u64; 3] = [0x10001000, 0x10002000, 0x10003000];
pub(crate) static mut __virtio_irqno: [u64; 3] = [1, 2, 3];

#[repr(C)]
#[derive(Clone, Copy)]
struct RequestDma {
    header: VirtioBlkReq,
    status: u8,
}

impl RequestDma {
    const EMPTY: Self = Self {
        header: VirtioBlkReq {
            type_: 0,
            reserved: 0,
            sector: 0,
        },
        status: 0,
    };
}

struct PendingRequest {
    chain: DescriptorChain,
    part: BioPart,
}

struct DiskInner {
    memory: QueueMemory,
    descriptors: DescriptorPool,
    pending: [Option<PendingRequest>; NUM],
    used_index: u16,
    descriptor_waiters: tq_t,
}

// SAFETY: all CPU queue state, including the wait queue's raw links, is
// accessed under the enclosing SpinLock. BioPart permits ownership transfer
// to an IRQ hart; DMA allocations are independently owned by QueueMemory.
unsafe impl Send for DiskInner {}

pub(crate) struct Disk {
    mmio: Mmio,
    requests: UnsafeCell<[RequestDma; NUM]>,
    inner: SpinLock<DiskInner>,
}

// SAFETY: CPU accesses to requests are serialized by inner, and touch only
// descriptor slots currently owned by the CPU. Device-written status bytes
// are read through volatile pointers after the used-ring completion fence.
// No reference to an element of requests is exposed, including during DMA.
unsafe impl Sync for Disk {}

struct DiskSlot {
    value: UnsafeCell<MaybeUninit<Disk>>,
    initialized: AtomicBool,
}

// SAFETY: install is an unsafe boot-only, once-per-slot operation. After its
// release publication the Disk never moves or drops, and Disk is Sync.
unsafe impl Sync for DiskSlot {}

impl DiskSlot {
    const fn new() -> Self {
        Self {
            value: UnsafeCell::new(MaybeUninit::uninit()),
            initialized: AtomicBool::new(false),
        }
    }

    /// # Safety
    /// Only the boot hart may call this, exactly once for this permanent slot,
    /// before registering the corresponding block device or interrupt handler.
    unsafe fn install(&'static self, disk: Disk) -> &'static Disk {
        assert!(
            !self.initialized.load(Ordering::Relaxed),
            "virtio disk initialized twice"
        );
        let slot = self.value.get().cast::<Disk>();
        // SAFETY: the caller owns this uninitialized permanent slot exclusively.
        unsafe { slot.write(disk) };
        // SAFETY: the preceding write initialized the complete Disk value;
        // it remains at this address for the lifetime of the kernel.
        let disk = unsafe { &*slot };
        {
            let mut state = disk.inner.lock();
            let waiters = &raw mut state.descriptor_waiters;
            let lock = state.lock_ptr();
            TqRef::from_ptr(waiters)
                .expect("virtio descriptor queue")
                .init(c"virtio_desc_wait".as_ptr(), lock);
        }
        self.initialized.store(true, Ordering::Release);
        disk
    }

    fn get(&'static self) -> &'static Disk {
        assert!(
            self.initialized.load(Ordering::Acquire),
            "virtio disk not initialized"
        );
        // SAFETY: the acquire observes install's fully initialized permanent
        // value; no later writer replaces the Disk or its immutable fields.
        unsafe { &*self.value.get().cast::<Disk>() }
    }
}

static DISKS: [DiskSlot; N_VIRTIO_DISK] = [const { DiskSlot::new() }; N_VIRTIO_DISK];

struct DeviceSlots(UnsafeCell<[MaybeUninit<blkdev_t>; N_VIRTIO_DISK]>);
// SAFETY: boot initializes each slot once before registration. Thereafter
// device-core lifecycle/locking governs all access to its raw device object;
// this wrapper never constructs an aliased mutable reference to the array.
unsafe impl Sync for DeviceSlots {}

impl DeviceSlots {
    fn slot(&self, index: usize) -> *mut blkdev_t {
        assert!(index < N_VIRTIO_DISK, "virtio block device index");
        self.0.get().cast::<blkdev_t>().wrapping_add(index)
    }
}

static DEVICES: DeviceSlots = DeviceSlots(UnsafeCell::new(
    [const { MaybeUninit::uninit() }; N_VIRTIO_DISK],
));
const DISK_NAMES: [&core::ffi::CStr; N_VIRTIO_DISK] = [c"disk0", c"disk1"];

impl Disk {
    #[cfg(feature = "bio_test")]
    pub(crate) fn run_tests() {
        runtime_test::run_tests();
    }

    fn at(index: usize) -> &'static Self {
        DISKS.get(index).expect("virtio disk index").get()
    }

    #[cold]
    fn fail(message: core::fmt::Arguments<'_>) -> ! {
        Printf::__panic_start();
        crate::kprintln!("{}", message);
        Printf::__panic_end()
    }

    fn submit(&self, part: BioPart) {
        let mut state = self.inner.lock();
        let chain = loop {
            // Keep the compiler barrier before descriptor allocation.
            compiler_fence(Ordering::SeqCst);
            if let Some(chain) = state.descriptors.allocate() {
                break chain;
            }
            // SAFETY: the scheduler returns the current live thread. Its state
            // is an atomic field; descriptor starvation is uninterruptible.
            unsafe {
                let current = xv6_current_thread();
                if !current.is_null() {
                    AtomicU32::from_ptr(&raw mut (*current).state)
                        .store(THREAD_UNINTERRUPTIBLE, Ordering::SeqCst);
                }
            }
            let waiters = &raw mut state.descriptor_waiters;
            // SAFETY: install initialized this queue against this exact lock;
            // wait_on releases and reacquires the guard without lost wakeups.
            unsafe { state.wait_on(waiters, ptr::null_mut()) };
        };

        let head = chain.head();
        let [_, data, result] = chain.indices();
        let request = self
            .requests
            .get()
            .cast::<RequestDma>()
            .wrapping_add(head.slot());
        // SAFETY: the allocated head index is in bounds. UnsafeCell permits
        // raw access without borrowing the device-shared request array.
        let (header, status) = unsafe { (&raw mut (*request).header, &raw mut (*request).status) };
        let operation = VirtioBlkReq {
            type_: if part.write() { 1 } else { 0 },
            reserved: 0,
            sector: part.sector(),
        };
        // SAFETY: this head slot is exclusively allocated and unpublished, so
        // the device cannot read its header or write its status yet.
        unsafe {
            ptr::write_volatile(header, operation);
            ptr::write_volatile(status, 0xff);
        }
        let descriptors = [
            VirtqDesc {
                addr: header as u64,
                len: core::mem::size_of::<VirtioBlkReq>() as u32,
                flags: DESC_NEXT,
                next: data.raw(),
            },
            VirtqDesc {
                addr: part.data_ptr() as u64,
                len: part.len() as u32,
                flags: DESC_NEXT | if part.write() { 0 } else { DESC_WRITE },
                next: result.raw(),
            },
            VirtqDesc {
                addr: status as u64,
                len: 1,
                flags: DESC_WRITE,
                next: 0,
            },
        ];
        // SAFETY: this chain is exclusively allocated. The permanent Disk
        // owns header/status and BioPart retains the checked payload through
        // completion. Each descriptor describes the corresponding buffer.
        unsafe { state.memory.prepare(&chain, descriptors) };
        assert!(
            state.pending[head.slot()].is_none(),
            "virtio occupied request slot"
        );
        state.pending[head.slot()] = Some(PendingRequest { chain, part });
        // SAFETY: pending now owns the prepared chain and its BioPart; neither
        // is released until this device reports the head in its used ring.
        unsafe { state.memory.publish(head) };
        debug_assert!(
            !Riscv::intr_get(),
            "virtio submission with interrupts enabled"
        );
        self.mmio.write(Register::QueueNotify, 0);
    }

    fn interrupt(&self) {
        let mut state = self.inner.lock();
        self.mmio.acknowledge_interrupt();
        loop {
            let cursor = state.used_index;
            let Some(id) = state.memory.next_used(cursor) else {
                break;
            };
            let Some(head) = DescriptorIndex::from_device(id) else {
                Self::fail(format_args!("virtio_disk_intr: invalid descriptor {}", id));
            };
            let Some(PendingRequest { chain, part }) = state.pending[head.slot()].take() else {
                Self::fail(format_args!(
                    "virtio_disk_intr: descriptor {} is not pending",
                    id
                ));
            };
            let request = self
                .requests
                .get()
                .cast::<RequestDma>()
                .wrapping_add(head.slot());
            // SAFETY: the checked head has a pending request in this Disk.
            // next_used fenced after observing completion; the device has
            // stopped writing this status byte and relinquished the chain.
            let status = unsafe { ptr::read_volatile(&raw const (*request).status) };
            // SAFETY: the used entry reports completion of this exact chain.
            unsafe { state.memory.clear(&chain) };
            state.descriptors.release(chain);
            if state.descriptors.has_chain() {
                let _ = TqRef::from_ptr(&raw mut state.descriptor_waiters)
                    .map(|waiters| waiters.wakeup_all(0, 0));
            }
            state.used_index = cursor.wrapping_add(1);
            fence(Ordering::SeqCst);

            // Completion may invoke a callback or release the bio. It must not
            // run under the disk lock or borrow device-shared queue memory.
            drop(state);
            part.complete(if status == 0 { Ok(()) } else { Err(Errno::Io) });
            state = self.inner.lock();
        }
    }

    fn register(diskno: usize) {
        let dev = DEVICES.slot(diskno);
        // SAFETY: this is the boot-owned, unpublished device slot. Blkdev's
        // documented empty representation has zero integers/null pointers and
        // None trait-object options; zeroing raw storage creates that state.
        unsafe { ptr::write_bytes(dev, 0, 1) };
        // SAFETY: dev now holds a valid, uniquely owned empty block device.
        unsafe {
            (*dev).dev.major = 2;
            (*dev).dev.minor = diskno as c_int + 1;
            (*dev).dev.devname = DISK_NAMES[diskno].as_ptr();
            (*dev).dev.devmode = (S_IFBLK | 0o600) as mode_t;
            (*dev).flags.set_readable(1);
            (*dev).flags.set_writable(1);
            (*dev).block_shift = 0;
            (*dev).ops = Some(&VIRTIO_DISK_OPS);
        }
        let error = Blkdev::register(dev);
        if error != 0 {
            Self::fail(format_args!(
                "virtio_blkdev_init: blkdev_register failed: {}",
                error
            ));
        }

        // SAFETY: dev is a permanent registered device; the IRQ table was
        // fixed at boot. Preserve the original first-IRQ + disk-index mapping.
        let (device, irq) = unsafe {
            (
                &raw mut (*dev).dev as *mut device_t,
                (__virtio_irqno[0] + diskno as u64) as c_int,
            )
        };
        let mut descriptor = IrqDesc {
            handler: Some(&VIRTIO_DISK_IRQ_HANDLER),
            data: diskno as *mut c_void,
            dev: device,
            irq: 0,
            count: 0,
            // SAFETY: the unqueued RCU head consists of null links/callback.
            rcu_head: unsafe { core::mem::zeroed() },
        };
        // SAFETY: the initialized descriptor is live for the registration
        // call, which copies it; its device/data identify permanent storage.
        let error =
            unsafe { IrqCore::register_irq_handler(IrqCore::plic_irq(irq), &raw mut descriptor) };
        if error != 0 {
            Self::fail(format_args!(
                "virtio_blkdev_init: register_irq_handler failed: {}",
                error
            ));
        }
    }

    fn init_one(diskno: usize) {
        // SAFETY: boot probing has finished and VM setup mapped the VirtIO
        // pages. This boot-only driver owns each probed device's registers.
        let mmio = unsafe { Mmio::from_mapped_base(__virtio_mmio_base[diskno]) };
        if mmio.read(Register::Magic) != 0x74726976
            || mmio.read(Register::Version) != 2
            || mmio.read(Register::DeviceId) != 2
            || mmio.read(Register::VendorId) != 0x554d4551
        {
            Self::fail(format_args!("could not find virtio disk {}", diskno));
        }
        mmio.write(Register::Status, 0);
        let mut status = STATUS_ACKNOWLEDGE;
        mmio.write(Register::Status, status);
        status |= STATUS_DRIVER;
        mmio.write(Register::Status, status);
        let features = mmio.read(Register::DeviceFeatures) & !UNSUPPORTED_FEATURES;
        mmio.write(Register::DriverFeatures, features);
        status |= STATUS_FEATURES_OK;
        mmio.write(Register::Status, status);
        status = mmio.read(Register::Status);
        if status & STATUS_FEATURES_OK == 0 {
            Self::fail(format_args!("virtio disk {} FEATURES_OK unset", diskno));
        }
        mmio.write(Register::QueueSelect, 0);
        if mmio.read(Register::QueueReady) != 0 {
            Self::fail(format_args!("virtio disk {} should not be ready", diskno));
        }
        match mmio.read(Register::QueueMax) {
            0 => Self::fail(format_args!("virtio disk {} has no queue 0", diskno)),
            max if max < NUM as u32 => {
                Self::fail(format_args!("virtio disk {} max queue too short", diskno))
            }
            _ => {}
        }
        let memory = QueueMemory::allocate()
            .unwrap_or_else(|_| Self::fail(format_args!("virtio disk {} kalloc", diskno)));
        let disk = Self {
            mmio,
            requests: UnsafeCell::new([RequestDma::EMPTY; NUM]),
            inner: SpinLock::new(
                c"virtio_disk",
                DiskInner {
                    memory,
                    descriptors: DescriptorPool::new(),
                    pending: core::array::from_fn(|_| None),
                    used_index: 0,
                    // SAFETY: an uninitialized Tq consists solely of zero-valid
                    // integer/pointer fields; install initializes its links before use.
                    descriptor_waiters: unsafe { core::mem::zeroed() },
                },
            ),
        };
        // SAFETY: boot calls once per bounded disk index before registration;
        // the slot gives the request DMA buffers their final permanent address.
        let disk = unsafe { DISKS[diskno].install(disk) };
        {
            let state = disk.inner.lock();
            // SAFETY: the installed disk permanently owns all three queue pages.
            unsafe { disk.mmio.configure_queue(&state.memory) };
        }
        disk.mmio.write(Register::QueueReady, 1);
        status |= STATUS_DRIVER_OK;
        disk.mmio.write(Register::Status, status);
        Self::register(diskno);
    }

    pub(crate) extern "C" fn init() {
        // SAFETY: boot has finalized platform configuration before this call.
        let (present, count) = unsafe { (platform.has_virtio, platform.virtio_count) };
        if present == 0 {
            return;
        }
        for index in 0..(count as usize).min(N_VIRTIO_DISK) {
            Self::init_one(index);
        }
    }
}

struct VirtioDiskIrqHandler;
static VIRTIO_DISK_IRQ_HANDLER: VirtioDiskIrqHandler = VirtioDiskIrqHandler;

impl IrqHandler for VirtioDiskIrqHandler {
    unsafe fn handle(&self, _irq: c_int, data: *mut c_void, _dev: *mut c_void) {
        Disk::at(data as usize).interrupt();
    }
}

struct VirtioDiskOps;
static VIRTIO_DISK_OPS: VirtioDiskOps = VirtioDiskOps;

impl BlkdevOps for VirtioDiskOps {
    unsafe fn open(&self, _blkdev: *mut blkdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn release(&self, _blkdev: *mut blkdev_t) -> KResult<()> {
        Ok(())
    }

    unsafe fn submit_bio(&self, blkdev: *mut blkdev_t, bio: *mut bio) -> KResult<()> {
        // SAFETY: the block-device layer supplies this live registered device;
        // registration assigns minors 1..=N_VIRTIO_DISK.
        let diskno = unsafe { (*blkdev).dev.minor }
            .checked_sub(1)
            .and_then(|minor| usize::try_from(minor).ok())
            .expect("virtio block device minor");
        let disk = Disk::at(diskno);
        // SAFETY: caller retains a live validated bio. begin pins its metadata,
        // and each yielded BioPart retains the data needed through completion.
        let request = unsafe { Bio::begin(bio) }?;
        for part in request {
            disk.submit(part);
        }
        Ok(())
    }
}
