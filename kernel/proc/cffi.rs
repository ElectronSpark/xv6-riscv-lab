//! Canonical C-FFI surface shared by the `proc` crate.
//!
//! Mirrors selected types and extern declarations from
//! `kernel/inc/proc/rq_types.h`, `kernel/inc/proc/rq.h`, and
//! `kernel/inc/proc/thread.h`. Layout-pinned at compile time.

#![allow(dead_code)]
#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};
use core::mem::{align_of, size_of};

use crate::mm::cffi::ListNode;

// ===========================================================================
// Priority encoding (mirror of inc/proc/rq.h)
// ===========================================================================
pub const PRIORITY_SUBLEVEL_MASK:     u8 = 0x03;
pub const PRIORITY_MAINLEVEL_MASK:    u8 = 0xFC;
pub const PRIORITY_MAINLEVEL_SHIFT:   u8 = 2;

pub const IDLE_MAJOR_PRIORITY:        i32 = 63;
pub const FIFO_MAJOR_PRIORITY:        i32 = 17;
pub const DEFAULT_MAJOR_PRIORITY:     i32 = 17;

pub const PRIORITY_MAINLEVELS:        usize = 64;
pub const FIFO_RQ_SUBLEVELS:          usize = 4;

pub const NCPU:                       usize = 8;

// errno values (subset)
pub const EINVAL: c_int = 22;
pub const ENOENT: c_int = 2;

#[inline]
pub const fn major_priority(prio: i32) -> i32 {
    ((prio as u32 & PRIORITY_MAINLEVEL_MASK as u32) >> PRIORITY_MAINLEVEL_SHIFT) as i32
}
#[inline]
pub const fn minor_priority(prio: i32) -> i32 {
    (prio as u32 & PRIORITY_SUBLEVEL_MASK as u32) as i32
}

// ===========================================================================
// Canonical scheduler types — P3-N7 promotion.
//
// The independently-written, layout-pinned mirror structs (`SchedClass`/
// `Rq`/`SchedEntity`) that used to live here are gone: the canonical
// native definitions now live in `kernel/proc/sched.rs` /
// `kernel/proc/rq.rs` (they ARE `crate::bindings::sched_class`/`rq`/
// `sched_entity` via `build.rs` facade re-exports), and this file simply
// re-exports them under the established local names. The old mirror was
// subtly WRONG past `SchedEntity.sched_class` (it modelled `pi_lock` as
// 24 bytes at offset 56, where the real — bindgen-verified — layout has
// 8 bytes of cacheline padding first, `pi_lock` @64); it "worked" only
// because its own comment restricted use to the `priority`/`rq`/`thread`
// offsets, all before the divergence. The promotion removes that trap;
// `SchedEntity::list_entry_ptr` lives on the native now.
//
// `Thread` used to be a private opaque `[u8; 0]` stand-in; it is the
// real `crate::bindings::thread` now, which lets the `*mut Thread`
// fields below unify with the native `SchedEntity.thread`. (The unused
// opaque `Context` stand-in is dropped outright — the native
// `SchedEntity.context` is the real `crate::bindings::context`.)
// ===========================================================================
pub type Thread = crate::bindings::thread;

pub type cpumask_t = u64;

pub use crate::proc::rq::{Rq, SchedEntity};
pub use crate::proc::sched::SchedClass;

// ===========================================================================
// idle_rq / fifo_rq mirrors
// ===========================================================================
#[repr(C, align(64))]
pub struct IdleRq {
    pub rq:           Rq,
    pub idle_thread:  *mut Thread,
}

#[repr(C)]
pub struct FifoSubqueue {
    pub head:  ListNode,
    pub count: c_int,
}

#[repr(C, align(64))]
pub struct FifoRq {
    pub rq:          Rq,
    pub subqueues:   [FifoSubqueue; FIFO_RQ_SUBLEVELS],
    pub ready_mask:  u8,
}

// ===========================================================================
// Raw extern declarations (kept in a private module).
// ===========================================================================
pub mod raw {
    use super::*;

