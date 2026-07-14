//! Pure-Rust port of `kernel/proc/workqueue.c`.
//!
//! Owns both the canonical public ABI (`workqueue_init`, `workqueue_create`,
//! `queue_work`, …) and the `xv6_wq_pub_*` aliases that the C side previously
//! exported from `proc_rust_shims.c::SECTION 13`. Internal `__wqp_*` helpers
//! have been translated as private Rust functions.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;

use crate::bindings::{
    list_node_t, slab_cache_t, spinlock_t, thread, tq_t, work_struct, workqueue,
    workqueue_callbacks,
};
use crate::proc::access::{
    is_err_or_null, list_node_init_raw, list_node_pop_back_raw, ptr_err_or, ListNodeRef,
    ThreadAccess, WorkStructRef, WorkqueueRef,
};

// -------- mirrored constants ----------------------------------------------
const WORK_STRUCT_FLAG_RUN_ON_DRAIN: u32 = 1 << 0;
const WORK_STRUCT_FLAG_FREE_AFTER_RUN: u32 = 1 << 1;
const WORK_STRUCT_FLAG_VALID_MASK: u32 =
    WORK_STRUCT_FLAG_RUN_ON_DRAIN | WORK_STRUCT_FLAG_FREE_AFTER_RUN;
const WORK_STRUCT_DEFAULT_FLAGS: u32 = WORK_STRUCT_FLAG_RUN_ON_DRAIN;
const WORKQUEUE_DEFAULT_MAX_ACTIVE: c_int = 8;
const WORKQUEUE_DEFAULT_MIN_ACTIVE: c_int = 2;
const MAX_WORKQUEUE_ACTIVE: c_int = 64;
const WORKQUEUE_NAME_MAX: usize = 31;
const KERNEL_STACK_ORDER: c_int = 2;
const SIGKILL: c_int = 9;
const SLAB_FLAG_EMBEDDED: u64 = 1 << 0;
const EINVAL: c_int = 22;
const ENOMEM: c_int = 12;
const THREAD_FLAG_KILLED: u64 = 2;
const THREAD_INTERRUPTIBLE: c_int = 2;

// =========================================================================
// FFI declarations as `pub safe fn` — Rust 2024 form. The caller side stays
// safe; the symbol still resolves to the original C/Rust definition.
// =========================================================================
unsafe extern "C" {
    pub safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    pub safe fn strncpy(d: *mut c_char, s: *const c_char, n: usize) -> *mut c_char;
    pub safe fn xv6_panic(msg: *const c_char) -> !;
    pub safe fn printf(fmt: *const c_char, ...);

    pub safe fn xv6_current_thread() -> *mut thread;
    pub safe fn xv6_thread_state_set(t: *mut thread, s: c_int);
    pub safe fn tcb_lock(t: *mut thread);
    pub safe fn tcb_unlock(t: *mut thread);

    pub safe fn scheduler_wakeup(t: *mut thread);
    pub safe fn scheduler_yield();
    pub safe fn wakeup(t: *mut thread);
    pub safe fn kill_thread(t: *mut thread, sig: c_int) -> c_int;
    pub safe fn exit(code: c_int) -> !;

    pub safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
    pub safe fn slab_free(p: *mut c_void);
    pub safe fn slab_cache_init(
        cache: *mut slab_cache_t, name: *mut c_char, sz: usize, flags: u64,
    ) -> c_int;

    pub safe fn spin_init(l: *mut spinlock_t, name: *mut c_char);

    pub safe fn tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t);
    pub safe fn tq_size(q: *mut tq_t) -> c_int;
    pub safe fn tq_wait(q: *mut tq_t, lock: *mut spinlock_t, slot: *mut u64) -> c_int;
    pub safe fn tq_wakeup(q: *mut tq_t, e: c_int, d: u64) -> *mut thread;
    pub safe fn tq_wakeup_all(q: *mut tq_t, e: c_int, d: u64) -> c_int;

    pub safe fn kthread_create(
        name: *const c_char, entry: *mut c_void, arg: u64, prio: c_int, order: c_int,
    ) -> *mut thread;
}

