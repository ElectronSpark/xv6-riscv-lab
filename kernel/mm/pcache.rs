//! Page cache — Rust port of `kernel/mm/pcache.c` (Phase 1).
//!
//! `struct pcache` and `struct pcache_node` are completely opaque on the
//! Rust side; every field read/write and every macro expansion goes
//! through accessor shims in `kernel/mm/pcache_shims.c`. The algorithmic
//! logic — locking sequences, retry loops, eviction policy, flusher
//! coordination — is reproduced here in `unsafe` Rust.
//!
//! Locking order (matches C):
//!   1. global pcache spinlock
//!   2. per-pcache spinlock
//!   3. page lock
//!   4. tree_lock (rwlock)

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

// ---------------------------------------------------------------------------
// Opaque handle types
// ---------------------------------------------------------------------------
#[repr(C)] pub struct Pcache { _opaque: [u8; 0] }
#[repr(C)] pub struct PcacheNode { _opaque: [u8; 0] }
#[repr(C)] pub struct Page { _opaque: [u8; 0] }
#[repr(C)] pub struct WorkStruct { _opaque: [u8; 0] }
#[repr(C)] pub struct Thread { _opaque: [u8; 0] }
#[repr(C)] pub struct Workqueue { _opaque: [u8; 0] }
// `Spinlock` re-exported from the crate-wide canonical mirror.
pub use crate::mm::cffi::Spinlock;

type ReadPageFn = Option<unsafe extern "C" fn(*mut Pcache, *mut Page) -> c_int>;
type WritePageFn = Option<unsafe extern "C" fn(*mut Pcache, *mut Page) -> c_int>;

