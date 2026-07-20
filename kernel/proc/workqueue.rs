//! Pure-Rust port of `kernel/proc/workqueue.c`.
//!
//! Owns both the canonical public ABI (`workqueue_init`, `workqueue_create`,
//! `queue_work`, …) and the `xv6_wq_pub_*` aliases that the C side previously
//! exported from `proc_rust_shims.c::SECTION 13`. Internal `__wqp_*` helpers
//! have been translated as private Rust functions.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;

use crate::bindings::{
    list_node_t, slab_cache_t, spinlock_t, thread, tq_t, work_struct, workqueue,
    workqueue_callbacks,
};
use crate::list::ListNode;
use crate::proc::proc_shims::{xv6_current_thread, xv6_panic, xv6_thread_state_set};
use crate::proc::access::{
    is_err_or_null, list_node_init_raw, list_node_pop_back_raw, ptr_err_or, ListNodeRef,
    ThreadAccess, TqRef, WorkStructRef, WorkqueueRef,
};

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3D, nativized in Wave P3-N3.
//
// `WorkStruct`/`Workqueue`/`WorkqueueCallbacks` ARE the kernel-wide Rust
// definitions of `kernel/inc/proc/workqueue_types.h`'s `struct
// work_struct`/`struct workqueue`/`struct workqueue_callbacks` now:
// `build.rs` blocklists the bindgen-generated forms and injects
// `pub use crate::proc::workqueue::WorkStruct as work_struct;` (etc.)
// raw-line re-exports, so the remaining bindgen struct that embeds a
// work_struct by value (`blkdev.flush_work`) and every
// `crate::bindings::{work_struct,workqueue,workqueue_callbacks}` path
// across the crate resolves here. The `workqueue_lifecycle_cb_t`/
// `workqueue_thread_lifecycle_cb_t` fn-pointer typedefs stay
// bindgen-emitted (plain `Option<unsafe extern "C" fn(..)>` aliases whose
// `*mut workqueue` parameter now resolves to the native `Workqueue`).
//
// `func`/`fault` deliberately keep the bindgen `*mut work_struct`
// callback signature (not `*mut WorkStruct`): they are genuine C-ABI
// function pointers invoked by `invoke_work_cb` and stored by callers
// across the crate that already pass `*mut work_struct`; only this
// struct's own field *storage* is native, the callback ABI is
// untouched.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct WorkStruct {
    pub(crate) entry: ListNode,
    pub(crate) func: Option<unsafe extern "C" fn(*mut work_struct)>,
    pub(crate) fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    pub(crate) data: u64,
    pub(crate) flags: u32,
}

// P3-N3 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// work_struct { entry: list_node_t, func: Option<..>, fault:
// Option<..>, data: uint64, flags: uint32 }`) and independently
// confirmed by a riscv64-unknown-elf-gcc `_Static_assert` probe
// (rv64gc/lp64d) against `kernel/inc/proc/workqueue_types.h`:
// size 48, align 8, offsets 0/16/24/32/40. Unlike `workqueue` (which
// gets align(64) from its embedded spinlock_t), work_struct has no
// embedded lock and stays naturally (8-byte) aligned.
const _: () = {
    assert!(core::mem::size_of::<WorkStruct>() == 48, "work_struct size");
    assert!(core::mem::align_of::<WorkStruct>() == 8, "work_struct natural alignment");
    assert!(core::mem::offset_of!(WorkStruct, entry) == 0, "work_struct.entry offset");
    assert!(core::mem::offset_of!(WorkStruct, func) == 16, "work_struct.func offset");
    assert!(core::mem::offset_of!(WorkStruct, fault) == 24, "work_struct.fault offset");
    assert!(core::mem::offset_of!(WorkStruct, data) == 32, "work_struct.data offset");
    assert!(core::mem::offset_of!(WorkStruct, flags) == 40, "work_struct.flags offset");
};

/// Native mirror of `struct workqueue_callbacks`
/// (`kernel/inc/proc/workqueue_types.h`). Field types are the
/// still-bindgen `workqueue_lifecycle_cb_t`/
/// `workqueue_thread_lifecycle_cb_t` typedefs — exactly what the
/// pre-nativization bindgen struct carried.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct WorkqueueCallbacks {
    pub(crate) workqueue_ctor: crate::bindings::workqueue_lifecycle_cb_t,
    pub(crate) workqueue_dtor: crate::bindings::workqueue_lifecycle_cb_t,
    pub(crate) manager_ctor: crate::bindings::workqueue_thread_lifecycle_cb_t,
    pub(crate) manager_dtor: crate::bindings::workqueue_thread_lifecycle_cb_t,
    pub(crate) worker_ctor: crate::bindings::workqueue_thread_lifecycle_cb_t,
    pub(crate) worker_dtor: crate::bindings::workqueue_thread_lifecycle_cb_t,
}

