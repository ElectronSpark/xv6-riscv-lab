//! Rust port of `kernel/lock/rcu.c`.
//!
//! Read-Copy-Update synchronization primitive. Timestamp-based grace
//! periods, per-CPU pending callback lists, per-CPU drainer kthreads;
//! see the original C file for the full algorithm narrative.
//!
//! # Public surface
//!
//! Two parallel APIs:
//!
//! * **Historical names** — `pub(crate) fn rcu_*` thin wrappers
//!   preserving the original 15 names for their in-crate consumers
//!   (formerly `#[no_mangle] extern "C"` exports; the C-ABI surface
//!   is dismantled).
//! * **Rust API** — [`KRcuRead`] RAII guard at the module root
//!   (parallel to [`crate::sync::KSpinlock`] /
//!   [`crate::machine::PreemptGuard`]) plus free functions under
//!   the [`api`] submodule: `api::read_lock`, `api::synchronize`,
//!   `api::barrier`, `api::synchronize_expedited`, `api::is_watching`,
//!   `api::note_context_switch`, `api::process_callbacks`,
//!   `api::kthread_wakeup`, `api::init`, `api::cpu_init`,
//!   `api::kthread_start_cpu`, and the `unsafe` `api::call`.
//!
//! ## Safety strategy
//!
//! * Every entry point is a one-line dispatcher to a *safe* `*_impl`
//!   function. All internal cross-calls go through the `_impl` form
//!   so no business-logic `fn` opens an `unsafe { ... }` block just
//!   to call another RCU function.
//! * **Preemption** is bracketed by [`PreemptGuard`] (RAII for
//!   `push_off` / `pop_off`).
//! * **Spinlocks** are accessed through [`KSpinlock`], whose
//!   [`KSpinlock::lock`] returns a drop-on-release guard.
//! * The single deref of `current_thread_ptr()` is encapsulated in
//!   [`with_current_thread`].
//! * Variadic `printf` and `trigger_panic` are routed through the
//!   small `kpanic_*` / `kwarn_*` helpers so business logic never
//!   opens an `unsafe { ... }` block just to format a static string.
//! * The kernel `cpus[]` array is bound as an immutable `static`
//!   wrapped in a `Sync` newtype; the only `rcu_*`-touched field
//!   (`rcu_timestamp`) is reinterpreted as `&'static AtomicU64`
//!   through a single helper.
//! * `rcu_head_t` field access lives in tiny inline helpers (one
//!   unsafe deref each) following the centralised-unsafe pattern
//!   already used by `completion.rs` and `mutex.rs`.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void, CStr};
use core::marker::PhantomData;
use core::mem::MaybeUninit;
use core::ptr::null_mut;
use core::sync::atomic::{AtomicI32, AtomicPtr, AtomicU64, Ordering};

use crate::bindings::{
    cpu_local, rcu_callback_t, rcu_head_t, slab_cache_t, spinlock_t, thread,
    tq_t, NCPU, SLAB_FLAG_DEBUG_BITMAP, SLAB_FLAG_STATIC,
};
use crate::machine::{self, PreemptGuard};
use crate::sync::KSpinlock;
// P3-1C mesh sweep: printf.rs is in scope for this wave, so `trigger_panic`
// becomes a plain crate-path import instead of an `extern "C"` redeclaration.
use crate::printf::trigger_panic;

// ---------------------------------------------------------------------------
// External C symbols
// ---------------------------------------------------------------------------

// P3-D2a: the thread-queue primitives (`kernel/proc/thread_queue.rs`)
// and scheduler entry points (`kernel/proc/sched.rs`) are ordinary Rust
// fns, reached as plain crate-path items instead of `extern "C"`
// redeclarations.
use crate::proc::{scheduler_yield, tq_init, tq_wakeup_all, wakeup, wakeup_interruptible};

// P3-D3c: `timer/sched_timer.rs`'s `sleep_ms` is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone -- crate-path import.
use crate::timer::sched_timer::sleep_ms;

// P3-D3b: `kthread_create` (proc/thread.rs) is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone; reached via the `crate::proc`
// glob re-export like its neighbors above.
use crate::proc::kthread_create;
/// `crate::mm::slab::slab_free` is genuinely `unsafe fn`; this file's
/// original extern declaration asserted `pub safe fn` (usual FFI facade).
#[inline]
fn slab_free(obj: *mut c_void) {
    // SAFETY: `obj` must originate from `slab_alloc` below (same contract
    // `crate::mm::slab::slab_free` always had).
    unsafe { crate::mm::slab::slab_free(obj) };
}

// `slab_cache_init`/`slab_alloc` are NOT plain `use` re-exports: this
// file's extern declaration always typed the cache pointer as the bindgen
// `slab_cache_t` (`RCU_HEAD_SLAB`'s storage type below) rather than
// `crate::mm::slab::SlabCache` (that module's own local view of the same
// C type — the same "locally convenient pointer type" idiom as
// `cffi::raw`'s spinlock wrappers), and passed `obj_size` as `u64`/`name`
// as `*const c_char` rather than `usize`/`*mut c_char`. These thin casts
// preserve the original call-site types.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t,
    name: *const c_char,
    obj_size: u64,
    flags: u32,
) -> c_int {
    // SAFETY: `cache` is `RCU_HEAD_SLAB`'s storage (a real `SlabCache`-
    // sized/aligned static, same C layout as `slab_cache_t`); `name` is
    // only read (never written) by the callee despite the `*mut`
    // parameter type.
    unsafe {
        crate::mm::slab::slab_cache_init(
            cache as *mut crate::mm::slab::SlabCache,
            name as *mut c_char,
            obj_size as usize,
            flags as u64,
        )
    }
}
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    // SAFETY: see `slab_cache_init` above.
    unsafe { crate::mm::slab::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}


// P3-D2a: `sched_attr` *is* generated into `bindings` (rq_types.h
// reaches the bindgen wrapper via rq.h), so the former local mirror
// struct is gone, and `sched_attr_init`/`sched_setattr` are ordinary
// Rust fns in `kernel/proc/rq.rs`, reached as plain crate-path items
// instead of `extern "C"` redeclarations.
use crate::bindings::sched_attr;
use crate::proc::{sched_attr_init, sched_setattr};

