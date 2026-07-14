//! Ramdisk driver -- Rust port of `kernel/ramdisk.c` (Phase 2 Wave 28,
//! sub-wave A -- see `docs/rustify/phase2_plan.md`). A block device
//! backed by pre-loaded memory (the FDT-described initrd region on real
//! hardware; unused on the qemu boot path, which uses [`super::
//! virtio_disk`] instead).
//!
//! No MMIO, no DMA rings, no interrupts -- this is a straight `memmove`
//! into/out of a contiguous physical-memory region located by
//! `platform.ramdisk_base`/`platform.ramdisk_size` (`dev/fdt.rs`,
//! Phase 2 Wave 23). `dev/bio.h`'s `static-inline` iteration helpers
//! (`bio_iter_*`, `bio_dir_write`, `bio_start_io_acct`) are reimplemented
//! natively here, same precedent as [`super::virtio_disk`]/
//! `kernel/dev/x1_sdhci.rs`/`kernel/bufcache.rs` -- this port does not
//! reuse `virtio_disk.rs`'s private copies, since neither file exposes
//! its (deliberately internal, non-`pub`) helpers across module
//! boundaries, matching this crate's established per-file convention for
//! `static inline` reimplementation.
//!
//! Unlike [`super::virtio_disk`], `submit_bio` completes synchronously
//! (no interrupt, no sleeping wait) -- it calls [`bio_complete`] itself,
//! at the end of the same call, exactly as the C original did.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{fence, Ordering};

use crate::bindings::{bio, bio_vec, blkdev_ops_t, blkdev_t, mode_t, page_t, platform_info, spinlock_t, EINVAL};

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.
    safe fn __panic_start();
    safe fn __panic_end() -> !;

    // lock/spinlock.rs -- raw calls (not `sync::KSpinlock` RAII):
    // `ramdisk_submit_bio` below has several early-return-while-holding-
    // the-lock exit paths (matching the C original's several
    // early-`spin_unlock`-then-`return` sites), which raw calls express
    // more directly than threading a guard through each branch.
    safe fn spin_init(l: *mut spinlock_t, name: *mut c_char);
    safe fn spin_lock(l: *mut spinlock_t);
    safe fn spin_unlock(l: *mut spinlock_t);

    // string.rs.
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;

    // string.rs.
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

    // mm/page.rs.
    safe fn __page_to_pa(page: *mut page_t) -> u64;

    // lock/completion.rs.
    safe fn completion_reinit(c: *mut crate::bindings::completion_t);
    safe fn complete_all(c: *mut crate::bindings::completion_t);

    // dev/fdt.rs (Phase 2 Wave 23): boot-probed platform config. Kept
    // `#[no_mangle]` in `dev/fdt.rs` (P3-1D mesh sweep: widely-shared data
    // anchor, see that file's own comment) -- this extern stays valid.
    static mut platform: platform_info;
}
// P3-1D mesh sweep: dev/blkdev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::dev::blkdev::blkdev_register;

/// `kernel/inc/uabi/stat.h` `S_IFBLK`, same local copy as other `dev/*.rs`.
const S_IFBLK: u32 = 0o060_000;

// ===========================================================================
// `dev/bio.h`'s static-inline helpers, reimplemented natively (this
// file's only consumer -- see module doc).
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
    unsafe { (*bio_ptr).__bindgen_anon_1.rw() != 0 }
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_start_io_acct(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    unsafe {
        (*bio_ptr).__bindgen_anon_1.set_done(0);
        (*bio_ptr).done_size = 0;
        (*bio_ptr).error = 0;
        completion_reinit(&raw mut (*bio_ptr).io_completion);
    }
    fence(Ordering::SeqCst);
}

/// # Safety
/// `bio_ptr` must be live.
unsafe fn bio_end_io_acct(bio_ptr: *mut bio) {
    // SAFETY: caller contract.
    unsafe { (*bio_ptr).__bindgen_anon_1.set_done(1) };
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
        complete_all(&raw mut (*bio_ptr).io_completion);
    }
}

// ===========================================================================
// State.
// ===========================================================================

struct RamdiskState {
    lock: spinlock_t,
    base: u64,
    size_bytes: u64,
    size_blocks: u64,
}

#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get()
    }
}

static RAMDISK: SyncCell<MaybeUninit<RamdiskState>> = SyncCell(UnsafeCell::new(MaybeUninit::uninit()));
static RAMDISK_DEV: SyncCell<MaybeUninit<blkdev_t>> = SyncCell(UnsafeCell::new(MaybeUninit::uninit()));

#[inline(always)]
fn ramdisk_ptr() -> *mut RamdiskState {
    RAMDISK.get() as *mut RamdiskState
}
#[inline(always)]
fn ramdisk_dev_ptr() -> *mut blkdev_t {
    RAMDISK_DEV.get() as *mut blkdev_t
}

// ===========================================================================
// Block device interface.
// ===========================================================================

extern "C" fn ramdisk_open(_blkdev: *mut blkdev_t) -> c_int {
    0
}
extern "C" fn ramdisk_release(_blkdev: *mut blkdev_t) -> c_int {
    0
}