// ---------------------------------------------------------------------------
// FFI block
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;
    use core::ffi::{c_char, c_int, c_void};
    unsafe extern "C" {
        // constants/globals
        pub safe fn xv6_pcache_blks_per_page() -> usize;
        pub safe fn xv6_pcache_align_blkno(blkno: u64) -> u64;
        pub safe fn xv6_pcache_pgsize() -> usize;
        pub safe fn xv6_pcache_flush_interval_jiffs() -> u64;
        pub safe fn xv6_pcache_default_dirty_rate() -> u8;
        pub safe fn xv6_pcache_default_max_pages() -> u64;
        pub safe fn xv6_hz() -> u64;
        pub safe fn xv6_kernel_stack_order() -> c_int;
        pub safe fn xv6_einval() -> c_int;
        pub safe fn xv6_ebusy() -> c_int;
        pub safe fn xv6_eintr() -> c_int;
        pub safe fn xv6_eio() -> c_int;
        pub safe fn xv6_eagain() -> c_int;
        pub safe fn xv6_enoent() -> c_int;
        pub safe fn xv6_einprogress() -> c_int;

        // global state
        pub safe fn xv6_pcache_global_spin_lock();
        pub safe fn xv6_pcache_global_spin_unlock();
        pub safe fn xv6_pcache_global_spin_assert_holding();
        pub safe fn xv6_pcache_get_flusher_thread() -> *mut Thread;
        pub safe fn xv6_pcache_set_flusher_thread(t: *mut Thread);
        pub safe fn xv6_pcache_get_flush_wq() -> *mut Workqueue;
        pub safe fn xv6_pcache_set_flush_wq(wq: *mut Workqueue);
        pub safe fn xv6_pcache_get_global_count() -> c_int;
        pub safe fn xv6_pcache_inc_global_count();
        pub safe fn xv6_pcache_dec_global_count();
        pub safe fn xv6_pcache_get_flusher_running() -> bool;
        pub safe fn xv6_pcache_set_flusher_running(v: bool);
        pub safe fn xv6_pcache_global_first() -> *mut Pcache;
        pub safe fn xv6_pcache_global_next(cur: *mut Pcache) -> *mut Pcache;

        // pcache fields
        pub safe fn xv6_pcache_spinlock(p: *mut Pcache) -> *mut Spinlock;
        pub safe fn xv6_pcache_spin_init(p: *mut Pcache);
        pub safe fn xv6_pcache_spin_lock(p: *mut Pcache);
        pub safe fn xv6_pcache_spin_unlock(p: *mut Pcache);
        pub safe fn xv6_pcache_spin_assert_holding(p: *mut Pcache);
        pub safe fn xv6_pcache_tree_lock_init(p: *mut Pcache);
        pub safe fn xv6_pcache_tree_rlock(p: *mut Pcache);
        pub safe fn xv6_pcache_tree_runlock(p: *mut Pcache);
        pub safe fn xv6_pcache_tree_wlock(p: *mut Pcache);
        pub safe fn xv6_pcache_tree_wunlock(p: *mut Pcache);
        pub safe fn xv6_pcache_list_entries_init(p: *mut Pcache);
        pub safe fn xv6_pcache_list_entry_is_detached(p: *mut Pcache) -> c_int;
        pub safe fn xv6_pcache_push_global(p: *mut Pcache);
        pub safe fn xv6_pcache_detach_global(p: *mut Pcache);
        pub safe fn xv6_pcache_blk_count(p: *mut Pcache) -> u64;
        pub safe fn xv6_pcache_page_count(p: *mut Pcache) -> i64;
        pub safe fn xv6_pcache_set_page_count(p: *mut Pcache, v: i64);
        pub safe fn xv6_pcache_add_page_count(p: *mut Pcache, d: i64);
        pub safe fn xv6_pcache_lru_count(p: *mut Pcache) -> i64;
        pub safe fn xv6_pcache_set_lru_count(p: *mut Pcache, v: i64);
        pub safe fn xv6_pcache_inc_lru_count(p: *mut Pcache);
        pub safe fn xv6_pcache_dec_lru_count(p: *mut Pcache);
        pub safe fn xv6_pcache_dirty_count(p: *mut Pcache) -> i64;
        pub safe fn xv6_pcache_set_dirty_count(p: *mut Pcache, v: i64);
        pub safe fn xv6_pcache_inc_dirty_count(p: *mut Pcache);
        pub safe fn xv6_pcache_dec_dirty_count(p: *mut Pcache);
        pub safe fn xv6_pcache_max_pages(p: *mut Pcache) -> u64;
        pub safe fn xv6_pcache_set_max_pages(p: *mut Pcache, v: u64);
        pub safe fn xv6_pcache_dirty_rate(p: *mut Pcache) -> u8;
        pub safe fn xv6_pcache_set_dirty_rate(p: *mut Pcache, v: u8);
        pub safe fn xv6_pcache_last_request(p: *mut Pcache) -> u64;
        pub safe fn xv6_pcache_set_last_request(p: *mut Pcache, v: u64);
        pub safe fn xv6_pcache_last_flushed(p: *mut Pcache) -> u64;
        pub safe fn xv6_pcache_set_last_flushed(p: *mut Pcache, v: u64);
        pub safe fn xv6_pcache_set_flags(p: *mut Pcache, v: u64);
        pub safe fn xv6_pcache_active(p: *mut Pcache) -> c_int;
        pub safe fn xv6_pcache_set_active(p: *mut Pcache, v: c_int);
        pub safe fn xv6_pcache_flush_requested(p: *mut Pcache) -> c_int;
        pub safe fn xv6_pcache_set_flush_requested(p: *mut Pcache, v: c_int);
        pub safe fn xv6_pcache_flush_error(p: *mut Pcache) -> c_int;
        pub safe fn xv6_pcache_set_flush_error(p: *mut Pcache, v: c_int);
        pub safe fn xv6_pcache_wait_refcount(p: *mut Pcache) -> u32;
        pub safe fn xv6_pcache_inc_wait_refcount(p: *mut Pcache);
        pub safe fn xv6_pcache_dec_wait_refcount(p: *mut Pcache);
        pub safe fn xv6_pcache_set_wait_refcount(p: *mut Pcache, v: u32);
        pub safe fn xv6_pcache_gfp_flags(p: *mut Pcache) -> u64;
        pub safe fn xv6_pcache_set_gfp_flags(p: *mut Pcache, v: u64);
        pub safe fn xv6_pcache_set_private_data(p: *mut Pcache, v: *mut c_void);
        pub safe fn xv6_pcache_get_ops(p: *mut Pcache) -> *mut c_void;
        pub safe fn xv6_pcache_ops_read_page_fn(p: *mut Pcache) -> *mut c_void;
        pub safe fn xv6_pcache_ops_write_page_fn(p: *mut Pcache) -> *mut c_void;
        pub safe fn xv6_pcache_ops_call_read_page(p: *mut Pcache, page: *mut Page) -> c_int;
        pub safe fn xv6_pcache_ops_call_write_page(p: *mut Pcache, page: *mut Page) -> c_int;
        pub safe fn xv6_pcache_ops_call_write_begin(p: *mut Pcache, page: *mut Page) -> c_int;
        pub safe fn xv6_pcache_ops_call_write_end(p: *mut Pcache, page: *mut Page) -> c_int;
        pub safe fn xv6_pcache_ops_call_mark_dirty(p: *mut Pcache, page: *mut Page);
        pub safe fn xv6_pcache_flush_completion_init(p: *mut Pcache);
        pub safe fn xv6_pcache_flush_completion_reinit(p: *mut Pcache);
        pub safe fn xv6_pcache_flush_completion_complete_all(p: *mut Pcache);
        pub safe fn xv6_pcache_flush_completion_wait(p: *mut Pcache);
        pub safe fn xv6_pcache_global_flusher_completion_init();
        pub safe fn xv6_pcache_global_flusher_completion_reinit();
        pub safe fn xv6_pcache_global_flusher_complete_all();
        pub safe fn xv6_pcache_global_flusher_wait();
        pub safe fn xv6_pcache_page_map_is_empty(p: *mut Pcache) -> c_int;
        pub safe fn xv6_pcache_is_zero_inited(p: *mut Pcache) -> c_int;

        // pcache_node fields
        pub safe fn xv6_pcache_node_init(n: *mut PcacheNode);
        pub safe fn xv6_pcache_node_pcache(n: *mut PcacheNode) -> *mut Pcache;
        pub safe fn xv6_pcache_node_set_pcache(n: *mut PcacheNode, p: *mut Pcache);
        pub safe fn xv6_pcache_node_page(n: *mut PcacheNode) -> *mut Page;
        pub safe fn xv6_pcache_node_set_page(n: *mut PcacheNode, page: *mut Page);
        pub safe fn xv6_pcache_node_set_data(n: *mut PcacheNode, d: *mut c_void);
        pub safe fn xv6_pcache_node_set_page_count(n: *mut PcacheNode, c: i64);
        pub safe fn xv6_pcache_node_blkno(n: *mut PcacheNode) -> u64;
        pub safe fn xv6_pcache_node_set_blkno(n: *mut PcacheNode, v: u64);
        pub safe fn xv6_pcache_node_size_get(n: *mut PcacheNode) -> usize;
        pub safe fn xv6_pcache_node_set_size(n: *mut PcacheNode, v: usize);
        pub safe fn xv6_pcache_node_set_last_request(n: *mut PcacheNode, v: u64);
        pub safe fn xv6_pcache_node_last_flushed(n: *mut PcacheNode) -> u64;
        pub safe fn xv6_pcache_node_dirty(n: *mut PcacheNode) -> c_int;
        pub safe fn xv6_pcache_node_set_dirty(n: *mut PcacheNode, v: c_int);
        pub safe fn xv6_pcache_node_uptodate(n: *mut PcacheNode) -> c_int;
        pub safe fn xv6_pcache_node_set_uptodate(n: *mut PcacheNode, v: c_int);
        pub safe fn xv6_pcache_node_io_in_progress(n: *mut PcacheNode) -> c_int;
        pub safe fn xv6_pcache_node_set_io_in_progress(n: *mut PcacheNode, v: c_int);
        pub safe fn xv6_pcache_node_lru_detached(n: *mut PcacheNode) -> c_int;
        pub safe fn xv6_pcache_node_push_lru(p: *mut Pcache, n: *mut PcacheNode);
        pub safe fn xv6_pcache_node_push_dirty(p: *mut Pcache, n: *mut PcacheNode);
        pub safe fn xv6_pcache_node_detach_lru(n: *mut PcacheNode);
        pub safe fn xv6_pcache_lru_last(p: *mut Pcache) -> *mut PcacheNode;
        pub safe fn xv6_pcache_dirty_last(p: *mut Pcache) -> *mut PcacheNode;
        pub safe fn xv6_pcache_lru_is_empty(p: *mut Pcache) -> c_int;
        pub safe fn xv6_pcache_dirty_is_empty(p: *mut Pcache) -> c_int;
        pub safe fn xv6_pcache_node_io_wait(p: *mut Pcache, n: *mut PcacheNode) -> c_int;
        pub safe fn xv6_pcache_node_io_wakeup_all(n: *mut PcacheNode);

        // rb tree
        pub safe fn xv6_pcache_rb_init(p: *mut Pcache);
        pub safe fn xv6_pcache_rb_find(p: *mut Pcache, blkno: u64) -> *mut PcacheNode;
        pub safe fn xv6_pcache_rb_insert(p: *mut Pcache, n: *mut PcacheNode) -> *mut PcacheNode;
        pub safe fn xv6_pcache_rb_delete(p: *mut Pcache, n: *mut PcacheNode);
        pub safe fn xv6_pcache_rb_first(p: *mut Pcache) -> *mut PcacheNode;

        // page
        pub safe fn xv6_page_pcache_get_pcache(page: *mut Page) -> *mut Pcache;
        pub safe fn xv6_page_pcache_set_pcache(page: *mut Page, p: *mut Pcache);
        pub safe fn xv6_page_pcache_get_node(page: *mut Page) -> *mut PcacheNode;
        pub safe fn xv6_page_pcache_set_node(page: *mut Page, n: *mut PcacheNode);
        pub safe fn xv6_page_is_pcache_type(page: *mut Page) -> c_int;
        pub safe fn xv6_pcache_page_lock_acquire(p: *mut Page);
        pub safe fn xv6_pcache_page_lock_release(p: *mut Page);
        pub safe fn xv6_pcache_page_lock_assert_holding(p: *mut Page);
        pub safe fn xv6_pcache_page_ref_inc_unlocked(p: *mut Page) -> c_int;
        pub safe fn xv6_pcache_page_ref_dec_unlocked(p: *mut Page) -> c_int;
        pub safe fn xv6_pcache_page_ref_count_fn(p: *mut Page) -> c_int;
        pub safe fn xv6_pcache_page_ref_dec(p: *mut Page) -> c_int;
        pub safe fn xv6_pcache_alloc_page() -> *mut Page;
        pub safe fn xv6_pcache_page_to_pa_ptr(p: *mut Page) -> *mut c_void;

        // workqueue
        pub safe fn xv6_pcache_init_flush_work(p: *mut Pcache,
            fn_: unsafe extern "C" fn(*mut WorkStruct));
        pub safe fn xv6_pcache_queue_flush_work(p: *mut Pcache) -> bool;
        pub safe fn xv6_pcache_work_data(w: *mut WorkStruct) -> u64;
        pub safe fn xv6_pcache_workqueue_create() -> *mut Workqueue;

        // thread/scheduling
        pub safe fn xv6_pcache_get_jiffs() -> u64;
        pub safe fn xv6_pcache_sleep_ms(ms: u64);
        pub safe fn xv6_pcache_wakeup(t: *mut Thread);
        pub safe fn xv6_pcache_wakeup_on_chan(chan: *mut c_void);
        pub safe fn xv6_pcache_sleep_on_chan(chan: *mut c_void, lock: *mut Spinlock);
        pub safe fn xv6_pcache_sleep_on_chan_interruptible(chan: *mut c_void, lock: *mut Spinlock) -> c_int;
        pub safe fn xv6_pcache_current_is_thread(t: *mut Thread) -> c_int;
        pub safe fn xv6_pcache_kthread_create(
            name: *const c_char,
            entry: unsafe extern "C" fn(u64, u64),
            a1: u64,
            a2: u64,
            stack_order: c_int,
        ) -> *mut Thread;

        // slab
        pub safe fn xv6_pcache_node_slab_init() -> c_int;
        pub safe fn xv6_pcache_node_alloc() -> *mut PcacheNode;
        pub safe fn xv6_pcache_node_free(p: *mut PcacheNode);
        pub safe fn xv6_pcache_node_slab_shrink_all();

        // panic/printf shims
        pub safe fn xv6_pcache_panic(msg: *const c_char) -> !;
        pub safe fn xv6_pcache_printf_register_warn();
        pub safe fn xv6_pcache_printf_flush_worker_null();
        pub safe fn xv6_pcache_printf_flush_queue_warn(p: *mut Pcache);
        pub safe fn xv6_pcache_printf_flush_wait_err(p: *mut Pcache, ret: c_int);
        pub safe fn xv6_pcache_printf_default_page_invalid();
        pub safe fn xv6_pcache_printf_invalid_page(page: *mut Page, p: *mut Pcache);
        pub safe fn xv6_pcache_printf_refcount_too_small(page: *mut Page, refcount: c_int);
        pub safe fn xv6_pcache_printf_read_refcount_too_small(page: *mut Page, refcount: c_int);
        pub safe fn xv6_pcache_printf_read_invalid_meta(page: *mut Page, blkno: u64, sz: usize);
        pub safe fn xv6_pcache_printf_io_unexpected(dirty: c_int, uptodate: c_int);
        pub safe fn xv6_pcache_printf_sys_sync_failed(ret: c_int);
        pub safe fn xv6_pcache_printf_subsystem_initialized();
        pub safe fn xv6_pcache_printf_flusher_started();
        pub safe fn xv6_pcache_printf_stats_header(p: *mut Pcache);
        pub safe fn xv6_pcache_printf_stats_body(active: c_int, blk_count: u64, dirty: i64,
            lru: i64, page: i64, max: i64, rate: c_int, requested: c_int, err: c_int);
        pub safe fn xv6_pcache_printf_dump_all_header(total: c_int);
    }
}
use ffi::*;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
#[inline]
fn assert_msg(cond: bool, msg: &'static [u8]) {
    if !cond {
        xv6_pcache_panic(msg.as_ptr() as *const c_char);
    }
}

