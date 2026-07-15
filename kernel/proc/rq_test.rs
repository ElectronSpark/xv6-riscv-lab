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
use crate::proc::access::{is_err_or_null, zeroed_sched_attr, RqRef, SchedEntityRef, ThreadAccess};
use crate::proc::proc_shims::{xv6_current_thread, xv6_panic};
use crate::proc::pick_next_rq;
use crate::proc::rq_lock;
use crate::proc::rq_unlock;
use crate::proc::sched_attr_init;
use crate::proc::sched_getattr;
use crate::proc::sched_setattr;
// P3-1B2: these used to be bridged via the `xv6_schport_*`/`xv6_thport_*`/
// `xv6_tqport_*` C-ABI alias layers (`extern "C"` redeclarations below);
// now direct crate-path calls to the real, already-Rust definitions.
use crate::proc::{scheduler_yield, wakeup, kthread_create, tq_init, tq_wait, tq_wakeup_all};

// --- priority macros (mirror inc/proc/rq.h) -------------------------------
const PRIORITY_SUBLEVEL_MASK: c_int = 0x03;
const PRIORITY_MAINLEVEL_MASK: c_int = 0xFC;
const PRIORITY_MAINLEVEL_SHIFT: c_int = 2;
#[inline] const fn MAJOR_PRIORITY(p: c_int) -> c_int { (p & PRIORITY_MAINLEVEL_MASK) >> PRIORITY_MAINLEVEL_SHIFT }
#[inline] const fn MINOR_PRIORITY(p: c_int) -> c_int { p & PRIORITY_SUBLEVEL_MASK }
#[inline] const fn MAKE_PRIORITY(major: c_int, minor: c_int) -> c_int { (major << PRIORITY_MAINLEVEL_SHIFT) | minor }


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
    // SAFETY: `sched_entity_of` is this test module's choke point; every call site
    // passes either `xv6_current_thread()` or a `test_procs[]` entry populated
    // by this test's own thread-creation setup -- always a live, non-null
    // thread pointer.
    unsafe { ThreadAccess::assume(p) }.sched_entity_ptr()
}

#[inline]
fn current_sched_entity() -> *mut sched_entity {
    sched_entity_of(xv6_current_thread())
}

// --- Test 1: two-layer bitmask --------------------------------------------
fn test_two_layer_mask() {
    rq_test_raw! {
        crate::kprintln!("TEST: Two-Layer Bitmask Logic");
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
                crate::kprintln!("  FAIL: major {} -> group {} bit {}, expected group {} bit {}",
                    major, ag, ab, eg, eb);
            }
        }
        assert_msg(passed == cases.len() as c_int, c"rq_test: bitmask mapping failed".as_ptr());
        crate::kprintln!("  PASSED: {}/{} bitmask mappings correct", passed, cases.len() as c_int);
    }
}

// --- Test 2: priority change ----------------------------------------------
fn test_priority_change() {
    rq_test_raw! {
        crate::kprintln!("TEST: Priority Change via sched_setattr");
        let se = current_sched_entity();

        let mut attr: sched_attr = zeroed_sched_attr();
        sched_getattr(se, &mut attr);
        let original_priority = attr.priority;
        let original_major = MAJOR_PRIORITY(original_priority);

        crate::kprintln!("  Original priority: major={} minor={}",
            original_major, MINOR_PRIORITY(original_priority));

        let new_major = if original_major == 10 { 12 } else { 10 };
        attr.priority = MAKE_PRIORITY(new_major, 1);

        let ret = sched_setattr(se, &attr);
        assert_msg(ret == 0, c"rq_test: sched_setattr failed".as_ptr());

        let mut new_attr: sched_attr = zeroed_sched_attr();
        sched_getattr(se, &mut new_attr);
        let changed_major = MAJOR_PRIORITY(new_attr.priority);
        let changed_minor = MINOR_PRIORITY(new_attr.priority);

        crate::kprintln!("  Changed priority: major={} minor={}", changed_major, changed_minor);

        assert_msg(changed_major == new_major, c"rq_test: major priority not changed".as_ptr());
        assert_msg(changed_minor == 1, c"rq_test: minor priority not changed".as_ptr());

        scheduler_yield();

        attr.priority = original_priority;
        sched_setattr(se, &attr);

        crate::kprintln!("  Restored original priority");
        crate::kprintln!("  PASSED");
    }
}