// P3-N3 hardcoded layout proof — bindgen: six consecutive
// `Option<unsafe extern "C" fn(..)>` (8 bytes each); cross-compiler
// probe: size 48, align 8, offsets 0/8/16/24/32/40.
const _: () = {
    assert!(core::mem::size_of::<WorkqueueCallbacks>() == 48, "workqueue_callbacks size");
    assert!(core::mem::align_of::<WorkqueueCallbacks>() == 8, "workqueue_callbacks alignment");
    assert!(core::mem::offset_of!(WorkqueueCallbacks, workqueue_ctor) == 0);
    assert!(core::mem::offset_of!(WorkqueueCallbacks, workqueue_dtor) == 8);
    assert!(core::mem::offset_of!(WorkqueueCallbacks, manager_ctor) == 16);
    assert!(core::mem::offset_of!(WorkqueueCallbacks, manager_dtor) == 24);
    assert!(core::mem::offset_of!(WorkqueueCallbacks, worker_ctor) == 32);
    assert!(core::mem::offset_of!(WorkqueueCallbacks, worker_dtor) == 40);
};

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 active : 1; uint64 dtor_called : 1; }` inside
/// `struct workqueue` (bindgen's `workqueue__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `active` at bit 0 and
/// `dtor_called` at bit 1 of the (8-byte) unit's byte 0 — identical to
/// bindgen's `get(0,1)`/`get(1,1)` accessors reproduced below.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct WorkqueueFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl WorkqueueFlagBits {
    #[inline]
    pub(crate) fn active(&self) -> u64 {
        (self.bits & 0b01) as u64
    }
    #[inline]
    pub(crate) fn set_active(&mut self, val: u64) {
        self.bits = (self.bits & !0b01) | ((val as u8) & 0b01);
    }
    #[inline]
    pub(crate) fn dtor_called(&self) -> u64 {
        ((self.bits >> 1) & 0b01) as u64
    }
    #[inline]
    pub(crate) fn set_dtor_called(&mut self, val: u64) {
        self.bits = (self.bits & !0b10) | (((val as u8) << 1) & 0b10);
    }
}

const _: () = {
    assert!(core::mem::size_of::<WorkqueueFlagBits>() == 8, "workqueue anon bitfield size");
    assert!(core::mem::align_of::<WorkqueueFlagBits>() == 8, "workqueue anon bitfield alignment");
};

/// Native mirror of `struct workqueue`
/// (`kernel/inc/proc/workqueue_types.h`). The C struct picks up
/// alignment 64 from its embedded `spinlock_t lock` (whose *typedef*
/// carries `__ALIGNED_CACHELINE` — see the P3-N2 note in
/// `kernel/lock/spinlock.rs`); bindgen expressed that as
/// `#[repr(align(64))]` on `workqueue` itself, reproduced here.
#[repr(C)]
#[repr(align(64))]
#[derive(Copy, Clone)]
pub struct Workqueue {
    pub(crate) lock: spinlock_t,
    pub(crate) idle_queue: tq_t,
    pub(crate) worker_list: ListNode,
    pub(crate) manager: *mut thread,
    pub(crate) pending_works: c_int,
    pub(crate) work_list: ListNode,
    pub(crate) name: [c_char; 32],
    pub(crate) flags: WorkqueueFlagBits,
    pub(crate) nr_workers: c_int,
    pub(crate) min_active: c_int,
    pub(crate) max_active: c_int,
    pub(crate) callbacks: WorkqueueCallbacks,
}

// P3-N3 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (`pub struct workqueue { lock:
// spinlock_t, idle_queue: tq_t, worker_list: list_node_t, manager:
// *mut thread, pending_works: c_int, work_list: list_node_t, name:
// [c_char; 32], __bindgen_anon_1: workqueue__bindgen_ty_1, nr_workers:
// c_int, min_active: c_int, max_active: c_int, callbacks:
// workqueue_callbacks }`, `#[repr(C)] #[repr(align(64))]`) and
// independently confirmed by the cross-compiler `_Static_assert`
// probe: size 256, align 64, offsets
// 0/24/72/88/96/104/120/[152]/160/164/168/176 (the anonymous bitfield
// struct occupies [152,160), pinned in the probe by both neighbours).
const _: () = {
    assert!(core::mem::size_of::<Workqueue>() == 256, "workqueue size");
    assert!(core::mem::align_of::<Workqueue>() == 64, "workqueue alignment");
    assert!(core::mem::offset_of!(Workqueue, lock) == 0, "workqueue.lock offset");
    assert!(core::mem::offset_of!(Workqueue, idle_queue) == 24, "workqueue.idle_queue offset");
    assert!(core::mem::offset_of!(Workqueue, worker_list) == 72, "workqueue.worker_list offset");
    assert!(core::mem::offset_of!(Workqueue, manager) == 88, "workqueue.manager offset");
    assert!(core::mem::offset_of!(Workqueue, pending_works) == 96, "workqueue.pending_works offset");
    assert!(core::mem::offset_of!(Workqueue, work_list) == 104, "workqueue.work_list offset");
    assert!(core::mem::offset_of!(Workqueue, name) == 120, "workqueue.name offset");
    assert!(core::mem::offset_of!(Workqueue, flags) == 152, "workqueue anon bitfield offset");
    assert!(core::mem::offset_of!(Workqueue, nr_workers) == 160, "workqueue.nr_workers offset");
    assert!(core::mem::offset_of!(Workqueue, min_active) == 164, "workqueue.min_active offset");
    assert!(core::mem::offset_of!(Workqueue, max_active) == 168, "workqueue.max_active offset");
    assert!(core::mem::offset_of!(Workqueue, callbacks) == 176, "workqueue.callbacks offset");
};

// -------- mirrored constants ----------------------------------------------
const WORK_STRUCT_FLAG_RUN_ON_DRAIN: u32 = 1 << 0;
const WORK_STRUCT_FLAG_FREE_AFTER_RUN: u32 = 1 << 1;
const WORK_STRUCT_FLAG_VALID_MASK: u32 =
    WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN;
