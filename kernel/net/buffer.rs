//! Stable packet allocation, checked payload bounds and owning queues.

#![deny(unsafe_op_in_unsafe_fn)]

use core::ffi::{c_char, c_uint, c_void};
use core::ops::Range;
use core::ptr::{self, NonNull};

#[cfg(not(test))]
use crate::mm::kalloc::Kmem;
#[cfg(test)]
use tests::Kmem;


pub(crate) const MBUF_SIZE: usize = 2048;
pub(crate) const MBUF_DEFAULT_HEADROOM: usize = 128;

/// Stable packet storage shared with network drivers. Moving this value would
/// invalidate `head`, so allocation and ownership operate through `Packet`.
/// The layout remains compatible with the driver and generated bindings.
#[repr(C)]
pub struct Mbuf {
    pub next: *mut Mbuf,
    pub head: *mut c_char,
    pub len: c_uint,
    pub buf: [u8; MBUF_SIZE],
}

const _: () = {
    assert!(core::mem::size_of::<Mbuf>() == 2072, "mbuf size");
    assert!(core::mem::align_of::<Mbuf>() == 8, "mbuf alignment");
    assert!(core::mem::offset_of!(Mbuf, next) == 0, "mbuf.next offset");
    assert!(core::mem::offset_of!(Mbuf, head) == 8, "mbuf.head offset");
    assert!(core::mem::offset_of!(Mbuf, len) == 16, "mbuf.len offset");
    assert!(core::mem::offset_of!(Mbuf, buf) == 20, "mbuf.buf offset (DMA backing store)");
};

impl Mbuf {
    /// Allocate storage for a driver. Ownership must eventually return through
    /// `Packet::from_raw` or `Mbuf::free` after DMA has stopped accessing it.
    pub(crate) fn alloc(headroom: c_uint) -> *mut Self {
        Packet::allocate(headroom as usize).map_or(ptr::null_mut(), Packet::into_raw)
    }

    /// # Safety
    /// `packet` must be an exclusively owned allocation from `Mbuf::alloc` or
    /// `Packet::into_raw`, with no outstanding DMA access or references.
    pub(crate) unsafe fn free(packet: *mut Self) {
        // SAFETY: the caller transfers an exclusively owned allocator page.
        unsafe { Kmem::kfree(packet.cast()) };
    }
}

/// Owns a pinned packet allocation. Checked offsets describe the live bytes;
/// raw `head` and `len` are synchronized only when handing it back to a driver.
/// A failed parse, allocation, copy, or transmit releases the packet via Drop.
#[must_use]
pub(crate) struct Packet {
    storage: NonNull<Mbuf>,
    range: Range<usize>,
}

impl Packet {
    pub(crate) fn allocate(headroom: usize) -> Option<Self> {
        if headroom > MBUF_SIZE {
            return None;
        }
        // SAFETY: the allocator returns a fresh page or null; Mbuf fits in it.
        let storage = NonNull::new(unsafe { Kmem::kalloc() }.cast::<Mbuf>())?;
        // SAFETY: this page is exclusively owned, aligned, and large enough.
        // All Mbuf fields admit zero (integers, bytes and nullable pointers).
        // Initialize in place without a 2 KiB temporary on the kernel stack.
        unsafe { storage.as_ptr().write_bytes(0, 1) };
        Some(Self { storage, range: headroom..headroom })
    }

    /// Takes ownership of a driver's packet. Invalid bounds cause the packet
    /// to be freed, rather than allowing a corrupt descriptor to expose memory.
    ///
    /// # Safety
    /// `raw` must be null or an exclusively owned, fully initialized allocation
    /// returned by `Mbuf::alloc`/`Packet::into_raw`. DMA must have completed.
    pub(crate) unsafe fn from_raw(raw: *mut Mbuf) -> Option<Self> {
        let storage = NonNull::new(raw)?;
        // SAFETY: the caller relinquishes all accesses to this initialized Mbuf.
        let buffer = unsafe { storage.as_ref() };
        let start = (buffer.head as usize).checked_sub(buffer.buf.as_ptr() as usize);
        let range = start.and_then(|start| {
            let end = start.checked_add(buffer.len as usize)?;
            (end <= MBUF_SIZE).then_some(start..end)
        });
        match range {
            Some(range) => Some(Self { storage, range }),
            None => {
                // SAFETY: even invalid head/len metadata does not change page ownership.
                unsafe { Mbuf::free(raw) };
                None
            }
        }
    }

