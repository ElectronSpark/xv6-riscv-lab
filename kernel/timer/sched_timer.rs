//! Scheduler-tick consumer — Rust port of `kernel/timer/sched_timer.c`
//! (Phase 2 Wave 8, see `docs/rustify/phase2_plan.md`).
//!
//! Owns the single scheduler timer instance (`SCHED_TIMER`, a
//! [`timer_core`]-backed [`timer_root`]) plus the public sleep/timeout API
//! built on top of it: [`sleep_ms`]/[`sleep_ms_interruptible`] (used by
//! `console.rs`, `proc/sysproc.rs`'s `sys_nanosleep`/`sys_sleep`) and
//! [`sched_timer_set`]/[`sched_timer_done`] (used by every blocking
//! primitive with a timeout — `lock/{mutex,rwsem,completion,semaphore}.rs`,
//! each of which keeps its own `extern "C"` declaration of these two names,
//! unchanged by this port).
//!
//! # Interrupt-context discipline
//!
//! [`__do_timer_tick`] is the bridge from the scheduler tick to the timer
//! wheel. It is called exactly once per [`sleep_ms`]... no — exactly once
//! per `Scheduler::yield_now()` (`proc/sched.rs::scheduler_yield_inner`, an
//! `extern` call this crate's proc module makes into this symbol), which
//! runs in **thread context**, not interrupt context. `timer_core::clockintr`
//! (the actual timer ISR) never calls into this file directly; it only
//! flips [`SCHED_TICK_CLEAR`] via `sched_timer_tick`. This dirty-flag
//! handoff (`AtomicBool::store`/`swap`, `Release`/`Acquire`, mirroring the
//! C `__atomic_clear`/`__atomic_test_and_set`) coalesces any number of timer
//! interrupts that fire between two `Scheduler::yield_now()` calls into a single
//! `timer_tick()` pass — so the workqueue submission in
//! [`timer_callback`]/`work_callback` (which *does* allocate, via
//! `slab_alloc`) only ever runs from thread context, never from the raw
//! ISR.
//!
//! # Timer node lifetime
//!
//! Two distinct node lifetimes are in play:
//!  - **Stack-allocated** (`sleep_ms`/`sleep_ms_interruptible`,
//!    `sched_timer_set`/`sched_timer_done`): the caller owns a
//!    `timer_node` on its own stack or embedded in a lock struct
//!    (`lock/mutex.rs`'s `timer: timer_node` field, etc.) and pairs
//!    `sched_timer_set` with `sched_timer_done` around a sleep.
//!  - **Slab-allocated** (`sched_timer_add`/`sched_timer_add_deadline`):
//!    a [`SchedTimerWork`] (embedding both a `timer_node` and a
//!    `work_struct`) is allocated from [`SCHED_TIMER_WORK_SLAB`], self-frees
//!    in `work_callback` after the caller's callback runs, and is not
//!    exposed to the caller at all.

#![allow(non_camel_case_types, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering};

use crate::bindings::{slab_cache_t, thread, thread_state, timer_node, timer_root, work_struct, workqueue};
use crate::machine;
use crate::timer::timer_core::{timer_add, timer_init, timer_node_init, timer_remove, timer_tick};

// ===========================================================================
// Constants mirrored from C headers.
// ===========================================================================
const TIMER_DEFAULT_RETRY_LIMIT: c_int = 3;
const WORKQUEUE_DEFAULT_MAX_ACTIVE: c_int = 8;

// thread_state (kernel/inc/proc/thread_types.h) — same values as
// `proc/sched.rs`'s local copy of this enum.
const THREAD_INTERRUPTIBLE: thread_state = 2;
const THREAD_KIILABLE: thread_state = 3;
const THREAD_TIMER: thread_state = 4;
const THREAD_KIILABLE_TIMER: thread_state = 5;
const THREAD_UNINTERRUPTIBLE: thread_state = 6;
const THREAD_RUNNING: thread_state = 8;

