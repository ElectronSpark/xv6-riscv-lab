//! Network device abstraction layer -- Rust port of `kernel/dev/netdev.c`
//! (Phase 2 Wave 24; see `docs/rustify/phase2_plan.md`).
//!
//! A small registration/lookup table so `kernel/net.rs`'s TX path can
//! transmit packets without being coupled to a specific NIC driver.
//! Registrants (`kernel/e1000.rs`, `kernel/dev/x1_emac.rs`) are native
//! Rust drivers now -- there is no C left anywhere in this call graph --
//! that own their own `Netdev` as a `static` (never allocated/freed) and
//! pass `&their_ndev` by pointer -- this module never allocates, frees,
//! or copies a `netdev`, it only links caller-owned nodes into an
//! intrusive singly-linked list via the struct's own `next` field.
//! [`Netdev`] is a plain native `#[repr(C)]` struct (no bindgen, no
//! `wrapper.h` entry, no C mirror to stay ABI-compatible with -- every
//! field access below is a plain read/write of caller-owned data, same
//! reasoning as `kernel/dev/dev.rs`'s device-table pointers). The old
//! `struct netdev_ops` fn-pointer table is gone (fn-pointer-ops-table ->
//! trait dispatch campaign): [`NetdevOps`] is a trait now, dispatched
//! through `Netdev::ops: Option<&'static dyn NetdevOps>`.
//!
//! # Concurrency (preserved from the C original)
//!
//! The C `netdev.c` used **no locking whatsoever** -- `netdev_list`/
//! `netdev_count` are plain (non-atomic) globals. This is safe in
//! practice because every call site today runs during single-hart,
//! sequential boot-time device probing (`netdev_init()` from
//! `start_kernel.c`'s early init; `netdev_register()` from
//! `e1000_init()`/`x1_emac_probe()`, themselves called from that same
//! sequential probe sequence) -- there is no concurrent registration or
//! list mutation in this tree. `netdev_set_link()` is the one function
//! that could plausibly run later (from an interrupt-driven link-state
//! poll), but it only mutates the single caller-owned `netdev` passed
//! in, never the list itself. This port keeps the exact same
//! unsynchronized-global design (`static mut`, no atomics) rather than
//! introducing new locking the original never had -- widening the
//! concurrency contract is out of this wave's scope and no caller
//! exercises it concurrently today (documented, not just assumed: grepped
//! every call site above).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_char, c_int};
use core::ptr;

use crate::bindings::{mbuf, netdev, netdev_link_cb_t};
use crate::string::strncmp;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N4 (struct), fn-pointer-ops-table -> trait
// dispatch campaign (ops field).
//
// `Netdev` IS the kernel-wide Rust definition of `kernel/inc/dev/
// netdev.h`'s `struct netdev` now: `build.rs` blocklists the
// bindgen-generated form and injects `pub use crate::dev::netdev::Netdev
// as netdev;` (no `_t` typedef exists), so every
// `crate::bindings::netdev` path across the crate (this file, e1000.rs,
// x1_emac.rs, net.rs) resolves here. The `netdev_link_cb_t` fn-pointer
// typedef deliberately stays bindgen-emitted (only referenced by
// pointer/alias — N3's workqueue-cb precedent). The `priv_` field name
// keeps bindgen's keyword-escape rename of C's `priv` (e1000.rs
// constructs the struct with a field literal using exactly that
// spelling). `ops` was a raw `*mut netdev_ops` pointing at a C-layout
// fn-pointer table; it is now `Option<&'static dyn NetdevOps>` (a
// 16-byte fat pointer) since `struct netdev_ops` itself was retired in
// favor of the [`NetdevOps`] trait below.
// ---------------------------------------------------------------------------

/// `struct netdev` (`kernel/inc/dev/netdev.h`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Netdev {
    pub name: [c_char; 16],
    pub mac: [crate::bindings::uint8; 6],
    pub ip: crate::bindings::uint32,
    pub mtu: c_int,
    pub link_up: c_int,
    pub speed: c_int,
    pub full_duplex: c_int,
    pub index: c_int,
    pub ops: Option<&'static dyn NetdevOps>,
    pub priv_: *mut core::ffi::c_void,
    pub next: *mut Netdev,
    pub link_cb: netdev_link_cb_t,
}

