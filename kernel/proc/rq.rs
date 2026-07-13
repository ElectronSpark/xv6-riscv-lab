//! Pure-Rust port of `kernel/proc/rq.c` (SECTION 19 of the former
//! `proc_rust_shims.c`).  Owns canonical public C ABI symbols and
//! provides `xv6_rqport_*` aliases used by sibling Rust/C ports.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int};
use core::mem::{size_of, MaybeUninit};
use core::ptr;
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};

use crate::bindings::{cpu_local, rq, rq_percpu, sched_attr, sched_class, sched_entity, thread};
use crate::machine::{cpuid, intr_get, intr_off, PreemptGuard};
use crate::proc::access::{
    err_ptr, is_err_or_null,
    RqPercpuRef, RqRef, SchedAttrConstRef, SchedAttrRef, SchedClassRef, SchedEntityRef,
    ThreadAccess, sched_entity_llist_push_head,
};
use crate::proc::init_fifo_rq;
use crate::proc::init_idle_rq;

const NCPU: usize = 8;
const PRIORITY_MAINLEVELS: usize = 64;
const PRIORITY_MAINLEVEL_SHIFT: c_int = 2;
const PRIORITY_MAINLEVEL_MASK: c_int = 0xFC;
const DEFAULT_MAJOR_PRIORITY: c_int = 17;
const DEFAULT_MINOR_PRIORITY: c_int = 0;
const DEFAULT_PRIORITY: c_int =
    (DEFAULT_MAJOR_PRIORITY << PRIORITY_MAINLEVEL_SHIFT) | DEFAULT_MINOR_PRIORITY;
const DEFAULT_TIME_SLICE: u32 = 10;

const EINVAL: c_int = 22;
const EALREADY: c_int = 114;

const THREAD_RUNNING: i32 = 8;
const THREAD_WAKENING: i32 = 7;

type cpumask_t = u64;

unsafe extern "C" {
    pub safe fn xv6_panic(msg: *const c_char) -> !;
    fn printf(fmt: *const c_char, ...) -> c_int;

    // Per-CPU array exported by C code.
    static mut cpus: [crate::bindings::cpu_local; NCPU];

    // From sched.c port (state accessors)
    safe fn xv6_current_thread() -> *mut thread;
}

#[inline]
fn thread_state_get(p: *mut thread) -> i32 {
    ThreadAccess::from_ptr(p).map_or(0, |t| t.state_load() as i32)
}
#[inline]
fn thread_state_set(p: *mut thread, s: i32) {
    if let Some(t) = ThreadAccess::from_ptr(p) { t.state_store(s as u32); }
}

macro_rules! kpanic { ($m:expr) => {{ xv6_panic(concat!($m, "\0").as_ptr() as *const c_char) }}; }
macro_rules! kassert { ($c:expr, $m:expr) => {{ if !($c) { kpanic!($m); } }}; }
macro_rules! rq_unsafe_call { ($e:expr) => {{ unsafe { $e } }}; }

// bits_ctz8: trailing zero count of low 8 bits, -1 if zero
#[inline] fn bits_ctz8(x: u64) -> c_int {
    let b = (x & 0xff) as u8;
    if b == 0 { -1 } else { b.trailing_zeros() as c_int }
}

#[inline] fn major_priority(prio: c_int) -> c_int {
    (prio & PRIORITY_MAINLEVEL_MASK) >> PRIORITY_MAINLEVEL_SHIFT
}

#[inline] fn thread_awoken(p: *mut thread) -> bool {
    let s = thread_state_get(p);
    s == THREAD_RUNNING || s == THREAD_WAKENING
}

// Per-CPU run queue data — cache-line aligned per declaration in C
#[repr(C, align(64))]
struct PercpuArr([UnsafeCell<MaybeUninit<rq_percpu>>; NCPU]);
// SAFETY: element `cpu_id` of this flat array is reached only via
// `rqg_percpu_base().wrapping_add(cpu_id)` and is, by convention,
// mutated only by the owning CPU's scheduler code; any field within a
// slot that legitimately needs cross-CPU access (e.g. run-queue
// spinlocks embedded in `rq_percpu`) provides its own synchronization,
// matching every other per-CPU array in this crate (cf. `machine.rs`'s
// CPU-indexed statics).
unsafe impl Sync for PercpuArr {}

const PERCPU_ELEM: UnsafeCell<MaybeUninit<rq_percpu>> =
    UnsafeCell::new(MaybeUninit::uninit());
static RQ_PERCPU_DATA: PercpuArr = PercpuArr([PERCPU_ELEM; NCPU]);

#[repr(C)]
struct RqGlobal {
    // Single-owner fields: written once (single-threaded, boot CPU only)
    // by `rq_global_init`/`rqg_set_sched_class` before any other CPU is
    // brought up, then only ever read afterwards. Accessed via
    // `rqg_ref()` (`&mut RqGlobal`), which is sound *only* because no
    // other CPU is concurrently touching `RqGlobal` while these fields
    // are live-written or -read that way.
    percpu: *mut rq_percpu,
    sched_class: [*mut sched_class; PRIORITY_MAINLEVELS],
    // Cross-CPU-shared field: `rq_cpu_activate` OR's in a bit from each
    // CPU's independent bring-up path (see `idle_thread_init`), and
    // `rq_select_task_rq`/`rq_cpu_is_idle` read it from any CPU with no
    // lock held. MUST be accessed only through `rqg_active_mask_atomic()`
    // (never through `rqg_ref()`/`&mut RqGlobal`) -- see that function for
    // the ordering rationale.
    active_cpu_mask: AtomicU64,
}
// SAFETY: see the field-level comments on `RqGlobal` above —
// `percpu`/`sched_class` are single-owner (written once at boot before
// any other CPU is up, read-only afterward), and `active_cpu_mask` is
// the one field genuinely shared across CPUs, accessed exclusively
// through `rqg_active_mask_atomic()`'s `AtomicU64` ops rather than
// through `&mut RqGlobal`/`&RqGlobal` (see Package B's fix in the
// rustify history: `&mut RqGlobal` is never materialized for that
// field).
unsafe impl Sync for RqGlobalCell {}
#[repr(transparent)]
struct RqGlobalCell(UnsafeCell<RqGlobal>);