// ===========================================================================
// `workqueue_create`/`queue_work`/`init_work_struct` (proc/workqueue.rs)
// and the `slab_*`/`__panic_*`/`printf` group keep their own `extern "C"`
// declarations: none of them are being demoted by this wave
// (`workqueue_create`/`queue_work`/`init_work_struct` in particular are
// also declared, as their own separate items, by several other
// `proc/*.rs` files, so resolving them here via `crate::proc::*` would
// risk `E0659` name-ambiguity through `proc/mod.rs`'s glob re-exports;
// not worth the churn for symbols whose ABI isn't changing).
// P3-D2a: `scheduler_yield`/`wakeup` (proc/sched.rs) ARE demoted by this
// wave -- reached as plain crate-path items below.
// ===========================================================================
use crate::proc::Scheduler;

// P3-D3b: proc/workqueue.rs's entry points are plain safe Rust fns now
// that their `#[no_mangle]` exports are gone; reached via the
// `crate::proc` glob re-export like `scheduler_yield`/`wakeup` above.
use crate::proc::{WorkStruct, Workqueue};

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {
    // printf is variadic, so it cannot be declared `safe`.
}

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

/// Replicates the C `assert(expr, fmt)` macro expansion. Same pattern as
/// `timer_core.rs`'s `timer_assert` / `irq/irq_core.rs`'s `irq_assert`.
fn sched_timer_assert(cond: bool, line: u32, function: &core::ffi::CStr, msg: &core::ffi::CStr) {
    if cond {
        return;
    }
    __panic_start();
    crate::kprintln!(
        "ASSERTION_FAILURE {}:{}: In function '{}':",
        "kernel/timer/sched_timer.rs",
        line as c_int,
        crate::printf::Cs(function.as_ptr()),
    );
    crate::kprint!("{}", crate::printf::Cs(msg.as_ptr()));
    crate::kprintln!();
    __panic_end()
}

// ===========================================================================
// Thread-state predicates. Duplicated locally rather than imported (proc's
// `THREAD_SLEEPING`/`thread_state_get` helpers live in private submodules —
// same cross-module-boundary reasoning as the externs above; also mirrors
// how `proc/thread_group.rs` and `proc/signal.rs` each keep their own copy
// of this exact predicate).
// ===========================================================================

/// Mirrors `__thread_state_get()` (`kernel/inc/proc/thread.h`): a plain
/// `__ATOMIC_SEQ_CST` load of `p->state`. (`machine::smp_load_acquire_state`
/// is a different, weaker ordering used for a different invariant elsewhere
/// in the crate -- not reused here to keep this a faithful 1:1 port.)
fn thread_state_get(p: *mut thread) -> thread_state {
    if p.is_null() {
        return 0; // THREAD_UNUSED
    }
    // SAFETY: `p` is non-null per the check above; `state` is a plain
    // 4-byte-aligned `thread_state` (`c_uint`) field, safe to reinterpret
    // as `AtomicU32` for the duration of this load (same layout
    // justification as `machine::thread_state_set`).
    unsafe {
        let sp = core::ptr::addr_of!((*p).state) as *const AtomicU32;
        (*sp).load(Ordering::SeqCst) as thread_state
    }
}

/// Mirrors `THREAD_SLEEPING(p)`.
fn thread_sleeping(p: *mut thread) -> bool {
    if p.is_null() {
        return false;
    }
    let st = thread_state_get(p);
    st == THREAD_INTERRUPTIBLE
        || st == THREAD_UNINTERRUPTIBLE
        || st == THREAD_KIILABLE
        || st == THREAD_TIMER
        || st == THREAD_KIILABLE_TIMER
}

// ===========================================================================
// sched_timer_work — slab-allocated envelope pairing a `timer_node` with a
// `work_struct`, used by `sched_timer_add`/`sched_timer_add_deadline`.
// ===========================================================================
#[repr(C)]
struct SchedTimerWork {
    tn: timer_node,
    work: work_struct,
    callback: Option<unsafe extern "C" fn(*mut c_void)>,
    data: *mut c_void,
}

#[repr(transparent)]
struct CacheCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: `slab_cache_t`'s own internal locking serialises all field
// mutation, matching `irq/irq_core.rs`'s identical `CacheCell` pattern.
unsafe impl Sync for CacheCell {}

static SCHED_TIMER_WORK_SLAB: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));

#[inline(always)]
fn sched_timer_work_slab_ptr() -> *mut slab_cache_t {
    SCHED_TIMER_WORK_SLAB.0.get() as *mut slab_cache_t
}

