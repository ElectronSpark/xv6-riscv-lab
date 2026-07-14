//! In-kernel Rust runtime test suite for the real workqueue implementation
//! (`kernel/proc/workqueue.rs`).
//!
//! Phase 4 (`docs/rustify/test_port_plan.md`) re-home of the retired
//! `test/src/ut_workqueue_main.c` cmocka suite. Per the plan's own
//! disposition table, that suite was "mock-only — never tested the real
//! impl": its five cases called `pcache_test_run_pending_work()` /
//! `pcache_test_fail_next_queue_work()`, host-only synchronous-drain and
//! failure-injection hooks with no real-kernel equivalent, against a
//! `workqueue.h`/`workqueue.c` pairing that (before this Rust port) was
//! entirely separate code from what ships in the kernel. None of its five
//! cases port over as-is — every case below is **new** coverage of the real
//! `kernel/proc/workqueue.rs`, exercising its actual manager/worker kthreads,
//! real slab-backed `workqueue`/`work_struct` allocation, and the real
//! idle-queue wakeup protocol, rather than a synchronous host stand-in.
//!
//! ## Case list
//!
//!   * T1 `ctor_invoked_on_create` — `workqueue_create_with_callbacks`
//!     synchronously invokes the ctor callback before returning.
//!   * T2 `dtor_invoked_once_on_kill` — `workqueue_kill` invokes the dtor
//!     exactly once, and is idempotent (a second kill is a safe no-op).
//!   * T3 `work_struct_field_integrity` — `create_work_struct_ex`
//!     round-trips `data`/`flags` correctly, and rejects an invalid flags
//!     mask by returning `NULL` (not by panicking — `init_work_struct_ex`
//!     *does* panic on invalid flags per its own doc, so this suite only
//!     ever drives work-struct construction through the `create_*`
//!     entry points, which fail closed instead).
//!   * T4 `queue_work_runs_async` — `queue_work` onto a live workqueue is
//!     picked up and executed by a real worker kthread, observed via a
//!     bounded poll (no synchronous "run pending work" hook exists for the
//!     real implementation — nor should it, since the whole point of a
//!     workqueue is asynchronous execution).
//!   * T5 `queue_work_fails_when_killed` — `queue_work` on an
//!     already-killed workqueue deterministically returns `false`. This is
//!     the real suite's replacement for the retired
//!     `pcache_test_fail_next_queue_work()` hook: instead of a mock forcing
//!     an arbitrary failure, this exercises one of `queue_work`'s two real
//!     failure conditions (the other, "work already enqueued", is
//!     inherently racy to hit deterministically and is not attempted here).
//!   * T6 `multi_worker_concurrency` — `max_active > 1` lets several real
//!     worker kthreads drain a burst of queued work concurrently; verifies
//!     every item runs exactly once.
//!   * T7 `free_after_run_stress` — `WORK_STRUCT_FLAG_FREE_AFTER_RUN`
//!     correctly self-frees a batch of heap work structs after they run,
//!     with no caller-side `free_work_struct` call, and without crashing.
//!
//! Not attempted: deterministically forcing the *drain* path (a killed
//! workqueue's idle workers running still-queued `RUN_ON_DRAIN` work through
//! the `fault` callback instead of `func`, see `execute_work` in
//! `workqueue.rs`). `workqueue_create[_with_callbacks]` immediately spins up
//! `min_active` (>= `WORKQUEUE_DEFAULT_MIN_ACTIVE` = 2) idle workers before
//! returning, so by the time a test can call `queue_work` there is usually
//! already an idle worker waiting — racing a kill against a specific queued
//! item's dequeue is not controllable from outside the scheduler without a
//! whitebox seam in `workqueue.rs` itself (out of this task's touch scope).
//! Flagged as a documented coverage gap rather than a flaky/non-deterministic
//! test.
//!
//! Note: `workqueue_kill` has no matching "destroy"/free for the
//! `struct workqueue` itself (only `work_struct`s have `free_work_struct`) —
//! this is a real, pre-existing property of the API (the one production
//! workqueue, the global pcache flush queue, is meant to live for the
//! kernel's lifetime), not a bug in this test file. Each case here leaks one
//! small slab-backed `workqueue` object; at 7 cases total this is
//! negligible and was judged not worth carrying scope into `workqueue.rs`
//! (not in this task's touch list) to add a destructor.
//!
//! Entry point: [`workqueue_test_launch_tests`] (`#[no_mangle] extern "C"`),
//! called from `kernel/start_kernel.rs` under
//! `#[cfg(feature = "workqueue_test")]` (env var `WORKQUEUE_TEST=1` at
//! `cmake` configure time) — the Wave-27 mechanism used by
//! `lock/{rwsem,semaphore}_test.rs`. Deliberately independent of the
//! pre-existing (empty-stub) `workqueue_smoke_test` cargo feature /
//! `workqueue_runtime_smoke_test()` call site Wave 27 left in place: that
//! stub is untouched (out of this task's touch scope — it lives in
//! `kernel/proc/workqueue.rs`), and this suite is wired as an independent,
//! additive gate rather than repurposing it.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{work_struct, workqueue, workqueue_callbacks};
use crate::proc::{
    create_work_struct, create_work_struct_ex, free_work_struct, queue_work, workqueue_create,
    workqueue_create_with_callbacks, workqueue_kill,
};

