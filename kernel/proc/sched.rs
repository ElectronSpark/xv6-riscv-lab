//! Pure-Rust port of `kernel/proc/sched.c` (SECTION 18 of the former
//! `proc_rust_shims.c`).  Owns the canonical public ABI symbols
//! (`sleep_lock`, `scheduler_yield`, `context_switch_finish`, ...),
//! called directly by sibling Rust modules -- the `xv6_schport_*` C-ABI
//! alias layer that used to front these was collapsed in the P3-1B2
//! sweep.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::{offset_of, MaybeUninit};

use crate::bindings::{
    context, cpumask_t, rb_node, rb_root, rq, sched_entity, spinlock_t, thread,
    thread_state, tnode_t, ttree_t,
};
use crate::machine::{
    cpu_local_ptr, cpu_relax, cpuid as cpuid_rs, intr_get, intr_off_save, intr_restore, pop_off,
    push_off,
};
use crate::lock::rcu::Rcu;
use crate::proc::proc_shims::xv6_panic;
use crate::lock::spinlock::RawSpinlock;
use crate::proc::access::{
    is_err_or_null, smp_load_acquire_i32, smp_load_acquire_state, smp_rmb,
    smp_store_release_state, CpuLocalRef, RqRef, SchedEntityRef, SpinLockRef, ThreadAccess,
    TnodeRef, TtreeRef,
};
// NO-STANDALONE-FN: the `ttree_init`/`ttree_wait`/`ttree_wakeup_key` and
// `tnode_get_thread` free-fn delegators (thread_queue.rs) were deleted; the
// four sites below build a `TtreeRef`/`TnodeRef` handle via `from_ptr` and
// invoke the corresponding inherent method.
// P3-1B2: the whole `xv6_rqport_*` C-ABI alias layer these used to name
// (some via plain `use`, some via `extern "C"` redeclaration below -- one
// symbol, `rq_unlock_current_irqrestore`, was even redeclared twice under
// two different local names) was collapsed; every rq.rs function is now
// imported once, by its real name, and called directly.
// NO-STANDALONE-FN: the cpu-keyed run-queue entry points are now
// associated fns on `Rq` (`Rq::pick_next`/`lock_current_irqsave`/
// `select_task_rq`/`flush_wake_list`/...); import the type, call
// `Rq::method(..)` at the sites below.
use crate::proc::Rq;
use crate::timer::sched_timer::__do_timer_tick;
use crate::ipi::ipi_send_single;

// ===========================================================================
// Native scheduler-policy dispatch — P3-10e (user directive: fully adapt
// Rust style / remove the C interfaces). `SchedClass` was the fn-pointer
// ops table of `kernel/inc/proc/rq_types.h`'s `struct sched_class`
// (`build.rs` blocklists the bindgen emission and re-exports it as
// `crate::bindings::sched_class`, facade `pub use`, N2 pattern). This
// wave — DEFERRED from P3-10d because the field is layout-embedded — turns
// it into a **closed-set enum with static dispatch**: the ops arc closes.
//
// There are exactly two implementors, forever: the idle policy
// (`sched_idle.rs`, one run-queue per CPU at `IDLE_MAJOR_PRIORITY`) and
// the FIFO policy (`sched_fifo.rs`, every other priority). Every old ops
// slot is a method that `match`es the variant and routes to the policy
// body; a slot a class left `NULL` becomes that variant's no-op / `None`
// match arm — byte-identical to the old `if let Some(fp)` fallback:
//   * `select_task_rq`/`put_prev_task`/`set_next_task`: idle = no-op/None.
//   * `enqueue_task`/`dequeue_task`/`pick_next_task`: both real.
//   * `task_fork`/`task_dead`: never populated by either class today
//     (always no-op), but still dispatched — kept as no-op arms so the
//     fork/dead control flow stays structurally identical.
//   * `task_tick`/`yield_task`: never populated AND never dispatched (the
//     `RqRef` wrappers had zero call sites) — deleted outright, matching
//     P3-10d's never-populated-never-dispatched autopsy.
//
// SchedAttr stays a plain `#[repr(C)]` POD (its N7 layout note below).
//
// LAYOUT SAFETY (P3-10e): the two `*mut sched_class` holders (`Rq`@0,
// `SchedEntity`@48) and the `rqg` table are NOT asm-consumed — `grep`
// over every `.S`/`asm-offsets.h` finds no `sched_class`/`sched_entity`
// offset (asm-offsets.h itself records "no .S file consumes THREAD_*"),
// and `thread` holds a *pointer* to its stack-resident `SchedEntity`
// (placed via `size_of::<sched_entity>()`, dynamic) rather than embedding
// it. So this crate owns the layout. The holders keep their exact byte
// image regardless: the field shrinks 8 -> 1 byte and 7 bytes of padding
// absorb the difference, so `Rq`/`SchedEntity` offsets, sizes (64 / 320)
// and the kernel-stack arrangement are all unchanged (see rq.rs asserts).
// ===========================================================================

/// Native scheduling policy (`kernel/inc/proc/rq_types.h`'s
/// `struct sched_class`, formerly a fn-pointer ops table) — a closed set
/// of exactly two policies, dispatched statically via `match`.
///
/// Stored in the layout-pinned `Rq`/`SchedEntity` `sched_class` fields and
/// the `rqg` table as `Option<SchedClass>` (`None` == the old NULL/unset
/// sentinel). The explicit discriminants pin the `Option` niche so a
/// zeroed byte reads back as `None`, byte-identical to a zeroed
/// `*mut sched_class` (`write_bytes(0)` / zeroed kernel-stack storage) —
/// see the `const _` niche proof below.
#[derive(Copy, Clone, PartialEq, Eq)]
#[repr(u8)]
pub enum SchedClass {
    /// Idle policy — `sched_idle.rs`.
    Idle = 1,
    /// FIFO policy — `sched_fifo.rs`.
    Fifo = 2,
}

impl SchedClass {
    /// `enqueue_task` — both policies implement it.
    #[inline]
    pub(crate) fn enqueue_task(self, r: *mut rq, se: *mut sched_entity) {
        // SAFETY: `r`/`se` are the caller's locked run-queue and a valid
        // scheduling entity (rq lock held, both non-null) — the exact
        // contract the fn-pointer callbacks demanded.
        unsafe {
            match self {
                SchedClass::Idle => super::sched_idle::idle_enqueue_task(r, se),
                SchedClass::Fifo => super::sched_fifo::fifo_enqueue_task(r, se),
            }
        }
    }

    /// `dequeue_task` — both policies implement it (idle's body panics,
    /// exactly as before: the idle rq is never dequeued from).
    #[inline]
    pub(crate) fn dequeue_task(self, r: *mut rq, se: *mut sched_entity) {
        // SAFETY: see `enqueue_task`.
        unsafe {
            match self {
                SchedClass::Idle => super::sched_idle::idle_dequeue_task(r, se),
                SchedClass::Fifo => super::sched_fifo::fifo_dequeue_task(r, se),
            }
        }
    }