/// The single scheduler timer instance (`static struct timer_root
/// __sched_timer;` in the C original — BSS-zeroed, made a valid
/// `timer_root` by `timer_init` in `sched_timer_init`, called exactly once
/// from `start_kernel.c` before any other access is possible).
#[repr(transparent)]
struct TimerRootCell(UnsafeCell<MaybeUninit<timer_root>>);
// SAFETY: every access after `sched_timer_init` goes through `timer_core`'s
// own `(*timer).lock`-guarded API (`timer_add`/`timer_remove`/`timer_tick`);
// this cell only hands out the raw storage address.
unsafe impl Sync for TimerRootCell {}

static SCHED_TIMER: TimerRootCell = TimerRootCell(UnsafeCell::new(MaybeUninit::zeroed()));

#[inline(always)]
fn sched_timer_ptr() -> *mut timer_root {
    SCHED_TIMER.0.get() as *mut timer_root
}

/// `static struct workqueue *__sched_timer_wq;` — set once in
/// `sched_timer_init`, read-only thereafter. Same "plain racy pointer ->
/// `AtomicPtr` with `Relaxed`" upgrade as `console.rs`'s `CONSOLE_TTY`
/// (single-writer-then-read-only, so `Relaxed` costs nothing and removes
/// the underlying data race).
static SCHED_TIMER_WQ: AtomicPtr<workqueue> = AtomicPtr::new(ptr::null_mut());

/// `static bool __sched_tick_clear;` — see the module doc's
/// "Interrupt-context discipline" section for the exact `Release`/`Acquire`
/// handoff this implements.
static SCHED_TICK_CLEAR: AtomicBool = AtomicBool::new(false);

// ===========================================================================
// Workqueue callbacks.
// ===========================================================================

unsafe extern "C" fn work_callback(work: *mut work_struct) {
    // SAFETY: `work` is always the embedded `work_struct` inside a live
    // `SchedTimerWork` published to `queue_work` by `timer_callback` below.
    let data = unsafe { (*work).data };
    let stw = data as *mut SchedTimerWork;
    // SAFETY: `stw` was just derived from a live work item's `data`;
    // dereferencing to check `callback` is safe (the pointer itself is
    // trusted to be a `SchedTimerWork` this module allocated).
    if stw.is_null() || unsafe { (*stw).callback }.is_none() {
        crate::kprintln!("warning: __work_callback: Invalid work or callback");
        return;
    }
    // SAFETY: `stw` is live and non-null (checked above); `tn` is the
    // embedded timer node `timer_callback` added to `SCHED_TIMER` --
    // removing it here is required before the caller's callback can be
    // allowed to e.g. free `stw`'s storage via a nested `sched_timer_add`.
    unsafe {
        timer_remove(&raw mut (*stw).tn);
        if let Some(cb) = (*stw).callback {
            cb((*stw).data);
        }
        free_sched_timer_work(stw);
    }
}

unsafe extern "C" fn timer_callback(tn: *mut timer_node) {
    // SAFETY: `tn` is live (called back by `timer_tick` while `SCHED_TIMER`
    // is locked); `data` was set to a `SchedTimerWork*` by
    // `alloc_sched_timer_work`.
    let data = unsafe { (*tn).data };
    let stw = data as *mut SchedTimerWork;
    if stw.is_null() {
        crate::kprintln!("warning: __timer_callback: Invalid work");
        return;
    }
    let wq = SCHED_TIMER_WQ.load(Ordering::Relaxed);
    // SAFETY: `stw` is live and non-null; `&raw mut (*stw).work` is the
    // embedded work item `alloc_sched_timer_work` initialized.
    let ok = unsafe { Workqueue::queue(wq, &raw mut (*stw).work) };
    if !ok {
        crate::kprintln!("warning: __sched_timer_add_timer_callback: Failed to queue work");
        // SAFETY: `stw` is live and non-null; queueing failed so it will
        // never be freed by `work_callback` -- this is the only remaining
        // owner.
        unsafe { free_sched_timer_work(stw) };
    }
}

