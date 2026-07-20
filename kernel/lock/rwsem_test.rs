//! Rust port of `kernel/lock/rwsem_test.c`.
//!
//! Reader/writer semaphore test suite (no artificial busy delays):
//!  1. Multiple readers can hold the lock concurrently.
//!  2. A writer waits until all readers release.
//!  3. Writers are mutually exclusive.
//!  4. Data consistency under mixed reader/writer stress.
//!
//! Entry point: [`rwsem_launch_tests`], called from
//! `kernel/start_kernel.rs` under `#[cfg(feature = "rwlock_test")]` via a
//! direct crate-path call (P3-D3c: the `#[no_mangle] extern "C"` C-ABI
//! surface is gone -- the ctest harness only toggles the cmake env gate,
//! it never resolves this symbol by name).

#![allow(non_camel_case_types, non_snake_case)]

use core::cell::UnsafeCell;
use core::ffi::{c_int, c_void, CStr};
use core::mem::MaybeUninit;
use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{mutex_t, rwsem_t, spinlock_t, tq_t};
use crate::lock::mutex::KMutex;
use crate::lock::rwsem::KRwSem;

// P3-D2a: proc/sched.rs + proc/thread_queue.rs entry points, reached as
// plain crate-path items instead of `extern "C"` redeclarations.
use crate::proc::{scheduler_yield, tq_size, wakeup};

// P3-D3b: `kthread_create` (proc/thread.rs) is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone; reached via the `crate::proc`
// glob re-export.


const KERNEL_STACK_ORDER: c_int = 2;

// `is_err_or_null`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::is_err_or_null;

/// `Sync` newtype for `static` storage of a POD C struct initialised at
/// runtime via a `*_init` call (mirrors `rcu.rs`'s `StaticCell`).
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

fn spawn(name: &'static CStr, entry: extern "C" fn(u64, u64)) -> bool {
    let np = crate::proc::thread::Thread::kthread_create(name.as_ptr(), entry as *mut c_void, 0, 0, KERNEL_STACK_ORDER);
    if is_err_or_null(np) {
        false
    } else {
        wakeup(np);
        true
    }
}

/// Spin-wait for `*cell == expected`, yielding between polls. Mirrors the
/// C `wait_for` helper (no diagnostic print — the original doesn't have
/// one either).
fn wait_for(cell: &AtomicI32, expected: i32, mut spin_loops: i32) -> bool {
    while spin_loops > 0 {
        spin_loops -= 1;
        if cell.load(Ordering::SeqCst) == expected {
            return true;
        }
        scheduler_yield();
    }
    false
}

// ---------------------------------------------------------------------------
// Shared instrumentation
// ---------------------------------------------------------------------------

static TEST_LOCK: StaticCell<rwsem_t> = StaticCell::uninit();
fn test_lock() -> KRwSem { KRwSem::from_ptr(TEST_LOCK.as_mut_ptr()) }

static ACTIVE_READERS: AtomicI32 = AtomicI32::new(0);
static MAX_ACTIVE_READERS: AtomicI32 = AtomicI32::new(0);
static ACTIVE_WRITERS: AtomicI32 = AtomicI32::new(0);
static ERROR_FLAG: AtomicI32 = AtomicI32::new(0);

fn fail() {
    ERROR_FLAG.store(1, Ordering::SeqCst);
}

fn print_result() {
    if ERROR_FLAG.load(Ordering::SeqCst) != 0 {
        // SAFETY: static NUL-terminated format string, no arguments.
        unsafe { crate::kprintln!("FAIL"); }
    } else {
        // SAFETY: see above.
        unsafe { crate::kprintln!("OK"); }
    }
}

static INTEGRITY_LOG_COUNT: AtomicI32 = AtomicI32::new(0);