    /// `select_task_rq` — `None` for idle (slot was NULL), so callers fall
    /// through to their default CPU search exactly as before.
    #[inline]
    pub(crate) fn select_task_rq(
        self,
        current_rq: *mut rq,
        se: *mut sched_entity,
        mask: cpumask_t,
    ) -> Option<*mut rq> {
        // SAFETY: see `enqueue_task`; `current_rq` may be null (fifo's body
        // ignores it), matching the old fn-pointer call.
        unsafe {
            match self {
                SchedClass::Idle => None,
                SchedClass::Fifo => Some(super::sched_fifo::fifo_select_task_rq(current_rq, se, mask)),
            }
        }
    }

    /// `pick_next_task` — both policies implement it.
    #[inline]
    pub(crate) fn pick_next_task(self, r: *mut rq) -> *mut sched_entity {
        // SAFETY: see `enqueue_task`.
        unsafe {
            match self {
                SchedClass::Idle => super::sched_idle::idle_pick_next_task(r),
                SchedClass::Fifo => super::sched_fifo::fifo_pick_next_task(r),
            }
        }
    }

    /// `put_prev_task` — no-op for idle (slot was NULL).
    #[inline]
    pub(crate) fn put_prev_task(self, r: *mut rq, se: *mut sched_entity) {
        // SAFETY: see `enqueue_task`.
        unsafe {
            match self {
                SchedClass::Idle => {}
                SchedClass::Fifo => super::sched_fifo::fifo_put_prev_task(r, se),
            }
        }
    }

    /// `set_next_task` — no-op for idle (slot was NULL).
    #[inline]
    pub(crate) fn set_next_task(self, r: *mut rq, se: *mut sched_entity) {
        // SAFETY: see `enqueue_task`.
        unsafe {
            match self {
                SchedClass::Idle => {}
                SchedClass::Fifo => super::sched_fifo::fifo_set_next_task(r, se),
            }
        }
    }

    /// `task_fork` — never populated by either policy today (both slots
    /// were NULL); returns `false` so `rq_task_fork`'s
    /// current-then-default fallback runs exactly as before.
    #[inline]
    pub(crate) fn task_fork(self, _r: *mut rq, _se: *mut sched_entity) -> bool {
        match self {
            SchedClass::Idle | SchedClass::Fifo => false,
        }
    }

    /// `task_dead` — never populated by either policy today (both slots
    /// were NULL); a no-op, matching the old `if let Some(fp)` fallback.
    #[inline]
    pub(crate) fn task_dead(self, _r: *mut rq, _se: *mut sched_entity) {
        match self {
            SchedClass::Idle | SchedClass::Fifo => {}
        }
    }
}

/// Native `struct sched_attr` (`kernel/inc/proc/rq_types.h`) — task
/// scheduling parameters for `sched_getattr()`/`sched_setattr()`.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct SchedAttr {
    pub size: u32,
    pub affinity_mask: cpumask_t,
    pub time_slice: u32,
    pub priority: c_int,
    pub flags: u32,
}

// P3-N7 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc `_Static_assert`
// probe (see the module note above).
const _: () = {
    // P3-10e niche proof: the `sched_class` fields (`Rq`, `SchedEntity`,
    // the `rqg` table) are `Option<SchedClass>` and are zero-initialised
    // (`write_bytes(0)` / zeroed kernel-stack storage / the `[None; _]`
    // static) precisely where the old `*mut sched_class` fields were NULL.
    // Pin the niche so a zero byte reads back as `None`, byte-identical to
    // the old NULL sentinel — the explicit `Idle = 1`/`Fifo = 2`
    // discriminants free value 0 for the niche, and this asserts Rust
    // actually lands `None` there.
    assert!(core::mem::size_of::<Option<SchedClass>>() == 1, "Option<SchedClass> must be one byte");
    let none_byte: u8 = unsafe { core::mem::transmute::<Option<SchedClass>, u8>(None) };
    assert!(none_byte == 0, "Option<SchedClass> None must be the zero byte");

    assert!(core::mem::size_of::<SchedAttr>() == 32, "sched_attr size");
    assert!(core::mem::align_of::<SchedAttr>() == 8, "sched_attr alignment");
    assert!(core::mem::offset_of!(SchedAttr, size) == 0, "sched_attr.size offset");
    assert!(core::mem::offset_of!(SchedAttr, affinity_mask) == 8, "sched_attr.affinity_mask offset");
    assert!(core::mem::offset_of!(SchedAttr, time_slice) == 16, "sched_attr.time_slice offset");
    assert!(core::mem::offset_of!(SchedAttr, priority) == 20, "sched_attr.priority offset");
    assert!(core::mem::offset_of!(SchedAttr, flags) == 24, "sched_attr.flags offset");
};

// ---------------- thread_state constants --------------------------------
const THREAD_UNUSED: thread_state = 0;
const THREAD_USED: thread_state = 1;
const THREAD_INTERRUPTIBLE: thread_state = 2;
const THREAD_KIILABLE: thread_state = 3;
const THREAD_TIMER: thread_state = 4;
const THREAD_KIILABLE_TIMER: thread_state = 5;
const THREAD_UNINTERRUPTIBLE: thread_state = 6;
const THREAD_WAKENING: thread_state = 7;
const THREAD_RUNNING: thread_state = 8;
const THREAD_STOPPED: thread_state = 9;
const THREAD_EXITING: thread_state = 10;
const THREAD_ZOMBIE: thread_state = 11;

const THREAD_FLAG_ONCHAN: u64 = 3;
const THREAD_FLAG_SELF_REAP: u64 = 6;

const EINTR: c_int = 4;

const CPU_FLAG_IN_ITR: u64 = 4;
const IPI_REASON_RESCHEDULE: c_int = 2;

/// NO-STANDALONE-FN marker type for the current-thread scheduler
/// orchestration API. The former free fns (`scheduler_yield`/`wakeup`/
/// `scheduler_wakeup[_interruptible/_stopped]`/`wakeup_on_chan`/
/// `context_switch_finish`/`scheduler_init`/`sched_holding`) operate on
/// the *current* CPU/thread and per-CPU statics — no struct receiver — so
/// the idiomatic non-standalone form is an associated fn on this marker
/// (`Scheduler::yield_now()`, `Scheduler::wakeup(p)`, ...). The private
/// `*_impl`/`*_inner` bodies stay as file-internal helpers (not
/// standalone API): the freeze/asm-critical logic is unchanged, only the
/// entry-point spelling moves onto `Scheduler`.
pub struct Scheduler;

// ---------------- extern C primitives -----------------------------------
// P3-D3c: `timer/timer_core.rs`'s `get_jiffs` is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone -- crate-path import.
use crate::timer::timer_core::get_jiffs;