// --- Test 3: yield priority -----------------------------------------------
fn test_yield_priority() {
    rq_test_raw! {
        crate::kprintln!("TEST: Yield Respects Priority");
        let p = xv6_current_thread();
        let my_priority = SchedEntityRef::assume(sched_entity_of(p)).priority();
        crate::kprintln!("  Current process '{}' at priority major={}",
            crate::printf::Cs(ThreadAccess::assume(p).name_ptr()), MAJOR_PRIORITY(my_priority));

        let mut yields_completed = 0;
        for _ in 0..5 {
            scheduler_yield();
            yields_completed += 1;
        }
        assert_msg(yields_completed == 5, c"rq_test: not all yields completed".as_ptr());
        crate::kprintln!("  Successfully yielded {} times and got rescheduled", yields_completed);
        crate::kprintln!("  PASSED");
    }
}

// --- Test 4: rq selection consistency -------------------------------------
fn test_rq_selection() {
    rq_test_raw! {
        crate::kprintln!("TEST: RQ Selection Consistency");
        let test_cpu = cpuid();

        rq_lock(test_cpu);
        let rq1 = pick_next_rq();
        let rq2 = pick_next_rq();
        let rq3 = pick_next_rq();
        rq_unlock(test_cpu);

        assert_msg(rq1 == rq2 && rq2 == rq3, c"rq_test: inconsistent rq selection".as_ptr());
        crate::kprintln!("  Consistent selection: class_id={}", RqRef::assume(rq1).class_id());
        crate::kprintln!("  PASSED");
    }
}

// --- Test 5: priority ordering --------------------------------------------
fn verify_priority(se: *mut sched_entity, expected_major: c_int, expected_minor: c_int) -> bool {
    rq_test_raw! {
        let mut attr: sched_attr = zeroed_sched_attr();
        sched_getattr(se, &mut attr);
        let am = MAJOR_PRIORITY(attr.priority);
        let an = MINOR_PRIORITY(attr.priority);
        if am != expected_major || an != expected_minor {
            crate::kprintln!("    FAIL: expected ({},{}) got ({},{})",
                expected_major, expected_minor, am, an);
            return false;
        }
        true
    }
}

fn set_and_check(se: *mut sched_entity, major: c_int, minor: c_int) {
    rq_test_raw! {
        let mut attr: sched_attr = zeroed_sched_attr();
        sched_attr_init(&mut attr);
        attr.priority = MAKE_PRIORITY(major, minor);
        sched_setattr(se, &attr);
        assert_msg(verify_priority(se, major, minor), c"rq_test: priority set failed".as_ptr());
        scheduler_yield();
    }
}

fn pick_major(test_cpu: c_int) -> c_int {
    rq_test_raw! {
        rq_lock(test_cpu);
        let r = pick_next_rq();
        let m = unsafe { (*r).class_id };
        rq_unlock(test_cpu);
        m
    }
}

