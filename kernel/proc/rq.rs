//! Pure-Rust port of `kernel/proc/rq.c` (SECTION 19 of the former
//! `proc_rust_shims.c`).  Owns the canonical run-queue entry points,
//! called directly by sibling Rust modules via `crate::proc::*` paths
//! (the `xv6_rqport_*` C-ABI alias layer that used to front these was
//! collapsed in the P3-1B2 sweep; the `#[no_mangle] extern "C"` export
//! surface itself was dismantled in P3-D2a).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int};
use core::mem::{size_of, MaybeUninit};
use core::ptr;
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};

use crate::bindings::{cpu_local, rq, rq_percpu, sched_attr, sched_class, sched_entity, thread};
use crate::machine::{cpuid, intr_get, intr_off, PreemptGuard};
use crate::proc::access::{
    is_err_or_null,
    RqPercpuRef, RqRef, SchedAttrConstRef, SchedAttrRef, SchedClassRef, SchedEntityRef,
    ThreadAccess, sched_entity_llist_push_head,
};
use crate::kstd::{result_to_errptr, Errno, KResult};
use crate::proc::proc_shims::{xv6_current_thread, xv6_panic};
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

// P3-D3c: the per-CPU array is `ipi.rs`'s `pub(crate) static mut cpus:
// CpuLocalArray` (a `#[repr(C)]` wrapper whose sole field sits at offset
// 0), reached by crate path instead of this file's old locally-typed
// `extern "C"` redeclaration; `cpu_local_ptr` below casts the wrapper's
// base address to `*mut cpu_local` exactly as before.
use crate::ipi::cpus;

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

// ===========================================================================
// Native run-queue types — P3-N7 nativization (user directive: remove
// the C-compatible interfaces). `LoadWeight` is the canonical native
// definition of `kernel/inc/proc/rq_types.h`'s `struct load_weight`:
// `build.rs` blocklists the bindgen emission and re-exports it as
// `crate::bindings::load_weight` (facade `pub use`, N2 pattern). It has
// zero field consumers anywhere in the crate (grep-verified — it was
// emitted only because the allowlist names it); nativized for family
// completeness.
//
// DERIVE DECISION (P3-N7): Copy + Clone, faithfully reproducing the
// pre-nativization bindgen emission (two plain u32s).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen form + cross-compiler `_Static_assert` probe (toolchain gcc,
// rv64gc/lp64d — scratchpad p3n7_static_assert_probe.c); both agree.
// ===========================================================================

/// Native `struct load_weight` (`kernel/inc/proc/rq_types.h`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct LoadWeight {
    pub weight: u32,
    pub inv_weight: u32,
}

const _: () = {
    assert!(size_of::<LoadWeight>() == 8, "load_weight size");
    assert!(core::mem::align_of::<LoadWeight>() == 4, "load_weight alignment");
    assert!(core::mem::offset_of!(LoadWeight, weight) == 0, "load_weight.weight offset");
    assert!(core::mem::offset_of!(LoadWeight, inv_weight) == 4, "load_weight.inv_weight offset");
};