const WORK_STRUCT_DEFAULT_FLAGS: u32 = WORK_STRUCT_FLAG_RUN_ON_DRAIN;
const WORKQUEUE_DEFAULT_MAX_ACTIVE: c_int = 8;
const WORKQUEUE_DEFAULT_MIN_ACTIVE: c_int = 2;
const MAX_WORKQUEUE_ACTIVE: c_int = 64;
const WORKQUEUE_NAME_MAX: usize = 31;
const KERNEL_STACK_ORDER: c_int = 2;
const SIGKILL: c_int = 9;
const SLAB_FLAG_EMBEDDED: u64 = 1 << 0;
const EINVAL: c_int = 22;
const ENOMEM: c_int = 12;
const THREAD_FLAG_KILLED: u64 = 2;
const THREAD_INTERRUPTIBLE: c_int = 2;

// =========================================================================
// FFI declarations as `pub safe fn` — Rust 2024 form. The caller side stays
// safe; the symbol still resolves to the original C/Rust definition.
// =========================================================================
unsafe extern "C" {
    pub safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    pub safe fn strncpy(d: *mut c_char, s: *const c_char, n: usize) -> *mut c_char;
}
// P3-D3c: the spinlock primitives are genuinely `unsafe fn`s in
// `crate::lock::spinlock` now that their `#[no_mangle]` exports are gone;
// this file's original extern declarations asserted `safe fn` (usual
// FFI-facade convention). Thin wrappers preserve that safe facade for the
// unchanged call sites.
/// SAFETY: see [`crate::lock::spinlock::spin_init`]'s contract.
fn spin_init(l: *mut spinlock_t, name: *mut c_char) {
    unsafe { crate::lock::spinlock::spin_init(l, name) }
}
// P3-D3c: `proc/exit.rs`'s `exit` is a plain (safe) Rust fn now that its
// `#[no_mangle]` export is gone -- imported via its private sibling module
// path (the `crate::proc` glob would work too, but the direct path is
// unambiguous by construction).
use super::exit::exit;

// P3-D3b: `tcb_lock`/`tcb_unlock`/`kthread_create`
// (kernel/proc/thread.rs) are plain safe Rust fns now that their
// `#[no_mangle]` exports are gone; the old file-private `extern "C"`
// redeclarations (kept non-`pub` to dodge E0659 with the glob reexport
// at `crate::proc`) are replaced by direct crate-path imports of the
// real definitions. NOTE: the old `kthread_create` redeclaration had
// drifted to `(.., arg: u64, prio: c_int, order: c_int)`; the real
// signature is `(.., arg1: u64, arg2: u64, stack_order: c_int)`. Both
// call sites below pass a literal `0` for the 4th argument and
// `KERNEL_STACK_ORDER` for the 5th, so the generated calls are
// value-identical — the divergent decl was latent, not live.
// NO-STANDALONE-FN: `kthread_create` is now the associated fn
// `thread::kthread_create` (called through the `bindings::thread` type, which
// *is* `crate::proc::thread::Thread`); `tcb_lock`/`tcb_unlock` are handle
// methods on `ThreadAccess`, so the call sites construct/reuse a handle.

// P3-D3a: the slab entry points are genuinely `unsafe fn` in
// `crate::mm::slab` now that their `#[no_mangle]` exports are gone; this
// file's original extern declarations asserted `safe fn` (usual FFI
// facade) and typed the cache pointer as the bindgen `slab_cache_t`
// rather than `crate::mm::slab::SlabCache` (same layout — see
// `cffi::raw`'s identical note). Thin cast + safe-facade wrappers
// preserve both.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
    }
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    unsafe { crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}
/// SAFETY: `obj` must originate from `slab_alloc` above.
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}

// P3-D2a: the scheduler entry points (`kernel/proc/sched.rs`) and the
// thread-queue primitives (`kernel/proc/thread_queue.rs`) are ordinary
// Rust fns, reached as plain crate-path items instead of the `extern
// "C"` redeclarations that used to sit in the block above.
// NO-STANDALONE-FN: the `tq_init`/`tq_size`/`tq_wait`/`tq_wakeup`/
// `tq_wakeup_all` free-fn delegators were deleted; the sites below build a
// `TqRef` handle (imported from `access` above) and invoke the method.
use crate::proc::{
    scheduler_wakeup, scheduler_yield,
    wakeup,
};
// P3-D2b: `kill_thread` (proc/signal.rs) likewise.
use crate::proc::kill_thread;

// =========================================================================
// Wrapper-construction helpers. Each pointer constructor is null-safe.
// =========================================================================
#[inline] fn wqr<'a>(p: *mut workqueue) -> Option<WorkqueueRef<'a>> {
    WorkqueueRef::from_ptr(p)
}
#[inline] fn wsr<'a>(p: *mut work_struct) -> Option<WorkStructRef<'a>> {
    WorkStructRef::from_ptr(p)
}
#[inline] fn ta<'a>(p: *mut thread) -> Option<ThreadAccess<'a>> {
    ThreadAccess::from_ptr(p)
}
#[inline] fn lnr<'a>(p: *mut list_node_t) -> Option<ListNodeRef<'a>> {
    ListNodeRef::from_ptr(p)
}

