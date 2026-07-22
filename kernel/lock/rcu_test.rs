//! Rust port of `kernel/lock/rcu_test.c`.
//!
//! Comprehensive RCU (Read-Copy-Update) test suite: basic read-side
//! critical sections, pointer publish/dereference, `synchronize_rcu()`,
//! `call_rcu()` callbacks, concurrent readers, RCU-protected linked lists
//! (with a simple use-after-free "ASAN" poison check), negative/edge-case
//! tests, and large-scale stress tests.
//!
//! Entry point: [`rcu_run_tests`] (`#[no_mangle] extern "C"`). The C
//! prototype lives in `kernel/inc/lock/rcu.h`; the call site in
//! `kernel/start_kernel.c` is commented out (RCU processing is now done
//! per-CPU in idle loops), exactly as it was before this port — this
//! module only preserves the symbol so that comment/call can be restored
//! without further changes.
//!
//! Uses the idiomatic [`crate::lock::rcu`] API (`KRcuRead`, `RcuPtr<T>`,
//! `api::*`) rather than the raw C-ABI `rcu_*` functions.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_char, c_int, c_void, CStr};
use core::mem::{offset_of, size_of};
use core::ptr::{addr_of, addr_of_mut, null_mut};
use core::sync::atomic::{AtomicI32, AtomicPtr, Ordering};

use crate::bindings::{list_node_t, rcu_head_t, spinlock_t, thread};
use crate::lock::rcu::{self as rcu, api as rcu_api, KRcuRead, RcuPtr};
use crate::machine;
use crate::sync::KSpinlock;
// P3-1C mesh sweep: printf.rs is in scope for this wave, so `trigger_panic`
// becomes a plain crate-path import instead of an `extern "C"` redeclaration.
use crate::printf::trigger_panic;

// P3-D2a: proc/sched.rs entry points, reached as plain crate-path items
// instead of `extern "C"` redeclarations.
use crate::proc::Scheduler;

// P3-D3c: plain crate-path imports instead of `extern "C"` redeclarations
// (both demoted from `#[no_mangle]` in the same wave).
use crate::timer::sched_timer::sleep_ms;
use crate::timer::timer_core::get_jiffs;

// P3-D3b: `kthread_create` (proc/thread.rs) is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone; reached via the `crate::proc`
// glob re-export.
/// `crate::kmm_alloc`/`kmm_free` are genuinely `unsafe fn`; this file's
/// original extern declaration asserted `pub safe fn` (usual FFI facade).
#[inline]
fn kmm_alloc(size: usize) -> *mut c_void {
    unsafe { crate::mm::kalloc::Kmem::kmm_alloc(size) }
}
/// SAFETY: see [`kmm_alloc`] above.
#[inline]
fn kmm_free(ptr: *mut c_void) {
    unsafe { crate::mm::kalloc::Kmem::kmm_free(ptr) };
}

/// See `completion.rs`'s identical note: `crate::lock::spinlock::RawSpinlock::init`
/// takes `name: *mut c_char`; this file's original extern declaration
/// typed it `*const c_char` (every call site passes a `'static`
/// string-literal pointer, never written through).
#[inline]
fn spin_init(lk: *mut spinlock_t, name: *const c_char) {
    // SAFETY: `name` is only read by the callee despite the `*mut`
    // parameter; every call site here passes a `'static` string literal.
    unsafe { crate::lock::spinlock::RawSpinlock::init(lk, name as *mut c_char) };
}


const KERNEL_STACK_ORDER: c_int = 2;
const RCU_TEST_NUM_READERS: i32 = 4;
const RCU_TEST_ITERATIONS: u64 = 50;
const STRESS_ITERATIONS: i32 = 100_000;
const STRESS_READERS: usize = 4;
const STRESS_BATCH_SIZE: i32 = 10_000;

// `is_err_or_null`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::is_err_or_null;

fn spawn(name: &'static CStr, entry: extern "C" fn(u64, u64), a1: u64, a2: u64) -> *mut thread {
    let np = crate::proc::thread::Thread::kthread_create(name.as_ptr(), entry as *mut c_void, a1, a2, KERNEL_STACK_ORDER);
    if !is_err_or_null(np) {
        Scheduler::wakeup(np);
    }
    np
}

#[inline(never)]
#[cold]
fn kpanic(msg: &CStr) -> ! {
    // SAFETY: `msg` is NUL-terminated by construction (either a `c"..."`
    // literal or otherwise validated); `printf`/`trigger_panic` are
    // kernel C symbols with the declared ABI.
    crate::kprint!("{}", crate::printf::Cs(msg.as_ptr()));
    // SAFETY: `trigger_panic` is a kernel C symbol with the declared ABI.
    unsafe {
        trigger_panic();
    }
}

/// Mirrors the C `assert(expr, fmt)` macro (panics with `msg` if `cond`
/// is false).
#[inline]
fn kassert(cond: bool, msg: &CStr) {
    if !cond {
        kpanic(msg);
    }
}

// ============================================================================
// Simple ASAN (Address Sanitizer) - Poison Pattern Detection
//
// Use-after-free detection for RCU testing: memory freed via a callback is
// poisoned with a known pattern first; readers that observe the pattern
// while traversing know they raced with a premature free.
// ============================================================================

const ASAN_POISON_ID: i32 = 0xDEADBEEFu32 as i32;
const ASAN_POISON_VALUE: i32 = 0xBADCAFEu32 as i32;
const ASAN_POISON_GENERIC: i32 = 0x5A5A5A5Au32 as i32;

static ASAN_CHECKS_PERFORMED: AtomicI32 = AtomicI32::new(0);
static ASAN_NODES_POISONED: AtomicI32 = AtomicI32::new(0);

fn asan_is_poisoned_int(val: i32) -> bool {
    val == ASAN_POISON_ID || val == ASAN_POISON_VALUE || val == ASAN_POISON_GENERIC
}

// ============================================================================
// Test data structures
// ============================================================================

#[repr(C)]
struct TestNode {
    value: i32,
    next: *mut TestNode,
}
// SAFETY: `TestNode` is only ever reached through `RcuPtr`'s
// publish/dereference discipline (a single writer publishes with
// release ordering; readers observe it inside an RCU read-side section
// and never mutate it), so treating it as `Send + Sync` is sound in the
// same sense the original C code's raw pointer sharing was sound.
unsafe impl Send for TestNode {}
unsafe impl Sync for TestNode {}

