//! Block device layer -- Rust port of `kernel/dev/blkdev.c` (Phase 2
//! Wave 21; see `docs/rustify/phase2_plan.md`).
//!
//! `blkdev_t` embeds `device_t` as its *first* field
//! (`kernel/inc/dev/dev_types.h`), so every `*mut device_t` <-> `*mut
//! blkdev_t` cast below is a plain offset-0 reinterpretation, matching
//! the C original's `(blkdev_t *)dev` casts -- same reasoning as
//! `kernel/dev/cdev.rs`.
//!
//! `struct bio`/`bio_vec` (`dev/bio_types.h`) are already real bindgen
//! types: transitively reachable since Phase 2 Wave 19
//! (`kernel/vfs/xv6fs/xv6fs_private.h` -> `dev/blkdev.h` -> `dev/bio.h`),
//! and now also explicitly included via `dev/blkdev.h` in `wrapper.h`
//! (this wave). `bio_validate()` itself stays in `kernel/dev/bio.c`
//! (still C, Wave 22) -- declared here as a local extern, matching the
//! established per-file convention.
//!
//! This is a thin dispatch layer on top of [`super::dev`]'s device-table
//! core: `blkdev_register()` installs a fixed [`DeviceOps`] forwarder
//! (`BLKDEV_DEVICE_OPS`, no ioctl -- block devices have none) that
//! forwards `device_t`-level open/release calls into the registrant's
//! own [`BlkdevOps`] trait object (P3-10c: the C-style `blkdev_ops`
//! fn-pointer table is gone; `kernel/virtio_disk.rs`/`kernel/ramdisk.rs`
//! /`kernel/dev/x1_sdhci.rs` install ZST trait implementors instead).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::c_int;

use crate::bindings::{
    bio, blkdev_t, device_t, dev_type_e_DEV_TYPE_BLOCK, EACCES, EINVAL, ENODEV, ENOSYS,
};
use crate::kobject::KArc;
use crate::kstd::{result_to_neg_errno, KResult};

use super::dev::{
    device_dup, device_get, device_put, device_register, device_unregister, DeviceOps,
};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// `err_ptr`/`is_err`'s canonical home is `crate::kstd` (P3-CS1
// centralization).
use crate::kstd::{err_ptr, is_err};

// P3-1D mesh sweep: dev/bio.rs is in scope for this wave; validates a
// bio's fields against its target block device.
use super::bio::bio_validate;

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N4 (device core family, blkdev half).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/dev/
// dev_types.h`'s `struct blkdev_ops`/`struct blkdev` now: `build.rs`
// blocklists the bindgen-generated forms and injects `pub use
// crate::dev::blkdev::... as ...;` facade re-exports (struct name +
// `_t` typedef alias each). The ops-table fn-pointer fields reproduce
// bindgen's forms exactly; the anonymous C bitfield struct became the
// named 8/8 `{bits,_pad}` holder `BlkdevFlagBits` (N3 precedent;
// consumers' `__bindgen_anon_1` sites re-pointed to `.flags`). Copy
// fidelity: `blkdev_ops` derived Copy/Clone in the pre-nativization
// bindgen output; `blkdev` derived NEITHER (it embeds the non-Copy
// `device_t`), so `Blkdev` deliberately has no derives.
// ---------------------------------------------------------------------------

/// The per-driver block-device operations vtable — wave P3-10c's
/// replacement for the C-style `struct blkdev_ops` fn-pointer table
/// (ops-table redesign, following the P3-10a `FileOps` pilot pattern).
/// Implementors are zero-sized unit structs with a `static` instance
/// (`VIRTIO_DISK_OPS` in `kernel/virtio_disk.rs`, `RAMDISK_OPS` in
/// `kernel/ramdisk.rs`, `SDHCI_BLK_OPS` in `kernel/dev/x1_sdhci.rs`)
/// installed into [`Blkdev::ops`] as `Some(&STATIC)`.
///
/// All three methods are *required*: the old `blkdev_ops_validate`
/// rejected registration unless every slot was `Some`, so a per-slot
/// `None` never existed on a registered device — the trait makes that
/// invalid state unrepresentable. "No table at all" (the old null-ops
/// pointer on a zeroed, unregistered `blkdev_t`) is now
/// `Blkdev::ops == None`, and [`blkdev_submit_bio`]'s `ENOSYS` guard
/// keys off exactly that.
///
/// Error encoding: `KResult<()>` — `Err(e)` is encoded to the same raw
/// negative `c_int` the old callbacks returned (via [`Errno::neg`]) at
/// the dispatch boundaries; drivers wrap already-negative cross-module
/// results in `Errno::Raw` so every errno value survives verbatim.
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU.
pub trait BlkdevOps: Sync {
    /// Device open hook, dispatched through the underlying
    /// `device_t`-level ops when the device table opens this device.
    ///
    /// # Safety
    /// `blkdev` must be the live, registered `Blkdev` this instance was
    /// installed on.
    unsafe fn open(&self, blkdev: *mut Blkdev) -> KResult<()>;