fn record_integrity_failure(label: &CStr, reason: &CStr, v1: i64, v2: i64) {
    if INTEGRITY_LOG_COUNT.fetch_add(1, Ordering::SeqCst) + 1 <= 8 {
        // SAFETY: static format string; args match %s/%s/%ld/%ld.
        unsafe {
            crate::kprintln!(
                "[rwsem][integrity][{}] {} (v1={} v2={})",
                crate::printf::Cs(label.as_ptr()),
                crate::printf::Cs(reason.as_ptr()),
                v1,
                v2,
            );
        }
    }
    fail();
}

// --- raw `rwsem_t` field accessors (diagnostics only; the actual locking
// policy lives entirely in `crate::lock::rwsem`) ---------------------------

fn read_queue_ptr(l: *mut rwsem_t) -> *mut tq_t {
    // SAFETY: `l` points at a live, initialised `rwsem_t`.
    unsafe { addr_of_mut!((*l).read_queue) }
}
fn write_queue_ptr(l: *mut rwsem_t) -> *mut tq_t {
    // SAFETY: see `read_queue_ptr`.
    unsafe { addr_of_mut!((*l).write_queue) }
}
fn lock_ptr(l: *mut rwsem_t) -> *mut spinlock_t {
    // SAFETY: see `read_queue_ptr`.
    unsafe { addr_of_mut!((*l).lock) }
}
fn get_readers(l: *mut rwsem_t) -> c_int {
    // SAFETY: see `read_queue_ptr`; racy read for diagnostics, matching
    // the original C `test_lock.readers` direct-field read.
    unsafe { (*l).readers }
}
fn get_holder(l: *mut rwsem_t) -> c_int {
    // SAFETY: see `get_readers`.
    unsafe { (*l).holder_pid }
}
fn queue_lock(q: *mut tq_t) -> *mut spinlock_t {
    // SAFETY: `q` points at a live, initialised `tq_t`.
    unsafe { (*q).lock }
}

fn check_rwsem_integrity(label: &CStr) {
    let l = TEST_LOCK.as_mut_ptr();
    let read_waiters = tq_size(read_queue_ptr(l));
    let write_waiters = tq_size(write_queue_ptr(l));
    if read_waiters < 0 || write_waiters < 0 {
        record_integrity_failure(
            label,
            c"negative waiter count",
            read_waiters as i64,
            write_waiters as i64,
        );
        return;
    }

    let readers = get_readers(l);
    if readers < 0 {
        record_integrity_failure(label, c"negative readers", readers as i64, 0);
    }

    let holder = get_holder(l);
    if readers > 0 && holder != -1 {
        record_integrity_failure(label, c"reader-writer overlap", readers as i64, holder as i64);
    }

    if holder == -1 && write_waiters < 0 {
        record_integrity_failure(label, c"invalid write waiters", write_waiters as i64, 0);
    }

    if holder != -1 && readers != 0 {
        record_integrity_failure(label, c"writer with readers", readers as i64, holder as i64);
    }

    let rq_lock = queue_lock(read_queue_ptr(l));
    let lk = lock_ptr(l);
    if rq_lock != lk {
        record_integrity_failure(label, c"read queue lock mismatch", rq_lock as i64, lk as i64);
    }

    let wq_lock = queue_lock(write_queue_ptr(l));
    if wq_lock != lk {
        record_integrity_failure(label, c"write queue lock mismatch", wq_lock as i64, lk as i64);
    }
}

// ---------------------------------------------------------------------------
// Test 1: multiple readers
// ---------------------------------------------------------------------------

static T1_TARGET_READERS: AtomicI32 = AtomicI32::new(0);
static T1_DONE_READERS: AtomicI32 = AtomicI32::new(0);
static T1_STARTED_READERS: AtomicI32 = AtomicI32::new(0);
static T1_RELEASE_READERS: AtomicI32 = AtomicI32::new(0);