#[inline]
fn pcache_is_active(p: *mut Pcache) -> bool {
    xv6_pcache_active(p) != 0
}

fn pcache_init_validate(p: *mut Pcache) -> c_int {
    let einval = xv6_einval();
    if p.is_null() {
        return -einval;
    }
    if xv6_pcache_get_ops(p).is_null() {
        return -einval;
    }
    if xv6_pcache_ops_read_page_fn(p).is_null()
        || xv6_pcache_ops_write_page_fn(p).is_null()
    {
        return -einval;
    }
    if xv6_pcache_blk_count(p) == 0 {
        return -einval;
    }
    if xv6_pcache_is_zero_inited(p) == 0 {
        return -einval;
    }
    0
}

#[inline]
fn pcache_page_valid(p: *mut Pcache, page: *mut Page) -> bool {
    if p.is_null() || page.is_null() {
        return false;
    }
    if xv6_page_is_pcache_type(page) == 0 {
        return false;
    }
    xv6_page_pcache_get_pcache(page) == p
        && !xv6_page_pcache_get_node(page).is_null()
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

// RAII drop guards for the pcache's various locking primitives. Each
// guard releases its matching shim on Drop, so callers can use scoped
// `let _g = lock_*()` patterns instead of manual lock/unlock pairs.
struct PcGlobalGuard;
impl Drop for PcGlobalGuard {
    fn drop(&mut self) { xv6_pcache_global_spin_unlock(); }
}
fn lock_pcache_global() -> PcGlobalGuard {
    xv6_pcache_global_spin_lock();
    PcGlobalGuard
}

struct PcLocalGuard(*mut Pcache);
impl Drop for PcLocalGuard {
    fn drop(&mut self) { xv6_pcache_spin_unlock(self.0); }
}
fn lock_pcache_local(p: *mut Pcache) -> PcLocalGuard {
    xv6_pcache_spin_lock(p);
    PcLocalGuard(p)
}

struct PcTreeReadGuard(*mut Pcache);
impl Drop for PcTreeReadGuard {
    fn drop(&mut self) { xv6_pcache_tree_runlock(self.0); }
}
fn rlock_pcache_tree(p: *mut Pcache) -> PcTreeReadGuard {
    xv6_pcache_tree_rlock(p);
    PcTreeReadGuard(p)
}

struct PcTreeWriteGuard(*mut Pcache);
impl Drop for PcTreeWriteGuard {
    fn drop(&mut self) { xv6_pcache_tree_wunlock(self.0); }
}
fn wlock_pcache_tree(p: *mut Pcache) -> PcTreeWriteGuard {
    xv6_pcache_tree_wlock(p);
    PcTreeWriteGuard(p)
}

fn pcache_register(p: *mut Pcache) {
    if p.is_null() {
        return;
    }
    let _gg = lock_pcache_global();
    let _gp = lock_pcache_local(p);
    if xv6_pcache_list_entry_is_detached(p) != 0 {
        xv6_pcache_push_global(p);
        xv6_pcache_inc_global_count();
    } else {
        xv6_pcache_printf_register_warn();
    }
}

// ---------------------------------------------------------------------------
// Flush coordination
// ---------------------------------------------------------------------------
fn pcache_notify_flush_complete(p: *mut Pcache) {
    if p.is_null() { return; }
    xv6_pcache_flush_completion_complete_all(p);
}

fn pcache_wait_flush_complete(p: *mut Pcache) -> c_int {
    if p.is_null() { return -xv6_einval(); }
    xv6_pcache_flush_completion_wait(p);
    xv6_pcache_flush_error(p)
}

fn pcache_queue_work(p: *mut Pcache) -> bool {
    if p.is_null() || xv6_pcache_get_flush_wq().is_null() {
        return false;
    }
    xv6_pcache_spin_assert_holding(p);
    if xv6_pcache_flush_requested(p) != 0 {
        return true;
    }
    xv6_pcache_init_flush_work(p, pcache_flush_worker);
    let queued = xv6_pcache_queue_flush_work(p);
    if queued {
        xv6_pcache_set_flush_requested(p, 1);
        xv6_pcache_set_last_request(p, xv6_pcache_get_jiffs());
        xv6_pcache_set_flush_error(p, 0);
        xv6_pcache_flush_completion_reinit(p);
    }
    queued
}

fn pcache_flush_done(p: *mut Pcache) {
    xv6_pcache_spin_assert_holding(p);
    xv6_pcache_set_flush_requested(p, 0);
    xv6_pcache_set_last_flushed(p, xv6_pcache_get_jiffs());
    pcache_notify_flush_complete(p);
}

fn pcache_flusher_start() {
    xv6_pcache_global_spin_assert_holding();
    if xv6_pcache_get_flusher_running() {
        return;
    }
    xv6_pcache_set_flusher_running(true);
    xv6_pcache_global_flusher_completion_reinit();
    let pcb = xv6_pcache_get_flusher_thread();
    if xv6_pcache_current_is_thread(pcb) == 0 {
        xv6_pcache_wakeup(pcb);
    }
}

fn pcache_wait_flusher() -> c_int {
    if xv6_pcache_get_flusher_running() {
        xv6_pcache_global_flusher_wait();
    }
    0
}

fn pcache_flusher_done() {
    xv6_pcache_global_spin_assert_holding();
    xv6_pcache_set_flusher_running(false);
    xv6_pcache_global_flusher_complete_all();
}

// ---------------------------------------------------------------------------
// Workqueue worker
// ---------------------------------------------------------------------------
unsafe extern "C" fn pcache_flush_worker(work: *mut WorkStruct) {
    let p = xv6_pcache_work_data(work) as *mut Pcache;
    let start_jiffs = xv6_pcache_get_jiffs();

    if p.is_null() {
        xv6_pcache_printf_flush_worker_null();
        return;
    }

    xv6_pcache_spin_lock(p);
    loop {
        let page = pcache_pop_dirty(p, start_jiffs);
        if page.is_null() { break; }

        let mut r = xv6_pcache_page_ref_inc_unlocked(page);
        assert_msg(r > 1, b"flush_worker: failed to increment page ref\0");

        r = pcache_node_io_begin(p, page);
        assert_msg(r == 0, b"flush_worker: failed to begin IO\0");

        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);

        // Real write outside lock
        let mut ret = xv6_pcache_ops_call_write_begin(p, page);
        if ret != 0 {
            xv6_pcache_spin_lock(p);
            xv6_pcache_set_flush_error(p, ret);
            flush_err_continue(p, page);
            continue;
        }
        ret = xv6_pcache_ops_call_write_page(p, page);
        if ret != 0 {
            let _ = xv6_pcache_ops_call_write_end(p, page);
            xv6_pcache_spin_lock(p);
            xv6_pcache_set_flush_error(p, ret);
            flush_err_continue(p, page);
            continue;
        }
        ret = xv6_pcache_ops_call_write_end(p, page);

        xv6_pcache_spin_lock(p);
        xv6_pcache_page_lock_acquire(page);
        if ret != 0 {
            xv6_pcache_set_flush_error(p, ret);
        }
        let pcnode = xv6_page_pcache_get_node(page);
        assert_msg(!pcnode.is_null(), b"flush_worker: page missing pcache_node\0");
        xv6_pcache_node_set_dirty(pcnode, 0);
        xv6_pcache_node_set_uptodate(pcnode, 1);
        let r2 = pcache_node_io_end(p, page);
        assert_msg(r2 == 0, b"flush_worker: failed to end IO\0");
        let r3 = xv6_pcache_page_ref_dec_unlocked(page);
        assert_msg(r3 >= 1, b"flush_worker: page refcount underflow\0");
        if r3 == 1 && xv6_pcache_node_lru_detached(pcnode) != 0 {
            pcache_push_lru(p, page);
            xv6_pcache_wakeup_on_chan(p as *mut c_void);
        }
        xv6_pcache_page_lock_release(page);
    }
    pcache_flush_done(p);
    xv6_pcache_spin_unlock(p);
}