/// # Safety
/// `callback` (if `Some`) must be safe to invoke later with `data`, from
/// workqueue-worker thread context.
unsafe fn alloc_sched_timer_work(
    deadline: u64,
    callback: Option<unsafe extern "C" fn(*mut c_void)>,
    data: *mut c_void,
) -> *mut SchedTimerWork {
    let stw = slab_alloc(sched_timer_work_slab_ptr()) as *mut SchedTimerWork;
    if stw.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `stw` is freshly slab-allocated, exclusively-owned memory
    // sized for `SchedTimerWork` (the slab was created with that
    // `obj_size` in `sched_timer_init`).
    unsafe {
        ptr::write_bytes(stw as *mut u8, 0, core::mem::size_of::<SchedTimerWork>());
        (*stw).callback = callback;
        (*stw).data = data;
        // Because no thread is waiting for this timer, retry_limit is set
        // to 1 (see `timer_core::timer_node_init`'s doc comment: this
        // parameter is a pre-existing no-op in the C original, kept here
        // for 1:1 fidelity -- the node is actually removed after its first
        // firing regardless of the value passed).
        timer_node_init(&raw mut (*stw).tn, deadline, Some(timer_callback), stw as *mut c_void, 1);
        WorkStruct::init(&raw mut (*stw).work, Some(work_callback), stw as u64);
    }
    stw
}

/// # Safety
/// `stw`, if non-null, must be a `SchedTimerWork` allocated by
/// [`alloc_sched_timer_work`] and not already freed.
unsafe fn free_sched_timer_work(stw: *mut SchedTimerWork) {
    if stw.is_null() {
        return;
    }
    // SAFETY: caller contract.
    unsafe { slab_free(stw as *mut c_void) };
}

// ===========================================================================
// clockintr <-> scheduler_yield handoff.
// ===========================================================================

/// Rust port of `sched_timer_tick()`. Called from `timer_core::clockintr`
/// (raw ISR context) on the boot hart, once per timer interrupt, via a
/// direct crate-path call (`crate::timer::sched_timer::sched_timer_tick`) --
/// P3-1B: no other caller anywhere, demoted from `#[no_mangle]`.
pub(crate) fn sched_timer_tick() {
    SCHED_TICK_CLEAR.store(false, Ordering::Release);
}

/// Rust port of `__do_timer_tick()`. Called from `proc/sched.rs`'s
/// `scheduler_yield_inner` (thread context), now via a direct crate-path
/// `use` -- P3-1B: demoted from `#[no_mangle]`.
pub(crate) fn __do_timer_tick() {
    let was_cleared = SCHED_TICK_CLEAR.swap(true, Ordering::Acquire);
    if !was_cleared {
        // SAFETY: `sched_timer_ptr()` is initialized by `sched_timer_init`,
        // called once before this function has any live caller
        // (`scheduler_yield` cannot run before `idle_thread_init`, which
        // `start_kernel.c` sequences after `sched_timer_init`).
        unsafe { timer_tick(sched_timer_ptr(), machine::Riscv::read_time()) };
    }
}

// ===========================================================================
// sched_timer_callback — wakes the sleeping thread a `sched_timer_set`
// timer node belongs to.
// ===========================================================================
unsafe extern "C" fn sched_timer_callback(tn: *mut timer_node) {
    // SAFETY: `tn` is live (called back while `SCHED_TIMER` is locked);
    // `data` was set to a `*mut thread` by `sched_timer_set`.
    let p = unsafe { (*tn).data } as *mut thread;
    if thread_sleeping(p) {
        Scheduler::wakeup(p);
    }
}

// ===========================================================================
// Public API — exact C names/signatures preserved.
// ===========================================================================

/// Rust port of `sched_timer_set()`.
///
/// # Safety
/// `tn`, if non-null, must point at caller-owned, writable storage for a
/// `timer_node` (it is unconditionally reinitialized via
/// [`timer_node_init`]) that outlives the subsequent sleep.
// P3-D3c: `#[no_mangle] extern "C"` dropped -- callers
// (`lock/{completion,mutex,rwsem,semaphore}.rs`) reach it by crate path.
pub(crate) unsafe fn sched_timer_set(tn: *mut timer_node, ms: u64) -> c_int {
    if tn.is_null() {
        return -(crate::bindings::EINVAL as c_int);
    }
    let expires = machine::Riscv::read_time() + machine::Riscv::ms_to_rawticks(ms);
    let current = machine::Riscv::current_thread_ptr();
    // SAFETY: `tn` is non-null per the check above and caller-owned per
    // this function's `# Safety` contract.
    unsafe {
        timer_node_init(tn, expires, Some(sched_timer_callback), current as *mut c_void, TIMER_DEFAULT_RETRY_LIMIT);
        timer_add(sched_timer_ptr(), tn)
    }
}

