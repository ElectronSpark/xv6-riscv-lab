//! Rust port of `kernel/proc/sched_fifo.c`.
//!
//! Exports the C ABI symbols [`init_fifo_rq`] and [`init_fifo_rq_range`].

#![allow(non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::c_int;
use core::ptr::null_mut;

use super::cffi::{
    self,
    cpumask_t, FifoRq, FifoSubqueue, Rq, SchedClass, SchedEntity,
    EINVAL, ENOENT, FIFO_RQ_SUBLEVELS, IDLE_MAJOR_PRIORITY, NCPU,
    PRIORITY_MAINLEVELS,
    major_priority, minor_priority,
};
use crate::machine;

const INT_MAX: c_int = 0x7FFF_FFFF;

// ---------------------------------------------------------------------------
// Per-cls-id, per-CPU FIFO run-queue table.
// __fifo_rqs[cls] is a heap-allocated array of NCPU `FifoRq`s,
// or null if the cls_id has not been registered.
// ---------------------------------------------------------------------------
#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn new(v: T) -> Self { SyncCell(UnsafeCell::new(v)) }
    #[inline] fn get(&self) -> *mut T { self.0.get() }
}

static FIFO_RQS: SyncCell<[*mut FifoRq; PRIORITY_MAINLEVELS]> =
    SyncCell::new([null_mut(); PRIORITY_MAINLEVELS]);

// Safe (bounds-checked) indexing: `cls` is always `< PRIORITY_MAINLEVELS`
// by construction (`major_priority()` masks with `PRIORITY_MAINLEVEL_MASK`,
// sized to match this array), and this path is not hot enough for the
// bounds check to matter — prefer a panic over `get_unchecked`'s UB if
// that invariant is ever violated.
#[inline]
fn fifo_rqs_get(cls: usize) -> *mut FifoRq {
    unsafe { (*FIFO_RQS.get())[cls] }
}
#[inline]
fn fifo_rqs_set(cls: usize, p: *mut FifoRq) {
    unsafe { (*FIFO_RQS.get())[cls] = p };
}

// ---------------------------------------------------------------------------
// Subqueue helpers
// ---------------------------------------------------------------------------
#[inline]
fn fifo_minor_prio(se: *mut SchedEntity) -> c_int {
    unsafe { minor_priority((*se).priority) }
}

#[inline]
fn subqueue(fifo_rq: *mut FifoRq, minor_prio: c_int) -> *mut FifoSubqueue {
    unsafe { &raw mut (*fifo_rq).subqueues[minor_prio as usize] }
}

// ---------------------------------------------------------------------------
// sched_class callbacks
// ---------------------------------------------------------------------------
extern "C" fn fifo_pick_next_task(rq: *mut Rq) -> *mut SchedEntity {
    let fifo_rq = rq as *mut FifoRq;
    let mask = unsafe { (*fifo_rq).ready_mask };
    if mask == 0 {
        return null_mut();
    }
    // Equivalent to `bits_ctz8(mask)` in C (a `__builtin_ctz`-backed macro).
    let idx = mask.trailing_zeros() as c_int;
    let sq = subqueue(fifo_rq, idx);
    // `list_entry` is the first member of SchedEntity's leading union, so
    // a `list_node_t*` taken from `sq->head` casts directly back to a
    // `*mut SchedEntity` (LIST_FIRST_NODE expanded).
    unsafe { cffi::list_first(&raw const (*sq).head) as *mut SchedEntity }
}

extern "C" fn fifo_enqueue_task(rq: *mut Rq, se: *mut SchedEntity) {
    let fifo_rq = rq as *mut FifoRq;
    let idx = fifo_minor_prio(se);
    let sq = subqueue(fifo_rq, idx);
    unsafe { cffi::list_push_back(&raw mut (*sq).head, SchedEntity::list_entry_ptr(se)); (*sq).count += 1; (*fifo_rq).ready_mask |= 1u8 << idx; }
}

extern "C" fn fifo_dequeue_task(rq: *mut Rq, se: *mut SchedEntity) {
    let fifo_rq = rq as *mut FifoRq;
    let idx = fifo_minor_prio(se);
    let sq = subqueue(fifo_rq, idx);
    unsafe { cffi::list_detach(SchedEntity::list_entry_ptr(se)); (*sq).count -= 1; if (*sq).count == 0 { (*fifo_rq).ready_mask &= !(1u8 << idx); } }
}

extern "C" fn fifo_put_prev_task(rq: *mut Rq, se: *mut SchedEntity) {
    let fifo_rq = rq as *mut FifoRq;
    let idx = fifo_minor_prio(se);
    let sq = subqueue(fifo_rq, idx);
    unsafe { cffi::list_push_back(&raw mut (*sq).head, SchedEntity::list_entry_ptr(se)); (*fifo_rq).ready_mask |= 1u8 << idx; cffi::rq_set_ready((*rq).class_id, (*rq).cpu_id); }
}

