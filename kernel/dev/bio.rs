//! Shared block-I/O submission and completion lifecycle.
//!
//! Callers allocate a BIO, populate its page spans, submit it through the
//! block-device layer, then wait before reusing or releasing the pages. BIO
//! references own only the header/vector allocation; callers retain separate
//! page and block-device ownership throughout I/O.
//!
//! `Bio::begin` validates immutable segment metadata and returns `BioRequest`.
//! Each yielded `BioPart` pins the header until its driver stops DMA and
//! completes the part. A submission sentinel prevents early completion while
//! later segments are still being queued. Atomic progress selects exactly one
//! final publisher, which invokes the callback and wakes waiters while holding
//! its own reference. Interrupted waits still drain DMA before returning.
//!
//! Raw entry points retain pointer/negative-errno conventions for existing
//! kernel callers, with explicit unsafe lifetime and synchronization contracts.

#![deny(unsafe_op_in_unsafe_fn)]

use core::ffi::{c_int, c_void};
use core::ptr::{self, NonNull};
use core::sync::atomic::{fence, Ordering};

mod transfer;
use transfer::{CompletionState, SegmentLayout, TransferLayout};

use crate::bindings::{bio, bio_vec, blkdev_t, bool_, page_t, PGSIZE};
use crate::kobject::{HasKobject, Kobject};
use crate::kstd::{result_to_errptr, Errno, KResult};
use crate::lock::completion::RawCompletion;
use crate::mm::kalloc::Kmem;

const BIO_MAX_VECS: i16 = transfer::MAX_SEGMENTS as i16;
const BIO_MAX_SIZE: usize = transfer::MAX_BYTES;
const E2BIG: c_int = 7;

/// A caller-owned page span; the BIO does not acquire a page reference.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct BioVec {
    pub bv_page: *mut page_t,
    pub len: u16,
    pub offset: u16,
}

/// Request state bits. The submitter initializes them before publication;
/// only the final completion updates them while the request is in flight.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct BioFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl BioFlagBits {
    #[inline]
    pub(crate) fn valid(&self) -> u64 {
        (self.bits & 0b001) as u64
    }
    #[inline]
    pub(crate) fn set_valid(&mut self, val: u64) {
        self.bits = (self.bits & !0b001) | (((val as u8) & 0b01) << 0);
    }
    #[inline]
    pub(crate) fn rw(&self) -> u64 {
        ((self.bits & 0b010) >> 1) as u64
    }
    #[inline]
    pub(crate) fn set_rw(&mut self, val: u64) {
        self.bits = (self.bits & !0b010) | (((val as u8) & 0b01) << 1);
    }
    #[inline]
    pub(crate) fn done(&self) -> u64 {
        ((self.bits & 0b100) >> 2) as u64
    }
    #[inline]
    pub(crate) fn set_done(&mut self, val: u64) {
        self.bits = (self.bits & !0b100) | (((val as u8) & 0b01) << 2);
    }
}

/// Invoked once after all segments and submission have finished.
///
/// The dispatcher holds a BIO reference through this callback and the ensuing
/// waiter wakeup. A callback may release its own reference, but must not reset,
/// mutate segment metadata, or resubmit this same BIO; its old completion is
/// still about to be signalled. It must not wait for that signal itself.
pub trait BioEndIo: Sync {
    /// # Safety
    /// `bio` is live, all its data transfers have stopped, and its final
    /// result fields are published. The callback obeys the lifecycle above.
    unsafe fn end_io(&self, bio: *mut Bio);
}

/// Reference-counted request header followed by `vec_length` vector slots.
/// The kobject is first for reference-counting casts; completion stays cache
/// aligned. Progress occupies existing padding without changing this layout.
#[repr(C, align(64))]
pub struct Bio {
    pub kobj: Kobject,
    pub list_entry: crate::bindings::list_node_t,
    pub bdev: *mut blkdev_t,
    pub block_shift: u16,
    pub vec_length: i16,
    pub size: u16,
    pub done_size: u16,
    pub blkno: u64,
    pub flags: BioFlagBits,
    pub end_io: Option<&'static dyn BioEndIo>,
    pub private_data: *mut c_void,
    pub error: c_int,
    // Occupies the former 16-byte padding slot; descriptor layout is stable.
    progress: CompletionState,
    pub io_completion: crate::bindings::completion_t,
    pub bvecs: crate::bindings::__IncompleteArrayField<BioVec>,
}