extern "C" fn t1_reader(_a1: u64, _a2: u64) {
    let g = match test_lock().read() {
        Ok(g) => g,
        Err(_) => { fail(); return; }
    };
    check_rwsem_integrity(c"T1 reader acquired");
    let ar = ACTIVE_READERS.fetch_add(1, Ordering::SeqCst) + 1;
    let mut prev = MAX_ACTIVE_READERS.load(Ordering::SeqCst);
    while ar > prev {
        match MAX_ACTIVE_READERS.compare_exchange(prev, ar, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => break,
            Err(v) => prev = v,
        }
    }
    T1_STARTED_READERS.fetch_add(1, Ordering::SeqCst);
    // Wait until main test signals release so all readers hold the lock
    // concurrently.
    while T1_RELEASE_READERS.load(Ordering::SeqCst) == 0 {
        scheduler_yield();
    }
    ACTIVE_READERS.fetch_sub(1, Ordering::SeqCst);
    drop(g);
    check_rwsem_integrity(c"T1 reader released");
    T1_DONE_READERS.fetch_add(1, Ordering::SeqCst);
}

fn run_test1() {
    // SAFETY: static format string.
    unsafe { crate::kprint!("[rwsem][T1] multiple readers... "); }
    let target: i32 = 4;
    T1_TARGET_READERS.store(target, Ordering::SeqCst);
    T1_DONE_READERS.store(0, Ordering::SeqCst);
    T1_STARTED_READERS.store(0, Ordering::SeqCst);
    T1_RELEASE_READERS.store(0, Ordering::SeqCst);
    ACTIVE_READERS.store(0, Ordering::SeqCst);
    MAX_ACTIVE_READERS.store(0, Ordering::SeqCst);
    ERROR_FLAG.store(0, Ordering::SeqCst);

    for _ in 0..target {
        if !spawn(c"run_test1", t1_reader) {
            fail();
        }
    }
    if !wait_for(&T1_STARTED_READERS, target, 50_000) {
        fail();
    }
    T1_RELEASE_READERS.store(1, Ordering::SeqCst);
    if !wait_for(&T1_DONE_READERS, target, 50_000) {
        fail();
    }
    let observed_max = MAX_ACTIVE_READERS.load(Ordering::SeqCst);
    if observed_max != target {
        // SAFETY: static format string; args match %d/%d/%d.
        unsafe {
            crate::kprint!(
                "(observed max={} started={} expected={}) ",
                observed_max,
                T1_STARTED_READERS.load(Ordering::SeqCst),
                target,
            );
        }
        fail();
    }
    check_rwsem_integrity(c"T1 final");
    print_result();
}

// ---------------------------------------------------------------------------
// Test 2: writer waits for readers
// ---------------------------------------------------------------------------

static T2_TARGET_READERS: AtomicI32 = AtomicI32::new(0);
static T2_DONE_READERS: AtomicI32 = AtomicI32::new(0);
static T2_WRITER_ACQUIRED: AtomicI32 = AtomicI32::new(0);

extern "C" fn t2_reader(_a1: u64, _a2: u64) {
    let g = match test_lock().read() {
        Ok(g) => g,
        Err(_) => { fail(); return; }
    };
    check_rwsem_integrity(c"T2 reader acquired");
    ACTIVE_READERS.fetch_add(1, Ordering::SeqCst);
    for _ in 0..5 {
        scheduler_yield();
    }
    ACTIVE_READERS.fetch_sub(1, Ordering::SeqCst);
    drop(g);
    check_rwsem_integrity(c"T2 reader released");
    T2_DONE_READERS.fetch_add(1, Ordering::SeqCst);
}