    /// Final-teardown hook, dispatched from the kobject release path
    /// once the device's refcount reaches zero.
    ///
    /// # Safety
    /// Same contract as [`BlkdevOps::open`]; the caller holds the final
    /// reference.
    unsafe fn release(&self, blkdev: *mut Blkdev) -> KResult<()>;

    /// Submit a block I/O request. Completion may be asynchronous
    /// (`bio_complete` from an interrupt handler) or synchronous.
    ///
    /// # Safety
    /// `blkdev` as in [`BlkdevOps::open`]; `bio` must be a live,
    /// validated bio (`bio_validate` has passed) whose pages remain
    /// valid until `bio_complete` fires.
    unsafe fn submit_bio(&self, blkdev: *mut Blkdev, bio: *mut bio) -> KResult<()>;
}

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 readable : 1; uint64 writable : 1; }` inside
/// `struct blkdev` (bindgen's `blkdev__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `readable` at bit 0 and
/// `writable` at bit 1 of the (8-byte) unit's byte 0 — identical to
/// bindgen's `get(0,1)`/`get(1,1)` accessors reproduced below.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct BlkdevFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl BlkdevFlagBits {
    #[inline]
    pub(crate) fn readable(&self) -> u64 {
        (self.bits & 0b01) as u64
    }
    #[inline]
    pub(crate) fn set_readable(&mut self, val: u64) {
        self.bits = (self.bits & !0b01) | (((val as u8) & 0b01) << 0);
    }
    #[inline]
    pub(crate) fn writable(&self) -> u64 {
        ((self.bits & 0b10) >> 1) as u64
    }
    #[inline]
    pub(crate) fn set_writable(&mut self, val: u64) {
        self.bits = (self.bits & !0b10) | (((val as u8) & 0b01) << 1);
    }
}

/// `struct blkdev` (typedef `blkdev_t`) — as of wave P3-10c this layout
/// is NATIVE-OWNED (post-P3-6 there is no bindgen, no wrapper.h, and no
/// C consumer; `kernel/inc/dev/dev_types.h` no longer constrains it).
/// The former embedded `blkdev_ops` fn-pointer table is now a real Rust
/// trait object, `Option<&'static dyn BlkdevOps>` (a 16-byte fat
/// pointer; `None` = the zeroed/unregistered state — valid via the
/// null-data-pointer niche, same reasoning as P3-10b's zeroed root
/// dummy).
#[repr(C)]
pub struct Blkdev {
    pub dev: device_t,
    pub flags: BlkdevFlagBits,
    pub block_shift: crate::bindings::uint16,
    pub ops: Option<&'static dyn BlkdevOps>,
}