    pub(crate) fn into_raw(mut self) -> *mut Mbuf {
        // SAFETY: the allocation is exclusively owned and range is in bounds.
        let buffer = unsafe { self.storage.as_mut() };
        buffer.head = buffer.buf[self.range.start..].as_mut_ptr().cast();
        buffer.len = self.range.len() as c_uint;
        let raw = self.storage.as_ptr();
        core::mem::forget(self);
        raw
    }

    pub(crate) fn len(&self) -> usize {
        self.range.len()
    }

    /// Appends initialized bytes. Both headroom and current payload count
    /// against the backing store, unlike the former `mbufput` length check.
    pub(crate) fn append(&mut self, len: usize) -> Option<&mut [u8]> {
        let end = self.range.end.checked_add(len)?;
        if end > MBUF_SIZE {
            return None;
        }
        let start = self.range.end;
        self.range.end = end;
        // SAFETY: this owner is the only accessor and the checked range fits buf.
        Some(&mut unsafe { self.storage.as_mut() }.buf[start..end])
    }

    pub(crate) fn prepend(&mut self, header: &[u8]) -> Option<()> {
        // Check before pointer arithmetic, including underflow from headroom.
        let start = self.range.start.checked_sub(header.len())?;
        // SAFETY: exclusively owned storage; start..old_start is in bounds.
        unsafe { self.storage.as_mut() }.buf[start..self.range.start].copy_from_slice(header);
        self.range.start = start;
        Some(())
    }

    pub(crate) fn retain(&mut self, payload: Range<usize>) -> Option<()> {
        if payload.start > payload.end || payload.end > self.len() {
            return None;
        }
        let start = self.range.start;
        self.range = start + payload.start..start + payload.end;
        Some(())
    }
}

impl AsRef<[u8]> for Packet {
    fn as_ref(&self) -> &[u8] {
        // SAFETY: storage remains owned by self and the range is always in buf.
        &unsafe { self.storage.as_ref() }.buf[self.range.clone()]
    }
}

impl Drop for Packet {
    fn drop(&mut self) {
        // SAFETY: the only owner is being dropped; into_raw suppresses this Drop
        // when the ownership moves to a driver or the receive queue.
        unsafe { Mbuf::free(self.storage.as_ptr()) };
    }
}

/// An owning packet queue. Its layout remains compatible with VFS's socket
/// allocation while queue operations use Rust ownership and Option.
#[repr(C)]
pub struct MbufQueue {
    head: *mut Mbuf,
    tail: *mut Mbuf,
}

// SAFETY: the links own their packets. Moving the queue transfers ownership;
// callers serialize shared queue access with the socket's SpinLock.
unsafe impl Send for MbufQueue {}

impl MbufQueue {
    pub(crate) const fn new() -> Self {
        Self { head: ptr::null_mut(), tail: ptr::null_mut() }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.head.is_null()
    }

    pub(crate) fn push(&mut self, packet: Packet) {
        let raw = packet.into_raw();
        // SAFETY: raw is freshly transferred and every existing link belongs to
        // this exclusively borrowed queue. The tail is valid when nonempty.
        unsafe {
            (*raw).next = ptr::null_mut();
            if self.is_empty() {
                self.head = raw;
            } else {
                (*self.tail).next = raw;
            }
        }
        self.tail = raw;
    }

    pub(crate) fn pop(&mut self) -> Option<Packet> {
        let head = NonNull::new(self.head)?;
        // SAFETY: the nonempty queue owns head; remove it before transferring.
        self.head = unsafe { head.as_ref().next };
        if self.head.is_null() {
            self.tail = ptr::null_mut();
        }
        // SAFETY: this allocation was enqueued by push and is now unlinked.
        unsafe { Packet::from_raw(head.as_ptr()) }
    }


}