/// err_continue arm of the flush worker (assumes spinlock held).
fn flush_err_continue(p: *mut Pcache, page: *mut Page) {
    xv6_pcache_spin_assert_holding(p);
    xv6_pcache_page_lock_acquire(page);
    let r = pcache_node_io_end(p, page);
    assert_msg(r == 0, b"flush_worker: failed to end IO (err)\0");
    pcache_push_dirty(p, page);
    let r2 = xv6_pcache_page_ref_dec_unlocked(page);
    assert_msg(r2 > 0, b"flush_worker: failed to decrement ref (err)\0");
    xv6_pcache_page_lock_release(page);
}

fn pcache_schedule_flushes_locked(round_start: u64, force_round: bool) -> bool {
    let mut pending_flush = false;
    let mut cur = xv6_pcache_global_first();
    while !cur.is_null() {
        let next = xv6_pcache_global_next(cur);
        xv6_pcache_spin_lock(cur);
        if !pcache_is_active(cur) {
            xv6_pcache_spin_unlock(cur);
            cur = next;
            continue;
        }
        let mut should_flush = false;
        let dirty_count = xv6_pcache_dirty_count(cur);
        if dirty_count > 0 {
            if force_round {
                should_flush = true;
            } else {
                let page_count = xv6_pcache_page_count(cur);
                let dirty_rate = xv6_pcache_dirty_rate(cur) as i64;
                let mut dirty_threshold: i64 = 0;
                if page_count > 0 && dirty_rate > 0 {
                    dirty_threshold = (page_count * dirty_rate) / 100;
                }
                if dirty_threshold == 0 && dirty_count > 0 {
                    dirty_threshold = 1;
                }
                let last_flushed = xv6_pcache_last_flushed(cur);
                if dirty_threshold > 0 && dirty_count >= dirty_threshold {
                    should_flush = true;
                } else if round_start >= last_flushed
                    && round_start - last_flushed >= xv6_pcache_flush_interval_jiffs()
                {
                    should_flush = true;
                }
            }
        }
        if should_flush {
            if !pcache_queue_work(cur) {
                if xv6_pcache_flush_requested(cur) == 0 {
                    xv6_pcache_printf_flush_queue_warn(cur);
                }
            }
        }
        if xv6_pcache_flush_requested(cur) != 0 {
            pending_flush = true;
        }
        xv6_pcache_spin_unlock(cur);
        cur = next;
    }
    pending_flush
}

fn pcache_pick_pending_before(jiffs: u64) -> *mut Pcache {
    xv6_pcache_global_spin_assert_holding();
    let mut cur = xv6_pcache_global_first();
    while !cur.is_null() {
        let next = xv6_pcache_global_next(cur);
        xv6_pcache_spin_lock(cur);
        if xv6_pcache_flush_requested(cur) != 0 {
            let last_request = xv6_pcache_last_request(cur);
            if last_request <= jiffs {
                xv6_pcache_spin_unlock(cur);
                return cur;
            }
        }
        xv6_pcache_spin_unlock(cur);
        cur = next;
    }
    ptr::null_mut()
}

fn pcache_wait_for_pending_flushes() {
    let start_jiffs = xv6_pcache_get_jiffs();
    loop {
        xv6_pcache_global_spin_lock();
        let p = pcache_pick_pending_before(start_jiffs);
        if p.is_null() {
            xv6_pcache_global_spin_unlock();
            break;
        }
        xv6_pcache_spin_lock(p);
        xv6_pcache_inc_wait_refcount(p);
        xv6_pcache_spin_unlock(p);
        xv6_pcache_global_spin_unlock();

        let ret = pcache_wait_flush_complete(p);
        if ret != 0 {
            xv6_pcache_printf_flush_wait_err(p, ret);
        }

        xv6_pcache_spin_lock(p);
        xv6_pcache_dec_wait_refcount(p);
        xv6_pcache_wakeup_on_chan(p as *mut c_void);
        xv6_pcache_spin_unlock(p);

        xv6_pcache_sleep_ms(10);
    }
}

unsafe extern "C" fn flusher_thread(_a1: u64, _a2: u64) {
    xv6_pcache_printf_flusher_started();
    loop {
        let round_start = xv6_pcache_get_jiffs();
        xv6_pcache_global_spin_lock();
        let force_round = xv6_pcache_get_flusher_running();
        pcache_flusher_start();
        let pending = pcache_schedule_flushes_locked(round_start, force_round);
        xv6_pcache_global_spin_unlock();
        if pending {
            pcache_wait_for_pending_flushes();
        }
        xv6_pcache_global_spin_lock();
        pcache_flusher_done();
        xv6_pcache_global_spin_unlock();

        let sleep_ticks = xv6_pcache_flush_interval_jiffs();
        let mut sleep_ms_val = (sleep_ticks * 1000) / xv6_hz();
        if sleep_ms_val == 0 { sleep_ms_val = 1; }
        xv6_pcache_sleep_ms(sleep_ms_val);
    }
}

fn create_flusher_thread() {
    let name = b"pcache_flusher\0".as_ptr() as *const c_char;
    let stack = xv6_kernel_stack_order();
    let np = xv6_pcache_kthread_create(name, flusher_thread, 0, 0, stack);
    assert_msg(!np.is_null(), b"Failed to create pcache flusher thread\0");
    xv6_pcache_set_flusher_thread(np);
    xv6_pcache_wakeup(np);
}

// ---------------------------------------------------------------------------
// Tree helpers (Rust-level — call into rb shims)
// ---------------------------------------------------------------------------
fn pcache_get_page_internal(
    p: *mut Pcache,
    blkno: u64,
    _size: usize,
    default_page: *mut Page,
) -> *mut Page {
    let blk_count = xv6_pcache_blk_count(p);
    if blkno >= blk_count { return ptr::null_mut(); }
    if blkno + xv6_pcache_blks_per_page() as u64 > blk_count {
        return ptr::null_mut();
    }

    if !default_page.is_null() {
        xv6_pcache_page_lock_assert_holding(default_page);
        let dp_pcache = xv6_page_pcache_get_pcache(default_page);
        let dp_node = xv6_page_pcache_get_node(default_page);
        if xv6_page_is_pcache_type(default_page) == 0
            || !dp_pcache.is_null()
            || dp_node.is_null()
            || xv6_pcache_node_page(dp_node) != default_page
        {
            xv6_pcache_printf_default_page_invalid();
            return ptr::null_mut();
        }
    }

    let found_node: *mut PcacheNode;
    if !default_page.is_null() {
        let _g = wlock_pcache_tree(p);
        let dp_node = xv6_page_pcache_get_node(default_page);
        found_node = xv6_pcache_rb_insert(p, dp_node);
        if found_node != dp_node {
            return xv6_pcache_node_page(found_node);
        }
    } else {
        let _g = rlock_pcache_tree(p);
        found_node = xv6_pcache_rb_find(p, blkno);
        if found_node.is_null() {
            return ptr::null_mut();
        }
    }
    xv6_pcache_node_page(found_node)
}

fn pcache_remove_node(p: *mut Pcache, page: *mut Page) {
    xv6_pcache_page_lock_assert_holding(page);
    let _g = wlock_pcache_tree(p);
    let pcnode = xv6_page_pcache_get_node(page);
    assert_msg(!pcnode.is_null(), b"remove_node: page has no node\0");
    assert_msg(xv6_pcache_node_page(pcnode) == page,
        b"remove_node: pcnode mismatch\0");
    assert_msg(xv6_pcache_node_lru_detached(pcnode) != 0,
        b"remove_node: must be detached from lru/dirty\0");
    xv6_pcache_rb_delete(p, pcnode);
}