unsafe extern "C" {
    fn __swtch_context(cur: *mut context, target: *mut context) -> *mut context;
}

// ===========================================================================
// Native layout — Wave P3-5 (the asm-offset-locked trio, part 2/3).
//
// This IS the kernel-wide Rust definition of `struct context`
// (`kernel/inc/trapframe.h`) now: `build.rs` blocklists the bindgen form
// and injects `pub use crate::proc::sched::Context as context;`, so
// every `crate::bindings::context` path (including this file's own
// `__swtch_context` extern above) resolves here.
//
// DANGER ZONE — ASM CONTRACT: kernel/proc/swtch.S's `__swtch_context`
// HARDCODES these offsets as literal displacements (`sd ra, 0(a0)` ..
// `sd s11, 104(a0)` and the matching `ld`s from a1) — it consumes no
// asm-offsets.h macro at all. The const asserts below cite those literal
// instructions; any field move fails the target build before an image
// can be linked. (asm-offsets.h also carries CONTEXT_* defines — no .S
// consumer, kept for reference and pinned by the same asserts.)
//
// Layout evidence (three-way agreement): the golden gcc-generated
// asm-offsets.h CONTEXT_* values + the riscv64-unknown-elf-gcc
// `_Static_assert` probe (rv64gc/lp64d, scratchpad
// p3_5_static_assert_probe.c) + the pre-nativization bindgen output
// (`#[repr(C)] #[repr(align(64))]`, derives Copy/Clone). C attribute
// `__ALIGNED_CACHELINE` == align(64); 14 u64 fields = 112 bytes,
// tail-padded to 128.
//
// Derive fidelity: bindgen derived Copy/Clone; the native keeps them,
// and build.rs's `NativeTypeCallbacks` answers Copy=Yes (the only
// by-value embedder, `sched_entity.context`, is native since P3-N7 and
// carries its own derives).
// ===========================================================================

/// Native `struct context` (`kernel/inc/trapframe.h`) — callee-saved
/// registers for kernel context switches, saved/restored by
/// `kernel/proc/swtch.S::__swtch_context`.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct Context {
    pub ra: u64,
    pub sp: u64,
    // callee-saved
    pub s0: u64,
    pub s1: u64,
    pub s2: u64,
    pub s3: u64,
    pub s4: u64,
    pub s5: u64,
    pub s6: u64,
    pub s7: u64,
    pub s8: u64,
    pub s9: u64,
    pub s10: u64,
    pub s11: u64,
}

// P3-5 hardcoded layout proof — each offset equals the literal
// displacement swtch.S uses (cited per assert), == the golden
// asm-offsets.h CONTEXT_* values, independently confirmed by the
// toolchain-gcc `_Static_assert` probe and the pre-nativization bindgen
// output. This is the drift tripwire for swtch.S's hardcoded contract.
const _: () = {
    assert!(core::mem::size_of::<Context>() == 128, "CONTEXT_SIZE (112 tail-padded to align 64)");
    assert!(core::mem::align_of::<Context>() == 64, "context alignment (__ALIGNED_CACHELINE)");
    assert!(core::mem::offset_of!(Context, ra) == 0, "swtch.S `sd ra, 0(a0)` / `ld ra, 0(a1)`");
    assert!(core::mem::offset_of!(Context, sp) == 8, "swtch.S `sd sp, 8(a0)` / `ld sp, 8(a1)`");
    assert!(core::mem::offset_of!(Context, s0) == 16, "swtch.S `sd s0, 16(a0)` / `ld s0, 16(a1)`");
    assert!(core::mem::offset_of!(Context, s1) == 24, "swtch.S `sd s1, 24(a0)` / `ld s1, 24(a1)`");
    assert!(core::mem::offset_of!(Context, s2) == 32, "swtch.S `sd s2, 32(a0)` / `ld s2, 32(a1)`");
    assert!(core::mem::offset_of!(Context, s3) == 40, "swtch.S `sd s3, 40(a0)` / `ld s3, 40(a1)`");
    assert!(core::mem::offset_of!(Context, s4) == 48, "swtch.S `sd s4, 48(a0)` / `ld s4, 48(a1)`");
    assert!(core::mem::offset_of!(Context, s5) == 56, "swtch.S `sd s5, 56(a0)` / `ld s5, 56(a1)`");
    assert!(core::mem::offset_of!(Context, s6) == 64, "swtch.S `sd s6, 64(a0)` / `ld s6, 64(a1)`");
    assert!(core::mem::offset_of!(Context, s7) == 72, "swtch.S `sd s7, 72(a0)` / `ld s7, 72(a1)`");
    assert!(core::mem::offset_of!(Context, s8) == 80, "swtch.S `sd s8, 80(a0)` / `ld s8, 80(a1)`");
    assert!(core::mem::offset_of!(Context, s9) == 88, "swtch.S `sd s9, 88(a0)` / `ld s9, 88(a1)`");
    assert!(
        core::mem::offset_of!(Context, s10) == 96,
        "swtch.S `sd s10, 96(a0)` / `ld s10, 96(a1)`"
    );
    assert!(
        core::mem::offset_of!(Context, s11) == 104,
        "swtch.S `sd s11, 104(a0)` / `ld s11, 104(a1)`"
    );
};

// P3-D3c: `bintree.rs`'s iterators are genuinely `unsafe fn`s now that
// their `#[no_mangle]` exports are gone; this file's original extern
// declarations were plain (non-`safe`) `fn`s, so every call site already
// sits in an `unsafe` context -- the plain `use` keeps them unchanged.

// P3-D3a: `page_free` is genuinely `unsafe fn` in `crate::mm::page` now
// that its `#[no_mangle]` export is gone; this file's original extern
// declaration asserted `safe fn` (usual FFI facade) and typed `order` as
// `c_int` rather than the real `u64` (non-negative small value under the
// old C ABI). The thin wrapper preserves both.
// ---------------- macros / helpers --------------------------------------
// NOTE: these two `macro_rules!` must stay at true file/module scope (not
// nested inside any `impl Scheduler { .. }` block below) -- macro_rules
// textual scoping does not escape its enclosing block, so nesting them
// inside an impl block would make `kassert!`/`kpanic!` invisible to every
// *other* `impl Scheduler` block later in the file.
macro_rules! kpanic { ($m:expr) => {{ xv6_panic(concat!($m, "\0").as_ptr() as *const c_char) }}; }
macro_rules! kassert { ($c:expr, $m:expr) => {{ if !($c) { kpanic!($m); } }}; }

/// SAFETY: `ptr` must originate from the paired `page_alloc`
/// (see `crate::mm::page::page_free`'s contract).
impl Scheduler {
#[inline]
fn page_free(ptr: *mut c_void, order: c_int) {
    unsafe { crate::mm::page::Page::page_free(ptr, order as u64) };
}

#[inline] fn cpu_access<'a>() -> CpuLocalRef<'a> {
    // SAFETY: `cpu_local_ptr()` indexes into statically-allocated, always-initialized
    // per-CPU storage; never null.
    unsafe { CpuLocalRef::assume(cpu_local_ptr()) }
}
#[inline] fn current_thread() -> *mut thread { Scheduler::cpu_access().proc_ptr() }

