//! Character device layer -- Rust port of `kernel/dev/cdev.c` (Phase 2
//! Wave 21; see `docs/rustify/phase2_plan.md`).
//!
//! `cdev_t` embeds `device_t` as its *first* field
//! (`kernel/inc/dev/dev_types.h`), so every `*mut device_t` <-> `*mut
//! cdev_t` cast below is a plain offset-0 reinterpretation, matching the
//! C original's `(cdev_t *)dev` casts (no `container_of` arithmetic
//! needed, same reasoning as `kernel/dev/dev.rs`'s kobject-release cast).
//!
//! This is a thin dispatch layer on top of [`super::dev`]'s device-table
//! core: `cdev_register()` installs a fixed `device_ops_t` vtable
//! (`CDEV_UNDERLYING_OPS`) that forwards `device_t`-level open/release/
//! ioctl calls into the registrant's own `cdev_ops_t` callbacks. Every
//! Rust cdev registrant (`kernel/console.rs`, `kernel/tty/tty_dev.rs`,
//! `kernel/tty/ptmx.rs`) calls `cdev_register`/`cdev_get`/`cdev_put` by
//! this file's exact symbol names -- their call ABI is unchanged by this
//! port.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_int, c_void};

use crate::bindings::{
    bool_, cdev_ops_t, cdev_t, device_ops_t, device_t, dev_type_e_DEV_TYPE_CHAR, EINVAL, ENODEV,
    ENOSYS,
};

use super::dev::{device_dup, device_get, device_put, device_register, device_unregister};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// `ERR_PTR`/`IS_ERR` (`kernel/inc/errno.h`), generic over the pointee
/// type. Reimplemented locally, matching this crate's established
/// per-file convention (see `kernel/vfs/inode.rs`).
const MAX_ERRNO: isize = 4095;
#[inline(always)]
fn is_err_value(p: usize) -> bool {
    p >= (-(MAX_ERRNO)) as usize
}
#[inline(always)]
fn err_ptr<T>(errno: c_int) -> *mut T {
    errno as isize as *mut T
}
#[inline(always)]
fn is_err<T>(p: *mut T) -> bool {
    is_err_value(p as usize)
}

// ---------------------------------------------------------------------------
// Underlying device_ops_t vtable: forwards device_t-level calls into the
// registrant's cdev_ops_t.
// ---------------------------------------------------------------------------

extern "C" fn underlying_dev_open(dev: *mut device_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    let cdev = dev as *mut cdev_t;
    // SAFETY: `dev` is `cdev_t`'s first field (offset 0) -- exact cast,
    // matching the C `(cdev_t *)dev`; `cdev_register()`'s
    // `cdev_opts_validate` guarantees `.ops.open` is `Some` before
    // registration can succeed, so this device_ops_t.open is only ever
    // installed on a genuinely open-capable cdev.
    unsafe { (*cdev).ops.open.unwrap()(cdev) }
}

extern "C" fn underlying_dev_release(dev: *mut device_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    let cdev = dev as *mut cdev_t;
    // SAFETY: see `underlying_dev_open`; `.ops.release` is likewise
    // guaranteed `Some` by `cdev_opts_validate`.
    unsafe { (*cdev).ops.release.unwrap()(cdev) }
}

extern "C" fn underlying_dev_ioctl(dev: *mut device_t, cmd: u64, arg: *mut c_void) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    let cdev = dev as *mut cdev_t;
    // SAFETY: `dev` is `cdev_t`'s first field; `.ops.ioctl` is read as a
    // plain `Option`, branched on below (ioctl is optional, unlike
    // open/release).
    let ioctl = unsafe { (*cdev).ops.ioctl };
    match ioctl {
        None => neg(ENOSYS),
        // SAFETY: `f` is a valid C-ABI function pointer from the
        // registrant's own `cdev_ops_t`; `cdev` is the same live pointer.
        Some(f) => unsafe { f(cdev, cmd, arg) },
    }
}

static CDEV_UNDERLYING_OPS: device_ops_t = device_ops_t {
    open: Some(underlying_dev_open),
    release: Some(underlying_dev_release),
    ioctl: Some(underlying_dev_ioctl),
};

fn cdev_opts_validate(ops: *const cdev_ops_t) -> bool {
    if ops.is_null() {
        return false;
    }
    // SAFETY: caller (cdev_register) guarantees `ops` points at the
    // live, embedded `dev->ops` field of a caller-owned `cdev_t`.
    let ops = unsafe { &*ops };
    ops.open.is_some() && ops.release.is_some()
}