// Variadic FFI cannot be marked safe.
unsafe extern "C" {
    pub fn printf(fmt: *const c_char, ...) -> c_int;
}
unsafe extern "C" {
    // Not `pub`: shares a bare name with the real definition glob-
    // reexported from `sched.rs` at `crate::proc` -- see the identical
    // finding/fix on `kthread_create`/`wakeup` below (P3-1B2 sweep).
    safe fn scheduler_yield();
    pub safe fn sleep_ms(ms: u64);
}

const WORK_STRUCT_FLAG_RUN_ON_DRAIN: u32 = 1 << 0;
const WORK_STRUCT_FLAG_FREE_AFTER_RUN: u32 = 1 << 1;
const WORK_STRUCT_FLAG_INVALID: u32 = 1 << 31; // outside the valid mask

// ---------------------------------------------------------------------------
// Pass/fail bookkeeping — same idiom as `mm/pcache_test.rs`.
// ---------------------------------------------------------------------------
static TESTS_RUN: AtomicI32 = AtomicI32::new(0);
static TESTS_PASSED: AtomicI32 = AtomicI32::new(0);
static CASE_ERROR: AtomicI32 = AtomicI32::new(0);

fn case_fail() {
    CASE_ERROR.store(1, Ordering::SeqCst);
}

fn run_test(name: &core::ffi::CStr, body: fn()) {
    // SAFETY: static format string; one %s arg.
    unsafe { printf(c"[workqueue][%s] ".as_ptr(), name.as_ptr()); }
    CASE_ERROR.store(0, Ordering::SeqCst);
    body();
    TESTS_RUN.fetch_add(1, Ordering::SeqCst);
    if CASE_ERROR.load(Ordering::SeqCst) == 0 {
        TESTS_PASSED.fetch_add(1, Ordering::SeqCst);
        // SAFETY: static format string, no args.
        unsafe { printf(c"OK\n".as_ptr()); }
    } else {
        // SAFETY: static format string, no args.
        unsafe { printf(c"FAIL\n".as_ptr()); }
    }
}

macro_rules! check {
    ($cond:expr) => {
        if !($cond) {
            case_fail();
        }
    };
}
macro_rules! check_eq {
    ($a:expr, $b:expr) => {
        if $a != $b {
            case_fail();
        }
    };
}

