//! Network device abstraction layer -- Rust port of `kernel/dev/netdev.c`
//! (Phase 2 Wave 24; see `docs/rustify/phase2_plan.md`).
//!
//! A small registration/lookup table so `kernel/net.c` (still C) can
//! transmit packets without being coupled to a specific NIC driver.
//! Registrants are still-C drivers (`kernel/e1000.c`,
//! `kernel/dev/x1_emac.c`) that own their own `struct netdev` as a
//! `static` (never allocated/freed) and pass `&their_ndev` by pointer --
//! this module never allocates, frees, or copies a `netdev`, it only
//! links caller-owned nodes into an intrusive singly-linked list via the
//! struct's own `next` field. `struct netdev`/`struct netdev_ops` keep
//! their exact C layout (bindgen, `kernel/inc/dev/netdev.h` added to
//! `wrapper.h`/`build.rs`'s allowlist this wave) -- every field access
//! below is a plain, ABI-identical read/write of caller-owned data, same
//! reasoning as `kernel/dev/dev.rs`'s device-table pointers.
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

use crate::bindings::{netdev, netdev_link_cb_t};
use crate::string::strncmp;

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

/// `void netdev_init(void)`.
// P3-1D mesh sweep: caller (`start_kernel.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn netdev_init() {
    // SAFETY: called once, from `start_kernel.c`'s single-hart early
    // init, before any registration can race it (see module doc).
    unsafe {
        NETDEV_LIST = ptr::null_mut();
        NETDEV_COUNT = 0;
    }
}

/// `int netdev_register(struct netdev *dev)`. Registers a network
/// device; the first registered device becomes the default transmit
/// interface (via [`netdev_get_default`]'s tail-walk).
// P3-1D mesh sweep: callers (`e1000.rs`, `net.rs`, `dev/x1_emac.rs`) now
// import this via crate-path `use` instead of an `extern` redeclaration --
// demoted.
pub(crate) extern "C" fn netdev_register(dev: *mut netdev) -> c_int {
    if dev.is_null() {
        return -1;
    }
    // SAFETY: `dev` is caller-provided and, per this crate's C-ABI
    // convention, valid for the duration of the call; reading `.ops` is
    // a plain field read.
    let ops = unsafe { (*dev).ops };
    if ops.is_null() {
        return -1;
    }
    // SAFETY: `ops` just checked non-null above.
    if unsafe { (*ops).transmit }.is_none() {
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
pub(crate) extern "C" fn netdev_get_default() -> *mut netdev {
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
pub(crate) extern "C" fn netdev_get_by_index(index: c_int) -> *mut netdev {
    // SAFETY: see `netdev_get_default`.
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
// `netdev_get_by_index` above.
#[allow(dead_code)]
pub(crate) extern "C" fn netdev_get_by_name(name: *const c_char) -> *mut netdev {
    if name.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: see `netdev_get_default`; `strncmp` is given `name`
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
pub(crate) extern "C" fn netdev_set_link(dev: *mut netdev, link_up: c_int) {
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
// `netdev_get_by_index` above.
#[allow(dead_code)]
pub(crate) extern "C" fn netdev_set_link_callback(dev: *mut netdev, cb: netdev_link_cb_t) {
    if dev.is_null() {
        return;
    }
    // SAFETY: see `netdev_set_link`.
    unsafe {
        (*dev).link_cb = cb;
    }
}