// =========================================================================
// Wrapper-construction helpers. Each pointer constructor is null-safe.
// =========================================================================
#[inline] fn wqr<'a>(p: *mut workqueue) -> Option<WorkqueueRef<'a>> {
    WorkqueueRef::from_ptr(p)
}
#[inline] fn wsr<'a>(p: *mut work_struct) -> Option<WorkStructRef<'a>> {
    WorkStructRef::from_ptr(p)
}
#[inline] fn ta<'a>(p: *mut thread) -> Option<ThreadAccess<'a>> {
    ThreadAccess::from_ptr(p)
}
#[inline] fn lnr<'a>(p: *mut list_node_t) -> Option<ListNodeRef<'a>> {
    ListNodeRef::from_ptr(p)
}

// Callback trampolines: invoking a stored `unsafe extern "C" fn` pointer is
// inherently unsafe — encapsulated once per arity.
#[inline] fn invoke_wq_cb(cb: Option<unsafe extern "C" fn(*mut workqueue)>, wq: *mut workqueue) {
    if let Some(f) = cb { unsafe { f(wq); } }
}
#[inline] fn invoke_wq_t_cb(
    cb: Option<unsafe extern "C" fn(*mut workqueue, *mut thread)>,
    wq: *mut workqueue, t: *mut thread,
) {
    if let Some(f) = cb { unsafe { f(wq, t); } }
}
#[inline] fn invoke_work_cb(
    cb: Option<unsafe extern "C" fn(*mut work_struct)>, w: *mut work_struct,
) {
    if let Some(f) = cb { unsafe { f(w); } }
}

// =========================================================================
// Thread-flag helpers (atomic bit ops via ThreadAccess).
// =========================================================================
fn thread_killed(t: *mut thread) -> bool {
    ta(t).map(|a| a.flags_test_bit(THREAD_FLAG_KILLED)).unwrap_or(false)
}
fn thread_set_killed(t: *mut thread) {
    if let Some(a) = ta(t) { a.flags_set_bit(THREAD_FLAG_KILLED); }
}

// -------- slab cache storage ----------------------------------------------
#[repr(transparent)]
struct CacheCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: both `WQP_WORKQUEUE_CACHE` and `WQP_WORK_STRUCT_CACHE` are
// written in full by `slab_cache_init` (called once during workqueue
// subsystem init, before any `slab_alloc` on either cache) and
// otherwise only accessed through the C slab allocator's own
// internally-synchronized primitives — same pattern as
// `thread_group.rs`'s `TG_POOL`.
unsafe impl Sync for CacheCell {}
static WQP_WORKQUEUE_CACHE: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));
static WQP_WORK_STRUCT_CACHE: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));
#[inline] fn wq_cache() -> *mut slab_cache_t { WQP_WORKQUEUE_CACHE.0.get() as *mut slab_cache_t }
#[inline] fn ws_cache() -> *mut slab_cache_t { WQP_WORK_STRUCT_CACHE.0.get() as *mut slab_cache_t }

#[inline] fn work_flags_valid(flags: u32) -> bool { (flags & !WORK_STRUCT_FLAG_VALID_MASK) == 0 }

fn alloc_workqueue() -> *mut workqueue {
    let wq = slab_alloc(wq_cache()) as *mut workqueue;
    if !wq.is_null() { memset(wq as *mut c_void, 0, core::mem::size_of::<workqueue>()); }
    wq
}
fn alloc_work_struct() -> *mut work_struct {
    let w = slab_alloc(ws_cache()) as *mut work_struct;
    if !w.is_null() { memset(w as *mut c_void, 0, core::mem::size_of::<work_struct>()); }
    w
}
#[inline] fn free_ptr<T>(p: *mut T) {
    if !p.is_null() { slab_free(p as *mut c_void); }
}

fn work_struct_valid(work: *mut work_struct) -> bool {
    let Some(w) = wsr(work) else { return false; };
    if !work_flags_valid(w.flags()) { return false; }
    w.func().is_some() || w.fault().is_some()
}