#[inline(always)]
fn ta_of<'a>(p: *mut thread) -> Option<ThreadAccess<'a>> {
    ThreadAccess::from_ptr(p)
}
#[inline(always)]
fn lk_of<'a>(p: *mut spinlock_t) -> Option<SpinLockRef<'a>> {
    SpinLockRef::from_ptr(p)
}

#[inline]
fn cpu_in_itr() -> bool {
    Scheduler::cpu_access().flag_set(CPU_FLAG_IN_ITR)
}

#[inline]
fn thread_state_get(p: *mut thread) -> thread_state {
    Scheduler::ta_of(p).map_or(THREAD_UNUSED, |t| t.state_load() as thread_state)
}
#[inline]
fn thread_state_set(p: *mut thread, s: thread_state) {
    if let Some(t) = Scheduler::ta_of(p) { t.state_store(s as u32); }
}
#[inline] fn state_is_sleeping(s: thread_state) -> bool {
    s == THREAD_INTERRUPTIBLE || s == THREAD_UNINTERRUPTIBLE
        || s == THREAD_KIILABLE || s == THREAD_TIMER || s == THREAD_KIILABLE_TIMER
}
#[inline] fn state_is_killable(s: thread_state) -> bool {
    s == THREAD_KIILABLE || s == THREAD_KIILABLE_TIMER || s == THREAD_INTERRUPTIBLE
}
#[inline] fn state_is_timer(s: thread_state) -> bool {
    s == THREAD_TIMER || s == THREAD_KIILABLE_TIMER || s == THREAD_INTERRUPTIBLE
}
#[inline] fn state_is_interruptible(s: thread_state) -> bool { s == THREAD_INTERRUPTIBLE }
#[inline] fn state_is_running(s: thread_state) -> bool { s == THREAD_RUNNING }
#[inline] fn state_is_stopped(s: thread_state) -> bool { s == THREAD_STOPPED }
#[inline] fn state_is_zombie(s: thread_state) -> bool { s == THREAD_ZOMBIE }

#[inline] fn thread_sleeping(p: *mut thread) -> bool { Scheduler::state_is_sleeping(Scheduler::thread_state_get(p)) }
#[inline] fn thread_running(p: *mut thread) -> bool { Scheduler::state_is_running(Scheduler::thread_state_get(p)) }
#[inline] fn thread_zombie(p: *mut thread) -> bool { Scheduler::state_is_zombie(Scheduler::thread_state_get(p)) }
#[inline] fn thread_stopped(p: *mut thread) -> bool { Scheduler::state_is_stopped(Scheduler::thread_state_get(p)) }
#[inline] fn thread_killable(p: *mut thread) -> bool { Scheduler::state_is_killable(Scheduler::thread_state_get(p)) }
#[inline] fn thread_timer(p: *mut thread) -> bool { Scheduler::state_is_timer(Scheduler::thread_state_get(p)) }
#[inline] fn thread_interruptible(p: *mut thread) -> bool { Scheduler::state_is_interruptible(Scheduler::thread_state_get(p)) }

#[inline]
fn thread_set_flag(p: *mut thread, bit: u64) {
    if let Some(t) = Scheduler::ta_of(p) { t.flags_set_bit(bit); }
}
#[inline]
fn thread_clear_flag(p: *mut thread, bit: u64) {
    if let Some(t) = Scheduler::ta_of(p) { t.flags_clear_bit(bit); }
}
#[inline]
fn thread_test_flag(p: *mut thread, bit: u64) -> bool {
    Scheduler::ta_of(p).map_or(false, |t| t.flags_test_bit(bit))
}

#[inline]
fn thread_from_context(ctx: *mut context) -> *mut thread {
    let off = offset_of!(sched_entity, context);
    let se = (ctx as *mut u8).wrapping_sub(off) as *mut sched_entity;
    // SAFETY: `se` is derived by pointer arithmetic assuming `ctx` points to the
    // `context` field of a live `sched_entity` -- the documented contract of
    // `thread_from_context`, a kernel-internal context-switch helper always
    // invoked with such a pointer.
    unsafe { SchedEntityRef::assume(se) }.thread_ptr()
}
}

// ---------------- static lock + chan queue ------------------------------
#[repr(transparent)]
struct SyncBuf<T>(UnsafeCell<MaybeUninit<T>>);
// SAFETY: `SyncBuf<T>` backs the two statics below only. `SLEEP_LOCK`
// is written in full by its one-shot `spin_init` call before any other
// use, then only ever touched through `spin_lock`/`spin_unlock`'s own
// internal synchronization. `CHAN_QUEUE_ROOT` is written by its
// one-shot tree-init call and every subsequent mutation happens while
// `SLEEP_LOCK` is held (the sleep/wakeup "chan queue" protocol) — the
// spinlock is the real serialization mechanism for both.
unsafe impl<T> Sync for SyncBuf<T> {}

static SLEEP_LOCK: SyncBuf<spinlock_t> = SyncBuf(UnsafeCell::new(MaybeUninit::uninit()));
static CHAN_QUEUE_ROOT: SyncBuf<ttree_t> = SyncBuf(UnsafeCell::new(MaybeUninit::uninit()));

impl Scheduler {
#[inline] fn sleep_lock_ptr() -> *mut spinlock_t {
    SLEEP_LOCK.0.get() as *mut spinlock_t
}
#[inline] fn chan_queue_ptr() -> *mut ttree_t {
    CHAN_QUEUE_ROOT.0.get() as *mut ttree_t
}

#[inline]
fn sleep_lock_ref<'a>() -> SpinLockRef<'a> {
    // SAFETY: `Scheduler::sleep_lock_ptr()` points into statically-allocated, always-initialized
    // storage (`SLEEP_LOCK`); never null.
    unsafe { SpinLockRef::assume(Scheduler::sleep_lock_ptr()) }
}

fn chan_queue_init() {
    if let Some(r) = TtreeRef::from_ptr(Scheduler::chan_queue_ptr()) {
        r.init(c"chan_queue_root".as_ptr(), Scheduler::sleep_lock_ptr());
    }
}

// ---------------- sleep lock helpers ------------------------------------
fn chan_holding_impl() -> c_int { Scheduler::sleep_lock_ref().holding() as c_int }
fn sleep_lock_impl() { Scheduler::sleep_lock_ref().lock(); }
fn sleep_unlock_impl() { Scheduler::sleep_lock_ref().unlock(); }
fn sleep_lock_irqsave_impl() -> c_int { Scheduler::sleep_lock_ref().lock_irqsave() }
fn sleep_unlock_irqrestore_impl(state: c_int) { Scheduler::sleep_lock_ref().unlock_irqrestore(state); }
fn sched_holding_impl() -> c_int { Rq::holding_current() }
}