// ---------------------------------------------------------------------------
// `cpus[]` — kernel global array of per-CPU records.
//
// `cpu_local` is not `Sync` (it contains raw pointer fields). We only
// touch one field — `rcu_timestamp` — and exclusively via atomic
// loads/stores, so wrapping the slot in a `Sync` newtype is sound.
// ---------------------------------------------------------------------------

#[repr(transparent)]
struct CpuSlot(cpu_local);
// SAFETY: only the `rcu_timestamp` field is accessed from Rust and
// only through atomic ops on both sides of the FFI boundary.
unsafe impl Sync for CpuSlot {}

#[inline]
fn cpu_rcu_ts(i: usize) -> &'static AtomicU64 {
    // P3-D3c: the per-CPU array is `ipi.rs`'s `pub(crate) static mut
    // cpus: CpuLocalArray` (a `#[repr(C)]` wrapper whose sole field sits
    // at offset 0), reached by crate path instead of this file's old
    // `#[link_name = "cpus"] static CPUS: [CpuSlot; NCPU]` view; the cast
    // below reproduces that view (`CpuSlot` is `#[repr(transparent)]`
    // over `cpu_local`).
    // SAFETY: `AtomicU64` is layout-compatible with `u64`. The C
    // kernel touches `rcu_timestamp` only via `__atomic_*` builtins
    // which match the atomic ABI used by `AtomicU64`; `i` is a valid CPU
    // index (< NCPU), so the offset stays inside the static.
    unsafe {
        let slot = (&raw const crate::ipi::cpus).cast::<CpuSlot>().add(i);
        &*(&(*slot).0.rcu_timestamp as *const u64 as *const AtomicU64)
    }
}

// ---------------------------------------------------------------------------
// Current-thread access
// ---------------------------------------------------------------------------

/// Run `f` with an exclusive borrow of the current thread (if any).
///
/// The closure runs synchronously; the borrow does not escape. The
/// current thread is the sole owner of its own fields, so an `&mut`
/// here cannot race even without preemption-off — but most callers
/// hold a `PreemptGuard` regardless.
#[inline]
fn with_current_thread<R>(f: impl FnOnce(&mut thread) -> R) -> Option<R> {
    let t = machine::current_thread_ptr();
    if t.is_null() {
        return None;
    }
    // SAFETY: `t` points at the current thread, which is unique to
    // this CPU; the closure observes/mutates only fields owned by
    // that thread.
    Some(f(unsafe { &mut *t }))
}

// ---------------------------------------------------------------------------
// Constants (verbatim from rcu.c)
// ---------------------------------------------------------------------------

const RCU_LAZY_GP_DELAY: i32 = 100;
const RCU_UINT64_MAX: u64 = u64::MAX;
const KERNEL_STACK_ORDER: c_int = 2;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3A, promoted to THE `struct rcu_head` in P3-N7.
//
// `RawRcuHead` is the canonical native definition of
// `kernel/inc/lock/rcu_type.h`'s `struct rcu_head`: `build.rs`
// blocklists the bindgen emission and re-exports this type as
// `crate::bindings::rcu_head` / `rcu_head_t` (facade `pub use`, N2
// pattern). It is embedded *by value* crate-wide (the still-bindgen
// `struct thread`'s own `rcu_head` field — which derives Copy/Clone
// and therefore needs this type to answer Copy=Yes in `build.rs`'s
// `NativeTypeCallbacks` — plus the native `DeviceMajor`/`IrqDesc`).
//
// The C struct's trailing member is an anonymous struct with a 1-bit
// `embedded_head` bitfield (bindgen emitted it as the 8-byte,
// `repr(align(8))` shell `rcu_head__bindgen_ty_1`) — represented here
// as a plain `u64` `flags` word (bit 0 = `embedded_head`) with
// matching accessor semantics, the N5/N6 "single-member anon flags"
// flattening precedent.
//
// DERIVE DECISION (P3-N7): Copy + Clone, faithfully reproducing the
// pre-nativization bindgen emission (`#[derive(Copy, Clone)]` — all
// fields are pointers/integers; `rcu_head` never embedded any
// blocklisted type, so it kept its derives through every previous
// wave).
//
// Layout evidence (P3-N7): temporary in-tree `offset_of!` gate on the
// live bindgen type + cross-compiler `_Static_assert` probe (toolchain
// gcc, rv64gc/lp64d — scratchpad p3n7_static_assert_probe.c); both
// agree on every value asserted below (no pipe-style divergence).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct RawRcuHead {
    pub next: *mut RawRcuHead,
    pub func: rcu_callback_t,
    pub data: *mut c_void,
    pub timestamp: u64,
    /// Anonymous C bitfield struct `{ uint64 embedded_head : 1; }`.
    pub flags: u64,
}

impl RawRcuHead {
    #[inline(always)]
    fn embedded_head(&self) -> bool {
        self.flags & 1 != 0
    }
    #[inline(always)]
    fn set_embedded_head(&mut self, v: bool) {
        self.flags = v as u64;
    }
}

// P3-N7 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc `_Static_assert`
// probe (see the representation note above).
const _: () = {
    assert!(core::mem::size_of::<RawRcuHead>() == 40, "rcu_head size");
    assert!(core::mem::align_of::<RawRcuHead>() == 8, "rcu_head alignment");
    assert!(core::mem::offset_of!(RawRcuHead, next) == 0, "rcu_head.next offset");
    assert!(core::mem::offset_of!(RawRcuHead, func) == 8, "rcu_head.func offset");
    assert!(core::mem::offset_of!(RawRcuHead, data) == 16, "rcu_head.data offset");
    assert!(core::mem::offset_of!(RawRcuHead, timestamp) == 24, "rcu_head.timestamp offset");
    assert!(core::mem::offset_of!(RawRcuHead, flags) == 32, "rcu_head anonymous-bitfield offset");
};

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

#[repr(C, align(64))]
struct RcuCpuData {
    pending_head: AtomicPtr<RawRcuHead>,
    pending_tail: AtomicPtr<RawRcuHead>,
    cb_count: AtomicU64,
    qs_count: AtomicU64,
    cb_invoked: AtomicU64,
}

impl RcuCpuData {
    const fn new() -> Self {
        Self {
            pending_head: AtomicPtr::new(null_mut()),
            pending_tail: AtomicPtr::new(null_mut()),
            cb_count: AtomicU64::new(0),
            qs_count: AtomicU64::new(0),
            cb_invoked: AtomicU64::new(0),
        }
    }
}

