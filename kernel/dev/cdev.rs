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
//! core: `cdev_register()` installs a fixed [`DeviceOps`] forwarder
//! (`CDEV_DEVICE_OPS`) that forwards `device_t`-level open/release/
//! ioctl calls into the registrant's own [`CdevOps`] trait object
//! (P3-10c: the C-style `cdev_ops` fn-pointer table is gone; every
//! registrant -- `kernel/console.rs`, `kernel/tty/tty_dev.rs`,
//! `kernel/tty/ptmx.rs`, `kernel/dev/nullrand.rs` -- installs a ZST
//! trait implementor instead).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_int, c_void};

use crate::bindings::{
    bool_, cdev_t, device_t, dev_type_e_DEV_TYPE_CHAR, EINVAL, ENODEV, ENOSYS,
};
use crate::kobject::KArc;
use crate::kstd::{Errno, KResult};

use super::dev::{DeviceInstance, DeviceOps};

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

/// The per-driver character-device operations vtable — wave P3-10c's
/// replacement for the C-style `struct cdev_ops` fn-pointer table
/// (ops-table redesign, following the P3-10a `FileOps` pilot pattern).
/// Implementors are zero-sized unit structs with a `static` instance
/// (`CONSOLE_CDEV_OPS` in `kernel/console.rs`, `TTY_CDEV_OPS` in
/// `kernel/tty/tty_dev.rs`, `NULL/RANDOM/ZERO_CDEV_OPS` in
/// `kernel/dev/nullrand.rs`, `PTS/PTMX_CDEV_OPS` in
/// `kernel/tty/ptmx.rs`) installed into [`Cdev::ops`] as
/// `Some(&STATIC)`.
///
/// Slot-nullability mapping (each old `Option<fn>` slot's `None`
/// dispatch behavior is preserved exactly):
///
/// * `open`/`release` — required methods: the old `cdev_opts_validate`
///   rejected registration unless both were `Some`, so "table present
///   but slot `None`" never existed on a registered device.
/// * `read`/`write` — default `Err(Errno::NoSys)`: exactly what
///   [`cdev_read`]/[`cdev_write`] returned for a `None` slot (the
///   ptmx/pts cdevs, whose I/O goes through `FileOps` instead, are the
///   live users of this default).
/// * `ioctl` — default `Err(Errno::NoSys)`: exactly the old
///   `underlying_dev_ioctl` `None`-slot return. `Ok(n)` is the raw
///   non-negative driver result; already-negative cross-module results
///   ride in `Errno::Raw` so every value crosses the `device_t`-level
///   dispatch verbatim.
/// * `poll` — returns `Option<c_int>` (tri-state, mirrors
///   `FileOps::poll`): `None` means "op not provided" and selects
///   `vfs_poll_scan`'s always-ready fallback, which a sentinel `Errno`
///   could not represent without conflating it with a driver result.
/// * `open_file` — returns `Option<c_int>` (tri-state): `None` selects
///   `open_cdev`'s direct-device-I/O fallback (`file->ops = None`,
///   VFS stores the cdev); `Some(0)` means the driver took over the
///   file (it may have installed `file->ops`); `Some(neg)` is the
///   driver's raw failure, returned to the opener as-is.
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU.
pub trait CdevOps: Sync {
    /// Device open hook, dispatched through the underlying
    /// `device_t`-level ops.
    ///
    /// # Safety
    /// `cdev` must be the live, registered `Cdev` this instance was
    /// installed on.
    unsafe fn open(&self, cdev: *mut Cdev) -> KResult<()>;

    /// Final-teardown hook, dispatched from the kobject release path
    /// once the device's refcount reaches zero.
    ///
    /// # Safety
    /// Same contract as [`CdevOps::open`]; the caller holds the final
    /// reference.
    unsafe fn release(&self, cdev: *mut Cdev) -> KResult<()>;

    /// Read up to `count` bytes into `buf` (a user VA when `user`).
    /// `Ok(n)` is the byte count actually read.
    ///
    /// # Safety
    /// `cdev` as in [`CdevOps::open`]; `buf` must be valid for `count`
    /// bytes in the `user`-selected address space. May sleep.
    unsafe fn read(&self, _cdev: *mut Cdev, _user: bool, _buf: *mut c_void, _count: usize)
        -> KResult<c_int> {
        Err(Errno::NoSys) // Read op not provided (old `None`-slot behavior).
    }