const _: () = {
    assert!(core::mem::size_of::<BioVec>() == 16, "bio_vec size");
    assert!(core::mem::align_of::<BioVec>() == 8, "bio_vec alignment");
    assert!(
        core::mem::offset_of!(BioVec, bv_page) == 0,
        "bio_vec.bv_page offset"
    );
    assert!(
        core::mem::offset_of!(BioVec, len) == 8,
        "bio_vec.len offset"
    );
    assert!(
        core::mem::offset_of!(BioVec, offset) == 10,
        "bio_vec.offset offset"
    );
    assert!(
        core::mem::size_of::<BioFlagBits>() == 8,
        "bio anon bitfield size"
    );
    assert!(
        core::mem::align_of::<BioFlagBits>() == 8,
        "bio anon bitfield alignment"
    );
    assert!(
        core::mem::size_of::<crate::bindings::completion_t>() == 128,
        "completion_t size"
    );
    assert!(
        core::mem::align_of::<crate::bindings::completion_t>() == 64,
        "completion_t alignment"
    );
    assert!(core::mem::size_of::<Bio>() == 320, "bio size");
    assert!(core::mem::align_of::<Bio>() == 64, "bio alignment");
    assert!(core::mem::offset_of!(Bio, kobj) == 0, "bio.kobj offset");
    assert!(
        core::mem::offset_of!(Bio, list_entry) == 48,
        "bio.list_entry offset"
    );
    assert!(core::mem::offset_of!(Bio, bdev) == 64, "bio.bdev offset");
    assert!(
        core::mem::offset_of!(Bio, block_shift) == 72,
        "bio.block_shift offset"
    );
    assert!(
        core::mem::offset_of!(Bio, vec_length) == 74,
        "bio.vec_length offset"
    );
    assert!(core::mem::offset_of!(Bio, size) == 76, "bio.size offset");
    assert!(
        core::mem::offset_of!(Bio, done_size) == 78,
        "bio.done_size offset"
    );
    assert!(core::mem::offset_of!(Bio, blkno) == 80, "bio.blkno offset");
    assert!(
        core::mem::offset_of!(Bio, flags) == 88,
        "bio anon bitfield offset"
    );
    assert!(
        core::mem::offset_of!(Bio, end_io) == 96,
        "bio.end_io offset"
    );
    assert!(
        core::mem::offset_of!(Bio, private_data) == 112,
        "bio.private_data offset"
    );
    assert!(core::mem::offset_of!(Bio, error) == 120, "bio.error offset");
    assert!(
        core::mem::offset_of!(Bio, progress) == 128,
        "bio.progress offset"
    );
    assert!(
        core::mem::offset_of!(Bio, io_completion) == 192,
        "bio.io_completion offset"
    );
    assert!(core::mem::offset_of!(Bio, bvecs) == 320, "bio.bvecs offset");
    assert!(
        core::mem::size_of::<Option<&'static dyn BioEndIo>>() == 16,
        "bio end_io fat pointer size"
    );
    assert!(
        core::mem::align_of::<Option<&'static dyn BioEndIo>>() == 8,
        "bio end_io fat pointer alignment"
    );
};

/// A held BIO reference without `Deref`: keeping a shared allocation alive
/// does not permit a reference to all of its concurrently changing fields.
struct BioRef(NonNull<Bio>);

impl BioRef {
    /// # Safety
    /// Caller holds a live reference and has frozen the segment metadata.
    unsafe fn acquire(raw: *mut Bio) -> Self {
        unsafe {
            Kobject::kobject_get(core::ptr::addr_of_mut!((*raw).kobj));
        }
        Self(unsafe { NonNull::new_unchecked(raw) })
    }