#[repr(C)]
struct RcuState {
    gp_start_timestamp: AtomicU64,
    gp_seq_completed: AtomicU64,
    gp_in_progress: AtomicI32,
    gp_lazy_start: AtomicI32,
    lazy_cb_count: AtomicI32,
    expedited_in_progress: AtomicI32,
    expedited_seq: AtomicU64,
    gp_count: AtomicU64,
    cb_invoked: AtomicU64,
    expedited_count: AtomicU64,
}

impl RcuState {
    const fn new() -> Self {
        Self {
            gp_start_timestamp: AtomicU64::new(0),
            gp_seq_completed: AtomicU64::new(0),
            gp_in_progress: AtomicI32::new(0),
            gp_lazy_start: AtomicI32::new(0),
            lazy_cb_count: AtomicI32::new(0),
            expedited_in_progress: AtomicI32::new(0),
            expedited_seq: AtomicU64::new(0),
            gp_count: AtomicU64::new(0),
            cb_invoked: AtomicU64::new(0),
            expedited_count: AtomicU64::new(0),
        }
    }
}

#[repr(C)]
struct RcuKthreadSlot {
    kthread: AtomicPtr<thread>,
    wakeup_pending: AtomicI32,
}

impl RcuKthreadSlot {
    const fn new() -> Self {
        Self {
            kthread: AtomicPtr::new(null_mut()),
            wakeup_pending: AtomicI32::new(0),
        }
    }
}

// Sync wrapper for `static` storage of plain POD C structs that are
// initialised at runtime via C `*_init` helpers.
#[repr(transparent)]
struct StaticCell<T>(UnsafeCell<MaybeUninit<T>>);
unsafe impl<T> Sync for StaticCell<T> {}
impl<T> StaticCell<T> {
    const fn uninit() -> Self {
        Self(UnsafeCell::new(MaybeUninit::uninit()))
    }
    #[inline]
    fn as_mut_ptr(&self) -> *mut T {
        self.0.get().cast()
    }
}

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------

static RCU_STATE: RcuState = RcuState::new();

const RCU_CPU_DATA_INIT: RcuCpuData = RcuCpuData::new();
static RCU_CPU_DATA: [RcuCpuData; NCPU as usize] = [RCU_CPU_DATA_INIT; NCPU as usize];

const RCU_KTHREAD_INIT: RcuKthreadSlot = RcuKthreadSlot::new();
static RCU_KTHREAD: [RcuKthreadSlot; NCPU as usize] = [RCU_KTHREAD_INIT; NCPU as usize];

static RCU_KTHREADS_STARTED: AtomicI32 = AtomicI32::new(0);

static RCU_HEAD_SLAB: StaticCell<slab_cache_t> = StaticCell::uninit();
static RCU_GP_LOCK_STORAGE: StaticCell<spinlock_t> = StaticCell::uninit();
static RCU_GP_WAITQ: StaticCell<tq_t> = StaticCell::uninit();
static RCU_GP_WAITQ_LOCK_STORAGE: StaticCell<spinlock_t> = StaticCell::uninit();

#[inline]
fn gp_lock() -> KSpinlock {
    KSpinlock::from_bindings(RCU_GP_LOCK_STORAGE.as_mut_ptr())
}
#[inline]
fn gp_waitq_lock() -> KSpinlock {
    KSpinlock::from_bindings(RCU_GP_WAITQ_LOCK_STORAGE.as_mut_ptr())
}

// ---------------------------------------------------------------------------
// Print / panic helpers — centralise the variadic-FFI unsafety.
// ---------------------------------------------------------------------------

#[inline(never)]
#[cold]
fn kpanic_msg(msg: &'static CStr) -> ! {
    // SAFETY: `trigger_panic` is a kernel C symbol with the declared ABI.
    crate::kprint!("{}", crate::printf::Cs(msg.as_ptr()));
    unsafe {
        trigger_panic();
    }
}

#[inline]
fn kwarn_msg(msg: &'static CStr) {
    crate::kprint!("{}", crate::printf::Cs(msg.as_ptr()));
}

/// Panic helper for the unbalanced-unlock diagnostic. Centralises
/// the only formatted printf in the read-side fast path.
#[inline(never)]
#[cold]
fn kpanic_unbalanced_unlock(name_ptr: *const c_char, pid: c_int) -> ! {
    // SAFETY: `name_ptr` is a NUL-terminated buffer inside the
    // live current `thread`; `pid` is an `i32`.
    crate::kprintln!(
        "PANIC: rcu_read_unlock: unbalanced unlock in thread {} (pid {})",
        crate::printf::Cs(name_ptr),
        pid,
    );
    unsafe {
        trigger_panic();
    }
}

fn print_kthread_started(cpu: u64) {
    crate::kprintln!("RCU callback kthread started on CPU {}", cpu);
}

fn warn_kthread_create_failed(cpu: c_int) {
    crate::kprintln!("Failed to create RCU kthread for CPU {}", cpu);
}

// ---------------------------------------------------------------------------
// rcu_head_t field helpers (centralised-unsafe pattern)
// ---------------------------------------------------------------------------

#[inline]
fn head_set_next(h: *mut RawRcuHead, n: *mut RawRcuHead) {
    // SAFETY: caller exclusively owns `h` (just allocated, or walking
    // a list chain it has detached).
    unsafe { (*h).next = n; }
}
#[inline]
fn head_next(h: *mut RawRcuHead) -> *mut RawRcuHead {
    // SAFETY: see `head_set_next`.
    unsafe { (*h).next }
}
#[inline]
fn head_timestamp(h: *mut RawRcuHead) -> u64 {
    // SAFETY: see `head_set_next`.
    unsafe { (*h).timestamp }
}

/// Read all the fields needed to invoke a callback in one unsafe deref.
struct HeadFields {
    func: rcu_callback_t,
    data: *mut c_void,
    embedded: bool,
}

#[inline]
fn head_read(h: *mut RawRcuHead) -> HeadFields {
    // SAFETY: caller owns `h` exclusively while reading.
    unsafe {
        let r = &*h;
        HeadFields {
            func: r.func,
            data: r.data,
            embedded: r.embedded_head(),
        }
    }
}

