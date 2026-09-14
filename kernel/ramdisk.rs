//! Memory-backed block device using the shared BIO submission lifecycle.
//!
//! A checked request iterator supplies page-bounded segments and advances in
//! 512-byte sectors. Copies complete synchronously under the ramdisk lock;
//! dropping the submission sentinel after unlocking publishes completion once.
//! The FDT initrd region remains reserved for the lifetime of the device.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;

use crate::bindings::{bio, blkdev_t, mode_t};

use crate::dev::blkdev::BlkdevOps;
use crate::kstd::{Errno, KResult};

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::Printf;

unsafe extern "C" {
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
}

// P3-D3c: `dev/fdt.rs`'s boot-probed platform config is a plain
// crate-path import now that its `#[no_mangle]` export is gone (same
// `platform_info` type, unchanged call sites -- reads of a `static mut`
// stay `unsafe` either way).
use crate::dev::fdt::platform;

// P3-1D mesh sweep: dev/blkdev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::dev::blkdev::Blkdev;
/// `kernel/inc/uabi/stat.h` `S_IFBLK`, same local copy as other `dev/*.rs`.
const S_IFBLK: u32 = 0o060_000;

pub(crate) struct Ramdisk;

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

static RAMDISK: crate::sync::SpinLock<RamdiskState> = crate::sync::SpinLock::new(
    c"ramdisk",
    RamdiskState {
        base: 0,
        size_bytes: 0,
        size_blocks: 0,
    },
);

#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get()
    }
}

static RAMDISK_DEV: SyncCell<MaybeUninit<blkdev_t>> =
    SyncCell(UnsafeCell::new(MaybeUninit::uninit()));

impl Ramdisk {
    #[inline(always)]
    fn dev_ptr() -> *mut blkdev_t {
        RAMDISK_DEV.get() as *mut blkdev_t
    }
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
        // SAFETY: the block layer forwards its validated submission contract.
        unsafe { Ramdisk::submit_bio(blkdev, bio_ptr) }
    }
}

impl Ramdisk {
    /// # Safety
    /// The block layer pins the BIO, device and segment pages until completion.
    unsafe fn submit_bio(_blkdev: *mut blkdev_t, bio_ptr: *mut bio) -> KResult<()> {
        // The dispatcher's caller pins the request and its pages through completion.
        let mut request = unsafe { crate::dev::bio::Bio::begin(bio_ptr) }?;
        let rd = RAMDISK.lock();
        let mut result = Ok(());
        for part in request.by_ref() {
            let address = part
                .sector()
                .checked_mul(512)
                .filter(|&offset| {
                    offset
                        .checked_add(part.len() as u64)
                        .is_some_and(|end| end <= rd.size_bytes)
                })
                .and_then(|offset| rd.base.checked_add(offset))
                .filter(|&address| address.checked_add(part.len() as u64).is_some());
            if let Some(address) = address {
                let disk_data = address as *mut u8;
                // RAMDISK's lock serializes access to its validated mapped region.
                // The part pins an in-bounds page span until its copy completes.
                // copy permits overlap without constructing aliased byte slices.
                unsafe {
                    if part.write() {
                        ptr::copy(part.data_ptr(), disk_data, part.len());
                    } else {
                        ptr::copy(disk_data, part.data_ptr(), part.len());
                    }
                }
                part.complete(Ok(()));
            } else {
                part.complete(Err(Errno::Inval));
                result = Err(Errno::Inval);
                break;
            }
        }
        drop(rd);
        // Release the submission sentinel only after releasing the device lock;
        // this may run the final callback and wake the waiting buffer owner.
        drop(request);
        result
    }

    /// `void ramdisk_init(void)`.
    // P3-1D mesh sweep: caller (`start_kernel.rs`) reaches this via a
    // crate-path `use` of `Ramdisk::init` (not an `extern` redeclaration).
    pub(crate) extern "C" fn init() {
        // `RAMDISK` starts life already in the `{ base: 0, size_bytes: 0,
        // size_blocks: 0 }` state (its `SpinLock::new` is a `const fn`), so
        // there is no separate "zero it, then `spin_init` the lock" step
        // left to do here -- unlike the C original / this file's pre-P3-8b
        // Rust port.

        // SAFETY: `platform` populated by `fdt_apply_platform_config` before
        // this runs.
        let (has_ramdisk, ramdisk_base, ramdisk_size) = unsafe {
            (
                platform.has_ramdisk,
                platform.ramdisk_base,
                platform.ramdisk_size,
            )
        };
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
        let dev = Self::dev_ptr();
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
        let errno = Blkdev::register(dev);
        if errno != 0 {
            Printf::__panic_start();
            // SAFETY: format string matches its one argument.
            crate::kprintln!("ramdisk_init: blkdev_register failed: {}", errno);
            Printf::__panic_end();
        }
    }
} // impl Ramdisk (submit_bio/init)