    fn finish(&self, bytes: usize, error: i32) {
        let raw = self.0.as_ptr();
        // Only this atomic field is shared while a request is in flight.
        let progress = unsafe { &*core::ptr::addr_of!((*raw).progress) };
        let Some(result) = progress.finish(bytes, error) else {
            return;
        };
        // The last pending token owns result publication. Every DMA operation
        // has finished and the submission sentinel has been released.
        unsafe {
            (*raw).done_size = result.bytes as u16;
            (*raw).error = if result.error != 0 {
                result.error
            } else if result.bytes != u32::from((*raw).size) {
                Errno::Io.neg()
            } else {
                0
            };
            (*raw).flags.set_done(1);
            fence(Ordering::SeqCst);
            if let Some(callback) = (*raw).end_io {
                callback.end_io(raw);
            }
            // This reference pins both the callback and wakeup accesses, even
            // if the callback/waiter releases its own BIO reference.
            RawCompletion::complete_all(core::ptr::addr_of_mut!((*raw).io_completion));
        }
    }
}

impl Clone for BioRef {
    fn clone(&self) -> Self {
        // The existing reference pins the allocation through acquisition.
        unsafe { Self::acquire(self.0.as_ptr()) }
    }
}

impl Drop for BioRef {
    fn drop(&mut self) {
        // Release exactly the reference acquired by this handle.
        unsafe {
            Kobject::kobject_put(core::ptr::addr_of_mut!((*self.0.as_ptr()).kobj));
        }
    }
}

/// One segment's ownership while queued or in flight. A driver retains this
/// token until DMA has stopped accessing its buffer, then consumes it through
/// `complete`. Dropping an unsubmitted/stopped token reports an I/O error.
pub(crate) struct BioPart {
    owner: BioRef,
    sector: u64,
    data: *mut u8,
    len: usize,
    write: bool,
    finished: bool,
}

// SAFETY: immutable metadata and a held global BIO reference travel together.
// Completion uses atomic accounting; the last token alone publishes results.
// The unsafe begin contract pins page storage until every token completes.
unsafe impl Send for BioPart {}

impl BioPart {
    pub fn sector(&self) -> u64 {
        self.sector
    }
    pub fn len(&self) -> usize {
        self.len
    }
    pub fn write(&self) -> bool {
        self.write
    }
    pub fn data_ptr(&self) -> *mut u8 {
        self.data
    }

    pub fn complete(mut self, result: KResult<()>) {
        self.finished = true;
        let (bytes, error) = match result {
            Ok(()) => (self.len, 0),
            Err(error) => (0, error.neg()),
        };
        self.owner.finish(bytes, error);
    }
}

impl Drop for BioPart {
    fn drop(&mut self) {
        if !self.finished {
            self.owner.finish(0, Errno::Io.neg());
        }
    }
}

/// Checked submission iterator. It holds a sentinel until dropped, so a fast
/// first completion cannot wake a caller while later vectors are being queued.
pub(crate) struct BioRequest {
    owner: BioRef,
    vectors: NonNull<BioVec>,
    count: usize,
    index: usize,
    bytes_issued: usize,
    layout: TransferLayout,
    write: bool,
}

impl Iterator for BioRequest {
    type Item = BioPart;

    fn next(&mut self) -> Option<BioPart> {
        if self.index == self.count {
            return None;
        }
        // begin validated the allocation/array and freezes it for this owner.
        let vector = unsafe { self.vectors.as_ptr().add(self.index).read() };
        let sector = self
            .layout
            .sector_at(self.bytes_issued)
            .expect("validated BIO offset");
        // Page descriptors and their direct mappings remain pinned by begin's
        // caller until completion; offset/end arithmetic was checked there.
        let address = unsafe { crate::mm::page::Page::__page_to_pa(vector.bv_page.cast()) };
        let data = (address + u64::from(vector.offset)) as *mut u8;
        let owner = self.owner.clone();
        unsafe { &*core::ptr::addr_of!((*owner.0.as_ptr()).progress) }.reserve();
        self.index += 1;
        self.bytes_issued += usize::from(vector.len);
        Some(BioPart {
            owner,
            sector,
            data,
            len: usize::from(vector.len),
            write: self.write,
            finished: false,
        })
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let remaining = self.count - self.index;
        (remaining, Some(remaining))
    }
}

