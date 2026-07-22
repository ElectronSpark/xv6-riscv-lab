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

use crate::bindings::{slab_cache_t, spinlock_t, thread, tq_t, work_struct, workqueue};
use crate::list::ListNode;
use crate::proc::proc_shims::{xv6_current_thread, xv6_panic, xv6_thread_state_set};
use crate::proc::access::{
    is_err_or_null, list_node_init_raw, list_node_pop_back_raw, ptr_err_or,
    ThreadAccess, TqRef, WorkStructRef, WorkqueueRef,
};

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3D, nativized in Wave P3-N3.
//
// `WorkStruct`/`Workqueue` ARE the kernel-wide Rust definitions of
// `kernel/inc/proc/workqueue_types.h`'s `struct work_struct`/`struct
// workqueue` now (the C header's `struct workqueue_callbacks` has no
// native counterpart any more -- see the TRAIT-OPS note just below):
// `build.rs` blocklists the bindgen-generated forms and injects
// `pub use crate::proc::workqueue::WorkStruct as work_struct;` (etc.)
// raw-line re-exports, so the remaining bindgen struct that embeds a
// work_struct by value (`blkdev.flush_work`) and every
// `crate::bindings::{work_struct,workqueue}` path across the crate
// resolves here. TRAIT-OPS (final wave) retired the bindgen
// `workqueue_callbacks` struct and its `workqueue_lifecycle_cb_t`/
// `workqueue_thread_lifecycle_cb_t` fn-pointer typedefs -- both replaced
// by the `WqLifecycle` trait below.
//
// TRAIT-OPS: the `func`/`fault` C-ABI fn-pointer pair -> single vtable
// dispatch slot. `WorkHandler` is dyn-compatible (both methods take
// `&self` + a raw pointer, no generics) so `dyn WorkHandler` is a valid
// trait object.
//
// Fault-default semantics: every real installer in this crate
// (`sched_timer.rs::work_callback`, `pcache.rs::pcache_flush_worker`,
// `vfs_syscall.rs::vfs_fput_work_func`, `fs.rs::iput_work_func`) only
// ever populated the old `func` slot and left `fault` at its
// zero-initialized `None` (grep-confirmed: no in-tree caller ever passed
// `Some` for the old `fault` parameter). `execute()` below only reaches
// `fault` when a `RUN_ON_DRAIN` item is executed against an inactive
// (killed/drained) queue; with `fault: None` that was a silent no-op via
// `invoke_work_cb`'s `None` arm (still honoring `FREE_AFTER_RUN`
// afterwards). The default method below reproduces exactly that.
pub(crate) trait WorkHandler: Sync {
    /// The item's normal execution body (was the `func` slot).
    ///
    /// # Safety
    /// `w` must point to a live `work_struct` this handler was installed
    /// on (via [`WorkStruct::init`]/[`WorkStruct::create`] or their `_ex`
    /// forms), valid for the duration of the call — see the module-level
    /// `# Safety` note in `crate::proc::access` that every
    /// `WorkStructRef` method already relies on.
    unsafe fn run(&self, w: *mut work_struct);

    /// Run instead of `run` when a `RUN_ON_DRAIN` item is executed
    /// against a drained (killed) queue (was the `fault` slot).
    ///
    /// # Safety
    /// Same contract as `run`.
    unsafe fn fault(&self, _w: *mut work_struct) {
        // Default: no-op. Reproduces the historical `fault: None`
        // behavior -- see the fault-default note above.
    }
}

/// `func`/`fault` deliberately keep the bindgen `*mut work_struct`
/// callback signature (not `*mut WorkStruct`) in [`WorkHandler`]'s
/// methods: they are genuine C-ABI pointers passed to handlers stored by
/// callers across the crate that already pass `*mut work_struct`; only
/// this struct's own field *storage* is native, the callback ABI is
/// untouched.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct WorkStruct {
    pub(crate) entry: ListNode,
    pub(crate) handler: Option<&'static dyn WorkHandler>,
    pub(crate) data: u64,
    pub(crate) flags: u32,
}