// P3-1B: only cross-file caller is `timer/timer_core.rs::clockintr` (now a
// direct crate-path `use`, no more `extern` redeclaration) -- demoted.
impl Scheduler { pub(crate) fn holding() -> c_int { Scheduler::sched_holding_impl() } }

impl Scheduler {
#[inline]
fn sched_assert_holding() {
    kassert!(Scheduler::sched_holding_impl() != 0, "rq_lock must be held");
}
#[inline]
fn sched_assert_unholding() {
    kassert!(Scheduler::sched_holding_impl() == 0, "rq_lock must not be held");
}
}

// ---------------- scheduler init ----------------------------------------
impl Scheduler {
fn scheduler_init_impl() {
    unsafe {
        RawSpinlock::init(Scheduler::sleep_lock_ptr(), c"xv6_schport_sleep_lock".as_ptr() as *mut c_char);
        Scheduler::chan_queue_init();
        Rq::global_init();
    }
}
}

// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
impl Scheduler { pub(crate) fn init() { Scheduler::scheduler_init_impl() } }

// ---------------- pick next ---------------------------------------------
impl Scheduler {
fn sched_pick_next() -> *mut thread {
    Scheduler::sched_assert_holding();
    let rq = Rq::pick_next();
    if is_err_or_null(rq) { return core::ptr::null_mut(); }
    // SAFETY: `rq` is proven non-null (and not an errptr) by the
    // `is_err_or_null` guard immediately above.
    let se = unsafe { RqRef::assume(rq) }.pick_next();
    if se.is_null() { return core::ptr::null_mut(); }
    let cur = Scheduler::current_thread();
    // SAFETY: `cur` is `Scheduler::current_thread()`, the currently running thread; the currently
    // running thread's pointer (from `xv6_current_thread()`/`current()`) is a
    // kernel-wide invariant: always non-null while executing kernel code on
    // behalf of a thread.
    let cur_se = unsafe { ThreadAccess::assume(cur) }.sched_entity_ptr();
    // SAFETY: `cur_se` is `cur`'s embedded scheduling entity: every thread that is
    // actually running (as `cur` is here) has a live, allocated `sched_entity`
    // by kernel invariant.
    let this_priority = unsafe { SchedEntityRef::assume(cur_se) }.priority();
    // SAFETY: `se` is checked non-null immediately above (`if se.is_null() { return
    // ...; }`).
    let next = unsafe { SchedEntityRef::assume(se) };
    let next_priority = next.priority();
    if Scheduler::thread_running(cur) && this_priority < next_priority {
        return cur;
    }
    let p = next.thread_ptr();
    kassert!(!p.is_null(), "sched_pick_next: se->thread is NULL");
    next.set_next();
    next.on_cpu_store_release(1);
    let pstate = Scheduler::thread_state_get(p);
    if pstate == THREAD_WAKENING {
        Scheduler::thread_state_set(p, THREAD_RUNNING);
    } else if pstate != THREAD_RUNNING {
        kassert!(pstate != THREAD_INTERRUPTIBLE, "try to schedule an interruptible thread");
        kassert!(pstate != THREAD_UNINTERRUPTIBLE, "try to schedule an uninterruptible thread");
        kassert!(pstate != THREAD_UNUSED, "try to schedule an uninitialized thread");
        kassert!(pstate != THREAD_ZOMBIE, "found a zombie thread in ready queue");
        kpanic!("try to schedule unknown thread state");
    }
    p
}
}

// ---------------- switch_to ---------------------------------------------
impl Scheduler {
fn switch_to_impl(cur: *mut thread, target: *mut thread) -> *mut thread {
    unsafe {
        let now = get_jiffs();
        Scheduler::cpu_access().store_rcu_timestamp_release(now);
        Scheduler::cpu_access().set_proc(target);
        let cur_se = ThreadAccess::from_raw(cur).unwrap_unchecked().sched_entity_ptr();
        let target_se = ThreadAccess::from_raw(target).unwrap_unchecked().sched_entity_ptr();
        let prev_ctx = __swtch_context(
            SchedEntityRef::from_raw(cur_se).unwrap_unchecked().context_ptr(),
            SchedEntityRef::from_raw(target_se).unwrap_unchecked().context_ptr(),
        );
        Scheduler::thread_from_context(prev_ctx)
    }
}

fn switch_to_internal(p: *mut thread) -> *mut thread {
    Scheduler::sched_assert_holding();
    kassert!(!intr_get(), "Interrupts must be disabled before switching to a thread");
    let proc = Scheduler::current_thread();
    let intena = Scheduler::cpu_access().intena();
    let mut spin_depth_expected = 1;
    if Scheduler::chan_holding_impl() != 0 { spin_depth_expected += 1; }
    kassert!(Scheduler::cpu_access().noff() == 0, "Thread must not hold any other locks when yielding");
    kassert!(Scheduler::cpu_access().spin_depth() == spin_depth_expected,
        "Thread must hold and only hold the rq lock when yielding");

    let prev = Scheduler::switch_to_impl(proc, p);

    kassert!(!intr_get(), "Interrupts must be disabled after switching");
    kassert!(Scheduler::current_thread() == proc, "Yield returned to a different thread");
    kassert!(Scheduler::thread_running(proc), "Thread state must be RUNNING after yield");
    Scheduler::cpu_access().set_intena(intena);
    prev
}
}

// ---------------- scheduler_yield ---------------------------------------
impl Scheduler {
fn scheduler_yield_inner() {
    __do_timer_tick();
    // SAFETY: `rq_flush_wake_list` (kernel/proc/rq.rs) is `unsafe` because
    // it walks the wake list through raw pointers; `cpuid_rs()` always
    // yields a valid, in-range CPU id into the statically-allocated,
    // always-initialized per-CPU `rq_percpu` storage this function
    // indexes, and the function itself range-checks it again before
    // touching anything.
    unsafe { Rq::flush_wake_list(cpuid_rs()); }

    let intr = Rq::lock_current_irqsave();
    let proc = Scheduler::current_thread();

    kassert!(!Scheduler::cpu_in_itr(), "Cannot yield CPU in interrupt context");

    let mut p = Scheduler::sched_pick_next();

    if p == Scheduler::current_thread() {
        Rq::unlock_current_irqrestore(intr);
    } else if p.is_null() {
        let idle = Scheduler::cpu_access().idle_thread_ptr();
        if proc == idle {
            Rq::unlock_current_irqrestore(intr);
        } else {
            p = idle;
            kassert!(!p.is_null(), "Idle process is NULL");
            Scheduler::context_switch_prepare_impl(proc, p);
            let prev = Scheduler::switch_to_internal(p);
            Scheduler::context_switch_finish_impl(prev, Scheduler::current_thread(), intr);
        }
    } else {
        Scheduler::context_switch_prepare_impl(proc, p);
        let prev = Scheduler::switch_to_internal(p);
        Scheduler::context_switch_finish_impl(prev, Scheduler::current_thread(), intr);
    }

    // SAFETY: see the matching call above.
    unsafe { Rq::flush_wake_list(cpuid_rs()); }
    // Plain safe fn as of P3-D3b (its `#[no_mangle] extern "C"` export
    // is gone).
    Rcu::check_callbacks();
}
}