impl ExactSizeIterator for BioRequest {}

impl Drop for BioRequest {
    fn drop(&mut self) {
        self.owner.finish(
            0,
            if self.index == self.count {
                0
            } else {
                Errno::Io.neg()
            },
        );
    }
}

impl Bio {
    /// Begin exactly one submission, freezing metadata and pinning the BIO.
    ///
    /// # Safety
    /// `raw` is a live, validated BIO allocation, exclusively controlled for
    /// submission. Its vector array has `vec_length` initialized elements.
    /// Vector metadata stays immutable, and every referenced page remains live
    /// and accessible for the requested DMA/copy direction until completion.
    /// The BIO only pins its header, not its pages or block device: the caller
    /// retains their separate owners until all parts and this request finish.
    /// No second submission, callback reinitialization, or CPU buffer access
    /// overlaps in-flight I/O.
    pub(crate) unsafe fn begin(raw: *mut Bio) -> KResult<BioRequest> {
        if raw.is_null() {
            return Err(Errno::Inval);
        }
        let (count, bytes, block_shift, sector, write, vectors) = unsafe {
            (
                (*raw).vec_length,
                (*raw).size,
                (*raw).block_shift,
                (*raw).blkno,
                (*raw).flags.rw() != 0,
                (*raw).bvecs.as_mut_ptr(),
            )
        };
        if count <= 0 || count > BIO_MAX_VECS {
            return Err(Errno::Inval);
        }
        // Borrow only the immutable vector array, not the shared BIO header.
        let slice = unsafe { core::slice::from_raw_parts(vectors, count as usize) };
        let layout = TransferLayout::new(
            sector,
            block_shift,
            usize::from(bytes),
            slice.iter().map(|v| SegmentLayout {
                has_page: !v.bv_page.is_null(),
                offset: usize::from(v.offset),
                len: usize::from(v.len),
            }),
        )
        .map_err(|_| Errno::Inval)?;
        for vector in slice {
            let address = unsafe { crate::mm::page::Page::__page_to_pa(vector.bv_page.cast()) };
            if address == 0
                || address
                    .checked_add(u64::from(vector.offset) + u64::from(vector.len))
                    .is_none()
            {
                return Err(Errno::Inval);
            }
        }
        let owner = unsafe { BioRef::acquire(raw) };
        unsafe {
            (*raw).flags.set_valid(1);
            (*raw).flags.set_done(0);
            (*raw).done_size = 0;
            (*raw).error = 0;
            core::ptr::write(
                core::ptr::addr_of_mut!((*raw).progress),
                CompletionState::new(),
            );
            RawCompletion::reinit(core::ptr::addr_of_mut!((*raw).io_completion));
        }
        fence(Ordering::SeqCst);
        Ok(BioRequest {
            owner,
            vectors: unsafe { NonNull::new_unchecked(vectors) },
            count: count as usize,
            index: 0,
            bytes_issued: 0,
            layout,
            write,
        })
    }

    /// Wait through cancellation until DMA has released the caller's pages.
    /// # Safety
    /// The caller holds a live BIO reference and has submitted it successfully.
    pub(crate) unsafe fn wait(raw: *mut Bio) -> c_int {
        let completion = unsafe { core::ptr::addr_of_mut!((*raw).io_completion) };
        let status = RawCompletion::wait_interruptible(completion);
        if status == Errno::Intr.neg() {
            RawCompletion::wait(completion);
        }
        let error = unsafe { (*raw).error };
        if error != 0 {
            error
        } else {
            status
        }
    }
}

// SAFETY: kobj is the stable offset-zero field of each live BIO allocation.
// Only its release callback deallocates the header when the last owner leaves.
unsafe impl HasKobject for Bio {
    fn kobj_ptr(this: *mut Self) -> *mut Kobject {
        this.cast()
    }
}