#[inline]
fn head_init(
    h: *mut RawRcuHead,
    func: rcu_callback_t,
    data: *mut c_void,
    ts: u64,
    embedded: bool,
) {
    // SAFETY: caller has just allocated `h` or is reinitialising a
    // node it owns exclusively.
    unsafe {
        let r = &mut *h;
        r.next = null_mut();
        r.func = func;
        r.data = data;
        r.timestamp = ts;
        r.set_embedded_head(embedded);
    }
}

/// Invoke a callback function pointer.
///
/// # Safety
/// `cb` must be a valid C-ABI callback; `data` must satisfy `cb`'s
/// contract.
#[inline]
unsafe fn invoke_cb(cb: unsafe extern "C" fn(*mut c_void), data: *mut c_void) {
    unsafe { cb(data); }
}

// ---------------------------------------------------------------------------
// Singly-linked callback list builder
// ---------------------------------------------------------------------------

struct CbList {
    head: *mut RawRcuHead,
    tail: *mut RawRcuHead,
}

impl CbList {
    #[inline]
    const fn new() -> Self {
        Self { head: null_mut(), tail: null_mut() }
    }
    #[inline]
    fn push(&mut self, h: *mut RawRcuHead) {
        head_set_next(h, null_mut());
        if self.tail.is_null() {
            self.head = h;
        } else {
            head_set_next(self.tail, h);
        }
        self.tail = h;
    }
    #[inline]
    fn is_empty(&self) -> bool {
        self.head.is_null()
    }
}

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------

// `is_err_or_null`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::is_err_or_null;

fn min_other_cpu_ts(exclude_cpu: c_int) -> u64 {
    // Goal #2: read-only min-scan over the per-CPU RCU timestamps ->
    // iterator fold. Each `.load(Acquire)` remains a distinct atomic
    // op, so this is behavior-identical to the manual loop. (Atomic
    // loads carry their own ordering and are never subject to the
    // Freeze/noalias hoisting that pins the raw lock structs to floor,
    // so this scan is a clean, sound conversion.)
    (0..NCPU as usize)
        .filter(|&i| i as c_int != exclude_cpu)
        .map(|i| cpu_rcu_ts(i).load(Ordering::Acquire))
        .filter(|&ts| ts != 0)
        .fold(RCU_UINT64_MAX, u64::min)
}

fn gp_completed() -> bool {
    let gp_start = RCU_STATE.gp_start_timestamp.load(Ordering::Acquire);
    if gp_start == 0 {
        return false;
    }
    // Goal #2: read-only all-scan -> iterator. A CPU with ts == 0 has
    // not recorded a quiescent state yet (skip it); any recorded
    // ts < gp_start means the grace period has not completed. Behavior-
    // identical to the early-return loop (distinct atomic loads).
    (0..NCPU as usize).all(|i| {
        let ts = cpu_rcu_ts(i).load(Ordering::Acquire);
        ts == 0 || ts >= gp_start
    })
}

fn wakeup_gp_waiters() {
    let _g = gp_waitq_lock().lock();
    tq_wakeup_all(RCU_GP_WAITQ.as_mut_ptr(), 0, 0);
}

fn wakeup_all_kthreads() {
    for slot in RCU_KTHREAD.iter() {
        let p = slot.kthread.load(Ordering::Acquire);
        if !p.is_null() {
            wakeup_interruptible(p);
        }
    }
}

// ---------------------------------------------------------------------------
// Internal safe `_impl` functions
//
// All cross-module logic lives here. The historical-name `rcu_*`
// wrappers (further down) are trivial dispatchers.
// ---------------------------------------------------------------------------

fn init_impl() {
    gp_lock().init(c"rcu_gp_lock".as_ptr());
    gp_waitq_lock().init(c"rcu_gp_waitq_lock".as_ptr());

    tq_init(
        RCU_GP_WAITQ.as_mut_ptr(),
        c"rcu_gp_waitq".as_ptr(),
        RCU_GP_WAITQ_LOCK_STORAGE.as_mut_ptr(),
    );

    let ret = slab_cache_init(
        RCU_HEAD_SLAB.as_mut_ptr(),
        c"rcu_head_cache".as_ptr(),
        core::mem::size_of::<rcu_head_t>() as u64,
        SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP,
    );
    if ret != 0 {
        kpanic_msg(c"PANIC: Failed to initialize rcu_head_cache slab cache\n");
    }

    RCU_STATE.gp_start_timestamp.store(0, Ordering::Release);
    RCU_STATE.gp_seq_completed.store(0, Ordering::Release);
    RCU_STATE.gp_in_progress.store(0, Ordering::Release);
    RCU_STATE.gp_count.store(0, Ordering::Release);
    RCU_STATE.cb_invoked.store(0, Ordering::Release);
    RCU_STATE.gp_lazy_start.store(1, Ordering::Release);
    RCU_STATE.lazy_cb_count.store(0, Ordering::Release);
    RCU_STATE.expedited_in_progress.store(0, Ordering::Release);
    RCU_STATE.expedited_seq.store(0, Ordering::Release);
    RCU_STATE.expedited_count.store(0, Ordering::Release);

    for i in 0..NCPU as i32 {
        cpu_init_impl(i);
        cpu_rcu_ts(i as usize).store(0, Ordering::Release);
    }
}

fn cpu_init_impl(cpu: c_int) {
    if cpu < 0 || cpu >= NCPU as c_int {
        return;
    }
    let rcp = &RCU_CPU_DATA[cpu as usize];
    rcp.pending_head.store(null_mut(), Ordering::Release);
    rcp.pending_tail.store(null_mut(), Ordering::Release);
    rcp.cb_count.store(0, Ordering::Release);
    rcp.qs_count.store(0, Ordering::Release);
    rcp.cb_invoked.store(0, Ordering::Release);
}

// --- read side ---

fn read_lock_impl() {
    machine::push_off();
    with_current_thread(|t| t.rcu_read_lock_nesting += 1);
}

fn read_unlock_impl() {
    if let Some(panic_args) = with_current_thread(|t| {
        t.rcu_read_lock_nesting -= 1;
        if t.rcu_read_lock_nesting < 0 {
            Some((t.name.as_ptr(), t.pid))
        } else {
            None
        }
    })
    .flatten()
    {
        // Late panic: we've already decremented; the kpanic does not return.
        kpanic_unbalanced_unlock(panic_args.0, panic_args.1);
    }
    machine::pop_off();
}