    /// Write up to `count` bytes from `buf` (a user VA when `user`).
    /// `Ok(n)` is the byte count actually written.
    ///
    /// # Safety
    /// Same contract as [`CdevOps::read`].
    unsafe fn write(&self, _cdev: *mut Cdev, _user: bool, _buf: *const c_void, _count: usize)
        -> KResult<c_int> {
        Err(Errno::NoSys) // Write op not provided (old `None`-slot behavior).
    }

    /// Driver ioctl. `Ok(n)` is the raw non-negative result passed
    /// through to the caller exactly as the old fn-pointer result was.
    ///
    /// # Safety
    /// `cdev` as in [`CdevOps::open`]; `arg`'s validity is
    /// `cmd`-specific.
    unsafe fn ioctl(&self, _cdev: *mut Cdev, _cmd: u64, _arg: *mut c_void) -> KResult<c_int> {
        Err(Errno::NoSys) // Ioctl op not provided (old `None`-slot behavior).
    }

    /// Poll for readiness: `Some(revents)` if this driver implements
    /// polling, `None` to let `vfs_poll_scan` fall back to
    /// always-ready.
    ///
    /// # Safety
    /// `cdev` as in [`CdevOps::open`].
    unsafe fn poll(&self, _cdev: *mut Cdev, _events: ::core::ffi::c_short) -> Option<c_int> {
        None
    }

    /// File-open takeover hook: `Some(ret)` if this driver handles the
    /// open itself (see the trait doc for the `ret` contract), `None`
    /// to let `open_cdev` fall back to direct device I/O.
    ///
    /// # Safety
    /// `cdev` as in [`CdevOps::open`]; `file` must be the live,
    /// not-yet-published `vfs_file` being opened.
    unsafe fn open_file(&self, _cdev: *mut Cdev, _file: *mut crate::bindings::vfs_file)
        -> Option<c_int> {
        None
    }
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

/// `struct cdev` (typedef `cdev_t`) — as of wave P3-10c this layout is
/// NATIVE-OWNED (post-P3-6 there is no bindgen, no wrapper.h, and no C
/// consumer; `kernel/inc/dev/dev_types.h` no longer constrains it).
/// The former embedded `cdev_ops` fn-pointer table is now a real Rust
/// trait object, `Option<&'static dyn CdevOps>` (a 16-byte fat pointer;
/// `None` = the zeroed/unregistered state — valid via the
/// null-data-pointer niche, same reasoning as P3-10b's zeroed root
/// dummy).
#[repr(C)]
pub struct Cdev {
    pub dev: device_t,
    pub flags: CdevFlagBits,
    pub ops: Option<&'static dyn CdevOps>,
}

// P3-10c layout facts — NATIVE-OWNED, no C mirror exists (bindgen,
// wrapper.h, and every C consumer are gone; these asserts document the
// invariants the code actually leans on rather than pinning to a
// header). Kept: `dev` at offset 0 (every `*mut device_t` <-> `*mut
// cdev_t` cast in this file is a plain offset-0 reinterpretation) and
// the niche-optimized `Option<&'static dyn CdevOps>` staying a plain
// fat pointer (16/8, all-zero bytes == `None` — the zeroed-storage
// registration precondition). Deleted: the stale P3-N4 C-fidelity
// size/offset pins for the moved fields and the whole `cdev_ops` table
// block (the table type itself no longer exists).
const _: () = {
    assert!(core::mem::size_of::<CdevFlagBits>() == 8, "cdev anon bitfield size");
    assert!(core::mem::align_of::<CdevFlagBits>() == 8, "cdev anon bitfield alignment");
    assert!(core::mem::align_of::<Cdev>() == 8, "cdev alignment");
    assert!(core::mem::offset_of!(Cdev, dev) == 0, "cdev.dev offset (cast invariant)");
    assert!(
        core::mem::size_of::<Option<&'static dyn CdevOps>>() == 16,
        "cdev ops fat pointer size"
    );
    assert!(
        core::mem::align_of::<Option<&'static dyn CdevOps>>() == 8,
        "cdev ops fat pointer alignment"
    );
};

// ---------------------------------------------------------------------------
// Underlying `DeviceOps` forwarder: forwards device_t-level calls into
// the registrant's `CdevOps` trait object.
// ---------------------------------------------------------------------------

/// Zero-sized [`DeviceOps`] forwarder installed on every registered
/// cdev's underlying `device_t` (P3-10c; was the fixed
/// `CDEV_UNDERLYING_OPS` `device_ops_t` vtable of `extern "C"`
/// trampolines). Every `*mut device_t` here is really the first field
/// of a `cdev_t` (offset 0 -- exact cast, matching the C
/// `(cdev_t *)dev`).
struct CdevDeviceOps;

/// The single shared instance `cdev_register` installs.
static CDEV_DEVICE_OPS: CdevDeviceOps = CdevDeviceOps;

impl DeviceOps for CdevDeviceOps {
    unsafe fn open(&self, dev: *mut device_t) -> KResult<()> {
        let cdev = dev as *mut cdev_t;
        // SAFETY: dispatch contract (`dev` live, registered) -- `.ops`
        // is `Some` on any registered cdev (`cdev_register`'s check);
        // open/release are required trait methods, so per-slot absence
        // is unrepresentable.
        let ops = unsafe { (*cdev).ops }.expect("cdev: registered device without ops");
        // SAFETY: `cdev` is the live, registered device this instance
        // was installed on -- exactly `CdevOps::open`'s contract.
        unsafe { ops.open(cdev) }
    }

