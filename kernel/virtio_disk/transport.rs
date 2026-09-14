//! The volatile MMIO and DMA boundary for the split virtqueue.

use core::ptr;
use core::sync::atomic::{fence, Ordering};

use super::queue::{DescriptorChain, DescriptorIndex, NUM};
use super::{VirtqAvail, VirtqDesc, VirtqUsed};
use crate::kstd::{Errno, KResult};
use crate::mm::mm_safe::{PageHandle, BUDDY};
use crate::mm::page::PAGE_TYPE_ANON;

const PAGE_SIZE: usize = crate::bindings::PGSIZE as usize;

#[derive(Clone, Copy)]
#[repr(usize)]
pub(super) enum Register {
    Magic = 0x000,
    Version = 0x004,
    DeviceId = 0x008,
    VendorId = 0x00c,
    DeviceFeatures = 0x010,
    DriverFeatures = 0x020,
    QueueSelect = 0x030,
    QueueMax = 0x034,
    QueueSize = 0x038,
    QueueReady = 0x044,
    QueueNotify = 0x050,
    InterruptStatus = 0x060,
    InterruptAck = 0x064,
    Status = 0x070,
    DescriptorLow = 0x080,
    DescriptorHigh = 0x084,
    AvailableLow = 0x090,
    AvailableHigh = 0x094,
    UsedLow = 0x0a0,
    UsedHigh = 0x0a4,
}

pub(super) struct Mmio {
    base: usize,
}

impl Mmio {
    /// # Safety
    /// `base` must identify the permanently mapped register page of this
    /// VirtIO device. Boot probing must be finished, and this driver must
    /// retain exclusive control of queue configuration for its lifetime.
    pub(super) unsafe fn from_mapped_base(base: u64) -> Self {
        let base = usize::try_from(base).expect("virtio MMIO address width");
        assert!(base != 0 && base % 4 == 0, "virtio MMIO alignment");
        assert!(
            base.checked_add(PAGE_SIZE).is_some(),
            "virtio MMIO address overflow"
        );
        Self { base }
    }

    pub(super) fn read(&self, register: Register) -> u32 {
        // SAFETY: construction proves this register page remains mapped;
        // every enum discriminant is an aligned in-page u32 register.
        unsafe { ptr::read_volatile((self.base + register as usize) as *const u32) }
    }

    pub(super) fn write(&self, register: Register, value: u32) {
        // SAFETY: same mapped-page invariant as read. Driver methods serialize
        // queue configuration and publication with the disk's lock.
        unsafe { ptr::write_volatile((self.base + register as usize) as *mut u32, value) }
    }

    /// # Safety
    /// The queue pages must remain alive until the device is stopped. This
    /// driver satisfies that by storing them in a permanent initialized disk.
    pub(super) unsafe fn configure_queue(&self, memory: &QueueMemory) {
        self.write(Register::QueueSize, NUM as u32);
        for (address, low, high) in [
            (
                memory.desc.data_ptr() as u64,
                Register::DescriptorLow,
                Register::DescriptorHigh,
            ),
            (
                memory.avail.data_ptr() as u64,
                Register::AvailableLow,
                Register::AvailableHigh,
            ),
            (
                memory.used.data_ptr() as u64,
                Register::UsedLow,
                Register::UsedHigh,
            ),
        ] {
            self.write(low, address as u32);
            self.write(high, (address >> 32) as u32);
        }
    }

    pub(super) fn acknowledge_interrupt(&self) {
        self.write(
            Register::InterruptAck,
            self.read(Register::InterruptStatus) & 0x3,
        );
        // Observe the device's used ring only after acknowledging its IRQ.
        fence(Ordering::SeqCst);
    }
}

/// Owns the three pages until the permanent disk itself is destroyed. Early
/// allocation failure drops the already allocated handles before publication.
pub(super) struct QueueMemory {
    desc: PageHandle<'static>,
    avail: PageHandle<'static>,
    used: PageHandle<'static>,
}