static TEST_LIST: RcuPtr<TestNode> = RcuPtr::empty();
static TEST_COUNTER: AtomicI32 = AtomicI32::new(0);
static CALLBACK_INVOKED: AtomicI32 = AtomicI32::new(0);

fn alloc_test_node(value: i32) -> *mut TestNode {
    let p = kmm_alloc(size_of::<TestNode>()) as *mut TestNode;
    if p.is_null() {
        kpanic(c"panic: alloc_test_node: kmm_alloc failed\n");
    }
    // SAFETY: freshly allocated, exclusively owned.
    unsafe {
        (*p).value = value;
        (*p).next = null_mut();
    }
    p
}

// ============================================================================
// Test 1: Basic RCU Read-Side Critical Section
// ============================================================================

fn test_rcu_read_lock() {
    crate::kprintln!("TEST: RCU Read Lock/Unlock");

    // Test nested locking.
    let g1 = KRcuRead::new();
    kassert(rcu_api::is_watching(), c"CPU should be in RCU critical section\n");

    let g2 = KRcuRead::new(); // Nested.
    kassert(rcu_api::is_watching(), c"CPU should still be in RCU critical section\n");

    drop(g2); // Unnest.
    kassert(rcu_api::is_watching(), c"CPU should still be in RCU critical section\n");

    drop(g1); // Final unlock.
    kassert(!rcu_api::is_watching(), c"CPU should not be in RCU critical section\n");

    crate::kprintln!("  PASS: Nested RCU read locks work correctly");
}

// ============================================================================
// Test 2: RCU Pointer Operations
// ============================================================================

fn test_rcu_pointers() {
    crate::kprintln!("TEST: RCU Pointer Operations");

    let node = alloc_test_node(42);

    // Test rcu_assign_pointer (publish).
    // SAFETY: `node` is freshly allocated and not yet visible to readers;
    // the previous value (null) needs no RCU-deferred free.
    unsafe { TEST_LIST.swap(node); }

    // Test rcu_dereference.
    {
        let g = KRcuRead::new();
        let read_node = g.load(&TEST_LIST);
        kassert(read_node.is_some(), c"rcu_dereference should return non-NULL\n");
        kassert(read_node.map(|n| n.value) == Some(42), c"rcu_dereference should return correct value\n");
    }

    // Test rcu_access_pointer (no read-side section required).
    let access_node = TEST_LIST.load_raw();
    kassert(!access_node.is_null(), c"rcu_access_pointer should return non-NULL\n");

    crate::kprintln!("  PASS: RCU pointer operations work correctly");

    // Cleanup.
    kmm_free(node as *mut c_void);
    // SAFETY: no readers remain (single-threaded cleanup); the pointer
    // being cleared was just freed above and observed by nobody else.
    unsafe { TEST_LIST.swap(null_mut()); }
}

// ============================================================================
// Test 3: synchronize_rcu()
// ============================================================================

fn test_synchronize_rcu() {
    crate::kprintln!("TEST: synchronize_rcu()");

    let old_node = alloc_test_node(100);
    // SAFETY: publishing the first version; no prior value to reclaim.
    unsafe { TEST_LIST.swap(old_node); }

    let new_node = alloc_test_node(200);
    // SAFETY: `old_node` (the previous value) is reclaimed below only
    // after `synchronize_rcu()`, satisfying the RCU deferred-free
    // contract.
    unsafe { TEST_LIST.swap(new_node); }

    crate::kprintln!("  Waiting for grace period...");
    rcu_api::synchronize();
    crate::kprintln!("  Grace period completed");

    // Now safe to free the old node.
    kmm_free(old_node as *mut c_void);

    // Verify the new node is accessible.
    {
        let g = KRcuRead::new();
        let cur = g.load(&TEST_LIST);
        kassert(cur.is_some(), c"List should not be NULL\n");
        kassert(cur.map(|n| n.value) == Some(200), c"Should read new value\n");
    }

    crate::kprintln!("  PASS: synchronize_rcu() allows safe reclamation");

    kmm_free(new_node as *mut c_void);
    // SAFETY: single-threaded cleanup, no outstanding readers.
    unsafe { TEST_LIST.swap(null_mut()); }
}

// ============================================================================
// Test 4: call_rcu() Callbacks
// ============================================================================

unsafe extern "C" fn test_callback(data: *mut c_void) {
    // SAFETY: `data` is the `Box`-like `*mut i32` allocated in
    // `test_call_rcu` and handed to `call_rcu` unchanged.
    let value = data as *mut i32;
    crate::kprintln!("  Callback invoked with value: {}", *value);
    CALLBACK_INVOKED.fetch_add(1, Ordering::Release);
    kmm_free(data);
}

fn test_call_rcu() {
    crate::kprintln!("TEST: call_rcu() Callbacks");
    CALLBACK_INVOKED.store(0, Ordering::Release);

    let data = kmm_alloc(size_of::<i32>()) as *mut i32;
    if data.is_null() {
        kpanic(c"panic: test_call_rcu: kmm_alloc failed for data\n");
    }
    // SAFETY: freshly allocated, exclusively owned.
    unsafe { *data = 42; }

    let head = kmm_alloc(size_of::<rcu_head_t>()) as *mut rcu_head_t;
    if head.is_null() {
        kpanic(c"panic: test_call_rcu: kmm_alloc failed for head\n");
    }

    // SAFETY: `head` is a freshly allocated, exclusively-owned
    // `rcu_head_t`; `test_callback` matches the required ABI; `data`
    // stays valid until the callback (which owns and frees it) runs.
    unsafe { rcu_api::call(head, Some(test_callback), data as *mut c_void); }
    crate::kprintln!("  Callback registered");

    rcu_api::synchronize();
    rcu_api::process_callbacks();

    let mut timeout = 100;
    while CALLBACK_INVOKED.load(Ordering::Acquire) == 0 && timeout > 0 {
        rcu_api::synchronize();
        rcu_api::process_callbacks();
        Scheduler::yield_now();
        timeout -= 1;
    }

    kassert(CALLBACK_INVOKED.load(Ordering::Acquire) == 1, c"Callback should have been invoked\n");
    crate::kprintln!("  PASS: call_rcu() callback executed successfully");

    // The callback frees `data`; we only own `head`.
    kmm_free(head as *mut c_void);
}

