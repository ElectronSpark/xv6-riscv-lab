//! Read-only tests of the real device, DMA ownership and shared BIO lifecycle.

use core::ptr;
use core::sync::atomic::{AtomicUsize, Ordering};

use super::{Bio, Blkdev, Disk, DEVICES};
use crate::dev::bio::BioEndIo;
use crate::kobject::KArc;
use crate::kstd::{errptr_to_result, Errno, KResult};
use crate::mm::mm_safe::{PageHandle, BUDDY};
use crate::mm::page::PAGE_TYPE_ANON;

static COMPLETIONS: AtomicUsize = AtomicUsize::new(0);
struct CountCompletion;
static COUNT_COMPLETION: CountCompletion = CountCompletion;

impl BioEndIo for CountCompletion {
    unsafe fn end_io(&self, _bio: *mut Bio) {
        COMPLETIONS.fetch_add(1, Ordering::SeqCst);
    }
}

fn page(fill: u8) -> KResult<PageHandle<'static>> {
    let page = BUDDY.alloc(0, PAGE_TYPE_ANON).ok_or(Errno::NoMem)?;
    // SAFETY: the test exclusively owns this whole unpublished page.
    unsafe { ptr::write_bytes(page.data_ptr(), fill, page.byte_len()) };
    Ok(page)
}

fn read(page: &mut PageHandle<'_>, sector: u64, lengths: &[u16]) -> KResult<()> {
    // Disk initialization makes this device permanent before the test runs.
    let _disk = Disk::at(0);
    let device = DEVICES.slot(0);
    // SAFETY: initialized permanent device, with no concurrent removal.
    let raw = errptr_to_result(unsafe {
        Bio::alloc(
            device,
            lengths.len() as i16,
            false as crate::bindings::bool_,
            Some(&COUNT_COMPLETION),
            ptr::null_mut(),
        )
    })
    .map_err(Errno::Raw)?;
    // SAFETY: alloc returned a new BIO reference; this owner remains alive
    // through submission, the callback, waiting and the result checks.
    let bio = unsafe { KArc::<Bio>::from_raw(raw) };
    let raw = KArc::as_ptr(&bio);
    // SAFETY: the newly allocated bio is private and has not been submitted.
    unsafe { (*raw).blkno = sector };
    let mut offset = 0;
    for (index, &length) in lengths.iter().enumerate() {
        // SAFETY: private BIO and live, exclusively owned page before submit.
        Errno::cint_result(unsafe {
            Bio::add_seg(raw, page.as_ptr().cast(), index as i16, length, offset)
        })?;
        offset += length;
    }
    let completions = COMPLETIONS.load(Ordering::SeqCst);
    // SAFETY: the private BIO and its page/device owners remain live until wait.
    Errno::cint_result(unsafe { Blkdev::submit_bio(device, raw) })?;
    // SAFETY: the BIO owner and its page stay pinned until all DMA finishes.
    let result = unsafe { Bio::wait(raw) };
    if COMPLETIONS.load(Ordering::SeqCst) != completions + 1 {
        Disk::fail(format_args!(
            "BIO TESTS: completion callback did not run exactly once"
        ));
    }
    // A successful completion must account for every submitted byte.
    // SAFETY: wait completed with this owner alive; no writer remains.
    if result == 0 && unsafe { (*raw).done_size } != offset {
        Disk::fail(format_args!("BIO TESTS: incomplete segment accounting"));
    }
    Errno::cint_result(result).map(|_| ())
}

fn same_data(left: &PageHandle<'_>, right: &PageHandle<'_>) -> bool {
    // SAFETY: both pages were initialized and their synchronous test reads
    // have finished. No DMA, mapping or other accessor can mutate the bytes.
    unsafe { left.as_bytes()[..2048] == right.as_bytes()[..2048] }
}