fn workqueue_struct_init(wq_p: *mut workqueue) {
    let Some(wq) = wqr(wq_p) else { return; };
    memset(wq_p as *mut c_void, 0, core::mem::size_of::<workqueue>());
    list_node_init_raw(wq.worker_list_ptr());
    list_node_init_raw(wq.work_list_ptr());
    spin_init(wq.lock_ptr(), c"workqueue_lock".as_ptr() as *mut c_char);
    tq_init(wq.idle_queue_ptr(), c"workqueue_idle".as_ptr(), wq.lock_ptr());
}

// -------- enqueue / dequeue ----------------------------------------------
fn enqueue_work(wq: WorkqueueRef<'_>, work: WorkStructRef<'_>) {
    if !work.entry_ref().is_detached() {
        xv6_panic(c"enqueue_work: work struct is already enqueued".as_ptr());
    }
    work.entry_ref().push_front(wq.work_list_ptr());
    wq.inc_pending_works();
}
fn dequeue_work(wq: WorkqueueRef<'_>) -> *mut work_struct {
    let off = core::mem::offset_of!(work_struct, entry);
    let last = list_node_pop_back_raw(wq.work_list_ptr());
    if last.is_null() { return ptr::null_mut(); }
    wq.dec_pending_works();
    (last as *mut u8).wrapping_sub(off) as *mut work_struct
}

// -------- callbacks -------------------------------------------------------
fn mark_workqueue_dtor_once(wq: WorkqueueRef<'_>) -> bool {
    let should = !wq.dtor_called();
    if should { wq.set_dtor_called(1); }
    should
}

fn wakeup_all_idle_workers(wq: WorkqueueRef<'_>) {
    let ret = tq_wakeup_all(wq.idle_queue_ptr(), 0, 0);
    if ret < 0 {
        printf(
            c"warning: failed to wake idle workers for workqueue %s\n".as_ptr() as *mut c_char,
            wq.name_ptr(),
        );
    }
}

// -------- exit routines ---------------------------------------------------
fn exit_worker_routine(exit_code: u64) -> ! {
    let cur_t = xv6_current_thread();
    let cur_a = ta(cur_t);
    tcb_lock(cur_t);
    let wq_p = cur_a.map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    tcb_unlock(cur_t);
    if !wq_p.is_null() {
        if let Some(wq) = wqr(wq_p) {
            invoke_wq_t_cb(wq.cb_worker_dtor(), wq_p, cur_t);
            wq.lock_ref().lock();
            if wq.manager_ptr() == cur_t {
                xv6_panic(c"Manager thread try to exit using worker exit routine".as_ptr());
            }
            tcb_lock(cur_t);
            if let Some(a) = cur_a {
                if !a.wq_entry_ref().is_detached() { a.wq_entry_ref().detach(); }
            }
            tcb_unlock(cur_t);
            wq.dec_nr_workers();
            if wq.nr_workers() < 0 {
                xv6_panic(c"Worker thread count is invalid\n".as_ptr());
            }
            wq.lock_ref().unlock();
        }
    } else if let Some(a) = cur_a {
        tcb_lock(cur_t);
        if !a.wq_entry_ref().is_detached() {
            xv6_panic(c"Worker thread not belong to a workqueue but attached\n".as_ptr());
        }
        tcb_unlock(cur_t);
    }
    exit(exit_code as c_int)
}
fn exit_manager_routine(exit_code: u64) -> ! {
    let cur_t = xv6_current_thread();
    let cur_a = ta(cur_t);
    tcb_lock(cur_t);
    let wq_p = cur_a.map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    tcb_unlock(cur_t);
    if let Some(wq) = wqr(wq_p) {
        invoke_wq_t_cb(wq.cb_manager_dtor(), wq_p, cur_t);
        wq.lock_ref().lock();
        if wq.manager_ptr() == cur_t { wq.set_manager(ptr::null_mut()); }
        wq.lock_ref().unlock();
    }
    exit(exit_code as c_int)
}