/// Rust port of `sched_timer_done()`.
///
/// # Safety
/// `tn`, if non-null, must point at a live `timer_node` (previously passed
/// to [`sched_timer_set`]).
// P3-D3c: same demotion as `sched_timer_set` above.
pub(crate) unsafe fn sched_timer_done(tn: *mut timer_node) {
    if tn.is_null() {
        return;
    }
    // SAFETY: caller contract.
    unsafe { timer_remove(tn) };
}

/// Rust port of `sleep_ms()`.
// P3-D3c: `#[no_mangle] extern "C"` dropped -- all callers reach it by
// crate path now.
pub(crate) fn sleep_ms(ms: u64) {
    if ms == 0 {
        return;
    }
    let p = machine::Riscv::current_thread_ptr();
    sched_timer_assert(!p.is_null(), line!(), c"sleep_ms", c"Current thread must not be NULL");

    // SAFETY: an all-zero bit pattern is a valid initial `timer_node`
    // (matches the C `struct timer_node tn = {0};`); `sched_timer_set`
    // below fully reinitializes it before use.
    let mut tn: timer_node = unsafe { core::mem::zeroed() };

    // Disable interrupts for the entire sleep/wake sequence to prevent the
    // timer callback from racing with our state transitions.
    let intr = machine::Riscv::intr_off_save();

    machine::Riscv::thread_state_set(p, THREAD_INTERRUPTIBLE);
    // SAFETY: `&raw mut tn` is caller-owned stack storage, live for the
    // rest of this function.
    let ret = unsafe { sched_timer_set(&raw mut tn, ms) };
    if ret != 0 {
        // Timer already expired or other transient failure -- just yield
        // the CPU once instead of sleeping, the best approximation for a
        // very short sleep.
        machine::Riscv::thread_state_set(p, THREAD_RUNNING);
        machine::Riscv::intr_restore(intr);
        Scheduler::yield_now();
        return;
    }

    Scheduler::yield_now();

    // After waking up, cancel the timer to avoid an unnecessary callback.
    // SAFETY: `tn` was successfully added by `sched_timer_set` above and
    // is still live stack storage.
    unsafe { sched_timer_done(&raw mut tn) };
    machine::Riscv::intr_restore(intr);
}

/// Rust port of `sleep_ms_interruptible()`.
///
/// Sleeps for up to `ms` milliseconds, interruptible by pending signals.
/// Returns the remaining time in milliseconds (0 if the full duration
/// elapsed, >0 if woken early).
// P3-1B: only caller is `proc/sysproc.rs` (in-scope; convert its own
// `extern` redeclaration to a direct crate-path `use`) -- demoted.
pub(crate) fn sleep_ms_interruptible(ms: u64) -> u64 {
    if ms == 0 {
        return 0;
    }
    let p = machine::Riscv::current_thread_ptr();
    sched_timer_assert(!p.is_null(), line!(), c"sleep_ms_interruptible", c"Current thread must not be NULL");

    // SAFETY: see `sleep_ms`.
    let mut tn: timer_node = unsafe { core::mem::zeroed() };
    let intr = machine::Riscv::intr_off_save();

    machine::Riscv::thread_state_set(p, THREAD_INTERRUPTIBLE);
    // SAFETY: see `sleep_ms`.
    let ret = unsafe { sched_timer_set(&raw mut tn, ms) };
    if ret != 0 {
        machine::Riscv::thread_state_set(p, THREAD_RUNNING);
        machine::Riscv::intr_restore(intr);
        Scheduler::yield_now();
        return 0;
    }

    Scheduler::yield_now();

    let mut remaining_ms = 0u64;
    let now = machine::Riscv::read_time();
    if tn.expires > now {
        remaining_ms = (tn.expires - now) / machine::Riscv::tick_ms();
    }

    // SAFETY: `tn` was successfully added by `sched_timer_set` above and
    // is still live stack storage.
    unsafe { sched_timer_done(&raw mut tn) };
    machine::Riscv::intr_restore(intr);
    remaining_ms
}

