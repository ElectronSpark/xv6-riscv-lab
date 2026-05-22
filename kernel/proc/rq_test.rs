//! Pure-Rust port of `kernel/proc/rq_test.c` (run-queue priority integration
//! tests). Owns both `rq_test_run` (canonical) and `xv6_rqtest_pub_run`
//! aliases. Translated from SECTION 14 of `proc_rust_shims.c`.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{
    cpumask_t, rq, sched_attr, sched_entity, spinlock_t, thread, tq_t,
};
use crate::lock::spinlock::spin_init;
use crate::lock::spinlock::spin_lock;
use crate::lock::spinlock::spin_unlock;
use crate::machine::cpuid;
use crate::proc::access::{is_err_or_null, RqRef, SchedEntityRef, ThreadAccess, zeroed};
use crate::proc::xv6_rqport_pick_next_rq;
use crate::proc::xv6_rqport_rq_lock;
use crate::proc::xv6_rqport_rq_unlock;
use crate::proc::xv6_rqport_sched_attr_init;
use crate::proc::xv6_rqport_sched_getattr;
use crate::proc::xv6_rqport_sched_setattr;

// --- priority macros (mirror inc/proc/rq.h) -------------------------------
const PRIORITY_SUBLEVEL_MASK: c_int = 0x03;
const PRIORITY_MAINLEVEL_MASK: c_int = 0xFC;
const PRIORITY_MAINLEVEL_SHIFT: c_int = 2;
#[inline] const fn MAJOR_PRIORITY(p: c_int) -> c_int { (p & PRIORITY_MAINLEVEL_MASK) >> PRIORITY_MAINLEVEL_SHIFT }
#[inline] const fn MINOR_PRIORITY(p: c_int) -> c_int { p & PRIORITY_SUBLEVEL_MASK }
#[inline] const fn MAKE_PRIORITY(major: c_int, minor: c_int) -> c_int { (major << PRIORITY_MAINLEVEL_SHIFT) | minor }

// --- C primitives ---------------------------------------------------------
unsafe extern "C" {
    pub safe fn printf(fmt: *const c_char, ...) -> c_int;


    pub safe fn xv6_current_thread() -> *mut thread;
    pub safe fn xv6_panic(msg: *const c_char) -> !;


    pub safe fn xv6_schport_scheduler_yield();
    pub safe fn xv6_schport_wakeup(t: *mut thread);

    pub safe fn xv6_thport_kthread_create(
        name: *const c_char,
        entry: *mut c_void,
        arg1: u64,
        arg2: u64,
        kstack_order: c_int,
    ) -> *mut thread;

    pub safe fn xv6_tqport_tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t);
    pub safe fn xv6_tqport_tq_wait(q: *mut tq_t, lk: *mut spinlock_t, rdata: *mut u64) -> c_int;
    pub safe fn xv6_tqport_tq_wakeup_all(q: *mut tq_t, errno: c_int, rdata: u64) -> c_int;
}

#[inline]
fn assert_msg(cond: bool, msg: *const c_char) {
    if !cond {
        xv6_panic(msg);
    }
}

macro_rules! rq_test_raw {
    ($($body:tt)*) => {{
        unsafe { $($body)* }
    }};
}

#[inline]
fn sched_entity_of(p: *mut thread) -> *mut sched_entity {
    ThreadAccess::assume(p).sched_entity_ptr()
}

#[inline]
fn current_sched_entity() -> *mut sched_entity {
    sched_entity_of(xv6_current_thread())
}

// --- Test 1: two-layer bitmask --------------------------------------------
fn test_two_layer_mask() {
    rq_test_raw! {
        printf(c"TEST: Two-Layer Bitmask Logic\n".as_ptr());
        let cases: [[c_int; 3]; 7] = [
            [0, 0, 0], [1, 0, 1], [7, 0, 7], [8, 1, 0],
            [15, 1, 7], [16, 2, 0], [63, 7, 7],
        ];
        let mut passed = 0;
        for c in cases.iter() {
            let (major, eg, eb) = (c[0], c[1], c[2]);
            let ag = major >> 3;
            let ab = major & 7;
            if ag == eg && ab == eb {
                passed += 1;
            } else {
                printf(c"  FAIL: major %d -> group %d bit %d, expected group %d bit %d\n".as_ptr(),
                    major, ag, ab, eg, eb);
            }
        }
        assert_msg(passed == cases.len() as c_int, c"rq_test: bitmask mapping failed".as_ptr());
        printf(c"  PASSED: %d/%d bitmask mappings correct\n".as_ptr(), passed, cases.len() as c_int);
    }
}