static RQ_GLOBAL: RqGlobalCell = RqGlobalCell(UnsafeCell::new(RqGlobal {
    percpu: ptr::null_mut(),
    sched_class: [ptr::null_mut(); PRIORITY_MAINLEVELS],
    active_cpu_mask: AtomicU64::new(0),
}));

#[inline] fn rqg() -> *mut RqGlobal { RQ_GLOBAL.0.get() }
// SAFETY: `rqg()` is a `'static` valid, aligned, non-null pointer into
// `RQ_GLOBAL`. Callers of `rqg_ref()` must restrict use to the
// single-owner fields documented on `RqGlobal` (`percpu`, `sched_class`)
// -- never `active_cpu_mask`, which is shared cross-CPU and would make
// this `&mut` alias a concurrently-read/written `AtomicU64` (see
// `rqg_active_mask_atomic`).
#[inline] fn rqg_ref<'a>() -> &'a mut RqGlobal { unsafe { &mut *rqg() } }
#[inline] fn rqg_percpu_base() -> *mut rq_percpu { rqg_ref().percpu }
#[inline] fn rqg_sched_class(cls_id: c_int) -> *mut sched_class { rqg_ref().sched_class[cls_id as usize] }
#[inline] fn rqg_set_sched_class(cls_id: usize, cls: *mut sched_class) { rqg_ref().sched_class[cls_id] = cls; }

/// Shared reference to just the `active_cpu_mask` field, obtained without
/// ever materializing `&mut RqGlobal`. `AtomicU64` is `Sync`, so handing
/// out `&AtomicU64` to any number of CPUs concurrently is sound; handing
/// out `&mut RqGlobal` while other CPUs read/RMW this same field (as the
/// old plain-`u64` code did) is a data race (Rust UB) regardless of
/// whether the racing values are individually "safe" integers.
#[inline] fn rqg_active_mask_atomic<'a>() -> &'a AtomicU64 {
    // SAFETY: `rqg()` is `'static`-valid; `active_cpu_mask` is a
    // `repr(C)` field at a fixed offset, so this is a plain field
    // projection through a raw pointer (`addr_of!`, no intermediate
    // reference to `RqGlobal` is created). The resulting `&AtomicU64`
    // may be freely shared/aliased across CPUs.
    unsafe { &*core::ptr::addr_of!((*rqg()).active_cpu_mask) }
}

// Ordering analysis (H-05): `rq_cpu_activate` is called once per CPU from
// `idle_thread_init`, *after* that CPU has already made plain
// (non-atomic) stores publishing its own scheduling state --
// `CpuLocalRef::set_proc`/`set_idle_thread` (kernel/proc/thread.rs) both
// run strictly before `xv6_rqport_rq_cpu_activate(cpuid())`. Readers on
// *other* CPUs gate a cross-CPU, non-atomic read of exactly that state on
// this bit: `rq_cpu_is_idle` returns early ("treat as idle") unless the
// mask bit is set, and only then reads `CpuLocalRef::idle_thread_ptr()`
// for that CPU. If the mask load did not synchronize with the activating
// CPU's Release, that idle-thread-pointer read would be racing the
// plain store above with no happens-before edge -- a second, adjacent
// data race riding on this one. So the mask is not "advisory only": one
// existing reader already depends on it as a publication fence.
// Therefore: `Release` on the writer (`fetch_or`) paired with `Acquire`
// on every reader (`load`), not `Relaxed`. `fetch_or`'s implicit read
// side does not need `Acquire`: each CPU only ever sets its *own* bit
// (`rq_cpu_activate(cpuid())`), so the RMW never needs to observe another
// CPU's concurrent modification to make a decision -- only to publish
// after it completes.
#[inline] fn rqg_active_mask() -> u64 { rqg_active_mask_atomic().load(Ordering::Acquire) }
#[inline] fn rqg_or_active_mask(mask: u64) { rqg_active_mask_atomic().fetch_or(mask, Ordering::Release); }
#[inline] fn cpu_local_ptr(cpu_id: c_int) -> *mut cpu_local {
    unsafe { (&raw mut cpus).cast::<cpu_local>().wrapping_add(cpu_id as usize) }
}

#[inline] fn rqpc(cpu_id: c_int) -> *mut rq_percpu {
    rqg_percpu_base().wrapping_add(cpu_id as usize)
}
#[inline] fn rqpc_current() -> *mut rq_percpu { rqpc(cpuid()) }
#[inline] fn rqpc_ref<'a>(cpu_id: c_int) -> RqPercpuRef<'a> {
    // SAFETY: `rqpc(cpu_id)` indexes into the statically-allocated, always-initialized
    // per-CPU `rq_percpu` array; every caller passes an in-range `cpu_id`, so
    // the result is never null.
    unsafe { RqPercpuRef::assume(rqpc(cpu_id)) }
}
#[inline] fn rq_ref<'a>(r: *mut rq) -> RqRef<'a> {
    // SAFETY: `rq_ref` is this file's choke point for `RqRef::assume`; every call site
    // passes an `rq` pointer obtained from `get_rq`/`rqpc_ref(..).rq_at(..)`
    // (both index statically-allocated, always-initialized storage) or already
    // null-checked by its own caller.
    unsafe { RqRef::assume(r) }
}
#[inline] fn se_ref<'a>(se: *mut sched_entity) -> SchedEntityRef<'a> {
    // SAFETY: `se_ref` is this file's choke point for `SchedEntityRef::assume`; every
    // call site passes a `sched_entity` pointer already null-checked by its
    // own caller, or the running thread's embedded, always-allocated
    // scheduling entity.
    unsafe { SchedEntityRef::assume(se) }
}
#[inline] fn sched_class_of(cls_id: c_int) -> *mut sched_class {
    rqg_sched_class(cls_id)
}
#[inline] fn get_rq(cls_id: c_int, cpu_id: c_int) -> *mut rq {
    rqpc_ref(cpu_id).rq_at(cls_id as usize)
}
#[inline] fn rq_lock_held(cpu_id: c_int) -> bool {
    rqpc_ref(cpu_id).lock_ref().holding()
}

// =========================================================================
// Public ABI (canonical, no_mangle) + xv6_rqport_* aliases via macro.
// =========================================================================