// Callback trampolines: invoking a stored `unsafe extern "C" fn` pointer is
// inherently unsafe — encapsulated once per arity.
#[inline] fn invoke_wq_cb(cb: Option<unsafe extern "C" fn(*mut workqueue)>, wq: *mut workqueue) {
    if let Some(f) = cb { unsafe { f(wq); } }
}
#[inline] fn invoke_wq_t_cb(
    cb: Option<unsafe extern "C" fn(*mut workqueue, *mut thread)>,
    wq: *mut workqueue, t: *mut thread,
) {
    if let Some(f) = cb { unsafe { f(wq, t); } }
}
#[inline] fn invoke_work_cb(
    cb: Option<unsafe extern "C" fn(*mut work_struct)>, w: *mut work_struct,
) {
    if let Some(f) = cb { unsafe { f(w); } }
}

// =========================================================================
// Thread-flag helpers (atomic bit ops via ThreadAccess).
// =========================================================================
fn thread_killed(t: *mut thread) -> bool {
    ta(t).map(|a| a.flags_test_bit(THREAD_FLAG_KILLED)).unwrap_or(false)
}
fn thread_set_killed(t: *mut thread) {
    if let Some(a) = ta(t) { a.flags_set_bit(THREAD_FLAG_KILLED); }
}

// -------- slab cache storage ----------------------------------------------
#[repr(transparent)]
struct CacheCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: both `WQP_WORKQUEUE_CACHE` and `WQP_WORK_STRUCT_CACHE` are
// written in full by `slab_cache_init` (called once during workqueue
// subsystem init, before any `slab_alloc` on either cache) and
// otherwise only accessed through the C slab allocator's own
// internally-synchronized primitives — same pattern as
// `thread_group.rs`'s `TG_POOL`.
unsafe impl Sync for CacheCell {}
static WQP_WORKQUEUE_CACHE: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));
static WQP_WORK_STRUCT_CACHE: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));
#[inline] fn wq_cache() -> *mut slab_cache_t { WQP_WORKQUEUE_CACHE.0.get() as *mut slab_cache_t }
#[inline] fn ws_cache() -> *mut slab_cache_t { WQP_WORK_STRUCT_CACHE.0.get() as *mut slab_cache_t }

#[inline] fn work_flags_valid(flags: u32) -> bool { (flags & !WORK_STRUCT_FLAG_VALID_MASK) == 0 }

fn alloc_workqueue() -> *mut workqueue {
    let wq = slab_alloc(wq_cache()) as *mut workqueue;
    if !wq.is_null() { memset(wq as *mut c_void, 0, core::mem::size_of::<workqueue>()); }
    wq
}
fn alloc_work_struct() -> *mut work_struct {
    let w = slab_alloc(ws_cache()) as *mut work_struct;
    if !w.is_null() { memset(w as *mut c_void, 0, core::mem::size_of::<work_struct>()); }
    w
}
#[inline] fn free_ptr<T>(p: *mut T) {
    if !p.is_null() { slab_free(p as *mut c_void); }
}

// ===========================================================================
// N-METH (RUSTIFY-PROC): the per-object algorithms that used to be free
// `fn(work: *mut work_struct)` / `fn(wq: WorkqueueRef<'_>)` helpers are now
// inherent METHODS on the `WorkStructRef`/`WorkqueueRef` *handles* (defined
// in [`crate::proc::access`]; inherent impls may live in any module of the
// defining crate). The handles reach the underlying C struct through a
// `NonNull` raw pointer -- crucially, NO `&WorkStruct`/`&Workqueue`
// reference is ever formed, so the Freeze-`noalias` read-hoist hazard that
// would fire on a `&self` method of these `Copy`/`UnsafeCell`-free (=>
// `Freeze`) native structs is structurally avoided: a `work_struct` is
// embedded in a queue and processed by a worker thread across a `tq_wait`
// park, and a `workqueue`'s counter/flag fields are mutated by other harts
// across `scheduler_yield()`. Every field touch here is a raw load/store the
// optimizer may not reorder. `&self` borrows the *handle* (a pointer), never
// the pointee. The raw-`*mut`-taking `*_impl` entry points below stay thin
// null-checking delegators (the C-ABI forwarders hold raw pointers).
// ===========================================================================

// -------- work_struct per-object operations -------------------------------
impl<'a> WorkStructRef<'a> {
    /// Valid iff the flags are well-formed and at least one of
    /// `func`/`fault` is set (was `work_struct_valid`).
    fn is_valid(&self) -> bool {
        work_flags_valid(self.flags()) && (self.func().is_some() || self.fault().is_some())
    }
    /// Full initializer (`init_work_struct_ex`); panics on invalid flags.
    fn init_ex(
        &self,
        func: Option<unsafe extern "C" fn(*mut work_struct)>,
        fault: Option<unsafe extern "C" fn(*mut work_struct)>,
        data: u64,
        flags: u32,
    ) {
        if !work_flags_valid(flags) {
            xv6_panic(c"init_work_struct_ex: invalid work flags".as_ptr());
        }
        self.entry_ref().init();
        self.set_func(func);
        self.set_fault(fault);
        self.set_data(data);
        self.set_flags(flags);
    }
    /// Run the callback (or, on a drained inactive queue, the fault
    /// handler), then free the item if `FREE_AFTER_RUN` is set. After a
    /// free the handle is dangling and must not be reused (it is not).
    fn execute(&self, queue_active: bool) {
        let run = queue_active || self.flag_set(WORK_STRUCT_FLAG_RUN_ON_DRAIN);
        if run {
            if !queue_active {
                invoke_work_cb(self.fault(), self.as_ptr());
            } else {
                invoke_work_cb(self.func(), self.as_ptr());
            }
        }
        if self.flag_set(WORK_STRUCT_FLAG_FREE_AFTER_RUN) { free_ptr(self.as_ptr()); }
    }
}