fn is_watching_impl() -> bool {
    with_current_thread(|t| t.rcu_read_lock_nesting > 0).unwrap_or(false)
}

// --- per-CPU pending list ---

fn cblist_enqueue(rcp: &RcuCpuData, head: *mut RawRcuHead) {
    head_set_next(head, null_mut());
    let tail = rcp.pending_tail.load(Ordering::Acquire);
    if tail.is_null() {
        rcp.pending_head.store(head, Ordering::Release);
        rcp.pending_tail.store(head, Ordering::Release);
    } else {
        head_set_next(tail, head);
        rcp.pending_tail.store(head, Ordering::Release);
    }
}

fn drain_pending(rcp: &RcuCpuData) -> *mut RawRcuHead {
    let _preempt = PreemptGuard::new();
    let head = rcp.pending_head.swap(null_mut(), Ordering::AcqRel);
    rcp.pending_tail.store(null_mut(), Ordering::Release);
    head
}

fn requeue_notready(rcp: &RcuCpuData, notready: &CbList) {
    let _preempt = PreemptGuard::new();
    let old_head = rcp.pending_head.load(Ordering::Acquire);
    head_set_next(notready.tail, old_head);
    rcp.pending_head.store(notready.head, Ordering::Release);
    if old_head.is_null() {
        rcp.pending_tail.store(notready.tail, Ordering::Release);
    }
}

fn partition_pending(mut p: *mut RawRcuHead, min_other: u64) -> (CbList, CbList) {
    let mut ready = CbList::new();
    let mut notready = CbList::new();
    while !p.is_null() {
        let cur = p;
        p = head_next(p);
        head_set_next(cur, null_mut());
        if head_timestamp(cur) <= min_other {
            ready.push(cur);
        } else {
            notready.push(cur);
        }
    }
    (ready, notready)
}

// --- grace-period management ---

fn start_gp() {
    let _g = gp_lock().lock();
    if RCU_STATE.gp_in_progress.load(Ordering::Acquire) != 0 {
        return;
    }
    let now = machine::read_time();
    RCU_STATE.gp_start_timestamp.store(now, Ordering::Release);
    RCU_STATE.gp_in_progress.store(1, Ordering::Release);
}

fn advance_gp() {
    if RCU_STATE.gp_in_progress.load(Ordering::Acquire) == 0 {
        return;
    }
    if !gp_completed() {
        return;
    }
    let _g = gp_lock().lock();
    // Double-check under the lock.
    if RCU_STATE.gp_in_progress.load(Ordering::Acquire) == 0 || !gp_completed() {
        return;
    }
    RCU_STATE.gp_seq_completed.fetch_add(1, Ordering::AcqRel);
    RCU_STATE.gp_in_progress.store(0, Ordering::Release);
    RCU_STATE.gp_count.fetch_add(1, Ordering::Release);
}

fn note_context_switch_impl() {
    let preempt = PreemptGuard::new();
    let cpu = preempt.cpuid();
    let now = machine::read_time();
    cpu_rcu_ts(cpu).store(now, Ordering::Release);
    RCU_CPU_DATA[cpu].qs_count.fetch_add(1, Ordering::Release);
    advance_gp();
}

// --- call_rcu ---

fn call_impl(head: *mut RawRcuHead, func: rcu_callback_t, data: *mut c_void) {
    let Some(cb) = func else { return; };

    let caller_supplied = !head.is_null();
    let head = if caller_supplied {
        head
    } else {
        let allocated = slab_alloc(RCU_HEAD_SLAB.as_mut_ptr()) as *mut RawRcuHead;
        if allocated.is_null() {
            // Allocation failure → degrade to synchronous behaviour.
            synchronize_impl();
            // SAFETY: caller-provided C callback; FFI contract.
            unsafe { invoke_cb(cb, data); }
            return;
        }
        allocated
    };

    head_init(head, func, data, machine::read_time(), caller_supplied);

    // Enqueue under preempt-off so the per-CPU list cannot race with
    // the local kthread draining it.
    let lazy_count = {
        let preempt = PreemptGuard::new();
        let cpu = preempt.cpuid();
        let rcp = &RCU_CPU_DATA[cpu];
        cblist_enqueue(rcp, head);
        rcp.cb_count.fetch_add(1, Ordering::Release);
        RCU_STATE.lazy_cb_count.fetch_add(1, Ordering::Release)
    };

    if RCU_STATE.gp_lazy_start.load(Ordering::Acquire) != 0 {
        if lazy_count >= RCU_LAZY_GP_DELAY {
            RCU_STATE.lazy_cb_count.store(0, Ordering::Release);
            start_gp();
        }
    } else {
        start_gp();
    }

    kthread_wakeup_impl();
}

// --- callback invocation ---

fn invoke_ready(list: *mut RawRcuHead) -> c_int {
    let mut cur = list;
    let mut count: c_int = 0;
    while !cur.is_null() {
        let next = head_next(cur);
        let f = head_read(cur);
        head_set_next(cur, null_mut());
        if let Some(cb) = f.func {
            // SAFETY: caller-supplied C callback; FFI contract.
            unsafe { invoke_cb(cb, f.data); }
            count += 1;
        }
        if !f.embedded {
            slab_free(cur as *mut c_void);
        }
        cur = next;
    }
    RCU_STATE.cb_invoked.fetch_add(count as u64, Ordering::Release);
    count
}

fn process_callbacks_for_cpu(cpu: c_int) {
    let rcp = &RCU_CPU_DATA[cpu as usize];
    let min_other = min_other_cpu_ts(cpu);

    let pending = drain_pending(rcp);
    if pending.is_null() {
        return;
    }

    let (ready, notready) = partition_pending(pending, min_other);

    if !ready.is_empty() {
        let count = invoke_ready(ready.head);
        rcp.cb_count.fetch_sub(count as u64, Ordering::Release);
        rcp.cb_invoked.fetch_add(count as u64, Ordering::Release);
    }
    if !notready.is_empty() {
        requeue_notready(rcp, &notready);
    }
}

fn process_callbacks_impl() {
    let cpu = {
        let g = PreemptGuard::new();
        g.cpuid() as c_int
    };
    process_callbacks_for_cpu(cpu);
}

// --- synchronize ---