// -------- execute_work ----------------------------------------------------
fn execute_work(work_p: *mut work_struct, queue_active: bool) {
    let Some(w) = wsr(work_p) else { return; };
    let run = queue_active || w.flag_set(WORK_STRUCT_FLAG_RUN_ON_DRAIN);
    if run {
        if !queue_active {
            invoke_work_cb(w.fault(), work_p);
        } else {
            invoke_work_cb(w.func(), work_p);
        }
    }
    if w.flag_set(WORK_STRUCT_FLAG_FREE_AFTER_RUN) { free_ptr(work_p); }
}

// -------- worker / manager routines --------------------------------------
unsafe extern "C" fn worker_routine() {
    let cur_t = xv6_current_thread();
    tcb_lock(cur_t);
    let wq_p = ta(cur_t).map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    if wq_p.is_null() { tcb_unlock(cur_t); exit(-EINVAL); }
    tcb_unlock(cur_t);
    let Some(wq) = wqr(wq_p) else { exit(-EINVAL); };
    wq.lock_ref().lock();
    if wq.manager_ptr() == cur_t { wq.lock_ref().unlock(); exit(-EINVAL); }
    wq.lock_ref().unlock();
    invoke_wq_t_cb(wq.cb_worker_ctor(), wq_p, cur_t);
    loop {
        wq.lock_ref().lock();
        if thread_killed(cur_t) { wq.lock_ref().unlock(); exit_worker_routine(0); }
        if !wq.is_active() {
            let work = dequeue_work(wq);
            if work.is_null() { wq.lock_ref().unlock(); exit_worker_routine(0); }
            wq.lock_ref().unlock();
            execute_work(work, false);
            continue;
        }
        let mut work = dequeue_work(wq);
        if work.is_null() {
            xv6_thread_state_set(cur_t, THREAD_INTERRUPTIBLE);
            tq_wait(
                wq.idle_queue_ptr(),
                wq.lock_ptr(),
                &raw mut work as *mut *mut work_struct as *mut u64,
            );
            if !wq.lock_ref().holding() {
                xv6_panic(c"tq_wait should return with workqueue lock held".as_ptr());
            }
            if work.is_null() { wq.lock_ref().unlock(); continue; }
        }
        wq.lock_ref().unlock();
        execute_work(work, true);
    }
}

unsafe extern "C" fn manager_routine() {
    let cur_t = xv6_current_thread();
    tcb_lock(cur_t);
    let wq_p = ta(cur_t).map(|a| a.wq_ptr()).unwrap_or(ptr::null_mut());
    if wq_p.is_null() { tcb_unlock(cur_t); exit(-EINVAL); }
    tcb_unlock(cur_t);
    let Some(wq) = wqr(wq_p) else { exit(-EINVAL); };
    wq.lock_ref().lock();
    wq.set_manager(cur_t);
    wq.lock_ref().unlock();
    invoke_wq_t_cb(wq.cb_manager_ctor(), wq_p, cur_t);
    wq.lock_ref().lock();
    loop {
        if thread_killed(cur_t) {
            if wq.is_active() {
                if create_manager(wq) == 0 { wakeup_manager(wq); }
            } else {
                wakeup_all_idle_workers(wq);
            }
            wq.lock_ref().unlock();
            exit_manager_routine(0);
        }
        if !wq.is_active() {
            wakeup_all_idle_workers(wq);
            wq.lock_ref().unlock();
            exit_manager_routine(0);
        }
        if wq.nr_workers() < 0 {
            xv6_panic(c"Worker thread count is invalid\n".as_ptr());
        }
        while wq.nr_workers() < wq.min_active()
            || (wq.pending_works() > wq.nr_workers() && wq.nr_workers() < wq.max_active())
        {
            if create_worker(wq) != 0 { break; }
        }
        let idle = wq.idle_queue_ptr();
        while tq_size(idle) != 0 && wq.nr_workers() - tq_size(idle) < wq.pending_works() {
            let p = tq_wakeup(idle, 0, 0);
            if is_err_or_null(p) {
                printf(c"warning: Failed to wake up idle worker\n".as_ptr() as *mut c_char);
            }
        }
        xv6_thread_state_set(cur_t, THREAD_INTERRUPTIBLE);
        wq.lock_ref().unlock();
        scheduler_yield();
        wq.lock_ref().lock();
    }
}

