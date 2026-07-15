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
//! core: `blkdev_register()` installs a fixed `device_ops_t` vtable
//! (`BLKDEV_UNDERLYING_OPS`, no ioctl -- block devices have none) that
//! forwards `device_t`-level open/release calls into the registrant's own
//! `blkdev_ops_t` callbacks. `virtio_disk.c`/`ramdisk.c`/`x1_sdhci.c`
//! (still C) call `blkdev_register`/`blkdev_get`/`blkdev_submit_bio` by
//! this file's exact symbol names -- their call ABI is unchanged by this
//! port.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::c_int;

use crate::bindings::{
    bio, blkdev_ops_t, blkdev_t, device_ops_t, device_t, dev_type_e_DEV_TYPE_BLOCK, EACCES,
    EINVAL, ENODEV, ENOSYS,
};
use crate::kobject::KArc;

use super::dev::{device_dup, device_get, device_put, device_register, device_unregister};

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
// Underlying device_ops_t vtable: forwards device_t-level calls into the
// registrant's blkdev_ops_t. Block devices have no ioctl (`.ioctl: None`,
// matching the C's `{ .open = ..., .release = ... }` designated
// initializer, which leaves `.ioctl` implicitly NULL).
// ---------------------------------------------------------------------------

extern "C" fn underlying_dev_open(dev: *mut device_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    let blkdev = dev as *mut blkdev_t;
    // SAFETY: `dev` is `blkdev_t`'s first field (offset 0) -- exact cast,
    // matching the C `(blkdev_t *)dev`; `blkdev_register()`'s
    // `blkdev_ops_validate` guarantees `.ops.open` is `Some` before
    // registration can succeed.
    unsafe { (*blkdev).ops.open.unwrap()(blkdev) }
}

extern "C" fn underlying_dev_release(dev: *mut device_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    let blkdev = dev as *mut blkdev_t;
    // SAFETY: see `underlying_dev_open`; `.ops.release` is likewise
    // guaranteed `Some` by `blkdev_ops_validate`.
    unsafe { (*blkdev).ops.release.unwrap()(blkdev) }
}

static BLKDEV_UNDERLYING_OPS: device_ops_t = device_ops_t {
    open: Some(underlying_dev_open),
    release: Some(underlying_dev_release),
    ioctl: None,
};

fn blkdev_ops_validate(ops: *const blkdev_ops_t) -> bool {
    if ops.is_null() {
        return false;
    }
    // SAFETY: caller (blkdev_register) guarantees `ops` points at the
    // live, embedded `dev->ops` field of a caller-owned `blkdev_t`.
    let ops = unsafe { &*ops };
    ops.open.is_some() && ops.release.is_some() && ops.submit_bio.is_some()
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
    // SAFETY: `dev` is caller-provided; reading `.ops`'s address is a
    // plain field-address computation on caller-owned data.
    if !blkdev_ops_validate(unsafe { &raw const (*dev).ops }) {
        return neg(EINVAL);
    }
    let device = dev as *mut device_t;
    // SAFETY: `dev` is caller-exclusive, not yet published
    // (device_register is the point of publication).
    unsafe {
        (*device).type_ = dev_type_e_DEV_TYPE_BLOCK;
        (*device).ops = BLKDEV_UNDERLYING_OPS;
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
        if (*blkdev).ops.submit_bio.is_none() {
            return neg(ENOSYS);
        }
        let rw = (*bio_ptr).__bindgen_anon_1.rw() != 0;
        let writable = (*blkdev).__bindgen_anon_1.writable() != 0;
        let readable = (*blkdev).__bindgen_anon_1.readable() != 0;
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

        (*blkdev).ops.submit_bio.unwrap()(blkdev, bio_ptr)
    }
}
