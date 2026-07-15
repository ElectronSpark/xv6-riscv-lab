//! Rust port of `kernel/lock/semaphore_test.c`.
//!
//! Semaphore runtime test suite, exercising the [`crate::lock::semaphore`]
//! API through kernel threads:
//!  1. Waiters block until tokens are posted.
//!  2. Try-wait succeeds while tokens remain and fails with `EAGAIN` when
//!     empty.
//!  3. Overflow protection rejects posts beyond `SEM_VALUE_MAX`.
//!  4. Producer/consumer stress validates ordering and wakeups under
//!     contention.
//!
//! Entry point: [`semaphore_launch_tests`] (`#[no_mangle] extern "C"`),
//! called from `kernel/start_kernel.c` under `#ifdef SEMAPHORE_RUNTIME_TEST`
//! — preserved verbatim from the deleted C file so that call site keeps
//! working unmodified.

#![allow(non_camel_case_types, non_snake_case)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{semaphore as sem_t, thread};
use crate::lock::mutex::KMutex;
use crate::lock::semaphore::{KSemaphore, SemError};

// P3-D2a: proc/sched.rs entry points, reached as plain crate-path items
// instead of `extern "C"` redeclarations.
use crate::proc::{scheduler_yield, wakeup};

unsafe extern "C" {
    pub safe fn kthread_create(
        name: *const c_char,
        entry: *mut c_void,
        arg1: u64,
        arg2: u64,
        stack_order: c_int,
    ) -> *mut thread;
}


const KERNEL_STACK_ORDER: c_int = 2;
// Matches `SEM_VALUE_MAX` in kernel/inc/lock/semaphore.h.
const SEM_VALUE_MAX: c_int = 2_147_483_640;

// `is_err_or_null`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::is_err_or_null;

/// `Sync` newtype for `static` storage of a POD C struct that is
/// initialised at runtime via a `*_init` call (mirrors `rcu.rs`'s
/// `StaticCell`).
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

fn spawn(name: &'static core::ffi::CStr, entry: extern "C" fn(u64, u64), a1: u64, a2: u64) -> bool {
    let np = kthread_create(name.as_ptr(), entry as *mut c_void, a1, a2, KERNEL_STACK_ORDER);
    if is_err_or_null(np) {
        false
    } else {
        wakeup(np);
        true
    }
}

// ---------------------------------------------------------------------------
// Shared instrumentation
// ---------------------------------------------------------------------------

static SEM_ERROR_FLAG: AtomicI32 = AtomicI32::new(0);

fn fail() {
    SEM_ERROR_FLAG.store(1, Ordering::SeqCst);
}

fn print_result() {
    if SEM_ERROR_FLAG.load(Ordering::SeqCst) != 0 {
        crate::kprintln!("FAIL");
    } else {
        crate::kprintln!("OK");
    }
}

/// Spin-wait for `*ptr == expected`, yielding between polls. Mirrors the C
/// `sem_wait_for` helper including the rate-limited diagnostic print.
fn sem_wait_for(cell: &AtomicI32, expected: i32, mut spin_loops: i32) -> bool {
    while spin_loops > 0 {
        spin_loops -= 1;
        if cell.load(Ordering::SeqCst) == expected {
            return true;
        }
        scheduler_yield();
    }
    if SEM_T4_LOG_BUDGET.fetch_add(1, Ordering::SeqCst) + 1 <= 8 {
        crate::kprintln!(
            "[sem][diag] wait_for timed out target={} value={} expected={}",
            crate::printf::Ptr(cell as *const AtomicI32 as u64),
            cell.load(Ordering::SeqCst),
            expected,
        );
    }
    false
}

// ---------------------------------------------------------------------------
// Test 1: waiters block until posted tokens
// ---------------------------------------------------------------------------

const SEM_TEST_WAITERS: i32 = 4;
static SEM_T1: StaticCell<sem_t> = StaticCell::uninit();
static SEM_T1_WAIT_REQUESTS: AtomicI32 = AtomicI32::new(0);
static SEM_T1_ACQUIRED: AtomicI32 = AtomicI32::new(0);

fn sem_t1() -> KSemaphore {
    KSemaphore::from_ptr(SEM_T1.as_mut_ptr())
}

extern "C" fn sem_test1_waiter(_a1: u64, _a2: u64) {
    SEM_T1_WAIT_REQUESTS.fetch_add(1, Ordering::SeqCst);
    if sem_t1().wait().is_err() {
        fail();
        return;
    }
    SEM_T1_ACQUIRED.fetch_add(1, Ordering::SeqCst);
}