fn synchronize_impl() {
    let sync_ts = machine::read_time();
    note_context_switch_impl();

    let max_wait: c_int = 100_000;
    let mut wait_count: c_int = 0;

    let my_cpu = {
        let g = PreemptGuard::new();
        g.cpuid() as c_int
    };

    while wait_count < max_wait {
        let min_ts = min_other_cpu_ts(my_cpu);
        if min_ts >= sync_ts {
            wakeup_all_kthreads();
            return;
        }
        scheduler_yield();
        wait_count += 1;
    }
    kwarn_msg(c"synchronize_rcu: WARNING - not all CPUs passed quiescent state\n");
}

fn barrier_impl() {
    let barrier_ts = machine::read_time();
    synchronize_impl();

    let max_wait: c_int = 100_000;
    let mut wait_count: c_int = 0;
    let mut all_done = false;

    while !all_done && wait_count < max_wait {
        wakeup_all_kthreads();
        process_callbacks_impl();

        all_done = true;
        'cpus: for rcp in RCU_CPU_DATA.iter() {
            if rcp.cb_count.load(Ordering::Acquire) == 0 {
                continue;
            }
            let mut cb = rcp.pending_head.load(Ordering::Acquire);
            while !cb.is_null() {
                if head_timestamp(cb) <= barrier_ts {
                    all_done = false;
                    break 'cpus;
                }
                cb = head_next(cb);
            }
        }

        if !all_done {
            synchronize_impl();
            scheduler_yield();
            wait_count += 1;
        }
    }
    synchronize_impl();
}

// --- expedited ---

fn expedited_gp() {
    let exp_start = {
        let _g = gp_lock().lock();
        if RCU_STATE.expedited_in_progress.load(Ordering::Acquire) != 0 {
            return;
        }
        RCU_STATE.expedited_in_progress.store(1, Ordering::Release);
        RCU_STATE.expedited_seq.fetch_add(1, Ordering::AcqRel);
        machine::read_time()
    };

    let max_wait: c_int = 10_000;
    let mut wait_count: c_int = 0;
    while wait_count < max_wait {
        let mut all_switched = true;
        for i in 0..NCPU as usize {
            let ts = cpu_rcu_ts(i).load(Ordering::Acquire);
            if ts == 0 {
                continue;
            }
            if ts <= exp_start {
                all_switched = false;
                break;
            }
        }
        if all_switched {
            break;
        }
        scheduler_yield();
        wait_count += 1;
    }

    let _g = gp_lock().lock();
    RCU_STATE.expedited_in_progress.store(0, Ordering::Release);
    RCU_STATE.expedited_count.fetch_add(1, Ordering::Release);
}

fn synchronize_expedited_impl() {
    let start_exp = RCU_STATE.expedited_seq.load(Ordering::Acquire);
    let old_lazy = RCU_STATE.gp_lazy_start.swap(0, Ordering::AcqRel);

    expedited_gp();
    start_gp();

    let max_wait: c_int = 50_000;
    let mut wait_count: c_int = 0;
    while wait_count < max_wait {
        let current_exp = RCU_STATE.expedited_seq.load(Ordering::Acquire);
        if current_exp > start_exp {
            RCU_STATE.gp_lazy_start.store(old_lazy, Ordering::Release);
            return;
        }
        advance_gp();
        scheduler_yield();
        wait_count += 1;
    }
    RCU_STATE.gp_lazy_start.store(old_lazy, Ordering::Release);
    kwarn_msg(c"synchronize_rcu_expedited: WARNING - expedited GP did not complete\n");
}

// --- per-CPU drainer kthread ---

extern "C" fn rcu_cb_kthread(cpu_id: u64, _arg2: u64) -> c_int {
    if machine::cpuid() as u64 != cpu_id {
        kpanic_msg(c"PANIC: RCU kthread started on wrong CPU\n");
    }
    print_kthread_started(cpu_id);

    loop {
        if machine::cpuid() as u64 != cpu_id {
            kpanic_msg(c"PANIC: RCU kthread running on wrong CPU\n");
        }

        let rcp = &RCU_CPU_DATA[cpu_id as usize];
        advance_gp();

        let min_other = min_other_cpu_ts(cpu_id as c_int);
        let pending = drain_pending(rcp);
        let (ready, notready) = partition_pending(pending, min_other);

        if !ready.is_empty() {
            let count = invoke_ready(ready.head);
            rcp.cb_count.fetch_sub(count as u64, Ordering::Release);
            rcp.cb_invoked.fetch_add(count as u64, Ordering::Release);
        }

        wakeup_gp_waiters();
        RCU_KTHREAD[cpu_id as usize]
            .wakeup_pending
            .store(0, Ordering::Release);

        if !notready.is_empty() {
            requeue_notready(rcp, &notready);
            sleep_ms(50);
        } else {
            sleep_ms(5000);
        }
    }
}

fn kthread_wakeup_impl() {
    if RCU_KTHREADS_STARTED.load(Ordering::Acquire) == 0 {
        return;
    }
    let cpu = {
        let g = PreemptGuard::new();
        g.cpuid()
    };
    let slot = &RCU_KTHREAD[cpu];
    let p = slot.kthread.load(Ordering::Acquire);
    if !p.is_null() {
        slot.wakeup_pending.store(1, Ordering::Release);
        wakeup_interruptible(p);
    }
}

// Per-CPU kthread names matching the C source.
static RCU_NAMES: [&[u8]; NCPU as usize] = [
    b"rcu_cb/0\0",
    b"rcu_cb/1\0",
    b"rcu_cb/2\0",
    b"rcu_cb/3\0",
    b"rcu_cb/4\0",
    b"rcu_cb/5\0",
    b"rcu_cb/6\0",
    b"rcu_cb/7\0",
];