struct BioReleaseKobj;
static BIO_RELEASE_KOBJ: BioReleaseKobj = BioReleaseKobj;

impl crate::kobject::KobjectRelease for BioReleaseKobj {
    unsafe fn release(&self, object: *mut Kobject) {
        // SAFETY: the final kobject reference owns this entire BIO allocation;
        // offset-zero embedding makes object its original allocation address.
        unsafe { Kmem::kmm_free(object.cast()) };
    }
}

impl Bio {
    /// Allocate a BIO with zeroed, empty segment slots and one owned reference.
    /// Returns an error pointer on invalid arguments or allocation failure.
    ///
    /// # Safety
    /// A nonnull `bdev` must be a live block device whose immutable geometry is
    /// initialized. The caller retains the device separately through I/O.
    /// Callback-private storage must remain valid for its callback's accesses.
    pub(crate) unsafe fn alloc(
        bdev: *mut blkdev_t,
        vec_length: i16,
        rw: bool_,
        end_io: Option<&'static dyn BioEndIo>,
        private_data: *mut c_void,
    ) -> *mut bio {
        // SAFETY: forwarded device/callback lifetime contract.
        result_to_errptr(unsafe { Self::alloc_inner(bdev, vec_length, rw, end_io, private_data) })
    }

    /// # Safety
    /// Same device/callback contract as alloc.
    unsafe fn alloc_inner(
        bdev: *mut blkdev_t,
        vec_length: i16,
        rw: bool_,
        end_io: Option<&'static dyn BioEndIo>,
        private_data: *mut c_void,
    ) -> KResult<*mut bio> {
        if bdev.is_null() || vec_length <= 0 || vec_length > BIO_MAX_VECS {
            return Err(Errno::Inval);
        }
        let bytes =
            core::mem::size_of::<Bio>() + vec_length as usize * core::mem::size_of::<BioVec>();
        // SAFETY: allocation follows global allocator initialization.
        let raw = unsafe { Kmem::kmm_alloc(bytes) }.cast::<Bio>();
        if raw.is_null() {
            return Err(Errno::NoMem);
        }
        // SAFETY: this is an exclusively owned bytes-sized allocation. All
        // fields have a zero-valid representation, including atomic progress,
        // empty vector pointers and optional callback/release trait objects.
        unsafe { ptr::write_bytes(raw.cast::<u8>(), 0, bytes) };
        // SAFETY: raw is private and initialized, and bdev is pinned by caller.
        unsafe {
            (*raw).bdev = bdev;
            (*raw).block_shift = (*bdev).block_shift;
            (*raw).vec_length = vec_length;
            (*raw).flags.set_rw(rw as u64);
            (*raw).end_io = end_io;
            (*raw).private_data = private_data;
            (*raw).kobj.name = c"bio".as_ptr();
            (*raw).kobj.ops.release = Some(&BIO_RELEASE_KOBJ);
            Kobject::kobject_init(&raw mut (*raw).kobj);
            RawCompletion::init(&raw mut (*raw).io_completion);
        }
        Ok(raw)
    }