fn sem_run_test1() {
    crate::kprint!("[sem][T1] waiters block until posted tokens... ");
    SEM_ERROR_FLAG.store(0, Ordering::SeqCst);
    SEM_T1_WAIT_REQUESTS.store(0, Ordering::SeqCst);
    SEM_T1_ACQUIRED.store(0, Ordering::SeqCst);

    if sem_t1().init(c"sem-test1".as_ptr(), 0).is_err() {
        fail();
    }

    for _ in 0..SEM_TEST_WAITERS {
        if !spawn(c"sem_t1", sem_test1_waiter, 0, 0) {
            fail();
        }
    }

    if !sem_wait_for(&SEM_T1_WAIT_REQUESTS, SEM_TEST_WAITERS, 50_000) {
        fail();
    }

    match sem_t1().value() {
        Ok(v) if v == -SEM_TEST_WAITERS => {}
        _ => fail(),
    }

    for _ in 0..SEM_TEST_WAITERS {
        if sem_t1().post().is_err() {
            fail();
        }
    }

    if !sem_wait_for(&SEM_T1_ACQUIRED, SEM_TEST_WAITERS, 50_000) {
        fail();
    }

    match sem_t1().value() {
        Ok(0) => {}
        _ => fail(),
    }

    print_result();
}

// ---------------------------------------------------------------------------
// Test 2: try-wait semantics
// ---------------------------------------------------------------------------

fn sem_run_test2() {
    crate::kprint!("[sem][T2] trywait semantics... ");
    SEM_ERROR_FLAG.store(0, Ordering::SeqCst);

    let mut storage: MaybeUninit<sem_t> = MaybeUninit::uninit();
    let local = KSemaphore::from_ptr(storage.as_mut_ptr());
    if local.init(c"sem-test2".as_ptr(), 2).is_err() {
        fail();
    } else {
        if local.try_wait().is_err() {
            fail();
        }
        if local.try_wait().is_err() {
            fail();
        }
        match local.try_wait() {
            Err(SemError::WouldBlock) => {}
            _ => fail(),
        }
        match local.value() {
            Ok(0) => {}
            _ => fail(),
        }
    }

    print_result();
}

// ---------------------------------------------------------------------------
// Test 3: overflow guard
// ---------------------------------------------------------------------------

fn sem_run_test3() {
    crate::kprint!("[sem][T3] overflow guard... ");
    SEM_ERROR_FLAG.store(0, Ordering::SeqCst);

    let mut storage: MaybeUninit<sem_t> = MaybeUninit::uninit();
    let local = KSemaphore::from_ptr(storage.as_mut_ptr());
    if local.init(c"sem-test3".as_ptr(), SEM_VALUE_MAX).is_err() {
        fail();
    } else {
        match local.post() {
            Err(SemError::Overflow) => {}
            _ => fail(),
        }
        match local.value() {
            Ok(v) if v == SEM_VALUE_MAX => {}
            _ => fail(),
        }
    }

    print_result();
}

// ---------------------------------------------------------------------------
// Test 4: producer/consumer stress
// ---------------------------------------------------------------------------

const SEM_T4_BUFFER_CAP: i32 = 16;
const SEM_T4_TOTAL_ITEMS: i32 = 512;
const SEM_T4_PRODUCERS: i32 = 3;
const SEM_T4_CONSUMERS: i32 = 4;

static SEM_T4_EMPTY: StaticCell<sem_t> = StaticCell::uninit();
static SEM_T4_FULL: StaticCell<sem_t> = StaticCell::uninit();
static SEM_T4_LOCK: StaticCell<crate::bindings::mutex_t> = StaticCell::uninit();

struct RingBuffer(UnsafeCell<[i32; SEM_T4_BUFFER_CAP as usize]>);
unsafe impl Sync for RingBuffer {}
static SEM_T4_BUFFER: RingBuffer = RingBuffer(UnsafeCell::new([0; SEM_T4_BUFFER_CAP as usize]));

static SEM_T4_HEAD: AtomicI32 = AtomicI32::new(0);
static SEM_T4_TAIL: AtomicI32 = AtomicI32::new(0);
static SEM_T4_PRODUCE_CURSOR: AtomicI32 = AtomicI32::new(0);
static SEM_T4_ITEMS_CONSUMED: AtomicI32 = AtomicI32::new(0);
static SEM_T4_PRODUCERS_DONE: AtomicI32 = AtomicI32::new(0);
static SEM_T4_CONSUMERS_DONE: AtomicI32 = AtomicI32::new(0);
static SEM_T4_LOG_BUDGET: AtomicI32 = AtomicI32::new(0);