extern "C" fn t2_writer(_a1: u64, _a2: u64) {
    let g = match test_lock().write() {
        Ok(g) => g,
        Err(_) => { fail(); return; }
    };
    check_rwsem_integrity(c"T2 writer acquired");
    if ACTIVE_READERS.load(Ordering::SeqCst) != 0 {
        // SAFETY: static format string; arg matches %d.
        unsafe {
            crate::kprintln!(
                "[rwsem][T2] writer saw active_readers={} (expected 0)",
                ACTIVE_READERS.load(Ordering::SeqCst),
            );
        }
        fail();
    }
    ACTIVE_WRITERS.store(1, Ordering::SeqCst);
    T2_WRITER_ACQUIRED.store(1, Ordering::SeqCst);
    for _ in 0..5 {
        scheduler_yield();
    }
    ACTIVE_WRITERS.store(0, Ordering::SeqCst);
    drop(g);
    check_rwsem_integrity(c"T2 writer released");
}

fn run_test2() {
    // SAFETY: static format string.
    unsafe { crate::kprint!("[rwsem][T2] writer waits for readers... "); }
    let target: i32 = 3;
    T2_TARGET_READERS.store(target, Ordering::SeqCst);
    T2_DONE_READERS.store(0, Ordering::SeqCst);
    T2_WRITER_ACQUIRED.store(0, Ordering::SeqCst);
    ACTIVE_READERS.store(0, Ordering::SeqCst);
    ACTIVE_WRITERS.store(0, Ordering::SeqCst);
    ERROR_FLAG.store(0, Ordering::SeqCst);

    for _ in 0..target {
        if !spawn(c"run_test2", t2_reader) {
            fail();
        }
    }
    if !wait_for(&T2_DONE_READERS, target, 80_000) {
        fail();
    }
    if !spawn(c"run_test2", t2_writer) {
        fail();
    }
    if !wait_for(&T2_WRITER_ACQUIRED, 1, 40_000) {
        fail();
    }
    if ACTIVE_READERS.load(Ordering::SeqCst) != 0 {
        fail();
    }
    check_rwsem_integrity(c"T2 final");
    print_result();
}

// ---------------------------------------------------------------------------
// Test 3: mutual exclusion for writers
// ---------------------------------------------------------------------------

static T3_DONE_WRITERS: AtomicI32 = AtomicI32::new(0);

extern "C" fn t3_writer(_a1: u64, _a2: u64) {
    let g = match test_lock().write() {
        Ok(g) => g,
        Err(_) => { fail(); return; }
    };
    check_rwsem_integrity(c"T3 writer acquired");
    if ACTIVE_WRITERS.load(Ordering::SeqCst) != 0 {
        // SAFETY: static format string; arg matches %d.
        unsafe {
            crate::kprintln!(
                "[rwsem][T3] mutual exclusion violated (active_writers={})",
                ACTIVE_WRITERS.load(Ordering::SeqCst),
            );
        }
        fail();
    }
    ACTIVE_WRITERS.store(1, Ordering::SeqCst);
    scheduler_yield();
    scheduler_yield();
    scheduler_yield();
    ACTIVE_WRITERS.store(0, Ordering::SeqCst);
    drop(g);
    check_rwsem_integrity(c"T3 writer released");
    T3_DONE_WRITERS.fetch_add(1, Ordering::SeqCst);
}

fn run_test3() {
    // SAFETY: static format string.
    unsafe { crate::kprint!("[rwsem][T3] mutual exclusion for writers... "); }
    T3_DONE_WRITERS.store(0, Ordering::SeqCst);
    ACTIVE_WRITERS.store(0, Ordering::SeqCst);
    ERROR_FLAG.store(0, Ordering::SeqCst);

    if !spawn(c"run_test3", t3_writer) {
        fail();
    }
    if !spawn(c"run_test3", t3_writer) {
        fail();
    }
    if !wait_for(&T3_DONE_WRITERS, 2, 80_000) {
        fail();
    }
    check_rwsem_integrity(c"T3 final");
    print_result();
}

// ---------------------------------------------------------------------------
// Test 4: data consistency under stress
// ---------------------------------------------------------------------------

const T4_DATA_LEN: usize = 32;
const T4_WRITER_ITERS: i32 = 150;
const T4_WRITER_THREADS: i32 = 2;
const T4_READER_THREADS: i32 = 6;

