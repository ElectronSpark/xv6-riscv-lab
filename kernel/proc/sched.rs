//! Pure-Rust port of `kernel/proc/sched.c` (SECTION 18 of the former
//! `proc_rust_shims.c`).  Owns the canonical public ABI symbols
//! (`sleep_lock`, `scheduler_yield`, `context_switch_finish`, ...) and
//! provides the `xv6_schport_*` aliases used by sibling Rust/C ports.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::{offset_of, MaybeUninit};

use crate::bindings::{
    context, rb_node, rb_root, rq, sched_entity, spinlock_t, thread,
    thread_state, tnode_t, ttree_t,
};
use crate::machine::{
    cpu_local_ptr, cpu_relax, cpuid as cpuid_rs, intr_get, intr_off_save, intr_restore, pop_off,
    push_off,
};
use crate::lock::rcu::rcu_check_callbacks;
use crate::lock::spinlock::spin_holding;
use crate::lock::spinlock::spin_init;
use crate::lock::spinlock::spin_lock;
use crate::lock::spinlock::spin_lock_irqsave;
use crate::lock::spinlock::spin_unlock;
use crate::lock::spinlock::spin_unlock_irqrestore;
use crate::proc::access::{
    is_err_or_null, smp_load_acquire_i32, smp_load_acquire_state, smp_rmb,
    smp_store_release_state, CpuLocalRef, RqRef, SchedEntityRef, SpinLockRef, ThreadAccess,
};
use crate::proc::tnode_get_thread;
use crate::proc::ttree_init;
use crate::proc::ttree_wait;
use crate::proc::ttree_wakeup_key;
use crate::proc::xv6_rqport_pick_next_rq;
use crate::proc::xv6_rqport_rq_global_init;
use crate::proc::xv6_rqport_rq_holding_current;
use crate::proc::xv6_rqport_rq_lock_current_irqsave;
use crate::proc::xv6_rqport_rq_put_prev_task;
use crate::proc::xv6_rqport_rq_set_next_task;
use crate::proc::xv6_rqport_rq_task_dead;
use crate::proc::xv6_rqport_rq_trylock_two;
use crate::proc::xv6_rqport_rq_unlock_two;

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

// ---------------- extern C primitives -----------------------------------
unsafe extern "C" {
    pub safe fn xv6_panic(msg: *const c_char) -> !;
    fn printf(fmt: *const c_char, ...) -> c_int;

    safe fn get_jiffs() -> u64;
    safe fn ipi_send_single(hartid: c_int, reason: c_int) -> c_int;

    fn __swtch_context(cur: *mut context, target: *mut context) -> *mut context;
    safe fn __do_timer_tick();

    fn rb_first_node(root: *mut rb_root) -> *mut rb_node;
    fn rb_next_node(node: *mut rb_node) -> *mut rb_node;

    safe fn page_free(ptr: *mut c_void, order: c_int);

    // RQ port functions (provided by SECTION 19 in proc_rust_shims.c)
    safe fn xv6_rqport_rq_pick_next_task(rq: *mut rq) -> *mut sched_entity;
    safe fn xv6_rqport_rq_select_task_rq(se: *mut sched_entity, mask: u64) -> *mut rq;
    safe fn xv6_rqport_rq_unlock_current_irqrestore(intr: c_int);
    safe fn xv6_rqport_rq_add_wake_list(cpuid: c_int, se: *mut sched_entity);
    safe fn xv6_rqport_rq_flush_wake_list(cpuid: c_int);
    safe fn xv6_rqport_rq_dequeue_task(rq: *mut rq, se: *mut sched_entity);
    safe fn xv6_rqport_rq_enqueue_task(rq: *mut rq, se: *mut sched_entity);

    #[link_name = "xv6_rqport_pick_next_rq"]
    safe fn rqport_pick_next_rq() -> *mut rq;
    #[link_name = "xv6_rqport_rq_global_init"]
    safe fn rqport_rq_global_init();
    #[link_name = "xv6_rqport_rq_holding_current"]
    safe fn rqport_rq_holding_current() -> c_int;
    #[link_name = "xv6_rqport_rq_lock_current_irqsave"]
    safe fn rqport_rq_lock_current_irqsave() -> c_int;
    #[link_name = "xv6_rqport_rq_set_next_task"]
    safe fn rqport_rq_set_next_task(se: *mut sched_entity);
    #[link_name = "xv6_rqport_rq_task_dead"]
    safe fn rqport_rq_task_dead(se: *mut sched_entity);
    #[link_name = "xv6_rqport_rq_put_prev_task"]
    safe fn rqport_rq_put_prev_task(se: *mut sched_entity);
    #[link_name = "xv6_rqport_rq_unlock_current_irqrestore"]
    safe fn rqport_rq_unlock_current_irqrestore(intr: c_int);
}

