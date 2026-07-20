//! Rust port of `kernel/proc/sched_idle.c`.
//!
//! Exports the C ABI symbol [`init_idle_rq`]. The corresponding C file
//! has been removed from `kernel/proc/CMakeLists.txt`.

#![allow(non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::c_int;
use core::ptr::null_mut;

use super::cffi::{
    self,
    FifoSubqueue, IdleRq, Rq, SchedClass, SchedEntity,
    IDLE_MAJOR_PRIORITY, NCPU,
    PRIORITY_MAINLEVEL_SHIFT, PRIORITY_SUBLEVEL_MASK,
};

// ---------------------------------------------------------------------------
// Per-CPU idle run-queue array. `kmm_alloc`'d once at boot, then read-only.
// ---------------------------------------------------------------------------
#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn new(v: T) -> Self { SyncCell(UnsafeCell::new(v)) }
    #[inline] fn get(&self) -> *mut T { self.0.get() }
}

static IDLE_RQS: SyncCell<*mut IdleRq> = SyncCell::new(null_mut());

// ---------------------------------------------------------------------------
// sched_class callbacks
// ---------------------------------------------------------------------------
// P3-N-METH: dispatched only from `SchedClass`'s methods in `sched.rs`
// (a `proc` sibling) via the explicit `super::sched_idle::` path — never
// via the `pub use sched_idle::*` glob — so `pub(crate)` -> `pub(super)`
// (smallest scope: `crate::proc`). Left raw (the SchedClass trait/policy
// layer, per 46320f2): raw `*mut Rq`/`*mut SchedEntity` from the dispatch
// site, freeze-hazard on Rq/SchedEntity fields — the same floor as the
// fifo callbacks.
pub(super) unsafe fn idle_pick_next_task(rq: *mut Rq) -> *mut SchedEntity {
    // `rq` is the first field of `IdleRq` → container_of is a no-op cast.
    let idle = rq as *mut IdleRq;
    let thread = (*idle).idle_thread;
    // `struct thread::sched_entity` is the first field of struct thread, so
    // again container_of is a no-op cast.  Mirror the C code structure:
    //     return idle_rq->idle_thread->sched_entity;
    // The C code returns `thread->sched_entity` which is itself a
    // `struct sched_entity *` field stored inside `struct thread`. We
    // call out via a tiny helper exported from the still-C proc code
    // to avoid duplicating the full thread layout here.
    thread_sched_entity(thread)
}

pub(super) unsafe fn idle_enqueue_task(rq: *mut Rq, se: *mut SchedEntity) {
    let idle = rq as *mut IdleRq;
    if !(*idle).idle_thread.is_null() {
        cffi::panic_proc(b"idle_enqueue_task: idle rq already has a thread\0");
    }
    (*idle).idle_thread = (*se).thread;
    (*se).rq = rq;
    (*se).priority =
        (IDLE_MAJOR_PRIORITY << PRIORITY_MAINLEVEL_SHIFT as i32)
        | PRIORITY_SUBLEVEL_MASK as i32;
}

pub(super) unsafe fn idle_dequeue_task(_rq: *mut Rq, _se: *mut SchedEntity) {
    cffi::panic_proc(b"idle_dequeue_task: trying to dequeue task from idle rq\0");
}

// P3-10e: the `IDLE_SCHED_CLASS` static fn-pointer table is gone — the
// idle policy is now the `SchedClass::Idle` enum variant, and the three
// implemented callbacks above are dispatched from `SchedClass`'s methods
// (`sched.rs`). The formerly-`None` slots need no representation.

// ---------------------------------------------------------------------------
// Helper exported from `proc/proc_shims.rs`: returns `thread->sched_entity`.
// P3-1B: only caller anywhere in the tree is this file -- demoted from
// `#[no_mangle]`. P3-N7: the pointer-cast shims are gone -- `cffi::
// Thread`/`SchedEntity` ARE `crate::bindings::{thread, sched_entity}`
// now (the mirrors were promoted to the native definitions), so this is
// a plain re-export.
// ---------------------------------------------------------------------------
use crate::proc::proc_shims::thread_sched_entity;

// ---------------------------------------------------------------------------
// Boot path
// ---------------------------------------------------------------------------
unsafe fn alloc_idle_rqs() {
    let size = core::mem::size_of::<IdleRq>() * NCPU;
    let p = cffi::kmm_alloc_zeroed(size) as *mut IdleRq;
    if p.is_null() {
        cffi::panic_proc(b"alloc_idle_rqs: failed to allocate idle_rqs\0");
    }
    *IDLE_RQS.get() = p;

    for i in 0..NCPU {
        let rq_ptr = &raw mut (*p.add(i)).rq;
        cffi::rq_init(rq_ptr);
        cffi::rq_register(rq_ptr, IDLE_MAJOR_PRIORITY, i as c_int);
        cffi::rq_set_ready(IDLE_MAJOR_PRIORITY, i as c_int);
    }
}

// P3-1B: only caller is `proc/rq.rs` (already a direct crate-path `use`,
// not an `extern` redeclaration) -- demoted.
// P3-N-METH: sole caller `proc/rq.rs` is a `proc` descendant reaching this
// via `super::sched_idle::init_idle_rq` (call site updated off the
// `pub use sched_idle::*` glob) -> `pub(crate)` -> `pub(super)`.
pub(super) unsafe extern "C" fn init_idle_rq() {
    cffi::sched_class_register(IDLE_MAJOR_PRIORITY, SchedClass::Idle);
    alloc_idle_rqs();
}

// Keep import live in case of future changes.
const _: Option<FifoSubqueue> = None;