impl Scheduler { pub(crate) fn yield_now() { Scheduler::scheduler_yield_inner() } }

// ---------------- scheduler_sleep ---------------------------------------
impl Scheduler {
fn scheduler_sleep_impl(lk: Option<SpinLockRef<'_>>, sleep_state: thread_state) {
    let intr = intr_off_save();
    let proc = Scheduler::current_thread();
    kassert!(!proc.is_null(), "PCB is NULL");
    kassert!(Scheduler::state_is_sleeping(sleep_state),
        "Thread must be in INTERRUPTIBLE or UNINTERRUPTIBLE state to sleep");
    Scheduler::thread_state_set(proc, sleep_state);
    let lk_holding = lk.is_some_and(|l| l.holding());
    if lk_holding {
        lk.expect("BUG: scheduler_sleep lk_holding true but lk is None").unlock();
    }
    Scheduler::scheduler_yield_inner();
    if lk_holding {
        lk.expect("BUG: scheduler_sleep lk_holding true but lk is None").lock();
    }
    intr_restore(intr);
}
}

// ---------------- wakeup helpers ----------------------------------------
impl Scheduler {
#[inline]
fn scheduler_wakeup_assert(p: *mut thread) {
    kassert!(!p.is_null(), "Cannot wake up a NULL process");
    if let Some(t) = Scheduler::ta_of(p) {
        kassert!(!t.lock_ref().holding(),
            "Process lock must not be held when waking up a process");
    }
    kassert!(Scheduler::sched_holding_impl() == 0,
        "Scheduler lock must not be held when waking up a process");
}

#[inline]
fn do_scheduler_wakeup_safe(p: *mut thread, from_stopped: bool) {
    unsafe { Scheduler::do_scheduler_wakeup(p, from_stopped); }
}

unsafe fn do_scheduler_wakeup(p: *mut thread, from_stopped: bool) {
    unsafe {
        let se = (*p).sched_entity;
        RawSpinlock::lock(&raw mut (*se).pi_lock);

        if p == Scheduler::current_thread() {
            smp_rmb();
            if from_stopped {
                RawSpinlock::unlock(&raw mut (*se).pi_lock);
                return;
            }
            let old_state = smp_load_acquire_state(&(*p).state as *const _);
            if !Scheduler::state_is_sleeping(old_state) {
                RawSpinlock::unlock(&raw mut (*se).pi_lock);
                return;
            }
            smp_store_release_state(&raw mut (*p).state, THREAD_RUNNING);
            RawSpinlock::unlock(&raw mut (*se).pi_lock);
            return;
        }

        smp_rmb();
        let mut old_state = smp_load_acquire_state(&(*p).state as *const _);
        if from_stopped {
            if old_state != THREAD_STOPPED {
                RawSpinlock::unlock(&raw mut (*se).pi_lock);
                return;
            }
        } else if !Scheduler::state_is_sleeping(old_state) {
            RawSpinlock::unlock(&raw mut (*se).pi_lock);
            return;
        }

        // retry loop
        loop {
            push_off();
            let rq = Rq::select_task_rq(se, (*se).affinity_mask);
            kassert!(!is_err_or_null(rq), "do_scheduler_wakeup: rq_select_task_rq failed");
            let mut origin_cpuid = smp_load_acquire_i32(&(*se).cpu_id as *const _);
            let target_cpu = (*rq).cpu_id;
            if origin_cpuid < 0 { origin_cpuid = target_cpu; }

            if Rq::trylock_two(origin_cpuid, target_cpu) == 0 {
                pop_off();
                RawSpinlock::unlock(&raw mut (*se).pi_lock);
                for _ in 0..10 { cpu_relax(); }
                RawSpinlock::lock(&raw mut (*se).pi_lock);
                old_state = smp_load_acquire_state(&(*p).state as *const _);
                if from_stopped {
                    if old_state != THREAD_STOPPED {
                        RawSpinlock::unlock(&raw mut (*se).pi_lock);
                        return;
                    }
                } else if !Scheduler::state_is_sleeping(old_state) {
                    RawSpinlock::unlock(&raw mut (*se).pi_lock);
                    return;
                }
                continue;
            }
            pop_off();

            let current_cpuid = smp_load_acquire_i32(&(*se).cpu_id as *const _);
            if current_cpuid >= 0 && current_cpuid != origin_cpuid {
                Rq::unlock_two(origin_cpuid, target_cpu);
                RawSpinlock::unlock(&raw mut (*se).pi_lock);
                RawSpinlock::lock(&raw mut (*se).pi_lock);
                old_state = smp_load_acquire_state(&(*p).state as *const _);
                if from_stopped {
                    if old_state != THREAD_STOPPED {
                        RawSpinlock::unlock(&raw mut (*se).pi_lock);
                        return;
                    }
                } else if !Scheduler::state_is_sleeping(old_state) {
                    RawSpinlock::unlock(&raw mut (*se).pi_lock);
                    return;
                }
                continue;
            }

            smp_store_release_state(&raw mut (*p).state, THREAD_WAKENING);

            if smp_load_acquire_i32(&(*se).on_rq as *const _) != 0 {
                smp_store_release_state(&raw mut (*p).state, THREAD_RUNNING);
                RawSpinlock::unlock(&raw mut (*se).pi_lock);
                Rq::unlock_two(origin_cpuid, target_cpu);
                return;
            }

            if smp_load_acquire_i32(&(*se).on_cpu as *const _) != 0 {
                Rq::add_wake_list(origin_cpuid, se);
                RawSpinlock::unlock(&raw mut (*se).pi_lock);
                Rq::unlock_two(origin_cpuid, target_cpu);
                ipi_send_single(origin_cpuid, IPI_REASON_RESCHEDULE);
                return;
            }

            RqRef::assume(rq).enqueue(se);
            RawSpinlock::unlock(&raw mut (*se).pi_lock);
            Rq::unlock_two(origin_cpuid, target_cpu);
            return;
        }
    }
}

fn scheduler_wakeup_impl(p: *mut thread) {
    Scheduler::scheduler_wakeup_assert(p);
    if !Scheduler::thread_sleeping(p) { return; }
    Scheduler::do_scheduler_wakeup_safe(p, false);
}
fn scheduler_wakeup_timeout_impl(p: *mut thread) {
    Scheduler::scheduler_wakeup_assert(p);
    if !Scheduler::thread_timer(p) { return; }
    Scheduler::do_scheduler_wakeup_safe(p, false);
}
fn scheduler_wakeup_killable_impl(p: *mut thread) {
    Scheduler::scheduler_wakeup_assert(p);
    if !Scheduler::thread_killable(p) { return; }
    Scheduler::do_scheduler_wakeup_safe(p, false);
}
fn scheduler_wakeup_interruptible_impl(p: *mut thread) {
    Scheduler::scheduler_wakeup_assert(p);
    if !Scheduler::thread_interruptible(p) { return; }
    Scheduler::do_scheduler_wakeup_safe(p, false);
}
fn scheduler_wakeup_stopped_impl(p: *mut thread) {
    Scheduler::scheduler_wakeup_assert(p);
    if !Scheduler::thread_stopped(p) { return; }
    Scheduler::do_scheduler_wakeup_safe(p, true);
}
}