// ---------------- macros / helpers --------------------------------------
macro_rules! kpanic { ($m:expr) => {{ xv6_panic(concat!($m, "\0").as_ptr() as *const c_char) }}; }
macro_rules! kassert { ($c:expr, $m:expr) => {{ if !($c) { kpanic!($m); } }}; }
macro_rules! schport_unsafe_call { ($e:expr) => {{ unsafe { $e } }}; }

#[inline] fn cpu_access<'a>() -> CpuLocalRef<'a> {
    CpuLocalRef::assume(cpu_local_ptr())
}
#[inline] fn current_thread() -> *mut thread { cpu_access().proc_ptr() }

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
    cpu_access().flag_set(CPU_FLAG_IN_ITR)
}

#[inline]
fn thread_state_get(p: *mut thread) -> thread_state {
    ta_of(p).map_or(THREAD_UNUSED, |t| t.state_load() as thread_state)
}
#[inline]
fn thread_state_set(p: *mut thread, s: thread_state) {
    if let Some(t) = ta_of(p) { t.state_store(s as u32); }
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

#[inline] fn thread_sleeping(p: *mut thread) -> bool { state_is_sleeping(thread_state_get(p)) }
#[inline] fn thread_running(p: *mut thread) -> bool { state_is_running(thread_state_get(p)) }
#[inline] fn thread_zombie(p: *mut thread) -> bool { state_is_zombie(thread_state_get(p)) }
#[inline] fn thread_stopped(p: *mut thread) -> bool { state_is_stopped(thread_state_get(p)) }
#[inline] fn thread_killable(p: *mut thread) -> bool { state_is_killable(thread_state_get(p)) }
#[inline] fn thread_timer(p: *mut thread) -> bool { state_is_timer(thread_state_get(p)) }
#[inline] fn thread_interruptible(p: *mut thread) -> bool { state_is_interruptible(thread_state_get(p)) }

#[inline]
fn thread_set_flag(p: *mut thread, bit: u64) {
    if let Some(t) = ta_of(p) { t.flags_set_bit(bit); }
}
#[inline]
fn thread_clear_flag(p: *mut thread, bit: u64) {
    if let Some(t) = ta_of(p) { t.flags_clear_bit(bit); }
}
#[inline]
fn thread_test_flag(p: *mut thread, bit: u64) -> bool {
    ta_of(p).map_or(false, |t| t.flags_test_bit(bit))
}

#[inline]
fn thread_from_context(ctx: *mut context) -> *mut thread {
    let off = offset_of!(sched_entity, context);
    let se = (ctx as *mut u8).wrapping_sub(off) as *mut sched_entity;
    SchedEntityRef::assume(se).thread_ptr()
}

// ---------------- static lock + chan queue ------------------------------
#[repr(transparent)]
struct SyncBuf<T>(UnsafeCell<MaybeUninit<T>>);
unsafe impl<T> Sync for SyncBuf<T> {}

static SLEEP_LOCK: SyncBuf<spinlock_t> = SyncBuf(UnsafeCell::new(MaybeUninit::uninit()));
static CHAN_QUEUE_ROOT: SyncBuf<ttree_t> = SyncBuf(UnsafeCell::new(MaybeUninit::uninit()));

#[inline] fn sleep_lock_ptr() -> *mut spinlock_t {
    SLEEP_LOCK.0.get() as *mut spinlock_t
}
#[inline] fn chan_queue_ptr() -> *mut ttree_t {
    CHAN_QUEUE_ROOT.0.get() as *mut ttree_t
}

#[inline]
fn sleep_lock_ref<'a>() -> SpinLockRef<'a> {
    SpinLockRef::assume(sleep_lock_ptr())
}