fn create_worker(wq: WorkqueueRef<'_>) -> c_int {
    let worker = kthread_create(
        c"worker_thread".as_ptr(),
        worker_routine as *mut c_void,
        wq.as_ptr() as u64,
        0,
        KERNEL_STACK_ORDER,
    );
    if is_err_or_null(worker) { return ptr_err_or(worker, -ENOMEM); }
    let Some(wa) = ta(worker) else { return -ENOMEM; };
    tcb_lock(worker);
    wa.set_wq_ptr(wq.as_ptr());
    wq.inc_nr_workers();
    wa.wq_entry_ref().push_back(wq.worker_list_ptr());
    tcb_unlock(worker);
    wakeup(worker);
    0
}
fn create_manager(wq: WorkqueueRef<'_>) -> c_int {
    let manager = kthread_create(
        c"manager_thread".as_ptr(),
        manager_routine as *mut c_void,
        wq.as_ptr() as u64,
        0,
        KERNEL_STACK_ORDER,
    );
    if is_err_or_null(manager) { return ptr_err_or(manager, -ENOMEM); }
    let Some(ma) = ta(manager) else { return -ENOMEM; };
    tcb_lock(manager);
    ma.set_wq_ptr(wq.as_ptr());
    tcb_unlock(manager);
    wq.set_manager(manager);
    0
}
fn wakeup_manager(wq: WorkqueueRef<'_>) {
    let m = wq.manager_ptr();
    if !m.is_null() { scheduler_wakeup(m); }
}

// =========================================================================
// Internal implementations.  Public ABI exports below all forward here.
// =========================================================================
fn init_work_struct_ex_impl(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) {
    let Some(w) = wsr(work) else { return; };
    if !work_flags_valid(flags) {
        xv6_panic(c"init_work_struct_ex: invalid work flags".as_ptr());
    }
    w.entry_ref().init();
    w.set_func(func);
    w.set_fault(fault);
    w.set_data(data);
    w.set_flags(flags);
}
fn create_work_struct_ex_impl(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) -> *mut work_struct {
    if !work_flags_valid(flags) { return ptr::null_mut(); }
    let w = alloc_work_struct();
    if w.is_null() { return ptr::null_mut(); }
    init_work_struct_ex_impl(w, func, fault, data, flags);
    w
}
fn free_work_struct_impl(work: *mut work_struct) { free_ptr(work); }

fn workqueue_init_impl() {
    let ret = slab_cache_init(
        wq_cache(), c"workqueue".as_ptr() as *mut c_char,
        core::mem::size_of::<workqueue>(), SLAB_FLAG_EMBEDDED,
    );
    if ret != 0 { xv6_panic(c"Failed to initialize workqueue slab cache".as_ptr()); }
    let ret = slab_cache_init(
        ws_cache(), c"work_struct".as_ptr() as *mut c_char,
        core::mem::size_of::<work_struct>(), SLAB_FLAG_EMBEDDED,
    );
    if ret != 0 { xv6_panic(c"Failed to initialize work_struct slab cache".as_ptr()); }
    printf(c"workqueue subsystem initialized\n".as_ptr() as *mut c_char);
}