#[repr(C)]
struct T4Dataset {
    version: i32,
    len: i32,
    checksum: i32,
    data: [i32; T4_DATA_LEN],
}
struct T4Cell(UnsafeCell<T4Dataset>);
unsafe impl Sync for T4Cell {}
static T4_DS: T4Cell = T4Cell(UnsafeCell::new(T4Dataset {
    version: 0,
    len: 0,
    checksum: 0,
    data: [0; T4_DATA_LEN],
}));

static T4_WRITERS_DONE: AtomicI32 = AtomicI32::new(0);
static T4_READER_DONE: AtomicI32 = AtomicI32::new(0);
static T4_ERROR_LOGS: AtomicI32 = AtomicI32::new(0);
static T4_START_LOCK: StaticCell<mutex_t> = StaticCell::uninit();
fn t4_start_lock() -> KMutex { KMutex::from_ptr(T4_START_LOCK.as_mut_ptr()) }

fn t4_log_budget_ok() -> bool {
    T4_ERROR_LOGS.fetch_add(1, Ordering::SeqCst) + 1 <= 10
}

extern "C" fn t4_writer(_a1: u64, _a2: u64) {
    // Barrier: block on the sleeplock until the master releases it, then
    // immediately release so everyone proceeds together.
    drop(t4_start_lock().lock());

    for _ in 0..T4_WRITER_ITERS {
        let g = match test_lock().write() {
            Ok(g) => g,
            Err(_) => { fail(); return; }
        };
        check_rwsem_integrity(c"T4 writer acquired");
        // SAFETY: the write guard `g` gives this thread exclusive access
        // to `T4_DS` for the duration of this block, matching the C
        // critical section under `rwsem_acquire_write`/`rwsem_release`.
        unsafe {
            let ds = &mut *T4_DS.0.get();
            let new_version = ds.version + 1;
            ds.version = new_version;
            ds.len = T4_DATA_LEN as i32;
            let mut sum: i32 = 0;
            for i in 0..T4_DATA_LEN {
                let val = (new_version << 16) ^ ((i as i32).wrapping_mul(0x9e37));
                ds.data[i] = val;
                sum = sum.wrapping_add(val);
            }
            ds.checksum = sum;
        }
        drop(g);
        check_rwsem_integrity(c"T4 writer released");
        scheduler_yield(); // allow readers to interleave
    }
    T4_WRITERS_DONE.fetch_add(1, Ordering::SeqCst);
}

extern "C" fn t4_reader(_a1: u64, _a2: u64) {
    drop(t4_start_lock().lock());

    loop {
        let g = match test_lock().read() {
            Ok(g) => g,
            Err(_) => { fail(); return; }
        };
        check_rwsem_integrity(c"T4 reader acquired");
        // SAFETY: the read guard `g` excludes concurrent writers, so the
        // snapshot below is internally consistent (multiple readers may
        // race with each other on plain loads, which is fine — they only
        // observe a writer-published, self-consistent snapshot).
        let (version, len, checksum, data_copy) = unsafe {
            let ds = &*T4_DS.0.get();
            (ds.version, ds.len, ds.checksum, ds.data)
        };
        if len != T4_DATA_LEN as i32 {
            if t4_log_budget_ok() {
                // SAFETY: static format string; arg matches %d.
                unsafe { crate::kprintln!("[rwsem][T4] len mismatch {}", len); }
            }
            fail();
        } else if version > 0 {
            let mut sum: i32 = 0;
            let mut bad = false;
            for i in 0..len as usize {
                let expected = (version << 16) ^ ((i as i32).wrapping_mul(0x9e37));
                let got = data_copy[i];
                if got != expected {
                    if t4_log_budget_ok() {
                        // SAFETY: static format string; args match %d/%x/%x/%d.
                        unsafe {
                            crate::kprintln!(
                                "[rwsem][T4] data[{}]={:x} expected {:x} (ver={})",
                                i as i32,
                                ((got as i32 as i64) as u64),
                                ((expected as i32 as i64) as u64),
                                version,
                            );
                        }
                    }
                    fail();
                    bad = true;
                    break;
                }
                sum = sum.wrapping_add(got);
            }
            if !bad && sum != checksum {
                if t4_log_budget_ok() {
                    // SAFETY: static format string; args match %x/%x/%d.
                    unsafe {
                        crate::kprintln!(
                            "[rwsem][T4] checksum mismatch sum={:x} stored={:x} ver={}",
                            ((sum as i32 as i64) as u64),
                            ((checksum as i32 as i64) as u64),
                            version,
                        );
                    }
                }
                fail();
            }
        }
        drop(g);
        check_rwsem_integrity(c"T4 reader released");
        if T4_WRITERS_DONE.load(Ordering::SeqCst) >= T4_WRITER_THREADS {
            break;
        }
        scheduler_yield();
    }
    T4_READER_DONE.fetch_add(1, Ordering::SeqCst);
}