// -------- workqueue per-object operations ---------------------------------
impl<'a> WorkqueueRef<'a> {
    /// Zero and initialize the embedded lock/queues/lists (was
    /// `workqueue_struct_init`).
    fn struct_init(&self) {
        memset(self.as_ptr() as *mut c_void, 0, core::mem::size_of::<workqueue>());
        list_node_init_raw(self.worker_list_ptr());
        list_node_init_raw(self.work_list_ptr());
        spin_init(self.lock_ptr(), c"workqueue_lock".as_ptr() as *mut c_char);
        if let Some(r) = TqRef::from_ptr(self.idle_queue_ptr()) {
            r.init(c"workqueue_idle".as_ptr(), self.lock_ptr());
        }
    }
    /// Push a detached work item onto the work list (was `enqueue_work`).
    fn enqueue(&self, work: WorkStructRef<'_>) {
        if !work.entry_ref().is_detached() {
            xv6_panic(c"enqueue_work: work struct is already enqueued".as_ptr());
        }
        work.entry_ref().push_front(self.work_list_ptr());
        self.inc_pending_works();
    }
    /// Pop the oldest work item, or null if the list is empty (was
    /// `dequeue_work`). Returns a raw pointer: the caller runs then frees it.
    fn dequeue(&self) -> *mut work_struct {
        let off = core::mem::offset_of!(work_struct, entry);
        let last = list_node_pop_back_raw(self.work_list_ptr());
        if last.is_null() { return ptr::null_mut(); }
        self.dec_pending_works();
        (last as *mut u8).wrapping_sub(off) as *mut work_struct
    }
    /// Latch the dtor-called flag; true exactly once (was
    /// `mark_workqueue_dtor_once`).
    fn mark_dtor_once(&self) -> bool {
        let should = !self.dtor_called();
        if should { self.set_dtor_called(1); }
        should
    }
    /// Wake every idle worker parked on the idle queue.
    fn wakeup_all_idle_workers(&self) {
        let ret = TqRef::from_ptr(self.idle_queue_ptr()).map_or(-EINVAL, |r| r.wakeup_all(0, 0));
        if ret < 0 {
            crate::kprintln!(
                "warning: failed to wake idle workers for workqueue {}",
                crate::printf::Cs(self.name_ptr()),
            );
        }
    }
    /// Spawn a worker thread bound to this queue (was `create_worker`).
    fn create_worker(&self) -> c_int {
        let worker = thread::kthread_create(
            c"worker_thread".as_ptr(),
            worker_routine as *mut c_void,
            self.as_ptr() as u64,
            0,
            KERNEL_STACK_ORDER,
        );
        if is_err_or_null(worker) { return ptr_err_or(worker, -ENOMEM); }
        let Some(wa) = ta(worker) else { return -ENOMEM; };
        wa.tcb_lock();
        wa.set_wq_ptr(self.as_ptr());
        self.inc_nr_workers();
        wa.wq_entry_ref().push_back(self.worker_list_ptr());
        wa.tcb_unlock();
        wakeup(worker);
        0
    }
    /// Spawn the manager thread bound to this queue (was `create_manager`).
    fn create_manager(&self) -> c_int {
        let manager = thread::kthread_create(
            c"manager_thread".as_ptr(),
            manager_routine as *mut c_void,
            self.as_ptr() as u64,
            0,
            KERNEL_STACK_ORDER,
        );
        if is_err_or_null(manager) { return ptr_err_or(manager, -ENOMEM); }
        let Some(ma) = ta(manager) else { return -ENOMEM; };
        ma.tcb_lock();
        ma.set_wq_ptr(self.as_ptr());
        ma.tcb_unlock();
        self.set_manager(manager);
        0
    }
    /// Wake the manager thread if one is bound (was `wakeup_manager`).
    fn wakeup_manager(&self) {
        let m = self.manager_ptr();
        if !m.is_null() { scheduler_wakeup(m); }
    }
    /// Deactivate the queue and tear down its manager (was
    /// `workqueue_kill_impl`, minus the null check now in its delegator).
    fn kill(&self) -> c_int {
        self.lock_ref().lock();
        if !self.is_active() { self.lock_ref().unlock(); return 0; }
        self.set_active(0);
        let should = self.mark_dtor_once();
        let manager = self.manager_ptr();
        self.lock_ref().unlock();
        if should { invoke_wq_cb(self.cb_workqueue_dtor(), self.as_ptr()); }
        if !manager.is_null() {
            let r = kill_thread(manager, SIGKILL);
            if r < 0 { thread_set_killed(manager); }
            scheduler_wakeup(manager);
        }
        0
    }
    /// Validate and enqueue a work item, waking the manager (was
    /// `queue_work_impl`, minus the null check now in its delegator).
    fn queue_work(&self, work_p: *mut work_struct) -> bool {
        let Some(w) = wsr(work_p) else { return false; };
        if !w.is_valid() { return false; }
        if !w.entry_ref().is_detached() { return false; }
        self.lock_ref().lock();
        if !self.is_active() { self.lock_ref().unlock(); return false; }
        self.enqueue(w);
        self.wakeup_manager();
        self.lock_ref().unlock();
        true
    }
}