fn workqueue_create_with_callbacks_impl(
    name: *const c_char,
    mut max_active: c_int,
    callbacks: *const workqueue_callbacks,
) -> *mut workqueue {
    if max_active < 0 { return ptr::null_mut(); }
    if max_active == 0 { max_active = WORKQUEUE_DEFAULT_MAX_ACTIVE; }
    else if max_active > MAX_WORKQUEUE_ACTIVE { max_active = MAX_WORKQUEUE_ACTIVE; }
    let name = if name.is_null() { c"unnamed".as_ptr() } else { name };
    let wq_p = alloc_workqueue();
    if wq_p.is_null() { return ptr::null_mut(); }
    workqueue_struct_init(wq_p);
    let Some(wq) = wqr(wq_p) else { return ptr::null_mut(); };
    strncpy(wq.name_buf_ptr(), name, WORKQUEUE_NAME_MAX);
    wq.copy_callbacks(callbacks);
    wq.set_max_active(max_active);
    wq.set_min_active(if max_active < WORKQUEUE_DEFAULT_MIN_ACTIVE {
        WORKQUEUE_DEFAULT_MIN_ACTIVE
    } else { max_active });
    wq.set_nr_workers(0);
    wq.set_active(1);
    invoke_wq_cb(wq.cb_workqueue_ctor(), wq_p);
    wq.lock_ref().lock();
    if create_manager(wq) != 0 {
        let should = mark_workqueue_dtor_once(wq);
        wq.lock_ref().unlock();
        if should { invoke_wq_cb(wq.cb_workqueue_dtor(), wq_p); }
        free_ptr(wq_p);
        return ptr::null_mut();
    }
    wakeup_manager(wq);
    wq.lock_ref().unlock();
    wq_p
}

fn workqueue_kill_impl(wq_p: *mut workqueue) -> c_int {
    let Some(wq) = wqr(wq_p) else { return -EINVAL; };
    wq.lock_ref().lock();
    if !wq.is_active() { wq.lock_ref().unlock(); return 0; }
    wq.set_active(0);
    let should = mark_workqueue_dtor_once(wq);
    let manager = wq.manager_ptr();
    wq.lock_ref().unlock();
    if should { invoke_wq_cb(wq.cb_workqueue_dtor(), wq_p); }
    if !manager.is_null() {
        let r = kill_thread(manager, SIGKILL);
        if r < 0 { thread_set_killed(manager); }
        scheduler_wakeup(manager);
    }
    0
}

fn queue_work_impl(wq_p: *mut workqueue, work_p: *mut work_struct) -> bool {
    let Some(wq) = wqr(wq_p) else { return false; };
    if !work_struct_valid(work_p) { return false; }
    let Some(w) = wsr(work_p) else { return false; };
    if !w.entry_ref().is_detached() { return false; }
    wq.lock_ref().lock();
    if !wq.is_active() { wq.lock_ref().unlock(); return false; }
    enqueue_work(wq, w);
    wakeup_manager(wq);
    wq.lock_ref().unlock();
    true
}

// =========================================================================
// Public ABI: canonical names AND xv6_wq_pub_* aliases.
//
// P3-1B mesh sweep: every `xv6_wq_pub_*` alias below is genuinely dead --
// zero callers anywhere in the tree (grep-verified). They existed only to
// mirror the canonical names for the long-deleted `proc_rust_shims.c`'s
// C-ABI callers; demoted from `#[no_mangle]` rather than deleted, per
// this wave's "preserve still-plausible public API, don't silently
// delete" convention (see `goldfish_rtc_init`'s precedent). The file's
// existing blanket `#![allow(dead_code)]` covers these, so no per-item
// attribute is added.
// =========================================================================

pub(crate) extern "C" fn xv6_wq_pub_init_work_struct_ex(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) { init_work_struct_ex_impl(work, func, fault, data, flags) }

pub(crate) extern "C" fn xv6_wq_pub_init_work_struct(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
) { init_work_struct_ex_impl(work, func, None, data, WORK_STRUCT_DEFAULT_FLAGS) }

pub(crate) extern "C" fn xv6_wq_pub_init_work_struct_flags(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) { init_work_struct_ex_impl(work, func, None, data, flags) }

pub(crate) extern "C" fn xv6_wq_pub_create_work_struct_ex(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) -> *mut work_struct { create_work_struct_ex_impl(func, fault, data, flags) }

pub(crate) extern "C" fn xv6_wq_pub_create_work_struct(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
) -> *mut work_struct {
    create_work_struct_ex_impl(func, None, data, WORK_STRUCT_DEFAULT_FLAGS)
}

pub(crate) extern "C" fn xv6_wq_pub_create_work_struct_flags(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64,
    flags: u32,
) -> *mut work_struct { create_work_struct_ex_impl(func, None, data, flags) }