// ---- rq_is_initialized ----
#[no_mangle]
pub extern "C" fn rq_is_initialized() -> bool {
    !rqg_percpu_base().is_null()
}
#[no_mangle]
pub extern "C" fn xv6_rqport_rq_is_initialized() -> bool {
    rq_is_initialized()
}

// ---- rq_set_ready / rq_clear_ready ----
#[no_mangle]
pub extern "C" fn rq_set_ready(cls_id: c_int, cpu_id: c_int) {
    let pc = rqpc_ref(cpu_id);
    let top_mask = 1u64 << (cls_id >> 3);
    let secondary_mask = 1u64 << cls_id;
    pc.or_ready_mask(top_mask);
    pc.or_ready_mask_secondary(secondary_mask);
}
#[no_mangle]
pub extern "C" fn xv6_rqport_rq_set_ready(cls_id: c_int, cpu_id: c_int) {
    rq_set_ready(cls_id, cpu_id)
}

#[no_mangle]
pub extern "C" fn rq_clear_ready(cls_id: c_int, cpu_id: c_int) {
    let pc = rqpc_ref(cpu_id);
    let top_id = cls_id >> 3;
    let secondary_mask = 1u64 << cls_id;
    let group_mask = 0xffu64 << (top_id << 3);
    let top_mask = 1u64 << top_id;
    pc.and_ready_mask_secondary(!secondary_mask);
    if pc.ready_mask_secondary() & group_mask == 0 {
        pc.and_ready_mask(!top_mask);
    }
}
#[no_mangle]
pub extern "C" fn xv6_rqport_rq_clear_ready(cls_id: c_int, cpu_id: c_int) {
    rq_clear_ready(cls_id, cpu_id)
}

// ---- get_rq_for_cpu ----
#[no_mangle]
pub extern "C" fn get_rq_for_cpu(cls_id: c_int, cpu_id: c_int) -> *mut rq {
    if cls_id < 0 || cls_id >= PRIORITY_MAINLEVELS as c_int {
        return err_ptr(-EINVAL);
    }
    if cpu_id < 0 || cpu_id >= NCPU as c_int {
        return err_ptr(-EINVAL);
    }
    get_rq(cls_id, cpu_id)
}
#[no_mangle]
pub extern "C" fn xv6_rqport_get_rq_for_cpu(cls_id: c_int, cpu_id: c_int) -> *mut rq {
    get_rq_for_cpu(cls_id, cpu_id)
}

// ---- pick_next_rq ----
#[no_mangle]
pub extern "C" fn pick_next_rq() -> *mut rq {
    let cpu = cpuid();
    let pc = rqpc_ref(cpu);
    let (top_mask, mut secondary_mask) = (pc.ready_mask(), pc.ready_mask_secondary());
    let top_id = bits_ctz8(top_mask);
    if top_id < 0 {
        kpanic!("pick_next_rq: no ready tasks");
    }
    let mut group_bits = ((secondary_mask >> (top_id << 3)) & 0xff) as u64;
    if group_bits == 0 {
        secondary_mask = pc.ready_mask_secondary();
        group_bits = ((secondary_mask >> (top_id << 3)) & 0xff) as u64;
        if group_bits == 0 {
            kpanic!("pick_next_rq: inconsistent ready mask");
        }
    }
    let cls_id = (top_id << 3) + bits_ctz8(group_bits);
    let r = get_rq_for_cpu(cls_id, cpu);
    if is_err_or_null(r) {
        kpanic!("pick_next_rq: invalid rq");
    }
    r
}
#[no_mangle]
pub extern "C" fn xv6_rqport_pick_next_rq() -> *mut rq { pick_next_rq() }

// ---- rq_global_init ----
#[no_mangle]
pub extern "C" fn rq_global_init() {
    rqg_ref().percpu = RQ_PERCPU_DATA.0[0].get() as *mut rq_percpu;
    for i in 0..NCPU as c_int {
        let pc = rqpc_ref(i);
        pc.zero_storage();
        pc.init_lock(c"rq_percpu_lock".as_ptr() as *mut c_char);
        pc.set_ready_mask(0);
        pc.set_ready_mask_secondary(0);
        pc.clear_wake_list_head();
        for j in 0..PRIORITY_MAINLEVELS {
            pc.set_rq_at(j, ptr::null_mut());
        }
    }
    for i in 0..PRIORITY_MAINLEVELS {
        rqg_set_sched_class(i, ptr::null_mut());
    }
    unsafe {
        init_idle_rq();
        init_fifo_rq();
    }
}
#[no_mangle]
pub extern "C" fn xv6_rqport_rq_global_init() { rq_global_init() }

// ---- rq_init ----
#[no_mangle]
pub unsafe extern "C" fn rq_init(r: *mut rq) {
    kassert!(!r.is_null(), "rq_init: rq is NULL");
    unsafe {
        ptr::write_bytes(r as *mut u8, 0, size_of::<rq>());
    }
    rq_ref(r).set_task_count(0);
}
#[no_mangle]
pub extern "C" fn xv6_rqport_rq_init(r: *mut rq) { rq_unsafe_call!(rq_init(r)) }

// ---- rq_register ----
#[no_mangle]
pub extern "C" fn rq_register(r: *mut rq, cls_id: c_int, cpu_id: c_int) {
    kassert!(!r.is_null(), "rq_register: rq is NULL");
    kassert!(cls_id >= 0 && cls_id < PRIORITY_MAINLEVELS as c_int, "rq_register: invalid cls_id");
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_register: invalid cpu_id");
    let pc = rqpc_ref(cpu_id);
    let cls = sched_class_of(cls_id);
    let rr = rq_ref(r);
    kassert!(pc.rq_at(cls_id as usize).is_null(), "rq_register: already registered");
    rr.set_class_id(cls_id);
    rr.set_cpu_id(cpu_id);
    rr.set_sched_class(cls);
    kassert!(!rr.sched_class_ptr().is_null(), "rq_register: sched_class is NULL");
    pc.set_rq_at(cls_id as usize, r);
}
#[no_mangle]
pub extern "C" fn xv6_rqport_rq_register(r: *mut rq, cls_id: c_int, cpu_id: c_int) {
    rq_register(r, cls_id, cpu_id)
}