// --- Test 2: priority change ----------------------------------------------
fn test_priority_change() {
    rq_test_raw! {
        printf(c"TEST: Priority Change via xv6_rqport_sched_setattr\n".as_ptr());
        let se = current_sched_entity();

        let mut attr: sched_attr = zeroed();
        xv6_rqport_sched_getattr(se, &mut attr);
        let original_priority = attr.priority;
        let original_major = MAJOR_PRIORITY(original_priority);

        printf(c"  Original priority: major=%d minor=%d\n".as_ptr(),
            original_major, MINOR_PRIORITY(original_priority));

        let new_major = if original_major == 10 { 12 } else { 10 };
        attr.priority = MAKE_PRIORITY(new_major, 1);

        let ret = xv6_rqport_sched_setattr(se, &attr);
        assert_msg(ret == 0, c"rq_test: xv6_rqport_sched_setattr failed".as_ptr());

        let mut new_attr: sched_attr = zeroed();
        xv6_rqport_sched_getattr(se, &mut new_attr);
        let changed_major = MAJOR_PRIORITY(new_attr.priority);
        let changed_minor = MINOR_PRIORITY(new_attr.priority);

        printf(c"  Changed priority: major=%d minor=%d\n".as_ptr(), changed_major, changed_minor);

        assert_msg(changed_major == new_major, c"rq_test: major priority not changed".as_ptr());
        assert_msg(changed_minor == 1, c"rq_test: minor priority not changed".as_ptr());

        xv6_schport_scheduler_yield();

        attr.priority = original_priority;
        xv6_rqport_sched_setattr(se, &attr);

        printf(c"  Restored original priority\n".as_ptr());
        printf(c"  PASSED\n".as_ptr());
    }
}

// --- Test 3: yield priority -----------------------------------------------
fn test_yield_priority() {
    rq_test_raw! {
        printf(c"TEST: Yield Respects Priority\n".as_ptr());
        let p = xv6_current_thread();
        let my_priority = SchedEntityRef::assume(sched_entity_of(p)).priority();
        printf(c"  Current process '%s' at priority major=%d\n".as_ptr(),
            ThreadAccess::assume(p).name_ptr(), MAJOR_PRIORITY(my_priority));

        let mut yields_completed = 0;
        for _ in 0..5 {
            xv6_schport_scheduler_yield();
            yields_completed += 1;
        }
        assert_msg(yields_completed == 5, c"rq_test: not all yields completed".as_ptr());
        printf(c"  Successfully yielded %d times and got rescheduled\n".as_ptr(), yields_completed);
        printf(c"  PASSED\n".as_ptr());
    }
}

// --- Test 4: rq selection consistency -------------------------------------
fn test_rq_selection() {
    rq_test_raw! {
        printf(c"TEST: RQ Selection Consistency\n".as_ptr());
        let test_cpu = cpuid();

        xv6_rqport_rq_lock(test_cpu);
        let rq1 = xv6_rqport_pick_next_rq();
        let rq2 = xv6_rqport_pick_next_rq();
        let rq3 = xv6_rqport_pick_next_rq();
        xv6_rqport_rq_unlock(test_cpu);

        assert_msg(rq1 == rq2 && rq2 == rq3, c"rq_test: inconsistent rq selection".as_ptr());
        printf(c"  Consistent selection: class_id=%d\n".as_ptr(), RqRef::assume(rq1).class_id());
        printf(c"  PASSED\n".as_ptr());
    }
}

// --- Test 5: priority ordering --------------------------------------------
fn verify_priority(se: *mut sched_entity, expected_major: c_int, expected_minor: c_int) -> bool {
    rq_test_raw! {
        let mut attr: sched_attr = zeroed();
        xv6_rqport_sched_getattr(se, &mut attr);
        let am = MAJOR_PRIORITY(attr.priority);
        let an = MINOR_PRIORITY(attr.priority);
        if am != expected_major || an != expected_minor {
            printf(c"    FAIL: expected (%d,%d) got (%d,%d)\n".as_ptr(),
                expected_major, expected_minor, am, an);
            return false;
        }
        true
    }
}