// Layout facts -- values through `ops` (offset 48) are unchanged from
// the P3-N4 hardcoded layout proof (bindgen-era capture, independently
// confirmed by a riscv64-unknown-elf-gcc `_Static_assert` probe against
// `kernel/inc/dev/netdev.h`: netdev 80/8, offsets
// 0/16/24/28/32/36/40/44/48/56/64/72 -- see wave-P3-N4 history for that
// evidence). `ops` widened from an 8-byte `*mut netdev_ops` to a
// 16-byte `Option<&'static dyn NetdevOps>` fat pointer
// (fn-pointer-ops-table -> trait dispatch campaign), so every field
// after it shifts by +8: `priv_` 56->64, `next` 64->72, `link_cb`
// 72->80. Total size grows 80->88 (still 8-byte aligned, no leftover
// padding to absorb the shift the way `Pcache`'s tail padding did).
const _: () = {
    assert!(core::mem::size_of::<Netdev>() == 88, "netdev size");
    assert!(core::mem::align_of::<Netdev>() == 8, "netdev alignment");
    assert!(core::mem::offset_of!(Netdev, name) == 0, "netdev.name offset");
    assert!(core::mem::offset_of!(Netdev, mac) == 16, "netdev.mac offset");
    assert!(core::mem::offset_of!(Netdev, ip) == 24, "netdev.ip offset");
    assert!(core::mem::offset_of!(Netdev, mtu) == 28, "netdev.mtu offset");
    assert!(core::mem::offset_of!(Netdev, link_up) == 32, "netdev.link_up offset");
    assert!(core::mem::offset_of!(Netdev, speed) == 36, "netdev.speed offset");
    assert!(core::mem::offset_of!(Netdev, full_duplex) == 40, "netdev.full_duplex offset");
    assert!(core::mem::offset_of!(Netdev, index) == 44, "netdev.index offset");
    assert!(core::mem::offset_of!(Netdev, ops) == 48, "netdev.ops offset");
    assert!(core::mem::offset_of!(Netdev, priv_) == 64, "netdev.priv_ offset");
    assert!(core::mem::offset_of!(Netdev, next) == 72, "netdev.next offset");
    assert!(core::mem::offset_of!(Netdev, link_cb) == 80, "netdev.link_cb offset");
    // `Option<&'static dyn NetdevOps>` stays a plain 16-byte fat pointer
    // (`None` = the zeroed/unregistered state -- valid via the
    // null-data-pointer niche, same reasoning as `Cdev`'s
    // `Option<&'static dyn CdevOps>`, `kernel/dev/cdev.rs`).
    assert!(
        core::mem::size_of::<Option<&'static dyn NetdevOps>>() == 16,
        "netdev ops fat pointer size"
    );
    assert!(
        core::mem::align_of::<Option<&'static dyn NetdevOps>>() == 8,
        "netdev ops fat pointer alignment"
    );
};

// ===========================================================================
// `NetdevOps` -- trait-based replacement (fn-pointer-ops-table -> trait
// dispatch campaign) for the former C-style `struct netdev_ops`
// fn-pointer table. Follows the `CdevOps`/`PcacheOps` precedent exactly
// (`kernel/dev/cdev.rs`, `kernel/mm/pcache.rs`). `struct netdev_ops` no
// longer exists anywhere -- there is no C consumer left to keep it
// ABI-compatible with, and the bindgen-era layout proof that used to
// pin its 8-byte/offset-0 `transmit` slot is retired along with the
// type itself.
// ===========================================================================

