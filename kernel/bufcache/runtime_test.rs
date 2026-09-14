//! Rejected-write retention on the real buffer cache and block device.
//!
//! The boot gate runs before writable user workloads. Its only submitted block
//! is u32::MAX, outside the small QEMU test disk. No valid sector is written.
//! The global sync API requires an otherwise quiet dirty list; both entry and
//! pre-sync checks reject unrelated dirty buffers before submission.

#![deny(unsafe_op_in_unsafe_fn)]

use core::ffi::c_void;
use core::ptr::NonNull;

use super::{ln_is_detached, ln_is_empty, Blkdev, Buf, BufCache, Hlist, RawMutex, BCACHE, BSIZE};
use crate::kstd::{is_err_or_null, Errno, KResult};
use crate::machine::Riscv;

const INVALID_BLOCK: u32 = u32::MAX;
const PATTERN: u8 = 0xa7;

/// Owns the synthetic key until teardown. While unlocked, dirty-list ownership
/// retains its buffer; cleanup reacquires the key through the ordinary cache
/// lookup before modifying it. No raw pointer is assumed to remain recyclable.
struct SyntheticBuffer {
    dev: u32,
    buffer: NonNull<Buf>,
    locked: bool,
}

impl SyntheticBuffer {
    fn new(dev: u32) -> Self {
        let buffer = NonNull::new(BufCache::get(dev, INVALID_BLOCK))
            .expect("bufcache test buffer allocation");
        Self {
            dev,
            buffer,
            locked: true,
        }
    }

    fn release(&mut self) {
        assert!(self.locked);
        Buf::release(self.buffer.as_ptr());
        self.locked = false;
    }

    fn lock(&mut self) {
        if !self.locked {
            self.buffer = NonNull::new(BufCache::get(self.dev, INVALID_BLOCK))
                .expect("bufcache test buffer lookup");
            self.locked = true;
        }
    }

    fn retained_without_reference(&self) -> bool {
        assert!(!self.locked);
        let _cache = BCACHE.lock();
        let buffer = self.buffer.as_ptr();
        // SAFETY: Buf storage is permanent. The cache lock protects identity,
        // dirty/refcount state and links even if a broken implementation made
        // this entry eligible for recycling after release.
        unsafe {
            (*buffer).dev == self.dev
                && (*buffer).blockno == INVALID_BLOCK
                && (*buffer).dirty != 0
                && (*buffer).refcnt == 0
                && ln_is_detached(&raw mut (*buffer).free_entry)
                && !ln_is_detached(&raw mut (*buffer).dirty_entry)
        }
    }

    fn is_only_dirty_entry(&self) -> bool {
        let cache = BCACHE.lock();
        let buffer = self.buffer.as_ptr();
        // SAFETY: the held cache lock protects both the head and target links.
        unsafe {
            cache.dirty_count == 1
                && cache.dirty_list.next == &raw mut (*buffer).dirty_entry
                && cache.dirty_list.prev == &raw mut (*buffer).dirty_entry
        }
    }
}

impl Drop for SyntheticBuffer {
    fn drop(&mut self) {
        self.lock();
        let buffer = self.buffer.as_ptr();
        {
            let mut cache = BCACHE.lock();
            // SAFETY: lookup holds one reference and the buffer mutex. This
            // impossible key has no external caller; the quiet boot gate has
            // no concurrent sync. Cache lock protects hash/list/refcount state.
            unsafe {
                assert_eq!((*buffer).dev, self.dev);
                assert_eq!((*buffer).blockno, INVALID_BLOCK);
                assert_eq!((*buffer).refcnt, 1);
                assert!(ln_is_detached(&raw mut (*buffer).free_entry));
                let dirty_entry = &raw mut (*buffer).dirty_entry;
                if !ln_is_detached(dirty_entry) {
                    Riscv::list_entry_detach(dirty_entry);
                    cache.dirty_count = cache
                        .dirty_count
                        .checked_sub(1)
                        .expect("bufcache synthetic dirty count");
                }
                (*buffer).dirty = 0;
                (*buffer).valid = 0;
                // Remove the impossible key entirely; the clean slot returns
                // to the ordinary free list when the held reference is put.
                let mut key = Buf::lookup_key(self.dev, INVALID_BLOCK);
                let removed = Hlist::pop(&raw mut cache.cached, (&raw mut key).cast::<c_void>());
                assert_eq!(removed, buffer.cast());
                (*buffer).dev = 0;
                (*buffer).blockno = 0;
            }
        }
        Buf::release(buffer);
        self.locked = false;
    }
}