fn kthread_start_cpu_impl(cpu: c_int) {
    if cpu < 0 || cpu >= NCPU as c_int {
        return;
    }
    let slot = &RCU_KTHREAD[cpu as usize];
    slot.kthread.store(null_mut(), Ordering::Release);
    slot.wakeup_pending.store(0, Ordering::Release);

    let name = RCU_NAMES[cpu as usize].as_ptr() as *const c_char;
    let entry: extern "C" fn(u64, u64) -> c_int = rcu_cb_kthread;
    let p = kthread_create(
        name,
        entry as *mut c_void,
        cpu as u64,
        0,
        KERNEL_STACK_ORDER,
    );
    if is_err_or_null(p) {
        warn_kthread_create_failed(cpu);
        return;
    }

    // Set CPU affinity BEFORE waking the kthread.
    let mut attr = sched_attr {
        size: 0,
        affinity_mask: 0,
        time_slice: 0,
        priority: 0,
        flags: 0,
    };
    sched_attr_init(&mut attr);
    attr.affinity_mask = 1u64 << cpu;
    // SAFETY: `p` is a freshly-created thread; `sched_entity` lives
    // for the thread's lifetime.
    let se = unsafe { (*p).sched_entity };
    sched_setattr(se, &attr);

    slot.kthread.store(p, Ordering::Release);
    wakeup(p);

    RCU_KTHREADS_STARTED.store(1, Ordering::Release);
}

// ---------------------------------------------------------------------------
// Idiomatic Rust API
//
// Mirrors `KSpinlock` / `PreemptGuard`: a guard type for the scoped
// read-side critical section, plus free functions under `api::` for
// the rest. All thin wrappers around the `_impl` functions above.
// ---------------------------------------------------------------------------

/// RAII guard for an RCU read-side critical section.
///
/// Construction enters the read-side section (disables preemption
/// and increments the per-thread nesting counter); drop releases it.
/// `!Send` because the guard pins preemption to the current hart.
///
/// ```ignore
/// {
///     let _g = crate::lock::rcu::KRcuRead::new();
///     // ... read-side critical section ...
/// } // released here
/// ```
#[must_use = "RCU read-side guard is released on drop"]
pub struct KRcuRead {
    _not_send: PhantomData<*const ()>,
}

impl KRcuRead {
    #[inline]
    pub fn new() -> Self {
        read_lock_impl();
        Self { _not_send: PhantomData }
    }

    /// Closure-scoped read-side section, mirroring
    /// [`crate::sync::KSpinlock::with`].
    #[inline]
    pub fn with<R>(f: impl FnOnce(&Self) -> R) -> R {
        let g = Self::new();
        f(&g)
    }

    /// Lifetime-anchored load of an [`RcuPtr<T>`]. The returned
    /// borrow is tied to `self`, so the compiler rejects any attempt
    /// to use it after the read-side guard is dropped.
    #[inline]
    pub fn load<'g, T>(&'g self, p: &RcuPtr<T>) -> Option<&'g T> {
        // SAFETY: `RcuPtr::swap` / `RcuPtr::new` establish the
        // publisher contract: while the pointer is observable, it
        // points at a live `T` that will not be freed until a grace
        // period elapses. `'g` cannot outlive the guard, so the `&T`
        // cannot outlive that grace-period window.
        let raw = p.inner.load(Ordering::Acquire);
        unsafe { raw.as_ref() }
    }
}

impl Default for KRcuRead {
    #[inline]
    fn default() -> Self { Self::new() }
}

impl Drop for KRcuRead {
    #[inline]
    fn drop(&mut self) {
        read_unlock_impl();
    }
}

// ---------------------------------------------------------------------------
// RcuPtr<T> — type-safe RCU-protected pointer cell
// ---------------------------------------------------------------------------

/// An atomically-replaceable pointer to a value protected by RCU.
///
/// Holds a `*mut T` cell. Reads are *safe* and lifetime-anchored via
/// [`KRcuRead::load`]; writes (`swap`) are `unsafe` because they
/// require the caller to defer freeing of the previous value until
/// after a grace period.
///
/// ```ignore
/// use crate::lock::rcu::{self, KRcuRead, RcuPtr};
///
/// static PTR: RcuPtr<MyData> = unsafe { RcuPtr::new(core::ptr::null_mut()) };
///
/// // Reader
/// {
///     let g = KRcuRead::new();
///     if let Some(d) = g.load(&PTR) {
///         // borrow `d: &MyData` cannot escape `g`
///         use_data(d);
///     }
/// }
///
/// // Writer
/// let new_box: *mut MyData = allocate_and_init();
/// let old = unsafe { PTR.swap(new_box) };
/// if !old.is_null() {
///     unsafe { rcu::api::call(core::ptr::null_mut(), Some(drop_my_data), old.cast()); }
/// }
/// ```
#[repr(transparent)]
pub struct RcuPtr<T> {
    inner: core::sync::atomic::AtomicPtr<T>,
}

impl<T> RcuPtr<T> {
    /// Create a new cell.
    ///
    /// # Safety
    /// If `p` is non-null it must point at a valid `T` that lives
    /// until the cell is updated via [`Self::swap`] AND a subsequent
    /// grace period elapses.
    #[inline]
    pub const unsafe fn new(p: *mut T) -> Self {
        Self { inner: core::sync::atomic::AtomicPtr::new(p) }
    }

    /// Empty cell (null pointer).
    #[inline]
    pub const fn empty() -> Self {
        Self { inner: core::sync::atomic::AtomicPtr::new(core::ptr::null_mut()) }
    }

    /// Publish `new` and return the previously-stored pointer.
    ///
    /// # Safety
    /// * `new`, if non-null, must satisfy the contract of [`Self::new`].
    /// * The returned previous pointer (if non-null) MUST NOT be
    ///   freed before a full grace period — typically via
    ///   [`api::call`] (asynchronous) or [`api::synchronize`]
    ///   (blocking) followed by a manual free.
    #[inline]
    pub unsafe fn swap(&self, new: *mut T) -> *mut T {
        self.inner.swap(new, Ordering::AcqRel)
    }

    /// Compare-and-swap. Same safety obligations as [`Self::swap`]
    /// for `new`; the previous pointer (if returned in `Ok`) must
    /// also be RCU-deferred for free.
    #[inline]
    pub unsafe fn compare_exchange(
        &self,
        current: *mut T,
        new: *mut T,
    ) -> Result<*mut T, *mut T> {
        self.inner
            .compare_exchange(current, new, Ordering::AcqRel, Ordering::Acquire)
    }

    /// Raw load without a guard. Returns a possibly-null pointer
    /// that the caller MUST treat as immediately-dangling unless
    /// they hold their own read-side guard.
    ///
    /// Prefer [`KRcuRead::load`] for normal use.
    #[inline]
    pub fn load_raw(&self) -> *mut T {
        self.inner.load(Ordering::Acquire)
    }
}