/// Spin-wait for `*cell == expected`, sleeping between polls (these tests
/// wait on real asynchronous kthreads, not just a scheduler-yield race, so a
/// short real sleep between polls is more appropriate than a tight yield
/// loop).
fn wait_for(cell: &AtomicI32, expected: i32, mut budget_ms: u64) -> bool {
    while budget_ms > 0 {
        if cell.load(Ordering::SeqCst) == expected {
            return true;
        }
        sleep_ms(5);
        budget_ms = budget_ms.saturating_sub(5);
    }
    cell.load(Ordering::SeqCst) == expected
}

// ---------------------------------------------------------------------------
// Shared instrumentation. Only one case runs at a time (sequential master),
// and every case resets the counters it uses before running.
// ---------------------------------------------------------------------------
static CTOR_CALLS: AtomicI32 = AtomicI32::new(0);
static DTOR_CALLS: AtomicI32 = AtomicI32::new(0);
static RUN_COUNT: AtomicI32 = AtomicI32::new(0);

unsafe extern "C" fn cb_ctor(_wq: *mut workqueue) {
    CTOR_CALLS.fetch_add(1, Ordering::SeqCst);
}
unsafe extern "C" fn cb_dtor(_wq: *mut workqueue) {
    DTOR_CALLS.fetch_add(1, Ordering::SeqCst);
}
unsafe extern "C" fn cb_run_counter(_w: *mut work_struct) {
    RUN_COUNT.fetch_add(1, Ordering::SeqCst);
}
unsafe extern "C" fn cb_noop(_w: *mut work_struct) {}

// ===========================================================================
// T1: ctor invoked synchronously on create.
// ===========================================================================
fn t1_ctor_invoked_on_create() {
    CTOR_CALLS.store(0, Ordering::SeqCst);
    DTOR_CALLS.store(0, Ordering::SeqCst);
    let callbacks = workqueue_callbacks {
        workqueue_ctor: Some(cb_ctor),
        workqueue_dtor: Some(cb_dtor),
        manager_ctor: None,
        manager_dtor: None,
        worker_ctor: None,
        worker_dtor: None,
    };
    let wq = workqueue_create_with_callbacks(c"wqtest_t1".as_ptr(), 1, &callbacks);
    if wq.is_null() {
        case_fail();
        return;
    }
    check_eq!(CTOR_CALLS.load(Ordering::SeqCst), 1);
    check_eq!(DTOR_CALLS.load(Ordering::SeqCst), 0);
    workqueue_kill(wq);
}

// ===========================================================================
// T2: dtor invoked exactly once; double-kill is a safe no-op.
// ===========================================================================
fn t2_dtor_invoked_once_on_kill() {
    CTOR_CALLS.store(0, Ordering::SeqCst);
    DTOR_CALLS.store(0, Ordering::SeqCst);
    let callbacks = workqueue_callbacks {
        workqueue_ctor: Some(cb_ctor),
        workqueue_dtor: Some(cb_dtor),
        manager_ctor: None,
        manager_dtor: None,
        worker_ctor: None,
        worker_dtor: None,
    };
    let wq = workqueue_create_with_callbacks(c"wqtest_t2".as_ptr(), 1, &callbacks);
    if wq.is_null() {
        case_fail();
        return;
    }
    check_eq!(workqueue_kill(wq), 0);
    check_eq!(DTOR_CALLS.load(Ordering::SeqCst), 1);
    check_eq!(workqueue_kill(wq), 0);
    check_eq!(DTOR_CALLS.load(Ordering::SeqCst), 1);
}