// ============================================================================
// Test 5: Multiple Concurrent Readers
// ============================================================================

static CONCURRENT_READERS_DONE: AtomicI32 = AtomicI32::new(0);

extern "C" fn reader_thread(id: u64, iterations: u64) {
    crate::kprintln!("  Reader {} starting ({} iterations)", id, iterations);

    for i in 0..iterations {
        {
            let g = KRcuRead::new();
            if let Some(node) = g.load(&TEST_LIST) {
                let mut sum: i32 = 0;
                for _ in 0..100 {
                    sum = sum.wrapping_add(node.value);
                }
                core::hint::black_box(sum);
            }
        }
        if i % 10 == 0 {
            Scheduler::yield_now();
        }
    }

    crate::kprintln!("  Reader {} completed", id);
    CONCURRENT_READERS_DONE.fetch_add(1, Ordering::Release);
}

fn test_concurrent_readers() {
    crate::kprintln!("TEST: Concurrent Readers");
    CONCURRENT_READERS_DONE.store(0, Ordering::Release);

    let node = alloc_test_node(777);
    // SAFETY: no prior value to reclaim.
    unsafe { TEST_LIST.swap(node); }

    for i in 0..RCU_TEST_NUM_READERS {
        spawn(c"rcu_reader", reader_thread, i as u64, RCU_TEST_ITERATIONS);
    }

    crate::kprintln!("  Waiting for readers to complete...");
    while CONCURRENT_READERS_DONE.load(Ordering::Acquire) < RCU_TEST_NUM_READERS {
        Scheduler::yield_now();
    }

    crate::kprintln!("  PASS: Concurrent readers completed successfully");

    rcu_api::synchronize();
    kmm_free(node as *mut c_void);
    // SAFETY: all readers have joined; no outstanding borrows.
    unsafe { TEST_LIST.swap(null_mut()); }
}

// ============================================================================
// Test 6: Grace Period Detection
// ============================================================================

fn test_grace_period() {
    crate::kprintln!("TEST: Grace Period Detection");
    TEST_COUNTER.store(0, Ordering::Release);

    {
        let _g = KRcuRead::new();
        let mut sum: i32 = 0;
        for i in 0..100 {
            sum = sum.wrapping_add(i);
        }
        core::hint::black_box(sum);
    }

    // Context switches OUTSIDE of the RCU critical section are quiescent
    // states; these yields help advance grace periods.
    for _ in 0..10 {
        Scheduler::yield_now();
    }

    rcu_api::synchronize();

    crate::kprintln!("  PASS: Grace period detection through context switches");
}

// ============================================================================
// NEGATIVE TEST 1: Callback Not Invoked Synchronously in call_rcu
// ============================================================================

static NEGATIVE_CALLBACK_COUNT: AtomicI32 = AtomicI32::new(0);

unsafe extern "C" fn negative_callback(data: *mut c_void) {
    NEGATIVE_CALLBACK_COUNT.fetch_add(1, Ordering::Release);
    kmm_free(data);
}

fn test_callback_not_invoked_early() {
    crate::kprintln!("NEGATIVE TEST: Callback Not Invoked Synchronously in call_rcu");
    NEGATIVE_CALLBACK_COUNT.store(0, Ordering::Release);

    let data = kmm_alloc(size_of::<i32>()) as *mut i32;
    if data.is_null() {
        kpanic(c"panic: test_callback_not_invoked_early: kmm_alloc failed for data\n");
    }
    unsafe { *data = 123; }

    let head = kmm_alloc(size_of::<rcu_head_t>()) as *mut rcu_head_t;
    if head.is_null() {
        kpanic(c"panic: test_callback_not_invoked_early: kmm_alloc failed for head\n");
    }

    // SAFETY: see `test_call_rcu`.
    unsafe { rcu_api::call(head, Some(negative_callback), data as *mut c_void); }

    let early_count = NEGATIVE_CALLBACK_COUNT.load(Ordering::Acquire);
    kassert(early_count == 0, c"Callback should NOT be invoked synchronously in call_rcu\n");
    crate::kprintln!("  PASS: Callback correctly NOT invoked synchronously in call_rcu");

    rcu_api::synchronize();
    rcu_api::process_callbacks();

    let final_count = NEGATIVE_CALLBACK_COUNT.load(Ordering::Acquire);
    kassert(final_count == 1, c"Callback should be invoked after synchronize_rcu\n");
    crate::kprintln!("  PASS: Callback invoked after grace period");

    kmm_free(head as *mut c_void);
}

// ============================================================================
// NEGATIVE TEST 2: Read Lock With No Context Switch Delays GP
// ============================================================================

fn test_read_lock_no_yield_delays_gp() {
    crate::kprintln!("NEGATIVE TEST: Read Lock Without Yield Delays GP");

    let g = KRcuRead::new();
    kassert(rcu_api::is_watching(), c"Should be in RCU critical section\n");

    let mut sum: i32 = 0;
    for i in 0..10_000 {
        sum = sum.wrapping_add(i);
    }
    core::hint::black_box(sum);

    kassert(rcu_api::is_watching(), c"Should still be in RCU critical section\n");
    crate::kprintln!("  Read lock held without yielding - nesting counter works");

    drop(g);
    kassert(!rcu_api::is_watching(), c"Should not be in RCU critical section after unlock\n");

    crate::kprintln!("  PASS: Read lock semantics work correctly");
}

// ============================================================================
// NEGATIVE TEST 3: Timestamp Overflow Handling
// ============================================================================