impl Scheduler {
/// Wake a known-non-null thread from a sleeping state (was the free
/// `scheduler_wakeup`).
pub(crate) fn wakeup_thread(p: *mut thread) { Scheduler::scheduler_wakeup_impl(p) }

/// Wake a known-non-null interruptible thread (was the free
/// `scheduler_wakeup_interruptible`).
pub(crate) fn wakeup_interruptible_thread(p: *mut thread) { Scheduler::scheduler_wakeup_interruptible_impl(p) }

/// Wake a stopped thread (was the free `scheduler_wakeup_stopped`).
pub(super) fn wakeup_stopped(p: *mut thread) { Scheduler::scheduler_wakeup_stopped_impl(p) }
}

// ---------------- sleep_on_chan / wakeup_on_chan ------------------------
impl Scheduler {
fn sleep_on_chan_common(chan: *mut c_void, lk: Option<SpinLockRef<'_>>, state: thread_state) -> c_int {
    let intr = Scheduler::sleep_lock_irqsave_impl();
    let cur = Scheduler::current_thread();
    kassert!(!cur.is_null(), "PCB is NULL");
    kassert!(!chan.is_null(), "Cannot sleep on a NULL channel");
    // SAFETY: `cur` is proven non-null by the diverging `kassert!` immediately above.
    unsafe { ThreadAccess::assume(cur) }.set_chan(chan);
    Scheduler::thread_set_flag(cur, THREAD_FLAG_ONCHAN);
    Scheduler::thread_state_set(cur, state);

    let lk_holding = lk.is_some_and(|l| l.holding());
    if lk_holding {
        lk.expect("BUG: sleep_on_chan_common lk_holding true but lk is None").unlock();
    }

    let ret = TtreeRef::from_ptr(Scheduler::chan_queue_ptr())
        .map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait(chan as u64, core::ptr::null_mut(), core::ptr::null_mut()));

    Scheduler::sleep_lock_irqsave_impl();
    Scheduler::thread_clear_flag(cur, THREAD_FLAG_ONCHAN);
    // SAFETY: `cur` is proven non-null by the diverging `kassert!` above.
    unsafe { ThreadAccess::assume(cur) }.set_chan(core::ptr::null_mut());
    Scheduler::sleep_unlock_irqrestore_impl(intr);

    if lk_holding {
        lk.expect("BUG: sleep_on_chan_common lk_holding true but lk is None").lock();
    }
    ret
}

fn sleep_on_chan_impl(chan: *mut c_void, lk: Option<SpinLockRef<'_>>) {
    let _ = Scheduler::sleep_on_chan_common(chan, lk, THREAD_UNINTERRUPTIBLE);
}

fn sleep_on_chan_interruptible_impl(chan: *mut c_void, lk: Option<SpinLockRef<'_>>) -> c_int {
    let ret = Scheduler::sleep_on_chan_common(chan, lk, THREAD_INTERRUPTIBLE);
    if ret != 0 { -EINTR } else { 0 }
}

fn wakeup_on_chan_impl(chan: *mut c_void) {
    Scheduler::sleep_lock_impl();
    let _ = TtreeRef::from_ptr(Scheduler::chan_queue_ptr())
        .map_or(-(crate::bindings::EINVAL as c_int), |r| r.wakeup_key(chan as u64, 0, 0));
    Scheduler::sleep_unlock_impl();
}
}

pub(crate) fn sleep_on_chan(chan: *mut c_void, lk: *mut spinlock_t) {
    Scheduler::sleep_on_chan_impl(chan, Scheduler::lk_of(lk))
}

pub(crate) fn sleep_on_chan_interruptible(chan: *mut c_void, lk: *mut spinlock_t) -> c_int {
    Scheduler::sleep_on_chan_interruptible_impl(chan, Scheduler::lk_of(lk))
}

impl Scheduler { pub(crate) fn wakeup_on_chan(chan: *mut c_void) { Scheduler::wakeup_on_chan_impl(chan) } }

// ---------------- dump --------------------------------------------------
// Provide our own state-to-str (inline static in C header — re-implement).
const STATE_STRS: &[(thread_state, &[u8])] = &[
    (THREAD_UNUSED,            b"UNUSED\0"),
    (THREAD_USED,              b"USED\0"),
    (THREAD_INTERRUPTIBLE,     b"INTERRUPTIBLE\0"),
    (THREAD_KIILABLE,          b"KILLABLE\0"),
    (THREAD_TIMER,             b"TIMER\0"),
    (THREAD_KIILABLE_TIMER,    b"KILLABLE_TIMER\0"),
    (THREAD_UNINTERRUPTIBLE,   b"UNINTERRUPTIBLE\0"),
    (THREAD_WAKENING,          b"WAKENING\0"),
    (THREAD_RUNNING,           b"RUNNING\0"),
    (THREAD_STOPPED,           b"STOPPED\0"),
    (THREAD_EXITING,           b"EXITING\0"),
    (THREAD_ZOMBIE,            b"ZOMBIE\0"),
];

impl Scheduler {
fn state_str(s: thread_state) -> *const c_char {
    STATE_STRS
        .iter()
        .find(|(k, _)| *k == s)
        .map_or_else(|| c"UNKNOWN".as_ptr(), |(_, v)| v.as_ptr() as *const c_char)
}
}