// ===========================================================================
// `Rq`/`RqPercpu`/`SchedEntity` — the scheduler hot core, nativized in
// the same P3-N7 wave (canonical definitions of `kernel/inc/proc/
// rq_types.h`'s `struct rq`/`struct rq_percpu`/`struct sched_entity`;
// `build.rs` blocklists the bindgen emissions and re-exports these
// under the C names, facade `pub use`, N2 pattern).
// `kernel/proc/cffi.rs`'s layout-pinned mirrors (`Rq`/`SchedEntity`)
// are promoted to re-exports of these definitions.
//
// DERIVE DECISIONS (P3-N7):
//  * `Rq`, `RqPercpu`: Copy + Clone, faithfully reproducing the
//    pre-nativization bindgen emissions. (`rq_percpu` kept its derives
//    through P3-N2 because its `rq_lock` member is *typedef*-spelled
//    `spinlock_t` in the header, which matched the `NativeTypeCallbacks`
//    entry; plain pointers/integers otherwise.)
//  * `SchedEntity` (and its bindgen union shell): no derives — the
//    bindgen emission had (silently, via the P3-N2 struct-X quirk on
//    its tag-spelled `struct spinlock`... members' Copy queries) lost
//    Copy/Clone; N6 flagged it for a deliberate decision here. First
//    principles agree with the established emission: a `sched_entity`
//    is a per-thread intrusive object (live rb/list links, an owned
//    `pi_lock`, the thread's `context` switch frame) whose bitwise
//    duplication would corrupt run-queue invariants (N1/N2 `tnode`
//    NONCOPY precedent), and no consumer `=`-copies or
//    literal-constructs one (grep-verified: it lives embedded in the
//    thread's kernel-stack head, zeroed + `sched_entity_init`ed in
//    place).
//
// The leading anonymous C union `{ struct rb_node rb_entry;
// list_node_t list_entry; }` — which bindgen has emitted as the
// degraded `__BindgenUnionField` blob shell `sched_entity__bindgen_
// ty_1` (24/8) since P3-N1 blocklisted its arm types — is a real Rust
// union here (`SchedEntityLink`, N2's `tnode` precedent), proven
// blob-identical by the asserts below.
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + cross-compiler `_Static_assert` probe (toolchain
// gcc, rv64gc/lp64d — scratchpad p3n7_static_assert_probe.c); both
// agree on every value asserted below — including the two
// cacheline-typedef `spinlock_t` placements (`pi_lock` @64, `rq_lock`
// @576), where gcc's layout matches the offsets bindgen emitted via
// its explicit `__bindgen_padding_*` fields (reproduced here as the
// `_pad*` members). No pipe-style divergence.
// ===========================================================================

use crate::bindings::{context, list_node_t, rb_node, spinlock_t};

/// Native `struct rq` (`kernel/inc/proc/rq_types.h`) — one run queue
/// (per scheduling class, per CPU).
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct Rq {
    pub sched_class: *mut sched_class,
    pub class_id: c_int,
    pub task_count: c_int,
    pub cpu_id: c_int,
}

/// Native `struct rq_percpu` (`kernel/inc/proc/rq_types.h`) — per-CPU
/// run-queue state, cacheline-aligned against false sharing.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct RqPercpu {
    pub rqs: [*mut Rq; PRIORITY_MAINLEVELS],
    pub ready_mask: u64,
    pub ready_mask_secondary: u64,
    // Explicit padding reproduction: the cacheline-aligned `spinlock_t`
    // typedef places `rq_lock` at 576 (bindgen emitted
    // `__bindgen_padding_0: [u64; 6]` because its emitted spinlock
    // facade is align-8).
    _pad0: [u64; 6],
    pub rq_lock: spinlock_t,
    pub wake_list_head: *mut SchedEntity,
    pub current_se: *mut SchedEntity,
}

/// Real Rust union for `struct sched_entity`'s leading anonymous
/// union `{ struct rb_node rb_entry; list_node_t list_entry; }`
/// (bindgen shell: `sched_entity__bindgen_ty_1`, a 24/8
/// `__BindgenUnionField` blob).
#[repr(C)]
#[derive(Copy, Clone)]
pub union SchedEntityLink {
    pub rb_entry: rb_node,
    pub list_entry: list_node_t,
}