fn chan_queue_init() {
    ttree_init(chan_queue_ptr(), c"chan_queue_root".as_ptr(), sleep_lock_ptr());
}

// ---------------- sleep lock helpers ------------------------------------
fn chan_holding_impl() -> c_int { sleep_lock_ref().holding() as c_int }
fn sleep_lock_impl() { sleep_lock_ref().lock(); }
fn sleep_unlock_impl() { sleep_lock_ref().unlock(); }
fn sleep_lock_irqsave_impl() -> c_int { sleep_lock_ref().lock_irqsave() }
fn sleep_unlock_irqrestore_impl(state: c_int) { sleep_lock_ref().unlock_irqrestore(state); }
fn sched_holding_impl() -> c_int { rqport_rq_holding_current() }

#[no_mangle]
pub extern "C" fn chan_holding() -> c_int { chan_holding_impl() }

#[no_mangle]
pub extern "C" fn sleep_lock() { sleep_lock_impl() }

#[no_mangle]
pub extern "C" fn sleep_unlock() { sleep_unlock_impl() }

#[no_mangle]
pub extern "C" fn sleep_lock_irqsave() -> c_int { sleep_lock_irqsave_impl() }

#[no_mangle]
pub extern "C" fn sleep_unlock_irqrestore(state: c_int) { sleep_unlock_irqrestore_impl(state) }

#[no_mangle]
pub extern "C" fn sched_holding() -> c_int { sched_holding_impl() }

#[inline]
fn sched_assert_holding() {
    kassert!(sched_holding_impl() != 0, "rq_lock must be held");
}
#[inline]
fn sched_assert_unholding() {
    kassert!(sched_holding_impl() == 0, "rq_lock must not be held");
}

// ---------------- scheduler init ----------------------------------------
fn scheduler_init_impl() {
    unsafe {
        spin_init(sleep_lock_ptr(), c"xv6_schport_sleep_lock".as_ptr() as *mut c_char);
        chan_queue_init();
        rqport_rq_global_init();
    }
}

#[no_mangle]
pub extern "C" fn scheduler_init() { scheduler_init_impl() }

// ---------------- pick next ---------------------------------------------
fn sched_pick_next() -> *mut thread {
    sched_assert_holding();
    let rq = rqport_pick_next_rq();
    if is_err_or_null(rq) { return core::ptr::null_mut(); }
    let se = xv6_rqport_rq_pick_next_task(rq);
    if se.is_null() { return core::ptr::null_mut(); }
    let cur = current_thread();
    let cur_se = ThreadAccess::assume(cur).sched_entity_ptr();
    let this_priority = SchedEntityRef::assume(cur_se).priority();
    let next = SchedEntityRef::assume(se);
    let next_priority = next.priority();
    if thread_running(cur) && this_priority < next_priority {
        return cur;
    }
    let p = next.thread_ptr();
    kassert!(!p.is_null(), "sched_pick_next: se->thread is NULL");
    rqport_rq_set_next_task(se);
    next.on_cpu_store_release(1);
    let pstate = thread_state_get(p);
    if pstate == THREAD_WAKENING {
        thread_state_set(p, THREAD_RUNNING);
    } else if pstate != THREAD_RUNNING {
        kassert!(pstate != THREAD_INTERRUPTIBLE, "try to schedule an interruptible thread");
        kassert!(pstate != THREAD_UNINTERRUPTIBLE, "try to schedule an uninterruptible thread");
        kassert!(pstate != THREAD_UNUSED, "try to schedule an uninitialized thread");
        kassert!(pstate != THREAD_ZOMBIE, "found a zombie thread in ready queue");
        kpanic!("try to schedule unknown thread state");
    }
    p
}

// ---------------- switch_to ---------------------------------------------
fn switch_to_impl(cur: *mut thread, target: *mut thread) -> *mut thread {
    unsafe {
        let now = get_jiffs();
        cpu_access().store_rcu_timestamp_release(now);
        cpu_access().set_proc(target);
        let cur_se = ThreadAccess::from_raw(cur).unwrap_unchecked().sched_entity_ptr();
        let target_se = ThreadAccess::from_raw(target).unwrap_unchecked().sched_entity_ptr();
        let prev_ctx = __swtch_context(
            SchedEntityRef::from_raw(cur_se).unwrap_unchecked().context_ptr(),
            SchedEntityRef::from_raw(target_se).unwrap_unchecked().context_ptr(),
        );
        thread_from_context(prev_ctx)
    }
}