fn set_and_check(se: *mut sched_entity, major: c_int, minor: c_int) {
    rq_test_raw! {
        let mut attr: sched_attr = zeroed();
        xv6_rqport_sched_attr_init(&mut attr);
        attr.priority = MAKE_PRIORITY(major, minor);
        xv6_rqport_sched_setattr(se, &attr);
        assert_msg(verify_priority(se, major, minor), c"rq_test: priority set failed".as_ptr());
        xv6_schport_scheduler_yield();
    }
}

fn pick_major(test_cpu: c_int) -> c_int {
    rq_test_raw! {
        xv6_rqport_rq_lock(test_cpu);
        let r = xv6_rqport_pick_next_rq();
        let m = unsafe { (*r).class_id };
        xv6_rqport_rq_unlock(test_cpu);
        m
    }
}

fn test_priority_ordering() {
    rq_test_raw! {
        printf(c"TEST: Priority Ordering (Comprehensive)\n".as_ptr());
        let se = current_sched_entity();
        let test_cpu = cpuid();

        let mut original_attr: sched_attr = zeroed();
        xv6_rqport_sched_getattr(se, &mut original_attr);

        // Case 1: different top-layer groups
        printf(c"  Case 1: Different top-layer groups\n".as_ptr());
        for &(major, group) in &[(1i32, 0i32), (9, 1), (17, 2), (50, 6)] {
            set_and_check(se, major, 0);
            let pm = pick_major(test_cpu);
            printf(c"    major=%d (group %d): xv6_rqport_pick_next_rq returned %d\n".as_ptr(), major, group, pm);
        }
        printf(c"    Case 1 PASSED\n".as_ptr());

        // Case 2: same group, different bits
        printf(c"  Case 2: Same group, different secondary bits\n".as_ptr());
        for &major in &[1i32, 3, 5, 7] {
            set_and_check(se, major, 0);
            let pm = pick_major(test_cpu);
            printf(c"    major=%d (bit %d): xv6_rqport_pick_next_rq returned %d\n".as_ptr(), major, major, pm);
        }
        printf(c"    Case 2 PASSED\n".as_ptr());

        // Case 3: same major, different minor
        printf(c"  Case 3: Same major, different minor priorities\n".as_ptr());
        for minor in 0..4 {
            set_and_check(se, 5, minor);
            printf(c"    major=5, minor=%d: priority set and yield OK\n".as_ptr(), minor);
        }
        printf(c"    Case 3 PASSED\n".as_ptr());

        // Case 4: boundary transitions
        printf(c"  Case 4: Group boundary transitions\n".as_ptr());
        for &(major, label) in &[
            (7i32, c"    major=7 (end of group 0): xv6_rqport_pick_next_rq returned %d\n"),
            (8,    c"    major=8 (start of group 1): xv6_rqport_pick_next_rq returned %d\n"),
            (62,   c"    major=62 (lowest usable): xv6_rqport_pick_next_rq returned %d\n"),
        ] {
            set_and_check(se, major, 0);
            let pm = pick_major(test_cpu);
            printf(label.as_ptr(), pm);
        }
        printf(c"    Case 4 PASSED\n".as_ptr());

        xv6_rqport_sched_setattr(se, &original_attr);
        printf(c"  All priority ordering cases PASSED\n".as_ptr());
        printf(c"  PASSED\n".as_ptr());
    }
}

// --- Test 6: priority-ordered activation ----------------------------------
const PRIORITY_TEST_COUNT: usize = 5;

static ACTIVATION_ORDER: [AtomicI32; PRIORITY_TEST_COUNT] = [
    AtomicI32::new(-1), AtomicI32::new(-1), AtomicI32::new(-1),
    AtomicI32::new(-1), AtomicI32::new(-1),
];
static ACTIVATION_INDEX: AtomicI32 = AtomicI32::new(0);
static PROCESSES_DONE: AtomicI32 = AtomicI32::new(0);