/// Native `struct sched_entity` (`kernel/inc/proc/rq_types.h`) — a
/// thread's scheduling identity, embedded at the head of its kernel
/// stack.
#[repr(C, align(64))]
pub struct SchedEntity {
    /// The leading anonymous union (bindgen: `__bindgen_anon_1`).
    pub link: SchedEntityLink,
    pub rq: *mut Rq,
    pub priority: c_int,
    pub thread: *mut thread,
    pub sched_class: *mut sched_class,
    // Explicit padding reproduction (bindgen `__bindgen_padding_0`):
    // the cacheline-aligned `spinlock_t` typedef places `pi_lock` at 64.
    _pad0: u64,
    pub pi_lock: spinlock_t,
    pub on_rq: c_int,
    pub on_cpu: c_int,
    pub cpu_id: c_int,
    pub wake_next: *mut SchedEntity,
    pub affinity_mask: cpumask_t,
    pub start_time: u64,
    pub exec_start: u64,
    pub exec_end: u64,
    // Explicit padding reproduction (bindgen `__bindgen_padding_1`);
    // `context`'s own align(64) lands it at 192 either way.
    _pad1: u64,
    pub context: context,
}

impl SchedEntity {
    /// Pointer to the embedded `list_entry` view of the head union
    /// (kept for `sched_fifo.rs`'s LIST_FIRST_NODE-style casts; the
    /// union is the struct's first member, so this is offset 0).
    #[inline]
    pub fn list_entry_ptr(se: *mut SchedEntity) -> *mut list_node_t {
        se.cast::<list_node_t>()
    }
}

// P3-N7 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc `_Static_assert`
// probe (see the block note above).
const _: () = {
    assert!(size_of::<Rq>() == 64, "rq size");
    assert!(core::mem::align_of::<Rq>() == 64, "rq alignment");
    assert!(core::mem::offset_of!(Rq, sched_class) == 0, "rq.sched_class offset");
    assert!(core::mem::offset_of!(Rq, class_id) == 8, "rq.class_id offset");
    assert!(core::mem::offset_of!(Rq, task_count) == 12, "rq.task_count offset");
    assert!(core::mem::offset_of!(Rq, cpu_id) == 16, "rq.cpu_id offset");

    assert!(size_of::<RqPercpu>() == 640, "rq_percpu size");
    assert!(core::mem::align_of::<RqPercpu>() == 64, "rq_percpu alignment");
    assert!(core::mem::offset_of!(RqPercpu, rqs) == 0, "rq_percpu.rqs offset");
    assert!(core::mem::offset_of!(RqPercpu, ready_mask) == 512, "rq_percpu.ready_mask offset");
    assert!(
        core::mem::offset_of!(RqPercpu, ready_mask_secondary) == 520,
        "rq_percpu.ready_mask_secondary offset"
    );
    assert!(core::mem::offset_of!(RqPercpu, rq_lock) == 576, "rq_percpu.rq_lock offset");
    assert!(
        core::mem::offset_of!(RqPercpu, wake_list_head) == 600,
        "rq_percpu.wake_list_head offset"
    );
    assert!(core::mem::offset_of!(RqPercpu, current_se) == 608, "rq_percpu.current_se offset");

    assert!(size_of::<SchedEntityLink>() == 24, "sched_entity link-union size");
    assert!(core::mem::align_of::<SchedEntityLink>() == 8, "sched_entity link-union alignment");

    assert!(size_of::<SchedEntity>() == 320, "sched_entity size");
    assert!(core::mem::align_of::<SchedEntity>() == 64, "sched_entity alignment");
    assert!(core::mem::offset_of!(SchedEntity, link) == 0, "sched_entity anonymous-union offset");
    assert!(core::mem::offset_of!(SchedEntity, rq) == 24, "sched_entity.rq offset");
    assert!(core::mem::offset_of!(SchedEntity, priority) == 32, "sched_entity.priority offset");
    assert!(core::mem::offset_of!(SchedEntity, thread) == 40, "sched_entity.thread offset");
    assert!(
        core::mem::offset_of!(SchedEntity, sched_class) == 48,
        "sched_entity.sched_class offset"
    );
    assert!(core::mem::offset_of!(SchedEntity, pi_lock) == 64, "sched_entity.pi_lock offset");
    assert!(core::mem::offset_of!(SchedEntity, on_rq) == 88, "sched_entity.on_rq offset");
    assert!(core::mem::offset_of!(SchedEntity, on_cpu) == 92, "sched_entity.on_cpu offset");
    assert!(core::mem::offset_of!(SchedEntity, cpu_id) == 96, "sched_entity.cpu_id offset");
    assert!(core::mem::offset_of!(SchedEntity, wake_next) == 104, "sched_entity.wake_next offset");
    assert!(
        core::mem::offset_of!(SchedEntity, affinity_mask) == 112,
        "sched_entity.affinity_mask offset"
    );
    assert!(
        core::mem::offset_of!(SchedEntity, start_time) == 120,
        "sched_entity.start_time offset"
    );
    assert!(
        core::mem::offset_of!(SchedEntity, exec_start) == 128,
        "sched_entity.exec_start offset"
    );
    assert!(core::mem::offset_of!(SchedEntity, exec_end) == 136, "sched_entity.exec_end offset");
    assert!(core::mem::offset_of!(SchedEntity, context) == 192, "sched_entity.context offset");
};

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
    // `rq_select_task_rq` reads it from any CPU with no
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
// run strictly before `rq_cpu_activate(cpuid())`. Readers on
// *other* CPUs gate a cross-CPU, non-atomic read of exactly that state on
// this bit: `rq_cpu_is_idle` (the reader this analysis was written for;
// deleted as dead code in P3-D2a, orderings deliberately retained)
// returned early ("treat as idle") unless the
// mask bit was set, and only then read `CpuLocalRef::idle_thread_ptr()`
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
    // Address-of only (no dereference): `&raw mut` on a (non-extern)
    // `static mut` is a safe operation.
    (&raw mut cpus).cast::<cpu_local>().wrapping_add(cpu_id as usize)
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
// Crate-internal entry points (P3-D2a: the former `#[no_mangle] pub
// extern "C"` export surface is dismantled) -- called directly
// cross-module via `crate::proc::*` paths.
// =========================================================================

