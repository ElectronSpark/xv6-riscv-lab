//! pcache_shims.rs - Rust port of `kernel/mm/pcache_shims.c`.
//!
//! `struct pcache` and `struct pcache_node` have complex layouts (cacheline
//! aligned spinlocks, rwlock, completion, tq, rb tree, work_struct, anonymous
//! bitfield unions). Bindgen handles the layout. This module re-exposes the
//! same `xv6_*` symbol surface the Rust port of pcache.c (`pcache.rs`)
//! expects, with field access via the bindgen-generated structs and helper
//! calls into the existing C kernel runtime (locks, rb tree, tq, workqueue,
//! slab, scheduler, page).

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(clippy::missing_safety_doc)]

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::{offset_of, size_of, MaybeUninit};
use core::ptr::{addr_of, addr_of_mut, null_mut};
use core::sync::atomic::{AtomicU32, Ordering};

use crate::bindings::{
    completion_t, list_node_t, page_t, pcache, pcache_node, pcache_ops,
    rb_node, rb_root, rb_root_opts, rwlock, slab_cache_t,
    spinlock_t, thread, tq_t, work_struct, workqueue,
    thread_state_THREAD_UNINTERRUPTIBLE,
    SLAB_FLAG_EMBEDDED,
    EAGAIN, EALREADY, EBUSY, EINPROGRESS, EINTR, EINVAL, EIO, ENOENT,
};

/// Matches `typedef int (*sleep_callback_t)(void *data);` in kernel/inc/types.h.
type SleepCallback = unsafe extern "C" fn(data: *mut c_void) -> c_int;

// ---------------------------------------------------------------------------
// Externs to the C kernel runtime
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;
    use core::ffi::{c_char, c_int, c_void};
    unsafe extern "C" {
        // Locks
        pub safe fn spin_init(l: *mut spinlock_t, name: *const c_char);
        pub safe fn spin_lock(l: *mut spinlock_t);
        pub safe fn spin_unlock(l: *mut spinlock_t);
        pub safe fn spin_holding(l: *mut spinlock_t) -> c_int;

        pub safe fn rwlock_init(rw: *mut rwlock, name: *const c_char);
        pub safe fn rwlock_rlock(rw: *mut rwlock);
        pub safe fn rwlock_runlock(rw: *mut rwlock);
        pub safe fn rwlock_wlock(rw: *mut rwlock);
        pub safe fn rwlock_wunlock(rw: *mut rwlock);
        pub safe fn rwlock_r_sleep_cb(data: *mut c_void) -> c_int;
        pub safe fn rwlock_r_wake_cb(data: *mut c_void, status: c_int);

        // Completion
        pub safe fn completion_init(c: *mut completion_t);
        pub safe fn completion_reinit(c: *mut completion_t);
        pub safe fn complete_all(c: *mut completion_t);
        pub safe fn wait_for_completion(c: *mut completion_t);

        // RB tree (allowlisted)
        pub safe fn rb_find_key(root: *mut rb_root, key: u64) -> *mut rb_node;
        pub safe fn rb_insert_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node;
        pub safe fn rb_delete_node_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node;
        pub safe fn rb_first_node(root: *mut rb_root) -> *mut rb_node;

        // Slab
        pub safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
        pub safe fn slab_free(p: *mut c_void);
        pub safe fn slab_cache_init(c: *mut slab_cache_t, name: *const c_char, sz: usize, flags: c_uint) -> c_int;
        pub safe fn slab_cache_shrink(c: *mut slab_cache_t, nums: c_int) -> c_int;

        // tq
        pub safe fn tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t);
        pub safe fn tq_wait_cb(
            q: *mut tq_t,
            sleep_cb: SleepCallback,
            wake_cb: Option<unsafe extern "C" fn(data: *mut c_void, status: c_int)>,
            data: *mut c_void,
            wake_data: *mut c_void,
        ) -> c_int;
        pub safe fn tq_wakeup_all(q: *mut tq_t, error_no: c_int, rdata: u64) -> c_int;

        // Workqueue
        pub safe fn workqueue_create(name: *const c_char, max_active: c_int) -> *mut workqueue;
        pub safe fn queue_work(wq: *mut workqueue, w: *mut work_struct) -> bool;
        pub safe fn init_work_struct(
            w: *mut work_struct,
            func: unsafe extern "C" fn(*mut work_struct),
            data: u64,
        );

        // Scheduling
        pub safe fn get_jiffs() -> u64;
        pub safe fn sleep_ms(ms: u64);
        pub safe fn wakeup(p: *mut thread);
        pub safe fn wakeup_on_chan(chan: *mut c_void);
        pub safe fn sleep_on_chan(chan: *mut c_void, lk: *mut spinlock_t);
        pub safe fn sleep_on_chan_interruptible(chan: *mut c_void, lk: *mut spinlock_t) -> c_int;
        pub safe fn kthread_create(
            name: *const c_char,
            entry: *mut c_void,
            a1: u64,
            a2: u64,
            stack_order: c_int,
        ) -> *mut thread;
        // __current_thread() is a C macro that reads `mycpu()->proc` with
        // interrupts disabled. Replicated in `current_thread_inline` below.

        // Page
        pub safe fn page_lock_acquire(p: *mut page_t);
        pub safe fn page_lock_release(p: *mut page_t);
        pub safe fn page_lock_assert_holding(p: *mut page_t);
        pub safe fn page_ref_inc_unlocked(p: *mut page_t) -> c_int;
        pub safe fn page_ref_dec_unlocked(p: *mut page_t) -> c_int;
        pub safe fn page_ref_count(p: *mut page_t) -> c_int;
        pub safe fn __page_ref_dec(p: *mut page_t) -> c_int;
        pub safe fn __page_alloc(order: u64, flags: u64) -> *mut page_t;
        pub safe fn __page_to_pa(p: *mut page_t) -> u64;

        // Panic plumbing
        pub safe fn __panic_start();
        pub safe fn __panic_end() -> !;
        pub safe fn printf(fmt: *const c_char, ...);
    }
}
use ffi::*;
use crate::machine;