// ===========================================================================
// T3: work_struct field integrity + invalid-flags rejection.
// ===========================================================================
fn t3_work_struct_field_integrity() {
    let work = create_work_struct_ex(Some(cb_noop), None, 0x5678, WORK_STRUCT_FLAG_RUN_ON_DRAIN);
    if work.is_null() {
        case_fail();
        return;
    }
    // SAFETY: `work` just came back non-null from `create_work_struct_ex`,
    // and this is the only reference to it (never queued) — plain,
    // non-bitfield field reads.
    unsafe {
        check_eq!((*work).data, 0x5678u64);
        check_eq!((*work).flags, WORK_STRUCT_FLAG_RUN_ON_DRAIN);
    }
    free_work_struct(work);

    // Invalid flags mask: `create_work_struct_ex` checks validity before
    // allocating and fails closed (returns NULL), unlike
    // `init_work_struct_ex` which panics on the same input — this suite
    // only ever exercises the fail-closed `create_*` entry points.
    let bad = create_work_struct_ex(Some(cb_noop), None, 0, WORK_STRUCT_FLAG_INVALID);
    check!(bad.is_null());
}

// ===========================================================================
// T4: queue_work is picked up and run by a real worker kthread.
// ===========================================================================
fn t4_queue_work_runs_async() {
    RUN_COUNT.store(0, Ordering::SeqCst);
    let wq = workqueue_create(c"wqtest_t4".as_ptr(), 2);
    if wq.is_null() {
        case_fail();
        return;
    }
    let work = create_work_struct(Some(cb_run_counter), 0);
    if work.is_null() {
        case_fail();
        workqueue_kill(wq);
        return;
    }
    check!(queue_work(wq, work));
    if !wait_for(&RUN_COUNT, 1, 2000) {
        case_fail();
    }
    free_work_struct(work);
    workqueue_kill(wq);
}

// ===========================================================================
// T5: queue_work fails deterministically once the workqueue is killed.
// ===========================================================================
fn t5_queue_work_fails_when_killed() {
    RUN_COUNT.store(0, Ordering::SeqCst);
    let wq = workqueue_create(c"wqtest_t5".as_ptr(), 1);
    if wq.is_null() {
        case_fail();
        return;
    }
    check_eq!(workqueue_kill(wq), 0);
    let work = create_work_struct(Some(cb_run_counter), 0);
    if work.is_null() {
        case_fail();
        return;
    }
    check!(!queue_work(wq, work));
    check_eq!(RUN_COUNT.load(Ordering::SeqCst), 0);
    free_work_struct(work);
}

// ===========================================================================
// T6: several real worker kthreads drain a burst of queued work.
// ===========================================================================
const T6_ITEMS: usize = 24;
fn t6_multi_worker_concurrency() {
    RUN_COUNT.store(0, Ordering::SeqCst);
    let wq = workqueue_create(c"wqtest_t6".as_ptr(), 6);
    if wq.is_null() {
        case_fail();
        return;
    }
    let mut items: [*mut work_struct; T6_ITEMS] = [core::ptr::null_mut(); T6_ITEMS];
    for slot in items.iter_mut() {
        let w = create_work_struct(Some(cb_run_counter), 0);
        if w.is_null() {
            case_fail();
            continue;
        }
        if !queue_work(wq, w) {
            case_fail();
        }
        *slot = w;
    }
    if !wait_for(&RUN_COUNT, T6_ITEMS as i32, 4000) {
        // SAFETY: static format string; two %d args.
        unsafe {
            printf(
                c"(ran %d/%d) ".as_ptr(),
                RUN_COUNT.load(Ordering::SeqCst),
                T6_ITEMS as i32,
            );
        }
        case_fail();
    }
    for &w in items.iter() {
        if !w.is_null() {
            free_work_struct(w);
        }
    }
    workqueue_kill(wq);
}

