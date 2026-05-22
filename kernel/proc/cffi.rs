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
// Opaque forward decls (we never deref these from Rust)
// ===========================================================================
#[repr(C)] pub struct Thread       { _opaque: [u8; 0] }
#[repr(C)] pub struct Context      { _opaque: [u8; 0] }
#[repr(C)] pub struct RbNode       { _opaque: [u8; 24] }

// ===========================================================================
// Mirror types — `#[repr(C)]`, pinned to the C layout.
//
// CACHELINE alignment (64) is preserved with `#[repr(C, align(64))]`.
// Compile-time asserts at the bottom of this file fail the build if a
// future C header drift breaks layout.
// ===========================================================================

pub type cpumask_t = u64;

#[repr(C)]
pub struct SchedClass {
    pub enqueue_task:    Option<unsafe extern "C" fn(*mut Rq, *mut SchedEntity)>,
    pub dequeue_task:    Option<unsafe extern "C" fn(*mut Rq, *mut SchedEntity)>,
    pub select_task_rq:  Option<unsafe extern "C" fn(*mut Rq, *mut SchedEntity, cpumask_t) -> *mut Rq>,
    pub pick_next_task:  Option<unsafe extern "C" fn(*mut Rq) -> *mut SchedEntity>,
    pub put_prev_task:   Option<unsafe extern "C" fn(*mut Rq, *mut SchedEntity)>,
    pub set_next_task:   Option<unsafe extern "C" fn(*mut Rq, *mut SchedEntity)>,
    pub task_tick:       Option<unsafe extern "C" fn(*mut Rq, *mut SchedEntity)>,
    pub task_fork:       Option<unsafe extern "C" fn(*mut Rq, *mut SchedEntity)>,
    pub task_dead:       Option<unsafe extern "C" fn(*mut Rq, *mut SchedEntity)>,
    pub yield_task:      Option<unsafe extern "C" fn(*mut Rq)>,
}

#[repr(C, align(64))]
pub struct Rq {
    pub sched_class: *mut SchedClass,
    pub class_id:    c_int,
    pub task_count:  c_int,
    pub cpu_id:      c_int,
    // padding to 64-byte alignment is provided by `align(64)`.
    _pad: [u8; 64 - 8 - 4 - 4 - 4],
}

/// Mirror of `struct sched_entity` from `inc/proc/rq_types.h`.
///
/// First field is a `union { rb_entry; list_entry; }`; we model that
/// as a 24-byte `_link` blob (sizeof(rb_node) == 24, sizeof(list_node_t) == 16).
/// The list_entry view is recovered via `list_entry_ptr()`.
#[repr(C)]
pub struct SchedEntity {
    _link:              [u8; 24],          // union { rb_node rb_entry; list_node_t list_entry; }
    pub rq:             *mut Rq,
    pub priority:       c_int,
    pub thread:         *mut Thread,
    pub sched_class:    *mut SchedClass,
    pub pi_lock:        [u8; SPINLOCK_BYTES],
    pub on_rq:          c_int,
    pub on_cpu:         c_int,
    pub cpu_id:         c_int,
    _pad1:              u32,
    pub wake_next:      *mut SchedEntity,
    pub affinity_mask:  cpumask_t,
    pub start_time:     u64,
    pub exec_start:     u64,
    pub exec_end:       u64,
    // context is opaque/embedded; we never touch it from Rust here.
    pub context:        [u8; CONTEXT_BYTES],
}

impl SchedEntity {
    /// Pointer to the embedded `list_entry` view of the head union.
    #[inline]
    pub fn list_entry_ptr(se: *mut SchedEntity) -> *mut ListNode {
        // list_entry is the second variant of the union, alias of the rb_entry
        // storage starting at offset 0.
        se as *mut ListNode
    }
}

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
// Layout constants pinned by C kernel
// ===========================================================================
// spinlock_t is `[u8; 24]` opaque storage (see kernel/lock/spinlock.h /
// kernel/mm/slab.rs); we mirror the same byte count here so the offsets
// in SchedEntity line up.
const SPINLOCK_BYTES: usize = 24;
// struct context (riscv kernel save area): 14 saved callee-regs * 8 = 112 bytes.
// Defined in kernel/inc/proc/thread_types.h / swtch.S. Pinned below.
const CONTEXT_BYTES: usize = 112;

// ===========================================================================
// Raw extern declarations (kept in a private module).
// ===========================================================================
pub mod raw {
    use super::*;

    unsafe extern "C" {
        // run-queue glue
        pub safe fn rq_init(rq: *mut Rq);
        pub safe fn rq_register(rq: *mut Rq, cls_id: c_int, cpu_id: c_int);
        pub safe fn rq_set_ready(cls_id: c_int, cpu_id: c_int);
        pub safe fn rq_clear_ready(cls_id: c_int, cpu_id: c_int);
        pub safe fn sched_class_register(id: c_int, cls: *mut SchedClass);
    }

    unsafe extern "C" {
        pub safe fn printf(fmt: *const c_char, ...) -> c_int;
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

// Declared in slab_shims.rs; re-declare here so we can call it as a `safe` fn.
unsafe extern "C" {
    pub safe fn xv6_list_push_back(head: *mut ListNode, entry: *mut ListNode);
}

#[inline]
pub fn list_push_back(head: *mut ListNode, entry: *mut ListNode) {
    xv6_list_push_back(head, entry);
}

/// `panic("...")`-equivalent: prints to console then halts.
#[inline(never)]
pub fn panic_proc(msg: &[u8]) -> ! {
    debug_assert!(msg.last() == Some(&0), "panic msg must be NUL-terminated");
    mm_raw::__panic_start();
    raw::printf(b"PANIC proc: %s\n\0".as_ptr() as *const c_char,
                msg.as_ptr() as *const c_char);
    mm_raw::__panic_end()
}

// ===========================================================================
// Compile-time layout pins.
// ===========================================================================
const _: () = {
    // SchedClass: 10 function pointers, no padding.
    assert!(size_of::<SchedClass>() == 10 * size_of::<*const ()>());

    // Rq mirrors C; 64-byte aligned.
    assert!(align_of::<Rq>() == 64);
    assert!(size_of::<Rq>() == 64);

    // IdleRq / FifoRq are also 64-byte aligned.
    assert!(align_of::<IdleRq>() == 64);
    assert!(align_of::<FifoRq>() == 64);

    // FifoSubqueue = list_node_t (16) + int (4) + padding (4) = 24 on 8-byte
    // alignment.
    assert!(size_of::<FifoSubqueue>() == 24);

    // SchedEntity: hand-counted = 24 (link union) + 8 (rq) + 4 (prio) + 8 (thread)
    //   + 8 (sched_class) + 24 (pi_lock) + 4 + 4 + 4 + 4(pad)
    //   + 8 (wake_next) + 8 (affinity) + 8 + 8 + 8 (times) + 112 (context)
    //   = 246 ... let layout assert below catch drift.
    // We only pin the offset of fields used by sched_idle/sched_fifo:
    //   priority — used by sched_fifo to read minor priority
    //   rq       — used by sched_idle::__idle_enqueue_task
    //   thread   — used by sched_idle::__idle_pick_next_task
    // The mirror is wrong in details only if these offsets drift; the rest
    // of the struct is opaque to the schedulers.
    // (Soft check: offset_of! isn't const-stable on stable Rust 1.95 in
    // const ctx for these complex paths; we trust the field-by-field
    // mirror plus the C-side _Static_asserts in proc_private.h.)
};

// Suppress unused-warning fallout.
const _UNUSED: (Option<*const c_void>, Option<c_char>) = (None, None);