#[no_mangle]
pub extern "C" fn switch_to(cur: *mut thread, target: *mut thread) -> *mut thread {
    switch_to_impl(cur, target)
}

fn switch_to_internal(p: *mut thread) -> *mut thread {
    sched_assert_holding();
    kassert!(!intr_get(), "Interrupts must be disabled before switching to a thread");
    let proc = current_thread();
    let intena = cpu_access().intena();
    let mut spin_depth_expected = 1;
    if chan_holding_impl() != 0 { spin_depth_expected += 1; }
    kassert!(cpu_access().noff() == 0, "Thread must not hold any other locks when yielding");
    kassert!(cpu_access().spin_depth() == spin_depth_expected,
        "Thread must hold and only hold the rq lock when yielding");

    let prev = switch_to_impl(proc, p);

    kassert!(!intr_get(), "Interrupts must be disabled after switching");
    kassert!(current_thread() == proc, "Yield returned to a different thread");
    kassert!(thread_running(proc), "Thread state must be RUNNING after yield");
    cpu_access().set_intena(intena);
    prev
}

// ---------------- scheduler_yield ---------------------------------------
fn scheduler_yield_inner() {
    __do_timer_tick();
    xv6_rqport_rq_flush_wake_list(cpuid_rs());

    let intr = rqport_rq_lock_current_irqsave();
    let proc = current_thread();

    kassert!(!cpu_in_itr(), "Cannot yield CPU in interrupt context");

    let mut p = sched_pick_next();

    if p == current_thread() {
        xv6_rqport_rq_unlock_current_irqrestore(intr);
    } else if p.is_null() {
        let idle = cpu_access().idle_thread_ptr();
        if proc == idle {
            xv6_rqport_rq_unlock_current_irqrestore(intr);
        } else {
            p = idle;
            kassert!(!p.is_null(), "Idle process is NULL");
            context_switch_prepare_impl(proc, p);
            let prev = switch_to_internal(p);
            context_switch_finish_impl(prev, current_thread(), intr);
        }
    } else {
        context_switch_prepare_impl(proc, p);
        let prev = switch_to_internal(p);
        context_switch_finish_impl(prev, current_thread(), intr);
    }

    xv6_rqport_rq_flush_wake_list(cpuid_rs());
    unsafe { rcu_check_callbacks(); }
}

#[no_mangle]
pub extern "C" fn scheduler_yield() { scheduler_yield_inner() }

// ---------------- scheduler_sleep ---------------------------------------
fn scheduler_sleep_impl(lk: Option<SpinLockRef<'_>>, sleep_state: thread_state) {
    let intr = intr_off_save();
    let proc = current_thread();
    kassert!(!proc.is_null(), "PCB is NULL");
    kassert!(state_is_sleeping(sleep_state),
        "Thread must be in INTERRUPTIBLE or UNINTERRUPTIBLE state to sleep");
    thread_state_set(proc, sleep_state);
    let lk_holding = lk.is_some_and(|l| l.holding());
    if lk_holding { lk.unwrap().unlock(); }
    scheduler_yield_inner();
    if lk_holding { lk.unwrap().lock(); }
    intr_restore(intr);
}

#[no_mangle]
pub extern "C" fn scheduler_sleep(lk: *mut spinlock_t, sleep_state: thread_state) {
    scheduler_sleep_impl(lk_of(lk), sleep_state)
}

// ---------------- wakeup helpers ----------------------------------------
#[inline]
fn scheduler_wakeup_assert(p: *mut thread) {
    kassert!(!p.is_null(), "Cannot wake up a NULL process");
    if let Some(t) = ta_of(p) {
        kassert!(!t.lock_ref().holding(),
            "Process lock must not be held when waking up a process");
    }
    kassert!(sched_holding_impl() == 0,
        "Scheduler lock must not be held when waking up a process");
}

#[inline]
fn do_scheduler_wakeup_safe(p: *mut thread, from_stopped: bool) {
    unsafe { do_scheduler_wakeup(p, from_stopped); }
}