    /// Replace one unsubmitted vector, leaving all metadata unchanged on error.
    /// Returns zero on success or a negative errno.
    ///
    /// # Safety
    /// Nonnull `raw` must be a caller-owned BIO whose vec_length still names
    /// the initialized slots in its allocation. No submission, callback, or
    /// concurrent metadata access may overlap. A nonnull page must be a live
    /// page descriptor; caller retains its ownership through eventual I/O.
    pub(crate) unsafe fn add_seg(
        raw: *mut bio,
        page: *mut page_t,
        index: i16,
        length: u16,
        offset: u16,
    ) -> c_int {
        if raw.is_null()
            || page.is_null()
            || length == 0
            || usize::from(offset) + usize::from(length) > PGSIZE as usize
        {
            return Errno::Inval.neg();
        }
        // SAFETY: caller owns this live BIO's mutable metadata exclusively.
        let (submitted, count, current) = unsafe {
            (
                (*raw).flags.valid() != 0 || (*raw).flags.done() != 0,
                (*raw).vec_length,
                (*raw).size,
            )
        };
        if submitted {
            return Errno::Io.neg();
        }
        if count <= 0 || count > BIO_MAX_VECS || index < 0 || index >= count {
            return Errno::Inval.neg();
        }
        // SAFETY: the index is within the caller-proven initialized array.
        let vector = unsafe { (*raw).bvecs.as_mut_ptr().add(index as usize) };
        // SAFETY: same live vector; no other metadata writer can race us.
        let previous = unsafe { (*vector).len };
        let Some(without_previous) = usize::from(current).checked_sub(usize::from(previous)) else {
            return Errno::Inval.neg();
        };
        let total = without_previous + usize::from(length);
        if total > BIO_MAX_SIZE {
            return -E2BIG;
        }
        // SAFETY: every validation passed before either metadata write; total
        // fits u16 because BIO_MAX_SIZE is 32768. No wrapping arithmetic.
        unsafe {
            vector.write(BioVec {
                bv_page: page,
                len: length,
                offset,
            });
            (*raw).size = total as u16;
        }
        0
    }

    /// Acquire another reference without acquiring any referenced page/device.
    /// # Safety
    /// A nonnull raw must name a live BIO with an already held reference.
    #[allow(dead_code)]
    pub(crate) unsafe fn dup(raw: *mut bio) -> c_int {
        if raw.is_null() {
            return Errno::Inval.neg();
        }
        // SAFETY: the caller's existing reference pins the embedded kobject.
        unsafe { Kobject::kobject_get(&raw mut (*raw).kobj) };
        0
    }

    /// Release one BIO reference; backing pages/devices remain caller-owned.
    /// # Safety
    /// A nonnull raw must name a live BIO reference transferred to this call.
    /// The caller may not use that reference after releasing it.
    pub(crate) unsafe fn release(raw: *mut bio) -> c_int {
        if raw.is_null() {
            return Errno::Inval.neg();
        }
        // SAFETY: ownership of this reference transfers to kobject_put.
        unsafe { Kobject::kobject_put(&raw mut (*raw).kobj) };
        0
    }

    /// Check immutable metadata and geometry before a driver's submission.
    /// # Safety
    /// Nonnull pointers name live, externally pinned objects. The BIO's
    /// vec_length describes initialized slots contained in its allocation;
    /// its header/vector metadata is exclusively controlled for submission.
    /// This check cannot validate the provenance or lifetime of raw pointers.
    pub(crate) unsafe fn validate(raw: *mut bio, device: *mut blkdev_t) -> c_int {
        if raw.is_null() || device.is_null() {
            return Errno::Inval.neg();
        }
        // SAFETY: both objects and the BIO's metadata are pinned by caller.
        let (matching_device, count, bytes, shift, sector, available) = unsafe {
            (
                (*raw).bdev == device && (*raw).block_shift == (*device).block_shift,
                (*raw).vec_length,
                (*raw).size,
                (*raw).block_shift,
                (*raw).blkno,
                Kobject::kobject_refcount(&raw mut (*raw).kobj) > 0
                    && (*raw).error == 0
                    && (*raw).flags.valid() == 0
                    && (*raw).flags.done() == 0,
            )
        };
        if !matching_device || !available || count <= 0 || count > BIO_MAX_VECS {
            return Errno::Inval.neg();
        }
        // SAFETY: count is bounded and caller guarantees this immutable array
        // is contained in the allocation. No header-wide reference is formed.
        let vectors = unsafe { core::slice::from_raw_parts((*raw).bvecs.as_ptr(), count as usize) };
        match TransferLayout::new(
            sector,
            shift,
            usize::from(bytes),
            vectors.iter().map(|vector| SegmentLayout {
                has_page: !vector.bv_page.is_null(),
                offset: usize::from(vector.offset),
                len: usize::from(vector.len),
            }),
        ) {
            Ok(_) => 0,
            Err(_) => Errno::Inval.neg(),
        }
    }
}