// -------- exit routines ---------------------------------------------------
fn exit_worker_routine(exit_code: u64) -> ! {
    let cur_t = xv6_current_thread();
    let cur_a = ta(cur_t);
    // SAFETY: `cur_t` is the running thread (`xv6_current_thread()`), always a
    // live `*mut thread`; the handle only takes/releases its own tcb lock.
    let cur_h = unsafe { ThreadAccess::assume(cur_t) };
    cur_h.tcb_lock();
    let wq_p = cur_a.map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    cur_h.tcb_unlock();
    if !wq_p.is_null() {
        if let Some(wq) = wqr(wq_p) {
            invoke_wq_t_cb(wq.cb_worker_dtor(), wq_p, cur_t);
            wq.lock_ref().lock();
            if wq.manager_ptr() == cur_t {
                xv6_panic(c"Manager thread try to exit using worker exit routine".as_ptr());
            }
            cur_h.tcb_lock();
            if let Some(a) = cur_a {
                if !a.wq_entry_ref().is_detached() { a.wq_entry_ref().detach(); }
            }
            cur_h.tcb_unlock();
            wq.dec_nr_workers();
            if wq.nr_workers() < 0 {
                xv6_panic(c"Worker thread count is invalid\n".as_ptr());
            }
            wq.lock_ref().unlock();
        }
    } else if let Some(a) = cur_a {
        cur_h.tcb_lock();
        if !a.wq_entry_ref().is_detached() {
            xv6_panic(c"Worker thread not belong to a workqueue but attached\n".as_ptr());
        }
        cur_h.tcb_unlock();
    }
    exit(exit_code as c_int)
}
fn exit_manager_routine(exit_code: u64) -> ! {
    let cur_t = xv6_current_thread();
    let cur_a = ta(cur_t);
    // SAFETY: `cur_t` is the running thread; see `exit_worker_routine`.
    let cur_h = unsafe { ThreadAccess::assume(cur_t) };
    cur_h.tcb_lock();
    let wq_p = cur_a.map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    cur_h.tcb_unlock();
    if let Some(wq) = wqr(wq_p) {
        invoke_wq_t_cb(wq.cb_manager_dtor(), wq_p, cur_t);
        wq.lock_ref().lock();
        if wq.manager_ptr() == cur_t { wq.set_manager(ptr::null_mut()); }
        wq.lock_ref().unlock();
    }
    exit(exit_code as c_int)
}

// -------- worker / manager routines --------------------------------------
unsafe extern "C" fn worker_routine() {
    let cur_t = xv6_current_thread();
    // SAFETY: `cur_t` is the running thread; unsafe-fn body is an unsafe context.
    let cur_h = ThreadAccess::assume(cur_t);
    cur_h.tcb_lock();
    let wq_p = ta(cur_t).map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    if wq_p.is_null() { cur_h.tcb_unlock(); exit(-EINVAL); }
    cur_h.tcb_unlock();
    let Some(wq) = wqr(wq_p) else { exit(-EINVAL); };
    wq.lock_ref().lock();
    if wq.manager_ptr() == cur_t { wq.lock_ref().unlock(); exit(-EINVAL); }
    wq.lock_ref().unlock();
    invoke_wq_t_cb(wq.cb_worker_ctor(), wq_p, cur_t);
    loop {
        wq.lock_ref().lock();
        if thread_killed(cur_t) { wq.lock_ref().unlock(); exit_worker_routine(0); }
        if !wq.is_active() {
            let work = wq.dequeue();
            if work.is_null() { wq.lock_ref().unlock(); exit_worker_routine(0); }
            wq.lock_ref().unlock();
            if let Some(w) = wsr(work) { w.execute(false); }
            continue;
        }
        let mut work = wq.dequeue();
        if work.is_null() {
            xv6_thread_state_set(cur_t, THREAD_INTERRUPTIBLE);
            let _ = TqRef::from_ptr(wq.idle_queue_ptr()).map_or(-EINVAL, |r| r.wait(
                wq.lock_ptr(),
                &raw mut work as *mut *mut work_struct as *mut u64,
            ));
            if !wq.lock_ref().holding() {
                xv6_panic(c"tq_wait should return with workqueue lock held".as_ptr());
            }
            if work.is_null() { wq.lock_ref().unlock(); continue; }
        }
        wq.lock_ref().unlock();
        if let Some(w) = wsr(work) { w.execute(true); }
    }
}

unsafe extern "C" fn manager_routine() {
    let cur_t = xv6_current_thread();
    // SAFETY: `cur_t` is the running thread; unsafe-fn body is an unsafe context.
    let cur_h = ThreadAccess::assume(cur_t);
    cur_h.tcb_lock();
    let wq_p = ta(cur_t).map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    if wq_p.is_null() { cur_h.tcb_unlock(); exit(-EINVAL); }
    cur_h.tcb_unlock();
    let Some(wq) = wqr(wq_p) else { exit(-EINVAL); };
    wq.lock_ref().lock();
    wq.set_manager(cur_t);
    wq.lock_ref().unlock();
    invoke_wq_t_cb(wq.cb_manager_ctor(), wq_p, cur_t);
    wq.lock_ref().lock();
    loop {
        if thread_killed(cur_t) {
            if wq.is_active() {
                if wq.create_manager() == 0 { wq.wakeup_manager(); }
            } else {
                wq.wakeup_all_idle_workers();
            }
            wq.lock_ref().unlock();
            exit_manager_routine(0);
        }
        if !wq.is_active() {
            wq.wakeup_all_idle_workers();
            wq.lock_ref().unlock();
            exit_manager_routine(0);
        }
        if wq.nr_workers() < 0 {
            xv6_panic(c"Worker thread count is invalid\n".as_ptr());
        }
        while wq.nr_workers() < wq.min_active()
            || (wq.pending_works() > wq.nr_workers() && wq.nr_workers() < wq.max_active())
        {
            if wq.create_worker() != 0 { break; }
        }
        let idle = wq.idle_queue_ptr();
        while TqRef::from_ptr(idle).map_or(-EINVAL, |r| r.size()) != 0
            && wq.nr_workers() - TqRef::from_ptr(idle).map_or(-EINVAL, |r| r.size()) < wq.pending_works()
        {
            let p = TqRef::from_ptr(idle).map_or(core::ptr::null_mut(), |r| r.wakeup_one(0, 0));
            if is_err_or_null(p) {
                crate::kprintln!("warning: Failed to wake up idle worker");
            }
        }
        xv6_thread_state_set(cur_t, THREAD_INTERRUPTIBLE);
        wq.lock_ref().unlock();
        scheduler_yield();
        wq.lock_ref().lock();
    }
}