unsafe fn do_scheduler_wakeup(p: *mut thread, from_stopped: bool) {
    unsafe {
        let se = (*p).sched_entity;
        spin_lock(&raw mut (*se).pi_lock);

        if p == current_thread() {
            smp_rmb();
            if from_stopped {
                spin_unlock(&raw mut (*se).pi_lock);
                return;
            }
            let old_state = smp_load_acquire_state(&(*p).state as *const _);
            if !state_is_sleeping(old_state) {
                spin_unlock(&raw mut (*se).pi_lock);
                return;
            }
            smp_store_release_state(&raw mut (*p).state, THREAD_RUNNING);
            spin_unlock(&raw mut (*se).pi_lock);
            return;
        }

        smp_rmb();
        let mut old_state = smp_load_acquire_state(&(*p).state as *const _);
        if from_stopped {
            if old_state != THREAD_STOPPED {
                spin_unlock(&raw mut (*se).pi_lock);
                return;
            }
        } else if !state_is_sleeping(old_state) {
            spin_unlock(&raw mut (*se).pi_lock);
            return;
        }

        // retry loop
        loop {
            push_off();
            let rq = xv6_rqport_rq_select_task_rq(se, (*se).affinity_mask);
            kassert!(!is_err_or_null(rq), "do_scheduler_wakeup: rq_select_task_rq failed");
            let mut origin_cpuid = smp_load_acquire_i32(&(*se).cpu_id as *const _);
            let target_cpu = (*rq).cpu_id;
            if origin_cpuid < 0 { origin_cpuid = target_cpu; }

            if xv6_rqport_rq_trylock_two(origin_cpuid, target_cpu) == 0 {
                pop_off();
                spin_unlock(&raw mut (*se).pi_lock);
                for _ in 0..10 { cpu_relax(); }
                spin_lock(&raw mut (*se).pi_lock);
                old_state = smp_load_acquire_state(&(*p).state as *const _);
                if from_stopped {
                    if old_state != THREAD_STOPPED {
                        spin_unlock(&raw mut (*se).pi_lock);
                        return;
                    }
                } else if !state_is_sleeping(old_state) {
                    spin_unlock(&raw mut (*se).pi_lock);
                    return;
                }
                continue;
            }
            pop_off();

            let current_cpuid = smp_load_acquire_i32(&(*se).cpu_id as *const _);
            if current_cpuid >= 0 && current_cpuid != origin_cpuid {
                xv6_rqport_rq_unlock_two(origin_cpuid, target_cpu);
                spin_unlock(&raw mut (*se).pi_lock);
                spin_lock(&raw mut (*se).pi_lock);
                old_state = smp_load_acquire_state(&(*p).state as *const _);
                if from_stopped {
                    if old_state != THREAD_STOPPED {
                        spin_unlock(&raw mut (*se).pi_lock);
                        return;
                    }
                } else if !state_is_sleeping(old_state) {
                    spin_unlock(&raw mut (*se).pi_lock);
                    return;
                }
                continue;
            }

            smp_store_release_state(&raw mut (*p).state, THREAD_WAKENING);

            if smp_load_acquire_i32(&(*se).on_rq as *const _) != 0 {
                smp_store_release_state(&raw mut (*p).state, THREAD_RUNNING);
                spin_unlock(&raw mut (*se).pi_lock);
                xv6_rqport_rq_unlock_two(origin_cpuid, target_cpu);
                return;
            }

            if smp_load_acquire_i32(&(*se).on_cpu as *const _) != 0 {
                xv6_rqport_rq_add_wake_list(origin_cpuid, se);
                spin_unlock(&raw mut (*se).pi_lock);
                xv6_rqport_rq_unlock_two(origin_cpuid, target_cpu);
                ipi_send_single(origin_cpuid, IPI_REASON_RESCHEDULE);
                return;
            }

            xv6_rqport_rq_enqueue_task(rq, se);
            spin_unlock(&raw mut (*se).pi_lock);
            xv6_rqport_rq_unlock_two(origin_cpuid, target_cpu);
            return;
        }
    }
}