// P3-N3 hardcoded layout proof, updated for TRAIT-OPS — values captured
// from the pre-nativization bindgen output (kernel_bindings.rs: `pub
// struct work_struct { entry: list_node_t, func: Option<..>, fault:
// Option<..>, data: uint64, flags: uint32 }`) and independently
// confirmed by a riscv64-unknown-elf-gcc `_Static_assert` probe
// (rv64gc/lp64d) against `kernel/inc/proc/workqueue_types.h`: size 48,
// align 8, offsets 0/16/24/32/40. The two 8-byte `func`/`fault` thin fn
// pointers (offsets 16/24) collapsed into one 16-byte
// `Option<&'static dyn WorkHandler>` fat pointer at offset 16 -- LAYOUT
// IS NET-NEUTRAL (same total size, same trailing field offsets): `data`
// stays at 32, `flags` at 40. No embedder (`Pcache::flush_work` in
// `mm/pcache.rs`, `SchedTimerWork::work` in `timer/sched_timer.rs`)
// needs an offset update. Unlike `workqueue` (which gets align(64) from
// its embedded spinlock_t), work_struct has no embedded lock and stays
// naturally (8-byte) aligned.
const _: () = {
    assert!(core::mem::size_of::<WorkStruct>() == 48, "work_struct size");
    assert!(core::mem::align_of::<WorkStruct>() == 8, "work_struct natural alignment");
    assert!(core::mem::offset_of!(WorkStruct, entry) == 0, "work_struct.entry offset");
    assert!(core::mem::offset_of!(WorkStruct, handler) == 16, "work_struct.handler offset");
    assert!(core::mem::offset_of!(WorkStruct, data) == 32, "work_struct.data offset");
    assert!(core::mem::offset_of!(WorkStruct, flags) == 40, "work_struct.flags offset");
    // Honest fat-pointer proof (IrqHandler/RbOps precedent): a `Option<&'static
    // dyn WorkHandler>` is a 2-word (data ptr, vtable ptr) fat pointer with a
    // niche in the data-pointer half, so `Option<..>` costs nothing extra.
    assert!(core::mem::size_of::<Option<&'static dyn WorkHandler>>() == 16, "handler fat-ptr size");
    assert!(core::mem::align_of::<Option<&'static dyn WorkHandler>>() == 8, "handler fat-ptr align");
};

// TRAIT-OPS (final wave): the six-slot `workqueue_lifecycle_cb_t`/
// `workqueue_thread_lifecycle_cb_t` fn-pointer table (former
// `struct workqueue_callbacks`, `kernel/inc/proc/workqueue_types.h`)
// collapses into one vtable dispatch slot, same shape as `WorkHandler`
// above: `WqLifecycle` is dyn-compatible (every method takes `&self`
// plus raw pointers, no generics) so `dyn WqLifecycle` is a valid trait
// object.
//
// All-default-is-no-op semantics: grep-exhaustive, the only in-tree
// installer of a non-empty callback set is `workqueue_test.rs`'s T1/T2
// cases, and even they only ever populate `workqueue_ctor`/
// `workqueue_dtor` (leaving `manager_ctor/dtor`/`worker_ctor/dtor` at
// their old `None`); every production workqueue (`Pcache`'s flush
// queue, `SchedTimer`'s queue, the VFS `iput` queue) is created via the
// callback-less `Workqueue::create`, i.e. `lifecycle: None`. Each
// dispatch site below used to guard the old `Option<fn>` slot with
// `invoke_wq_cb`/`invoke_wq_t_cb`'s own `None`-is-a-silent-no-op arm;
// a `None` `lifecycle` field reproduces that identically, and the six
// default (no-op) method bodies below reproduce the historical
// per-slot `None` behavior for any future installer that only
// overrides a subset of the six.
pub(crate) trait WqLifecycle: Sync {
    /// Runs once a fully-initialized queue is published, before
    /// `create_with_callbacks` spins up its manager thread (was the
    /// `workqueue_ctor` slot).
    ///
    /// # Safety
    /// `wq` must point to a live, properly initialized `workqueue` --
    /// same contract as every `WorkqueueRef` method (module-level
    /// `# Safety` note in `crate::proc::access`).
    unsafe fn workqueue_ctor(&self, _wq: *mut workqueue) {}
    /// Runs the first time a queue is killed (was the `workqueue_dtor`
    /// slot); see `WorkqueueRef::kill`'s `mark_dtor_once` guard for the
    /// exactly-once contract this is called under.
    ///
    /// # Safety
    /// Same contract as [`Self::workqueue_ctor`].
    unsafe fn workqueue_dtor(&self, _wq: *mut workqueue) {}
    /// Runs on a fresh manager thread right after it registers itself
    /// as `wq.manager` (was the `manager_ctor` slot).
    ///
    /// # Safety
    /// `wq`/`t` must point to a live `workqueue`/`thread`.
    unsafe fn manager_ctor(&self, _wq: *mut workqueue, _t: *mut thread) {}
    /// Runs on a manager thread as it exits (was the `manager_dtor`
    /// slot).
    ///
    /// # Safety
    /// Same contract as [`Self::manager_ctor`].
    unsafe fn manager_dtor(&self, _wq: *mut workqueue, _t: *mut thread) {}
    /// Runs on a fresh worker thread before it enters its dequeue loop
    /// (was the `worker_ctor` slot).
    ///
    /// # Safety
    /// Same contract as [`Self::manager_ctor`].
    unsafe fn worker_ctor(&self, _wq: *mut workqueue, _t: *mut thread) {}
    /// Runs on a worker thread as it exits (was the `worker_dtor`
    /// slot).
    ///
    /// # Safety
    /// Same contract as [`Self::manager_ctor`].
    unsafe fn worker_dtor(&self, _wq: *mut workqueue, _t: *mut thread) {}
}

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
    pub(crate) lifecycle: Option<&'static dyn WqLifecycle>,
}

// P3-N3 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (`pub struct workqueue { lock:
// spinlock_t, idle_queue: tq_t, worker_list: list_node_t, manager:
// *mut thread, pending_works: c_int, work_list: list_node_t, name:
// [c_char; 32], __bindgen_anon_1: workqueue__bindgen_ty_1, nr_workers:
// c_int, min_active: c_int, max_active: c_int, callbacks:
// workqueue_callbacks }`, `#[repr(C)] #[repr(align(64))]`) and
// independently confirmed by the cross-compiler `_Static_assert` probe:
// pre-TRAIT-OPS size 256, align 64, offsets
// 0/24/72/88/96/104/120/[152]/160/164/168/176 (the anonymous bitfield
// struct occupies [152,160), pinned in the probe by both neighbours).
//
// TRAIT-OPS (final wave): the trailing 48-byte, 6-slot
// `WorkqueueCallbacks` field (offset 176, extending to 224, padded to
// 256 for the `align(64)` tail) shrinks to one 16-byte
// `Option<&'static dyn WqLifecycle>` fat pointer at the same offset
// (176) -- `lifecycle` is the LAST field so no other offset above
// moves. New end-of-fields is 176+16=192, which is *itself* an exact
// multiple of 64, so the `align(64)` tail needs zero extra padding:
// `size_of::<Workqueue>()` SHRINKS 256 -> 192 (compiler-checked below,
// not just asserted).
const _: () = {
    assert!(core::mem::size_of::<Workqueue>() == 192, "workqueue size");
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
    assert!(core::mem::offset_of!(Workqueue, lifecycle) == 176, "workqueue.lifecycle offset");
    // Honest fat-pointer proof (WorkHandler/IrqHandler/RbOps precedent).
    assert!(core::mem::size_of::<Option<&'static dyn WqLifecycle>>() == 16, "lifecycle fat-ptr size");
    assert!(core::mem::align_of::<Option<&'static dyn WqLifecycle>>() == 8, "lifecycle fat-ptr align");
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
/// SAFETY: see [`crate::lock::spinlock::RawSpinlock::init`]'s contract.
fn spin_init(l: *mut spinlock_t, name: *mut c_char) {
    unsafe { crate::lock::spinlock::RawSpinlock::init(l, name) }
}
// P3-D3c: `proc/exit.rs`'s `exit` is a plain (safe) Rust fn now that its
// `#[no_mangle]` export is gone -- imported via its private sibling module
// path (the `crate::proc` glob would work too, but the direct path is
// unambiguous by construction).
use crate::proc::Proc;

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
use crate::proc::Scheduler;
// P3-D2b: `kill_thread` (proc/signal.rs) likewise.

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

// TRAIT-OPS (final wave): the former `invoke_wq_cb`/`invoke_wq_t_cb`
// callback trampolines (each a thin "invoke a stored `unsafe extern "C"
// fn` pointer, no-op if `None`" wrapper) are gone -- every lifecycle
// dispatch site below now calls straight through the single stored
// `Option<&'static dyn WqLifecycle>` slot (`if let Some(l) = ... { l.
// <slot>(..) }`), same shape as `WorkStructRef::execute`'s direct `dyn
// WorkHandler` dispatch just above.

// NO-STANDALONE-FN: the former `thread_killed`/`thread_set_killed` free fns
// are gone. The killed-flag test/set is now expressed directly at each call
// site through the `ThreadAccess` handle's already-public atomic bit accessors
// (`flags_test_bit`/`flags_set_bit`, defined in `crate::proc::access`) — the
// operation *is* a ThreadAccess method now, with the null guard folded into
// the `ta()`/`cur_h` handle the site already holds.

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
// across `Scheduler::yield_now()`. Every field touch here is a raw load/store the
// optimizer may not reorder. `&self` borrows the *handle* (a pointer), never
// the pointee. The raw-`*mut`-taking `*_impl` entry points below stay thin
// null-checking delegators (the C-ABI forwarders hold raw pointers).
// ===========================================================================

// -------- work_struct per-object operations -------------------------------
impl<'a> WorkStructRef<'a> {
    /// Valid iff the flags are well-formed and a handler is installed
    /// (was `work_struct_valid`, which required at least one of the old
    /// `func`/`fault` slots -- both now live behind the one `handler`
    /// slot, see `WorkHandler`).
    fn is_valid(&self) -> bool {
        work_flags_valid(self.flags()) && self.handler().is_some()
    }
    /// Full initializer (`init_work_struct_ex`); panics on invalid flags.
    fn init_ex(
        &self,
        handler: Option<&'static dyn WorkHandler>,
        data: u64,
        flags: u32,
    ) {
        if !work_flags_valid(flags) {
            xv6_panic(c"init_work_struct_ex: invalid work flags".as_ptr());
        }
        self.entry_ref().init();
        self.set_handler(handler);
        self.set_data(data);
        self.set_flags(flags);
    }
    /// Run the callback (or, on a drained inactive queue, the fault
    /// handler), then free the item if `FREE_AFTER_RUN` is set. After a
    /// free the handle is dangling and must not be reused (it is not).
    fn execute(&self, queue_active: bool) {
        let run = queue_active || self.flag_set(WORK_STRUCT_FLAG_RUN_ON_DRAIN);
        if run {
            if let Some(h) = self.handler() {
                // SAFETY: `self.as_ptr()` is this handle's own live
                // `work_struct`, exactly the contract `WorkHandler::run`/
                // `::fault` document.
                unsafe {
                    if !queue_active { h.fault(self.as_ptr()); } else { h.run(self.as_ptr()); }
                }
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
        Scheduler::wakeup(worker);
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
        if !m.is_null() { Scheduler::wakeup_thread(m); }
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
        if should {
            if let Some(l) = self.lifecycle() {
                // SAFETY: `self.as_ptr()` is this handle's own live `workqueue`
                // -- exactly the contract `WqLifecycle::workqueue_dtor` documents.
                unsafe { l.workqueue_dtor(self.as_ptr()); }
            }
        }
        if !manager.is_null() {
            let r = crate::proc::access::ThreadAccess::from_ptr(manager).map_or(-22, |ta| ta.kill_thread(SIGKILL));
            if r < 0 { if let Some(a) = ta(manager) { a.flags_set_bit(THREAD_FLAG_KILLED); } }
            Scheduler::wakeup_thread(manager);
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
            if let Some(l) = wq.lifecycle() {
                // SAFETY: `wq_p`/`cur_t` are the live `workqueue`/`thread` this
                // exiting worker belongs to -- `WqLifecycle::worker_dtor`'s contract.
                unsafe { l.worker_dtor(wq_p, cur_t); }
            }
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
    Proc::exit(exit_code as c_int)
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
        if let Some(l) = wq.lifecycle() {
            // SAFETY: `wq_p`/`cur_t` are the live `workqueue`/`thread` this
            // exiting manager belongs to -- `WqLifecycle::manager_dtor`'s contract.
            unsafe { l.manager_dtor(wq_p, cur_t); }
        }
        wq.lock_ref().lock();
        if wq.manager_ptr() == cur_t { wq.set_manager(ptr::null_mut()); }
        wq.lock_ref().unlock();
    }
    Proc::exit(exit_code as c_int)
}

// -------- worker / manager routines --------------------------------------
unsafe extern "C" fn worker_routine() {
    let cur_t = xv6_current_thread();
    // SAFETY: `cur_t` is the running thread; unsafe-fn body is an unsafe context.
    let cur_h = ThreadAccess::assume(cur_t);
    cur_h.tcb_lock();
    let wq_p = ta(cur_t).map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    if wq_p.is_null() { cur_h.tcb_unlock(); Proc::exit(-EINVAL); }
    cur_h.tcb_unlock();
    let Some(wq) = wqr(wq_p) else { Proc::exit(-EINVAL); };
    wq.lock_ref().lock();
    if wq.manager_ptr() == cur_t { wq.lock_ref().unlock(); Proc::exit(-EINVAL); }
    wq.lock_ref().unlock();
    if let Some(l) = wq.lifecycle() {
        // SAFETY: `wq_p`/`cur_t` are the live `workqueue`/`thread` this fresh
        // worker belongs to -- `WqLifecycle::worker_ctor`'s contract; this
        // fn's own `unsafe extern "C"` body is already an unsafe context.
        unsafe { l.worker_ctor(wq_p, cur_t); }
    }
    loop {
        wq.lock_ref().lock();
        if cur_h.flags_test_bit(THREAD_FLAG_KILLED) { wq.lock_ref().unlock(); exit_worker_routine(0); }
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
    if wq_p.is_null() { cur_h.tcb_unlock(); Proc::exit(-EINVAL); }
    cur_h.tcb_unlock();
    let Some(wq) = wqr(wq_p) else { Proc::exit(-EINVAL); };
    wq.lock_ref().lock();
    wq.set_manager(cur_t);
    wq.lock_ref().unlock();
    if let Some(l) = wq.lifecycle() {
        // SAFETY: `wq_p`/`cur_t` are the live `workqueue`/`thread` this fresh
        // manager belongs to -- `WqLifecycle::manager_ctor`'s contract; this
        // fn's own `unsafe extern "C"` body is already an unsafe context.
        unsafe { l.manager_ctor(wq_p, cur_t); }
    }
    wq.lock_ref().lock();
    loop {
        if cur_h.flags_test_bit(THREAD_FLAG_KILLED) {
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
        Scheduler::yield_now();
        wq.lock_ref().lock();
    }
}

// =========================================================================
// NO-STANDALONE-FN: cross-kernel entry points + constructors/factories as
// associated functions on the native `Workqueue`/`WorkStruct` types (the
// bindgen `workqueue`/`work_struct` paths resolve to these very types). The
// per-object algorithms live on the `WorkqueueRef`/`WorkStructRef` handles
// above; each associated fn here holds either a subsystem-global body (no
// receiver — same precedent as the scheduler's `Scheduler` marker assoc fns)
// or a thin null-checking delegator that builds a handle. The former free
// `xv6_wq_pub_*` aliases and the dead `init/create_work_struct[_flags/_ex]`
// flavour variants were grep-verified zero-caller dead code (not exported —
// `extern "C"` without `#[no_mangle]`, so they never satisfied the C-header
// prototypes either) and are DELETED rather than carried as standalone fns.
//
// Visibility mirrors the retired canonical names' out-of-`crate::proc`
// callers: pub(crate) for mm/pcache, timer/sched_timer, vfs/{fs,vfs_syscall},
// start_kernel; pub(super) for the sole `crate::proc::workqueue_test` callers.
// =========================================================================

// -------- Workqueue lifecycle & subsystem entry points --------------------
impl Workqueue {
    /// Initialize the workqueue subsystem's slab caches (was
    /// `workqueue_init`/`workqueue_init_impl`). start_kernel.
    pub(crate) fn init() {
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

    /// Runtime smoke-test hook (was `workqueue_runtime_smoke_test`). start_kernel.
    pub(crate) fn runtime_smoke_test() {}

    /// Slab-allocate and zero a `workqueue` (was `alloc_workqueue`).
    fn alloc() -> *mut workqueue {
        let wq = slab_alloc(wq_cache()) as *mut workqueue;
        if !wq.is_null() { memset(wq as *mut c_void, 0, core::mem::size_of::<workqueue>()); }
        wq
    }

    /// Create a workqueue with the given lifecycle callbacks (was
    /// `workqueue_create_with_callbacks[_impl]`). workqueue_test.
    pub(super) fn create_with_callbacks(
        name: *const c_char,
        mut max_active: c_int,
        lifecycle: Option<&'static dyn WqLifecycle>,
    ) -> *mut workqueue {
        if max_active < 0 { return ptr::null_mut(); }
        if max_active == 0 { max_active = WORKQUEUE_DEFAULT_MAX_ACTIVE; }
        else if max_active > MAX_WORKQUEUE_ACTIVE { max_active = MAX_WORKQUEUE_ACTIVE; }
        let name = if name.is_null() { c"unnamed".as_ptr() } else { name };
        let wq_p = Self::alloc();
        if wq_p.is_null() { return ptr::null_mut(); }
        let Some(wq) = wqr(wq_p) else { return ptr::null_mut(); };
        wq.struct_init();
        strncpy(wq.name_buf_ptr(), name, WORKQUEUE_NAME_MAX);
        wq.set_lifecycle(lifecycle);
        wq.set_max_active(max_active);
        wq.set_min_active(if max_active < WORKQUEUE_DEFAULT_MIN_ACTIVE {
            WORKQUEUE_DEFAULT_MIN_ACTIVE
        } else { max_active });
        wq.set_nr_workers(0);
        wq.set_active(1);
        if let Some(l) = wq.lifecycle() {
            // SAFETY: `wq_p` is the just-initialized, live `workqueue` this
            // handle owns -- `WqLifecycle::workqueue_ctor`'s contract.
            unsafe { l.workqueue_ctor(wq_p); }
        }
        wq.lock_ref().lock();
        if wq.create_manager() != 0 {
            let should = wq.mark_dtor_once();
            wq.lock_ref().unlock();
            if should {
                if let Some(l) = wq.lifecycle() {
                    // SAFETY: same contract as the `workqueue_ctor` call above.
                    unsafe { l.workqueue_dtor(wq_p); }
                }
            }
            free_ptr(wq_p);
            return ptr::null_mut();
        }
        wq.wakeup_manager();
        wq.lock_ref().unlock();
        wq_p
    }

    /// Create a callback-less workqueue (was `workqueue_create`).
    /// pcache/timer/vfs/workqueue_test.
    pub(crate) fn create(name: *const c_char, max_active: c_int) -> *mut workqueue {
        Self::create_with_callbacks(name, max_active, None)
    }

    /// Deactivate a workqueue and tear down its manager (was
    /// `workqueue_kill[_impl]`). Thin null-checking delegator to
    /// `WorkqueueRef::kill`. workqueue_test.
    pub(super) fn kill(wq_p: *mut workqueue) -> c_int {
        wqr(wq_p).map(|wq| wq.kill()).unwrap_or(-EINVAL)
    }

    /// Validate and enqueue a work item on a live queue (was
    /// `queue_work[_impl]`). Thin null-checking delegator to
    /// `WorkqueueRef::queue_work`. pcache/timer/vfs/workqueue_test.
    pub(crate) fn queue(wq_p: *mut workqueue, work_p: *mut work_struct) -> bool {
        wqr(wq_p).map(|wq| wq.queue_work(work_p)).unwrap_or(false)
    }
}

// -------- WorkStruct construction / lifetime ------------------------------
impl WorkStruct {
    /// Slab-allocate and zero a `work_struct` (was `alloc_work_struct`).
    fn alloc() -> *mut work_struct {
        let w = slab_alloc(ws_cache()) as *mut work_struct;
        if !w.is_null() { memset(w as *mut c_void, 0, core::mem::size_of::<work_struct>()); }
        w
    }

    /// Full initializer over a raw item (was `init_work_struct_ex_impl`); thin
    /// null-checking delegator to `WorkStructRef::init_ex`.
    fn init_ex(
        work: *mut work_struct,
        handler: Option<&'static dyn WorkHandler>,
        data: u64,
        flags: u32,
    ) {
        if let Some(w) = wsr(work) { w.init_ex(handler, data, flags); }
    }

    /// Default-flag initializer over a caller-owned item (was
    /// `init_work_struct`). pcache/timer.
    pub(crate) fn init(
        work: *mut work_struct,
        handler: Option<&'static dyn WorkHandler>,
        data: u64,
    ) {
        Self::init_ex(work, handler, data, WORK_STRUCT_DEFAULT_FLAGS)
    }

    /// Allocate + fully initialize a work item (was
    /// `create_work_struct_ex[_impl]`). workqueue_test.
    pub(super) fn create_ex(
        handler: Option<&'static dyn WorkHandler>,
        data: u64,
        flags: u32,
    ) -> *mut work_struct {
        if !work_flags_valid(flags) { return ptr::null_mut(); }
        let w = Self::alloc();
        if w.is_null() { return ptr::null_mut(); }
        Self::init_ex(w, handler, data, flags);
        w
    }

    /// Allocate + default-flag initialize a work item (was
    /// `create_work_struct`). vfs/workqueue_test.
    pub(crate) fn create(
        handler: Option<&'static dyn WorkHandler>,
        data: u64,
    ) -> *mut work_struct {
        Self::create_ex(handler, data, WORK_STRUCT_DEFAULT_FLAGS)
    }

    /// Free a slab-allocated work item (was `free_work_struct[_impl]`).
    /// vfs/vfs_syscall/workqueue_test.
    pub(crate) fn free(work: *mut work_struct) { free_ptr(work); }
}