// SAFETY: `RcuPtr` is a typed `AtomicPtr`. Sending the cell across
// threads requires `T: Send` (the pointee can end up on any CPU);
// sharing the cell (`&RcuPtr<T>`) across threads requires `T: Sync`
// since concurrent readers see shared `&T`.
unsafe impl<T: Send + Sync> Send for RcuPtr<T> {}
unsafe impl<T: Send + Sync> Sync for RcuPtr<T> {}

/// Idiomatic Rust API. Each function is a thin safe wrapper around
/// the private `_impl` form; the corresponding historical `rcu_*`
/// name lives at the module root as a `pub(crate)` dispatcher.
pub mod api {
    use super::*;

    /// Enter an RCU read-side critical section. The returned guard
    /// releases it on drop. Equivalent to [`KRcuRead::new`].
    #[inline]
    pub fn read_lock() -> KRcuRead { KRcuRead::new() }

    /// `true` iff the calling thread is currently in an RCU
    /// read-side critical section.
    #[inline]
    pub fn is_watching() -> bool { is_watching_impl() }

    /// Wait for all pre-existing RCU read-side critical sections on
    /// every CPU to complete.
    #[inline]
    pub fn synchronize() { synchronize_impl() }

    /// Like [`synchronize`] but additionally waits for all
    /// already-queued callbacks to execute.
    #[inline]
    pub fn barrier() { barrier_impl() }

    /// Expedited variant of [`synchronize`] — kicks every CPU
    /// through a quiescent state immediately.
    #[inline]
    pub fn synchronize_expedited() { synchronize_expedited_impl() }

    /// Record a quiescent state on the current CPU.
    #[inline]
    pub fn note_context_switch() { note_context_switch_impl() }

    /// Run the per-CPU callback drainer once on the current CPU.
    #[inline]
    pub fn process_callbacks() { process_callbacks_impl() }

    /// Wake the current CPU's RCU drainer kthread (no-op if none).
    #[inline]
    pub fn kthread_wakeup() { kthread_wakeup_impl() }

    /// Read CPU `cpu`'s last-recorded RCU quiescent-state timestamp
    /// (`cpus[cpu].rcu_timestamp`). Diagnostic use only (e.g. tests
    /// verifying that the mechanism is being updated); zero means "no
    /// timestamp recorded yet".
    #[inline]
    pub fn cpu_timestamp(cpu: usize) -> u64 {
        cpu_rcu_ts(cpu).load(Ordering::Acquire)
    }

    /// One-shot subsystem init. Call once from CPU 0 during boot.
    #[inline]
    pub fn init() { init_impl() }

    /// Per-CPU subsystem init. Called by [`init`] for each CPU; can
    /// be re-invoked after CPU hotplug-down/up.
    #[inline]
    pub fn cpu_init(cpu: c_int) { cpu_init_impl(cpu) }

    /// Spawn the per-CPU RCU drainer kthread on `cpu` and pin its
    /// affinity.
    #[inline]
    pub fn kthread_start_cpu(cpu: c_int) { kthread_start_cpu_impl(cpu) }

    /// Schedule `func(data)` after a grace period.
    ///
    /// If `head` is null, a slab-allocated head is used and freed by
    /// the drainer; otherwise the caller-supplied head is kept and
    /// the caller owns it again once `func` returns.
    ///
    /// # Safety
    /// * `func` must be a valid C-ABI callback.
    /// * `data` must be valid for `func`'s contract.
    /// * If `head` is non-null it must point at a `rcu_head_t` that
    ///   outlives the callback invocation.
    #[inline]
    pub unsafe fn call(
        head: *mut rcu_head_t,
        func: rcu_callback_t,
        data: *mut c_void,
    ) {
        // SAFETY: layout equivalence between `rcu_head_t` and the
        // native `RawRcuHead` is proven at compile time (see the
        // assertions next to `RawRcuHead`'s definition).
        call_impl(head as *mut RawRcuHead, func, data);
    }
}

// ---------------------------------------------------------------------------
// Historical-name wrappers — preserve the 15 original `rcu_*` names for
// their cross-module Rust consumers (the `#[no_mangle] extern "C"`
// exports are gone; everything reaches these by crate path). Each body
// is a single-line dispatch into the matching `_impl`. Functions whose
// contract is genuinely safe (`rcu_read_lock`/`_unlock`/
// `rcu_check_callbacks` — pure dispatch into safe `_impl`s, mirrored by
// the safe `api::*` forms above) are plain safe fns, matching the `safe
// fn` declarations their consumers already relied on; the rest keep
// `unsafe fn`.
// ---------------------------------------------------------------------------

pub(crate) unsafe fn rcu_init() { init_impl() }

pub(crate) unsafe fn rcu_cpu_init(cpu: c_int) { cpu_init_impl(cpu) }

#[inline]
pub(crate) fn rcu_read_lock() { read_lock_impl() }

#[inline]
pub(crate) fn rcu_read_unlock() { read_unlock_impl() }

pub(crate) unsafe fn rcu_is_watching() -> c_int {
    if is_watching_impl() { 1 } else { 0 }
}

pub(crate) unsafe fn rcu_note_context_switch() { note_context_switch_impl() }

#[inline]
pub(crate) fn rcu_check_callbacks() { note_context_switch_impl() }

/// # Safety
/// Same contract as [`api::call`].
pub(crate) unsafe fn call_rcu(
    head: *mut rcu_head_t,
    func: rcu_callback_t,
    data: *mut c_void,
) {
    // SAFETY: see the identical cast in `api::call`.
    call_impl(head as *mut RawRcuHead, func, data)
}

pub(crate) unsafe fn rcu_process_callbacks() { process_callbacks_impl() }

pub(crate) unsafe fn synchronize_rcu() { synchronize_impl() }

pub(crate) unsafe fn rcu_barrier() { barrier_impl() }

pub(crate) unsafe fn synchronize_rcu_expedited() { synchronize_expedited_impl() }

pub(crate) unsafe fn rcu_kthread_wakeup() { kthread_wakeup_impl() }

pub(crate) unsafe fn rcu_kthread_start_cpu(cpu: c_int) { kthread_start_cpu_impl(cpu) }

pub(crate) unsafe fn rcu_kthread_start() {
    // Kept for ABI compatibility; per-CPU init is done by
    // `rcu_kthread_start_cpu` from each CPU's idle path.
}