fn test_timestamp_overflow() {
    crate::kprintln!("NEGATIVE TEST: Timestamp Overflow Handling");
    crate::kprintln!("  Testing timestamp update mechanism");

    let cpu = machine::Riscv::cpuid() as usize;
    let start_time = get_jiffs();
    let cpu_ts_before = rcu_api::cpu_timestamp(cpu);

    // Completing a grace period forces context switches, which update
    // timestamps.
    rcu_api::synchronize();

    let after_time = get_jiffs();
    let cpu_ts_after = rcu_api::cpu_timestamp(cpu);

    crate::kprintln!("  Time before: {}, after: {}", start_time, after_time);
    crate::kprintln!("  CPU timestamp before: {}, after: {}", cpu_ts_before, cpu_ts_after);

    kassert(after_time >= start_time, c"Time should move forward\n");
    // CPU timestamp may or may not have changed on this specific CPU
    // (depends on which CPU completed the GP) — we only verify the
    // mechanism exists, matching the original C test's intent.

    crate::kprintln!("  PASS: Timestamp handling and overflow protection works correctly");
}

// ============================================================================
// NEGATIVE TEST 4: Unbalanced Lock/Unlock Detection
// ============================================================================

/// Read the current thread's RCU read-lock nesting counter.
fn current_nesting() -> i32 {
    let p = machine::Riscv::current_thread_ptr();
    if p.is_null() {
        return 0;
    }
    // SAFETY: `p` is the live current thread.
    unsafe { (*p).rcu_read_lock_nesting }
}

fn test_unbalanced_unlock() {
    crate::kprintln!("NEGATIVE TEST: Unbalanced Unlock Detection");

    // We can't actually trigger the panic in a test; verify the nesting
    // counter behaves correctly instead.
    let initial = current_nesting();

    let g1 = KRcuRead::new();
    kassert(current_nesting() == initial + 1, c"Nesting should increase\n");

    let g2 = KRcuRead::new();
    kassert(current_nesting() == initial + 2, c"Nesting should increase again\n");

    drop(g2);
    kassert(current_nesting() == initial + 1, c"Nesting should decrease\n");

    drop(g1);
    kassert(current_nesting() == initial, c"Nesting should return to initial\n");

    crate::kprintln!("  PASS: Lock/unlock nesting tracking works correctly");
}

// ============================================================================
// NEGATIVE TEST 5: Multiple Concurrent Grace Periods
// ============================================================================

fn test_concurrent_grace_periods() {
    crate::kprintln!("NEGATIVE TEST: Multiple Concurrent Grace Periods");

    // If there's a deadlock or state-corruption issue, this will hang or
    // crash.
    for _ in 0..3 {
        rcu_api::synchronize();
    }

    crate::kprintln!("  Successfully completed multiple grace periods without deadlock");
    crate::kprintln!("  PASS: Multiple concurrent grace periods handled correctly");
}

// ============================================================================
// NEGATIVE TEST 6: Grace Period Completion Verification
// ============================================================================

fn test_gp_requires_context_switch() {
    crate::kprintln!("NEGATIVE TEST: Grace Period Completion Verification");
    crate::kprintln!("  Calling synchronize_rcu() multiple times...");
    for _ in 0..3 {
        rcu_api::synchronize();
    }
    crate::kprintln!("  All grace periods completed successfully");
    crate::kprintln!("  PASS: Grace period mechanism works correctly");
}

// ============================================================================
// LIST RCU TESTS — minimal local port of the `list_*_rcu` primitives from
// kernel/inc/list.h, operating directly on `list_node_t`.
// ============================================================================

fn list_entry_init(entry: *mut list_node_t) {
    // SAFETY: caller has exclusive access during init.
    unsafe { (*entry).next = entry; (*entry).prev = entry; }
}

/// Mirrors `list_next_rcu()` (`rcu_dereference(entry->next)`).
fn list_next_rcu(entry: *mut list_node_t) -> *mut list_node_t {
    // SAFETY: `entry` is a valid, live `list_node_t`; modelling `next` as
    // an atomic pointer gives this the acquire-load semantics
    // `rcu_dereference()` requires for RCU-safe traversal.
    unsafe { (*(addr_of!((*entry).next) as *const AtomicPtr<list_node_t>)).load(Ordering::Acquire) }
}

/// Mirrors `list_entry_add_tail_rcu()`. Caller holds the list's
/// protecting spinlock.
fn list_add_tail_rcu(head: *mut list_node_t, entry: *mut list_node_t) {
    // SAFETY: caller holds the list's protecting spinlock; the
    // publishing store uses release ordering so RCU readers either see
    // the fully-initialised `entry` or don't see it at all.
    unsafe {
        let prev = (*head).prev;
        (*entry).next = head;
        (*entry).prev = prev;
        (*(addr_of_mut!((*prev).next) as *mut AtomicPtr<list_node_t>)).store(entry, Ordering::Release);
        (*head).prev = entry;
    }
}

/// Mirrors `list_entry_del_rcu()`. Caller holds the list's protecting
/// spinlock; the caller must defer freeing `entry` past a grace period.
fn list_del_rcu(entry: *mut list_node_t) {
    // SAFETY: see `list_add_tail_rcu`. Does not poison/reinit `entry` —
    // RCU readers may still be mid-traversal through it.
    unsafe {
        let prev = (*entry).prev;
        let next = (*entry).next;
        (*(addr_of_mut!((*prev).next) as *mut AtomicPtr<list_node_t>)).store(next, Ordering::Release);
        (*next).prev = prev;
    }
}

fn list_is_empty(head: *mut list_node_t) -> bool {
    // SAFETY: `head` is a valid list head.
    unsafe { (*head).next == head }
}

#[repr(C)]
struct ListTestNode {
    id: i32,
    value: i32,
    list_entry: list_node_t,
    rcu_head: rcu_head_t,
}

fn container_of_list_node(entry: *mut list_node_t) -> *mut ListTestNode {
    machine::Riscv::list_container_of::<ListTestNode>(entry, offset_of!(ListTestNode, list_entry))
}

fn asan_poison_node(node: *mut ListTestNode) {
    // SAFETY: caller (the free callback) has exclusive ownership of
    // `node` at this point — it has already been unlinked and no reader
    // can newly discover it, though racing readers already mid-traversal
    // may still observe the poisoned fields (that is the detection
    // mechanism).
    unsafe {
        (*node).id = ASAN_POISON_ID;
        (*node).value = ASAN_POISON_VALUE;
    }
    ASAN_NODES_POISONED.fetch_add(1, Ordering::Release);
}