struct LockCell(core::cell::UnsafeCell<core::mem::MaybeUninit<spinlock_t>>);
unsafe impl Sync for LockCell {}
static PRIORITY_TEST_LOCK: LockCell =
    LockCell(core::cell::UnsafeCell::new(core::mem::MaybeUninit::zeroed()));

struct TqCell(core::cell::UnsafeCell<core::mem::MaybeUninit<tq_t>>);
unsafe impl Sync for TqCell {}
static MAIN_WAIT_QUEUE: TqCell =
    TqCell(core::cell::UnsafeCell::new(core::mem::MaybeUninit::zeroed()));

#[inline] fn lock_ptr() -> *mut spinlock_t { PRIORITY_TEST_LOCK.0.get() as *mut spinlock_t }
#[inline] fn tq_ptr() -> *mut tq_t { MAIN_WAIT_QUEUE.0.get() as *mut tq_t }

// MAKE_PRIORITY values must be const for static array.
const TEST_PRIORITIES: [c_int; PRIORITY_TEST_COUNT] = [
    (50 << 2) | 0,
    (17 << 2) | 0,
    (5  << 2) | 0,
    (25 << 2) | 0,
    (2  << 2) | 0,
];
const EXPECTED_ORDER: [c_int; PRIORITY_TEST_COUNT] = [4, 2, 1, 3, 0];

unsafe extern "C" fn priority_test_proc_entry(my_index: u64, _unused: u64) -> c_int {
    rq_test_raw! {
        spin_lock(lock_ptr());
        let my_order = ACTIVATION_INDEX.fetch_add(1, Ordering::AcqRel);
        ACTIVATION_ORDER[my_index as usize].store(my_order, Ordering::Release);
        let done = PROCESSES_DONE.fetch_add(1, Ordering::AcqRel) + 1;
        let all_done = done == PRIORITY_TEST_COUNT as c_int;
        spin_unlock(lock_ptr());

        if all_done {
            xv6_tqport_tq_wakeup_all(tq_ptr(), 0, 0);
        }
        0
    }
}

fn test_priority_ordered_activation() {
    rq_test_raw! {
        printf(c"TEST: Priority-Ordered Process Activation\n".as_ptr());

        spin_init(lock_ptr(), c"prio_test".as_ptr() as *mut c_char);
        xv6_tqport_tq_init(tq_ptr(), c"main_wait".as_ptr(), lock_ptr());
        ACTIVATION_INDEX.store(0, Ordering::Release);
        PROCESSES_DONE.store(0, Ordering::Release);
        for i in 0..PRIORITY_TEST_COUNT {
            ACTIVATION_ORDER[i].store(-1, Ordering::Release);
        }

        let mut test_procs: [*mut thread; PRIORITY_TEST_COUNT] = [core::ptr::null_mut(); PRIORITY_TEST_COUNT];

        printf(c"  Phase 1: Creating %d processes with preemption disabled\n".as_ptr(),
            PRIORITY_TEST_COUNT as c_int);

        let g = crate::machine::PreemptGuard::new();

        let test_cpu = g.cpuid() as c_int;
        let cpu_mask: cpumask_t = 1u64 << test_cpu;

        for i in 0..PRIORITY_TEST_COUNT {
            test_procs[i] = xv6_thport_kthread_create(
                c"prio_test".as_ptr(),
                priority_test_proc_entry as *mut c_void,
                i as u64, 0, 0,
            );
            assert_msg(!is_err_or_null(test_procs[i]), c"rq_test: kthread_create failed".as_ptr());

            let se = sched_entity_of(test_procs[i]);
            let mut attr: sched_attr = zeroed();
            xv6_rqport_sched_attr_init(&mut attr);
            attr.priority = TEST_PRIORITIES[i];
            attr.affinity_mask = cpu_mask;
            xv6_rqport_sched_setattr(se, &attr);

            printf(c"    Created process %d (pid=%d) with priority major=%d on CPU %d\n".as_ptr(),
                i as c_int, ThreadAccess::assume(test_procs[i]).pid(), MAJOR_PRIORITY(TEST_PRIORITIES[i]), test_cpu);
        }

        printf(c"  Phase 2: Waking up all processes\n".as_ptr());
        for i in 0..PRIORITY_TEST_COUNT {
            xv6_schport_wakeup(test_procs[i]);
        }

        printf(c"  Phase 3: Enabling preemption and yielding\n".as_ptr());
        drop(g);
        xv6_schport_scheduler_yield();

        printf(c"  Phase 4: Waiting for all processes to complete\n".as_ptr());
        spin_lock(lock_ptr());
        while PROCESSES_DONE.load(Ordering::Acquire) < PRIORITY_TEST_COUNT as c_int {
            xv6_tqport_tq_wait(tq_ptr(), lock_ptr(), core::ptr::null_mut());
        }
        spin_unlock(lock_ptr());

        printf(c"  Phase 5: Verifying activation order\n".as_ptr());
        printf(c"    Expected: ".as_ptr());
        for i in 0..PRIORITY_TEST_COUNT {
            printf(c"proc[%d] ".as_ptr(), EXPECTED_ORDER[i]);
        }
        printf(c"\n".as_ptr());

        printf(c"    Actual:   ".as_ptr());
        let mut correct = true;
        for i in 0..PRIORITY_TEST_COUNT {
            let mut proc_at_pos: c_int = -1;
            for j in 0..PRIORITY_TEST_COUNT {
                if ACTIVATION_ORDER[j].load(Ordering::Acquire) == i as c_int {
                    proc_at_pos = j as c_int;
                    break;
                }
            }
            printf(c"proc[%d] ".as_ptr(), proc_at_pos);
            if proc_at_pos != EXPECTED_ORDER[i] {
                correct = false;
            }
        }
        printf(c"\n".as_ptr());

        assert_msg(correct, c"rq_test: Priority ordering failed".as_ptr());
        printf(c"    Processes activated in correct priority order!\n".as_ptr());
        printf(c"  PASSED\n".as_ptr());
    }
}

