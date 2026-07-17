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
use core::ffi::{c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{fence, Ordering};

use crate::bindings::{bio, bio_vec, blkdev_t, mode_t, page_t, platform_info, EINVAL};

use crate::dev::blkdev::BlkdevOps;
use crate::kstd::{Errno, KResult};

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.

    // string.rs.
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;

    // string.rs.
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

}

// P3-D3c: `dev/fdt.rs`'s boot-probed platform config is a plain
// crate-path import now that its `#[no_mangle]` export is gone (same
// `platform_info` type, unchanged call sites -- reads of a `static mut`
// stay `unsafe` either way).
use crate::dev::fdt::platform;

// P3-D3a: `__page_to_pa` is genuinely `unsafe fn` in `crate::mm::page`
// now that its `#[no_mangle]` export is gone; this file's original
// extern declaration asserted `safe fn` (usual FFI facade) with the
// bindgen `page_t` view rather than page.rs's own `Page` struct (same
// layout, different Rust name). The thin wrapper preserves both.
/// SAFETY: `page` must be a live `Page`
/// (see [`crate::mm::page::__page_to_pa`]'s contract).
#[inline]
fn __page_to_pa(page: *mut page_t) -> u64 {
    unsafe { crate::mm::__page_to_pa(page as *mut crate::mm::page::Page) }
}
// P3-1D mesh sweep: dev/blkdev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::dev::blkdev::blkdev_register;
// P3-D3b: lock/completion.rs's entry points are plain safe Rust fns now
// that their `#[no_mangle]` exports are gone; reached by crate path.
use crate::lock::completion::{complete_all, completion_reinit};

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
        completion_reinit(&raw mut (*bio_ptr).io_completion);
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
        complete_all(&raw mut (*bio_ptr).io_completion);
    }
}

// ===========================================================================
// State.
// ===========================================================================

/// `base`/`size_bytes`/`size_blocks` -- the ramdisk's location/extent,
/// set up once by [`ramdisk_init`] and read (never mutated again) by
/// every [`ramdisk_submit_bio`] call thereafter.
///
/// Wave P3-8b: this used to be a `spinlock_t` field embedded in the
/// struct plus a raw `spin_lock`/`spin_unlock` pair around every access
/// (see git history) -- now a [`crate::sync::SpinLock`] owns the data
/// directly, so a held [`crate::sync::SpinLockGuard`] `Deref`s straight
/// to these three fields with no `unsafe` needed for the field access
/// itself, and every early-return exit path in `ramdisk_submit_bio`
/// releases the lock for free when the guard drops (RAII) instead of a
/// hand-paired `spin_unlock` before each `return`.
struct RamdiskState {
    base: u64,
    size_bytes: u64,
    size_blocks: u64,
}

static RAMDISK: crate::sync::SpinLock<RamdiskState> =
    crate::sync::SpinLock::new(c"ramdisk", RamdiskState { base: 0, size_bytes: 0, size_blocks: 0 });

#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get()
    }
}

static RAMDISK_DEV: SyncCell<MaybeUninit<blkdev_t>> = SyncCell(UnsafeCell::new(MaybeUninit::uninit()));

#[inline(always)]
fn ramdisk_dev_ptr() -> *mut blkdev_t {
    RAMDISK_DEV.get() as *mut blkdev_t
}

// ===========================================================================
// Block device interface.
// ===========================================================================

/// Zero-sized [`BlkdevOps`] implementor for the ramdisk (P3-10c; was
/// the `static blkdev_ops_t RAMDISK_OPS` fn-pointer table -- the three
/// former `extern "C"` callbacks are trait methods now).
struct RamdiskOps;

/// The single shared instance `ramdisk_init` installs.
static RAMDISK_OPS: RamdiskOps = RamdiskOps;

impl BlkdevOps for RamdiskOps {
    unsafe fn open(&self, _blkdev: *mut blkdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn release(&self, _blkdev: *mut blkdev_t) -> KResult<()> {
        Ok(())
    }
    unsafe fn submit_bio(&self, blkdev: *mut blkdev_t, bio_ptr: *mut bio) -> KResult<()> {
        ramdisk_submit_bio(blkdev, bio_ptr)
    }
}

fn ramdisk_submit_bio(_blkdev: *mut blkdev_t, bio_ptr: *mut bio) -> KResult<()> {
    let rd = RAMDISK.lock();

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
            // Release the lock before invoking the completion callback
            // (matches the original's lock-hold window: never call out
            // to `bio_complete` while still holding `RAMDISK`).
            drop(rd);
            // SAFETY: `bio_ptr` live.
            unsafe {
                (*bio_ptr).error = -(EINVAL as c_int);
                bio_complete(bio_ptr);
            }
            return Err(Errno::Inval);
        }

        // Calculate offset in ramdisk.
        let offset = sector * 512;

        // Check bounds. `rd.size_bytes` -- plain field read through the
        // guard, no `unsafe` (the lock proves exclusive access).
        if offset + bvec.len as u64 > rd.size_bytes {
            // SAFETY: format string matches its three arguments.
            crate::kprintln!(
                "ramdisk: access beyond end of device (offset={:x}, len={}, size={:x})",
                offset,
                bvec.len as c_int,
                rd.size_bytes,
            );
            drop(rd);
            // SAFETY: `bio_ptr` live.
            unsafe {
                (*bio_ptr).error = -(EINVAL as c_int);
                bio_complete(bio_ptr);
            }
            return Err(Errno::Inval);
        }

        let pa = __page_to_pa(page) as *mut c_void;
        if pa.is_null() {
            drop(rd);
            // SAFETY: `bio_ptr` live.
            unsafe {
                (*bio_ptr).error = -(EINVAL as c_int);
                bio_complete(bio_ptr);
            }
            return Err(Errno::Inval);
        }

        // Direct access to contiguous physical memory.
        let ramdisk_addr = (rd.base + offset) as *mut c_void;

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

    drop(rd);

    // SAFETY: `bio_ptr` live.
    unsafe {
        (*bio_ptr).error = 0;
        bio_complete(bio_ptr);
    }
    Ok(())
}

/// `void ramdisk_init(void)`.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn ramdisk_init() {
    // `RAMDISK` starts life already in the `{ base: 0, size_bytes: 0,
    // size_blocks: 0 }` state (its `SpinLock::new` is a `const fn`), so
    // there is no separate "zero it, then `spin_init` the lock" step
    // left to do here -- unlike the C original / this file's pre-P3-8b
    // Rust port.

    // SAFETY: `platform` populated by `fdt_apply_platform_config` before
    // this runs.
    let (has_ramdisk, ramdisk_base, ramdisk_size) =
        unsafe { (platform.has_ramdisk, platform.ramdisk_base, platform.ramdisk_size) };
    if has_ramdisk == 0 || ramdisk_base == 0 || ramdisk_size == 0 {
        return;
    }

    {
        let mut rd = RAMDISK.lock();
        rd.base = ramdisk_base;
        rd.size_bytes = ramdisk_size;
        rd.size_blocks = ramdisk_size / 512;

        crate::kprintln!(
            "ramdisk: initialized {} KB ramdisk ({} sectors) at 0x{:x}",
            rd.size_bytes / 1024,
            rd.size_blocks,
            rd.base,
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
        (*dev).flags.set_readable(1);
        (*dev).flags.set_writable(1);
        (*dev).block_shift = 0; // 2^0 * 512 = 512 bytes per block
        (*dev).ops = Some(&RAMDISK_OPS);
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