// ---------------------------------------------------------------------------
// page alloc / discard
// ---------------------------------------------------------------------------
fn pcache_page_alloc() -> *mut Page {
    let pcnode = xv6_pcache_node_alloc();
    if pcnode.is_null() { return ptr::null_mut(); }
    let page = xv6_pcache_alloc_page();
    if page.is_null() {
        xv6_pcache_node_free(pcnode);
        return ptr::null_mut();
    }
    xv6_pcache_node_init(pcnode);
    xv6_pcache_node_set_page(pcnode, page);
    xv6_pcache_node_set_page_count(pcnode, 1);
    xv6_pcache_node_set_size(pcnode, xv6_pcache_pgsize());
    xv6_pcache_node_set_data(pcnode, xv6_pcache_page_to_pa_ptr(page));
    xv6_page_pcache_set_node(page, pcnode);
    xv6_page_pcache_set_pcache(page, ptr::null_mut());
    page
}

fn pcache_page_put(page: *mut Page) {
    if page.is_null() { return; }
    xv6_pcache_page_ref_dec(page);
}

fn pcache_page_discard(page: *mut Page) {
    if page.is_null() { return; }
    let pcnode = xv6_page_pcache_get_node(page);
    if !pcnode.is_null() {
        xv6_page_pcache_set_node(page, ptr::null_mut());
        xv6_pcache_node_free(pcnode);
    }
    xv6_pcache_page_ref_dec(page);
}

fn pcache_node_attach_page(p: *mut Pcache, page: *mut Page) {
    xv6_pcache_page_lock_assert_holding(page);
    xv6_pcache_spin_assert_holding(p);
    let pcnode = xv6_page_pcache_get_node(page);
    assert_msg(!pcnode.is_null(), b"attach: page has no node\0");
    assert_msg(xv6_pcache_node_page(pcnode) == page, b"attach: mismatch\0");
    assert_msg(xv6_pcache_node_pcache(pcnode).is_null(),
        b"attach: pcache_node's pcache must be NULL\0");
    xv6_pcache_node_set_page_count(pcnode, 1);
    xv6_pcache_node_set_pcache(pcnode, p);
    xv6_page_pcache_set_pcache(page, p);
    xv6_page_pcache_set_node(page, pcnode);
    xv6_pcache_add_page_count(p, 1);
}

fn pcache_node_detach_page(p: *mut Pcache, page: *mut Page) {
    xv6_pcache_page_lock_assert_holding(page);
    xv6_pcache_spin_assert_holding(p);
    let pcnode = xv6_page_pcache_get_node(page);
    assert_msg(!pcnode.is_null(), b"detach: page has no node\0");
    assert_msg(xv6_pcache_node_page(pcnode) == page, b"detach: mismatch\0");
    assert_msg(xv6_pcache_node_pcache(pcnode) == p, b"detach: pcache mismatch\0");
    assert_msg(xv6_pcache_node_lru_detached(pcnode) != 0,
        b"detach: must be detached from lru/dirty\0");
    xv6_page_pcache_set_pcache(page, ptr::null_mut());
    xv6_pcache_node_set_pcache(pcnode, ptr::null_mut());
    xv6_pcache_add_page_count(p, -1);
    assert_msg(xv6_pcache_page_count(p) >= 0, b"detach: page_count negative\0");
}

// ---------------------------------------------------------------------------
// IO sync
// ---------------------------------------------------------------------------
fn pcache_node_io_begin(p: *mut Pcache, page: *mut Page) -> c_int {
    let _g = rlock_pcache_tree(p);
    let node = xv6_page_pcache_get_node(page);
    if xv6_pcache_node_io_in_progress(node) != 0 {
        return -xv6_einval(); // We use EALREADY in C; same numeric handling not critical
    }
    xv6_pcache_node_set_io_in_progress(node, 1);
    xv6_pcache_node_set_last_request(node, xv6_pcache_get_jiffs());
    0
}

fn pcache_node_io_end(p: *mut Pcache, page: *mut Page) -> c_int {
    let _g = rlock_pcache_tree(p);
    let node = xv6_page_pcache_get_node(page);
    if xv6_pcache_node_io_in_progress(node) == 0 {
        return -xv6_einval();
    }
    xv6_pcache_node_set_io_in_progress(node, 0);
    xv6_pcache_node_io_wakeup_all(node);
    0
}

fn pcache_node_io_wait(p: *mut Pcache, page: *mut Page) -> c_int {
    let _g = rlock_pcache_tree(p);
    let node = xv6_page_pcache_get_node(page);
    while xv6_pcache_node_io_in_progress(node) != 0 {
        let _ = xv6_pcache_node_io_wait(p, node);
    }
    0
}

// ---------------------------------------------------------------------------
// LRU / dirty list
// ---------------------------------------------------------------------------
fn pcache_push_lru(p: *mut Pcache, page: *mut Page) {
    xv6_pcache_spin_assert_holding(p);
    xv6_pcache_page_lock_assert_holding(page);
    let pcnode = xv6_page_pcache_get_node(page);
    assert_msg(!pcnode.is_null(), b"push_lru: no node\0");
    assert_msg(xv6_pcache_node_dirty(pcnode) == 0, b"push_lru: dirty\0");
    assert_msg(xv6_pcache_node_pcache(pcnode) == p, b"push_lru: pcache mismatch\0");
    assert_msg(xv6_pcache_node_page(pcnode) == page, b"push_lru: page mismatch\0");
    assert_msg(xv6_pcache_page_ref_count_fn(page) == 1, b"push_lru: ref != 1\0");
    assert_msg(xv6_pcache_node_lru_detached(pcnode) != 0,
        b"push_lru: already in list\0");
    xv6_pcache_node_push_lru(p, pcnode);
    xv6_pcache_inc_lru_count(p);
}

fn pcache_pop_lru(p: *mut Pcache) -> *mut Page {
    xv6_pcache_spin_assert_holding(p);
    if xv6_pcache_lru_is_empty(p) != 0 {
        return ptr::null_mut();
    }
    loop {
        let pcnode = xv6_pcache_lru_last(p);
        if pcnode.is_null() {
            return ptr::null_mut();
        }
        let page = xv6_pcache_node_page(pcnode);
        assert_msg(!page.is_null(), b"pop_lru: no page\0");
        xv6_pcache_page_lock_acquire(page);
        if xv6_pcache_node_lru_detached(pcnode) != 0 {
            xv6_pcache_page_lock_release(page);
            continue;
        }
        assert_msg(xv6_pcache_node_pcache(pcnode) == p,
            b"pop_lru: pcache mismatch\0");
        xv6_pcache_dec_lru_count(p);
        assert_msg(xv6_pcache_lru_count(p) >= 0, b"pop_lru: count underflow\0");
        xv6_pcache_node_detach_lru(pcnode);
        return page;
    }
}

fn pcache_remove_lru(p: *mut Pcache, page: *mut Page) {
    xv6_pcache_spin_assert_holding(p);
    xv6_pcache_page_lock_assert_holding(page);
    let pcnode = xv6_page_pcache_get_node(page);
    assert_msg(!pcnode.is_null(), b"remove_lru: no node\0");
    assert_msg(xv6_pcache_node_page(pcnode) == page, b"remove_lru: page mismatch\0");
    assert_msg(xv6_pcache_node_pcache(pcnode) == p,
        b"remove_lru: pcache mismatch\0");
    assert_msg(xv6_pcache_node_lru_detached(pcnode) == 0,
        b"remove_lru: not in list\0");
    xv6_pcache_node_detach_lru(pcnode);
    if xv6_pcache_node_dirty(pcnode) != 0 {
        xv6_pcache_dec_dirty_count(p);
        assert_msg(xv6_pcache_dirty_count(p) >= 0,
            b"remove_lru: dirty count underflow\0");
    } else {
        xv6_pcache_dec_lru_count(p);
        assert_msg(xv6_pcache_lru_count(p) >= 0,
            b"remove_lru: lru count underflow\0");
    }
}

fn pcache_push_dirty(p: *mut Pcache, page: *mut Page) {
    xv6_pcache_spin_assert_holding(p);
    xv6_pcache_page_lock_assert_holding(page);
    let pcnode = xv6_page_pcache_get_node(page);
    assert_msg(!pcnode.is_null(), b"push_dirty: no node\0");
    assert_msg(xv6_pcache_node_dirty(pcnode) != 0,
        b"push_dirty: not dirty\0");
    assert_msg(xv6_pcache_node_pcache(pcnode) == p,
        b"push_dirty: pcache mismatch\0");
    assert_msg(xv6_pcache_node_page(pcnode) == page,
        b"push_dirty: page mismatch\0");
    if xv6_pcache_node_lru_detached(pcnode) != 0 {
        xv6_pcache_inc_dirty_count(p);
    } else {
        xv6_pcache_node_detach_lru(pcnode);
    }
    xv6_pcache_node_push_dirty(p, pcnode);
}