fn scheduler_wakeup_impl(p: *mut thread) {
    scheduler_wakeup_assert(p);
    if !thread_sleeping(p) { return; }
    do_scheduler_wakeup_safe(p, false);
}
fn scheduler_wakeup_timeout_impl(p: *mut thread) {
    scheduler_wakeup_assert(p);
    if !thread_timer(p) { return; }
    do_scheduler_wakeup_safe(p, false);
}
fn scheduler_wakeup_killable_impl(p: *mut thread) {
    scheduler_wakeup_assert(p);
    if !thread_killable(p) { return; }
    do_scheduler_wakeup_safe(p, false);
}
fn scheduler_wakeup_interruptible_impl(p: *mut thread) {
    scheduler_wakeup_assert(p);
    if !thread_interruptible(p) { return; }
    do_scheduler_wakeup_safe(p, false);
}
fn scheduler_wakeup_stopped_impl(p: *mut thread) {
    scheduler_wakeup_assert(p);
    if !thread_stopped(p) { return; }
    do_scheduler_wakeup_safe(p, true);
}

#[no_mangle]
pub extern "C" fn scheduler_wakeup(p: *mut thread) { scheduler_wakeup_impl(p) }
#[no_mangle]
pub extern "C" fn scheduler_wakeup_timeout(p: *mut thread) { scheduler_wakeup_timeout_impl(p) }
#[no_mangle]
pub extern "C" fn scheduler_wakeup_killable(p: *mut thread) { scheduler_wakeup_killable_impl(p) }
#[no_mangle]
pub extern "C" fn scheduler_wakeup_interruptible(p: *mut thread) { scheduler_wakeup_interruptible_impl(p) }
#[no_mangle]
pub extern "C" fn scheduler_wakeup_stopped(p: *mut thread) { scheduler_wakeup_stopped_impl(p) }

// ---------------- sleep_on_chan / wakeup_on_chan ------------------------
fn sleep_on_chan_common(chan: *mut c_void, lk: Option<SpinLockRef<'_>>, state: thread_state) -> c_int {
    let intr = sleep_lock_irqsave_impl();
    let cur = current_thread();
    kassert!(!cur.is_null(), "PCB is NULL");
    kassert!(!chan.is_null(), "Cannot sleep on a NULL channel");
    ThreadAccess::assume(cur).set_chan(chan);
    thread_set_flag(cur, THREAD_FLAG_ONCHAN);
    thread_state_set(cur, state);

    let lk_holding = lk.is_some_and(|l| l.holding());
    if lk_holding { lk.unwrap().unlock(); }

    let ret = ttree_wait(chan_queue_ptr(), chan as u64, core::ptr::null_mut(), core::ptr::null_mut());

    sleep_lock_irqsave_impl();
    thread_clear_flag(cur, THREAD_FLAG_ONCHAN);
    ThreadAccess::assume(cur).set_chan(core::ptr::null_mut());
    sleep_unlock_irqrestore_impl(intr);

    if lk_holding { lk.unwrap().lock(); }
    ret
}

fn sleep_on_chan_impl(chan: *mut c_void, lk: Option<SpinLockRef<'_>>) {
    let _ = sleep_on_chan_common(chan, lk, THREAD_UNINTERRUPTIBLE);
}

fn sleep_on_chan_interruptible_impl(chan: *mut c_void, lk: Option<SpinLockRef<'_>>) -> c_int {
    let ret = sleep_on_chan_common(chan, lk, THREAD_INTERRUPTIBLE);
    if ret != 0 { -EINTR } else { 0 }
}

fn wakeup_on_chan_impl(chan: *mut c_void) {
    sleep_lock_impl();
    let _ = ttree_wakeup_key(chan_queue_ptr(), chan as u64, 0, 0);
    sleep_unlock_impl();
}

#[no_mangle]
pub extern "C" fn sleep_on_chan(chan: *mut c_void, lk: *mut spinlock_t) {
    sleep_on_chan_impl(chan, lk_of(lk))
}

#[no_mangle]
pub extern "C" fn sleep_on_chan_interruptible(chan: *mut c_void, lk: *mut spinlock_t) -> c_int {
    sleep_on_chan_interruptible_impl(chan, lk_of(lk))
}

#[no_mangle]
pub extern "C" fn wakeup_on_chan(chan: *mut c_void) { wakeup_on_chan_impl(chan) }

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