// ---- sched_entity_init ----
#[no_mangle]
pub extern "C" fn sched_entity_init(se: *mut sched_entity, p: *mut thread) {
    kassert!(!se.is_null(), "sched_entity_init: se is NULL");
    let sr = se_ref(se);
    sr.set_rq(ptr::null_mut());
    sr.set_priority(DEFAULT_PRIORITY);
    sr.set_sched_class(ptr::null_mut());
    sr.pi_lock_ref().init(c"se_pi_lock".as_ptr() as *mut c_char);
    sr.set_on_rq_plain(0);
    sr.set_on_cpu_plain(0);
    sr.set_cpu_id(-1);
    sr.set_affinity_mask((1u64 << NCPU) - 1);
    sr.set_start_time(0);
    sr.set_exec_start(0);
    sr.set_exec_end(0);
    sr.set_thread(p);
}
#[no_mangle]
pub extern "C" fn xv6_rqport_sched_entity_init(se: *mut sched_entity, p: *mut thread) {
    sched_entity_init(se, p)
}

// ---- sched_class_register ----
#[no_mangle]
pub extern "C" fn sched_class_register(id: c_int, cls: *mut sched_class) {
    if id < 0 || id >= PRIORITY_MAINLEVELS as c_int {
        kpanic!("sched_class_register: invalid id");
    }
    if cls.is_null() {
        kpanic!("sched_class_register: cls is NULL");
    }
    // SAFETY: `cls` is proven non-null by the diverging `kpanic!` immediately above.
    if !unsafe { SchedClassRef::assume(cls) }.has_pick_next_task() {
        kpanic!("sched_class_register: no pick_next_task");
    }
    rqg_set_sched_class(id as usize, cls);
}
#[no_mangle]
pub extern "C" fn xv6_rqport_sched_class_register(id: c_int, cls: *mut sched_class) {
    sched_class_register(id, cls)
}