// P3-10c layout facts — NATIVE-OWNED, no C mirror exists (bindgen,
// wrapper.h, and every C consumer are gone; these asserts document the
// invariants the code actually leans on rather than pinning to a
// header). Kept: `dev` at offset 0 (every `*mut device_t` <-> `*mut
// blkdev_t` cast in this file is a plain offset-0 reinterpretation) and
// the niche-optimized `Option<&'static dyn BlkdevOps>` staying a plain
// fat pointer (16/8, all-zero bytes == `None` — the zeroed-storage
// registration precondition). Deleted: the stale P3-N4 C-fidelity
// size/offset pins for the moved fields and the whole `blkdev_ops`
// table block (the table type itself no longer exists).
const _: () = {
    assert!(core::mem::size_of::<BlkdevFlagBits>() == 8, "blkdev anon bitfield size");
    assert!(core::mem::align_of::<BlkdevFlagBits>() == 8, "blkdev anon bitfield alignment");
    assert!(core::mem::align_of::<Blkdev>() == 8, "blkdev alignment");
    assert!(core::mem::offset_of!(Blkdev, dev) == 0, "blkdev.dev offset (cast invariant)");
    assert!(
        core::mem::size_of::<Option<&'static dyn BlkdevOps>>() == 16,
        "blkdev ops fat pointer size"
    );
    assert!(
        core::mem::align_of::<Option<&'static dyn BlkdevOps>>() == 8,
        "blkdev ops fat pointer alignment"
    );
};

// ---------------------------------------------------------------------------
// Underlying `DeviceOps` forwarder: forwards device_t-level calls into
// the registrant's `BlkdevOps` trait object. Block devices have no
// ioctl -- the trait default (`None` -> dev_ioctl's `ENOTTY` fallback)
// reproduces the old table's implicitly-NULL `.ioctl` slot.
// ---------------------------------------------------------------------------

/// Zero-sized [`DeviceOps`] forwarder installed on every registered
/// blkdev's underlying `device_t` (P3-10c; was the fixed
/// `BLKDEV_UNDERLYING_OPS` `device_ops_t` vtable of `extern "C"`
/// trampolines). Every `*mut device_t` here is really the first field
/// of a `blkdev_t` (offset 0 -- exact cast, matching the C
/// `(blkdev_t *)dev`).
struct BlkdevDeviceOps;

/// The single shared instance `blkdev_register` installs.
static BLKDEV_DEVICE_OPS: BlkdevDeviceOps = BlkdevDeviceOps;

impl DeviceOps for BlkdevDeviceOps {
    unsafe fn open(&self, dev: *mut device_t) -> KResult<()> {
        let blkdev = dev as *mut blkdev_t;
        // SAFETY: dispatch contract (`dev` live, registered) -- `.ops`
        // is `Some` on any registered blkdev (`blkdev_register`'s
        // check; the trait makes per-slot absence unrepresentable).
        let ops = unsafe { (*blkdev).ops }.expect("blkdev: registered device without ops");
        // SAFETY: `blkdev` is the live, registered device this instance
        // was installed on -- exactly `BlkdevOps::open`'s contract.
        unsafe { ops.open(blkdev) }
    }

    unsafe fn release(&self, dev: *mut device_t) -> KResult<()> {
        let blkdev = dev as *mut blkdev_t;
        // SAFETY: see `open` above.
        let ops = unsafe { (*blkdev).ops }.expect("blkdev: registered device without ops");
        // SAFETY: kobject-release contract -- the caller holds the
        // final reference; `blkdev` is live for this call.
        unsafe { ops.release(blkdev) }
    }
}

// ---------------------------------------------------------------------------
// Public C ABI.
// ---------------------------------------------------------------------------

/// Get a block device by its major and minor numbers. Returns the
/// blkdev on success, or `ERR_PTR` on error.
// P3-1D mesh sweep: callers (`bufcache.rs`, `vfs/xv6fs/superblock.rs`,
// `vfs/file.rs`) now import this via crate-path `use` instead of an
// `extern` redeclaration -- demoted.
// P3-9: converted to `KArc<device_t>` -- same manual-get/conditional-put
// leak hazard fixed as `dev/cdev.rs::cdev_get` (see its comment); the
// `Drop` on the "wrong type" branch now releases the reference
// automatically.
pub(crate) extern "C" fn blkdev_get(major: c_int, minor: c_int) -> *mut blkdev_t {
    let device = device_get(major, minor);
    if is_err(device) {
        return device as *mut blkdev_t;
    }
    // SAFETY: `device` is non-null and not an error-pointer here --
    // `device_get`'s success postcondition (one held kobject reference)
    // is exactly `KArc::from_raw`'s precondition.
    let device = unsafe { KArc::<device_t>::from_raw(device) };
    if device.type_ != dev_type_e_DEV_TYPE_BLOCK {
        // `device` (the KArc) drops here, releasing the reference.
        return err_ptr(neg(ENODEV));
    }
    KArc::into_raw(device) as *mut blkdev_t
}