impl QueueMemory {
    fn allocate_page() -> KResult<PageHandle<'static>> {
        let page = BUDDY.alloc(0, PAGE_TYPE_ANON).ok_or(Errno::NoMem)?;
        // SAFETY: this exclusively owned, unpublished page has PAGE_SIZE
        // writable bytes; initialize all of them before typed DMA access.
        unsafe { ptr::write_bytes(page.data_ptr(), 0, PAGE_SIZE) };
        Ok(page)
    }

    pub(super) fn allocate() -> KResult<Self> {
        Ok(Self {
            desc: Self::allocate_page()?,
            avail: Self::allocate_page()?,
            used: Self::allocate_page()?,
        })
    }

    /// # Safety
    /// The caller exclusively owns this unsubmitted chain. Every descriptor
    /// address must designate a DMA buffer valid in the declared direction
    /// until the device reports this chain complete.
    pub(super) unsafe fn prepare(&mut self, chain: &DescriptorChain, values: [VirtqDesc; 3]) {
        for (index, value) in chain.indices().into_iter().zip(values) {
            // SAFETY: checked indices fit the allocated descriptor page;
            // caller owns these slots until publishing the chain.
            unsafe {
                ptr::write_volatile(
                    self.desc.data_ptr().cast::<VirtqDesc>().add(index.slot()),
                    value,
                )
            };
        }
    }

    /// # Safety
    /// `head` must belong to a prepared, unpublished chain whose buffers stay
    /// live until completion. QueueMemory itself must remain pinned and live
    /// while the device can consume this entry.
    pub(super) unsafe fn publish(&mut self, head: DescriptorIndex) {
        let avail = self.avail.data_ptr().cast::<VirtqAvail>();
        // SAFETY: avail is an owned, aligned DMA page; the CPU alone writes
        // these fields and the enclosing disk lock serializes submissions.
        let index = unsafe { ptr::read_volatile(&raw const (*avail).idx) };
        // SAFETY: modulo bounds the ring slot; no Rust reference aliases DMA.
        unsafe { ptr::write_volatile(&raw mut (*avail).ring[index as usize % NUM], head.raw()) };
        // Make headers/descriptors/ring entry visible before publishing idx.
        fence(Ordering::SeqCst);
        // SAFETY: same owned page and CPU-writer invariant as above.
        unsafe { ptr::write_volatile(&raw mut (*avail).idx, index.wrapping_add(1)) };
        // Publish the index before the caller notifies the device.
        fence(Ordering::SeqCst);
    }

    pub(super) fn next_used(&mut self, cursor: u16) -> Option<u32> {
        let used = self.used.data_ptr().cast::<VirtqUsed>();
        // SAFETY: used remains an allocated DMA page. Volatile forces a fresh
        // read of the device's producer index on every polling iteration.
        if cursor == unsafe { ptr::read_volatile(&raw const (*used).idx) } {
            return None;
        }
        // Read the completed entry only after observing its producer index.
        fence(Ordering::SeqCst);
        // SAFETY: modulo bounds the slot in the owned DMA page. The caller
        // checks the untrusted descriptor ID before using it as an index.
        Some(unsafe { ptr::read_volatile(&raw const (*used).ring[cursor as usize % NUM].id) })
    }

    /// # Safety
    /// The device must have reported completion of this outstanding chain.
    pub(super) unsafe fn clear(&mut self, chain: &DescriptorChain) {
        for index in chain.indices() {
            // SAFETY: checked index fits the owned page; device completion
            // returns exclusive write ownership of this descriptor to us.
            unsafe {
                ptr::write_volatile(
                    self.desc.data_ptr().cast::<VirtqDesc>().add(index.slot()),
                    VirtqDesc {
                        addr: 0,
                        len: 0,
                        flags: 0,
                        next: 0,
                    },
                );
            }
            // Preserve the descriptor-release fence before waking waiters.
            fence(Ordering::SeqCst);
        }
    }
}

const _: () = {
    assert!(core::mem::size_of::<VirtqDesc>() * NUM <= PAGE_SIZE);
    assert!(core::mem::size_of::<VirtqAvail>() <= PAGE_SIZE);
    assert!(core::mem::size_of::<VirtqUsed>() <= PAGE_SIZE);
};