fn require(condition: bool, message: &str) -> KResult<()> {
    if condition {
        return Ok(());
    }
    crate::kprintln!("[bufcache] {}", message);
    Err(Errno::Io)
}

/// Exercise release, unpin and failed sync without touching a valid disk block.
///
/// # Safety
/// Run once after buffer-cache/device initialization in schedulable thread
/// context, before writable workloads or other cache synchronizers begin.
/// `dev` must identify the small QEMU test disk: block u32::MAX (in BSIZE-byte
/// units) must lie beyond its capacity and the driver must report EIO for it.
/// Read-only filesystem users may run; no other code may use this synthetic key.
/// The quiet dirty-list condition is checked but cannot exclude a later writer.
pub(crate) unsafe fn run(dev: u32) -> KResult<()> {
    {
        let mut cache = BCACHE.lock();
        require(
            cache.dirty_count == 0 && ln_is_empty(&raw mut cache.dirty_list),
            "dirty cache before rejected-write test",
        )?;
    }
    // Verify that the supplied devno resolves before testing sync's error path.
    let device = Blkdev::get(super::dev_major(dev), super::dev_minor(dev));
    if is_err_or_null(device) {
        return Err(Errno::NoDev);
    }
    let put = Blkdev::put(device);
    require(put == 0, "device reference release")?;

    let mut synthetic = SyntheticBuffer::new(dev);
    let buffer = synthetic.buffer.as_ptr();
    // SAFETY: get returned the one locked buffer reference; the preallocated
    // BSIZE-byte payload is exclusive and initialized before dirty publication.
    unsafe {
        core::slice::from_raw_parts_mut((*buffer).data, BSIZE as usize).fill(PATTERN);
        (*buffer).valid = 1;
    }
    Buf::write_async(buffer);
    synthetic.release();
    require(
        synthetic.retained_without_reference(),
        "release recycled a dirty buffer",
    )?;

    // Exercise the separate unpin path with the same dirty, unrecyclable slot.
    Buf::pin(buffer);
    Buf::unpin(buffer);
    require(
        synthetic.retained_without_reference(),
        "unpin recycled a dirty buffer",
    )?;
    require(
        synthetic.is_only_dirty_entry(),
        "unrelated dirty data before sync",
    )?;

    // The synthetic entry is the only dirty buffer. Its rejected write must
    // remain dirty and end this sync pass, rather than retrying forever or
    // placing failed data on the free list.
    let result = BufCache::sync();
    require(
        matches!(result, Err(error) if error.raw() == Errno::Io.raw()),
        "failed sync did not propagate EIO",
    )?;
    require(
        synthetic.retained_without_reference(),
        "failed sync discarded or recycled dirty data",
    )?;
    require(
        synthetic.is_only_dirty_entry(),
        "failed sync lost the dirty-list entry",
    )?;
    synthetic.lock();
    // SAFETY: the reacquired mutex protects this initialized payload; sync has
    // drained its BIO/DMA operation before returning.
    let intact = unsafe {
        RawMutex::is_holding(&raw mut (*synthetic.buffer.as_ptr()).lock) != 0
            && core::slice::from_raw_parts((*synthetic.buffer.as_ptr()).data, BSIZE as usize)
                .iter()
                .all(|byte| *byte == PATTERN)
    };
    require(intact, "failed write changed cached data")?;
    drop(synthetic);
    // A failed pass must also release its gate so later flushes can proceed.
    BufCache::sync()?;
    {
        let mut cache = BCACHE.lock();
        require(
            cache.dirty_count == 0 && ln_is_empty(&raw mut cache.dirty_list),
            "synthetic dirty entry was not cleaned up",
        )?;
    }
    crate::kprintln!("BUFCACHE TESTS: 1/1 PASSED (rejected write retained dirty data)");
    Ok(())
}