// P3-1D mesh sweep: no live caller anywhere in the tree today (full-tree
// grep) -- demoted; `#[allow(dead_code)]` documents the gap, same
// precedent as `dev/cdev.rs`'s `cdev_dup`.
#[allow(dead_code)]
pub(crate) extern "C" fn blkdev_dup(dev: *mut blkdev_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    device_dup(dev as *mut device_t)
}

// P3-1D mesh sweep: callers (`bufcache.rs`, `vfs/xv6fs/superblock.rs`,
// `vfs/file.rs`) now import this via crate-path `use` instead of an
// `extern` redeclaration -- demoted.
pub(crate) extern "C" fn blkdev_put(dev: *mut blkdev_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    device_put(dev as *mut device_t)
}

// P3-1D mesh sweep: callers (`ramdisk.rs`, `virtio_disk.rs`,
// `dev/x1_sdhci.rs`) now import this via crate-path `use` instead of an
// `extern` redeclaration -- demoted.
pub(crate) extern "C" fn blkdev_register(dev: *mut blkdev_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    // Ops validation (was `blkdev_ops_validate`): the old per-slot
    // `Some` checks are unrepresentable-by-construction now that all
    // three methods are required trait methods -- only "no table at
    // all" remains rejectable.
    // SAFETY: `dev` is caller-provided; reading `.ops` is a plain field
    // read of caller-owned data.
    if unsafe { (*dev).ops }.is_none() {
        return neg(EINVAL);
    }
    let device = dev as *mut device_t;
    // SAFETY: `dev` is caller-exclusive, not yet published
    // (device_register is the point of publication).
    unsafe {
        (*device).type_ = dev_type_e_DEV_TYPE_BLOCK;
        (*device).ops = Some(&BLKDEV_DEVICE_OPS);
    }
    device_register(device)
}

// P3-1D mesh sweep: no live caller anywhere in the tree today (full-tree
// grep) -- demoted; `#[allow(dead_code)]` documents the gap, same
// precedent as `blkdev_dup` above.
#[allow(dead_code)]
pub(crate) extern "C" fn blkdev_unregister(dev: *mut blkdev_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    device_unregister(dev as *mut device_t)
}

// P3-1D mesh sweep: callers (`ramdisk.rs`, `bufcache.rs`,
// `virtio_disk.rs`, `dev/x1_sdhci.rs`, `vfs/xv6fs/superblock.rs`) now
// import this via crate-path `use` instead of an `extern` redeclaration --
// demoted.
pub(crate) extern "C" fn blkdev_submit_bio(blkdev: *mut blkdev_t, bio_ptr: *mut bio) -> c_int {
    if blkdev.is_null() || bio_ptr.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: `blkdev`/`bio_ptr` are caller-provided, live pointers --
    // caller holds references on both (matches the C contract: neither
    // `blkdev_submit_bio` nor its C original take/drop any reference).
    unsafe {
        if (*blkdev).dev.type_ != dev_type_e_DEV_TYPE_BLOCK {
            return neg(ENODEV);
        }
        // A missing table is the old "submit_bio slot `None`" state (a
        // registered device always has `Some` ops) -- same `ENOSYS`.
        let Some(ops) = (*blkdev).ops else {
            return neg(ENOSYS);
        };
        let rw = (*bio_ptr).flags.rw() != 0;
        let writable = (*blkdev).flags.writable() != 0;
        let readable = (*blkdev).flags.readable() != 0;
        if rw && !writable {
            return neg(EACCES);
        }
        if !rw && !readable {
            return neg(EACCES);
        }
        (*bio_ptr).block_shift = (*blkdev).block_shift;

        let ret = bio_validate(bio_ptr, blkdev);
        if ret != 0 {
            return ret;
        }

        // SAFETY: `bio_validate` just passed and the caller keeps `bio`
        // live until completion -- exactly `BlkdevOps::submit_bio`'s
        // contract.
        result_to_neg_errno(ops.submit_bio(blkdev, bio_ptr))
    }
}