fn run_test4() {
    // SAFETY: static format string.
    unsafe { crate::kprint!("[rwsem][T4] data consistency under stress... "); }
    ERROR_FLAG.store(0, Ordering::SeqCst);
    // SAFETY: exclusive access — no test thread has been spawned yet.
    unsafe {
        let ds = &mut *T4_DS.0.get();
        ds.version = 0;
        ds.len = T4_DATA_LEN as i32;
        ds.checksum = 0;
        ds.data = [0; T4_DATA_LEN];
    }
    T4_WRITERS_DONE.store(0, Ordering::SeqCst);
    T4_READER_DONE.store(0, Ordering::SeqCst);
    T4_ERROR_LOGS.store(0, Ordering::SeqCst);
    t4_start_lock().init(c"t4start".as_ptr().cast_mut());

    // Acquire the barrier sleeplock so spawned threads block when they try
    // to acquire it.
    let barrier = t4_start_lock().lock();

    for _ in 0..T4_WRITER_THREADS {
        if !spawn(c"run_test4", t4_writer) {
            fail();
        }
    }
    for _ in 0..T4_READER_THREADS {
        if !spawn(c"run_test4", t4_reader) {
            fail();
        }
    }
    // Release the barrier.
    drop(barrier);

    if !wait_for(&T4_WRITERS_DONE, T4_WRITER_THREADS, 400_000) {
        fail();
    }
    if !wait_for(&T4_READER_DONE, T4_READER_THREADS, 400_000) {
        fail();
    }
    check_rwsem_integrity(c"T4 final");
    print_result();
}

// ---------------------------------------------------------------------------
// Master + entry point
// ---------------------------------------------------------------------------

extern "C" fn rwsem_test_master(_a1: u64, _a2: u64) {
    for _ in 0..10_000 {
        scheduler_yield();
    }
    // SAFETY: static format strings, no arguments, throughout this fn.
    unsafe { crate::kprintln!("[rwsem] starting simple rwsem tests"); }
    if test_lock().init(0, c"rwsem-test".as_ptr()).is_err() {
        unsafe { crate::kprintln!("[rwsem] init failed"); }
        return;
    }
    check_rwsem_integrity(c"init");
    run_test1();
    run_test2();
    run_test3();
    run_test4();
    unsafe { crate::kprintln!("[rwsem] tests finished"); }
}

/// Spawn the rwsem test-suite master kthread. Called from
/// `start_kernel.rs` under `#[cfg(feature = "rwlock_test")]` (P3-D3c:
/// direct crate-path call, no more C-ABI entry point).
pub fn rwsem_launch_tests() {
    if !spawn(c"rwsem_test_master", rwsem_test_master) {
        // SAFETY: static format string, no arguments.
        unsafe { crate::kprintln!("[rwsem] cannot create test master thread"); }
    }
}