    unsafe fn release(&self, dev: *mut device_t) -> KResult<()> {
        let cdev = dev as *mut cdev_t;
        // SAFETY: see `open` above.
        let ops = unsafe { (*cdev).ops }.expect("cdev: registered device without ops");
        // SAFETY: kobject-release contract -- the caller holds the
        // final reference; `cdev` is live for this call.
        unsafe { ops.release(cdev) }
    }

    unsafe fn ioctl(&self, dev: *mut device_t, cmd: u64, arg: *mut c_void) -> Option<c_int> {
        let cdev = dev as *mut cdev_t;
        // SAFETY: dispatch contract; reading `.ops` is a plain field
        // read. A `None` table can't occur on a registered cdev, but
        // `neg(ENOSYS)` (the trait default's value) keeps the old
        // `None`-slot behavior for a hypothetical torn-down device.
        let Some(ops) = (unsafe { (*cdev).ops }) else {
            return Some(neg(ENOSYS));
        };
        // SAFETY: `cdev` is the live device being dispatched on;
        // `arg`'s validity is forwarded from `dev_ioctl`'s contract.
        // `Ok(n)`/`Err(Raw(n))` re-encode to the exact `c_int` the old
        // fn-pointer returned; the default method's `Err(NoSys)` is the
        // old `None`-slot `neg(ENOSYS)` (deliberately `Some(..)` here,
        // NOT the device-level `ENOTTY` fallback -- cdevs always
        // provided a device-level ioctl trampoline).
        Some(match unsafe { ops.ioctl(cdev, cmd, arg) } {
            Ok(n) => n,
            Err(e) => e.neg(),
        })
    }
}

// ---------------------------------------------------------------------------
// Public C ABI.
//
// KERNEL-OO: relocated onto `impl Cdev` -- behavior-preserving only: raw
// `*mut cdev_t` params kept (`Cdev` is Freeze and shared cross-hart via
// the device table's refcount/RCU, so no `&self` receiver), bodies
// byte-identical, `unsafe extern "C"` signatures preserved exactly.
// ---------------------------------------------------------------------------

impl Cdev {
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
    pub(crate) extern "C" fn get(major: c_int, minor: c_int) -> *mut cdev_t {
        let device = DeviceInstance::get(major, minor);
        if is_err(device) {
            return device as *mut cdev_t; // propagate error from device_get
        }
        // SAFETY: `device` is non-null and not an error-pointer here --
        // `DeviceInstance::get`'s success postcondition (one held kobject
        // reference) is exactly `KArc::from_raw`'s precondition.
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
    pub(crate) extern "C" fn dup(dev: *mut cdev_t) -> c_int {
        if dev.is_null() {
            return neg(EINVAL);
        }
        DeviceInstance::dup(dev as *mut device_t)
    }

    // P3-1D mesh sweep: callers (`vfs/file.rs`) now import this via crate-path
    // `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn put(dev: *mut cdev_t) -> c_int {
        if dev.is_null() {
            return neg(EINVAL);
        }
        DeviceInstance::put(dev as *mut device_t);
        0
    }