// ---- rq_lock / unlock / trylock / current variants ----
#[no_mangle]
pub extern "C" fn rq_lock(cpu_id: c_int) {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_lock: invalid cpu_id");
    rqpc_ref(cpu_id).lock_ref().lock();
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_lock(cpu_id: c_int) { rq_lock(cpu_id) }

#[no_mangle]
pub extern "C" fn rq_trylock(cpu_id: c_int) -> c_int {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_trylock: invalid cpu_id");
    rqpc_ref(cpu_id).lock_ref().trylock()
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_trylock(cpu_id: c_int) -> c_int { rq_trylock(cpu_id) }

#[no_mangle]
pub extern "C" fn rq_unlock(cpu_id: c_int) {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_unlock: invalid cpu_id");
    kassert!(rq_lock_held(cpu_id), "rq_unlock: lock not held");
    rqpc_ref(cpu_id).lock_ref().unlock();
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_unlock(cpu_id: c_int) { rq_unlock(cpu_id) }

#[no_mangle]
pub extern "C" fn rq_lock_irqsave(cpu_id: c_int) -> c_int {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_lock_irqsave: invalid cpu_id");
    rqpc_ref(cpu_id).lock_ref().lock_irqsave()
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_lock_irqsave(cpu_id: c_int) -> c_int { rq_lock_irqsave(cpu_id) }

#[no_mangle]
pub extern "C" fn rq_unlock_irqrestore(cpu_id: c_int, state: c_int) {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_unlock_irqrestore: invalid cpu_id");
    kassert!(rq_lock_held(cpu_id), "rq_unlock_irqrestore: lock not held");
    rqpc_ref(cpu_id).lock_ref().unlock_irqrestore(state);
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_unlock_irqrestore(cpu_id: c_int, state: c_int) { rq_unlock_irqrestore(cpu_id, state) }

#[no_mangle]
pub extern "C" fn rq_lock_current_irqsave() -> c_int {
    let intr_state = intr_get() as c_int;
    intr_off();
    rq_lock_irqsave(cpuid());
    intr_state
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_lock_current_irqsave() -> c_int { rq_lock_current_irqsave() }

#[no_mangle]
pub extern "C" fn rq_unlock_current_irqrestore(state: c_int) {
    rq_unlock_irqrestore(cpuid(), state)
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_unlock_current_irqrestore(state: c_int) { rq_unlock_current_irqrestore(state) }

#[no_mangle]
pub extern "C" fn rq_lock_two(c1: c_int, c2: c_int) {
    kassert!(c1 >= 0 && c1 < NCPU as c_int, "rq_lock_two: invalid c1");
    kassert!(c2 >= 0 && c2 < NCPU as c_int, "rq_lock_two: invalid c2");
    if c1 < c2 { rq_lock(c1); rq_lock(c2); }
    else if c2 < c1 { rq_lock(c2); rq_lock(c1); }
    else { rq_lock(c1); }
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_lock_two(c1: c_int, c2: c_int) { rq_lock_two(c1, c2) }

#[no_mangle]
pub extern "C" fn rq_trylock_two(c1: c_int, c2: c_int) -> c_int {
    kassert!(c1 >= 0 && c1 < NCPU as c_int, "rq_trylock_two: invalid c1");
    kassert!(c2 >= 0 && c2 < NCPU as c_int, "rq_trylock_two: invalid c2");
    let (first, second) = if c1 <= c2 { (c1, c2) } else { (c2, c1) };
    if rq_trylock(first) == 0 { return 0; }
    if first == second { return 1; }
    if rq_trylock(second) == 0 {
        rq_unlock(first);
        return 0;
    }
    1
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_trylock_two(c1: c_int, c2: c_int) -> c_int { rq_trylock_two(c1, c2) }

#[no_mangle]
pub extern "C" fn rq_unlock_two(c1: c_int, c2: c_int) {
    kassert!(c1 >= 0 && c1 < NCPU as c_int, "rq_unlock_two: invalid c1");
    kassert!(c2 >= 0 && c2 < NCPU as c_int, "rq_unlock_two: invalid c2");
    if c1 < c2 { rq_unlock(c2); rq_unlock(c1); }
    else if c2 < c1 { rq_unlock(c1); rq_unlock(c2); }
    else { rq_unlock(c1); }
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_unlock_two(c1: c_int, c2: c_int) { rq_unlock_two(c1, c2) }

#[no_mangle]
pub extern "C" fn rq_lock_current() {
    let g = PreemptGuard::new();
    rq_lock(g.cpuid() as c_int);
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_lock_current() { rq_lock_current() }

#[no_mangle]
pub extern "C" fn rq_unlock_current() {
    rq_unlock(cpuid())
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_unlock_current() { rq_unlock_current() }

#[no_mangle]
pub extern "C" fn rq_holding(cpu_id: c_int) -> c_int {
    if cpu_id < 0 || cpu_id >= NCPU as c_int { return 0; }
    if rq_lock_held(cpu_id) { 1 } else { 0 }
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_holding(cpu_id: c_int) -> c_int { rq_holding(cpu_id) }

#[no_mangle]
pub extern "C" fn rq_holding_current() -> c_int {
    // SAFETY: `rqpc_current()` indexes into statically-allocated, always-initialized
    // per-CPU storage; never null.
    unsafe { RqPercpuRef::assume(rqpc_current()) }.lock_ref().holding() as c_int
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_holding_current() -> c_int { rq_holding_current() }

// ---- rq_percpu_lock_get / put_unlock ----
#[no_mangle]
pub extern "C" fn rq_percpu_lock_get(cpu_id: c_int) -> *mut rq_percpu {
    if cpu_id < 0 || cpu_id >= NCPU as c_int { return ptr::null_mut(); }
    let pc = rqpc(cpu_id);
    rqpc_ref(cpu_id).lock_ref().lock();
    pc
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_percpu_lock_get(cpu_id: c_int) -> *mut rq_percpu { rq_percpu_lock_get(cpu_id) }

#[no_mangle]
pub extern "C" fn rq_percpu_lock_get_current() -> *mut rq_percpu {
    let _g = crate::machine::PreemptGuard::new();
    let pc = rqpc_current();
    // SAFETY: `pc` (`rqpc_current()`) indexes into statically-allocated, always-
    // initialized per-CPU storage; never null.
    unsafe { RqPercpuRef::assume(pc) }.lock_ref().lock();
    pc
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_percpu_lock_get_current() -> *mut rq_percpu { rq_percpu_lock_get_current() }

#[no_mangle]
pub extern "C" fn rq_percpu_put_unlock(pc: *mut rq_percpu) {
    if pc.is_null() { return; }
    // SAFETY: `pc` is checked non-null immediately above.
    unsafe { RqPercpuRef::assume(pc) }.lock_ref().unlock();
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_percpu_put_unlock(pc: *mut rq_percpu) { rq_percpu_put_unlock(pc) }

// ---- rq_select_task_rq ----
#[no_mangle]
pub extern "C" fn rq_select_task_rq(se: *mut sched_entity, cpumask: cpumask_t) -> *mut rq {
    if se.is_null() { return err_ptr(-EINVAL); }
    let sr = se_ref(se);
    let major_prio = sr.priority() >> PRIORITY_MAINLEVEL_SHIFT;
    if major_prio < 0 || major_prio >= PRIORITY_MAINLEVELS as c_int {
        return err_ptr(-EINVAL);
    }
    let cls = sched_class_of(major_prio);
    if cls.is_null() { return err_ptr(-EINVAL); }

    let active = rqg_active_mask();
    let mut effective_mask = cpumask & active;
    if effective_mask == 0 { effective_mask = active; }

    // SAFETY: `cls` is checked non-null immediately above (`if cls.is_null() { return
    // err_ptr(-EINVAL); }`).
    if let Some(selected) = unsafe { SchedClassRef::assume(cls) }
        .select_task_rq(sr.rq_ptr(), se, effective_mask)
    {
        return selected;
    }

    let cur_cpu = cpuid();
    if effective_mask & (1u64 << cur_cpu) != 0 {
        let r = get_rq_for_cpu(major_prio, cur_cpu);
        if !is_err_or_null(r) { return r; }
    }
    for cpu in 0..NCPU as c_int {
        if effective_mask & (1u64 << cpu) != 0 {
            let r = get_rq_for_cpu(major_prio, cpu);
            if !is_err_or_null(r) { return r; }
        }
    }
    ptr::null_mut()
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_select_task_rq(se: *mut sched_entity, mask: cpumask_t) -> *mut rq { rq_select_task_rq(se, mask) }

// ---- enqueue / dequeue / pick / put_prev / set_next ----
#[no_mangle]
pub extern "C" fn rq_enqueue_task(r: *mut rq, se: *mut sched_entity) {
    let rr = rq_ref(r);
    let sr = se_ref(se);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_enqueue_task: rq lock not held");
    if !sr.rq_ptr().is_null() {
        let p = sr.thread_ptr();
        let pt = ThreadAccess::from_ptr(p);
        let pn = pt.map_or(c"NULL".as_ptr(), |t| t.name_ptr());
        let se_rq = sr.rq_ptr();
        unsafe {
            printf(c"rq_enqueue_task BUG: se->rq=%p (cpu=%d), target rq=%p (cpu=%d)\n".as_ptr() as *const c_char,
                se_rq, if se_rq.is_null() { -1 } else { rq_ref(se_rq).cpu_id() }, r, rr.cpu_id());
            printf(c"  thread=%s pid=%d state=%d on_rq=%d on_cpu=%d se_cpu=%d\n".as_ptr() as *const c_char,
                pn, pt.map_or(-1, |t| t.pid()), if p.is_null() { -1 } else { thread_state_get(p) },
                sr.on_rq_plain(), sr.on_cpu_plain(), sr.cpu_id());
        }
        kpanic!("rq_enqueue_task: se rq is not NULL");
    }
    rr.enqueue_task(se);
    sr.set_rq(r);
    sr.cpu_id_store_release(rr.cpu_id());
    sr.on_rq_store_release(1);
    sr.set_sched_class(rr.sched_class_ptr());
    rr.inc_task_count();
    rq_set_ready(rr.class_id(), rr.cpu_id());
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_enqueue_task(r: *mut rq, se: *mut sched_entity) { rq_enqueue_task(r, se) }

#[no_mangle]
pub extern "C" fn rq_dequeue_task(r: *mut rq, se: *mut sched_entity) {
    let rr = rq_ref(r);
    let sr = se_ref(se);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_dequeue_task: rq lock not held");
    kassert!(sr.rq_ptr() == r, "rq_dequeue_task: se->rq mismatch");
    kassert!(rr.task_count() > 0, "rq_dequeue_task: task_count == 0");
    kassert!(sr.sched_class_ptr() == rq_ref(sr.rq_ptr()).sched_class_ptr(), "rq_dequeue_task: sched_class mismatch");
    rr.dequeue_task(se);
    sr.set_rq(ptr::null_mut());
    sr.set_sched_class(ptr::null_mut());
    sr.on_rq_store_release(0);
    rr.dec_task_count();
    if rr.task_count() == 0 {
        rq_clear_ready(rr.class_id(), rr.cpu_id());
    }
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_dequeue_task(r: *mut rq, se: *mut sched_entity) { rq_dequeue_task(r, se) }

#[no_mangle]
pub extern "C" fn rq_pick_next_task(r: *mut rq) -> *mut sched_entity {
    let rr = rq_ref(r);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_pick_next_task: rq lock not held");
    rr.pick_next_task()
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_pick_next_task(r: *mut rq) -> *mut sched_entity { rq_pick_next_task(r) }

#[no_mangle]
pub extern "C" fn rq_put_prev_task(se: *mut sched_entity) {
    let sr = se_ref(se);
    let r = sr.rq_ptr();
    kassert!(!r.is_null(), "rq_put_prev_task: se->rq NULL");
    let rr = rq_ref(r);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_put_prev_task: rq lock not held");
    kassert!(rr.task_count() > 0, "rq_put_prev_task: task_count == 0");
    kassert!(sr.sched_class_ptr() == rr.sched_class_ptr(), "rq_put_prev_task: sched_class mismatch");
    rr.put_prev_task(se);
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_put_prev_task(se: *mut sched_entity) { rq_put_prev_task(se) }

#[no_mangle]
pub extern "C" fn rq_set_next_task(se: *mut sched_entity) {
    let sr = se_ref(se);
    let r = sr.rq_ptr();
    kassert!(!r.is_null(), "rq_set_next_task: se->rq NULL");
    let rr = rq_ref(r);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_set_next_task: rq lock not held");
    kassert!(rr.task_count() > 0, "rq_set_next_task: task_count == 0");
    kassert!(sr.sched_class_ptr() == rr.sched_class_ptr(), "rq_set_next_task: sched_class mismatch");
    rqpc_ref(rr.cpu_id()).current_se_store_release(se);
    rr.set_next_task(se);
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_set_next_task(se: *mut sched_entity) { rq_set_next_task(se) }

// ---- rq_cpu_allowed ----
#[no_mangle]
pub extern "C" fn rq_cpu_allowed(se: *mut sched_entity, cpu_id: c_int) -> bool {
    if se.is_null() { return false; }
    (se_ref(se).affinity_mask() & (1u64 << cpu_id)) != 0
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_cpu_allowed(se: *mut sched_entity, cpu_id: c_int) -> bool { rq_cpu_allowed(se, cpu_id) }

// ---- rq_task_tick / fork / dead / yield ----
#[no_mangle]
pub extern "C" fn rq_task_tick(se: *mut sched_entity) {
    let sr = se_ref(se);
    let r = sr.rq_ptr();
    kassert!(!sr.sched_class_ptr().is_null(), "rq_task_tick: sched_class NULL");
    kassert!(!r.is_null(), "rq_task_tick: rq NULL");
    let rr = rq_ref(r);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_task_tick: rq lock not held");
    kassert!(sr.sched_class_ptr() == rr.sched_class_ptr(), "rq_task_tick: sched_class mismatch");
    rr.task_tick(se);
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_task_tick(se: *mut sched_entity) { rq_task_tick(se) }

#[no_mangle]
pub extern "C" fn rq_task_fork(se: *mut sched_entity) {
    let sr = se_ref(se);
    // SAFETY: `xv6_current_thread()` is the running thread; the currently running
    // thread's pointer (from `xv6_current_thread()`/`current()`) is a kernel-
    // wide invariant: always non-null while executing kernel code on behalf of
    // a thread.
    let cur = unsafe { ThreadAccess::assume(xv6_current_thread()) }.sched_entity_ptr();
    let cur_cls = se_ref(cur).sched_class_ptr();
    if !cur_cls.is_null()
        // SAFETY: `cur_cls` is proven non-null by the short-circuiting `&&`
        // (`!cur_cls.is_null() && ...`).
        && unsafe { SchedClassRef::assume(cur_cls) }.task_fork(sr.rq_ptr(), se)
    {
        return;
    }
    let def_cls = sched_class_of(DEFAULT_MAJOR_PRIORITY);
    if !def_cls.is_null() {
        // SAFETY: `def_cls` is checked non-null immediately above.
        unsafe { SchedClassRef::assume(def_cls) }.task_fork(sr.rq_ptr(), se);
    }
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_task_fork(se: *mut sched_entity) { rq_task_fork(se) }

#[no_mangle]
pub extern "C" fn rq_task_dead(se: *mut sched_entity) {
    let sr = se_ref(se);
    if !sr.rq_ptr().is_null() && !sr.sched_class_ptr().is_null() {
        sr.call_sched_class_task_dead();
    }
    if !sr.rq_ptr().is_null() {
        rq_dequeue_task(sr.rq_ptr(), se);
    }
    sr.set_sched_class(ptr::null_mut());
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_task_dead(se: *mut sched_entity) { rq_task_dead(se) }

#[no_mangle]
pub extern "C" fn rq_yield_task() {
    // SAFETY: `xv6_current_thread()` is the running thread; the currently running
    // thread's pointer (from `xv6_current_thread()`/`current()`) is a kernel-
    // wide invariant: always non-null while executing kernel code on behalf of
    // a thread.
    let cur = unsafe { ThreadAccess::assume(xv6_current_thread()) }.sched_entity_ptr();
    let current_rq = se_ref(cur).rq_ptr();
    kassert!(!current_rq.is_null(), "rq_yield_task: current_rq NULL");
    let rr = rq_ref(current_rq);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_yield_task: rq lock not held");
    rr.yield_task();
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_yield_task() { rq_yield_task() }

// ---- rq_cpu_is_idle ----
#[no_mangle]
pub extern "C" fn rq_cpu_is_idle(cpu_id: c_int) -> bool {
    if cpu_id < 0 || cpu_id >= NCPU as c_int { return false; }
    if rqg_active_mask() & (1u64 << cpu_id) == 0 {
        return true;
    }
    let current_se = rqpc_ref(cpu_id).current_se_load_acquire();
    let idle_se = {
        let current_se = rqpc_ref(cpu_id).current_se_load_acquire();
        let cpu_local_p = cpu_local_ptr(cpu_id);
        // SAFETY: `cpu_local_ptr(cpu_id)` indexes into statically-allocated, always-
        // initialized per-CPU storage; `cpu_id` was range-checked at this
        // function's entry above.
        let idle = unsafe { crate::proc::access::CpuLocalRef::assume(cpu_local_p) }.idle_thread_ptr();
        let idle_se = ThreadAccess::from_ptr(idle).map_or(ptr::null_mut(), |t| t.sched_entity_ptr());
        let _ = current_se;
        idle_se
    };
    current_se.is_null() || current_se == idle_se
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_cpu_is_idle(cpu_id: c_int) -> bool { rq_cpu_is_idle(cpu_id) }

// ---- wake list (LLIST) ----
#[inline]
unsafe fn __thread_state_get(p: *mut thread) -> i32 { thread_state_get(p) }
#[inline]
unsafe fn __thread_state_set(p: *mut thread, s: i32) { thread_state_set(p, s); }

#[inline]
unsafe fn llist_push(head_p: *mut *mut sched_entity, node: *mut sched_entity) {
    let head_atomic = head_p as *mut AtomicPtr<sched_entity>;
    let mut old = unsafe { (*head_atomic).load(Ordering::Acquire) };
    loop {
        unsafe {
            se_ref(node).set_wake_next(old);
            match (*head_atomic).compare_exchange_weak(
                old, node, Ordering::Release, Ordering::Acquire,
            ) {
                Ok(_) => return,
                Err(prev) => old = prev,
            }
        }
    }
}

#[inline]
unsafe fn llist_migrate(head_p: *mut *mut sched_entity) -> *mut sched_entity {
    let head_atomic = head_p as *mut AtomicPtr<sched_entity>;
    let mut old = unsafe { (*head_atomic).load(Ordering::Acquire) };
    loop {
        match unsafe {
            (*head_atomic).compare_exchange_weak(
                old, ptr::null_mut(), Ordering::Release, Ordering::Acquire,
            )
        } {
            Ok(_) => return old,
            Err(prev) => old = prev,
        }
    }
}

#[no_mangle]
pub extern "C" fn rq_add_wake_list(cpu_id: c_int, se: *mut sched_entity) -> c_int {
    if se.is_null() { return -EINVAL; }
    let sr = se_ref(se);
    let p = sr.thread_ptr();
    if p.is_null() { return -EINVAL; }
    if !thread_awoken(p) { return -EINVAL; }
    let pc = rq_percpu_lock_get(cpu_id);
    if pc.is_null() { return -EINVAL; }
    // SAFETY: `pc` is checked non-null immediately above.
    let pcr = unsafe { RqPercpuRef::assume(pc) };
    if sr.on_rq_load_acquire() != 0 {
        rq_percpu_put_unlock(pc);
        return -EALREADY;
    }
    pcr.wake_list_push(se);
    rq_percpu_put_unlock(pc);
    0
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_add_wake_list(cpu_id: c_int, se: *mut sched_entity) -> c_int { rq_add_wake_list(cpu_id, se) }

#[no_mangle]
pub extern "C" fn rq_pop_all_wake_list(pc: *mut rq_percpu) -> *mut sched_entity {
    if pc.is_null() { return ptr::null_mut(); }
    // SAFETY: `pc` is checked non-null immediately above.
    unsafe { RqPercpuRef::assume(pc) }.wake_list_migrate()
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_pop_all_wake_list(pc: *mut rq_percpu) -> *mut sched_entity { rq_pop_all_wake_list(pc) }

#[no_mangle]
pub unsafe extern "C" fn rq_flush_wake_list(cpu_id: c_int) {
    if cpu_id < 0 || cpu_id >= NCPU as c_int { return; }
    unsafe {
        let pc = rq_percpu_lock_get(cpu_id);
        let mut wake_list = llist_migrate(&raw mut (*pc).wake_list_head);
        if wake_list.is_null() {
            rq_percpu_put_unlock(pc);
            return;
        }
        let mut retry_list: *mut sched_entity = ptr::null_mut();
        while !wake_list.is_null() {
            let se = wake_list;
            wake_list = (*se).wake_next;
            (*se).wake_next = ptr::null_mut();

            if __thread_state_get((*se).thread) != THREAD_WAKENING {
                continue;
            }
            let on_rq_p = &raw mut (*se).on_rq as *mut core::sync::atomic::AtomicI32;
            if (*on_rq_p).load(Ordering::Acquire) != 0 {
                __thread_state_set((*se).thread, THREAD_RUNNING);
                continue;
            }
            let major_prio = (*se).priority >> PRIORITY_MAINLEVEL_SHIFT;
            let r = (*pc).rqs[major_prio as usize];
            if !r.is_null() {
                rq_enqueue_task(r, se);
                __thread_state_set((*se).thread, THREAD_RUNNING);
            } else {
                llist_push(&raw mut retry_list, se);
            }
        }
        while !retry_list.is_null() {
            let se = retry_list;
            retry_list = (*se).wake_next;
            llist_push(&raw mut (*pc).wake_list_head, se);
        }
        rq_percpu_put_unlock(pc);
    }
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_flush_wake_list(cpu_id: c_int) { rq_unsafe_call!(rq_flush_wake_list(cpu_id)) }

// ---- sched_attr_init / getattr / setattr ----
#[no_mangle]
pub extern "C" fn sched_attr_init(attr: *mut sched_attr) {
    if attr.is_null() { return; }
    // SAFETY: `attr` is checked non-null immediately above.
    let ar = unsafe { SchedAttrRef::assume(attr) };
    ar.set_size(size_of::<sched_attr>() as u32);
    ar.set_affinity_mask((1u64 << NCPU) - 1);
    ar.set_time_slice(DEFAULT_TIME_SLICE);
    ar.set_priority(DEFAULT_PRIORITY);
    ar.set_flags(0);
}
#[no_mangle] pub extern "C" fn xv6_rqport_sched_attr_init(attr: *mut sched_attr) { sched_attr_init(attr) }

#[no_mangle]
pub extern "C" fn sched_getattr(se: *mut sched_entity, attr: *mut sched_attr) -> c_int {
    if se.is_null() || attr.is_null() { return -EINVAL; }
    let sr = se_ref(se);
    // SAFETY: `attr` is checked non-null at the top of `sched_getattr` above.
    let ar = unsafe { SchedAttrRef::assume(attr) };
    let _g = sr.pi_lock_ref().scoped_lock();
    ar.set_size(size_of::<sched_attr>() as u32);
    ar.set_affinity_mask(sr.affinity_mask());
    ar.set_time_slice(DEFAULT_TIME_SLICE);
    ar.set_priority(sr.priority());
    ar.set_flags(0);
    0
}
#[no_mangle] pub extern "C" fn xv6_rqport_sched_getattr(se: *mut sched_entity, attr: *mut sched_attr) -> c_int { sched_getattr(se, attr) }

#[no_mangle]
pub extern "C" fn sched_setattr(se: *mut sched_entity, attr: *const sched_attr) -> c_int {
    if se.is_null() || attr.is_null() { return -EINVAL; }
    let sr = se_ref(se);
    // SAFETY: `attr` is checked non-null at the top of `sched_setattr` above.
    let ar = unsafe { SchedAttrConstRef::assume(attr) };
    let major = major_priority(ar.priority());
    if major < 0 || major >= PRIORITY_MAINLEVELS as c_int {
        return -EINVAL;
    }
    let valid_mask: cpumask_t = (1u64 << NCPU) - 1;
    if ar.affinity_mask() & valid_mask == 0 {
        return -EINVAL;
    }
    let _g = sr.pi_lock_ref().scoped_lock();
    sr.set_affinity_mask(ar.affinity_mask() & valid_mask);
    sr.set_priority(ar.priority());
    0
}
#[no_mangle] pub extern "C" fn xv6_rqport_sched_setattr(se: *mut sched_entity, attr: *const sched_attr) -> c_int { sched_setattr(se, attr) }

// ---- rq_cpu_activate / get_active_cpu_mask ----
#[no_mangle]
pub extern "C" fn rq_cpu_activate(cpu: c_int) {
    if cpu >= 0 && cpu < NCPU as c_int {
        rqg_or_active_mask(1u64 << cpu);
    }
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_cpu_activate(cpu: c_int) { rq_cpu_activate(cpu) }

#[no_mangle]
pub extern "C" fn rq_get_active_cpu_mask() -> u64 {
    rqg_active_mask()
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_get_active_cpu_mask() -> u64 { rq_get_active_cpu_mask() }

// ---- rq_dump / sys_dumprq ----
#[no_mangle]
pub unsafe extern "C" fn rq_dump() {
    unsafe {
        printf(c"Run Queue Status:\n".as_ptr() as *const c_char);
        printf(c"Priority    ".as_ptr() as *const c_char);
        for cpu in 0..NCPU as c_int {
            printf(c"CPU%d        ".as_ptr() as *const c_char, cpu);
        }
        printf(c"\n".as_ptr() as *const c_char);
        printf(c"--------    ".as_ptr() as *const c_char);
        for _ in 0..NCPU {
            printf(c"--------    ".as_ptr() as *const c_char);
        }
        printf(c"\n".as_ptr() as *const c_char);

        for prio in 0..PRIORITY_MAINLEVELS as c_int {
            let mut has = false;
            for cpu in 0..NCPU as c_int {
                let r = get_rq_for_cpu(prio, cpu);
                if !is_err_or_null(r) && (*r).task_count > 0 {
                    has = true; break;
                }
            }
            if !has { continue; }
            if prio < 10 {
                printf(c"%d           ".as_ptr() as *const c_char, prio);
            } else {
                printf(c"%d          ".as_ptr() as *const c_char, prio);
            }
            for cpu in 0..NCPU as c_int {
                let r = get_rq_for_cpu(prio, cpu);
                if is_err_or_null(r) {
                    printf(c"-           ".as_ptr() as *const c_char);
                } else {
                    let count = (*r).task_count;
                    if count < 10 {
                        printf(c"%d           ".as_ptr() as *const c_char, count);
                    } else if count < 100 {
                        printf(c"%d          ".as_ptr() as *const c_char, count);
                    } else {
                        printf(c"%d         ".as_ptr() as *const c_char, count);
                    }
                }
            }
            printf(c"\n".as_ptr() as *const c_char);
        }

        printf(c"\nReady Masks:\n".as_ptr() as *const c_char);
        printf(c"            ".as_ptr() as *const c_char);
        for cpu in 0..NCPU as c_int {
            printf(c"CPU%d        ".as_ptr() as *const c_char, cpu);
        }
        printf(c"\n".as_ptr() as *const c_char);

        printf(c"Top (8b)    ".as_ptr() as *const c_char);
        for cpu in 0..NCPU as c_int {
            let pc = rq_percpu_lock_get(cpu);
            printf(c"0x%lx        ".as_ptr() as *const c_char, (*pc).ready_mask & 0xff);
            rq_percpu_put_unlock(pc);
        }
        printf(c"\n".as_ptr() as *const c_char);

        printf(c"Secondary   ".as_ptr() as *const c_char);
        for cpu in 0..NCPU as c_int {
            let pc = rq_percpu_lock_get(cpu);
            printf(c"0x%lx ".as_ptr() as *const c_char, (*pc).ready_mask_secondary);
            rq_percpu_put_unlock(pc);
        }
        printf(c"\n".as_ptr() as *const c_char);
    }
}
#[no_mangle] pub extern "C" fn xv6_rqport_rq_dump() { rq_unsafe_call!(rq_dump()) }

#[no_mangle]
pub extern "C" fn sys_dumprq() -> u64 {
    rq_unsafe_call!(rq_dump());
    0
}
#[no_mangle] pub extern "C" fn xv6_rqport_sys_dumprq() -> u64 { sys_dumprq() }