// ---------------------------------------------------------------------------
// Replicate the C `__current_thread()` macro: read mycpu()->proc with
// interrupts disabled to prevent races with CPU migration.
// ---------------------------------------------------------------------------

#[inline(always)]
fn current_thread_inline() -> *mut thread {
    machine::current_thread_ptr()
}

const PGSIZE: usize = 4096;
const BLK_SIZE_SHIFT: u32 = 9; // 512-byte block
const PCACHE_DEFAULT_DIRTY_RATE: u8 = 15;
const PCACHE_DEFAULT_MAX_PAGES: u64 = 4096;
const HZ: u64 = 1000;
const PCACHE_FLUSH_INTERVAL_JIFFS: u64 = 30 * HZ;
const KERNEL_STACK_ORDER: c_int = 2;
const WORKQUEUE_DEFAULT_MAX_ACTIVE: c_int = 8;
const PAGE_TYPE_PCACHE: c_int = 4; // page_type enum value
const PAGE_FLAG_TYPE_MASK: u64 = 0xFF;

// ---------------------------------------------------------------------------
// Global storage. C-side equivalents were compile-time initialized via
// SPINLOCK_INITIALIZED / LIST_ENTRY_INITIALIZED. We zero-init and finish in
// `xv6_pcache_globals_init` (called once at the top of pcache_global_init).
// ---------------------------------------------------------------------------
#[repr(transparent)]
struct SyncCell<T>(core::cell::UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn new(v: T) -> Self { SyncCell(core::cell::UnsafeCell::new(v)) }
    #[inline] fn get(&self) -> *mut T { self.0.get() }
}

static PCACHE_GLOBAL_LIST: SyncCell<list_node_t> = SyncCell::new(list_node_t {
    prev: null_mut(),
    next: null_mut(),
});
static PCACHE_GLOBAL_COUNT: SyncCell<c_int> = SyncCell::new(0);
static PCACHE_GLOBAL_FLUSH_WQ: SyncCell<*mut workqueue> = SyncCell::new(null_mut());
static PCACHE_GLOBAL_SPINLOCK: SyncCell<MaybeUninit<spinlock_t>> =
    SyncCell::new(MaybeUninit::zeroed());
static PCACHE_NODE_SLAB: SyncCell<MaybeUninit<slab_cache_t>> =
    SyncCell::new(MaybeUninit::zeroed());
static PCACHE_GLOBAL_FLUSHER_COMPLETION: SyncCell<MaybeUninit<completion_t>> =
    SyncCell::new(MaybeUninit::zeroed());
static PCACHE_FLUSHER_THREAD_PCB: SyncCell<*mut thread> = SyncCell::new(null_mut());
static PCACHE_GLOBAL_FLUSHER_RUNNING: SyncCell<bool> = SyncCell::new(false);

#[inline]
fn global_list_head() -> *mut list_node_t { PCACHE_GLOBAL_LIST.get() }
#[inline]
fn global_spinlock() -> *mut spinlock_t { unsafe {
    (*PCACHE_GLOBAL_SPINLOCK.get()).as_mut_ptr()
} }
#[inline]
fn global_completion() -> *mut completion_t { unsafe {
    (*PCACHE_GLOBAL_FLUSHER_COMPLETION.get()).as_mut_ptr()
} }
#[inline]
fn node_slab() -> *mut slab_cache_t { unsafe {
    (*PCACHE_NODE_SLAB.get()).as_mut_ptr()
} }

static SPINLOCK_NAME: &[u8] = b"pcache_global_spinlock\0";
static PCACHE_LOCK_NAME: &[u8] = b"pcache_lock\0";
static PCACHE_TREE_LOCK_NAME: &[u8] = b"pcache_tree_lock\0";
static PCACHE_IO_NAME: &[u8] = b"pcache_io\0";
static PCACHE_NODE_NAME: &[u8] = b"pcache_node\0";
static PCACHE_FLUSH_WQ_NAME: &[u8] = b"pcache_flush_wq\0";

// ---------------------------------------------------------------------------
// list_node_t helpers (replicate the inline C versions in kernel/inc/list.h)
// ---------------------------------------------------------------------------
#[inline]
fn list_init(e: *mut list_node_t) { unsafe {
    (*e).prev = e;
    (*e).next = e;
} }
#[inline]
fn list_detach(e: *mut list_node_t) { unsafe {
    (*(*e).prev).next = (*e).next;
    (*(*e).next).prev = (*e).prev;
    list_init(e);
} }
#[inline]
fn list_insert(prev: *mut list_node_t, e: *mut list_node_t) { unsafe {
    let next = (*prev).next;
    (*e).prev = prev;
    (*e).next = next;
    (*prev).next = e;
    (*next).prev = e;
} }
#[inline]
unsafe fn list_push_front(head: *mut list_node_t, e: *mut list_node_t) {
    list_insert(head, e);
}
#[inline]
unsafe fn list_is_empty(head: *mut list_node_t) -> bool {
    (*head).next == head
}
#[inline]
unsafe fn list_entry_is_detached(e: *mut list_node_t) -> bool {
    (*e).next == e
}