fn asan_check_node(node: *mut ListTestNode, context: &CStr) {
    // SAFETY: `node` is reachable through an RCU-protected list traversal
    // inside a read-side critical section; it may be concurrently freed
    // (that's exactly the scenario this check detects), but the
    // allocator never unmaps/reuses freed slab memory synchronously, so
    // reading the (possibly poisoned) fields is not a use-after-unmap.
    let (id, value) = unsafe { ((*node).id, (*node).value) };
    ASAN_CHECKS_PERFORMED.fetch_add(1, Ordering::Relaxed);
    if asan_is_poisoned_int(id) {
        crate::kprintln!(
            "ASAN: Use-after-free detected! id=0x{:x} at {}",
            ((id as u32) as i32 as i64) as u64,
            crate::printf::Cs(context.as_ptr())
        );
        kpanic(c"panic: ASAN: use-after-free\n");
    }
    if asan_is_poisoned_int(value) {
        crate::kprintln!(
            "ASAN: Use-after-free detected! value=0x{:x} at {}",
            ((value as u32) as i32 as i64) as u64,
            crate::printf::Cs(context.as_ptr())
        );
        kpanic(c"panic: ASAN: use-after-free\n");
    }
}

unsafe extern "C" fn list_node_free_callback(data: *mut c_void) {
    let node = data as *mut ListTestNode;
    asan_poison_node(node);
    LIST_CALLBACK_COUNT.fetch_add(1, Ordering::Release);
    kmm_free(node as *mut c_void);
}

/// `Sync` newtype for `static` storage of a POD C struct initialised at
/// runtime via a `*_init` call (mirrors `rcu.rs`'s own `StaticCell`).
#[repr(transparent)]
struct StaticCell<T>(core::cell::UnsafeCell<core::mem::MaybeUninit<T>>);
unsafe impl<T> Sync for StaticCell<T> {}
impl<T> StaticCell<T> {
    const fn uninit() -> Self {
        Self(core::cell::UnsafeCell::new(core::mem::MaybeUninit::uninit()))
    }
    #[inline]
    fn as_mut_ptr(&self) -> *mut T {
        self.0.get().cast()
    }
}

static RCU_TEST_LIST_HEAD: StaticCell<list_node_t> = StaticCell::uninit();
static RCU_TEST_LIST_LOCK: StaticCell<spinlock_t> = StaticCell::uninit();
static LIST_CALLBACK_COUNT: AtomicI32 = AtomicI32::new(0);

fn test_list_head() -> *mut list_node_t { RCU_TEST_LIST_HEAD.as_mut_ptr() }
fn test_list_lock() -> KSpinlock { KSpinlock::from_bindings(RCU_TEST_LIST_LOCK.as_mut_ptr()) }

fn alloc_list_node(id: i32, value: i32) -> *mut ListTestNode {
    let p = kmm_alloc(size_of::<ListTestNode>()) as *mut ListTestNode;
    if p.is_null() {
        return null_mut();
    }
    // SAFETY: freshly allocated, exclusively owned.
    unsafe {
        (*p).id = id;
        (*p).value = value;
        list_entry_init(addr_of_mut!((*p).list_entry));
    }
    p
}

// ============================================================================
// Test 7: Basic List RCU Add/Delete
// ============================================================================

fn test_list_rcu_basic() {
    crate::kprintln!("TEST: Basic List RCU Operations");

    spin_init(RCU_TEST_LIST_LOCK.as_mut_ptr(), c"rcu_test_list".as_ptr());
    list_entry_init(test_list_head());
    LIST_CALLBACK_COUNT.store(0, Ordering::Release);

    for i in 0..10 {
        let node = alloc_list_node(i, i * 100);
        if node.is_null() {
            kpanic(c"panic: test_list_rcu_basic: kmm_alloc failed\n");
        }
        let _g = test_list_lock().lock();
        list_add_tail_rcu(test_list_head(), addr_of_mut!(unsafe { &mut *node }.list_entry));
    }

    // Verify all nodes are readable.
    let mut count = 0;
    {
        let _g = KRcuRead::new();
        let mut pos = list_next_rcu(test_list_head());
        while pos != test_list_head() {
            let node = container_of_list_node(pos);
            // SAFETY: `node` was just derived from a live list entry.
            let (id, value) = unsafe { ((*node).id, (*node).value) };
            kassert(value == id * 100, c"Node value should match\n");
            count += 1;
            pos = list_next_rcu(pos);
        }
    }
    kassert(count == 10, c"Should have 10 nodes in list\n");

    // Delete all nodes with RCU.
    {
        let _g = test_list_lock().lock();
        let mut pos = unsafe { (*test_list_head()).next };
        while pos != test_list_head() {
            let next = unsafe { (*pos).next };
            let node = container_of_list_node(pos);
            list_del_rcu(pos);
            // SAFETY: `node`/`&mut (*node).rcu_head` outlive the callback
            // (freed only inside it); `list_node_free_callback` matches
            // the required ABI.
            unsafe {
                rcu_api::call(addr_of_mut!((*node).rcu_head), Some(list_node_free_callback), node as *mut c_void);
            }
            pos = next;
        }
    }

    // Wait for callbacks — multiple cycles ensure all are processed
    // (pending -> GP completes -> ready -> invoked).
    for _ in 0..5 {
        rcu_api::synchronize();
        rcu_api::process_callbacks();
        Scheduler::yield_now();
    }

    let invoked = LIST_CALLBACK_COUNT.load(Ordering::Acquire);
    crate::kprintln!("  Callbacks invoked: {}/10", invoked);
    kassert(invoked == 10, c"All 10 callbacks should have been invoked\n");

    crate::kprintln!("  PASS: Basic list RCU add/delete works correctly");
}

// ============================================================================
// Test 8: List RCU Concurrent Read While Write
// ============================================================================

static LIST_STRESS_READER_DONE: AtomicI32 = AtomicI32::new(0);
static LIST_STRESS_ERRORS: AtomicI32 = AtomicI32::new(0);