fn pcache_pop_dirty(p: *mut Pcache, latest_flush_jiffs: u64) -> *mut Page {
    xv6_pcache_spin_assert_holding(p);
    if xv6_pcache_dirty_is_empty(p) != 0 {
        return ptr::null_mut();
    }
    loop {
        let pcnode = xv6_pcache_dirty_last(p);
        if pcnode.is_null() {
            return ptr::null_mut();
        }
        let page = xv6_pcache_node_page(pcnode);
        assert_msg(!page.is_null(), b"pop_dirty: no page\0");
        xv6_pcache_page_lock_acquire(page);
        let last_flushed = xv6_pcache_node_last_flushed(pcnode);
        if last_flushed > latest_flush_jiffs && latest_flush_jiffs != 0 {
            xv6_pcache_page_lock_release(page);
            return ptr::null_mut();
        }
        if xv6_pcache_node_lru_detached(pcnode) != 0 {
            xv6_pcache_page_lock_release(page);
            continue;
        }
        assert_msg(xv6_pcache_node_pcache(pcnode) == p,
            b"pop_dirty: pcache mismatch\0");
        assert_msg(xv6_pcache_node_dirty(pcnode) != 0,
            b"pop_dirty: not dirty\0");
        assert_msg(xv6_pcache_node_io_in_progress(pcnode) == 0,
            b"pop_dirty: io in progress\0");
        xv6_pcache_dec_dirty_count(p);
        assert_msg(xv6_pcache_dirty_count(p) >= 0,
            b"pop_dirty: dirty count underflow\0");
        xv6_pcache_node_detach_lru(pcnode);
        return page;
    }
}

fn pcache_evict_lru(p: *mut Pcache) -> *mut Page {
    let page = pcache_pop_lru(p);
    if page.is_null() { return ptr::null_mut(); }
    let pcnode = xv6_page_pcache_get_node(page);
    pcache_remove_node(p, page);
    pcache_node_detach_page(p, page);
    xv6_page_pcache_set_node(page, ptr::null_mut());
    xv6_pcache_node_set_page(pcnode, ptr::null_mut());
    xv6_pcache_page_lock_release(page);
    xv6_pcache_node_free(pcnode);
    page
}

// ===========================================================================
// Public API
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn pcache_global_init() {
    extern "C" { fn xv6_pcache_globals_init(); }
    xv6_pcache_globals_init();
    let ret = xv6_pcache_node_slab_init();
    assert_msg(ret == 0, b"Failed to initialize pcache node slab\0");
    let wq = xv6_pcache_workqueue_create();
    assert_msg(!wq.is_null(), b"Failed to create global pcache flush workqueue\0");
    xv6_pcache_set_flush_wq(wq);
    xv6_pcache_printf_subsystem_initialized();
    xv6_pcache_global_flusher_completion_init();
    xv6_pcache_global_flusher_complete_all();
    create_flusher_thread();
}

#[no_mangle]
pub unsafe extern "C" fn pcache_init(p: *mut Pcache) -> c_int {
    let ret = pcache_init_validate(p);
    if ret != 0 { return ret; }
    xv6_pcache_list_entries_init(p);
    xv6_pcache_set_dirty_count(p, 0);
    xv6_pcache_set_lru_count(p, 0);
    xv6_pcache_set_page_count(p, 0);
    xv6_pcache_set_flags(p, 0);
    xv6_pcache_rb_init(p);
    if xv6_pcache_gfp_flags(p) == 0 {
        xv6_pcache_set_gfp_flags(p, 0);
    }
    xv6_pcache_spin_init(p);
    xv6_pcache_tree_lock_init(p);
    xv6_pcache_flush_completion_init(p);
    xv6_pcache_flush_completion_complete_all(p);
    xv6_pcache_set_private_data(p, ptr::null_mut());
    xv6_pcache_set_flush_error(p, 0);
    xv6_pcache_set_wait_refcount(p, 0);
    xv6_pcache_set_active(p, 1);
    xv6_pcache_set_flush_requested(p, 0);
    if xv6_pcache_max_pages(p) == 0 {
        xv6_pcache_set_max_pages(p, xv6_pcache_default_max_pages());
    }
    let rate = xv6_pcache_dirty_rate(p);
    if rate == 0 || rate > 100 {
        xv6_pcache_set_dirty_rate(p, xv6_pcache_default_dirty_rate());
    }
    let now = xv6_pcache_get_jiffs();
    xv6_pcache_set_last_flushed(p, now);
    xv6_pcache_set_last_request(p, now);
    pcache_register(p);
    0
}

#[no_mangle]
pub unsafe extern "C" fn pcache_teardown(p: *mut Pcache) {
    if p.is_null() { return; }

    // 1. unregister
    xv6_pcache_global_spin_lock();
    xv6_pcache_spin_lock(p);
    if xv6_pcache_list_entry_is_detached(p) == 0 {
        xv6_pcache_detach_global(p);
        xv6_pcache_dec_global_count();
    }
    xv6_pcache_spin_unlock(p);
    xv6_pcache_global_spin_unlock();

    // 2. wait wait_refcount drain
    xv6_pcache_spin_lock(p);
    while xv6_pcache_wait_refcount(p) > 0 {
        xv6_pcache_sleep_on_chan(p as *mut c_void, xv6_pcache_spinlock(p));
    }
    xv6_pcache_spin_unlock(p);

    // 3. mark inactive
    xv6_pcache_spin_lock(p);
    xv6_pcache_set_active(p, 0);
    let flush_pending = xv6_pcache_flush_requested(p) != 0;
    xv6_pcache_wakeup_on_chan(p as *mut c_void);
    xv6_pcache_spin_unlock(p);

    if flush_pending {
        let _ = pcache_wait_flush_complete(p);
    }

    // 4. evict clean LRU pages
    xv6_pcache_spin_lock(p);
    loop {
        let victim = pcache_evict_lru(p);
        if victim.is_null() { break; }
        pcache_page_put(victim);
    }
    xv6_pcache_spin_unlock(p);

    // 5. drain remaining rb-tree
    xv6_pcache_spin_lock(p);
    xv6_pcache_tree_wlock(p);
    loop {
        let node = xv6_pcache_rb_first(p);
        if node.is_null() { break; }
        xv6_pcache_rb_delete(p, node);
        let pg = xv6_pcache_node_page(node);
        if !pg.is_null() {
            xv6_pcache_page_lock_acquire(pg);
            if xv6_pcache_node_lru_detached(node) == 0 {
                xv6_pcache_node_detach_lru(node);
            }
            xv6_page_pcache_set_node(pg, ptr::null_mut());
            xv6_page_pcache_set_pcache(pg, ptr::null_mut());
            xv6_pcache_node_set_page(node, ptr::null_mut());
            xv6_pcache_page_lock_release(pg);
            pcache_page_put(pg);
        }
        xv6_pcache_node_free(node);
    }
    xv6_pcache_set_page_count(p, 0);
    xv6_pcache_set_lru_count(p, 0);
    xv6_pcache_set_dirty_count(p, 0);
    xv6_pcache_tree_wunlock(p);
    xv6_pcache_spin_unlock(p);
}