const SEM_T4_SEEN_INIT: AtomicI32 = AtomicI32::new(0);
static SEM_T4_SEEN: [AtomicI32; SEM_T4_TOTAL_ITEMS as usize] =
    [SEM_T4_SEEN_INIT; SEM_T4_TOTAL_ITEMS as usize];

fn sem_t4_empty() -> KSemaphore { KSemaphore::from_ptr(SEM_T4_EMPTY.as_mut_ptr()) }
fn sem_t4_full() -> KSemaphore { KSemaphore::from_ptr(SEM_T4_FULL.as_mut_ptr()) }
fn sem_t4_lock() -> KMutex { KMutex::from_ptr(SEM_T4_LOCK.as_mut_ptr()) }

fn t4_log_budget_ok() -> bool {
    SEM_T4_LOG_BUDGET.fetch_add(1, Ordering::SeqCst) + 1 <= 8
}

extern "C" fn sem_t4_producer(_a1: u64, _a2: u64) {
    loop {
        let ticket = SEM_T4_PRODUCE_CURSOR.fetch_add(1, Ordering::SeqCst);
        if ticket >= SEM_T4_TOTAL_ITEMS {
            break;
        }
        if sem_t4_empty().wait().is_err() {
            if t4_log_budget_ok() {
                crate::kprintln!("[sem][T4][prod] sem_wait(empty) failed");
            }
            fail();
            return;
        }
        {
            let _g = sem_t4_lock().lock();
            let head = SEM_T4_HEAD.load(Ordering::SeqCst);
            // SAFETY: `head` is in `0..SEM_T4_BUFFER_CAP`; the mutex above
            // is the sole writer-side synchronisation, mirroring the C
            // `mutex_lock(&sem_t4_lock)` critical section.
            unsafe { (*SEM_T4_BUFFER.0.get())[head as usize] = ticket; }
            SEM_T4_HEAD.store((head + 1) % SEM_T4_BUFFER_CAP, Ordering::SeqCst);
        }
        if sem_t4_full().post().is_err() {
            if t4_log_budget_ok() {
                crate::kprintln!("[sem][T4][prod] sem_post(full) failed");
            }
            fail();
            return;
        }
        scheduler_yield();
    }
    SEM_T4_PRODUCERS_DONE.fetch_add(1, Ordering::SeqCst);
}

extern "C" fn sem_t4_consumer(_a1: u64, _a2: u64) {
    loop {
        if SEM_T4_ITEMS_CONSUMED.load(Ordering::SeqCst) >= SEM_T4_TOTAL_ITEMS
            && SEM_T4_PRODUCERS_DONE.load(Ordering::SeqCst) >= SEM_T4_PRODUCERS
        {
            break;
        }

        if sem_t4_full().wait().is_err() {
            if t4_log_budget_ok() {
                crate::kprintln!("[sem][T4][cons] sem_wait(full) failed");
            }
            fail();
            return;
        }

        let value = {
            let _g = sem_t4_lock().lock();
            if SEM_T4_ITEMS_CONSUMED.load(Ordering::SeqCst) >= SEM_T4_TOTAL_ITEMS {
                drop(_g);
                if sem_t4_full().post().is_err() {
                    fail();
                }
                break;
            }
            let tail = SEM_T4_TAIL.load(Ordering::SeqCst);
            // SAFETY: see `sem_t4_producer`.
            let v = unsafe { (*SEM_T4_BUFFER.0.get())[tail as usize] };
            SEM_T4_TAIL.store((tail + 1) % SEM_T4_BUFFER_CAP, Ordering::SeqCst);
            v
        };

        let mut bad = false;
        if value < 0 || value >= SEM_T4_TOTAL_ITEMS {
            fail();
            bad = true;
        } else if SEM_T4_SEEN[value as usize].swap(1, Ordering::SeqCst) != 0 {
            fail();
            bad = true;
        }
        if bad && t4_log_budget_ok() {
            if value < 0 || value >= SEM_T4_TOTAL_ITEMS {
                crate::kprintln!(
                    "[sem][T4][cons] out-of-range value={} tail={} head={}",
                    value,
                    SEM_T4_TAIL.load(Ordering::SeqCst),
                    SEM_T4_HEAD.load(Ordering::SeqCst),
                );
            } else {
                crate::kprintln!("[sem][T4][cons] duplicate value={}", value);
            }
        }

        let consumed = SEM_T4_ITEMS_CONSUMED.fetch_add(1, Ordering::SeqCst) + 1;

        if sem_t4_empty().post().is_err() {
            if t4_log_budget_ok() {
                crate::kprintln!("[sem][T4][cons] sem_post(empty) failed");
            }
            fail();
            return;
        }

        if consumed >= SEM_T4_TOTAL_ITEMS && SEM_T4_PRODUCERS_DONE.load(Ordering::SeqCst) >= SEM_T4_PRODUCERS {
            break;
        }

        scheduler_yield();
    }
    SEM_T4_CONSUMERS_DONE.fetch_add(1, Ordering::SeqCst);
}