impl Drop for MbufQueue {
    fn drop(&mut self) {
        while self.pop().is_some() {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::alloc::{alloc, dealloc, Layout};
    use std::cell::Cell;

    std::thread_local! {
        static LIVE_PAGES: Cell<usize> = const { Cell::new(0) };
    }

    // Exercise the actual ownership and bounds implementation on host pages.
    // Only the kernel's physical page allocator is substituted.
    pub(super) struct Kmem;

    impl Kmem {
        pub(super) unsafe fn kalloc() -> *mut c_void {
            // SAFETY: the fixed page layout is nonzero and well aligned.
            let page = unsafe { alloc(Layout::from_size_align(4096, 4096).unwrap()).cast::<c_void>() };
            if !page.is_null() {
                LIVE_PAGES.set(LIVE_PAGES.get() + 1);
            }
            page
        }

        pub(super) unsafe fn kfree(page: *mut c_void) {
            LIVE_PAGES.set(LIVE_PAGES.get() - 1);
            // SAFETY: Packet only returns pages from the matching allocator.
            unsafe { dealloc(page.cast(), Layout::from_size_align(4096, 4096).unwrap()) };
        }
    }

    #[test]
    fn append_accounts_for_headroom_and_preserves_state_on_failure() {
        let mut packet = Packet::allocate(MBUF_DEFAULT_HEADROOM).unwrap();
        let capacity = MBUF_SIZE - MBUF_DEFAULT_HEADROOM;
        packet.append(capacity).unwrap().fill(0xab);
        assert_eq!(packet.len(), capacity);
        assert!(packet.append(1).is_none());
        assert!(packet.append(usize::MAX).is_none());
        assert_eq!(packet.as_ref(), &[0xab; MBUF_SIZE - MBUF_DEFAULT_HEADROOM]);
        assert!(packet.prepend(&[0; MBUF_DEFAULT_HEADROOM + 1]).is_none());
        packet.prepend(&[0xcd; MBUF_DEFAULT_HEADROOM]).unwrap();
        assert_eq!(packet.len(), MBUF_SIZE);
        assert!(packet.prepend(&[0]).is_none());
        assert_eq!(&packet.as_ref()[..MBUF_DEFAULT_HEADROOM], &[0xcd; MBUF_DEFAULT_HEADROOM]);
    }

    #[test]
    fn packet_handoff_preserves_bytes_and_pinned_storage() {
        let mut packet = Packet::allocate(MBUF_DEFAULT_HEADROOM).unwrap();
        packet.append(4).unwrap().copy_from_slice(b"test");
        let raw = packet.into_raw();
        // SAFETY: raw has just transferred into this test, with no DMA users.
        let packet = unsafe { Packet::from_raw(raw) }.unwrap();
        assert_eq!(packet.as_ref(), b"test");
        assert_eq!(packet.storage.as_ptr(), raw);
    }

    #[test]
    fn invalid_driver_lengths_are_rejected_before_slice_creation() {
        let raw = Mbuf::alloc(MBUF_DEFAULT_HEADROOM as c_uint);
        // SAFETY: the test owns this initialized raw allocation.
        unsafe { (*raw).len = MBUF_SIZE as c_uint };
        // SAFETY: the owned allocation is valid; malformed metadata is checked.
        assert!(unsafe { Packet::from_raw(raw) }.is_none());
        assert!(Packet::allocate(MBUF_SIZE + 1).is_none());
        let mut empty = Packet::allocate(MBUF_SIZE).unwrap();
        assert!(empty.append(1).is_none());
        assert_eq!(empty.append(0).unwrap(), &[]);
    }

    #[test]
    fn retain_rejects_reversed_or_out_of_bounds_payloads() {
        let mut packet = Packet::allocate(0).unwrap();
        packet.append(6).unwrap().copy_from_slice(b"abcdef");
        let reversed = Range { start: 5, end: 2 };
        assert!(packet.retain(reversed).is_none());
        assert!(packet.retain(0..7).is_none());
        assert_eq!(packet.as_ref(), b"abcdef");
        packet.retain(1..5).unwrap();
        assert_eq!(packet.as_ref(), b"bcde");
    }

    #[test]
    fn queue_transfers_packets_and_resets_both_links() {
        let mut queue = MbufQueue::new();
        assert!(queue.pop().is_none());
        for byte in 0..3 {
            let mut packet = Packet::allocate(0).unwrap();
            packet.append(1).unwrap()[0] = byte;
            queue.push(packet);
        }
        for byte in 0..3 {
            assert_eq!(queue.pop().unwrap().as_ref(), &[byte]);
        }
        assert!(queue.is_empty());
        assert!(queue.tail.is_null());
        queue.push(Packet::allocate(0).unwrap());
        assert!(queue.pop().is_some());
        assert!(queue.pop().is_none());
    }

    #[test]
    fn dropping_queue_releases_pending_packets() {
        let before = LIVE_PAGES.get();
        {
            let mut queue = MbufQueue::new();
            for _ in 0..3 {
                queue.push(Packet::allocate(0).unwrap());
            }
            assert_eq!(LIVE_PAGES.get(), before + 3);
        }
        assert_eq!(LIVE_PAGES.get(), before);
    }
}