// ===========================================================================
// T7: FREE_AFTER_RUN self-frees a batch of heap work structs.
// ===========================================================================
const T7_ITEMS: usize = 32;
fn t7_free_after_run_stress() {
    RUN_COUNT.store(0, Ordering::SeqCst);
    let wq = workqueue_create(c"wqtest_t7".as_ptr(), 4);
    if wq.is_null() {
        case_fail();
        return;
    }
    for _ in 0..T7_ITEMS {
        let w = create_work_struct_ex(
            Some(cb_run_counter),
            None,
            0,
            WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN,
        );
        if w.is_null() {
            case_fail();
            continue;
        }
        if !queue_work(wq, w) {
            case_fail();
        }
    }
    if !wait_for(&RUN_COUNT, T7_ITEMS as i32, 4000) {
        case_fail();
    }
    // No `free_work_struct` here by design: `WORK_STRUCT_FLAG_FREE_AFTER_RUN`
    // means the workqueue already freed each item right after running it
    // (see `execute_work` in `workqueue.rs`). Reaching this point without a
    // crash across `T7_ITEMS` runs is itself the assertion.
    workqueue_kill(wq);
}

// ===========================================================================
// Master + entry point
// ===========================================================================

extern "C" fn workqueue_test_master(_a1: u64, _a2: u64) {
    for _ in 0..10_000 {
        scheduler_yield();
    }
    // SAFETY: static format string, no arguments.
    unsafe { printf(c"[workqueue] starting workqueue tests\n".as_ptr()); }

    run_test(c"T1 ctor_invoked_on_create", t1_ctor_invoked_on_create);
    run_test(c"T2 dtor_invoked_once_on_kill", t2_dtor_invoked_once_on_kill);
    run_test(c"T3 work_struct_field_integrity", t3_work_struct_field_integrity);
    run_test(c"T4 queue_work_runs_async", t4_queue_work_runs_async);
    run_test(c"T5 queue_work_fails_when_killed", t5_queue_work_fails_when_killed);
    run_test(c"T6 multi_worker_concurrency", t6_multi_worker_concurrency);
    run_test(c"T7 free_after_run_stress", t7_free_after_run_stress);

    let passed = TESTS_PASSED.load(Ordering::SeqCst);
    let total = TESTS_RUN.load(Ordering::SeqCst);
    if passed == total {
        // SAFETY: static format string; two %d args.
        unsafe { printf(c"WORKQUEUE TESTS: %d/%d PASSED\n".as_ptr(), passed, total); }
    } else {
        // SAFETY: see above.
        unsafe { printf(c"WORKQUEUE TESTS: %d/%d FAILED\n".as_ptr(), passed, total); }
    }
}

// Not `pub`: both fns below share a bare name with a real definition
// glob-reexported from `thread.rs`/`sched.rs` at `crate::proc`. Making
// these crate-visible would trigger E0659 ambiguous-glob-reexport the
// moment any other proc submodule imports the real one by its bare name
// (P3-1B2 sweep, same finding as `clone.rs`/`workqueue.rs`). Only ever
// called from within this file, so file-private is both sufficient and
// correct.
unsafe extern "C" {
    safe fn kthread_create(
        name: *const c_char,
        entry: *mut c_void,
        arg1: u64,
        arg2: u64,
        stack_order: c_int,
    ) -> *mut crate::bindings::thread;
    safe fn wakeup(p: *mut crate::bindings::thread);
}

#[inline]
fn is_err_or_null<T>(p: *mut T) -> bool {
    if p.is_null() {
        return true;
    }
    let n = p as usize;
    (n as isize) < 0 && (n.wrapping_neg() as usize) <= 4095
}

/// Spawns the workqueue test-suite master kthread. Entry point called
/// from `kernel/start_kernel.rs` (crate-path `use`, not an `extern`
/// redeclaration -- P3-1B mesh sweep) under `#[cfg(feature =
/// "workqueue_test")]`.
pub(crate) extern "C" fn workqueue_test_launch_tests() {
    const KERNEL_STACK_ORDER: c_int = 2;
    let np = kthread_create(
        c"workqueue_test_master".as_ptr(),
        workqueue_test_master as *mut c_void,
        0,
        0,
        KERNEL_STACK_ORDER,
    );
    if is_err_or_null(np) {
        // SAFETY: static format string, no arguments.
        unsafe { printf(c"[workqueue] cannot create test master thread\n".as_ptr()); }
    } else {
        wakeup(np);
    }
}