#[no_mangle]
pub unsafe extern "C" fn pcache_get_page(p: *mut Pcache, blkno: u64) -> *mut Page {
    if p.is_null() || !pcache_is_active(p) { return ptr::null_mut(); }
    let base_blkno = xv6_pcache_align_blkno(blkno);
    let blk_count = xv6_pcache_blk_count(p);
    if base_blkno >= blk_count { return ptr::null_mut(); }
    if base_blkno + xv6_pcache_blks_per_page() as u64 > blk_count {
        return ptr::null_mut();
    }

    'retry_lookup: loop {
        let page = pcache_get_page_internal(p, base_blkno, xv6_pcache_pgsize(), ptr::null_mut());
        if !page.is_null() {
            xv6_pcache_spin_lock(p);
            xv6_pcache_page_lock_acquire(page);
            if !pcache_page_valid(p, page) {
                xv6_pcache_page_lock_release(page);
                xv6_pcache_spin_unlock(p);
                continue 'retry_lookup;
            }
            let pcnode = xv6_page_pcache_get_node(page);
            assert_msg(!pcnode.is_null(), b"get_page: missing pcache node\0");
            if xv6_pcache_node_blkno(pcnode) != base_blkno {
                xv6_pcache_page_lock_release(page);
                xv6_pcache_spin_unlock(p);
                continue 'retry_lookup;
            }
            if xv6_pcache_node_dirty(pcnode) == 0
                && xv6_pcache_node_lru_detached(pcnode) == 0
            {
                pcache_remove_lru(p, page);
            }
            let r = xv6_pcache_page_ref_inc_unlocked(page);
            if r < 0 {
                xv6_pcache_page_lock_release(page);
                xv6_pcache_spin_unlock(p);
                continue 'retry_lookup;
            }
            xv6_pcache_page_lock_release(page);
            xv6_pcache_spin_unlock(p);
            return page;
        }

        // Alloc fresh page
        let new_page = pcache_page_alloc();
        if new_page.is_null() { return ptr::null_mut(); }

        xv6_pcache_page_lock_acquire(new_page);
        let new_pcnode = xv6_page_pcache_get_node(new_page);
        assert_msg(!new_pcnode.is_null(), b"get_page: new page no node\0");
        xv6_pcache_node_set_blkno(new_pcnode, base_blkno);
        xv6_pcache_node_set_dirty(new_pcnode, 0);
        xv6_pcache_node_set_uptodate(new_pcnode, 0);
        xv6_pcache_node_set_io_in_progress(new_pcnode, 0);
        xv6_pcache_node_set_size(new_pcnode, xv6_pcache_pgsize());

        xv6_pcache_spin_lock(p);

        if xv6_pcache_max_pages(p) > 0 {
            while xv6_pcache_page_count(p) as u64 >= xv6_pcache_max_pages(p) {
                let victim = pcache_evict_lru(p);
                if !victim.is_null() {
                    pcache_page_put(victim);
                    continue;
                }
                if xv6_pcache_dirty_count(p) > 0 {
                    pcache_queue_work(p);
                }
                xv6_pcache_page_lock_release(new_page);
                let ret = xv6_pcache_sleep_on_chan_interruptible(
                    p as *mut c_void, xv6_pcache_spinlock(p));
                xv6_pcache_page_lock_acquire(new_page);
                if ret == -xv6_eintr() {
                    xv6_pcache_page_lock_release(new_page);
                    xv6_pcache_spin_unlock(p);
                    pcache_page_discard(new_page);
                    return ptr::null_mut();
                }
                if !pcache_is_active(p) {
                    xv6_pcache_page_lock_release(new_page);
                    xv6_pcache_spin_unlock(p);
                    pcache_page_discard(new_page);
                    return ptr::null_mut();
                }
            }
        }

        let page = pcache_get_page_internal(p, base_blkno, xv6_pcache_pgsize(), new_page);
        if page.is_null() {
            xv6_pcache_page_lock_release(new_page);
            xv6_pcache_spin_unlock(p);
            pcache_page_discard(new_page);
            return ptr::null_mut();
        }

        if page != new_page {
            xv6_pcache_page_lock_release(new_page);
            xv6_pcache_spin_unlock(p);
            pcache_page_discard(new_page);
            continue 'retry_lookup;
        }

        pcache_node_attach_page(p, new_page);
        let r = xv6_pcache_page_ref_inc_unlocked(new_page);
        assert_msg(r > 1, b"get_page: failed to add caller reference\0");
        xv6_pcache_page_lock_release(new_page);
        xv6_pcache_spin_unlock(p);
        return new_page;
    }
}

#[no_mangle]
pub unsafe extern "C" fn pcache_put_page(p: *mut Pcache, page: *mut Page) {
    if p.is_null() || page.is_null() { return; }

    xv6_pcache_spin_lock(p);
    xv6_pcache_page_lock_acquire(page);

    if !pcache_page_valid(p, page) {
        xv6_pcache_printf_invalid_page(page, p);
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        return;
    }

    let pcnode = xv6_page_pcache_get_node(page);
    let refcount = xv6_pcache_page_ref_count_fn(page);
    if refcount < 2 {
        xv6_pcache_printf_refcount_too_small(page, refcount);
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        return;
    }
    let new_refcount = xv6_pcache_page_ref_dec_unlocked(page);
    assert_msg(new_refcount >= 1, b"put_page: refcount underflow\0");

    if new_refcount == 1 {
        let dirty = xv6_pcache_node_dirty(pcnode) != 0;
        let detached = xv6_pcache_node_lru_detached(pcnode) != 0;
        let uptodate = xv6_pcache_node_uptodate(pcnode) != 0;

        if dirty && detached {
            pcache_push_dirty(p, page);
        } else if !dirty && detached {
            if !uptodate {
                pcache_remove_node(p, page);
                pcache_node_detach_page(p, page);
                xv6_page_pcache_set_node(page, ptr::null_mut());
                xv6_pcache_node_set_page(pcnode, ptr::null_mut());
                xv6_pcache_wakeup_on_chan(p as *mut c_void);
                xv6_pcache_page_lock_release(page);
                xv6_pcache_spin_unlock(p);
                xv6_pcache_node_free(pcnode);
                pcache_page_put(page);
                return;
            }
            pcache_push_lru(p, page);
            xv6_pcache_wakeup_on_chan(p as *mut c_void);
        } else {
            if dirty {
                assert_msg(!detached, b"put_page: dirty page lost from dirty list\0");
            } else if !uptodate {
                if !detached {
                    pcache_remove_lru(p, page);
                }
            }
        }
    }

    xv6_pcache_page_lock_release(page);
    xv6_pcache_spin_unlock(p);
}

#[no_mangle]
pub unsafe extern "C" fn pcache_mark_page_dirty(p: *mut Pcache, page: *mut Page) -> c_int {
    if p.is_null() || page.is_null() { return -xv6_einval(); }
    let mut ret: c_int = 0;

    xv6_pcache_spin_lock(p);
    xv6_pcache_page_lock_acquire(page);

    if !pcache_page_valid(p, page) {
        ret = -xv6_einval();
    } else {
        let pcnode = xv6_page_pcache_get_node(page);
        if xv6_pcache_node_dirty(pcnode) != 0 {
            // already dirty
        } else if xv6_pcache_node_io_in_progress(pcnode) != 0 {
            ret = -xv6_ebusy();
        } else {
            if xv6_pcache_node_lru_detached(pcnode) == 0
                && xv6_pcache_node_dirty(pcnode) == 0
            {
                pcache_remove_lru(p, page);
            }
            xv6_pcache_node_set_dirty(pcnode, 1);
            xv6_pcache_node_set_uptodate(pcnode, 1);
            xv6_pcache_ops_call_mark_dirty(p, page);
            pcache_push_dirty(p, page);
        }
    }

    xv6_pcache_page_lock_release(page);
    xv6_pcache_spin_unlock(p);
    ret
}

#[no_mangle]
pub unsafe extern "C" fn pcache_invalidate_page(p: *mut Pcache, page: *mut Page) -> c_int {
    if p.is_null() || page.is_null() { return -xv6_einval(); }
    let mut ret: c_int = 0;
    xv6_pcache_spin_lock(p);
    xv6_pcache_page_lock_acquire(page);
    if !pcache_page_valid(p, page) {
        ret = -xv6_einval();
    } else {
        let pcnode = xv6_page_pcache_get_node(page);
        if xv6_pcache_node_io_in_progress(pcnode) != 0 {
            ret = -xv6_ebusy();
        } else {
            if xv6_pcache_node_lru_detached(pcnode) == 0 {
                pcache_remove_lru(p, page);
            }
            if xv6_pcache_node_dirty(pcnode) != 0 {
                xv6_pcache_node_set_dirty(pcnode, 0);
            }
            xv6_pcache_node_set_uptodate(pcnode, 0);
        }
    }
    xv6_pcache_page_lock_release(page);
    xv6_pcache_spin_unlock(p);
    ret
}