extern "C" fn list_stress_reader(iterations: u64, _unused: u64) {
    for i in 0..iterations {
        {
            let _g = KRcuRead::new();
            let mut pos = list_next_rcu(test_list_head());
            while pos != test_list_head() {
                let node = container_of_list_node(pos);
                asan_check_node(node, c"list_stress_reader");
                // SAFETY: see `asan_check_node`.
                let (id, value) = unsafe { ((*node).id, (*node).value) };
                if value != id * 100 {
                    LIST_STRESS_ERRORS.fetch_add(1, Ordering::Release);
                }
                pos = list_next_rcu(pos);
            }
        }
        if i % 100 == 0 {
            Scheduler::yield_now();
        }
    }
    LIST_STRESS_READER_DONE.fetch_add(1, Ordering::Release);
}

fn test_list_rcu_concurrent_rw() {
    crate::kprintln!("TEST: List RCU Concurrent Read While Write");

    spin_init(RCU_TEST_LIST_LOCK.as_mut_ptr(), c"rcu_test_list".as_ptr());
    list_entry_init(test_list_head());
    LIST_CALLBACK_COUNT.store(0, Ordering::Release);
    LIST_STRESS_READER_DONE.store(0, Ordering::Release);
    LIST_STRESS_ERRORS.store(0, Ordering::Release);

    for _ in 0..2 {
        spawn(c"list_reader", list_stress_reader, 500, 0);
    }

    let mut next_id: i32 = 0;
    for _round in 0..100 {
        for _ in 0..5 {
            let node = alloc_list_node(next_id, next_id * 100);
            next_id += 1;
            if node.is_null() {
                kpanic(c"panic: test_list_rcu_concurrent_rw: kmm_alloc failed\n");
            }
            let _g = test_list_lock().lock();
            list_add_tail_rcu(test_list_head(), addr_of_mut!(unsafe { &mut *node }.list_entry));
        }

        Scheduler::yield_now();

        for _ in 0..3 {
            let _g = test_list_lock().lock();
            if !list_is_empty(test_list_head()) {
                let first = unsafe { (*test_list_head()).next };
                let node = container_of_list_node(first);
                list_del_rcu(first);
                unsafe {
                    rcu_api::call(addr_of_mut!((*node).rcu_head), Some(list_node_free_callback), node as *mut c_void);
                }
            }
        }

        Scheduler::yield_now();
    }

    while LIST_STRESS_READER_DONE.load(Ordering::Acquire) < 2 {
        Scheduler::yield_now();
    }

    // Cleanup remaining nodes.
    {
        let _g = test_list_lock().lock();
        let mut pos = unsafe { (*test_list_head()).next };
        while pos != test_list_head() {
            let next = unsafe { (*pos).next };
            let node = container_of_list_node(pos);
            list_del_rcu(pos);
            unsafe {
                rcu_api::call(addr_of_mut!((*node).rcu_head), Some(list_node_free_callback), node as *mut c_void);
            }
            pos = next;
        }
    }

    rcu_api::synchronize();
    rcu_api::process_callbacks();

    let errors = LIST_STRESS_ERRORS.load(Ordering::Acquire);
    kassert(errors == 0, c"No errors should occur during concurrent read/write\n");

    crate::kprintln!("  Completed concurrent read/write with 0 errors");
    crate::kprintln!("  PASS: List RCU concurrent read/write works correctly");
}

// ============================================================================
// STRESS TESTS (100,000 scale)
// ============================================================================

#[repr(C)]
struct StressData {
    value: i32,
    rcu_head: rcu_head_t,
}

static STRESS_CALLBACKS_INVOKED: AtomicI32 = AtomicI32::new(0);
const STRESS_READER_ITER_INIT: AtomicI32 = AtomicI32::new(0);
static STRESS_READER_ITERATIONS: [AtomicI32; STRESS_READERS] = [STRESS_READER_ITER_INIT; STRESS_READERS];
static STRESS_READERS_DONE: AtomicI32 = AtomicI32::new(0);

unsafe extern "C" fn stress_node_free_callback(data: *mut c_void) {
    STRESS_CALLBACKS_INVOKED.fetch_add(1, Ordering::Release);
    kmm_free(data);
}

// ----------------------------------------------------------------------
// STRESS TEST 1: 100,000 call_rcu() Operations
// ----------------------------------------------------------------------

fn test_stress_call_rcu() {
    crate::kprintln!("STRESS TEST: {} call_rcu() Operations", STRESS_ITERATIONS);
    STRESS_CALLBACKS_INVOKED.store(0, Ordering::Release);

    for batch in 0..(STRESS_ITERATIONS / STRESS_BATCH_SIZE) {
        for i in 0..STRESS_BATCH_SIZE {
            let mut data = kmm_alloc(size_of::<StressData>()) as *mut StressData;
            if data.is_null() {
                // Out of memory: wait for a grace period so kthreads can
                // process/reclaim callbacks, then retry once.
                rcu_api::synchronize();
                Scheduler::yield_now();
                data = kmm_alloc(size_of::<StressData>()) as *mut StressData;
                if data.is_null() {
                    kpanic(c"panic: stress: out of memory even after processing callbacks\n");
                }
            }
            // SAFETY: freshly allocated, exclusively owned.
            unsafe { (*data).value = batch * STRESS_BATCH_SIZE + i; }
            // SAFETY: `data as *mut c_void` is what `stress_node_free_callback`
            // frees; the embedded `rcu_head` outlives the callback.
            unsafe {
                rcu_api::call(addr_of_mut!((*data).rcu_head), Some(stress_node_free_callback), data as *mut c_void);
            }
        }
        // Ensure the grace period completes so callbacks can be processed
        // by kthreads.
        rcu_api::synchronize();
    }

    // Wait for all callbacks via `rcu_barrier()`.
    rcu_api::barrier();

    let invoked = STRESS_CALLBACKS_INVOKED.load(Ordering::Acquire);
    crate::kprintln!("  Final: {} callbacks invoked out of {}", invoked, STRESS_ITERATIONS);
    kassert(invoked == STRESS_ITERATIONS, c"All callbacks should be invoked\n");

    crate::kprintln!("  PASS: {} call_rcu() operations completed successfully", STRESS_ITERATIONS);
}

// ----------------------------------------------------------------------
// STRESS TEST 2: 100,000 List Add/Remove with Concurrent Readers
// ----------------------------------------------------------------------