// ---- rq_set_ready / rq_clear_ready ----
pub(crate) fn rq_set_ready(cls_id: c_int, cpu_id: c_int) {
    let pc = rqpc_ref(cpu_id);
    let top_mask = 1u64 << (cls_id >> 3);
    let secondary_mask = 1u64 << cls_id;
    pc.or_ready_mask(top_mask);
    pc.or_ready_mask_secondary(secondary_mask);
}

pub(crate) fn rq_clear_ready(cls_id: c_int, cpu_id: c_int) {
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

// ---- get_rq_for_cpu ----
fn get_rq_for_cpu_inner(cls_id: c_int, cpu_id: c_int) -> KResult<*mut rq> {
    if cls_id < 0 || cls_id >= PRIORITY_MAINLEVELS as c_int {
        return Err(Errno::Inval);
    }
    if cpu_id < 0 || cpu_id >= NCPU as c_int {
        return Err(Errno::Inval);
    }
    Ok(get_rq(cls_id, cpu_id))
}

pub(crate) fn get_rq_for_cpu(cls_id: c_int, cpu_id: c_int) -> *mut rq {
    result_to_errptr(get_rq_for_cpu_inner(cls_id, cpu_id))
}

// ---- pick_next_rq ----
pub(crate) fn pick_next_rq() -> *mut rq {
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

// ---- rq_global_init ----
pub(crate) fn rq_global_init() {
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

// ---- rq_init ----
pub(crate) unsafe fn rq_init(r: *mut rq) {
    kassert!(!r.is_null(), "rq_init: rq is NULL");
    unsafe {
        ptr::write_bytes(r as *mut u8, 0, size_of::<rq>());
    }
    rq_ref(r).set_task_count(0);
}

// ---- rq_register ----
pub(crate) fn rq_register(r: *mut rq, cls_id: c_int, cpu_id: c_int) {
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

// ---- sched_entity_init ----
pub(crate) fn sched_entity_init(se: *mut sched_entity, p: *mut thread) {
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

// ---- sched_class_register ----
pub(crate) fn sched_class_register(id: c_int, cls: *mut sched_class) {
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

// ---- rq_lock / unlock / trylock / current variants ----
pub(crate) fn rq_lock(cpu_id: c_int) {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_lock: invalid cpu_id");
    rqpc_ref(cpu_id).lock_ref().lock();
}

pub(crate) fn rq_trylock(cpu_id: c_int) -> c_int {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_trylock: invalid cpu_id");
    rqpc_ref(cpu_id).lock_ref().trylock()
}

pub(crate) fn rq_unlock(cpu_id: c_int) {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_unlock: invalid cpu_id");
    kassert!(rq_lock_held(cpu_id), "rq_unlock: lock not held");
    rqpc_ref(cpu_id).lock_ref().unlock();
}

pub(crate) fn rq_lock_irqsave(cpu_id: c_int) -> c_int {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_lock_irqsave: invalid cpu_id");
    rqpc_ref(cpu_id).lock_ref().lock_irqsave()
}

pub(crate) fn rq_unlock_irqrestore(cpu_id: c_int, state: c_int) {
    kassert!(cpu_id >= 0 && cpu_id < NCPU as c_int, "rq_unlock_irqrestore: invalid cpu_id");
    kassert!(rq_lock_held(cpu_id), "rq_unlock_irqrestore: lock not held");
    rqpc_ref(cpu_id).lock_ref().unlock_irqrestore(state);
}

pub(crate) fn rq_lock_current_irqsave() -> c_int {
    let intr_state = intr_get() as c_int;
    intr_off();
    rq_lock_irqsave(cpuid());
    intr_state
}

pub(crate) fn rq_unlock_current_irqrestore(state: c_int) {
    rq_unlock_irqrestore(cpuid(), state)
}

pub(crate) fn rq_trylock_two(c1: c_int, c2: c_int) -> c_int {
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

pub(crate) fn rq_unlock_two(c1: c_int, c2: c_int) {
    kassert!(c1 >= 0 && c1 < NCPU as c_int, "rq_unlock_two: invalid c1");
    kassert!(c2 >= 0 && c2 < NCPU as c_int, "rq_unlock_two: invalid c2");
    if c1 < c2 { rq_unlock(c2); rq_unlock(c1); }
    else if c2 < c1 { rq_unlock(c1); rq_unlock(c2); }
    else { rq_unlock(c1); }
}

pub(crate) fn rq_lock_current() {
    let g = PreemptGuard::new();
    rq_lock(g.cpuid() as c_int);
}

pub(crate) fn rq_unlock_current() {
    rq_unlock(cpuid())
}

pub(crate) fn rq_holding_current() -> c_int {
    // SAFETY: `rqpc_current()` indexes into statically-allocated, always-initialized
    // per-CPU storage; never null.
    unsafe { RqPercpuRef::assume(rqpc_current()) }.lock_ref().holding() as c_int
}

// ---- rq_percpu_lock_get / put_unlock ----
pub(crate) fn rq_percpu_lock_get(cpu_id: c_int) -> *mut rq_percpu {
    if cpu_id < 0 || cpu_id >= NCPU as c_int { return ptr::null_mut(); }
    let pc = rqpc(cpu_id);
    rqpc_ref(cpu_id).lock_ref().lock();
    pc
}

pub(crate) fn rq_percpu_put_unlock(pc: *mut rq_percpu) {
    if pc.is_null() { return; }
    // SAFETY: `pc` is checked non-null immediately above.
    unsafe { RqPercpuRef::assume(pc) }.lock_ref().unlock();
}

// ---- rq_select_task_rq ----
fn rq_select_task_rq_inner(se: *mut sched_entity, cpumask: cpumask_t) -> KResult<*mut rq> {
    if se.is_null() { return Err(Errno::Inval); }
    let sr = se_ref(se);
    let major_prio = sr.priority() >> PRIORITY_MAINLEVEL_SHIFT;
    if major_prio < 0 || major_prio >= PRIORITY_MAINLEVELS as c_int {
        return Err(Errno::Inval);
    }
    let cls = sched_class_of(major_prio);
    if cls.is_null() { return Err(Errno::Inval); }

    let active = rqg_active_mask();
    let mut effective_mask = cpumask & active;
    if effective_mask == 0 { effective_mask = active; }

    // SAFETY: `cls` is checked non-null immediately above (`if cls.is_null() { return
    // Err(Errno::Inval); }`).
    if let Some(selected) = unsafe { SchedClassRef::assume(cls) }
        .select_task_rq(sr.rq_ptr(), se, effective_mask)
    {
        return Ok(selected);
    }

    let cur_cpu = cpuid();
    if effective_mask & (1u64 << cur_cpu) != 0 {
        let r = get_rq_for_cpu(major_prio, cur_cpu);
        if !is_err_or_null(r) { return Ok(r); }
    }
    for cpu in 0..NCPU as c_int {
        if effective_mask & (1u64 << cpu) != 0 {
            let r = get_rq_for_cpu(major_prio, cpu);
            if !is_err_or_null(r) { return Ok(r); }
        }
    }
    Ok(ptr::null_mut())
}

pub(crate) fn rq_select_task_rq(se: *mut sched_entity, cpumask: cpumask_t) -> *mut rq {
    result_to_errptr(rq_select_task_rq_inner(se, cpumask))
}

// ---- enqueue / dequeue / pick / put_prev / set_next ----
pub(crate) fn rq_enqueue_task(r: *mut rq, se: *mut sched_entity) {
    let rr = rq_ref(r);
    let sr = se_ref(se);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_enqueue_task: rq lock not held");
    if !sr.rq_ptr().is_null() {
        let p = sr.thread_ptr();
        let pt = ThreadAccess::from_ptr(p);
        let pn = pt.map_or(c"NULL".as_ptr(), |t| t.name_ptr());
        let se_rq = sr.rq_ptr();
        crate::kprintln!("rq_enqueue_task BUG: se->rq={} (cpu={}), target rq={} (cpu={})",
            crate::printf::Ptr(se_rq as u64), if se_rq.is_null() { -1 } else { rq_ref(se_rq).cpu_id() }, crate::printf::Ptr(r as u64), rr.cpu_id());
        crate::kprintln!("  thread={} pid={} state={} on_rq={} on_cpu={} se_cpu={}",
            crate::printf::Cs(pn), pt.map_or(-1, |t| t.pid()), if p.is_null() { -1 } else { thread_state_get(p) },
            sr.on_rq_plain(), sr.on_cpu_plain(), sr.cpu_id());
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

pub(crate) fn rq_dequeue_task(r: *mut rq, se: *mut sched_entity) {
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

pub(crate) fn rq_pick_next_task(r: *mut rq) -> *mut sched_entity {
    let rr = rq_ref(r);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_pick_next_task: rq lock not held");
    rr.pick_next_task()
}

pub(crate) fn rq_put_prev_task(se: *mut sched_entity) {
    let sr = se_ref(se);
    let r = sr.rq_ptr();
    kassert!(!r.is_null(), "rq_put_prev_task: se->rq NULL");
    let rr = rq_ref(r);
    kassert!(rq_lock_held(rr.cpu_id()), "rq_put_prev_task: rq lock not held");
    kassert!(rr.task_count() > 0, "rq_put_prev_task: task_count == 0");
    kassert!(sr.sched_class_ptr() == rr.sched_class_ptr(), "rq_put_prev_task: sched_class mismatch");
    rr.put_prev_task(se);
}

pub(crate) fn rq_set_next_task(se: *mut sched_entity) {
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

// ---- rq_task_fork / dead ----
pub(crate) fn rq_task_fork(se: *mut sched_entity) {
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

pub(crate) fn rq_task_dead(se: *mut sched_entity) {
    let sr = se_ref(se);
    if !sr.rq_ptr().is_null() && !sr.sched_class_ptr().is_null() {
        sr.call_sched_class_task_dead();
    }
    if !sr.rq_ptr().is_null() {
        rq_dequeue_task(sr.rq_ptr(), se);
    }
    sr.set_sched_class(ptr::null_mut());
}

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

pub(crate) fn rq_add_wake_list(cpu_id: c_int, se: *mut sched_entity) -> c_int {
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

pub(crate) unsafe fn rq_flush_wake_list(cpu_id: c_int) {
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

// ---- sched_attr_init / getattr / setattr ----
pub(crate) fn sched_attr_init(attr: *mut sched_attr) {
    if attr.is_null() { return; }
    // SAFETY: `attr` is checked non-null immediately above.
    let ar = unsafe { SchedAttrRef::assume(attr) };
    ar.set_size(size_of::<sched_attr>() as u32);
    ar.set_affinity_mask((1u64 << NCPU) - 1);
    ar.set_time_slice(DEFAULT_TIME_SLICE);
    ar.set_priority(DEFAULT_PRIORITY);
    ar.set_flags(0);
}

pub(crate) fn sched_getattr(se: *mut sched_entity, attr: *mut sched_attr) -> c_int {
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

pub(crate) fn sched_setattr(se: *mut sched_entity, attr: *const sched_attr) -> c_int {
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

// ---- rq_cpu_activate ----
pub(crate) fn rq_cpu_activate(cpu: c_int) {
    if cpu >= 0 && cpu < NCPU as c_int {
        rqg_or_active_mask(1u64 << cpu);
    }
}

// ---- rq_dump / sys_dumprq ----
pub(crate) unsafe fn rq_dump() {
    unsafe {
        crate::kprintln!("Run Queue Status:");
        crate::kprint!("Priority    ");
        for cpu in 0..NCPU as c_int {
            crate::kprint!("CPU{}        ", cpu);
        }
        crate::kprintln!();
        crate::kprint!("--------    ");
        for _ in 0..NCPU {
            crate::kprint!("--------    ");
        }
        crate::kprintln!();

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
                crate::kprint!("{}           ", prio);
            } else {
                crate::kprint!("{}          ", prio);
            }
            for cpu in 0..NCPU as c_int {
                let r = get_rq_for_cpu(prio, cpu);
                if is_err_or_null(r) {
                    crate::kprint!("-           ");
                } else {
                    let count = (*r).task_count;
                    if count < 10 {
                        crate::kprint!("{}           ", count);
                    } else if count < 100 {
                        crate::kprint!("{}          ", count);
                    } else {
                        crate::kprint!("{}         ", count);
                    }
                }
            }
            crate::kprintln!();
        }

        crate::kprintln!("\nReady Masks:");
        crate::kprint!("            ");
        for cpu in 0..NCPU as c_int {
            crate::kprint!("CPU{}        ", cpu);
        }
        crate::kprintln!();

        crate::kprint!("Top (8b)    ");
        for cpu in 0..NCPU as c_int {
            let pc = rq_percpu_lock_get(cpu);
            crate::kprint!("0x{:x}        ", ((*pc).ready_mask & 0xff) as u64);
            rq_percpu_put_unlock(pc);
        }
        crate::kprintln!();

        crate::kprint!("Secondary   ");
        for cpu in 0..NCPU as c_int {
            let pc = rq_percpu_lock_get(cpu);
            crate::kprint!("0x{:x} ", (*pc).ready_mask_secondary as u64);
            rq_percpu_put_unlock(pc);
        }
        crate::kprintln!();
    }
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_dumprq() -> u64 {
    rq_unsafe_call!(rq_dump());
    0
}