pub(crate) extern "C" fn xv6_wq_pub_free_work_struct(work: *mut work_struct) {
    free_work_struct_impl(work)
}

pub(crate) extern "C" fn xv6_wq_pub_workqueue_init() { workqueue_init_impl() }

pub(crate) extern "C" fn xv6_wq_pub_workqueue_create_with_callbacks(
    name: *const c_char,
    max_active: c_int,
    callbacks: *const workqueue_callbacks,
) -> *mut workqueue { workqueue_create_with_callbacks_impl(name, max_active, callbacks) }

pub(crate) extern "C" fn xv6_wq_pub_workqueue_create(
    name: *const c_char,
    max_active: c_int,
) -> *mut workqueue { workqueue_create_with_callbacks_impl(name, max_active, ptr::null()) }

pub(crate) extern "C" fn xv6_wq_pub_workqueue_kill(wq: *mut workqueue) -> c_int {
    workqueue_kill_impl(wq)
}

pub(crate) extern "C" fn xv6_wq_pub_queue_work(
    wq: *mut workqueue, work: *mut work_struct,
) -> bool { queue_work_impl(wq, work) }

pub(crate) extern "C" fn xv6_wq_pub_workqueue_runtime_smoke_test() {}

// ============== canonical ABI names ====================
pub(crate) extern "C" fn workqueue_init() { workqueue_init_impl() }
pub(crate) extern "C" fn workqueue_runtime_smoke_test() {}
#[no_mangle]
pub extern "C" fn workqueue_create(name: *const c_char, max_active: c_int) -> *mut workqueue {
    workqueue_create_with_callbacks_impl(name, max_active, ptr::null())
}
pub(crate) extern "C" fn workqueue_create_with_callbacks(
    name: *const c_char, max_active: c_int, callbacks: *const workqueue_callbacks,
) -> *mut workqueue { workqueue_create_with_callbacks_impl(name, max_active, callbacks) }
pub(crate) extern "C" fn workqueue_kill(wq: *mut workqueue) -> c_int { workqueue_kill_impl(wq) }
#[no_mangle]
pub extern "C" fn queue_work(wq: *mut workqueue, work: *mut work_struct) -> bool {
    queue_work_impl(wq, work)
}
#[no_mangle]
pub extern "C" fn init_work_struct(
    work: *mut work_struct, func: Option<unsafe extern "C" fn(*mut work_struct)>, data: u64,
) { init_work_struct_ex_impl(work, func, None, data, WORK_STRUCT_DEFAULT_FLAGS) }
pub(crate) extern "C" fn init_work_struct_flags(
    work: *mut work_struct, func: Option<unsafe extern "C" fn(*mut work_struct)>, data: u64, flags: u32,
) { init_work_struct_ex_impl(work, func, None, data, flags) }
pub(crate) extern "C" fn init_work_struct_ex(
    work: *mut work_struct,
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64, flags: u32,
) { init_work_struct_ex_impl(work, func, fault, data, flags) }
#[no_mangle]
pub extern "C" fn create_work_struct(
    func: Option<unsafe extern "C" fn(*mut work_struct)>, data: u64,
) -> *mut work_struct {
    create_work_struct_ex_impl(func, None, data, WORK_STRUCT_DEFAULT_FLAGS)
}
pub(crate) extern "C" fn create_work_struct_flags(
    func: Option<unsafe extern "C" fn(*mut work_struct)>, data: u64, flags: u32,
) -> *mut work_struct { create_work_struct_ex_impl(func, None, data, flags) }
pub(crate) extern "C" fn create_work_struct_ex(
    func: Option<unsafe extern "C" fn(*mut work_struct)>,
    fault: Option<unsafe extern "C" fn(*mut work_struct)>,
    data: u64, flags: u32,
) -> *mut work_struct { create_work_struct_ex_impl(func, fault, data, flags) }
#[no_mangle]
pub extern "C" fn free_work_struct(work: *mut work_struct) { free_work_struct_impl(work) }

// Suppress unused-ref-helper warning (kept for future consumers).
#[allow(dead_code)] fn _silence_helpers() { let _ = lnr as fn(_) -> _; }