fn validate_segments(page: &PageHandle<'_>) -> KResult<()> {
    let _disk = Disk::at(0);
    let device = DEVICES.slot(0);
    // SAFETY: this initialized device is permanent; no callback stores any
    // external state. The test keeps its independently owned page alive.
    let raw = errptr_to_result(unsafe {
        Bio::alloc(
            device,
            9,
            false as crate::bindings::bool_,
            None,
            ptr::null_mut(),
        )
    })
    .map_err(Errno::Raw)?;
    // SAFETY: alloc returned one owned reference. This BIO is never submitted,
    // so dropping it frees only metadata and requires no completion or wait.
    let bio = unsafe { KArc::<Bio>::from_raw(raw) };
    let raw = KArc::as_ptr(&bio);
    let snapshot = || {
        // SAFETY: this private BIO contains nine initialized vector slots.
        // Copy only scalar metadata; no borrow survives a subsequent update.
        unsafe {
            (
                (*raw).size,
                core::array::from_fn::<_, 9, _>(|index| {
                    let vector = (*raw).bvecs.as_ptr().add(index).read();
                    (vector.bv_page, vector.len, vector.offset)
                }),
            )
        }
    };
    // SAFETY: the BIO is private and unsubmitted, with its page still owned.
    Errno::cint_result(unsafe { Bio::add_seg(raw, page.as_ptr().cast(), 0, 512, 0) })?;
    let before = snapshot();
    for (index, length, offset) in [(1, u16::MAX, 0), (0, 512, 4095)] {
        // SAFETY: both selected slots belong to this unsubmitted BIO. Invalid
        // lengths/offsets are checked arguments, not invalid pointer lifetimes.
        let error = unsafe { Bio::add_seg(raw, page.as_ptr().cast(), index, length, offset) };
        if error != Errno::Inval.neg() || snapshot() != before {
            Disk::fail(format_args!("BIO TESTS: invalid segment changed metadata"));
        }
    }
    // The first rejected case would wrap 512 + 65535 to 511 in u16 arithmetic.
    // Replacing an existing vector must subtract its old contribution instead.
    // SAFETY: same private BIO and live page; this full-page span is valid.
    Errno::cint_result(unsafe { Bio::add_seg(raw, page.as_ptr().cast(), 0, 4096, 0) })?;
    let replacement = snapshot();
    if replacement.0 != 4096 || replacement.1[0] != (page.as_ptr().cast(), 4096, 0) {
        Disk::fail(format_args!("BIO TESTS: replacement size accounting"));
    }
    // Reusing the page is safe for an unsent metadata test: no DMA accesses any
    // vector. Eight full-page vectors exactly fill the 32768-byte BIO budget.
    for index in 1..8 {
        // SAFETY: each index is within the nine allocated, private slots.
        Errno::cint_result(unsafe { Bio::add_seg(raw, page.as_ptr().cast(), index, 4096, 0) })?;
    }
    let before = snapshot();
    if before.0 != 32768 {
        Disk::fail(format_args!("BIO TESTS: full request size accounting"));
    }
    // SAFETY: the final allocated slot is empty; budget rejection must precede
    // both its installation and any change to the aggregate size.
    let error = unsafe { Bio::add_seg(raw, page.as_ptr().cast(), 8, 4096, 0) };
    if error != -7 /* E2BIG */ || snapshot() != before {
        Disk::fail(format_args!("BIO TESTS: request budget changed metadata"));
    }
    crate::kprintln!("[bio] segment validation preserves metadata: OK");
    Ok(())
}

fn run() -> KResult<()> {
    let mut contiguous = page(0xa5)?;
    let mut split = page(0x5a)?;
    read(&mut contiguous, 0, &[2048])?;
    read(&mut split, 0, &[512, 1024, 512])?;
    if !same_data(&contiguous, &split) {
        Disk::fail(format_args!("BIO TESTS: unequal segment read mismatch"));
    }
    crate::kprintln!("[bio] unequal segments and one completion: OK");

    // The sector fits checked arithmetic but lies far beyond the test disk.
    // All three failed requests must drain before the BIO can signal EIO.
    let error = read(&mut split, 1 << 48, &[512, 1024, 512]);
    if !matches!(error, Err(error) if error.raw() == Errno::Io.raw()) {
        Disk::fail(format_args!(
            "BIO TESTS: out-of-range read did not return EIO: {:?}",
            error
        ));
    }
    crate::kprintln!("[bio] device EIO drains all segments: OK");

    read(&mut split, 0, &[512, 1024, 512])?;
    if !same_data(&contiguous, &split) {
        Disk::fail(format_args!("BIO TESTS: read after EIO mismatch"));
    }
    crate::kprintln!("[bio] valid read after EIO: OK");
    validate_segments(&split)?;
    let device = DEVICES.slot(0);
    // SAFETY: disk initialization permanently owns this registered device.
    let dev = unsafe { ((*device).dev.major as u32) << 20 | (*device).dev.minor as u32 };
    // SAFETY: run_tests blocks the init thread before user programs begin.
    // Other boot tests do not write or synchronize this small QEMU test disk.
    unsafe { crate::bufcache::runtime_test::run(dev) }?;
    Ok(())
}

/// Run in init's schedulable context before it starts writable user programs.
#[inline(never)]
pub(super) fn run_tests() {
    if let Err(error) = run() {
        Disk::fail(format_args!(
            "BIO TESTS: allocation/submission failed: {:?}",
            error
        ));
    }
    crate::kprintln!("BIO TESTS: 5/5 PASSED");
}
