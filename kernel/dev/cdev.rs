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
use crate::kobject::KArc;

use super::dev::{device_dup, device_get, device_put, device_register, device_unregister};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// `err_ptr`/`is_err`'s canonical home is `crate::kstd` (P3-CS1
// centralization).
use crate::kstd::{err_ptr, is_err};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N4 (device core family, cdev half).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/dev/
// dev_types.h`'s `struct cdev_ops`/`struct cdev` now: `build.rs`
// blocklists the bindgen-generated forms and injects `pub use
// crate::dev::cdev::... as ...;` facade re-exports (struct name + `_t`
// typedef alias each). The ops-table fn-pointer fields reproduce
// bindgen's forms exactly; the anonymous C bitfield struct
// `struct { uint64 readable : 1; uint64 writable : 1; }` became the
// named 8/8 `{bits,_pad}` holder `CdevFlagBits` with safe masking
// accessors bit-identical to bindgen's little-endian unit (N3
// precedent; consumers' `__bindgen_anon_1` sites re-pointed to
// `.flags`). Copy fidelity: `cdev_ops` derived Copy/Clone in the
// pre-nativization bindgen output; `cdev` derived NEITHER (it embeds
// the non-Copy `device_t`), so `Cdev` deliberately has no derives.
// ---------------------------------------------------------------------------

/// `struct cdev_ops` (typedef `cdev_ops_t`, `kernel/inc/dev/dev_types.h`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct CdevOps {
    pub read: Option<
        unsafe extern "C" fn(
            cdev: *mut Cdev,
            user: bool_,
            buf: *mut c_void,
            count: usize,
        ) -> c_int,
    >,
    pub write: Option<
        unsafe extern "C" fn(
            cdev: *mut Cdev,
            user: bool_,
            buf: *const c_void,
            count: usize,
        ) -> c_int,
    >,
    pub open: Option<unsafe extern "C" fn(cdev: *mut Cdev) -> c_int>,
    pub release: Option<unsafe extern "C" fn(cdev: *mut Cdev) -> c_int>,
    pub ioctl: Option<
        unsafe extern "C" fn(
            cdev: *mut Cdev,
            cmd: crate::bindings::uint64,
            arg: *mut c_void,
        ) -> c_int,
    >,
    pub poll: Option<
        unsafe extern "C" fn(cdev: *mut Cdev, events: ::core::ffi::c_short) -> c_int,
    >,
    pub open_file: Option<
        unsafe extern "C" fn(cdev: *mut Cdev, file: *mut crate::bindings::vfs_file) -> c_int,
    >,
}

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 readable : 1; uint64 writable : 1; }` inside
/// `struct cdev` (bindgen's `cdev__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `readable` at bit 0 and
/// `writable` at bit 1 of the (8-byte) unit's byte 0 — identical to
/// bindgen's `get(0,1)`/`get(1,1)` accessors reproduced below.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct CdevFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl CdevFlagBits {
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

/// `struct cdev` (typedef `cdev_t`, `kernel/inc/dev/dev_types.h`).
#[repr(C)]
pub struct Cdev {
    pub dev: device_t,
    pub flags: CdevFlagBits,
    pub ops: CdevOps,
}

// P3-N4 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// cdev_ops { read, write, open, release, ioctl, poll, open_file }` (all
// `Option<unsafe extern "C" fn ...>`), `pub struct cdev { dev: device_t,
// __bindgen_anon_1: cdev__bindgen_ty_1, ops: cdev_ops_t }`) and
// independently confirmed by a riscv64-unknown-elf-gcc `_Static_assert`
// probe (rv64gc/lp64d) against `kernel/inc/dev/dev_types.h` — see the
// P3-N4 wave record: cdev_ops 56/8 offsets 0/8/16/24/32/40/48, cdev
// 160/8 offsets 0/[96]/104 (the anonymous bitfield struct occupies
// [96,104), pinned in the probe by its predecessor plus the `ops`
// offset).
const _: () = {
    assert!(core::mem::size_of::<CdevOps>() == 56, "cdev_ops size");
    assert!(core::mem::align_of::<CdevOps>() == 8, "cdev_ops alignment");
    assert!(core::mem::offset_of!(CdevOps, read) == 0, "cdev_ops.read offset");
    assert!(core::mem::offset_of!(CdevOps, write) == 8, "cdev_ops.write offset");
    assert!(core::mem::offset_of!(CdevOps, open) == 16, "cdev_ops.open offset");
    assert!(core::mem::offset_of!(CdevOps, release) == 24, "cdev_ops.release offset");
    assert!(core::mem::offset_of!(CdevOps, ioctl) == 32, "cdev_ops.ioctl offset");
    assert!(core::mem::offset_of!(CdevOps, poll) == 40, "cdev_ops.poll offset");
    assert!(core::mem::offset_of!(CdevOps, open_file) == 48, "cdev_ops.open_file offset");
    assert!(core::mem::size_of::<CdevFlagBits>() == 8, "cdev anon bitfield size");
    assert!(core::mem::align_of::<CdevFlagBits>() == 8, "cdev anon bitfield alignment");
    assert!(core::mem::size_of::<Cdev>() == 160, "cdev size");
    assert!(core::mem::align_of::<Cdev>() == 8, "cdev alignment");
    assert!(core::mem::offset_of!(Cdev, dev) == 0, "cdev.dev offset");
    assert!(core::mem::offset_of!(Cdev, flags) == 96, "cdev anon bitfield offset");
    assert!(core::mem::offset_of!(Cdev, ops) == 104, "cdev.ops offset");
};

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
// P3-9: converted to `KArc<device_t>` -- was a manual
// `device_get()`/conditional-`device_put()` pair (see git history); the
// `device_put` on the "wrong type" branch was easy to forget if a third
// branch were ever added, and easy to double-call by accident. Now the
// KArc's `Drop` releases the reference automatically on every exit path
// except the one that hands it back out via `into_raw`.
pub(crate) extern "C" fn cdev_get(major: c_int, minor: c_int) -> *mut cdev_t {
    let device = device_get(major, minor);
    if is_err(device) {
        return device as *mut cdev_t; // propagate error from device_get
    }
    // SAFETY: `device` is non-null and not an error-pointer here --
    // `device_get`'s success postcondition (one held kobject reference)
    // is exactly `KArc::from_raw`'s precondition.
    let device = unsafe { KArc::<device_t>::from_raw(device) };
    if device.type_ != dev_type_e_DEV_TYPE_CHAR {
        // `device` (the KArc) drops here, releasing the reference --
        // no manual `device_put` call needed.
        return err_ptr(neg(ENODEV)); // not a character device
    }
    KArc::into_raw(device) as *mut cdev_t
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
        if (*cdev).flags.readable() == 0 || (*cdev).ops.read.is_none() {
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
        if (*cdev).flags.writable() == 0 || (*cdev).ops.write.is_none() {
            return neg(ENOSYS); // write operation not supported
        }
        (*cdev).ops.write.unwrap()(cdev, user, buf, count)
    }
}