extern "C" fn fifo_set_next_task(rq: *mut Rq, se: *mut SchedEntity) {
    let fifo_rq = rq as *mut FifoRq;
    let idx = fifo_minor_prio(se);
    let sq = subqueue(fifo_rq, idx);
    unsafe { cffi::list_detach(SchedEntity::list_entry_ptr(se)); if (*sq).count == 1 { (*fifo_rq).ready_mask &= !(1u8 << idx); } if (*rq).task_count == 1 { cffi::rq_clear_ready((*rq).class_id, (*rq).cpu_id); } }
}

extern "C" fn fifo_select_task_rq(
    _prev_rq: *mut Rq,
    se: *mut SchedEntity,
    mut cpumask: cpumask_t,
) -> *mut Rq {
    let major_prio = unsafe { major_priority((*se).priority) };
    let minor_prio = unsafe { minor_priority((*se).priority) };

    if cpumask == 0 {
        cpumask = (1u64 << NCPU) - 1;
    }

    let bank = fifo_rqs_get(major_prio as usize);
    if bank.is_null() {
        // ERR_PTR(-EINVAL)
        return (-(EINVAL as isize)) as *mut Rq;
    }

    let cur_cpu = machine::cpuid();
    let mut best_rq: *mut Rq = null_mut();
    let mut best_count: c_int = INT_MAX;

    // Helper closure to obtain rq and count for a CPU if it is in cpumask
    let mut get_rq_count = |cpu: usize| -> Option<(*mut Rq, c_int)> {
        if (cpumask & (1u64 << cpu)) != 0 {
            let fifo_rq = unsafe { bank.add(cpu) };
            let sq = subqueue(fifo_rq, minor_prio);
            unsafe { Some((&raw mut (*fifo_rq).rq, (*sq).count)) }
        } else {
            None
        }
    };

    // Prefer current CPU for locality.
    if let Some((rq, count)) = get_rq_count(cur_cpu as usize) {
        best_rq = rq;
        best_count = count;
        if best_count == 0 {
            return best_rq;
        }
    }

    // Scan the remaining allowed CPUs.
    for cpu in (0..NCPU).filter(|&cpu| cpu != cur_cpu as usize) {
        if let Some((rq, count)) = get_rq_count(cpu) {
            if count < best_count {
                best_rq = rq;
                best_count = count;
                if best_count == 0 {
                    return best_rq;
                }
            }
        }
    }

    if best_rq.is_null() {
        return (-(ENOENT as isize)) as *mut Rq;
    }
    best_rq
}

static FIFO_SCHED_CLASS: SyncCell<SchedClass> = SyncCell::new(SchedClass {
    enqueue_task:   Some(fifo_enqueue_task),
    dequeue_task:   Some(fifo_dequeue_task),
    select_task_rq: Some(fifo_select_task_rq),
    pick_next_task: Some(fifo_pick_next_task),
    put_prev_task:  Some(fifo_put_prev_task),
    set_next_task:  Some(fifo_set_next_task),
    task_tick:      None,
    task_fork:      None,
    task_dead:      None,
    yield_task:     None,
});

// ---------------------------------------------------------------------------
// init helpers
// ---------------------------------------------------------------------------
fn fifo_subqueue_init(sq: *mut FifoSubqueue) {
    unsafe {
        cffi::list_init(&raw mut (*sq).head);
        (*sq).count = 0;
    }
}

fn fifo_rq_init(fifo_rq: *mut FifoRq, cls_id: c_int, cpu_id: c_int) {
    for i in 0..FIFO_RQ_SUBLEVELS {
        fifo_subqueue_init(unsafe { &raw mut (*fifo_rq).subqueues[i] });
    }
    unsafe {
        (*fifo_rq).ready_mask = 0;
        cffi::rq_init(&raw mut (*fifo_rq).rq);
        cffi::rq_clear_ready(cls_id, cpu_id);
    }
}

fn alloc_fifo_rqs_for_cls(cls_id: c_int) {
    let size = core::mem::size_of::<FifoRq>() * NCPU;
    let p = unsafe { cffi::kmm_alloc_zeroed(size) as *mut FifoRq };
    if p.is_null() {
        unsafe {
            cffi::panic_proc(
                b"alloc_fifo_rqs: failed to allocate fifo_rqs\0",
            );
        }
    }
    fifo_rqs_set(cls_id as usize, p);
    for i in 0..NCPU {
        unsafe {
            fifo_rq_init(p.add(i), cls_id, i as c_int);
            cffi::rq_register(&raw mut (*p.add(i)).rq, cls_id, i as c_int);
        }
    }
}

// P3-1B: only caller is `init_fifo_rq` (same file) -- demoted.
pub(crate) extern "C" fn init_fifo_rq_range(start_cls_id: c_int, end_cls_id: c_int) {
    let mut cls = start_cls_id;
    while cls < end_cls_id {
        unsafe { cffi::sched_class_register(cls, FIFO_SCHED_CLASS.get()) };
        alloc_fifo_rqs_for_cls(cls);
        cls += 1;
    }
}

// P3-1B: only caller is `proc/rq.rs` (already a direct crate-path `use`,
// not an `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn init_fifo_rq() {
    init_fifo_rq_range(1, IDLE_MAJOR_PRIORITY);
}