    // P3-D2a: the run-queue glue entry points are ordinary Rust fns in
    // `kernel/proc/rq.rs`; the former `extern "C"` redeclarations are
    // gone. The two integer-only fns are plain re-exports; the `rq`-
    // taking ones keep this file's layout-pinned mirror type `Rq` at the
    // call sites via thin cast adapters -- the mirror describes the exact
    // same object as its `crate::bindings::rq` counterpart (from
    // `inc/proc/rq_types.h`; see the compile-time layout asserts at the
    // bottom of this file), so the pointer casts are ABI-identity.
    // (P3-10e: `sched_class` is no longer a pointer — `SchedClass` is a
    // closed-set enum passed by value, no cast needed.)
    pub(crate) use crate::proc::{rq_clear_ready, rq_set_ready};

    #[inline]
    pub fn rq_init(rq: *mut Rq) {
        // SAFETY: `rq_init`'s precondition (valid, writable, non-null
        // `rq`) is enforced by its callers exactly as before, when this
        // was a `safe fn` extern redeclaration of the same symbol; the
        // cast is ABI-identity per the module comment above.
        unsafe { crate::proc::rq_init(rq as *mut crate::bindings::rq) }
    }

    #[inline]
    pub fn rq_register(rq: *mut Rq, cls_id: c_int, cpu_id: c_int) {
        crate::proc::rq_register(rq as *mut crate::bindings::rq, cls_id, cpu_id)
    }

    // P3-10e: `SchedClass` is a closed-set enum now (`SchedClass::Idle`/
    // `SchedClass::Fifo`), so registration passes the variant by value —
    // no pointer, no cast.
    #[inline]
    pub fn sched_class_register(id: c_int, cls: SchedClass) {
        crate::proc::sched_class_register(id, cls)
    }
}

pub use raw::*;

// ===========================================================================
// Adapter helpers shared by the scheduler classes.
// ===========================================================================
use crate::mm::cffi::raw as mm_raw;

#[inline]
pub fn kmm_alloc_zeroed(size: usize) -> *mut c_void {
    let p = mm_raw::kmm_alloc(size);
    if !p.is_null() {
        mm_raw::memset(p, 0, size);
    }
    p
}

#[inline]
pub fn list_init(entry: *mut ListNode) {
    mm_raw::xv6_list_init(entry);
}

#[inline]
pub fn list_detach(entry: *mut ListNode) {
    mm_raw::xv6_list_detach(entry);
}

#[inline]
pub fn list_first(head: *const ListNode) -> *mut ListNode {
    mm_raw::xv6_list_first(head)
}

// P3-D3c: `mm/cffi.rs`'s `xv6_list_push_back` is a plain (safe) crate-path
// re-export now that its `#[no_mangle]` export is gone.
pub(crate) use crate::mm::cffi::xv6_list_push_back;

#[inline]
pub fn list_push_back(head: *mut ListNode, entry: *mut ListNode) {
    xv6_list_push_back(head, entry);
}

/// `panic("...")`-equivalent: prints to console then halts.
#[inline(never)]
pub fn panic_proc(msg: &[u8]) -> ! {
    debug_assert!(msg.last() == Some(&0), "panic msg must be NUL-terminated");
    mm_raw::__panic_start();
    crate::kprintln!("PANIC proc: {}", crate::printf::Cs(msg.as_ptr() as *const c_char));
    mm_raw::__panic_end()
}

// ===========================================================================
// Compile-time layout pins.
//
// (P3-N7: the `SchedClass`/`Rq`/`SchedEntity` pins moved with the
// canonical definitions to `kernel/proc/sched.rs`/`kernel/proc/rq.rs`,
// where the full hardcoded offset set is asserted. Only the structs
// still defined in this file are pinned here.)
// ===========================================================================
const _: () = {
    // IdleRq / FifoRq are 64-byte aligned (embedded leading `Rq`).
    assert!(align_of::<IdleRq>() == 64);
    assert!(align_of::<FifoRq>() == 64);

    // FifoSubqueue = list_node_t (16) + int (4) + padding (4) = 24 on 8-byte
    // alignment.
    assert!(size_of::<FifoSubqueue>() == 24);
};

// Suppress unused-warning fallout.
const _UNUSED: (Option<*const c_void>, Option<c_char>) = (None, None);