// ---------------------------------------------------------------------------
// Public C ABI.
// ---------------------------------------------------------------------------

/// Get a character device by its major and minor numbers. Returns the
/// cdev on success, or `ERR_PTR` on error.
// P3-1D mesh sweep: callers (`vfs/file.rs`) now import this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn cdev_get(major: c_int, minor: c_int) -> *mut cdev_t {
    let device = device_get(major, minor);
    if is_err(device) {
        return device as *mut cdev_t; // propagate error from device_get
    }
    // SAFETY: `device` is non-null and not an error-pointer here --
    // `device_get`'s success contract.
    if unsafe { (*device).type_ } != dev_type_e_DEV_TYPE_CHAR {
        device_put(device); // release the device reference
        return err_ptr(neg(ENODEV)); // not a character device
    }
    device as *mut cdev_t
}

// P3-1D mesh sweep: no live caller anywhere in the tree today (full-tree
// grep) -- demoted; `#[allow(dead_code)]` documents the gap rather than
// deleting still-plausible public API, matching this crate's established
// precedent (e.g. `ipi.rs`'s `get_cpu_active_mask`).
#[allow(dead_code)]
pub(crate) extern "C" fn cdev_dup(dev: *mut cdev_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    device_dup(dev as *mut device_t)
}

// P3-1D mesh sweep: callers (`vfs/file.rs`) now import this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn cdev_put(dev: *mut cdev_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    device_put(dev as *mut device_t);
    0
}

// P3-1D mesh sweep: callers (`console.rs`, `tty/tty_dev.rs`, `tty/ptmx.rs`,
// `dev/nullrand.rs`) now import this via crate-path `use` instead of an
// `extern` redeclaration -- demoted.
pub(crate) extern "C" fn cdev_register(dev: *mut cdev_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: `dev` is caller-provided; reading `.ops`'s address is a
    // plain field-address computation on caller-owned data.
    if !cdev_opts_validate(unsafe { &raw const (*dev).ops }) {
        return neg(EINVAL); // invalid character device operations
    }
    let device = dev as *mut device_t;
    // SAFETY: `dev` is caller-exclusive, not yet published (device_register
    // is the point of publication).
    unsafe {
        (*device).type_ = dev_type_e_DEV_TYPE_CHAR;
        (*device).ops = CDEV_UNDERLYING_OPS; // set underlying device operations
    }
    device_register(device)
}

// P3-1D mesh sweep: callers (`tty/ptmx.rs`, `tty/pty.rs`) now import this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn cdev_unregister(dev: *mut cdev_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    device_unregister(dev as *mut device_t)
}

// P3-1D mesh sweep: caller (`vfs/file.rs`) now imports this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn cdev_read(cdev: *mut cdev_t, user: bool_, buf: *mut c_void, count: usize) -> c_int {
    if cdev.is_null() || buf.is_null() || count == 0 {
        return neg(EINVAL); // invalid arguments
    }
    // SAFETY: `cdev` is caller-provided; caller (VFS layer) guarantees
    // liveness for the duration of this call (cdev_read has no
    // refcounting of its own, matching the C original).
    unsafe {
        if (*cdev).dev.type_ != dev_type_e_DEV_TYPE_CHAR {
            return neg(ENODEV); // not a character device
        }
        if (*cdev).__bindgen_anon_1.readable() == 0 || (*cdev).ops.read.is_none() {
            return neg(ENOSYS); // read operation not supported
        }
        (*cdev).ops.read.unwrap()(cdev, user, buf, count)
    }
}

// P3-1D mesh sweep: caller (`vfs/file.rs`) now imports this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn cdev_write(
    cdev: *mut cdev_t,
    user: bool_,
    buf: *const c_void,
    count: usize,
) -> c_int {
    if cdev.is_null() || buf.is_null() || count == 0 {
        return neg(EINVAL); // invalid arguments
    }
    // SAFETY: see `cdev_read`.
    unsafe {
        if (*cdev).dev.type_ != dev_type_e_DEV_TYPE_CHAR {
            return neg(ENODEV); // not a character device
        }
        if (*cdev).__bindgen_anon_1.writable() == 0 || (*cdev).ops.write.is_none() {
            return neg(ENOSYS); // write operation not supported
        }
        (*cdev).ops.write.unwrap()(cdev, user, buf, count)
    }
}