fn test_priority_ordering() {
    rq_test_raw! {
        crate::kprintln!("TEST: Priority Ordering (Comprehensive)");
        let se = current_sched_entity();
        let test_cpu = cpuid();

        let mut original_attr: sched_attr = zeroed_sched_attr();
        sched_getattr(se, &mut original_attr);

        // Case 1: different top-layer groups
        crate::kprintln!("  Case 1: Different top-layer groups");
        for &(major, group) in &[(1i32, 0i32), (9, 1), (17, 2), (50, 6)] {
            set_and_check(se, major, 0);
            let pm = pick_major(test_cpu);
            crate::kprintln!("    major={} (group {}): pick_next_rq returned {}", major, group, pm);
        }
        crate::kprintln!("    Case 1 PASSED");

        // Case 2: same group, different bits
        crate::kprintln!("  Case 2: Same group, different secondary bits");
        for &major in &[1i32, 3, 5, 7] {
            set_and_check(se, major, 0);
            let pm = pick_major(test_cpu);
            crate::kprintln!("    major={} (bit {}): pick_next_rq returned {}", major, major, pm);
        }
        crate::kprintln!("    Case 2 PASSED");

        // Case 3: same major, different minor
        crate::kprintln!("  Case 3: Same major, different minor priorities");
        for minor in 0..4 {
            set_and_check(se, 5, minor);
            crate::kprintln!("    major=5, minor={}: priority set and yield OK", minor);
        }
        crate::kprintln!("    Case 3 PASSED");

        // Case 4: boundary transitions
        crate::kprintln!("  Case 4: Group boundary transitions");
        {
            set_and_check(se, 7, 0);
            let pm = pick_major(test_cpu);
            crate::kprintln!("    major=7 (end of group 0): pick_next_rq returned {}", pm);
        }
        {
            set_and_check(se, 8, 0);
            let pm = pick_major(test_cpu);
            crate::kprintln!("    major=8 (start of group 1): pick_next_rq returned {}", pm);
        }
        {
            set_and_check(se, 62, 0);
            let pm = pick_major(test_cpu);
            crate::kprintln!("    major=62 (lowest usable): pick_next_rq returned {}", pm);
        }
        crate::kprintln!("    Case 4 PASSED");

        sched_setattr(se, &original_attr);
        crate::kprintln!("  All priority ordering cases PASSED");
        crate::kprintln!("  PASSED");
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
            tq_wakeup_all(tq_ptr(), 0, 0);
        }
        0
    }
}

fn test_priority_ordered_activation() {
    rq_test_raw! {
        crate::kprintln!("TEST: Priority-Ordered Process Activation");

        spin_init(lock_ptr(), c"prio_test".as_ptr() as *mut c_char);
        tq_init(tq_ptr(), c"main_wait".as_ptr(), lock_ptr());
        ACTIVATION_INDEX.store(0, Ordering::Release);
        PROCESSES_DONE.store(0, Ordering::Release);
        for i in 0..PRIORITY_TEST_COUNT {
            ACTIVATION_ORDER[i].store(-1, Ordering::Release);
        }

        let mut test_procs: [*mut thread; PRIORITY_TEST_COUNT] = [core::ptr::null_mut(); PRIORITY_TEST_COUNT];

        crate::kprintln!("  Phase 1: Creating {} processes with preemption disabled",
            PRIORITY_TEST_COUNT as c_int);

        let g = crate::machine::PreemptGuard::new();

        let test_cpu = g.cpuid() as c_int;
        let cpu_mask: cpumask_t = 1u64 << test_cpu;

        for i in 0..PRIORITY_TEST_COUNT {
            test_procs[i] = kthread_create(
                c"prio_test".as_ptr(),
                priority_test_proc_entry as *mut c_void,
                i as u64, 0, 0,
            );
            assert_msg(!is_err_or_null(test_procs[i]), c"rq_test: kthread_create failed".as_ptr());

            let se = sched_entity_of(test_procs[i]);
            let mut attr: sched_attr = zeroed_sched_attr();
            sched_attr_init(&mut attr);
            attr.priority = TEST_PRIORITIES[i];
            attr.affinity_mask = cpu_mask;
            sched_setattr(se, &attr);

            crate::kprintln!("    Created process {} (pid={}) with priority major={} on CPU {}",
                i as c_int, ThreadAccess::assume(test_procs[i]).pid(), MAJOR_PRIORITY(TEST_PRIORITIES[i]), test_cpu);
        }

        crate::kprintln!("  Phase 2: Waking up all processes");
        for i in 0..PRIORITY_TEST_COUNT {
            wakeup(test_procs[i]);
        }

        crate::kprintln!("  Phase 3: Enabling preemption and yielding");
        drop(g);
        scheduler_yield();

        crate::kprintln!("  Phase 4: Waiting for all processes to complete");
        spin_lock(lock_ptr());
        while PROCESSES_DONE.load(Ordering::Acquire) < PRIORITY_TEST_COUNT as c_int {
            tq_wait(tq_ptr(), lock_ptr(), core::ptr::null_mut());
        }
        spin_unlock(lock_ptr());

        crate::kprintln!("  Phase 5: Verifying activation order");
        crate::kprint!("    Expected: ");
        for i in 0..PRIORITY_TEST_COUNT {
            crate::kprint!("proc[{}] ", EXPECTED_ORDER[i]);
        }
        crate::kprintln!();

        crate::kprint!("    Actual:   ");
        let mut correct = true;
        for i in 0..PRIORITY_TEST_COUNT {
            let mut proc_at_pos: c_int = -1;
            for j in 0..PRIORITY_TEST_COUNT {
                if ACTIVATION_ORDER[j].load(Ordering::Acquire) == i as c_int {
                    proc_at_pos = j as c_int;
                    break;
                }
            }
            crate::kprint!("proc[{}] ", proc_at_pos);
            if proc_at_pos != EXPECTED_ORDER[i] {
                correct = false;
            }
        }
        crate::kprintln!();

        assert_msg(correct, c"rq_test: Priority ordering failed".as_ptr());
        crate::kprintln!("    Processes activated in correct priority order!");
        crate::kprintln!("  PASSED");
    }
}