// =========================================================================
// Internal implementations.  Public ABI exports below all forward here.
// =========================================================================
fn init_work_struct_ex_impl(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) {
    if let Some(w) = wsr(work) { w.init_ex(func, fault, data, flags); }
}
fn create_work_struct_ex_impl(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) -> *mut work_struct {
    if !work_flags_valid(flags) { return ptr::null_mut(); }
    let w = alloc_work_struct();
    if w.is_null() { return ptr::null_mut(); }
    init_work_struct_ex_impl(w, func, fault, data, flags);
    w
}
fn free_work_struct_impl(work: *mut work_struct) { free_ptr(work); }

fn workqueue_init_impl() {
    let ret = slab_cache_init(
        wq_cache(), c"workqueue".as_ptr() as *mut c_char,
        core::mem::size_of::<workqueue>(), SLAB_FLAG_EMBEDDED,
    );
    if ret != 0 { xv6_panic(c"Failed to initialize workqueue slab cache".as_ptr()); }
    let ret = slab_cache_init(
        ws_cache(), c"work_struct".as_ptr() as *mut c_char,
        core::mem::size_of::<work_struct>(), SLAB_FLAG_EMBEDDED,
    );
    if ret != 0 { xv6_panic(c"Failed to initialize work_struct slab cache".as_ptr()); }
    crate::kprintln!("workqueue subsystem initialized");
}

fn workqueue_create_with_callbacks_impl(
    name: *const c_char,
    mut max_active: c_int,
    callbacks: *const workqueue_callbacks,
) -> *mut workqueue {
    if max_active < 0 { return ptr::null_mut(); }
    if max_active == 0 { max_active = WORKQUEUE_DEFAULT_MAX_ACTIVE; }
    else if max_active > MAX_WORKQUEUE_ACTIVE { max_active = MAX_WORKQUEUE_ACTIVE; }
    let name = if name.is_null() { c"unnamed".as_ptr() } else { name };
    let wq_p = alloc_workqueue();
    if wq_p.is_null() { return ptr::null_mut(); }
    let Some(wq) = wqr(wq_p) else { return ptr::null_mut(); };
    wq.struct_init();
    strncpy(wq.name_buf_ptr(), name, WORKQUEUE_NAME_MAX);
    wq.copy_callbacks(callbacks);
    wq.set_max_active(max_active);
    wq.set_min_active(if max_active < WORKQUEUE_DEFAULT_MIN_ACTIVE {
        WORKQUEUE_DEFAULT_MIN_ACTIVE
    } else { max_active });
    wq.set_nr_workers(0);
    wq.set_active(1);
    invoke_wq_cb(wq.cb_workqueue_ctor(), wq_p);
    wq.lock_ref().lock();
    if wq.create_manager() != 0 {
        let should = wq.mark_dtor_once();
        wq.lock_ref().unlock();
        if should { invoke_wq_cb(wq.cb_workqueue_dtor(), wq_p); }
        free_ptr(wq_p);
        return ptr::null_mut();
    }
    wq.wakeup_manager();
    wq.lock_ref().unlock();
    wq_p
}

// Thin null-checking delegators: the C-ABI forwarders below hold raw
// `*mut workqueue`/`*mut work_struct`; each wraps in a handle and calls the
// `WorkqueueRef` method that carries the real logic.
fn workqueue_kill_impl(wq_p: *mut workqueue) -> c_int {
    wqr(wq_p).map(|wq| wq.kill()).unwrap_or(-EINVAL)
}

fn queue_work_impl(wq_p: *mut workqueue, work_p: *mut work_struct) -> bool {
    wqr(wq_p).map(|wq| wq.queue_work(work_p)).unwrap_or(false)
}

// =========================================================================
// Public ABI: canonical names AND xv6_wq_pub_* aliases.
//
// P3-1B mesh sweep: every `xv6_wq_pub_*` alias below is genuinely dead --
// zero callers anywhere in the tree (grep-verified). They existed only to
// mirror the canonical names for the long-deleted `proc_rust_shims.c`'s
// C-ABI callers; kept (rather than deleted) per this wave's "preserve
// still-plausible public API, don't silently delete" convention (see
// `goldfish_rtc_init`'s precedent). RUSTIFY-PROC visibility sweep: their
// `pub(crate)` was the widest surface with zero justification, so they are
// now module-PRIVATE (`extern "C" fn`, no `pub`); the file's blanket
// `#![allow(dead_code)]` covers them.
// =========================================================================