// container_of-style helpers
#[inline]
unsafe fn pcache_from_list_entry(node: *mut list_node_t) -> *mut pcache {
    (node as *mut u8).wrapping_sub(offset_of!(pcache, list_entry)) as *mut pcache
}
#[inline]
unsafe fn node_from_lru_entry(node: *mut list_node_t) -> *mut pcache_node {
    (node as *mut u8).wrapping_sub(offset_of!(pcache_node, lru_entry)) as *mut pcache_node
}
#[inline]
unsafe fn node_from_tree_entry(node: *mut rb_node) -> *mut pcache_node {
    (node as *mut u8).wrapping_sub(offset_of!(pcache_node, tree_entry)) as *mut pcache_node
}
// ---------------------------------------------------------------------------
// Panic helper
// ---------------------------------------------------------------------------
static PANIC_PCACHE_FMT: &[u8] = b"PANIC: %s\n\0";

#[inline]
unsafe fn pcache_assert(cond: bool, msg: &'static [u8]) {
    if !cond {
        do_pcache_panic(msg.as_ptr() as *const c_char);
    }
}
#[inline(never)]
unsafe fn do_pcache_panic(msg: *const c_char) -> ! {
    __panic_start();
    printf(PANIC_PCACHE_FMT.as_ptr() as *const c_char, msg);
    __panic_end()
}
// ---------------------------------------------------------------------------
// One-shot global initialization invoked from pcache_global_init.
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn xv6_pcache_globals_init() {
    list_init(global_list_head());
    spin_init(global_spinlock(), SPINLOCK_NAME.as_ptr() as *const c_char);
}