fn sem_run_test4() {
    crate::kprint!("[sem][T4] producer/consumer stress... ");
    SEM_ERROR_FLAG.store(0, Ordering::SeqCst);

    if sem_t4_empty().init(c"sem-empty".as_ptr(), SEM_T4_BUFFER_CAP).is_err() {
        fail();
    }
    if sem_t4_full().init(c"sem-full".as_ptr(), 0).is_err() {
        fail();
    }
    sem_t4_lock().init(c"sem-buffer".as_ptr().cast_mut());

    SEM_T4_HEAD.store(0, Ordering::SeqCst);
    SEM_T4_TAIL.store(0, Ordering::SeqCst);
    SEM_T4_PRODUCE_CURSOR.store(0, Ordering::SeqCst);
    SEM_T4_ITEMS_CONSUMED.store(0, Ordering::SeqCst);
    SEM_T4_PRODUCERS_DONE.store(0, Ordering::SeqCst);
    SEM_T4_CONSUMERS_DONE.store(0, Ordering::SeqCst);
    SEM_T4_LOG_BUDGET.store(0, Ordering::SeqCst);
    for slot in SEM_T4_SEEN.iter() {
        slot.store(0, Ordering::SeqCst);
    }

    for _ in 0..SEM_T4_PRODUCERS {
        if !spawn(c"sem_prod", sem_t4_producer, 0, 0) {
            fail();
        }
    }
    for _ in 0..SEM_T4_CONSUMERS {
        if !spawn(c"sem_cons", sem_t4_consumer, 0, 0) {
            fail();
        }
    }

    if !sem_wait_for(&SEM_T4_PRODUCERS_DONE, SEM_T4_PRODUCERS, 400_000) {
        fail();
    } else {
        for _ in 0..SEM_T4_CONSUMERS {
            if sem_t4_full().post().is_err() && t4_log_budget_ok() {
                crate::kprintln!("[sem][T4] failed to post wake sentinel for consumers");
            }
        }
    }
    if !sem_wait_for(&SEM_T4_CONSUMERS_DONE, SEM_T4_CONSUMERS, 400_000) {
        fail();
    }

    let consumed = SEM_T4_ITEMS_CONSUMED.load(Ordering::SeqCst);
    if consumed != SEM_T4_TOTAL_ITEMS {
        if t4_log_budget_ok() {
            crate::kprintln!("[sem][T4] consumed={} expected={}", consumed, SEM_T4_TOTAL_ITEMS);
        }
        fail();
    }

    for i in 0..SEM_T4_TOTAL_ITEMS {
        if SEM_T4_SEEN[i as usize].load(Ordering::SeqCst) == 0 {
            if t4_log_budget_ok() {
                crate::kprintln!("[sem][T4] missing item {}", i);
            }
            fail();
        }
    }

    print_result();
}

// ---------------------------------------------------------------------------
// Master + entry point
// ---------------------------------------------------------------------------

extern "C" fn semaphore_test_master(_a1: u64, _a2: u64) {
    for _ in 0..10_000 {
        scheduler_yield();
    }

    crate::kprintln!("[sem] starting semaphore tests");
    sem_run_test1();
    sem_run_test2();
    sem_run_test3();
    sem_run_test4();
    crate::kprintln!("[sem] tests finished");
}

/// Spawn the semaphore test-suite master kthread. Preserves the original
/// `void semaphore_launch_tests(void)` C-ABI entry point called from
/// `kernel/start_kernel.c` under `#ifdef SEMAPHORE_RUNTIME_TEST`.
#[no_mangle]
pub extern "C" fn semaphore_launch_tests() {
    if !spawn(c"semaphore_test_master", semaphore_test_master, 0, 0) {
        crate::kprintln!("[sem] cannot create test master thread");
    }
}