fn state_str(s: thread_state) -> *const c_char {
    for (k, v) in STATE_STRS.iter() {
        if *k == s { return v.as_ptr() as *const c_char; }
    }
    c"UNKNOWN".as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn scheduler_dump_chan_queue() {
    unsafe {
        printf(c"Channel Queue Dump:\n".as_ptr() as *const c_char);
        let root = &raw mut (*chan_queue_ptr()).root;
        let tree_entry_off = offset_of!(tnode, __bindgen_anon_1) + offset_of!(crate::bindings::tnode__bindgen_ty_1__bindgen_ty_2, entry);
        let mut node = rb_first_node(root);
        while !node.is_null() {
            let next = rb_next_node(node);
            let tn = (node as *mut u8).wrapping_sub(tree_entry_off) as *mut tnode_t;
            let proc = tnode_get_thread(tn);
            if proc.is_null() {
                printf(c"  Process: NULL\n".as_ptr() as *const c_char);
            } else {
                let t = ThreadAccess::from_raw(proc).unwrap_unchecked();
                printf(
                    c"Chan: %p,  Proc: %s (PID: %d, State: %s)\n".as_ptr() as *const c_char,
                    (*proc).chan,
                    (*proc).name.as_ptr(),
                    t.pid(),
                    state_str(thread_state_get(proc)),
                );
            }
            node = next;
        }
    }
}

use crate::bindings::tnode;

// ---------------- wakeup wrappers ---------------------------------------
fn wakeup_impl(p: *mut thread) {
    if p.is_null() { return; }
    scheduler_wakeup_impl(p);
}
fn wakeup_timeout_impl(p: *mut thread) {
    if p.is_null() { return; }
    scheduler_wakeup_timeout_impl(p);
}
fn wakeup_killable_impl(p: *mut thread) {
    if p.is_null() { return; }
    scheduler_wakeup_killable_impl(p);
}
fn wakeup_interruptible_impl(p: *mut thread) {
    if p.is_null() { return; }
    scheduler_wakeup_interruptible_impl(p);
}

#[no_mangle]
pub extern "C" fn wakeup(p: *mut thread) { wakeup_impl(p) }
#[no_mangle]
pub extern "C" fn wakeup_timeout(p: *mut thread) { wakeup_timeout_impl(p) }
#[no_mangle]
pub extern "C" fn wakeup_killable(p: *mut thread) { wakeup_killable_impl(p) }
#[no_mangle]
pub extern "C" fn wakeup_interruptible(p: *mut thread) { wakeup_interruptible_impl(p) }

fn sys_dumpchan_impl() -> u64 {
    sleep_lock_impl();
    unsafe { scheduler_dump_chan_queue(); }
    sleep_unlock_impl();
    0
}

#[no_mangle]
pub extern "C" fn sys_dumpchan() -> u64 { sys_dumpchan_impl() }

// ---------------- context switch ----------------------------------------
fn context_switch_prepare_impl(prev: *mut thread, next: *mut thread) {
    kassert!(!prev.is_null(), "Previous process is NULL");
    kassert!(!next.is_null(), "Next process is NULL");
    sched_assert_holding();
    let next_se = ThreadAccess::assume(next).sched_entity_ptr();
    let next_se_ref = SchedEntityRef::assume(next_se);
    next_se_ref.on_cpu_store_release(1);
    next_se_ref.set_cpu_id(cpuid_rs());
    if thread_zombie(prev) {
        rqport_rq_task_dead(ThreadAccess::assume(prev).sched_entity_ptr());
    }
}

#[no_mangle]
pub extern "C" fn context_switch_prepare(prev: *mut thread, next: *mut thread) {
    context_switch_prepare_impl(prev, next)
}

fn context_switch_finish_impl(prev: *mut thread, next: *mut thread, intr: c_int) {
    kassert!(!prev.is_null(), "Previous process is NULL");
    kassert!(!next.is_null(), "Next process is NULL");
    let pstate = thread_state_get(prev);
    let prev_access = ThreadAccess::assume(prev);
    let se = prev_access.sched_entity_ptr();
    let idle = cpu_access().idle_thread_ptr();

    if prev != idle {
        if state_is_running(pstate) {
            rqport_rq_put_prev_task(se);
        } else if state_is_sleeping(pstate) || state_is_stopped(pstate) {
            let se_rq = SchedEntityRef::assume(se).rq_ptr();
            if !se_rq.is_null() {
                xv6_rqport_rq_dequeue_task(se_rq, se);
            }
        }
    }

    if thread_test_flag(prev, THREAD_FLAG_SELF_REAP) {
        let (kstack_addr, kstack_order) = (prev_access.kstack_addr(), prev_access.kstack_order());
        page_free(kstack_addr as *mut c_void, kstack_order);
        // prev is now dangling -- do not touch.
    } else {
        SchedEntityRef::assume(prev_access.sched_entity_ptr()).on_cpu_store_release(0);
    }

    if chan_holding_impl() != 0 {
        sleep_unlock_irqrestore_impl(0);
    }

    rqport_rq_unlock_current_irqrestore(intr);
}

#[no_mangle]
pub extern "C" fn context_switch_finish(prev: *mut thread, next: *mut thread, intr: c_int) {
    context_switch_finish_impl(prev, next, intr)
}

// ---------------- xv6_schport_* aliases ---------------------------------
macro_rules! schport_alias {
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* ) -> $ret:ty) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) -> $ret { $target($($pn),*) }
    };
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* )) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) { $target($($pn),*) }
    };
}