/// Rust port of `sched_timer_init()`. Called exactly once from
/// `start_kernel.c`, after `idle_thread_init` (see that call site's
/// comment: `idle_thread_init` must run first so `Rq::cpu_activate()` has
/// marked this CPU active before any `workqueue_create` can enqueue a
/// worker thread).
// P3-1B: only caller is `start_kernel.rs` (in-scope; convert its own
// `extern` redeclaration to a direct crate-path `use`) -- demoted.
pub(crate) fn sched_timer_init() {
    // SAFETY: `sched_timer_ptr()` points at `SCHED_TIMER`'s BSS-zeroed
    // storage, not yet published anywhere else; `timer_init` fully
    // initializes it (including registering the CLINT timer IRQ handler).
    unsafe { timer_init(sched_timer_ptr()) };
    SCHED_TICK_CLEAR.store(false, Ordering::Relaxed);

    let wq = Workqueue::create(c"sched_timer_wq".as_ptr(), WORKQUEUE_DEFAULT_MAX_ACTIVE);
    SCHED_TIMER_WQ.store(wq, Ordering::Relaxed);
    sched_timer_assert(!wq.is_null(), line!(), c"sched_timer_init", c"Failed to create scheduler timer workqueue");

    let ret = slab_cache_init(
        sched_timer_work_slab_ptr(),
        c"sched_timer_work_slab".as_ptr() as *mut c_char,
        core::mem::size_of::<SchedTimerWork>(),
        crate::bindings::SLAB_FLAG_EMBEDDED as u64,
    );
    sched_timer_assert(ret == 0, line!(), c"sched_timer_init", c"Failed to initialize sched_timer_work_slab");
}

/// Rust port of `sched_timer_add_deadline()`.
///
/// # Safety
/// `callback` (if `Some`) must be safe to invoke later with `data`, from
/// workqueue-worker thread context, at or after `deadline` (raw `time` CSR
/// ticks).
// P3-1B: zero callers anywhere in the tree (grep-verified, same as the C
// original -- this API was never wired up beyond `sched_timer_add` below,
// itself also dead). Demoted from `#[no_mangle]`; `#[allow(dead_code)]`
// documents the gap rather than silently deleting still-plausible public
// API (matches `goldfish_rtc_init`'s precedent in this same wave/module).
#[allow(dead_code)]
pub(crate) unsafe fn sched_timer_add_deadline(
    callback: Option<unsafe extern "C" fn(*mut c_void)>,
    data: *mut c_void,
    deadline: u64,
) -> c_int {
    if callback.is_none() {
        return -(crate::bindings::EINVAL as c_int);
    }
    // SAFETY: caller contract forwarded unchanged.
    let stw = unsafe { alloc_sched_timer_work(deadline, callback, data) };
    if stw.is_null() {
        return -(crate::bindings::ENOMEM as c_int);
    }
    // SAFETY: `stw` is live and non-null (checked above).
    let ret = unsafe { timer_add(sched_timer_ptr(), &raw mut (*stw).tn) };
    if ret != 0 {
        // SAFETY: `timer_add` failed, so `stw` was never linked anywhere
        // and this is the sole owner.
        unsafe { free_sched_timer_work(stw) };
        return ret;
    }
    0
}

/// Rust port of `sched_timer_add()`.
///
/// # Safety
/// Same contract as [`sched_timer_add_deadline`].
// P3-1B: zero callers anywhere in the tree (grep-verified) -- see
// `sched_timer_add_deadline`'s comment just above.
#[allow(dead_code)]
pub(crate) unsafe fn sched_timer_add(
    callback: Option<unsafe extern "C" fn(*mut c_void)>,
    data: *mut c_void,
    ms: u64,
) -> c_int {
    if callback.is_none() {
        return -(crate::bindings::EINVAL as c_int);
    }
    let deadline = machine::Riscv::read_time() + machine::Riscv::ms_to_rawticks(ms);
    // SAFETY: caller contract forwarded unchanged.
    unsafe { sched_timer_add_deadline(callback, data, deadline) }
}