extern "C" fn xv6_wq_pub_init_work_struct_ex(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) { init_work_struct_ex_impl(work, func, fault, data, flags) }

extern "C" fn xv6_wq_pub_init_work_struct(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
) { init_work_struct_ex_impl(work, func, None, data, WORK_STRUCT_DEFAULT_FLAGS) }

extern "C" fn xv6_wq_pub_init_work_struct_flags(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) { init_work_struct_ex_impl(work, func, None, data, flags) }

extern "C" fn xv6_wq_pub_create_work_struct_ex(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) -> *mut work_struct { create_work_struct_ex_impl(func, fault, data, flags) }

extern "C" fn xv6_wq_pub_create_work_struct(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
) -> *mut work_struct {
    create_work_struct_ex_impl(func, None, data, WORK_STRUCT_DEFAULT_FLAGS)
}

extern "C" fn xv6_wq_pub_create_work_struct_flags(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) -> *mut work_struct { create_work_struct_ex_impl(func, None, data, flags) }

extern "C" fn xv6_wq_pub_free_work_struct(work: *mut work_struct) {
    free_work_struct_impl(work)
}

extern "C" fn xv6_wq_pub_workqueue_init() { workqueue_init_impl() }

extern "C" fn xv6_wq_pub_workqueue_create_with_callbacks(
    name: *const c_char,
    max_active: c_int,
    callbacks: *const workqueue_callbacks,
) -> *mut workqueue { workqueue_create_with_callbacks_impl(name, max_active, callbacks) }

extern "C" fn xv6_wq_pub_workqueue_create(
    name: *const c_char,
    max_active: c_int,
) -> *mut workqueue { workqueue_create_with_callbacks_impl(name, max_active, ptr::null()) }

extern "C" fn xv6_wq_pub_workqueue_kill(wq: *mut workqueue) -> c_int {
    workqueue_kill_impl(wq)
}

extern "C" fn xv6_wq_pub_queue_work(
    wq: *mut workqueue, work: *mut work_struct,
) -> bool { queue_work_impl(wq, work) }

extern "C" fn xv6_wq_pub_workqueue_runtime_smoke_test() {}

// ============== canonical ABI names ====================
// RUSTIFY-PROC visibility sweep (out-of-`crate::proc` callers grep-verified):
//   pub(crate) kept  -> reached from mm/pcache, timer/sched_timer, vfs/fs,
//                       vfs/vfs_syscall, or start_kernel;
//   pub(super)       -> sole callers in `crate::proc::workqueue_test`;
//   private          -> zero callers anywhere (dead flavour variants).
pub(crate) extern "C" fn workqueue_init() { workqueue_init_impl() }        // start_kernel
pub(crate) extern "C" fn workqueue_runtime_smoke_test() {}                 // start_kernel
pub(crate) fn workqueue_create(name: *const c_char, max_active: c_int) -> *mut workqueue {
    workqueue_create_with_callbacks_impl(name, max_active, ptr::null())    // pcache/timer/vfs/start_kernel
}
pub(super) extern "C" fn workqueue_create_with_callbacks(                  // workqueue_test only
    name: *const c_char, max_active: c_int, callbacks: *const workqueue_callbacks,
) -> *mut workqueue { workqueue_create_with_callbacks_impl(name, max_active, callbacks) }
pub(super) extern "C" fn workqueue_kill(wq: *mut workqueue) -> c_int { workqueue_kill_impl(wq) } // workqueue_test only
pub(crate) fn queue_work(wq: *mut workqueue, work: *mut work_struct) -> bool {
    queue_work_impl(wq, work)                                             // pcache/timer/vfs
}
pub(crate) fn init_work_struct(                                           // pcache/timer
    work: *mut work_struct, func: Option<unsafe extern "C" fn(*mut work_struct)>, data: u64,
) { init_work_struct_ex_impl(work, func, None, data, WORK_STRUCT_DEFAULT_FLAGS) }
extern "C" fn init_work_struct_flags(                                     // dead
    work: *mut work_struct, func: Option<unsafe extern "C" fn(*mut work_struct)>, data: u64, flags: u32,
) { init_work_struct_ex_impl(work, func, None, data, flags) }
extern "C" fn init_work_struct_ex(                                        // dead
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64, flags: u32,
) { init_work_struct_ex_impl(work, func, fault, data, flags) }
pub(crate) fn create_work_struct(                                        // vfs
    func: Option<unsafe extern "C" fn(*mut work_struct)>, data: u64,
) -> *mut work_struct {
    create_work_struct_ex_impl(func, None, data, WORK_STRUCT_DEFAULT_FLAGS)
}
extern "C" fn create_work_struct_flags(                                  // dead
    func: Option<unsafe extern "C" fn(*mut work_struct)>, data: u64, flags: u32,
) -> *mut work_struct { create_work_struct_ex_impl(func, None, data, flags) }
pub(super) extern "C" fn create_work_struct_ex(                          // workqueue_test only
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64, flags: u32,
) -> *mut work_struct { create_work_struct_ex_impl(func, fault, data, flags) }
pub(crate) fn free_work_struct(work: *mut work_struct) { free_work_struct_impl(work) } // vfs

// Suppress unused-ref-helper warning (kept for future consumers).
#[allow(dead_code)] fn _silence_helpers() { let _ = lnr as fn(_) -> _; }