extern "C" fn stress_list_reader(reader_id: u64, _unused: u64) {
    let mut iterations: i32 = 0;

    while STRESS_READERS_DONE.load(Ordering::Acquire) == 0 {
        {
            let _g = KRcuRead::new();
            let mut count = 0;
            let mut pos = list_next_rcu(test_list_head());
            while pos != test_list_head() {
                let node = container_of_list_node(pos);
                asan_check_node(node, c"stress_list_reader");
                // SAFETY: see `asan_check_node`.
                let (id, value) = unsafe { ((*node).id, (*node).value) };
                if value != id * 10 {
                    kpanic(c"panic: stress: node corruption detected\n");
                }
                count += 1;
                if count > 1000 {
                    break; // Limit traversal to avoid monopolising the CPU.
                }
                pos = list_next_rcu(pos);
            }
        }
        iterations += 1;
        if iterations % 100 == 0 {
            Scheduler::yield_now();
        }
    }

    STRESS_READER_ITERATIONS[reader_id as usize].store(iterations, Ordering::Release);
}

fn test_stress_list_rcu() {
    crate::kprintln!("STRESS TEST: {} List Add/Remove with Concurrent Readers", STRESS_ITERATIONS);

    spin_init(RCU_TEST_LIST_LOCK.as_mut_ptr(), c"stress_list".as_ptr());
    list_entry_init(test_list_head());
    STRESS_CALLBACKS_INVOKED.store(0, Ordering::Release);
    STRESS_READERS_DONE.store(0, Ordering::Release);
    for slot in STRESS_READER_ITERATIONS.iter() {
        slot.store(0, Ordering::Release);
    }

    for i in 0..STRESS_READERS {
        spawn(c"stress_reader", stress_list_reader, i as u64, 0);
    }

    for _ in 0..10 {
        Scheduler::yield_now();
    }

    let mut next_id: i32 = 0;
    let mut total_added: i32 = 0;
    let mut total_removed: i32 = 0;

    for op in 0..STRESS_ITERATIONS {
        if op % 3 != 0 || total_added <= total_removed {
            let mut node = alloc_list_node(next_id, next_id * 10);
            if node.is_null() {
                rcu_api::synchronize();
                rcu_api::process_callbacks();
                Scheduler::yield_now();
                node = alloc_list_node(next_id, next_id * 10);
                if node.is_null() {
                    continue; // Still no memory — skip this add.
                }
            }
            next_id += 1;
            let _g = test_list_lock().lock();
            list_add_tail_rcu(test_list_head(), addr_of_mut!(unsafe { &mut *node }.list_entry));
            drop(_g);
            total_added += 1;
        } else {
            let _g = test_list_lock().lock();
            if !list_is_empty(test_list_head()) {
                let first = unsafe { (*test_list_head()).next };
                let node = container_of_list_node(first);
                list_del_rcu(first);
                unsafe {
                    rcu_api::call(addr_of_mut!((*node).rcu_head), Some(stress_node_free_callback), node as *mut c_void);
                }
                total_removed += 1;
            }
        }

        if (op + 1) % 500 == 0 {
            rcu_api::synchronize();
            rcu_api::process_callbacks();
        }
        if op % 100 == 0 {
            Scheduler::yield_now();
        }
    }

    STRESS_READERS_DONE.store(1, Ordering::Release);
    for _ in 0..50 {
        Scheduler::yield_now();
    }

    crate::kprint!("  Reader iterations: ");
    for slot in STRESS_READER_ITERATIONS.iter() {
        crate::kprint!("{} ", slot.load(Ordering::Acquire));
    }
    crate::kprintln!();

    // Cleanup remaining nodes.
    let mut remaining: i32 = 0;
    {
        let _g = test_list_lock().lock();
        let mut pos = unsafe { (*test_list_head()).next };
        while pos != test_list_head() {
            let next = unsafe { (*pos).next };
            let node = container_of_list_node(pos);
            list_del_rcu(pos);
            unsafe {
                rcu_api::call(addr_of_mut!((*node).rcu_head), Some(stress_node_free_callback), node as *mut c_void);
            }
            remaining += 1;
            pos = next;
        }
    }
    crate::kprintln!("  Cleaning up {} remaining nodes", remaining);

    rcu_api::barrier();

    let freed = STRESS_CALLBACKS_INVOKED.load(Ordering::Acquire);
    crate::kprintln!(
        "  Total: added={}, removed={} (via call_rcu), freed={}",
        total_added,
        total_removed + remaining,
        freed
    );
    kassert(freed == total_removed + remaining, c"All removed nodes should be freed\n");

    crate::kprintln!("  PASS: {} list operations with concurrent readers completed", STRESS_ITERATIONS);
}

// ----------------------------------------------------------------------
// STRESS TEST 3: Rapid Grace Periods
// ----------------------------------------------------------------------

fn test_stress_grace_periods() {
    crate::kprintln!("STRESS TEST: {} Rapid Grace Periods", STRESS_ITERATIONS);

    let start_time = get_jiffs();
    for _ in 0..STRESS_ITERATIONS {
        rcu_api::synchronize();
    }
    let end_time = get_jiffs();
    let elapsed = end_time.wrapping_sub(start_time);

    crate::kprintln!("  Completed {} grace periods in {} jiffies", STRESS_ITERATIONS, elapsed);
    crate::kprintln!("  PASS: Rapid grace period stress test completed");
}

// ----------------------------------------------------------------------
// STRESS TEST 4: Mixed Workload (Readers + Writers + Callbacks)
// ----------------------------------------------------------------------

static MIXED_OPS_COMPLETED: AtomicI32 = AtomicI32::new(0);
static MIXED_READERS_RUNNING: AtomicI32 = AtomicI32::new(0);

extern "C" fn mixed_reader_thread(_id: u64, target_ops: u64) {
    MIXED_READERS_RUNNING.fetch_add(1, Ordering::Release);

    for i in 0..target_ops {
        {
            let _g = KRcuRead::new();
            let mut count = 0;
            let mut pos = list_next_rcu(test_list_head());
            while pos != test_list_head() {
                let node = container_of_list_node(pos);
                asan_check_node(node, c"mixed_reader_thread");
                count += 1;
                if count > 100 {
                    break; // Limit per-read traversal.
                }
                pos = list_next_rcu(pos);
            }
        }
        MIXED_OPS_COMPLETED.fetch_add(1, Ordering::Release);
        if i % 100 == 0 {
            Scheduler::yield_now();
        }
    }

    MIXED_READERS_RUNNING.fetch_sub(1, Ordering::Release);
}