impl Scheduler {
unsafe fn scheduler_dump_chan_queue() {
    crate::kprintln!("Channel Queue Dump:");
    unsafe {
        let root = &raw mut (*Scheduler::chan_queue_ptr()).root;
        // P3-N2: `tnode` IS `thread_queue::Tnode` now; the anonymous
        // union field is `u`, and the tree arm is the native
        // `TnodeTreeArm` (its `entry` sits at arm offset 0).
        let tree_entry_off = offset_of!(tnode, u)
            + offset_of!(crate::proc::thread_queue::TnodeTreeArm, entry);
        // Read-only walk (dump only, no tree mutation) -> the additive
        // `RawRbRoot::iter()` in-order iterator replaces the hand-rolled
        // `rb_first_node`/`rb_next_node` cursor.
        for node in (*root).iter() {
            let tn = (node as *mut u8).wrapping_sub(tree_entry_off) as *mut tnode_t;
            let proc = TnodeRef::from_ptr(tn).map_or(core::ptr::null_mut(), |r| r.thread_ptr());
            if proc.is_null() {
                crate::kprintln!("  Process: NULL");
            } else {
                let t = ThreadAccess::from_raw(proc).unwrap_unchecked();
                crate::kprintln!(
                    "Chan: {},  Proc: {} (PID: {}, State: {})",
                    crate::printf::Ptr((*proc).chan as u64),
                    crate::printf::Cs((*proc).name.as_ptr()),
                    t.pid(),
                    crate::printf::Cs(Scheduler::state_str(Scheduler::thread_state_get(proc))),
                );
            }
        }
    }
}
}

use crate::bindings::tnode;

// ---------------- wakeup wrappers ---------------------------------------
impl Scheduler {
fn wakeup_impl(p: *mut thread) {
    if p.is_null() { return; }
    Scheduler::scheduler_wakeup_impl(p);
}
fn wakeup_timeout_impl(p: *mut thread) {
    if p.is_null() { return; }
    Scheduler::scheduler_wakeup_timeout_impl(p);
}
fn wakeup_killable_impl(p: *mut thread) {
    if p.is_null() { return; }
    Scheduler::scheduler_wakeup_killable_impl(p);
}
fn wakeup_interruptible_impl(p: *mut thread) {
    if p.is_null() { return; }
    Scheduler::scheduler_wakeup_interruptible_impl(p);
}
}

impl Scheduler {
/// Null-safe generic wakeup (was the free `wakeup`).
pub(crate) fn wakeup(p: *mut thread) { Scheduler::wakeup_impl(p) }

/// Null-safe interruptible wakeup (was the free `wakeup_interruptible`).
pub(crate) fn wakeup_interruptible(p: *mut thread) { Scheduler::wakeup_interruptible_impl(p) }
}

impl Scheduler {
fn sys_dumpchan_impl() -> u64 {
    Scheduler::sleep_lock_impl();
    unsafe { Scheduler::scheduler_dump_chan_queue(); }
    Scheduler::sleep_unlock_impl();
    0
}
}

// P3-1B: referenced only as a fn-pointer value in `irq/syscall.rs`'s
// dispatch table (crate-path `use`, not a link-name lookup) -- demoted.
pub(crate) extern "C" fn sys_dumpchan() -> u64 { Scheduler::sys_dumpchan_impl() }

// ---------------- context switch ----------------------------------------
impl Scheduler {
fn context_switch_prepare_impl(prev: *mut thread, next: *mut thread) {
    kassert!(!prev.is_null(), "Previous process is NULL");
    kassert!(!next.is_null(), "Next process is NULL");
    Scheduler::sched_assert_holding();
    // SAFETY: `next` is proven non-null by the diverging `kassert!` immediately above.
    let next_se = unsafe { ThreadAccess::assume(next) }.sched_entity_ptr();
    // SAFETY: `next_se` is `next`'s embedded scheduling entity: every thread reaching
    // a context switch has a live, allocated `sched_entity` by kernel
    // invariant.
    let next_se_ref = unsafe { SchedEntityRef::assume(next_se) };
    next_se_ref.on_cpu_store_release(1);
    next_se_ref.set_cpu_id(cpuid_rs());
    if Scheduler::thread_zombie(prev) {
        // SAFETY: `prev` is proven non-null by the diverging `kassert!` above;
        // its embedded `sched_entity` is live by kernel invariant.
        unsafe { SchedEntityRef::assume(ThreadAccess::assume(prev).sched_entity_ptr()) }.task_dead();
    }
}

fn context_switch_finish_impl(prev: *mut thread, next: *mut thread, intr: c_int) {
    kassert!(!prev.is_null(), "Previous process is NULL");
    kassert!(!next.is_null(), "Next process is NULL");
    let pstate = Scheduler::thread_state_get(prev);
    // SAFETY: `prev` is proven non-null by the diverging `kassert!` above.
    let prev_access = unsafe { ThreadAccess::assume(prev) };
    let se = prev_access.sched_entity_ptr();
    let idle = Scheduler::cpu_access().idle_thread_ptr();

    if prev != idle {
        if Scheduler::state_is_running(pstate) {
            // SAFETY: `se` is `prev_access`'s embedded scheduling entity,
            // field-derived from the already-valid `prev_access` handle above.
            unsafe { SchedEntityRef::assume(se) }.put_prev();
        } else if Scheduler::state_is_sleeping(pstate) || Scheduler::state_is_stopped(pstate) {
            // SAFETY: `se` is `prev_access`'s embedded scheduling entity, field-
            // derived from the already-valid `prev_access` handle above.
            let se_rq = unsafe { SchedEntityRef::assume(se) }.rq_ptr();
            if !se_rq.is_null() {
                // SAFETY: `se_rq` is proven non-null immediately above.
                unsafe { RqRef::assume(se_rq) }.dequeue(se);
            }
        }
    }

    if Scheduler::thread_test_flag(prev, THREAD_FLAG_SELF_REAP) {
        let (kstack_addr, kstack_order) = (prev_access.kstack_addr(), prev_access.kstack_order());
        Scheduler::page_free(kstack_addr as *mut c_void, kstack_order);
        // prev is now dangling -- do not touch.
    } else {
        // SAFETY: `prev_access` is already a valid handle (constructed above);
        // `.sched_entity_ptr()` is a field-address derived from it.
        unsafe { SchedEntityRef::assume(prev_access.sched_entity_ptr()) }.on_cpu_store_release(0);
    }

    if Scheduler::chan_holding_impl() != 0 {
        Scheduler::sleep_unlock_irqrestore_impl(0);
    }

    Rq::unlock_current_irqrestore(intr);
}
}

impl Scheduler {
/// Finish a context switch: put/dequeue prev, free self-reap stacks,
/// release the rq lock (was the free `context_switch_finish`).
pub(super) fn context_switch_finish(prev: *mut thread, next: *mut thread, intr: c_int) {
    Scheduler::context_switch_finish_impl(prev, next, intr)
}
}

// The `xv6_schport_*` C-ABI alias layer that used to front chan_holding/
// sleep_lock[_irqsave]/sleep_unlock[_irqrestore]/sched_holding/
// scheduler_init/switch_to/scheduler_yield/scheduler_sleep/
// scheduler_wakeup[_timeout/_killable/_interruptible/_stopped]/
// sleep_on_chan[_interruptible]/wakeup_on_chan/scheduler_dump_chan_queue/
// wakeup[_timeout/_killable/_interruptible]/sys_dumpchan/
// context_switch_prepare/context_switch_finish was collapsed in the
// P3-1B2 sweep: every caller now invokes these canonical names directly
// (crate-path, no FFI hop).