// --- Test 7: affinity change ----------------------------------------------
fn test_affinity_change() {
    rq_test_raw! {
        printf(c"TEST: CPU Affinity Change\n".as_ptr());
        let se = current_sched_entity();

        let mut attr: sched_attr = zeroed();
        xv6_rqport_sched_getattr(se, &mut attr);
        let original_mask = attr.affinity_mask;

        printf(c"  Original affinity mask: 0x%lx\n".as_ptr(), original_mask);

        let cur_cpu = cpuid();
        attr.affinity_mask = 1u64 << cur_cpu;
        let ret = xv6_rqport_sched_setattr(se, &attr);
        assert_msg(ret == 0, c"rq_test: setattr for affinity failed".as_ptr());

        xv6_rqport_sched_getattr(se, &mut attr);
        assert_msg(attr.affinity_mask == 1u64 << cur_cpu, c"rq_test: affinity not changed correctly".as_ptr());

        printf(c"  Pinned to CPU %d, mask: 0x%lx\n".as_ptr(), cur_cpu, attr.affinity_mask);
        xv6_schport_scheduler_yield();

        let new_cpu = cpuid();
        assert_msg(new_cpu == cur_cpu, c"rq_test: CPU changed despite affinity pin".as_ptr());

        attr.affinity_mask = original_mask;
        xv6_rqport_sched_setattr(se, &attr);
        printf(c"  Restored original affinity\n".as_ptr());
        printf(c"  PASSED\n".as_ptr());
    }
}

// --- main entry point -----------------------------------------------------
#[no_mangle]
pub extern "C" fn rq_test_run() {
    rq_test_raw! {
        printf(c"\n========================================\n".as_ptr());
        printf(c"Run Queue Priority Integration Tests\n".as_ptr());
        printf(c"Running on CPU %ld\n".as_ptr(), cpuid() as i64);
        printf(c"========================================\n\n".as_ptr());

        test_two_layer_mask();
        test_priority_change();
        test_yield_priority();
        test_rq_selection();
        test_priority_ordering();
        test_priority_ordered_activation();
        test_affinity_change();

        printf(c"\n========================================\n".as_ptr());
        printf(c"All Integration Tests PASSED!\n".as_ptr());
        printf(c"========================================\n\n".as_ptr());
    }
}

#[no_mangle]
pub extern "C" fn xv6_rqtest_pub_run() {
    rq_test_run()
}