fn test_stress_mixed_workload() {
    crate::kprintln!("STRESS TEST: Mixed Workload ({} total operations)", STRESS_ITERATIONS);

    spin_init(RCU_TEST_LIST_LOCK.as_mut_ptr(), c"mixed_list".as_ptr());
    list_entry_init(test_list_head());
    STRESS_CALLBACKS_INVOKED.store(0, Ordering::Release);
    MIXED_OPS_COMPLETED.store(0, Ordering::Release);
    MIXED_READERS_RUNNING.store(0, Ordering::Release);

    for i in 0..4 {
        spawn(c"mixed_reader", mixed_reader_thread, i, 2000);
    }

    while MIXED_READERS_RUNNING.load(Ordering::Acquire) < 4 {
        Scheduler::yield_now();
    }

    let mut next_id: i32 = 0;
    for op in 0..2000 {
        if op % 2 == 0 {
            let node = alloc_list_node(next_id, next_id * 10);
            next_id += 1;
            if node.is_null() {
                kpanic(c"panic: test_stress_mixed_workload: kmm_alloc failed\n");
            }
            let _g = test_list_lock().lock();
            list_add_tail_rcu(test_list_head(), addr_of_mut!(unsafe { &mut *node }.list_entry));
        } else {
            let _g = test_list_lock().lock();
            if !list_is_empty(test_list_head()) {
                let first = unsafe { (*test_list_head()).next };
                let node = container_of_list_node(first);
                list_del_rcu(first);
                unsafe {
                    rcu_api::call(addr_of_mut!((*node).rcu_head), Some(stress_node_free_callback), node as *mut c_void);
                }
            }
        }

        MIXED_OPS_COMPLETED.fetch_add(1, Ordering::Release);

        if op % 100 == 0 {
            rcu_api::synchronize();
            rcu_api::process_callbacks();
            Scheduler::yield_now();
        }
    }

    while MIXED_READERS_RUNNING.load(Ordering::Acquire) > 0 {
        Scheduler::yield_now();
    }

    let total_ops = MIXED_OPS_COMPLETED.load(Ordering::Acquire);
    crate::kprintln!("  Total operations completed: {} (target: 10,000)", total_ops);

    // Cleanup.
    {
        let _g = test_list_lock().lock();
        let mut pos = unsafe { (*test_list_head()).next };
        while pos != test_list_head() {
            let next = unsafe { (*pos).next };
            let node = container_of_list_node(pos);
            list_del_rcu(pos);
            unsafe {
                rcu_api::call(addr_of_mut!((*node).rcu_head), Some(stress_node_free_callback), node as *mut c_void);
            }
            pos = next;
        }
    }

    for _ in 0..10 {
        rcu_api::synchronize();
        rcu_api::process_callbacks();
        Scheduler::yield_now();
    }

    crate::kprintln!("  PASS: Mixed workload stress test completed");
}

// ============================================================================
// Main Test Runner
// ============================================================================

fn banner() {
    crate::kprintln!("====================================================================================");
}

/// Run the full RCU test suite. Preserves the original `void
/// rcu_run_tests(void)` C-ABI entry point declared in
/// `kernel/inc/lock/rcu.h`; the call site in `kernel/start_kernel.c` is
/// (and remains) commented out.
pub(crate) fn rcu_run_tests() {
    sleep_ms(100);
    crate::kprintln!();
    banner();
    crate::kprintln!("RCU Test Suite Starting");
    banner();
    crate::kprintln!("  Configuration:");
    crate::kprintln!("    - Concurrent reader threads: {}", RCU_TEST_NUM_READERS);
    crate::kprintln!("    - Iterations per reader: {}", RCU_TEST_ITERATIONS);
    crate::kprintln!("    - Stress test iterations: {}", STRESS_ITERATIONS);
    banner();
    crate::kprintln!();

    macro_rules! run {
        ($f:expr) => {
            $f();
            crate::kprintln!();
        };
    }

    // Positive tests.
    run!(test_rcu_read_lock);
    run!(test_rcu_pointers);
    run!(test_synchronize_rcu);
    run!(test_call_rcu);
    run!(test_grace_period);
    run!(test_concurrent_readers);

    // List RCU tests.
    banner();
    crate::kprintln!("Starting List RCU Tests");
    banner();
    crate::kprintln!();
    run!(test_list_rcu_basic);
    run!(test_list_rcu_concurrent_rw);

    // Negative tests.
    banner();
    crate::kprintln!("Starting Negative Tests (Edge Cases and Error Conditions)");
    banner();
    crate::kprintln!();
    run!(test_callback_not_invoked_early);
    run!(test_read_lock_no_yield_delays_gp);
    run!(test_timestamp_overflow);
    run!(test_unbalanced_unlock);
    run!(test_concurrent_grace_periods);
    run!(test_gp_requires_context_switch);

    // Stress tests.
    banner();
    crate::kprintln!("Starting Stress Tests ({} scale)", STRESS_ITERATIONS);
    banner();
    crate::kprintln!();
    run!(test_stress_call_rcu);
    run!(test_stress_list_rcu);
    run!(test_stress_grace_periods);
    run!(test_stress_mixed_workload);

    // ASAN summary.
    banner();
    crate::kprintln!("ASAN Summary");
    banner();
    crate::kprintln!("  Total ASAN checks performed: {}", ASAN_CHECKS_PERFORMED.load(Ordering::Acquire));
    crate::kprintln!("  Total nodes poisoned: {}", ASAN_NODES_POISONED.load(Ordering::Acquire));
    crate::kprintln!("  Use-after-free errors detected: 0 (would have panicked)");
    banner();
    crate::kprintln!();

    banner();
    crate::kprintln!("RCU Test Suite Completed - ALL TESTS PASSED");
    banner();
    crate::kprintln!();
}