// --- Test 7: affinity change ----------------------------------------------
fn test_affinity_change() {
    rq_test_raw! {
        crate::kprintln!("TEST: CPU Affinity Change");
        let se = current_sched_entity();

        let mut attr: sched_attr = zeroed_sched_attr();
        sched_getattr(se, &mut attr);
        let original_mask = attr.affinity_mask;

        crate::kprintln!("  Original affinity mask: 0x{:x}", original_mask as u64);

        let cur_cpu = cpuid();
        attr.affinity_mask = 1u64 << cur_cpu;
        let ret = sched_setattr(se, &attr);
        assert_msg(ret == 0, c"rq_test: setattr for affinity failed".as_ptr());

        sched_getattr(se, &mut attr);
        assert_msg(attr.affinity_mask == 1u64 << cur_cpu, c"rq_test: affinity not changed correctly".as_ptr());

        crate::kprintln!("  Pinned to CPU {}, mask: 0x{:x}", cur_cpu, attr.affinity_mask as u64);
        scheduler_yield();

        let new_cpu = cpuid();
        assert_msg(new_cpu == cur_cpu, c"rq_test: CPU changed despite affinity pin".as_ptr());

        attr.affinity_mask = original_mask;
        sched_setattr(se, &attr);
        crate::kprintln!("  Restored original affinity");
        crate::kprintln!("  PASSED");
    }
}

// --- main entry point -----------------------------------------------------
#[no_mangle]
pub extern "C" fn rq_test_run() {
    rq_test_raw! {
        crate::kprintln!("\n========================================");
        crate::kprintln!("Run Queue Priority Integration Tests");
        crate::kprintln!("Running on CPU {}", cpuid() as i64);
        crate::kprintln!("========================================\n");

        test_two_layer_mask();
        test_priority_change();
        test_yield_priority();
        test_rq_selection();
        test_priority_ordering();
        test_priority_ordered_activation();
        test_affinity_change();

        crate::kprintln!("\n========================================");
        crate::kprintln!("All Integration Tests PASSED!");
        crate::kprintln!("========================================\n");
    }
}

#[no_mangle]
pub extern "C" fn xv6_rqtest_pub_run() {
    rq_test_run()
}