    // P3-1D mesh sweep: callers (`console.rs`, `tty/tty_dev.rs`, `tty/ptmx.rs`,
    // `dev/nullrand.rs`) now import this via crate-path `use` instead of an
    // `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn register(dev: *mut cdev_t) -> c_int {
        if dev.is_null() {
            return neg(EINVAL);
        }
        // Ops validation (was `cdev_opts_validate`): the old open/release
        // `Some` checks are unrepresentable-by-construction now that both
        // are required trait methods -- only "no table at all" remains
        // rejectable.
        // SAFETY: `dev` is caller-provided; reading `.ops` is a plain field
        // read of caller-owned data.
        if unsafe { (*dev).ops }.is_none() {
            return neg(EINVAL); // invalid character device operations
        }
        let device = dev as *mut device_t;
        // SAFETY: `dev` is caller-exclusive, not yet published
        // (`DeviceInstance::register` is the point of publication).
        unsafe {
            (*device).type_ = dev_type_e_DEV_TYPE_CHAR;
            (*device).ops = Some(&CDEV_DEVICE_OPS); // set underlying device operations
        }
        DeviceInstance::register(device)
    }

    // P3-1D mesh sweep: callers (`tty/ptmx.rs`, `tty/pty.rs`) now import this
    // via crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn unregister(dev: *mut cdev_t) -> c_int {
        if dev.is_null() {
            return neg(EINVAL);
        }
        DeviceInstance::unregister(dev as *mut device_t)
    }

    // P3-1D mesh sweep: caller (`vfs/file.rs`) now imports this via crate-path
    // `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn read(cdev: *mut cdev_t, user: bool_, buf: *mut c_void, count: usize) -> c_int {
        if cdev.is_null() || buf.is_null() || count == 0 {
            return neg(EINVAL); // invalid arguments
        }
        // SAFETY: `cdev` is caller-provided; caller (VFS layer) guarantees
        // liveness for the duration of this call (`Cdev::read` has no
        // refcounting of its own, matching the C original).
        unsafe {
            if (*cdev).dev.type_ != dev_type_e_DEV_TYPE_CHAR {
                return neg(ENODEV); // not a character device
            }
            // A missing table and the trait's default `Err(NoSys)` `read`
            // both reproduce the old `None`-slot `neg(ENOSYS)`.
            if (*cdev).flags.readable() == 0 {
                return neg(ENOSYS); // read operation not supported
            }
            let Some(ops) = (*cdev).ops else {
                return neg(ENOSYS);
            };
            match ops.read(cdev, user != 0, buf, count) {
                Ok(n) => n,
                Err(e) => e.neg(),
            }
        }
    }

    // P3-1D mesh sweep: caller (`vfs/file.rs`) now imports this via crate-path
    // `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn write(
        cdev: *mut cdev_t,
        user: bool_,
        buf: *const c_void,
        count: usize,
    ) -> c_int {
        if cdev.is_null() || buf.is_null() || count == 0 {
            return neg(EINVAL); // invalid arguments
        }
        // SAFETY: see `Cdev::read`.
        unsafe {
            if (*cdev).dev.type_ != dev_type_e_DEV_TYPE_CHAR {
                return neg(ENODEV); // not a character device
            }
            // Same `None`-table / default-`Err(NoSys)` mapping as
            // `Cdev::read`.
            if (*cdev).flags.writable() == 0 {
                return neg(ENOSYS); // write operation not supported
            }
            let Some(ops) = (*cdev).ops else {
                return neg(ENOSYS);
            };
            match ops.write(cdev, user != 0, buf, count) {
                Ok(n) => n,
                Err(e) => e.neg(),
            }
        }
    }
}
