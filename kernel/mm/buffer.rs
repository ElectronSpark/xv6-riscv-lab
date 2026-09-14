//! Initialized, owned scratch bytes for bounded kernel operations.
//!
//! The existing small-object allocator supports requests up to one page.
//! This owner preserves that limit, exposes initialized byte slices, and frees
//! storage on every exit path. It does not adopt arbitrary raw allocations.

#![deny(unsafe_op_in_unsafe_fn)]

use core::ops::{Deref, DerefMut};
use core::ptr::NonNull;

#[cfg(not(test))]
use crate::mm::kalloc::Kmem;
#[cfg(test)]
use tests::Kmem;

pub(crate) const MAX_BUFFER_SIZE: usize = 4096;

#[must_use]
pub(crate) struct KernelBuffer {
    storage: NonNull<u8>,
    len: usize,
}

impl KernelBuffer {
    /// Allocate initialized scratch bytes. An empty buffer needs no allocation;
    /// excessive requests and allocator exhaustion return None.
    pub(crate) fn zeroed(len: usize) -> Option<Self> {
        if len > MAX_BUFFER_SIZE {
            return None;
        }
        if len == 0 {
            return Some(Self {
                storage: NonNull::dangling(),
                len: 0,
            });
        }
        // SAFETY: the kernel initializes Kmem before syscall entry. Allocation
        // returns fresh writable storage for the checked requested byte count.
        let storage = NonNull::new(unsafe { Kmem::kmm_alloc(len) }.cast::<u8>())?;
        // SAFETY: the fresh allocation is exclusively owned and len bytes long.
        unsafe { storage.as_ptr().write_bytes(0, len) };
        Some(Self { storage, len })
    }
}

impl Deref for KernelBuffer {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        // SAFETY: construction initialized every byte; the allocation stays
        // owned for this borrow. An empty buffer uses an aligned dangling ptr.
        unsafe { core::slice::from_raw_parts(self.storage.as_ptr(), self.len) }
    }
}

impl DerefMut for KernelBuffer {
    fn deref_mut(&mut self) -> &mut [u8] {
        // SAFETY: this exclusive owner borrow excludes any safe slice aliases.
        unsafe { core::slice::from_raw_parts_mut(self.storage.as_ptr(), self.len) }
    }
}

impl Drop for KernelBuffer {
    fn drop(&mut self) {
        if self.len != 0 {
            // SAFETY: this owner holds the one unreleased Kmem allocation.
            unsafe { Kmem::kmm_free(self.storage.as_ptr().cast()) };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::alloc::Layout;
    use core::ffi::c_void;
    use std::alloc::{alloc, dealloc};
    use std::cell::{Cell, RefCell};
    use std::collections::HashMap;
    use std::panic::catch_unwind;

    std::thread_local! {
        static LIVE: RefCell<HashMap<usize, Layout>> = RefCell::new(HashMap::new());
        static FAIL: Cell<bool> = const { Cell::new(false) };
    }

    pub(super) struct Kmem;
    impl Kmem {
        pub(super) unsafe fn kmm_alloc(len: usize) -> *mut c_void {
            if FAIL.get() {
                return core::ptr::null_mut();
            }
            let layout = Layout::from_size_align(len, 8).unwrap();
            // SAFETY: nonzero requested size and valid alignment.
            let raw = unsafe { alloc(layout) };
            if !raw.is_null() {
                // Dirty recycled-looking bytes ensure zeroing is exercised.
                unsafe { raw.write_bytes(0xa5, len) };
                LIVE.with(|live| assert!(live.borrow_mut().insert(raw as usize, layout).is_none()));
            }
            raw.cast()
        }

        pub(super) unsafe fn kmm_free(raw: *mut c_void) {
            let layout = LIVE
                .with(|live| live.borrow_mut().remove(&(raw as usize)))
                .expect("buffer freed exactly once");
            // SAFETY: the matching allocation was removed from the live set.
            unsafe { dealloc(raw.cast(), layout) };
        }
    }

    fn live() -> usize {
        LIVE.with(|live| live.borrow().len())
    }

    #[test]
    fn initializes_dirty_storage_and_exposes_only_requested_bytes() {
        let mut bytes = KernelBuffer::zeroed(73).unwrap();
        assert_eq!(bytes.len(), 73);
        assert!(bytes.iter().all(|&byte| byte == 0));
        bytes[72] = 42;
        assert_eq!(bytes[72], 42);
        assert_eq!(live(), 1);
        drop(bytes);
        assert_eq!(live(), 0);
    }

    #[test]
    fn zero_length_and_excessive_requests_do_not_allocate() {
        let bytes = KernelBuffer::zeroed(0).unwrap();
        assert!(bytes.is_empty());
        assert!(KernelBuffer::zeroed(MAX_BUFFER_SIZE + 1).is_none());
        assert!(KernelBuffer::zeroed(usize::MAX).is_none());
        assert_eq!(live(), 0);
    }

    #[test]
    fn allocation_failure_and_unwind_leave_no_live_buffer() {
        FAIL.set(true);
        assert!(KernelBuffer::zeroed(64).is_none());
        FAIL.set(false);
        assert_eq!(live(), 0);
        let result = catch_unwind(|| {
            let _bytes = KernelBuffer::zeroed(MAX_BUFFER_SIZE).unwrap();
            panic!("exercise early rollback");
        });
        assert!(result.is_err());
        assert_eq!(live(), 0);
    }
}