macro_rules! schport_unsafe_alias {
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* ) -> $ret:ty) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) -> $ret { schport_unsafe_call!($target($($pn),*)) }
    };
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* )) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) { schport_unsafe_call!($target($($pn),*)) }
    };
}

schport_alias!(xv6_schport_chan_holding => chan_holding() -> c_int);
schport_alias!(xv6_schport_sleep_lock => sleep_lock());
schport_alias!(xv6_schport_sleep_unlock => sleep_unlock());
schport_alias!(xv6_schport_sleep_lock_irqsave => sleep_lock_irqsave() -> c_int);
schport_alias!(xv6_schport_sleep_unlock_irqrestore => sleep_unlock_irqrestore(state: c_int));
schport_alias!(xv6_schport_sched_holding => sched_holding() -> c_int);
schport_alias!(xv6_schport_scheduler_init => scheduler_init());
schport_alias!(xv6_schport_switch_to => switch_to(cur: *mut thread, target: *mut thread) -> *mut thread);
schport_alias!(xv6_schport_scheduler_yield => scheduler_yield());
schport_alias!(xv6_schport_scheduler_sleep => scheduler_sleep(lk: *mut spinlock_t, s: thread_state));
schport_alias!(xv6_schport_scheduler_wakeup => scheduler_wakeup(p: *mut thread));
schport_alias!(xv6_schport_scheduler_wakeup_timeout => scheduler_wakeup_timeout(p: *mut thread));
schport_alias!(xv6_schport_scheduler_wakeup_killable => scheduler_wakeup_killable(p: *mut thread));
schport_alias!(xv6_schport_scheduler_wakeup_interruptible => scheduler_wakeup_interruptible(p: *mut thread));
schport_alias!(xv6_schport_scheduler_wakeup_stopped => scheduler_wakeup_stopped(p: *mut thread));
schport_alias!(xv6_schport_sleep_on_chan => sleep_on_chan(chan: *mut c_void, lk: *mut spinlock_t));
schport_alias!(xv6_schport_sleep_on_chan_interruptible => sleep_on_chan_interruptible(chan: *mut c_void, lk: *mut spinlock_t) -> c_int);
schport_alias!(xv6_schport_wakeup_on_chan => wakeup_on_chan(chan: *mut c_void));
schport_unsafe_alias!(xv6_schport_scheduler_dump_chan_queue => scheduler_dump_chan_queue());
schport_alias!(xv6_schport_wakeup => wakeup(p: *mut thread));
schport_alias!(xv6_schport_wakeup_timeout => wakeup_timeout(p: *mut thread));
schport_alias!(xv6_schport_wakeup_killable => wakeup_killable(p: *mut thread));
schport_alias!(xv6_schport_wakeup_interruptible => wakeup_interruptible(p: *mut thread));
schport_alias!(xv6_schport_sys_dumpchan => sys_dumpchan() -> u64);
schport_alias!(xv6_schport_context_switch_prepare => context_switch_prepare(prev: *mut thread, next: *mut thread));
schport_alias!(xv6_schport_context_switch_finish => context_switch_finish(prev: *mut thread, next: *mut thread, intr: c_int));