// ===========================================================================
// Constants exported as functions
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_blks_per_page() -> usize {
    PGSIZE >> BLK_SIZE_SHIFT
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_blk_mask() -> u64 {
    (PGSIZE as u64 >> BLK_SIZE_SHIFT) - 1
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_align_blkno(blkno: u64) -> u64 {
    blkno & !((PGSIZE as u64 >> BLK_SIZE_SHIFT) - 1)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_pgsize() -> usize { PGSIZE }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_size() -> usize {
    size_of::<pcache_node>()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_interval_jiffs() -> u64 {
    PCACHE_FLUSH_INTERVAL_JIFFS
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_default_dirty_rate() -> u8 {
    PCACHE_DEFAULT_DIRTY_RATE
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_default_max_pages() -> u64 {
    PCACHE_DEFAULT_MAX_PAGES
}
#[no_mangle] pub unsafe extern "C" fn xv6_hz() -> u64 { HZ }
#[no_mangle] pub unsafe extern "C" fn xv6_kernel_stack_order() -> c_int { KERNEL_STACK_ORDER }
#[no_mangle] pub unsafe extern "C" fn xv6_workqueue_default_max_active() -> c_int {
    WORKQUEUE_DEFAULT_MAX_ACTIVE
}
#[no_mangle] pub unsafe extern "C" fn xv6_page_type_pcache_value() -> c_int { PAGE_TYPE_PCACHE }
#[no_mangle] pub unsafe extern "C" fn xv6_einval() -> c_int { EINVAL as c_int }
#[no_mangle] pub unsafe extern "C" fn xv6_ebusy() -> c_int { EBUSY as c_int }
#[no_mangle] pub unsafe extern "C" fn xv6_eintr() -> c_int { EINTR as c_int }
#[no_mangle] pub unsafe extern "C" fn xv6_eio() -> c_int { EIO as c_int }
#[no_mangle] pub unsafe extern "C" fn xv6_eagain() -> c_int { EAGAIN as c_int }
#[no_mangle] pub unsafe extern "C" fn xv6_enoent() -> c_int { ENOENT as c_int }
#[no_mangle] pub unsafe extern "C" fn xv6_einprogress() -> c_int { EINPROGRESS as c_int }
#[no_mangle] pub unsafe extern "C" fn xv6_ealready() -> c_int { EALREADY as c_int }

// ===========================================================================
// Globals accessors
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_list_head() -> *mut list_node_t {
    global_list_head()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_spinlock() -> *mut spinlock_t {
    global_spinlock()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_slab() -> *mut slab_cache_t {
    node_slab()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_flusher_completion() -> *mut completion_t {
    global_completion()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_get_flusher_thread() -> *mut thread {
    *PCACHE_FLUSHER_THREAD_PCB.get()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_flusher_thread(t: *mut thread) {
    *PCACHE_FLUSHER_THREAD_PCB.get() = t;
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_get_flush_wq() -> *mut workqueue {
    *PCACHE_GLOBAL_FLUSH_WQ.get()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_flush_wq(wq: *mut workqueue) {
    *PCACHE_GLOBAL_FLUSH_WQ.get() = wq;
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_get_global_count() -> c_int {
    *PCACHE_GLOBAL_COUNT.get()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_inc_global_count() {
    *PCACHE_GLOBAL_COUNT.get() += 1;
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_dec_global_count() {
    let c = PCACHE_GLOBAL_COUNT.get();
    if *c > 0 { *c -= 1; }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_get_flusher_running() -> bool {
    *PCACHE_GLOBAL_FLUSHER_RUNNING.get()
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_flusher_running(v: bool) {
    *PCACHE_GLOBAL_FLUSHER_RUNNING.get() = v;
}

// ===========================================================================
// pcache lock / list accessors
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_spinlock(p: *mut pcache) -> *mut spinlock_t {
    addr_of_mut!((*p).spinlock)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_spin_init(p: *mut pcache) {
    spin_init(addr_of_mut!((*p).spinlock), PCACHE_LOCK_NAME.as_ptr() as *const c_char);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_spin_lock(p: *mut pcache) {
    spin_lock(addr_of_mut!((*p).spinlock));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_spin_unlock(p: *mut pcache) {
    spin_unlock(addr_of_mut!((*p).spinlock));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_spin_assert_holding(p: *mut pcache) {
    pcache_assert(spin_holding(addr_of_mut!((*p).spinlock)) != 0,
        b"pcache_spin_assert_holding: pcache spinlock not held\0");
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_spin_lock() {
    spin_lock(global_spinlock());
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_spin_unlock() {
    spin_unlock(global_spinlock());
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_spin_assert_holding() {
    pcache_assert(spin_holding(global_spinlock()) != 0,
        b"pcache_global_spin_assert_holding: not held\0");
}

// tree_lock (rwlock)
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_tree_lock(p: *mut pcache) -> *mut rwlock {
    addr_of_mut!((*p).tree_lock)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_tree_lock_init(p: *mut pcache) {
    rwlock_init(addr_of_mut!((*p).tree_lock), PCACHE_TREE_LOCK_NAME.as_ptr() as *const c_char);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_tree_rlock(p: *mut pcache) {
    rwlock_rlock(addr_of_mut!((*p).tree_lock));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_tree_runlock(p: *mut pcache) {
    rwlock_runlock(addr_of_mut!((*p).tree_lock));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_tree_wlock(p: *mut pcache) {
    rwlock_wlock(addr_of_mut!((*p).tree_lock));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_tree_wunlock(p: *mut pcache) {
    rwlock_wunlock(addr_of_mut!((*p).tree_lock));
}

// List entries on the pcache
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_list_entries_init(p: *mut pcache) {
    list_init(addr_of_mut!((*p).list_entry));
    list_init(addr_of_mut!((*p).lru));
    list_init(addr_of_mut!((*p).dirty_list));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_list_entry_is_detached(p: *mut pcache) -> c_int {
    if list_entry_is_detached(addr_of_mut!((*p).list_entry)) { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_push_global(p: *mut pcache) {
    list_push_front(global_list_head(), addr_of_mut!((*p).list_entry));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_detach_global(p: *mut pcache) {
    list_detach(addr_of_mut!((*p).list_entry));
}

// ===========================================================================
// pcache scalar fields
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_blk_count(p: *mut pcache) -> u64 { (*p).blk_count }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_count(p: *mut pcache) -> i64 { (*p).page_count }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_page_count(p: *mut pcache, v: i64) { (*p).page_count = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_add_page_count(p: *mut pcache, d: i64) {
    (*p).page_count += d;
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_lru_count(p: *mut pcache) -> i64 { (*p).lru_count }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_lru_count(p: *mut pcache, v: i64) { (*p).lru_count = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_inc_lru_count(p: *mut pcache) { (*p).lru_count += 1; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_dec_lru_count(p: *mut pcache) { (*p).lru_count -= 1; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_dirty_count(p: *mut pcache) -> i64 { (*p).dirty_count }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_dirty_count(p: *mut pcache, v: i64) { (*p).dirty_count = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_inc_dirty_count(p: *mut pcache) { (*p).dirty_count += 1; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_dec_dirty_count(p: *mut pcache) { (*p).dirty_count -= 1; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_max_pages(p: *mut pcache) -> u64 { (*p).max_pages }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_max_pages(p: *mut pcache, v: u64) { (*p).max_pages = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_dirty_rate(p: *mut pcache) -> u8 { (*p).dirty_rate }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_dirty_rate(p: *mut pcache, v: u8) { (*p).dirty_rate = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_last_request(p: *mut pcache) -> u64 { (*p).last_request }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_last_request(p: *mut pcache, v: u64) { (*p).last_request = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_last_flushed(p: *mut pcache) -> u64 { (*p).last_flushed }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_last_flushed(p: *mut pcache, v: u64) { (*p).last_flushed = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flags(p: *mut pcache) -> u64 {
    (*p).__bindgen_anon_1.flags
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_flags(p: *mut pcache, v: u64) {
    (*p).__bindgen_anon_1.flags = v;
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_active(p: *mut pcache) -> c_int {
    if (*p).__bindgen_anon_1.__bindgen_anon_1.active() != 0 { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_active(p: *mut pcache, v: c_int) {
    (*p).__bindgen_anon_1.__bindgen_anon_1.set_active(if v != 0 { 1 } else { 0 });
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_requested(p: *mut pcache) -> c_int {
    if (*p).__bindgen_anon_1.__bindgen_anon_1.flush_requested() != 0 { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_flush_requested(p: *mut pcache, v: c_int) {
    (*p).__bindgen_anon_1.__bindgen_anon_1.set_flush_requested(if v != 0 { 1 } else { 0 });
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_error(p: *mut pcache) -> c_int { (*p).flush_error }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_flush_error(p: *mut pcache, v: c_int) { (*p).flush_error = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_wait_refcount(p: *mut pcache) -> u32 { (*p).wait_refcount }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_wait_refcount(p: *mut pcache, v: u32) { (*p).wait_refcount = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_inc_wait_refcount(p: *mut pcache) { (*p).wait_refcount += 1; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_dec_wait_refcount(p: *mut pcache) { (*p).wait_refcount -= 1; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_gfp_flags(p: *mut pcache) -> u64 { (*p).gfp_flags }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_gfp_flags(p: *mut pcache, v: u64) { (*p).gfp_flags = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_private_data(p: *mut pcache) -> *mut c_void { (*p).private_data }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_set_private_data(p: *mut pcache, v: *mut c_void) { (*p).private_data = v; }

// ops vtable accessors
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_get_ops(p: *mut pcache) -> *mut pcache_ops { (*p).ops }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_ops_read_page_fn(p: *mut pcache) -> *mut c_void {
    let ops = (*p).ops;
    if ops.is_null() { return null_mut(); }
    match (*ops).read_page {
        Some(f) => f as *mut c_void,
        None => null_mut(),
    }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_ops_write_page_fn(p: *mut pcache) -> *mut c_void {
    let ops = (*p).ops;
    if ops.is_null() { return null_mut(); }
    match (*ops).write_page {
        Some(f) => f as *mut c_void,
        None => null_mut(),
    }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_ops_call_read_page(p: *mut pcache, page: *mut page_t) -> c_int {
    ((*(*p).ops).read_page.unwrap_unchecked())(p, page)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_ops_call_write_page(p: *mut pcache, page: *mut page_t) -> c_int {
    ((*(*p).ops).write_page.unwrap_unchecked())(p, page)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_ops_call_write_begin(p: *mut pcache, page: *mut page_t) -> c_int {
    let ops = (*p).ops;
    if !ops.is_null() {
        if let Some(f) = (*ops).write_begin { return f(p, page); }
    }
    0
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_ops_call_write_end(p: *mut pcache, page: *mut page_t) -> c_int {
    let ops = (*p).ops;
    if !ops.is_null() {
        if let Some(f) = (*ops).write_end { return f(p, page); }
    }
    0
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_ops_call_mark_dirty(p: *mut pcache, page: *mut page_t) {
    let ops = (*p).ops;
    if !ops.is_null() {
        if let Some(f) = (*ops).mark_dirty { f(p, page); }
    }
}

// completion (flush_completion)
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_completion(p: *mut pcache) -> *mut completion_t {
    addr_of_mut!((*p).flush_completion)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_completion_init(p: *mut pcache) {
    completion_init(addr_of_mut!((*p).flush_completion));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_completion_reinit(p: *mut pcache) {
    completion_reinit(addr_of_mut!((*p).flush_completion));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_completion_complete_all(p: *mut pcache) {
    complete_all(addr_of_mut!((*p).flush_completion));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_completion_wait(p: *mut pcache) {
    wait_for_completion(addr_of_mut!((*p).flush_completion));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_flusher_completion_init() {
    completion_init(global_completion());
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_flusher_completion_reinit() {
    completion_reinit(global_completion());
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_flusher_complete_all() {
    complete_all(global_completion());
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_flusher_wait() {
    wait_for_completion(global_completion());
}

// rb tree
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_map(p: *mut pcache) -> *mut rb_root {
    addr_of_mut!((*p).page_map)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_map_is_empty(p: *mut pcache) -> c_int {
    if (*p).page_map.node.is_null() { 1 } else { 0 }
}

// flush_work
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_flush_work(p: *mut pcache) -> *mut work_struct {
    addr_of_mut!((*p).flush_work)
}

// zero-init check
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_is_zero_inited(p: *mut pcache) -> c_int {
    let rb_empty = (*p).page_map.node.is_null();
    let lru = &(*p).lru;
    let dl = &(*p).dirty_list;
    let le = &(*p).list_entry;
    if (*p).page_count == 0
        && (*p).dirty_count == 0
        && (*p).__bindgen_anon_1.flags == 0
        && rb_empty
        && lru.next.is_null() && lru.prev.is_null()
        && dl.next.is_null() && dl.prev.is_null()
        && le.next.is_null() && le.prev.is_null()
    { 1 } else { 0 }
}

// ===========================================================================
// pcache_node accessors and helpers
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_zero(n: *mut pcache_node) {
    core::ptr::write_bytes(n as *mut u8, 0, size_of::<pcache_node>());
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_init(n: *mut pcache_node) {
    core::ptr::write_bytes(n as *mut u8, 0, size_of::<pcache_node>());
    // rb_node_init: __parent_color = (unsigned long)node
    (*n).tree_entry.__parent_color = addr_of!((*n).tree_entry) as u64;
    list_init(addr_of_mut!((*n).lru_entry));
    tq_init(addr_of_mut!((*n).io_waiters), PCACHE_IO_NAME.as_ptr() as *const c_char, null_mut());
    (*n).blkno = u64::MAX;
    (*n).page_count = 0;
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_pcache(n: *mut pcache_node) -> *mut pcache { (*n).pcache }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_pcache(n: *mut pcache_node, p: *mut pcache) { (*n).pcache = p; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_page(n: *mut pcache_node) -> *mut page_t { (*n).page }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_page(n: *mut pcache_node, page: *mut page_t) { (*n).page = page; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_data(n: *mut pcache_node) -> *mut c_void { (*n).data }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_data(n: *mut pcache_node, d: *mut c_void) { (*n).data = d; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_page_count(n: *mut pcache_node) -> i64 { (*n).page_count }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_page_count(n: *mut pcache_node, c: i64) { (*n).page_count = c; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_blkno(n: *mut pcache_node) -> u64 { (*n).blkno }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_blkno(n: *mut pcache_node, v: u64) { (*n).blkno = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_size_get(n: *mut pcache_node) -> usize { (*n).size }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_size(n: *mut pcache_node, v: usize) { (*n).size = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_last_request(n: *mut pcache_node) -> u64 { (*n).last_request }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_last_request(n: *mut pcache_node, v: u64) { (*n).last_request = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_last_flushed(n: *mut pcache_node) -> u64 { (*n).last_flushed }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_last_flushed(n: *mut pcache_node, v: u64) { (*n).last_flushed = v; }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_dirty(n: *mut pcache_node) -> c_int {
    if (*n).__bindgen_anon_1.dirty() != 0 { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_dirty(n: *mut pcache_node, v: c_int) {
    (*n).__bindgen_anon_1.set_dirty(if v != 0 { 1 } else { 0 });
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_uptodate(n: *mut pcache_node) -> c_int {
    if (*n).__bindgen_anon_1.uptodate() != 0 { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_uptodate(n: *mut pcache_node, v: c_int) {
    (*n).__bindgen_anon_1.set_uptodate(if v != 0 { 1 } else { 0 });
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_io_in_progress(n: *mut pcache_node) -> c_int {
    if (*n).__bindgen_anon_1.io_in_progress() != 0 { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_set_io_in_progress(n: *mut pcache_node, v: c_int) {
    (*n).__bindgen_anon_1.set_io_in_progress(if v != 0 { 1 } else { 0 });
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_lru_detached(n: *mut pcache_node) -> c_int {
    if list_entry_is_detached(addr_of_mut!((*n).lru_entry)) { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_push_lru(p: *mut pcache, n: *mut pcache_node) {
    list_push_front(addr_of_mut!((*p).lru), addr_of_mut!((*n).lru_entry));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_push_dirty(p: *mut pcache, n: *mut pcache_node) {
    list_push_front(addr_of_mut!((*p).dirty_list), addr_of_mut!((*n).lru_entry));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_detach_lru(n: *mut pcache_node) {
    list_detach(addr_of_mut!((*n).lru_entry));
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_lru_last(p: *mut pcache) -> *mut pcache_node {
    let head = addr_of_mut!((*p).lru);
    if list_is_empty(head) { return null_mut(); }
    node_from_lru_entry((*head).prev)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_dirty_last(p: *mut pcache) -> *mut pcache_node {
    let head = addr_of_mut!((*p).dirty_list);
    if list_is_empty(head) { return null_mut(); }
    node_from_lru_entry((*head).prev)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_lru_is_empty(p: *mut pcache) -> c_int {
    if list_is_empty(addr_of_mut!((*p).lru)) { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_dirty_is_empty(p: *mut pcache) -> c_int {
    if list_is_empty(addr_of_mut!((*p).dirty_list)) { 1 } else { 0 }
}

// IO waiters tq operations
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_io_wait(p: *mut pcache, n: *mut pcache_node) -> c_int {
    let cur = current_thread_inline();
    if !cur.is_null() {
        // __thread_state_set(current, THREAD_UNINTERRUPTIBLE) using SEQ_CST atomic store
        let state_ptr = addr_of_mut!((*cur).state) as *mut AtomicU32;
        (*state_ptr).store(thread_state_THREAD_UNINTERRUPTIBLE as u32, Ordering::SeqCst);
    }
    tq_wait_cb(
        addr_of_mut!((*n).io_waiters),
        rwlock_r_sleep_cb,
        Some(rwlock_r_wake_cb),
        addr_of_mut!((*p).tree_lock) as *mut c_void,
        null_mut(),
    );
    0
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_io_wakeup_all(n: *mut pcache_node) {
    tq_wakeup_all(addr_of_mut!((*n).io_waiters), 0, 0);
}

// ===========================================================================
// rb tree manipulation
// ===========================================================================
unsafe extern "C" fn pcache_rb_compare(k1: u64, k2: u64) -> c_int {
    if k1 < k2 { -1 } else if k1 > k2 { 1 } else { 0 }
}
unsafe extern "C" fn pcache_rb_get_key(node: *mut rb_node) -> u64 {
    let pcnode = node_from_tree_entry(node);
    (*pcnode).blkno
}

static PCACHE_RB_OPTS: SyncCell<rb_root_opts> = SyncCell::new(rb_root_opts {
    keys_cmp_fun: Some(pcache_rb_compare),
    get_key_fun: Some(pcache_rb_get_key),
});

#[no_mangle] pub unsafe extern "C" fn xv6_pcache_rb_init(p: *mut pcache) {
    (*p).page_map.node = null_mut();
    (*p).page_map.opts = PCACHE_RB_OPTS.get();
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_rb_find(p: *mut pcache, blkno: u64) -> *mut pcache_node {
    let node = rb_find_key(addr_of_mut!((*p).page_map), blkno);
    if node.is_null() { return null_mut(); }
    node_from_tree_entry(node)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_rb_insert(p: *mut pcache, pcnode: *mut pcache_node) -> *mut pcache_node {
    let node = rb_insert_color(addr_of_mut!((*p).page_map), addr_of_mut!((*pcnode).tree_entry));
    if node.is_null() { return null_mut(); }
    node_from_tree_entry(node)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_rb_delete(p: *mut pcache, pcnode: *mut pcache_node) {
    let removed = rb_delete_node_color(addr_of_mut!((*p).page_map), addr_of_mut!((*pcnode).tree_entry));
    pcache_assert(removed == addr_of_mut!((*pcnode).tree_entry),
        b"xv6_pcache_rb_delete: removed mismatch\0");
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_rb_first(p: *mut pcache) -> *mut pcache_node {
    let node = rb_first_node(addr_of_mut!((*p).page_map));
    if node.is_null() { return null_mut(); }
    node_from_tree_entry(node)
}

// ===========================================================================
// page_t accessors for the pcache union member
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_page_pcache_get_pcache(page: *mut page_t) -> *mut pcache {
    (*page).__bindgen_anon_2.pcache.pcache
}
#[no_mangle] pub unsafe extern "C" fn xv6_page_pcache_set_pcache(page: *mut page_t, p: *mut pcache) {
    (*page).__bindgen_anon_2.pcache.pcache = p;
}
#[no_mangle] pub unsafe extern "C" fn xv6_page_pcache_get_node(page: *mut page_t) -> *mut pcache_node {
    (*page).__bindgen_anon_2.pcache.pcache_node
}
#[no_mangle] pub unsafe extern "C" fn xv6_page_pcache_set_node(page: *mut page_t, n: *mut pcache_node) {
    (*page).__bindgen_anon_2.pcache.pcache_node = n;
}
#[no_mangle] pub unsafe extern "C" fn xv6_page_ref_count_read(page: *mut page_t) -> c_int {
    (*page).ref_count
}
#[no_mangle] pub unsafe extern "C" fn xv6_page_is_pcache_type(page: *mut page_t) -> c_int {
    if page.is_null() { return 0; }
    let flags = (*page).__bindgen_anon_1.flags;
    if (flags & PAGE_FLAG_TYPE_MASK) == PAGE_TYPE_PCACHE as u64 { 1 } else { 0 }
}

// ===========================================================================
// Workqueue helpers
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_init_flush_work(
    p: *mut pcache,
    func: unsafe extern "C" fn(*mut work_struct),
) {
    init_work_struct(addr_of_mut!((*p).flush_work), func, p as u64);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_queue_flush_work(p: *mut pcache) -> bool {
    let wq = *PCACHE_GLOBAL_FLUSH_WQ.get();
    if wq.is_null() { return false; }
    queue_work(wq, addr_of_mut!((*p).flush_work))
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_work_data(w: *mut work_struct) -> u64 {
    (*w).data
}

// ===========================================================================
// Thread / scheduling shims
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_get_jiffs() -> u64 { get_jiffs() }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_sleep_ms(ms: u64) { sleep_ms(ms); }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_wakeup(t: *mut thread) { wakeup(t); }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_wakeup_on_chan(chan: *mut c_void) { wakeup_on_chan(chan); }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_sleep_on_chan(chan: *mut c_void, lock: *mut spinlock_t) {
    sleep_on_chan(chan, lock);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_sleep_on_chan_interruptible(chan: *mut c_void, lock: *mut spinlock_t) -> c_int {
    sleep_on_chan_interruptible(chan, lock)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_current_is_thread(t: *mut thread) -> c_int {
    if current_thread_inline() == t { 1 } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_kthread_create(
    name: *const c_char,
    entry: unsafe extern "C" fn(u64, u64),
    a1: u64,
    a2: u64,
    stack_order: c_int,
) -> *mut thread {
    let np = kthread_create(name, entry as *mut c_void, a1, a2, stack_order);
    // IS_ERR_OR_NULL: pointer in error range (high bits set) or null -> NULL
    if np.is_null() { return null_mut(); }
    let raw = np as u64;
    // ERR_PTR encodes errno as (unsigned long)(-errno); detect via high bit set
    if (raw as i64) < 0 && (raw as i64) >= -4095 { return null_mut(); }
    np
}

// ===========================================================================
// Page-level helpers used by the Rust port
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_lock_acquire(p: *mut page_t) { page_lock_acquire(p); }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_lock_release(p: *mut page_t) { page_lock_release(p); }
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_lock_assert_holding(p: *mut page_t) {
    page_lock_assert_holding(p);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_ref_inc_unlocked(p: *mut page_t) -> c_int {
    page_ref_inc_unlocked(p)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_ref_dec_unlocked(p: *mut page_t) -> c_int {
    page_ref_dec_unlocked(p)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_ref_count_fn(p: *mut page_t) -> c_int {
    page_ref_count(p)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_ref_dec(p: *mut page_t) -> c_int {
    __page_ref_dec(p)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_alloc_page() -> *mut page_t {
    __page_alloc(0, PAGE_TYPE_PCACHE as u64)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_page_to_pa_ptr(p: *mut page_t) -> *mut c_void {
    __page_to_pa(p) as *mut c_void
}

// ===========================================================================
// Slab helpers
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_slab_init() -> c_int {
    slab_cache_init(
        node_slab(),
        PCACHE_NODE_NAME.as_ptr() as *const c_char,
        size_of::<pcache_node>(),
        SLAB_FLAG_EMBEDDED,
    )
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_alloc() -> *mut c_void {
    slab_alloc(node_slab())
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_free(p: *mut c_void) {
    slab_free(p);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_node_slab_shrink_all() {
    slab_cache_shrink(node_slab(), 0x7fff_ffff);
}

// ===========================================================================
// Workqueue creation
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_workqueue_create() -> *mut workqueue {
    workqueue_create(PCACHE_FLUSH_WQ_NAME.as_ptr() as *const c_char, WORKQUEUE_DEFAULT_MAX_ACTIVE)
}

// ===========================================================================
// Iterate the global pcache list
// ===========================================================================
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_first() -> *mut pcache {
    let head = global_list_head();
    if list_is_empty(head) { return null_mut(); }
    pcache_from_list_entry((*head).next)
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_global_next(cur: *mut pcache) -> *mut pcache {
    let head = global_list_head();
    let next = (*cur).list_entry.next;
    if next == head { return null_mut(); }
    pcache_from_list_entry(next)
}

// ===========================================================================
// Panic and printf shims
// ===========================================================================
#[no_mangle]
pub unsafe extern "C" fn xv6_pcache_panic(msg: *const c_char) -> ! {
    do_pcache_panic(msg)
}

#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_register_warn() {
    printf(b"warning: __pcache_register: pcache already registered\0".as_ptr() as *const c_char);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_flush_worker_null() {
    printf(b"__pcache_flush_worker: pcache is NULL\n\0".as_ptr() as *const c_char);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_flush_queue_warn(p: *mut pcache) {
    printf(b"warning: flusher failed to queue work for pcache %p\n\0".as_ptr() as *const c_char, p);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_flush_wait_err(p: *mut pcache, ret: c_int) {
    printf(b"warning: __pcache_wait_for_pending_flushes: pcache %p flush error %d\n\0".as_ptr() as *const c_char, p, ret);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_default_page_invalid() {
    printf(b"__pcache_get_page: default_page is not from the given pcache\n\0".as_ptr() as *const c_char);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_invalid_page(page: *mut page_t, p: *mut pcache) {
    printf(b"pcache_put_page(): invalid page %p for cache %p\n\0".as_ptr() as *const c_char, page, p);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_refcount_too_small(page: *mut page_t, refcount: c_int) {
    printf(b"pcache_put_page(): page %p refcount %d is too small to drop\n\0".as_ptr() as *const c_char, page, refcount);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_read_refcount_too_small(page: *mut page_t, refcount: c_int) {
    printf(b"pcache_read_page(): page %p refcount %d is too small to read\n\0".as_ptr() as *const c_char, page, refcount);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_read_invalid_meta(page: *mut page_t, blkno: u64, sz: usize) {
    printf(b"pcache_read_page(): invalid metadata for page %p (blkno=%llu size=%zu)\n\0".as_ptr() as *const c_char,
        page, blkno, sz);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_io_unexpected(dirty: c_int, uptodate: c_int) {
    printf(b"pcache_read_page(): io in progress with unexpected state (dirty=%d uptodate=%d)\n\0".as_ptr() as *const c_char,
        dirty, uptodate);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_sys_sync_failed(ret: c_int) {
    printf(b"sys_sync: pcache_sync failed with error %d\n\0".as_ptr() as *const c_char, ret);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_subsystem_initialized() {
    printf(b"Page cache subsystem initialized\n\0".as_ptr() as *const c_char);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_flusher_started() {
    printf(b"pcache flusher thread started\n\0".as_ptr() as *const c_char);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_stats_header(p: *mut pcache) {
    printf(b"Pcache %p stats:\n\0".as_ptr() as *const c_char, p);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_stats_body(
    active: c_int, blk_count: u64, dirty: i64, lru: i64, page: i64, max: i64,
    rate: c_int, requested: c_int, err: c_int,
) {
    printf(b"  Active: %d\n\0".as_ptr() as *const c_char, active);
    printf(b"  Block count: %llu\n\0".as_ptr() as *const c_char, blk_count);
    printf(b"  Dirty count: %ld\n\0".as_ptr() as *const c_char, dirty);
    printf(b"  LRU count: %ld\n\0".as_ptr() as *const c_char, lru);
    printf(b"  Page count / Max pages: %ld/%ld\n\0".as_ptr() as *const c_char, page, max);
    printf(b"  Dirty rate: %d%%\n\0".as_ptr() as *const c_char, rate);
    printf(b"  Flush requested: %d\n\0".as_ptr() as *const c_char, requested);
    printf(b"  Flush error: %d\n\0".as_ptr() as *const c_char, err);
}
#[no_mangle] pub unsafe extern "C" fn xv6_pcache_printf_dump_all_header(total: c_int) {
    printf(b"Dumping all pcache stats:\n\0".as_ptr() as *const c_char);
    printf(b"Total pcaches: %d\n\0".as_ptr() as *const c_char, total);
}