extern "C" fn ramdisk_submit_bio(_blkdev: *mut blkdev_t, bio_ptr: *mut bio) -> c_int {
    let rd = ramdisk_ptr();

    // SAFETY: `rd` initialised once in `ramdisk_init`, before any
    // `submit_bio` call can race it (block device registered only after
    // `rd` is fully set up).
    spin_lock(unsafe { &raw mut (*rd).lock });

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
            spin_unlock(unsafe { &raw mut (*rd).lock });
            // SAFETY: `bio_ptr` live.
            unsafe {
                (*bio_ptr).error = -(EINVAL as c_int);
                bio_complete(bio_ptr);
            }
            return -(EINVAL as c_int);
        }

        // Calculate offset in ramdisk.
        let offset = sector * 512;

        // Check bounds.
        // SAFETY: `rd` live.
        if offset + bvec.len as u64 > unsafe { (*rd).size_bytes } {
            // SAFETY: format string matches its three arguments.
            crate::kprintln!(
                "ramdisk: access beyond end of device (offset={:x}, len={}, size={:x})",
                offset,
                bvec.len as c_int,
                unsafe { (*rd).size_bytes },
            );
            spin_unlock(unsafe { &raw mut (*rd).lock });
            // SAFETY: `bio_ptr` live.
            unsafe {
                (*bio_ptr).error = -(EINVAL as c_int);
                bio_complete(bio_ptr);
            }
            return -(EINVAL as c_int);
        }

        let pa = __page_to_pa(page) as *mut c_void;
        if pa.is_null() {
            spin_unlock(unsafe { &raw mut (*rd).lock });
            // SAFETY: `bio_ptr` live.
            unsafe {
                (*bio_ptr).error = -(EINVAL as c_int);
                bio_complete(bio_ptr);
            }
            return -(EINVAL as c_int);
        }

        // Direct access to contiguous physical memory.
        // SAFETY: `rd` live.
        let ramdisk_addr = (unsafe { (*rd).base } + offset) as *mut c_void;

        // SAFETY: `bio_ptr` live.
        if unsafe { bio_dir_write(bio_ptr) } {
            // Write to ramdisk.
            // SAFETY: `pa + bvec.offset` valid for `bvec.len` bytes (the
            // page this segment was built against); `ramdisk_addr`
            // valid for `bvec.len` bytes (bounds-checked above).
            unsafe { memmove(ramdisk_addr, (pa as u64 + bvec.offset as u64) as *const c_void, bvec.len as usize) };
        } else {
            // Read from ramdisk.
            // SAFETY: same as above, reversed direction.
            unsafe { memmove((pa as u64 + bvec.offset as u64) as *mut c_void, ramdisk_addr, bvec.len as usize) };
        }

        iter.size_done += bvec.len;
        // SAFETY: `bio_ptr` live.
        unsafe { bio_iter_next_seg(bio_ptr, &mut iter) };
    }

    spin_unlock(unsafe { &raw mut (*rd).lock });

    // SAFETY: `bio_ptr` live.
    unsafe {
        (*bio_ptr).error = 0;
        bio_complete(bio_ptr);
    }
    0
}

static RAMDISK_OPS: blkdev_ops_t =
    blkdev_ops_t { open: Some(ramdisk_open), release: Some(ramdisk_release), submit_bio: Some(ramdisk_submit_bio) };

/// `void ramdisk_init(void)`.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn ramdisk_init() {
    let rd = ramdisk_ptr();
    // SAFETY: `rd` exclusively owned at this point (boot-time, single
    // caller, `start_kernel.rs`'s `start_kernel_post_init`).
    unsafe {
        (*rd).base = 0;
        (*rd).size_bytes = 0;
        (*rd).size_blocks = 0;
        spin_init(&raw mut (*rd).lock, c"ramdisk".as_ptr() as *mut c_char);
    }

    // SAFETY: `platform` populated by `fdt_apply_platform_config` before
    // this runs.
    let (has_ramdisk, ramdisk_base, ramdisk_size) =
        unsafe { (platform.has_ramdisk, platform.ramdisk_base, platform.ramdisk_size) };
    if has_ramdisk == 0 || ramdisk_base == 0 || ramdisk_size == 0 {
        return;
    }

    // SAFETY: `rd` live.
    unsafe {
        (*rd).base = ramdisk_base;
        (*rd).size_bytes = ramdisk_size;
        (*rd).size_blocks = ramdisk_size / 512;

        crate::kprintln!(
            "ramdisk: initialized {} KB ramdisk ({} sectors) at 0x{:x}",
            (*rd).size_bytes / 1024,
            (*rd).size_blocks,
            (*rd).base,
        );
    }

    // Register the ramdisk as a block device.
    let dev = ramdisk_dev_ptr();
    // SAFETY: `dev` exclusively owned at this point (not yet registered/
    // published); zero it first (matches the C static initializer's
    // implicit zero-fill for every field the designated initializer
    // doesn't list), same precedent as `kernel/virtio_disk.rs`.
    unsafe {
        memset(dev as *mut c_void, 0, core::mem::size_of::<blkdev_t>());
        (*dev).dev.major = 3;
        (*dev).dev.minor = 1;
        (*dev).dev.devname = c"ramdisk".as_ptr();
        (*dev).dev.devmode = (S_IFBLK | 0o600) as mode_t;
        (*dev).__bindgen_anon_1.set_readable(1);
        (*dev).__bindgen_anon_1.set_writable(1);
        (*dev).block_shift = 0; // 2^0 * 512 = 512 bytes per block
        (*dev).ops = RAMDISK_OPS;
    }

    // `dev` fully initialised above.
    let errno = blkdev_register(dev);
    if errno != 0 {
        __panic_start();
        // SAFETY: format string matches its one argument.
        crate::kprintln!("ramdisk_init: blkdev_register failed: {}", errno);
        __panic_end();
    }
}