#[no_mangle]
pub unsafe extern "C" fn pcache_invalidate_blk(p: *mut Pcache, blkno: u64) -> c_int {
    if p.is_null() || !pcache_is_active(p) { return -xv6_einval(); }
    let base_blkno = xv6_pcache_align_blkno(blkno);

    xv6_pcache_spin_lock(p);
    let page = pcache_get_page_internal(p, base_blkno, xv6_pcache_pgsize(), ptr::null_mut());
    if page.is_null() {
        xv6_pcache_spin_unlock(p);
        return 0;
    }
    xv6_pcache_page_lock_acquire(page);
    if !pcache_page_valid(p, page) {
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        return 0;
    }
    let pcnode = xv6_page_pcache_get_node(page);
    if xv6_pcache_node_blkno(pcnode) != base_blkno {
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        return 0;
    }
    if xv6_pcache_node_io_in_progress(pcnode) != 0 {
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        return -xv6_ebusy();
    }
    if xv6_pcache_node_lru_detached(pcnode) == 0 {
        pcache_remove_lru(p, page);
    }
    xv6_pcache_node_set_dirty(pcnode, 0);
    xv6_pcache_node_set_uptodate(pcnode, 0);
    xv6_pcache_page_lock_release(page);
    xv6_pcache_spin_unlock(p);
    0
}

#[no_mangle]
pub unsafe extern "C" fn pcache_discard_blk(p: *mut Pcache, blkno: u64) -> c_int {
    if p.is_null() || !pcache_is_active(p) { return -xv6_einval(); }
    let base_blkno = xv6_pcache_align_blkno(blkno);

    xv6_pcache_spin_lock(p);
    let page = pcache_get_page_internal(p, base_blkno, xv6_pcache_pgsize(), ptr::null_mut());
    if page.is_null() {
        xv6_pcache_spin_unlock(p);
        return 0;
    }
    xv6_pcache_page_lock_acquire(page);
    if !pcache_page_valid(p, page) {
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        return 0;
    }
    let pcnode = xv6_page_pcache_get_node(page);
    if xv6_pcache_node_blkno(pcnode) != base_blkno {
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        return 0;
    }
    if xv6_pcache_node_io_in_progress(pcnode) != 0 {
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        return -xv6_ebusy();
    }
    if xv6_pcache_node_lru_detached(pcnode) == 0 {
        pcache_remove_lru(p, page);
    }
    xv6_pcache_tree_wlock(p);
    xv6_pcache_rb_delete(p, pcnode);
    xv6_pcache_add_page_count(p, -1);
    xv6_pcache_tree_wunlock(p);
    xv6_page_pcache_set_node(page, ptr::null_mut());
    xv6_page_pcache_set_pcache(page, ptr::null_mut());
    xv6_pcache_node_set_page(pcnode, ptr::null_mut());
    xv6_pcache_page_lock_release(page);
    xv6_pcache_spin_unlock(p);
    xv6_pcache_node_free(pcnode);
    pcache_page_put(page);
    0
}

#[no_mangle]
pub unsafe extern "C" fn pcache_flush(p: *mut Pcache) -> c_int {
    if p.is_null() { return -xv6_einval(); }
    xv6_pcache_spin_lock(p);
    if !pcache_is_active(p) {
        xv6_pcache_spin_unlock(p);
        return -xv6_einval();
    }
    let queued = pcache_queue_work(p);
    if !queued {
        xv6_pcache_set_flush_requested(p, 0);
        xv6_pcache_set_flush_error(p, -xv6_eagain());
        xv6_pcache_spin_unlock(p);
        return -xv6_eagain();
    }
    xv6_pcache_spin_unlock(p);
    pcache_wait_flush_complete(p)
}

#[no_mangle]
pub unsafe extern "C" fn pcache_sync() -> c_int {
    xv6_pcache_global_spin_lock();
    pcache_flusher_start();
    xv6_pcache_global_spin_unlock();
    pcache_wait_flusher()
}

#[no_mangle]
pub unsafe extern "C" fn pcache_read_page(p: *mut Pcache, page: *mut Page) -> c_int {
    if p.is_null() || page.is_null() { return -xv6_einval(); }

    'retry_locked: loop {
        xv6_pcache_spin_lock(p);
        xv6_pcache_page_lock_acquire(page);

        if !pcache_is_active(p) || !pcache_page_valid(p, page) {
            xv6_pcache_page_lock_release(page);
            xv6_pcache_spin_unlock(p);
            return -xv6_einval();
        }
        let refcount = xv6_pcache_page_ref_count_fn(page);
        if refcount < 2 {
            xv6_pcache_printf_read_refcount_too_small(page, refcount);
            xv6_pcache_page_lock_release(page);
            xv6_pcache_spin_unlock(p);
            return -xv6_einval();
        }
        let pcnode = xv6_page_pcache_get_node(page);
        let blkno = xv6_pcache_node_blkno(pcnode);
        let size = xv6_pcache_node_size_get(pcnode);
        if blkno >= xv6_pcache_blk_count(p) || size == 0 || size > xv6_pcache_pgsize() {
            xv6_pcache_printf_read_invalid_meta(page, blkno, size);
            xv6_pcache_page_lock_release(page);
            xv6_pcache_spin_unlock(p);
            return -xv6_einval();
        }

        if xv6_pcache_node_io_in_progress(pcnode) != 0 {
            let dirty = xv6_pcache_node_dirty(pcnode);
            let uptodate = xv6_pcache_node_uptodate(pcnode);
            xv6_pcache_page_lock_release(page);
            xv6_pcache_spin_unlock(p);
            if uptodate != 0 { return 0; }
            if dirty == 0 && uptodate == 0 {
                let _ = pcache_node_io_wait(p, page);
                continue 'retry_locked;
            }
            xv6_pcache_printf_io_unexpected(dirty, uptodate);
            return -xv6_eio();
        }

        if xv6_pcache_node_uptodate(pcnode) != 0 {
            xv6_pcache_page_lock_release(page);
            xv6_pcache_spin_unlock(p);
            return 0;
        }

        let ret = pcache_node_io_begin(p, page);
        assert_msg(ret == 0, b"read_page: io_begin failed\0");

        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);

        let mut wait_for_completion = false;
        let mut ret = xv6_pcache_ops_call_read_page(p, page);
        if ret == -xv6_einprogress() {
            wait_for_completion = true;
            ret = 0;
        } else if ret != 0 {
            let _ = pcache_node_io_end(p, page);
            return ret;
        }

        if wait_for_completion {
            let r = pcache_node_io_wait(p, page);
            if r != 0 {
                let _ = pcache_node_io_end(p, page);
                return r;
            }
        }

        xv6_pcache_spin_lock(p);
        xv6_pcache_page_lock_acquire(page);
        let mut ret_post: c_int = 0;
        if !pcache_page_valid(p, page) {
            ret_post = -xv6_einval();
        } else {
            let pcnode = xv6_page_pcache_get_node(page);
            if xv6_pcache_node_lru_detached(pcnode) == 0 {
                pcache_remove_lru(p, page);
            }
            xv6_pcache_node_set_dirty(pcnode, 0);
            xv6_pcache_node_set_uptodate(pcnode, 1);
        }
        xv6_pcache_page_lock_release(page);
        xv6_pcache_spin_unlock(p);
        let _ = pcache_node_io_end(p, page);
        return ret_post;
    }
}

// ===========================================================================
// Diagnostics and syscall handlers
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn dump_pcache_stats(p: *mut Pcache) {
    if p.is_null() { return; }
    xv6_pcache_spin_lock(p);
    xv6_pcache_printf_stats_header(p);
    xv6_pcache_printf_stats_body(
        xv6_pcache_active(p),
        xv6_pcache_blk_count(p),
        xv6_pcache_dirty_count(p),
        xv6_pcache_lru_count(p),
        xv6_pcache_page_count(p),
        xv6_pcache_max_pages(p) as i64,
        xv6_pcache_dirty_rate(p) as c_int,
        xv6_pcache_flush_requested(p),
        xv6_pcache_flush_error(p),
    );
    xv6_pcache_spin_unlock(p);
}

#[no_mangle]
pub unsafe extern "C" fn dump_all_pcache_stats() {
    xv6_pcache_global_spin_lock();
    xv6_pcache_printf_dump_all_header(xv6_pcache_get_global_count());
    let mut cur = xv6_pcache_global_first();
    while !cur.is_null() {
        let next = xv6_pcache_global_next(cur);
        dump_pcache_stats(cur);
        cur = next;
    }
    xv6_pcache_global_spin_unlock();
}

#[no_mangle]
pub unsafe extern "C" fn pcache_shrink_caches() {
    xv6_pcache_node_slab_shrink_all();
}

#[no_mangle]
pub unsafe extern "C" fn sys_sync() -> u64 {
    let ret = pcache_sync();
    if ret != 0 {
        xv6_pcache_printf_sys_sync_failed(ret);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn sys_dumppcache() -> u64 {
    dump_all_pcache_stats();
    0
}