/// The per-driver network-transmit operations vtable. Implementors are
/// zero-sized unit structs with a `static` instance (`E1000NetdevOps` in
/// `kernel/e1000.rs`, `X1EmacNetdevOps` in `kernel/dev/x1_emac.rs`)
/// installed into [`Netdev::ops`] as `Some(&STATIC)`.
///
/// Slot-nullability mapping (the old `Option<fn>` slot's `None`
/// dispatch behavior is preserved exactly): `transmit` is a REQUIRED
/// method. The old `Netdev::register` rejected registration unless
/// *both* the whole `ops` table was non-null *and* its `transmit` slot
/// was `Some` -- and every real implementor always supplied `transmit`,
/// so "table present but `transmit` `None`" never existed on a
/// registered device. That two-part check collapses to the single
/// `ops.is_none()` test seen in [`Netdev::register`] below, now that
/// `transmit`'s absence is unrepresentable on any `Some(ops)`.
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU (the TX path in `net.rs` may run
/// on any hart).
pub trait NetdevOps: Sync {
    /// Transmit one packet on `dev`; returns 0 on success.
    ///
    /// # Safety
    /// `dev` must be the live, registered [`Netdev`] this instance was
    /// installed on; `m` must be a caller-owned, live `mbuf`.
    unsafe fn transmit(&self, dev: *mut netdev, m: *mut mbuf) -> c_int;
}

// ---------------------------------------------------------------------------
// External C symbols.
// ---------------------------------------------------------------------------
unsafe extern "C" {
    // printf.rs -- variadic, cannot be marked `safe`.
}

/// `kernel/inc/dev/netdev.h`: `#define NETDEV_MAX 4`.
const NETDEV_MAX: c_int = 4;
/// `kernel/inc/dev/netdev.h`: `#define NETDEV_NAME_MAX 16`.
const NETDEV_NAME_MAX: usize = 16;

/// Head of the intrusive linked list (most-recently-registered first --
/// `netdev_register` prepends, matching the C original).
static mut NETDEV_LIST: *mut netdev = ptr::null_mut();
/// Count of registered devices so far (also doubles as the next
/// assigned `index`).
static mut NETDEV_COUNT: c_int = 0;

// ---------------------------------------------------------------------------
// KERNEL-OO: relocated onto `impl Netdev` -- behavior-preserving only:
// raw `*mut netdev` params kept (matches this file's module-doc
// "no locking" contract -- there's no receiver-refcount reasoning to
// disturb), bodies byte-identical, `unsafe extern "C"` signatures
// preserved exactly.
// ---------------------------------------------------------------------------

impl Netdev {
    /// `void netdev_init(void)`.
    // P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
    // crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn init() {
        // SAFETY: called once, from `start_kernel.c`'s single-hart early
        // init, before any registration can race it (see module doc).
        unsafe {
            NETDEV_LIST = ptr::null_mut();
            NETDEV_COUNT = 0;
        }
    }

    /// `int netdev_register(struct netdev *dev)`. Registers a network
    /// device; the first registered device becomes the default transmit
    /// interface (via [`Netdev::get_default`]'s tail-walk).
    // P3-1D mesh sweep: callers (`e1000.rs`, `net.rs`, `dev/x1_emac.rs`) now
    // import this via crate-path `use` instead of an `extern` redeclaration --
    // demoted.
    pub(crate) extern "C" fn register(dev: *mut netdev) -> c_int {
        if dev.is_null() {
            return -1;
        }
        // SAFETY: `dev` is caller-provided and, per this crate's C-ABI
        // convention, valid for the duration of the call; reading `.ops` is
        // a plain field read.
        let ops = unsafe { (*dev).ops };
        // `transmit` is a required `NetdevOps` trait method now (no
        // default body) -- any `Some(ops)` vtable already supplies it,
        // so the old separate "table present" + "transmit slot present"
        // checks collapse into this single `is_none()` test (see
        // `NetdevOps`'s doc comment).
        if ops.is_none() {
            return -1;
        }

        // SAFETY: single-threaded boot-time registration (see module doc).
        unsafe {
            if NETDEV_COUNT >= NETDEV_MAX {
                return -1;
            }

            (*dev).index = NETDEV_COUNT;
            NETDEV_COUNT += 1;
            (*dev).next = NETDEV_LIST;
            NETDEV_LIST = dev;

            crate::kprintln!(
                "netdev: registered {} (MAC {:x}:{:x}:{:x}:{:x}:{:x}:{:x}) idx {}",
                crate::printf::Cs((*dev).name.as_ptr()),
                ((*dev).mac[0] as c_int as i64) as u64,
                ((*dev).mac[1] as c_int as i64) as u64,
                ((*dev).mac[2] as c_int as i64) as u64,
                ((*dev).mac[3] as c_int as i64) as u64,
                ((*dev).mac[4] as c_int as i64) as u64,
                ((*dev).mac[5] as c_int as i64) as u64,
                (*dev).index,
            );
        }
        0
    }

    /// `struct netdev *netdev_get_default(void)` -- returns the
    /// first-registered device (walks to the tail of the prepend-ordered
    /// list, matching the C original exactly).
    // P3-1D mesh sweep: caller (`net.rs`) now imports this via crate-path
    // `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn get_default() -> *mut netdev {
        // SAFETY: single-threaded boot-time list access (see module doc);
        // the list is a plain NULL-terminated singly-linked chain of
        // caller-owned, never-freed nodes.
        unsafe {
            let mut d = NETDEV_LIST;
            let mut last: *mut netdev = ptr::null_mut();
            while !d.is_null() {
                last = d;
                d = (*d).next;
            }
            last
        }
    }

    /// `struct netdev *netdev_get_by_index(int index)`.
    // P3-1D mesh sweep: no live caller anywhere in the tree today (full-tree
    // grep, matches the pre-existing RUST_FORCE_UNDEFINED comment) --
    // demoted; `#[allow(dead_code)]` documents the gap.
    #[allow(dead_code)]
    pub(crate) extern "C" fn get_by_index(index: c_int) -> *mut netdev {
        // SAFETY: see `Netdev::get_default`.
        unsafe {
            let mut d = NETDEV_LIST;
            while !d.is_null() {
                if (*d).index == index {
                    return d;
                }
                d = (*d).next;
            }
        }
        ptr::null_mut()
    }

    /// `struct netdev *netdev_get_by_name(const char *name)`.
    // P3-1D mesh sweep: no live caller anywhere in the tree today -- demoted;
    // `#[allow(dead_code)]` documents the gap, same precedent as
    // `Netdev::get_by_index` above.
    #[allow(dead_code)]
    pub(crate) extern "C" fn get_by_name(name: *const c_char) -> *mut netdev {
        if name.is_null() {
            return ptr::null_mut();
        }
        // SAFETY: see `Netdev::get_default`; `strncmp` is given `name`
        // (caller-provided, assumed NUL-terminated or at least
        // `NETDEV_NAME_MAX`-readable, matching the C original's contract)
        // and `(*d).name` (a live, embedded, fixed-size array field of a
        // registered node).
        unsafe {
            let mut d = NETDEV_LIST;
            while !d.is_null() {
                if strncmp((*d).name.as_ptr(), name, NETDEV_NAME_MAX) == 0 {
                    return d;
                }
                d = (*d).next;
            }
        }
        ptr::null_mut()
    }

    /// `void netdev_set_link(struct netdev *dev, int link_up)` -- updates
    /// link state and notifies the registered link-change callback (if any)
    /// on a genuine transition.
    // P3-1D mesh sweep: caller (`dev/x1_emac.rs`) now imports this via
    // crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn set_link(dev: *mut netdev, link_up: c_int) {
        if dev.is_null() {
            return;
        }
        // SAFETY: `dev` is caller-provided and valid for the duration of
        // this call (same contract as every other function in this file).
        unsafe {
            let old = (*dev).link_up;
            (*dev).link_up = link_up;
            if old != link_up {
                if let Some(cb) = (*dev).link_cb {
                    cb(dev, link_up);
                }
            }
        }
    }

    /// `void netdev_set_link_callback(struct netdev *dev, netdev_link_cb_t
    /// cb)`.
    // P3-1D mesh sweep: no live caller anywhere in the tree today -- demoted;
    // `#[allow(dead_code)]` documents the gap, same precedent as
    // `Netdev::get_by_index` above.
    #[allow(dead_code)]
    pub(crate) extern "C" fn set_link_callback(dev: *mut netdev, cb: netdev_link_cb_t) {
        if dev.is_null() {
            return;
        }
        // SAFETY: see `Netdev::set_link`.
        unsafe {
            (*dev).link_cb = cb;
        }
    }
}
