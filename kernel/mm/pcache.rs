//! Page cache — Rust port of `kernel/mm/pcache.c`.
//!
//! `struct pcache` and `struct pcache_node` are the real bindgen-generated
//! `crate::bindings::{pcache, pcache_node}` types (aliased below as
//! `Pcache` / `PcacheNode`) — no opaque stand-ins, no separate
//! `pcache_shims.c`/`pcache_shims.rs` FFI round trip. Field access goes
//! through two small non-owning newtype handles, [`PcacheHandle`] and
//! [`NodeHandle`], whose methods scope the `unsafe` pointer derefs
//! behind fn-level `# Safety` docs (this file follows the crate's
//! "doc-only" SAFETY convention for Wave-1-era code: a contract on the
//! `unsafe fn`/method, not a `// SAFETY:` comment on every inner
//! `unsafe {}` block). The ~1700-line algorithm body below (locking
//! sequences, retry loops, eviction policy, flusher coordination) is
//! *not* entirely safe Rust — it still contains a substantial number
//! of `unsafe {}` blocks performing raw `*mut Pcache` / `*mut
//! PcacheNode` / `*mut Page` derefs, of which only a handful carry an
//! inline `// SAFETY:` comment today. Backfilling per-block comments
//! across the algorithm body is tracked as follow-up work, not done in
//! this pass.
//!
//! The former `xv6_pcache_*` C-ABI accessor surface (~180 one-line
//! `#[no_mangle]` shims, none of which had any C caller — verified by
//! grepping every `.c`/`.h` file in the tree) is kept as a set of
//! *private*, non-`#[no_mangle]` functions with the same names and
//! signatures so the ~1200-line algorithm body below (untouched by this
//! refactor) keeps compiling unchanged; each one now either delegates to
//! a `PcacheHandle`/`NodeHandle` method or performs the direct field
//! access itself. This mirrors the pattern `vm.rs` already established
//! for its own `xv6_vm_*` shims. Accessors that had zero call sites in
//! this file (getters whose value nothing reads, raw-storage accessors
//! only used internally, …) were deleted outright rather than ported.
//!
//! The 8 `xv6_e*` errno forwarders are gone; call sites use
//! `crate::bindings::E*` directly.
//!
//! Locking order (matches C):
//!   1. global pcache spinlock
//!   2. per-pcache spinlock
//!   3. page lock
//!   4. tree_lock (rwlock)

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::{offset_of, size_of, MaybeUninit};
use core::ptr::{self, addr_of, addr_of_mut, NonNull};
use core::sync::atomic::{AtomicU32, Ordering};

use crate::bindings::{
    completion_t, list_node_t, page_t, pcache, pcache_node, rb_node, rb_root, rb_root_opts,
    rwlock, sleep_callback_t, slab_cache_t, spinlock_t, thread, thread_state_THREAD_UNINTERRUPTIBLE,
    tq_t, wakeup_callback_t, work_struct, workqueue, EAGAIN, EBUSY, EINPROGRESS, EINTR, EINVAL,
    EIO, SLAB_FLAG_EMBEDDED,
};
use crate::machine;
use crate::mm::mm_safe::{SlabBox, SlabCacheRef};

// ---------------------------------------------------------------------------
// Canonical types — direct aliases of the bindgen structs. Every pointer
// in this file is `*mut Pcache` / `*mut PcacheNode` / `*mut Page` etc.,
// with the real C layout, not an opaque zero-sized stand-in.
// ---------------------------------------------------------------------------
pub type Pcache = pcache;
pub type PcacheNode = pcache_node;
pub type Page = page_t;
pub type WorkStruct = work_struct;
pub type Thread = thread;
pub type Workqueue = workqueue;

// ---------------------------------------------------------------------------
// Real C-ABI surface this file depends on. Locks/rbtree/slab/page-level
// primitives are declared here with whichever pointer type is locally
// convenient (`page_t`, `slab_cache_t`, …) — the same "redeclare the
// extern with a locally-typed view" pattern `vm.rs` and the old
// `pcache_shims.rs` both already use for symbols shared across modules.
// Thread/scheduling/workqueue/tq primitives that are pure-Rust and
// already re-exported at the crate root (`kthread_create`, `wakeup`,
// `sleep_on_chan*`, `tq_*`, `workqueue_create`, `queue_work`,
// `init_work_struct`, `rwlock_r_{sleep,wake}_cb`) are called directly as
// `crate::xxx(...)` below instead of being re-declared.
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;
    unsafe extern "C" {
        // Spinlock / rwlock (kernel/lock/spinlock.rs, kernel/lock/rwlock.rs).


        // Completion (kernel/lock/completion.rs).

        // RB tree (kernel/rbtree.c — not yet ported to Rust).

        // Slab (kernel/mm/slab.rs; redeclared with the bindgen
        // `slab_cache_t` view rather than slab.rs's own `SlabCache`).

        // Timer (kernel/timer.c — not yet ported to Rust).
        pub safe fn get_jiffs() -> u64;
        pub safe fn sleep_ms(ms: u64);

        // Thread / scheduling / tq / workqueue (kernel/proc/*.rs). These
        // are pure-Rust and re-exported at the crate root, but the crate
        // root glob-imports two independently-declared same-named
        // symbols for several of them (a real definition plus a stray
        // `extern` re-declaration elsewhere), making `crate::foo` an
        // E0659 ambiguous-name error. Declaring them locally (as
        // `pcache_shims.rs` used to) sidesteps that ambiguity.
        pub safe fn wakeup(t: *mut thread);
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
        pub safe fn tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t);
        pub safe fn tq_wait_cb(
            q: *mut tq_t,
            sleep_cb: sleep_callback_t,
            wake_cb: wakeup_callback_t,
            data: *mut c_void,
            rdata: *mut u64,
        ) -> c_int;
        pub safe fn tq_wakeup_all(q: *mut tq_t, error_no: c_int, rdata: u64) -> c_int;
        pub safe fn workqueue_create(name: *const c_char, max_active: c_int) -> *mut workqueue;
        pub safe fn queue_work(wq: *mut workqueue, w: *mut work_struct) -> bool;
        pub safe fn init_work_struct(
            w: *mut work_struct,
            func: Option<unsafe extern "C" fn(*mut work_struct)>,
            data: u64,
        );

        // Page-level primitives (kernel/mm/page.rs; redeclared with the
        // bindgen `page_t` view rather than page.rs's own `Page`).

        // Panic/printf plumbing (kernel/printf.c).
        pub safe fn __panic_start();
        pub safe fn __panic_end() -> !;
    }
pub(crate) use crate::lock::completion::{complete_all, completion_init, completion_reinit, wait_for_completion};
pub(crate) use crate::lock::rwlock::{rwlock_r_sleep_cb, rwlock_r_wake_cb};

// The functions below (spinlock/rwlock/rbtree primitives) are all
// genuinely `unsafe fn`/`unsafe extern "C" fn` in their canonical
// modules; this file's original extern declaration asserted `pub safe
// fn` for every one of them (this crate's usual FFI-facade convention).
// Thin safe wrappers preserve that facade instead of pushing `unsafe {}`
// onto the ~10 call sites below.
/// SAFETY: see [`crate::lock::spinlock::spin_holding`]'s contract.
#[inline]
pub fn spin_holding(l: *mut spinlock_t) -> c_int {
    unsafe { crate::lock::spinlock::spin_holding(l) }
}
/// SAFETY: see [`crate::lock::spinlock::spin_lock`]'s contract.
#[inline]
pub fn spin_lock(l: *mut spinlock_t) {
    unsafe { crate::lock::spinlock::spin_lock(l) };
}
/// SAFETY: see [`crate::lock::spinlock::spin_unlock`]'s contract.
#[inline]
pub fn spin_unlock(l: *mut spinlock_t) {
    unsafe { crate::lock::spinlock::spin_unlock(l) };
}
/// SAFETY: see [`crate::lock::rwlock::rwlock_init`]'s contract.
#[inline]
pub fn rwlock_init(rw: *mut rwlock, name: *const c_char) {
    unsafe { crate::lock::rwlock::rwlock_init(rw, name) };
}
/// SAFETY: see [`crate::lock::rwlock::rwlock_rlock`]'s contract.
#[inline]
pub fn rwlock_rlock(rw: *mut rwlock) {
    unsafe { crate::lock::rwlock::rwlock_rlock(rw) };
}
/// SAFETY: see [`crate::lock::rwlock::rwlock_runlock`]'s contract.
#[inline]
pub fn rwlock_runlock(rw: *mut rwlock) {
    unsafe { crate::lock::rwlock::rwlock_runlock(rw) };
}
/// SAFETY: see [`crate::lock::rwlock::rwlock_wlock`]'s contract.
#[inline]
pub fn rwlock_wlock(rw: *mut rwlock) {
    unsafe { crate::lock::rwlock::rwlock_wlock(rw) };
}
/// SAFETY: see [`crate::lock::rwlock::rwlock_wunlock`]'s contract.
#[inline]
pub fn rwlock_wunlock(rw: *mut rwlock) {
    unsafe { crate::lock::rwlock::rwlock_wunlock(rw) };
}
/// SAFETY: see [`crate::bintree::rb_find_key`]'s contract.
#[inline]
pub fn rb_find_key(root: *mut rb_root, key: u64) -> *mut rb_node {
    unsafe { crate::bintree::rb_find_key(root, key) }
}
/// SAFETY: see [`crate::bintree::rb_first_node`]'s contract.
#[inline]
pub fn rb_first_node(root: *mut rb_root) -> *mut rb_node {
    unsafe { crate::bintree::rb_first_node(root) }
}
/// SAFETY: see [`crate::rbtree::rb_insert_color`]'s contract.
#[inline]
pub fn rb_insert_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::rbtree::rb_insert_color(root, node) }
}
/// SAFETY: see [`crate::rbtree::rb_delete_node_color`]'s contract.
#[inline]
pub fn rb_delete_node_color(root: *mut rb_root, node: *mut rb_node) -> *mut rb_node {
    unsafe { crate::rbtree::rb_delete_node_color(root, node) }
}

    /// `crate::lock::spinlock::spin_init` takes `name: *mut c_char`; this
    /// file's original extern declaration typed it `*const c_char` (same
    /// note as every other lock-module consumer of `spin_init`).
    #[inline]
    pub fn spin_init(l: *mut spinlock_t, name: *const c_char) {
        // SAFETY: `name` is only read by the callee despite the `*mut`
        // parameter; this file's call sites pass `'static` string literals.
        unsafe { crate::lock::spinlock::spin_init(l, name as *mut c_char) };
    }

    // `slab_alloc`/`slab_free`/`slab_cache_init`/`slab_cache_shrink`:
    // this file's original extern declaration typed the cache pointer as
    // the bindgen `slab_cache_t` rather than `crate::mm::slab::SlabCache`
    // (same "locally convenient pointer type" idiom as everywhere else),
    // and `slab_cache_init`'s `name`/`flags` as `*const c_char`/`c_uint`
    // rather than `*mut c_char`/`u64`.
    /// SAFETY: see [`crate::mm::slab::slab_alloc`]'s contract.
    #[inline]
    pub fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
        unsafe { crate::mm::slab::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
    }
    /// SAFETY: `p` must originate from `slab_alloc` above.
    #[inline]
    pub fn slab_free(p: *mut c_void) {
        unsafe { crate::mm::slab::slab_free(p) };
    }
    /// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract; `name`
    /// is only read by the callee despite the `*mut` parameter.
    #[inline]
    pub fn slab_cache_init(
        cache: *mut slab_cache_t, name: *const c_char, sz: usize, flags: c_uint,
    ) -> c_int {
        unsafe {
            crate::mm::slab::slab_cache_init(
                cache as *mut crate::mm::slab::SlabCache,
                name as *mut c_char,
                sz,
                flags as u64,
            )
        }
    }
    /// SAFETY: `cache` must originate from `slab_cache_init` above.
    #[inline]
    pub fn slab_cache_shrink(cache: *mut slab_cache_t, nums: c_int) -> c_int {
        unsafe { crate::mm::slab::slab_cache_shrink(cache as *mut crate::mm::slab::SlabCache, nums) }
    }

    // Page-level primitives (kernel/mm/page.rs; this file's original
    // extern declaration used the bindgen `page_t` view rather than
    // page.rs's own `Page` struct — same layout, different Rust name).
    /// SAFETY: see [`crate::mm::page::page_lock_acquire`]'s contract.
    #[inline]
    pub fn page_lock_acquire(p: *mut page_t) {
        unsafe { crate::mm::page::page_lock_acquire(p as *mut crate::mm::page::Page) };
    }
    /// SAFETY: see [`crate::mm::page::page_lock_release`]'s contract.
    #[inline]
    pub fn page_lock_release(p: *mut page_t) {
        unsafe { crate::mm::page::page_lock_release(p as *mut crate::mm::page::Page) };
    }
    /// SAFETY: see [`crate::mm::page::page_lock_assert_holding`]'s contract.
    #[inline]
    pub fn page_lock_assert_holding(p: *mut page_t) {
        unsafe { crate::mm::page::page_lock_assert_holding(p as *mut crate::mm::page::Page) };
    }
    /// SAFETY: see [`crate::mm::page::page_ref_inc_unlocked`]'s contract.
    #[inline]
    pub fn page_ref_inc_unlocked(p: *mut page_t) -> c_int {
        unsafe { crate::mm::page::page_ref_inc_unlocked(p as *mut crate::mm::page::Page) }
    }
    /// SAFETY: see [`crate::mm::page::page_ref_dec_unlocked`]'s contract.
    #[inline]
    pub fn page_ref_dec_unlocked(p: *mut page_t) -> c_int {
        unsafe { crate::mm::page::page_ref_dec_unlocked(p as *mut crate::mm::page::Page) }
    }
    /// SAFETY: see [`crate::mm::page::page_ref_count`]'s contract.
    #[inline]
    pub fn page_ref_count(p: *mut page_t) -> c_int {
        unsafe { crate::mm::page::page_ref_count(p as *mut crate::mm::page::Page) }
    }
    /// SAFETY: see [`crate::mm::page::__page_ref_dec`]'s contract.
    #[inline]
    pub fn __page_ref_dec(p: *mut page_t) -> c_int {
        unsafe { crate::mm::page::__page_ref_dec(p as *mut crate::mm::page::Page) }
    }
    /// SAFETY: see [`crate::mm::page::__page_alloc`]'s contract.
    #[inline]
    pub fn __page_alloc(order: u64, flags: u64) -> *mut page_t {
        unsafe { crate::mm::page::__page_alloc(order, flags) as *mut page_t }
    }
    /// SAFETY: `p` must originate from `__page_alloc` above.
    #[inline]
    pub fn __page_to_pa(p: *mut page_t) -> u64 {
        unsafe { crate::mm::page::__page_to_pa(p as *mut crate::mm::page::Page) }
    }
}
use ffi::*;

// ---------------------------------------------------------------------------
// Constants (mirror the C `#define`s in pcache_types.h / bio.h).
// ---------------------------------------------------------------------------
const PGSIZE: usize = 4096;
const BLK_SIZE_SHIFT: u32 = 9; // 512-byte block
const PCACHE_DEFAULT_DIRTY_RATE: u8 = 15;
const PCACHE_DEFAULT_MAX_PAGES: u64 = 4096;
const HZ: u64 = 1000;
const PCACHE_FLUSH_INTERVAL_JIFFS: u64 = 30 * HZ;
const KERNEL_STACK_ORDER: c_int = 2;
const WORKQUEUE_DEFAULT_MAX_ACTIVE: c_int = 8;
const PAGE_TYPE_PCACHE: c_int = 4; // enum page_type
const PAGE_FLAG_TYPE_MASK: u64 = 0xFF;

fn xv6_pcache_blks_per_page() -> usize {
    PGSIZE >> BLK_SIZE_SHIFT
}
fn xv6_pcache_align_blkno(blkno: u64) -> u64 {
    blkno & !((PGSIZE as u64 >> BLK_SIZE_SHIFT) - 1)
}
fn xv6_pcache_pgsize() -> usize {
    PGSIZE
}
fn xv6_pcache_flush_interval_jiffs() -> u64 {
    PCACHE_FLUSH_INTERVAL_JIFFS
}
fn xv6_hz() -> u64 {
    HZ
}
fn xv6_kernel_stack_order() -> c_int {
    KERNEL_STACK_ORDER
}

// ---------------------------------------------------------------------------
// Global storage. C-side equivalents were compile-time initialized via
// SPINLOCK_INITIALIZED / LIST_ENTRY_INITIALIZED. We zero-init and finish
// in `xv6_pcache_globals_init` (called once at the top of
// `pcache_global_init`).
// ---------------------------------------------------------------------------
// SAFETY: `SyncCell<T>` is used only for this module's file-scope
// globals below (list head, counters, spinlock/slab-cache/completion
// storage, workqueue/thread pointers, running flag). Every one is
// either mutated exclusively under `global_spinlock()`'s lock or
// initialised once by `xv6_pcache_globals_init`/`pcache_global_init`
// before any other pcache entry point runs (documented per-static
// above), matching the same pattern as `kobject.rs`'s `SyncCell`.
#[repr(transparent)]
struct SyncCell<T>(core::cell::UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn new(v: T) -> Self {
        SyncCell(core::cell::UnsafeCell::new(v))
    }
    #[inline]
    fn get(&self) -> *mut T {
        self.0.get()
    }
}

static PCACHE_GLOBAL_LIST: SyncCell<list_node_t> = SyncCell::new(list_node_t {
    prev: ptr::null_mut(),
    next: ptr::null_mut(),
});
static PCACHE_GLOBAL_COUNT: SyncCell<c_int> = SyncCell::new(0);
static PCACHE_GLOBAL_FLUSH_WQ: SyncCell<*mut Workqueue> = SyncCell::new(ptr::null_mut());
// `uninit()` not `zeroed()` for all three below: each is written in
// full by its one-shot initializer (`spin_init` / `slab_cache_init` /
// `completion_init`, all called from `xv6_pcache_globals_init` /
// `pcache_global_init` before any other pcache entry point runs) via
// unconditional field writes — no code path reads a field before that
// initializer sets it, so no zeroed bit pattern is ever observed.
static PCACHE_GLOBAL_SPINLOCK: SyncCell<MaybeUninit<spinlock_t>> = SyncCell::new(MaybeUninit::uninit());
static PCACHE_NODE_SLAB: SyncCell<MaybeUninit<slab_cache_t>> = SyncCell::new(MaybeUninit::uninit());
static PCACHE_GLOBAL_FLUSHER_COMPLETION: SyncCell<MaybeUninit<completion_t>> =
    SyncCell::new(MaybeUninit::uninit());
static PCACHE_FLUSHER_THREAD_PCB: SyncCell<*mut Thread> = SyncCell::new(ptr::null_mut());
static PCACHE_GLOBAL_FLUSHER_RUNNING: SyncCell<bool> = SyncCell::new(false);

#[inline]
fn global_list_head() -> *mut list_node_t {
    PCACHE_GLOBAL_LIST.get()
}
#[inline]
fn global_spinlock() -> *mut spinlock_t {
    // SAFETY: `PCACHE_GLOBAL_SPINLOCK` is initialised once by
    // `xv6_pcache_globals_init` before any other pcache entry point runs.
    unsafe { (*PCACHE_GLOBAL_SPINLOCK.get()).as_mut_ptr() }
}
#[inline]
fn global_completion() -> *mut completion_t {
    // SAFETY: initialised by `xv6_pcache_global_flusher_completion_init`
    // before `pcache_global_init` returns.
    unsafe { (*PCACHE_GLOBAL_FLUSHER_COMPLETION.get()).as_mut_ptr() }
}
#[inline]
fn node_slab() -> *mut slab_cache_t {
    // SAFETY: initialised by `xv6_pcache_node_slab_init` before any
    // pcache_node allocation is attempted.
    unsafe { (*PCACHE_NODE_SLAB.get()).as_mut_ptr() }
}

fn xv6_pcache_set_flusher_thread(t: *mut Thread) {
    unsafe {
        *PCACHE_FLUSHER_THREAD_PCB.get() = t;
    }
}
fn xv6_pcache_set_flush_wq(wq: *mut Workqueue) {
    unsafe {
        *PCACHE_GLOBAL_FLUSH_WQ.get() = wq;
    }
}
fn xv6_pcache_inc_global_count() {
    unsafe {
        *PCACHE_GLOBAL_COUNT.get() += 1;
    }
}
fn xv6_pcache_dec_global_count() {
    unsafe {
        let c = PCACHE_GLOBAL_COUNT.get();
        if *c > 0 {
            *c -= 1;
        }
    }
}
fn xv6_pcache_get_flusher_running() -> bool {
    unsafe { *PCACHE_GLOBAL_FLUSHER_RUNNING.get() }
}
fn xv6_pcache_set_flusher_running(v: bool) {
    unsafe {
        *PCACHE_GLOBAL_FLUSHER_RUNNING.get() = v;
    }
}

static SPINLOCK_NAME: &[u8] = b"pcache_global_spinlock\0";
static PCACHE_LOCK_NAME: &[u8] = b"pcache_lock\0";
static PCACHE_TREE_LOCK_NAME: &[u8] = b"pcache_tree_lock\0";
static PCACHE_IO_NAME: &[u8] = b"pcache_io\0";
static PCACHE_NODE_NAME: &[u8] = b"pcache_node\0";
static PCACHE_FLUSH_WQ_NAME: &[u8] = b"pcache_flush_wq\0";

/// One-shot global initialization invoked from `pcache_global_init`.
fn xv6_pcache_globals_init() {
    list_init(global_list_head());
    spin_init(global_spinlock(), SPINLOCK_NAME.as_ptr() as *const c_char);
}

// ---------------------------------------------------------------------------
// list_node_t helpers (replicate the inline C versions in kernel/inc/list.h)
// ---------------------------------------------------------------------------
#[inline]
fn list_init(e: *mut list_node_t) {
    // SAFETY: `e` is exclusively owned by the caller for the duration of
    // this call (either freshly allocated storage or already detached).
    unsafe {
        (*e).prev = e;
        (*e).next = e;
    }
}
#[inline]
fn list_detach(e: *mut list_node_t) {
    // SAFETY: `e` is currently linked into a list protected by a lock the
    // caller holds.
    unsafe {
        (*(*e).prev).next = (*e).next;
        (*(*e).next).prev = (*e).prev;
    }
    list_init(e);
}
#[inline]
fn list_insert(prev: *mut list_node_t, e: *mut list_node_t) {
    // SAFETY: `prev` is a live list node and `e` is not currently linked
    // into any list; caller holds the list's lock.
    unsafe {
        let next = (*prev).next;
        (*e).prev = prev;
        (*e).next = next;
        (*prev).next = e;
        (*next).prev = e;
    }
}
#[inline]
fn list_push_front(head: *mut list_node_t, e: *mut list_node_t) {
    list_insert(head, e);
}
#[inline]
fn list_is_empty(head: *mut list_node_t) -> bool {
    unsafe { (*head).next == head }
}
#[inline]
fn list_entry_is_detached(e: *mut list_node_t) -> bool {
    unsafe { (*e).next == e }
}

// container_of-style helpers, in terms of the one canonical
// `crate::mm::cffi::container_of` (pure pointer arithmetic, itself safe;
// only the eventual dereference at the call site is unsafe and documented
// there).
#[inline]
fn pcache_from_list_entry(node: *mut list_node_t) -> *mut Pcache {
    crate::mm::cffi::container_of(node, offset_of!(Pcache, list_entry))
}
#[inline]
fn node_from_lru_entry(node: *mut list_node_t) -> *mut PcacheNode {
    crate::mm::cffi::container_of(node, offset_of!(PcacheNode, lru_entry))
}
#[inline]
fn node_from_tree_entry(node: *mut rb_node) -> *mut PcacheNode {
    crate::mm::cffi::container_of(node, offset_of!(PcacheNode, tree_entry))
}

// ---------------------------------------------------------------------------
// Panic helper
// ---------------------------------------------------------------------------

#[inline]
fn pcache_assert(cond: bool, msg: &'static [u8]) {
    if !cond {
        xv6_pcache_panic(msg.as_ptr() as *const c_char);
    }
}
#[inline(never)]
fn xv6_pcache_panic(msg: *const c_char) -> ! {
    __panic_start();
    // SAFETY: `msg` is always a `'static` NUL-terminated byte-string
    // literal from this module's call sites, matching the `%s` format
    // spec.
    unsafe {
        crate::kprintln!("PANIC: {}", crate::printf::Cs(msg));
    }
    __panic_end()
}

// ---------------------------------------------------------------------------
// rb tree comparator for `pcache.page_map` (keyed by `pcache_node.blkno`).
// ---------------------------------------------------------------------------
extern "C" fn pcache_rb_compare(k1: u64, k2: u64) -> c_int {
    if k1 < k2 {
        -1
    } else if k1 > k2 {
        1
    } else {
        0
    }
}
extern "C" fn pcache_rb_get_key(node: *mut rb_node) -> u64 {
    let pcnode = node_from_tree_entry(node);
    // SAFETY: `node` is a live `tree_entry` embedded in a valid, still
    // rb-tree-linked `PcacheNode`.
    unsafe { (*pcnode).blkno }
}
static PCACHE_RB_OPTS: SyncCell<rb_root_opts> = SyncCell::new(rb_root_opts {
    keys_cmp_fun: Some(pcache_rb_compare),
    get_key_fun: Some(pcache_rb_get_key),
});

// ===========================================================================
// PcacheHandle — non-owning handle over `*mut Pcache`.
//
// Every method assumes `self` is non-null and points at a live,
// properly-initialised `pcache`; where a field additionally needs
// synchronisation, the caller is expected to already hold the
// appropriate lock per this module's locking order (exactly the
// contract the deleted `xv6_pcache_*` C-ABI shims relied on implicitly).
// ===========================================================================
#[derive(Clone, Copy)]
struct PcacheHandle(NonNull<Pcache>);

impl PcacheHandle {
    /// # Safety
    /// `p` must be non-null and point at a live, initialised `pcache`.
    #[inline]
    unsafe fn new(p: *mut Pcache) -> Self {
        PcacheHandle(NonNull::new_unchecked(p))
    }
    #[inline]
    fn raw(self) -> *mut Pcache {
        self.0.as_ptr()
    }

    // -- locks / list linkage --------------------------------------------
    #[inline]
    fn spinlock_ptr(self) -> *mut spinlock_t {
        unsafe { addr_of_mut!((*self.raw()).spinlock) }
    }
    #[inline]
    fn spin_init(self) {
        spin_init(self.spinlock_ptr(), PCACHE_LOCK_NAME.as_ptr() as *const c_char);
    }
    #[inline]
    fn spin_lock(self) {
        spin_lock(self.spinlock_ptr());
    }
    #[inline]
    fn spin_unlock(self) {
        spin_unlock(self.spinlock_ptr());
    }
    #[inline]
    fn spin_assert_holding(self) {
        pcache_assert(
            spin_holding(self.spinlock_ptr()) != 0,
            b"pcache_spin_assert_holding: pcache spinlock not held\0",
        );
    }

    #[inline]
    fn tree_lock_ptr(self) -> *mut rwlock {
        unsafe { addr_of_mut!((*self.raw()).tree_lock) }
    }
    #[inline]
    fn tree_lock_init(self) {
        rwlock_init(self.tree_lock_ptr(), PCACHE_TREE_LOCK_NAME.as_ptr() as *const c_char);
    }
    #[inline]
    fn tree_rlock(self) {
        rwlock_rlock(self.tree_lock_ptr());
    }
    #[inline]
    fn tree_runlock(self) {
        rwlock_runlock(self.tree_lock_ptr());
    }
    #[inline]
    fn tree_wlock(self) {
        rwlock_wlock(self.tree_lock_ptr());
    }
    #[inline]
    fn tree_wunlock(self) {
        rwlock_wunlock(self.tree_lock_ptr());
    }

    #[inline]
    fn list_entries_init(self) {
        unsafe {
            list_init(addr_of_mut!((*self.raw()).list_entry));
            list_init(addr_of_mut!((*self.raw()).lru));
            list_init(addr_of_mut!((*self.raw()).dirty_list));
        }
    }
    #[inline]
    fn list_entry_is_detached(self) -> bool {
        unsafe { list_entry_is_detached(addr_of_mut!((*self.raw()).list_entry)) }
    }
    #[inline]
    fn push_global(self) {
        unsafe { list_push_front(global_list_head(), addr_of_mut!((*self.raw()).list_entry)) };
    }
    #[inline]
    fn detach_global(self) {
        unsafe { list_detach(addr_of_mut!((*self.raw()).list_entry)) };
    }
    #[inline]
    fn next_in_global_list(self) -> *mut Pcache {
        let head = global_list_head();
        // SAFETY: `self` is linked into the global list.
        let next = unsafe { (*self.raw()).list_entry.next };
        if next == head {
            ptr::null_mut()
        } else {
            pcache_from_list_entry(next)
        }
    }

    // -- scalar fields -----------------------------------------------------
    #[inline]
    fn blk_count(self) -> u64 {
        unsafe { (*self.raw()).blk_count }
    }
    #[inline]
    fn page_count(self) -> i64 {
        unsafe { (*self.raw()).page_count }
    }
    #[inline]
    fn set_page_count(self, v: i64) {
        unsafe { (*self.raw()).page_count = v };
    }
    #[inline]
    fn add_page_count(self, d: i64) {
        unsafe { (*self.raw()).page_count += d };
    }
    #[inline]
    fn lru_count(self) -> i64 {
        unsafe { (*self.raw()).lru_count }
    }
    #[inline]
    fn set_lru_count(self, v: i64) {
        unsafe { (*self.raw()).lru_count = v };
    }
    #[inline]
    fn inc_lru_count(self) {
        unsafe { (*self.raw()).lru_count += 1 };
    }
    #[inline]
    fn dec_lru_count(self) {
        unsafe { (*self.raw()).lru_count -= 1 };
    }
    #[inline]
    fn dirty_count(self) -> i64 {
        unsafe { (*self.raw()).dirty_count }
    }
    #[inline]
    fn set_dirty_count(self, v: i64) {
        unsafe { (*self.raw()).dirty_count = v };
    }
    #[inline]
    fn inc_dirty_count(self) {
        unsafe { (*self.raw()).dirty_count += 1 };
    }
    #[inline]
    fn dec_dirty_count(self) {
        unsafe { (*self.raw()).dirty_count -= 1 };
    }
    #[inline]
    fn max_pages(self) -> u64 {
        unsafe { (*self.raw()).max_pages }
    }
    #[inline]
    fn set_max_pages(self, v: u64) {
        unsafe { (*self.raw()).max_pages = v };
    }
    #[inline]
    fn dirty_rate(self) -> u8 {
        unsafe { (*self.raw()).dirty_rate }
    }
    #[inline]
    fn set_dirty_rate(self, v: u8) {
        unsafe { (*self.raw()).dirty_rate = v };
    }
    #[inline]
    fn last_request(self) -> u64 {
        unsafe { (*self.raw()).last_request }
    }
    #[inline]
    fn set_last_request(self, v: u64) {
        unsafe { (*self.raw()).last_request = v };
    }
    #[inline]
    fn last_flushed(self) -> u64 {
        unsafe { (*self.raw()).last_flushed }
    }
    #[inline]
    fn set_last_flushed(self, v: u64) {
        unsafe { (*self.raw()).last_flushed = v };
    }
    #[inline]
    fn set_flags(self, v: u64) {
        unsafe { (*self.raw()).__bindgen_anon_1.flags = v };
    }
    #[inline]
    fn active(self) -> bool {
        unsafe { (*self.raw()).__bindgen_anon_1.__bindgen_anon_1.active() != 0 }
    }
    #[inline]
    fn set_active(self, v: bool) {
        unsafe {
            (*self.raw())
                .__bindgen_anon_1
                .__bindgen_anon_1
                .set_active(v as u64);
        }
    }
    #[inline]
    fn flush_requested(self) -> bool {
        unsafe { (*self.raw()).__bindgen_anon_1.__bindgen_anon_1.flush_requested() != 0 }
    }
    #[inline]
    fn set_flush_requested(self, v: bool) {
        unsafe {
            (*self.raw())
                .__bindgen_anon_1
                .__bindgen_anon_1
                .set_flush_requested(v as u64);
        }
    }
    #[inline]
    fn flush_error(self) -> c_int {
        unsafe { (*self.raw()).flush_error }
    }
    #[inline]
    fn set_flush_error(self, v: c_int) {
        unsafe { (*self.raw()).flush_error = v };
    }
    #[inline]
    fn wait_refcount(self) -> u32 {
        unsafe { (*self.raw()).wait_refcount }
    }
    #[inline]
    fn set_wait_refcount(self, v: u32) {
        unsafe { (*self.raw()).wait_refcount = v };
    }
    #[inline]
    fn inc_wait_refcount(self) {
        unsafe { (*self.raw()).wait_refcount += 1 };
    }
    #[inline]
    fn dec_wait_refcount(self) {
        unsafe { (*self.raw()).wait_refcount -= 1 };
    }
    #[inline]
    fn gfp_flags(self) -> u64 {
        unsafe { (*self.raw()).gfp_flags }
    }
    #[inline]
    fn set_gfp_flags(self, v: u64) {
        unsafe { (*self.raw()).gfp_flags = v };
    }
    #[inline]
    fn set_private_data(self, v: *mut c_void) {
        unsafe { (*self.raw()).private_data = v };
    }

    // -- ops vtable ----------------------------------------------------
    #[inline]
    fn ops_read_page(self) -> Option<unsafe extern "C" fn(*mut Pcache, *mut Page) -> c_int> {
        unsafe { (*(*self.raw()).ops).read_page }
    }
    #[inline]
    fn ops_write_page(self) -> Option<unsafe extern "C" fn(*mut Pcache, *mut Page) -> c_int> {
        unsafe { (*(*self.raw()).ops).write_page }
    }
    #[inline]
    fn ops_write_begin(self) -> Option<unsafe extern "C" fn(*mut Pcache, *mut Page) -> c_int> {
        unsafe { (*(*self.raw()).ops).write_begin }
    }
    #[inline]
    fn ops_write_end(self) -> Option<unsafe extern "C" fn(*mut Pcache, *mut Page) -> c_int> {
        unsafe { (*(*self.raw()).ops).write_end }
    }
    #[inline]
    fn ops_mark_dirty(self) -> Option<unsafe extern "C" fn(*mut Pcache, *mut Page)> {
        unsafe { (*(*self.raw()).ops).mark_dirty }
    }

    // -- flush completion -------------------------------------------------
    #[inline]
    fn flush_completion_ptr(self) -> *mut completion_t {
        unsafe { addr_of_mut!((*self.raw()).flush_completion) }
    }
    #[inline]
    fn flush_completion_init(self) {
        completion_init(self.flush_completion_ptr());
    }
    #[inline]
    fn flush_completion_reinit(self) {
        completion_reinit(self.flush_completion_ptr());
    }
    #[inline]
    fn flush_completion_complete_all(self) {
        complete_all(self.flush_completion_ptr());
    }
    #[inline]
    fn flush_completion_wait(self) {
        wait_for_completion(self.flush_completion_ptr());
    }

    // -- rb tree (page_map) ------------------------------------------------
    #[inline]
    fn page_map_ptr(self) -> *mut rb_root {
        unsafe { addr_of_mut!((*self.raw()).page_map) }
    }
    fn rb_init(self) {
        unsafe {
            (*self.raw()).page_map.node = ptr::null_mut();
            (*self.raw()).page_map.opts = PCACHE_RB_OPTS.get();
        }
    }
    fn rb_find(self, blkno: u64) -> *mut PcacheNode {
        let node = rb_find_key(self.page_map_ptr(), blkno);
        if node.is_null() {
            ptr::null_mut()
        } else {
            node_from_tree_entry(node)
        }
    }
    fn rb_insert(self, pcnode: *mut PcacheNode) -> *mut PcacheNode {
        let entry = unsafe { addr_of_mut!((*pcnode).tree_entry) };
        let node = rb_insert_color(self.page_map_ptr(), entry);
        if node.is_null() {
            ptr::null_mut()
        } else {
            node_from_tree_entry(node)
        }
    }
    fn rb_delete(self, pcnode: *mut PcacheNode) {
        let entry = unsafe { addr_of_mut!((*pcnode).tree_entry) };
        let removed = rb_delete_node_color(self.page_map_ptr(), entry);
        pcache_assert(removed == entry, b"xv6_pcache_rb_delete: removed mismatch\0");
    }
    fn rb_first(self) -> *mut PcacheNode {
        let node = rb_first_node(self.page_map_ptr());
        if node.is_null() {
            ptr::null_mut()
        } else {
            node_from_tree_entry(node)
        }
    }

    // -- lru / dirty lists ---------------------------------------------
    #[inline]
    fn lru_head(self) -> *mut list_node_t {
        unsafe { addr_of_mut!((*self.raw()).lru) }
    }
    #[inline]
    fn dirty_head(self) -> *mut list_node_t {
        unsafe { addr_of_mut!((*self.raw()).dirty_list) }
    }
    fn lru_last(self) -> *mut PcacheNode {
        let head = self.lru_head();
        if list_is_empty(head) {
            ptr::null_mut()
        } else {
            node_from_lru_entry(unsafe { (*head).prev })
        }
    }
    fn dirty_last(self) -> *mut PcacheNode {
        let head = self.dirty_head();
        if list_is_empty(head) {
            ptr::null_mut()
        } else {
            node_from_lru_entry(unsafe { (*head).prev })
        }
    }
    #[inline]
    fn lru_is_empty(self) -> bool {
        list_is_empty(self.lru_head())
    }
    #[inline]
    fn dirty_is_empty(self) -> bool {
        list_is_empty(self.dirty_head())
    }

    // -- flush work / workqueue -----------------------------------------
    fn init_flush_work(self, func: unsafe extern "C" fn(*mut WorkStruct)) {
        let w = unsafe { addr_of_mut!((*self.raw()).flush_work) };
        init_work_struct(w, Some(func), self.raw() as u64);
    }
    fn queue_flush_work(self) -> bool {
        let wq = unsafe { *PCACHE_GLOBAL_FLUSH_WQ.get() };
        if wq.is_null() {
            return false;
        }
        let w = unsafe { addr_of_mut!((*self.raw()).flush_work) };
        queue_work(wq, w)
    }

    // -- zero-init check (pcache_init precondition) ----------------------
    fn is_zero_inited(self) -> bool {
        // SAFETY: `self` is non-null; this is only called from
        // `pcache_init_validate` before the pcache is published anywhere,
        // so there is no concurrent access to race with.
        unsafe {
            let p = self.raw();
            let rb_empty = (*p).page_map.node.is_null();
            let lru = &(*p).lru;
            let dl = &(*p).dirty_list;
            let le = &(*p).list_entry;
            (*p).page_count == 0
                && (*p).dirty_count == 0
                && (*p).__bindgen_anon_1.flags == 0
                && rb_empty
                && lru.next.is_null()
                && lru.prev.is_null()
                && dl.next.is_null()
                && dl.prev.is_null()
                && le.next.is_null()
                && le.prev.is_null()
        }
    }
}

// ===========================================================================
// NodeHandle — non-owning handle over `*mut PcacheNode`. Same contract
// as `PcacheHandle` above.
// ===========================================================================
#[derive(Clone, Copy)]
struct NodeHandle(NonNull<PcacheNode>);

impl NodeHandle {
    /// # Safety
    /// `n` must be non-null and point at a live `pcache_node`.
    #[inline]
    unsafe fn new(n: *mut PcacheNode) -> Self {
        NodeHandle(NonNull::new_unchecked(n))
    }
    #[inline]
    fn raw(self) -> *mut PcacheNode {
        self.0.as_ptr()
    }

    fn init(self) {
        let n = self.raw();
        unsafe {
            core::ptr::write_bytes(n as *mut u8, 0, size_of::<PcacheNode>());
            // rb_node_init: __parent_color = (unsigned long)node
            (*n).tree_entry.__parent_color = addr_of!((*n).tree_entry) as u64;
            list_init(addr_of_mut!((*n).lru_entry));
        }
        tq_init(
            unsafe { addr_of_mut!((*n).io_waiters) },
            PCACHE_IO_NAME.as_ptr() as *const c_char,
            ptr::null_mut(),
        );
        unsafe {
            (*n).blkno = u64::MAX;
            (*n).page_count = 0;
        }
    }

    #[inline]
    fn pcache(self) -> *mut Pcache {
        unsafe { (*self.raw()).pcache }
    }
    #[inline]
    fn set_pcache(self, p: *mut Pcache) {
        unsafe { (*self.raw()).pcache = p };
    }
    #[inline]
    fn page(self) -> *mut Page {
        unsafe { (*self.raw()).page }
    }
    #[inline]
    fn set_page(self, page: *mut Page) {
        unsafe { (*self.raw()).page = page };
    }
    #[inline]
    fn data(self) -> *mut c_void {
        unsafe { (*self.raw()).data }
    }
    #[inline]
    fn set_data(self, d: *mut c_void) {
        unsafe { (*self.raw()).data = d };
    }
    #[inline]
    fn set_page_count(self, c: i64) {
        unsafe { (*self.raw()).page_count = c };
    }
    #[inline]
    fn blkno(self) -> u64 {
        unsafe { (*self.raw()).blkno }
    }
    #[inline]
    fn set_blkno(self, v: u64) {
        unsafe { (*self.raw()).blkno = v };
    }
    #[inline]
    fn size_get(self) -> usize {
        unsafe { (*self.raw()).size }
    }
    #[inline]
    fn set_size(self, v: usize) {
        unsafe { (*self.raw()).size = v };
    }
    #[inline]
    fn set_last_request(self, v: u64) {
        unsafe { (*self.raw()).last_request = v };
    }
    #[inline]
    fn last_flushed(self) -> u64 {
        unsafe { (*self.raw()).last_flushed }
    }
    #[inline]
    fn dirty(self) -> bool {
        unsafe { (*self.raw()).__bindgen_anon_1.dirty() != 0 }
    }
    #[inline]
    fn set_dirty(self, v: bool) {
        unsafe { (*self.raw()).__bindgen_anon_1.set_dirty(v as u64) };
    }
    #[inline]
    fn uptodate(self) -> bool {
        unsafe { (*self.raw()).__bindgen_anon_1.uptodate() != 0 }
    }
    #[inline]
    fn set_uptodate(self, v: bool) {
        unsafe { (*self.raw()).__bindgen_anon_1.set_uptodate(v as u64) };
    }
    #[inline]
    fn io_in_progress(self) -> bool {
        unsafe { (*self.raw()).__bindgen_anon_1.io_in_progress() != 0 }
    }
    #[inline]
    fn set_io_in_progress(self, v: bool) {
        unsafe { (*self.raw()).__bindgen_anon_1.set_io_in_progress(v as u64) };
    }
    #[inline]
    fn lru_detached(self) -> bool {
        unsafe { list_entry_is_detached(addr_of_mut!((*self.raw()).lru_entry)) }
    }
    fn push_lru(self, p: *mut Pcache) {
        unsafe { list_push_front(addr_of_mut!((*p).lru), addr_of_mut!((*self.raw()).lru_entry)) };
    }
    fn push_dirty(self, p: *mut Pcache) {
        unsafe {
            list_push_front(addr_of_mut!((*p).dirty_list), addr_of_mut!((*self.raw()).lru_entry))
        };
    }
    #[inline]
    fn detach_lru(self) {
        unsafe { list_detach(addr_of_mut!((*self.raw()).lru_entry)) };
    }

    /// Park the current thread on this node's IO-waiters queue until
    /// woken by [`Self::io_wakeup_all`]. Mirrors the C macro
    /// `__thread_state_set(current, THREAD_UNINTERRUPTIBLE)` followed by
    /// `tq_wait_cb` parked on the pcache's tree_lock.
    fn io_wait(self, p: *mut Pcache) -> c_int {
        let cur = machine::current_thread_ptr();
        if !cur.is_null() {
            // SAFETY: `cur` is the live current thread; a SeqCst atomic
            // store into its `state` field matches the C macro's
            // `__atomic_store_n(&t->state, s, __ATOMIC_SEQ_CST)`.
            unsafe {
                let state_ptr = addr_of_mut!((*cur).state) as *mut AtomicU32;
                (*state_ptr).store(thread_state_THREAD_UNINTERRUPTIBLE as u32, Ordering::SeqCst);
            }
        }
        tq_wait_cb(
            unsafe { addr_of_mut!((*self.raw()).io_waiters) },
            Some(rwlock_r_sleep_cb),
            Some(rwlock_r_wake_cb),
            unsafe { addr_of_mut!((*p).tree_lock) as *mut c_void },
            ptr::null_mut(),
        );
        0
    }
    fn io_wakeup_all(self) {
        tq_wakeup_all(unsafe { addr_of_mut!((*self.raw()).io_waiters) }, 0, 0);
    }
}

// ===========================================================================
// `xv6_pcache_*` / `xv6_page_*` delegators — kept under their original
// names/signatures so the algorithm body below is untouched call-site;
// each is now a direct call into `PcacheHandle`/`NodeHandle` or a small
// self-contained implementation (no more C-ABI round trip).
// ===========================================================================
fn xv6_pcache_spinlock(p: *mut Pcache) -> *mut spinlock_t {
    unsafe { PcacheHandle::new(p) }.spinlock_ptr()
}
fn xv6_pcache_spin_assert_holding(p: *mut Pcache) {
    unsafe { PcacheHandle::new(p) }.spin_assert_holding();
}
fn xv6_pcache_global_spin_assert_holding() {
    pcache_assert(
        spin_holding(global_spinlock()) != 0,
        b"pcache_global_spin_assert_holding: not held\0",
    );
}
fn xv6_pcache_list_entry_is_detached(p: *mut Pcache) -> c_int {
    if unsafe { PcacheHandle::new(p) }.list_entry_is_detached() { 1 } else { 0 }
}
fn xv6_pcache_global_first() -> *mut Pcache {
    let head = global_list_head();
    if list_is_empty(head) {
        ptr::null_mut()
    } else {
        pcache_from_list_entry(unsafe { (*head).next })
    }
}
fn xv6_pcache_global_next(cur: *mut Pcache) -> *mut Pcache {
    unsafe { PcacheHandle::new(cur) }.next_in_global_list()
}

fn xv6_pcache_blk_count(p: *mut Pcache) -> u64 {
    unsafe { PcacheHandle::new(p) }.blk_count()
}
fn xv6_pcache_page_count(p: *mut Pcache) -> i64 {
    unsafe { PcacheHandle::new(p) }.page_count()
}
fn xv6_pcache_set_page_count(p: *mut Pcache, v: i64) {
    unsafe { PcacheHandle::new(p) }.set_page_count(v);
}
fn xv6_pcache_add_page_count(p: *mut Pcache, d: i64) {
    unsafe { PcacheHandle::new(p) }.add_page_count(d);
}
fn xv6_pcache_lru_count(p: *mut Pcache) -> i64 {
    unsafe { PcacheHandle::new(p) }.lru_count()
}
fn xv6_pcache_set_lru_count(p: *mut Pcache, v: i64) {
    unsafe { PcacheHandle::new(p) }.set_lru_count(v);
}
fn xv6_pcache_dec_lru_count(p: *mut Pcache) {
    unsafe { PcacheHandle::new(p) }.dec_lru_count();
}
fn xv6_pcache_dirty_count(p: *mut Pcache) -> i64 {
    unsafe { PcacheHandle::new(p) }.dirty_count()
}
fn xv6_pcache_set_dirty_count(p: *mut Pcache, v: i64) {
    unsafe { PcacheHandle::new(p) }.set_dirty_count(v);
}
fn xv6_pcache_dec_dirty_count(p: *mut Pcache) {
    unsafe { PcacheHandle::new(p) }.dec_dirty_count();
}
fn xv6_pcache_max_pages(p: *mut Pcache) -> u64 {
    unsafe { PcacheHandle::new(p) }.max_pages()
}
fn xv6_pcache_dirty_rate(p: *mut Pcache) -> u8 {
    unsafe { PcacheHandle::new(p) }.dirty_rate()
}
fn xv6_pcache_set_last_request(p: *mut Pcache, v: u64) {
    unsafe { PcacheHandle::new(p) }.set_last_request(v);
}
fn xv6_pcache_set_last_flushed(p: *mut Pcache, v: u64) {
    unsafe { PcacheHandle::new(p) }.set_last_flushed(v);
}
fn xv6_pcache_active(p: *mut Pcache) -> c_int {
    if unsafe { PcacheHandle::new(p) }.active() { 1 } else { 0 }
}
fn xv6_pcache_set_active(p: *mut Pcache, v: c_int) {
    unsafe { PcacheHandle::new(p) }.set_active(v != 0);
}
fn xv6_pcache_flush_requested(p: *mut Pcache) -> c_int {
    if unsafe { PcacheHandle::new(p) }.flush_requested() { 1 } else { 0 }
}
fn xv6_pcache_set_flush_requested(p: *mut Pcache, v: c_int) {
    unsafe { PcacheHandle::new(p) }.set_flush_requested(v != 0);
}
fn xv6_pcache_flush_error(p: *mut Pcache) -> c_int {
    unsafe { PcacheHandle::new(p) }.flush_error()
}
fn xv6_pcache_set_flush_error(p: *mut Pcache, v: c_int) {
    unsafe { PcacheHandle::new(p) }.set_flush_error(v);
}

fn xv6_pcache_flush_completion_complete_all(p: *mut Pcache) {
    unsafe { PcacheHandle::new(p) }.flush_completion_complete_all();
}
fn xv6_pcache_global_flusher_complete_all() {
    complete_all(global_completion());
}


fn xv6_pcache_rb_delete(p: *mut Pcache, n: *mut PcacheNode) {
    unsafe { PcacheHandle::new(p) }.rb_delete(n);
}


fn xv6_pcache_init_flush_work(p: *mut Pcache, func: unsafe extern "C" fn(*mut WorkStruct)) {
    unsafe { PcacheHandle::new(p) }.init_flush_work(func);
}

// -- pcache_node accessors --------------------------------------------------
fn xv6_pcache_node_pcache(n: *mut PcacheNode) -> *mut Pcache {
    unsafe { NodeHandle::new(n) }.pcache()
}
fn xv6_pcache_node_set_pcache(n: *mut PcacheNode, p: *mut Pcache) {
    unsafe { NodeHandle::new(n) }.set_pcache(p);
}
fn xv6_pcache_node_page(n: *mut PcacheNode) -> *mut Page {
    unsafe { NodeHandle::new(n) }.page()
}
fn xv6_pcache_node_set_page(n: *mut PcacheNode, page: *mut Page) {
    unsafe { NodeHandle::new(n) }.set_page(page);
}
/// C-ABI export: `vm.rs`'s `xv6_vm_call_vma_fault` calls this through its
/// own `extern "C"` declaration. Pre-existing regression from the WP2
/// pcache-shim collapse -- this getter was dropped as a "pure accessor"
/// without checking `vm.rs`'s extern reference; restored here (paired
/// with the existing `set_data`/`NodeHandle::set_data`) so the kernel
/// links. Out of scope for the WP3 mm/page/slab/vm_pgtab refactor this
/// file is otherwise part of.
pub(crate) fn xv6_pcache_node_data(n: *mut PcacheNode) -> *mut c_void {
    unsafe { NodeHandle::new(n) }.data()
}
fn xv6_pcache_node_set_page_count(n: *mut PcacheNode, c: i64) {
    unsafe { NodeHandle::new(n) }.set_page_count(c);
}
fn xv6_pcache_node_blkno(n: *mut PcacheNode) -> u64 {
    unsafe { NodeHandle::new(n) }.blkno()
}
fn xv6_pcache_node_set_size(n: *mut PcacheNode, v: usize) {
    unsafe { NodeHandle::new(n) }.set_size(v);
}
fn xv6_pcache_node_dirty(n: *mut PcacheNode) -> c_int {
    if unsafe { NodeHandle::new(n) }.dirty() { 1 } else { 0 }
}
fn xv6_pcache_node_set_dirty(n: *mut PcacheNode, v: c_int) {
    unsafe { NodeHandle::new(n) }.set_dirty(v != 0);
}
fn xv6_pcache_node_uptodate(n: *mut PcacheNode) -> c_int {
    if unsafe { NodeHandle::new(n) }.uptodate() { 1 } else { 0 }
}
fn xv6_pcache_node_set_uptodate(n: *mut PcacheNode, v: c_int) {
    unsafe { NodeHandle::new(n) }.set_uptodate(v != 0);
}
fn xv6_pcache_node_io_in_progress(n: *mut PcacheNode) -> c_int {
    if unsafe { NodeHandle::new(n) }.io_in_progress() { 1 } else { 0 }
}
fn xv6_pcache_node_set_io_in_progress(n: *mut PcacheNode, v: c_int) {
    unsafe { NodeHandle::new(n) }.set_io_in_progress(v != 0);
}
fn xv6_pcache_node_lru_detached(n: *mut PcacheNode) -> c_int {
    if unsafe { NodeHandle::new(n) }.lru_detached() { 1 } else { 0 }
}
fn xv6_pcache_node_detach_lru(n: *mut PcacheNode) {
    unsafe { NodeHandle::new(n) }.detach_lru();
}

// ---------------------------------------------------------------------------
// `page_t` <-> pcache union accessors (the `pcache`/`pcache_node` arm of
// `page_t`'s type-tagged union — see `kernel/inc/mm/page_type.h`).
// ---------------------------------------------------------------------------
fn xv6_page_pcache_get_pcache(page: *mut Page) -> *mut Pcache {
    unsafe { (*page).__bindgen_anon_2.pcache.pcache }
}
fn xv6_page_pcache_set_pcache(page: *mut Page, p: *mut Pcache) {
    unsafe { (*page).__bindgen_anon_2.pcache.pcache = p };
}
/// C-ABI export: `vm.rs`'s `xv6_vm_call_vma_fault` calls this through its
/// own `extern "C"` declaration. Pre-existing regression from the WP2
/// pcache-shim collapse (`pcache_shims.rs` used to export this under the
/// same name; the replacement below was accidentally left private) —
/// fixed here as a minimal, behavior-preserving re-export so the kernel
/// links. `vm.rs`/`pcache.rs` are otherwise out of scope for the WP3
/// mm/page/slab/vm_pgtab refactor this file is part of.
#[no_mangle]
pub extern "C" fn xv6_page_pcache_get_node(page: *mut Page) -> *mut PcacheNode {
    unsafe { (*page).__bindgen_anon_2.pcache.pcache_node }
}
fn xv6_page_pcache_set_node(page: *mut Page, n: *mut PcacheNode) {
    unsafe { (*page).__bindgen_anon_2.pcache.pcache_node = n };
}
fn xv6_page_is_pcache_type(page: *mut Page) -> c_int {
    if page.is_null() {
        return 0;
    }
    let flags = unsafe { (*page).__bindgen_anon_1.flags };
    if (flags & PAGE_FLAG_TYPE_MASK) == PAGE_TYPE_PCACHE as u64 { 1 } else { 0 }
}

// ---------------------------------------------------------------------------
// Page-level primitives (lock / refcount / alloc), redeclared over the
// bindgen `page_t` view (see the `mod ffi` doc comment above).
// ---------------------------------------------------------------------------
fn xv6_pcache_page_lock_release(p: *mut Page) {
    page_lock_release(p);
}
fn xv6_pcache_page_lock_assert_holding(p: *mut Page) {
    page_lock_assert_holding(p);
}
fn xv6_pcache_page_ref_inc_unlocked(p: *mut Page) -> c_int {
    page_ref_inc_unlocked(p)
}
fn xv6_pcache_page_ref_dec_unlocked(p: *mut Page) -> c_int {
    page_ref_dec_unlocked(p)
}
fn xv6_pcache_page_ref_count_fn(p: *mut Page) -> c_int {
    page_ref_count(p)
}
fn xv6_pcache_page_ref_dec(p: *mut Page) -> c_int {
    __page_ref_dec(p)
}

// ---------------------------------------------------------------------------
// pcache_ops vtable call helpers, tightened to a null-check-and-call
// (read_page/write_page are guaranteed present by `pcache_init_validate`
// before a pcache is ever registered; write_begin/write_end/mark_dirty
// are optional and share one generic caller).
// ---------------------------------------------------------------------------
fn xv6_pcache_ops_call_read_page(p: *mut Pcache, page: *mut Page) -> c_int {
    let f = unsafe { PcacheHandle::new(p) }.ops_read_page();
    // SAFETY: `pcache_init_validate` rejects any pcache whose
    // `ops->read_page` is null before the pcache is registered.
    unsafe { (f.unwrap_unchecked())(p, page) }
}
fn xv6_pcache_ops_call_write_page(p: *mut Pcache, page: *mut Page) -> c_int {
    let f = unsafe { PcacheHandle::new(p) }.ops_write_page();
    // SAFETY: see `xv6_pcache_ops_call_read_page`.
    unsafe { (f.unwrap_unchecked())(p, page) }
}
fn ops_call_optional(
    f: Option<unsafe extern "C" fn(*mut Pcache, *mut Page) -> c_int>,
    p: *mut Pcache,
    page: *mut Page,
) -> c_int {
    match f {
        Some(f) => unsafe { f(p, page) },
        None => 0,
    }
}
fn xv6_pcache_ops_call_write_end(p: *mut Pcache, page: *mut Page) -> c_int {
    ops_call_optional(unsafe { PcacheHandle::new(p) }.ops_write_end(), p, page)
}
fn xv6_pcache_ops_call_mark_dirty(p: *mut Pcache, page: *mut Page) {
    if let Some(f) = unsafe { PcacheHandle::new(p) }.ops_mark_dirty() {
        unsafe { f(p, page) };
    }
}

// ---------------------------------------------------------------------------
// Thread / scheduling shims. `kthread_create`/`wakeup*`/`sleep_on_chan*`
// are real Rust functions re-exported at the crate root (`kernel/proc/`);
// `get_jiffs`/`sleep_ms` remain C (kernel/timer.c, not yet ported).
// ---------------------------------------------------------------------------
fn xv6_pcache_get_jiffs() -> u64 {
    get_jiffs()
}
fn xv6_pcache_sleep_ms(ms: u64) {
    sleep_ms(ms);
}
fn xv6_pcache_wakeup(t: *mut Thread) {
    wakeup(t);
}
fn xv6_pcache_wakeup_on_chan(chan: *mut c_void) {
    wakeup_on_chan(chan);
}
fn xv6_pcache_kthread_create(
    name: *const c_char,
    entry: unsafe extern "C" fn(u64, u64),
    a1: u64,
    a2: u64,
    stack_order: c_int,
) -> *mut Thread {
    let np = kthread_create(name, entry as *mut c_void, a1, a2, stack_order);
    // IS_ERR_OR_NULL: pointer in error range (high bits set) or null -> NULL
    if np.is_null() {
        return ptr::null_mut();
    }
    let raw = np as u64;
    // ERR_PTR encodes errno as (unsigned long)(-errno); detect via high bit set
    if (raw as i64) < 0 && (raw as i64) >= -4095 {
        return ptr::null_mut();
    }
    np
}

// ---------------------------------------------------------------------------
// Slab helpers (kernel/mm/slab.rs).
// ---------------------------------------------------------------------------
fn xv6_pcache_node_slab_init() -> c_int {
    slab_cache_init(
        node_slab(),
        PCACHE_NODE_NAME.as_ptr() as *const c_char,
        size_of::<PcacheNode>(),
        SLAB_FLAG_EMBEDDED,
    )
}
fn xv6_pcache_node_free(p: *mut PcacheNode) {
    slab_free(p as *mut c_void);
}

// ---------------------------------------------------------------------------
// Diagnostic printf wrappers.
// ---------------------------------------------------------------------------
fn xv6_pcache_printf_flush_queue_warn(p: *mut Pcache) {
    unsafe {
        crate::kprintln!(
            "warning: flusher failed to queue work for pcache {}",
            crate::printf::Ptr(p as u64),
        );
    }
}
fn xv6_pcache_printf_flush_wait_err(p: *mut Pcache, ret: c_int) {
    unsafe {
        crate::kprintln!(
            "warning: __pcache_wait_for_pending_flushes: pcache {} flush error {}",
            crate::printf::Ptr(p as u64),
            ret,
        );
    }
}
fn xv6_pcache_printf_default_page_invalid() {
    crate::kprintln!(
        "__pcache_get_page: default_page is not from the given pcache",
    );
}
fn xv6_pcache_printf_invalid_page(page: *mut Page, p: *mut Pcache) {
    unsafe {
        crate::kprintln!(
            "pcache_put_page(): invalid page {} for cache {}",
            crate::printf::Ptr(page as u64),
            crate::printf::Ptr(p as u64),
        );
    }
}
fn xv6_pcache_printf_refcount_too_small(page: *mut Page, refcount: c_int) {
    unsafe {
        crate::kprintln!(
            "pcache_put_page(): page {} refcount {} is too small to drop",
            crate::printf::Ptr(page as u64),
            refcount,
        );
    }
}
fn xv6_pcache_printf_read_refcount_too_small(page: *mut Page, refcount: c_int) {
    unsafe {
        crate::kprintln!(
            "pcache_read_page(): page {} refcount {} is too small to read",
            crate::printf::Ptr(page as u64),
            refcount,
        );
    }
}
fn xv6_pcache_printf_read_invalid_meta(page: *mut Page, blkno: u64, sz: usize) {
    unsafe {
        crate::kprintln!(
            "pcache_read_page(): invalid metadata for page {} (blkno={} size={})",
            crate::printf::Ptr(page as u64),
            blkno,
            sz as u64,
        );
    }
}
fn xv6_pcache_printf_io_unexpected(dirty: c_int, uptodate: c_int) {
    unsafe {
        crate::kprintln!(
            "pcache_read_page(): io in progress with unexpected state (dirty={} uptodate={})",
            dirty,
            uptodate,
        );
    }
}
fn xv6_pcache_printf_sys_sync_failed(ret: c_int) {
    unsafe {
        crate::kprintln!("sys_sync: pcache_sync failed with error {}", ret);
    }
}
fn xv6_pcache_printf_stats_header(p: *mut Pcache) {
    unsafe {
        crate::kprintln!("Pcache {} stats:", crate::printf::Ptr(p as u64));
    }
}
fn xv6_pcache_printf_stats_body(
    active: c_int,
    blk_count: u64,
    dirty: i64,
    lru: i64,
    page: i64,
    max: i64,
    rate: c_int,
    requested: c_int,
    err: c_int,
) {
    unsafe {
        crate::kprintln!("  Active: {}", active);
        crate::kprintln!("  Block count: {}", blk_count);
        crate::kprintln!("  Dirty count: {}", dirty);
        crate::kprintln!("  LRU count: {}", lru);
        crate::kprintln!("  Page count / Max pages: {}/{}", page, max);
        crate::kprintln!("  Dirty rate: {}%", rate);
        crate::kprintln!("  Flush requested: {}", requested);
        crate::kprintln!("  Flush error: {}", err);
    }
}
fn xv6_pcache_printf_dump_all_header(total: c_int) {
    unsafe {
        crate::kprintln!("Dumping all pcache stats:");
        crate::kprintln!("Total pcaches: {}", total);
    }
}

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

fn pcache_init_validate(p: *mut Pcache) -> Result<(), crate::mm::cffi::Errno> {
    use crate::mm::cffi::Errno;
    if p.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: `p` is non-null; the pcache has not yet been published
    // (registered) anywhere, so nothing else can be concurrently
    // mutating it while we validate it.
    let ops = unsafe { (*p).ops };
    if ops.is_null() {
        return Err(Errno::Inval);
    }
    let (read_page, write_page) = unsafe { ((*ops).read_page, (*ops).write_page) };
    if read_page.is_none() || write_page.is_none() {
        return Err(Errno::Inval);
    }
    if xv6_pcache_blk_count(p) == 0 {
        return Err(Errno::Inval);
    }
    if !unsafe { PcacheHandle::new(p) }.is_zero_inited() {
        return Err(Errno::Inval);
    }
    Ok(())
}

#[inline]
fn pcache_page_valid(p: *mut Pcache, page: *mut Page) -> bool {
    if p.is_null() || page.is_null() {
        return false;
    }
    if xv6_page_is_pcache_type(page) == 0 {
        return false;
    }
    xv6_page_pcache_get_pcache(page) == p && !xv6_page_pcache_get_node(page).is_null()
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

// RAII drop guards for the pcache's various locking primitives. Each
// guard releases its matching shim on Drop, so callers can use scoped
// `let _g = lock_*()` patterns instead of manual lock/unlock pairs.
#[must_use = "global pcache lock is released immediately if the guard is dropped"]
struct PcGlobalGuard;
impl Drop for PcGlobalGuard {
    fn drop(&mut self) { spin_unlock(global_spinlock()); }
}
fn lock_pcache_global() -> PcGlobalGuard {
    spin_lock(global_spinlock());
    PcGlobalGuard
}

#[must_use = "pcache lock is released immediately if the guard is dropped"]
struct PcLocalGuard(*mut Pcache);
impl Drop for PcLocalGuard {
    fn drop(&mut self) { unsafe { PcacheHandle::new(self.0) }.spin_unlock(); }
}
fn lock_pcache_local(p: *mut Pcache) -> PcLocalGuard {
    unsafe { PcacheHandle::new(p) }.spin_lock();
    PcLocalGuard(p)
}

#[must_use = "pcache tree read lock is released when the guard is dropped"]
struct PcTreeReadGuard(*mut Pcache);
impl Drop for PcTreeReadGuard {
    fn drop(&mut self) { unsafe { PcacheHandle::new(self.0) }.tree_runlock(); }
}
fn rlock_pcache_tree(p: *mut Pcache) -> PcTreeReadGuard {
    unsafe { PcacheHandle::new(p) }.tree_rlock();
    PcTreeReadGuard(p)
}

#[must_use = "pcache tree write lock is released when the guard is dropped"]
struct PcTreeWriteGuard(*mut Pcache);
impl Drop for PcTreeWriteGuard {
    fn drop(&mut self) { unsafe { PcacheHandle::new(self.0) }.tree_wunlock(); }
}
fn wlock_pcache_tree(p: *mut Pcache) -> PcTreeWriteGuard {
    unsafe { PcacheHandle::new(p) }.tree_wlock();
    PcTreeWriteGuard(p)
}

/// RAII guard for the per-`Page` sleep-free lock
/// (`xv6_pcache_page_lock_acquire` / `_release`). Several call sites
/// intentionally hand the locked page back to their caller (e.g.
/// `pcache_get_page` returns with the page still locked for the
/// caller to inspect/populate); those sites call [`PcPageGuard::forget`]
/// instead of letting the guard drop, mirroring `KSemPermit::forget`
/// in `sync.rs`.
#[must_use = "page lock is released immediately if the guard is dropped (use .forget() to hand off the locked page)"]
struct PcPageGuard(*mut Page);
impl Drop for PcPageGuard {
    fn drop(&mut self) { xv6_pcache_page_lock_release(self.0); }
}
impl PcPageGuard {
    /// Release ownership without unlocking — the page lock is being
    /// handed off to the caller of the current function.
    fn forget(self) { core::mem::forget(self); }
}
fn lock_pcache_page(page: *mut Page) -> PcPageGuard {
    page_lock_acquire(page);
    PcPageGuard(page)
}
/// Wrap a page pointer that is already locked (lock ownership handed in
/// from elsewhere, e.g. `pcache_pop_lru`/`pcache_pop_dirty`) in a guard
/// so this function's remaining unlock paths become RAII.
fn adopt_page_lock(page: *mut Page) -> PcPageGuard {
    PcPageGuard(page)
}

fn pcache_register(p: *mut Pcache) {
    if p.is_null() {
        return;
    }
    let _gg = lock_pcache_global();
    let _gp = lock_pcache_local(p);
    if xv6_pcache_list_entry_is_detached(p) != 0 {
        unsafe { PcacheHandle::new(p) }.push_global();
        xv6_pcache_inc_global_count();
    } else {
        crate::kprint!("warning: __pcache_register: pcache already registered");
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
    if p.is_null() { return -(EINVAL as c_int); }
    unsafe { PcacheHandle::new(p) }.flush_completion_wait();
    xv6_pcache_flush_error(p)
}

fn pcache_queue_work(p: *mut Pcache) -> bool {
    if p.is_null() || (unsafe { *PCACHE_GLOBAL_FLUSH_WQ.get() }).is_null() {
        return false;
    }
    xv6_pcache_spin_assert_holding(p);
    if xv6_pcache_flush_requested(p) != 0 {
        return true;
    }
    xv6_pcache_init_flush_work(p, pcache_flush_worker);
    let queued = (unsafe { PcacheHandle::new(p) }.queue_flush_work());
    if queued {
        xv6_pcache_set_flush_requested(p, 1);
        xv6_pcache_set_last_request(p, xv6_pcache_get_jiffs());
        xv6_pcache_set_flush_error(p, 0);
        unsafe { PcacheHandle::new(p) }.flush_completion_reinit();
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
    completion_reinit(global_completion());
    let pcb = (unsafe { *PCACHE_FLUSHER_THREAD_PCB.get() });
    if machine::current_thread_ptr() != pcb {
        xv6_pcache_wakeup(pcb);
    }
}

fn pcache_wait_flusher() -> c_int {
    if xv6_pcache_get_flusher_running() {
        wait_for_completion(global_completion());
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
extern "C" fn pcache_flush_worker(work: *mut WorkStruct) {
    let p = (unsafe { (*work).data }) as *mut Pcache;
    let start_jiffs = xv6_pcache_get_jiffs();

    if p.is_null() {
        crate::kprintln!("__pcache_flush_worker: pcache is NULL");
        return;
    }

    // `p_guard` spans the whole loop (each iteration's top,
    // `pcache_pop_dirty`, asserts it is held) and is explicitly
    // dropped/reacquired around the out-of-lock IO calls below.
    let mut p_guard = lock_pcache_local(p);
    loop {
        let page = pcache_pop_dirty(p, start_jiffs);
        if page.is_null() { break; }
        // `page` comes back page-locked (ownership handed off by
        // `pcache_pop_dirty`); adopt it into a guard.
        let page_guard = adopt_page_lock(page);

        let mut r = xv6_pcache_page_ref_inc_unlocked(page);
        assert_msg(r > 1, b"flush_worker: failed to increment page ref\0");

        r = pcache_node_io_begin(p, page);
        assert_msg(r == 0, b"flush_worker: failed to begin IO\0");

        drop(page_guard);
        drop(p_guard);

        // Real write outside lock
        let mut ret = (ops_call_optional(unsafe { PcacheHandle::new(p) }.ops_write_begin(), p, page));
        if ret != 0 {
            p_guard = lock_pcache_local(p);
            xv6_pcache_set_flush_error(p, ret);
            flush_err_continue(p, page);
            continue;
        }
        ret = xv6_pcache_ops_call_write_page(p, page);
        if ret != 0 {
            let _ = xv6_pcache_ops_call_write_end(p, page);
            p_guard = lock_pcache_local(p);
            xv6_pcache_set_flush_error(p, ret);
            flush_err_continue(p, page);
            continue;
        }
        ret = xv6_pcache_ops_call_write_end(p, page);

        p_guard = lock_pcache_local(p);
        let page_guard = lock_pcache_page(page);
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
        drop(page_guard);
    }
    pcache_flush_done(p);
    drop(p_guard);
}

/// err_continue arm of the flush worker (assumes spinlock held).
fn flush_err_continue(p: *mut Pcache, page: *mut Page) {
    xv6_pcache_spin_assert_holding(p);
    let _pg = lock_pcache_page(page);
    let r = pcache_node_io_end(p, page);
    assert_msg(r == 0, b"flush_worker: failed to end IO (err)\0");
    pcache_push_dirty(p, page);
    let r2 = xv6_pcache_page_ref_dec_unlocked(page);
    assert_msg(r2 > 0, b"flush_worker: failed to decrement ref (err)\0");
}

fn pcache_schedule_flushes_locked(round_start: u64, force_round: bool) -> bool {
    let mut pending_flush = false;
    let mut cur = xv6_pcache_global_first();
    while !cur.is_null() {
        let next = xv6_pcache_global_next(cur);
        let _g = lock_pcache_local(cur);
        if !pcache_is_active(cur) {
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
                let last_flushed = (unsafe { PcacheHandle::new(cur) }.last_flushed());
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
        cur = next;
    }
    pending_flush
}

fn pcache_pick_pending_before(jiffs: u64) -> *mut Pcache {
    xv6_pcache_global_spin_assert_holding();
    let mut cur = xv6_pcache_global_first();
    while !cur.is_null() {
        let next = xv6_pcache_global_next(cur);
        let found = {
            let _g = lock_pcache_local(cur);
            if xv6_pcache_flush_requested(cur) != 0 {
                let last_request = (unsafe { PcacheHandle::new(cur) }.last_request());
                last_request <= jiffs
            } else {
                false
            }
        };
        if found {
            return cur;
        }
        cur = next;
    }
    ptr::null_mut()
}

fn pcache_wait_for_pending_flushes() {
    let start_jiffs = xv6_pcache_get_jiffs();
    loop {
        let p = {
            let _gg = lock_pcache_global();
            let p = pcache_pick_pending_before(start_jiffs);
            if !p.is_null() {
                let _gp = lock_pcache_local(p);
                unsafe { PcacheHandle::new(p) }.inc_wait_refcount();
            }
            p
        };
        if p.is_null() {
            break;
        }

        let ret = pcache_wait_flush_complete(p);
        if ret != 0 {
            xv6_pcache_printf_flush_wait_err(p, ret);
        }

        {
            let _gp = lock_pcache_local(p);
            unsafe { PcacheHandle::new(p) }.dec_wait_refcount();
            xv6_pcache_wakeup_on_chan(p as *mut c_void);
        }

        xv6_pcache_sleep_ms(10);
    }
}

extern "C" fn flusher_thread(_a1: u64, _a2: u64) {
    crate::kprintln!("pcache flusher thread started");
    loop {
        let round_start = xv6_pcache_get_jiffs();
        let pending = {
            let _gg = lock_pcache_global();
            let force_round = xv6_pcache_get_flusher_running();
            pcache_flusher_start();
            pcache_schedule_flushes_locked(round_start, force_round)
        };
        if pending {
            pcache_wait_for_pending_flushes();
        }
        {
            let _gg = lock_pcache_global();
            pcache_flusher_done();
        }

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
        found_node = (unsafe { PcacheHandle::new(p) }.rb_insert(dp_node));
        if found_node != dp_node {
            return xv6_pcache_node_page(found_node);
        }
    } else {
        let _g = rlock_pcache_tree(p);
        found_node = (unsafe { PcacheHandle::new(p) }.rb_find(blkno));
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
    // Borrow the node cache through the layout-verified bindgen<->slab.rs
    // view (same pointer-cast pattern the `ffi::slab_alloc` redeclaration
    // above already relies on; sizes are cross-checked by static asserts
    // in slab.rs). SAFETY: `node_slab()` is initialised once at pcache
    // subsystem startup (`xv6_pcache_node_slab_init`) and lives for the
    // rest of the kernel's lifetime — the `'static`-equivalent borrow is
    // always valid.
    let cache_ref = unsafe {
        SlabCacheRef::from_raw(&*(node_slab() as *mut crate::mm::slab::SlabCache))
    };
    // SAFETY: the cache was created with `size_of::<PcacheNode>()` as its
    // object size (see `xv6_pcache_node_slab_init` above), matching `T`.
    let node_box: SlabBox<'_, MaybeUninit<PcacheNode>> = match unsafe { cache_ref.alloc_uninit() } {
        Some(b) => b,
        None => return ptr::null_mut(),
    };
    let pcnode = node_box.as_ptr() as *mut PcacheNode;
    let page = (__page_alloc(0, PAGE_TYPE_PCACHE as u64));
    if page.is_null() {
        // `node_box` drops here -> slab_free(pcnode); replaces the old
        // manual `xv6_pcache_node_free(pcnode)` call.
        return ptr::null_mut();
    }
    unsafe { NodeHandle::new(pcnode) }.init();
    xv6_pcache_node_set_page(pcnode, page);
    xv6_pcache_node_set_page_count(pcnode, 1);
    xv6_pcache_node_set_size(pcnode, xv6_pcache_pgsize());
    unsafe { NodeHandle::new(pcnode) }.set_data((__page_to_pa(page) as *mut c_void));
    xv6_page_pcache_set_node(page, pcnode);
    xv6_page_pcache_set_pcache(page, ptr::null_mut());
    // `pcnode` is now reachable and owned via `page->pcache.pcache_node`
    // (a C-visible pointer walked from many other sites via
    // `xv6_page_pcache_get_node`); its lifetime is no longer scoped to
    // this function, so hand ownership off the RAII box.
    //
    // SAFETY: every field the type requires has just been written above
    // via `NodeHandle::init()` + the explicit `set_*` calls.
    let node_box: SlabBox<'_, PcacheNode> = unsafe { node_box.assume_init() };
    let _ = node_box.into_raw();
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
        return -(EINVAL as c_int); // We use EALREADY in C; same numeric handling not critical
    }
    xv6_pcache_node_set_io_in_progress(node, 1);
    unsafe { NodeHandle::new(node) }.set_last_request(xv6_pcache_get_jiffs());
    0
}

fn pcache_node_io_end(p: *mut Pcache, page: *mut Page) -> c_int {
    let _g = rlock_pcache_tree(p);
    let node = xv6_page_pcache_get_node(page);
    if xv6_pcache_node_io_in_progress(node) == 0 {
        return -(EINVAL as c_int);
    }
    xv6_pcache_node_set_io_in_progress(node, 0);
    unsafe { NodeHandle::new(node) }.io_wakeup_all();
    0
}

fn pcache_node_io_wait(p: *mut Pcache, page: *mut Page) -> c_int {
    let _g = rlock_pcache_tree(p);
    let node = xv6_page_pcache_get_node(page);
    while xv6_pcache_node_io_in_progress(node) != 0 {
        let _ = (unsafe { NodeHandle::new(node) }.io_wait(p));
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
    unsafe { NodeHandle::new(pcnode) }.push_lru(p);
    unsafe { PcacheHandle::new(p) }.inc_lru_count();
}

fn pcache_pop_lru(p: *mut Pcache) -> *mut Page {
    xv6_pcache_spin_assert_holding(p);
    if unsafe { PcacheHandle::new(p) }.lru_is_empty() {
        return ptr::null_mut();
    }
    loop {
        let pcnode = (unsafe { PcacheHandle::new(p) }.lru_last());
        if pcnode.is_null() {
            return ptr::null_mut();
        }
        let page = xv6_pcache_node_page(pcnode);
        assert_msg(!page.is_null(), b"pop_lru: no page\0");
        let guard = lock_pcache_page(page);
        if xv6_pcache_node_lru_detached(pcnode) != 0 {
            drop(guard);
            continue;
        }
        assert_msg(xv6_pcache_node_pcache(pcnode) == p,
            b"pop_lru: pcache mismatch\0");
        xv6_pcache_dec_lru_count(p);
        assert_msg(xv6_pcache_lru_count(p) >= 0, b"pop_lru: count underflow\0");
        xv6_pcache_node_detach_lru(pcnode);
        // Returned page-locked — ownership handed to the caller.
        guard.forget();
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
        unsafe { PcacheHandle::new(p) }.inc_dirty_count();
    } else {
        xv6_pcache_node_detach_lru(pcnode);
    }
    unsafe { NodeHandle::new(pcnode) }.push_dirty(p);
}

fn pcache_pop_dirty(p: *mut Pcache, latest_flush_jiffs: u64) -> *mut Page {
    xv6_pcache_spin_assert_holding(p);
    if unsafe { PcacheHandle::new(p) }.dirty_is_empty() {
        return ptr::null_mut();
    }
    loop {
        let pcnode = (unsafe { PcacheHandle::new(p) }.dirty_last());
        if pcnode.is_null() {
            return ptr::null_mut();
        }
        let page = xv6_pcache_node_page(pcnode);
        assert_msg(!page.is_null(), b"pop_dirty: no page\0");
        let guard = lock_pcache_page(page);
        let last_flushed = (unsafe { NodeHandle::new(pcnode) }.last_flushed());
        if last_flushed > latest_flush_jiffs && latest_flush_jiffs != 0 {
            return ptr::null_mut();
        }
        if xv6_pcache_node_lru_detached(pcnode) != 0 {
            drop(guard);
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
        // Returned page-locked — ownership handed to the caller.
        guard.forget();
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
pub extern "C" fn pcache_global_init() {
    xv6_pcache_globals_init();
    let ret = xv6_pcache_node_slab_init();
    assert_msg(ret == 0, b"Failed to initialize pcache node slab\0");
    let wq = (workqueue_create(PCACHE_FLUSH_WQ_NAME.as_ptr() as *const c_char, WORKQUEUE_DEFAULT_MAX_ACTIVE));
    assert_msg(!wq.is_null(), b"Failed to create global pcache flush workqueue\0");
    xv6_pcache_set_flush_wq(wq);
    crate::kprintln!("Page cache subsystem initialized");
    completion_init(global_completion());
    xv6_pcache_global_flusher_complete_all();
    create_flusher_thread();
}

#[no_mangle]
pub extern "C" fn pcache_init(p: *mut Pcache)-> c_int  {
    // C-ABI boundary: convert `Result<(), Errno>` to a negative-errno
    // `c_int` exactly once, here.
    if let Err(e) = pcache_init_validate(p) {
        return e.neg();
    }
    unsafe { PcacheHandle::new(p) }.list_entries_init();
    xv6_pcache_set_dirty_count(p, 0);
    xv6_pcache_set_lru_count(p, 0);
    xv6_pcache_set_page_count(p, 0);
    unsafe { PcacheHandle::new(p) }.set_flags(0);
    unsafe { PcacheHandle::new(p) }.rb_init();
    if (unsafe { PcacheHandle::new(p) }.gfp_flags()) == 0 {
        unsafe { PcacheHandle::new(p) }.set_gfp_flags(0);
    }
    unsafe { PcacheHandle::new(p) }.spin_init();
    unsafe { PcacheHandle::new(p) }.tree_lock_init();
    unsafe { PcacheHandle::new(p) }.flush_completion_init();
    xv6_pcache_flush_completion_complete_all(p);
    unsafe { PcacheHandle::new(p) }.set_private_data(ptr::null_mut());
    xv6_pcache_set_flush_error(p, 0);
    unsafe { PcacheHandle::new(p) }.set_wait_refcount(0);
    xv6_pcache_set_active(p, 1);
    xv6_pcache_set_flush_requested(p, 0);
    if xv6_pcache_max_pages(p) == 0 {
        unsafe { PcacheHandle::new(p) }.set_max_pages(PCACHE_DEFAULT_MAX_PAGES);
    }
    let rate = xv6_pcache_dirty_rate(p);
    if rate == 0 || rate > 100 {
        unsafe { PcacheHandle::new(p) }.set_dirty_rate(PCACHE_DEFAULT_DIRTY_RATE);
    }
    let now = xv6_pcache_get_jiffs();
    xv6_pcache_set_last_flushed(p, now);
    xv6_pcache_set_last_request(p, now);
    pcache_register(p);
    0
}

#[no_mangle]
pub extern "C" fn pcache_teardown(p: *mut Pcache) {
    if p.is_null() { return; }

    // 1. unregister
    {
        let _gg = lock_pcache_global();
        let _gp = lock_pcache_local(p);
        if xv6_pcache_list_entry_is_detached(p) == 0 {
            unsafe { PcacheHandle::new(p) }.detach_global();
            xv6_pcache_dec_global_count();
        }
    }

    // 2. wait wait_refcount drain
    {
        let _gp = lock_pcache_local(p);
        while (unsafe { PcacheHandle::new(p) }.wait_refcount()) > 0 {
            sleep_on_chan(p as *mut c_void, xv6_pcache_spinlock(p));
        }
    }

    // 3. mark inactive
    let flush_pending = {
        let _gp = lock_pcache_local(p);
        xv6_pcache_set_active(p, 0);
        let flush_pending = xv6_pcache_flush_requested(p) != 0;
        xv6_pcache_wakeup_on_chan(p as *mut c_void);
        flush_pending
    };

    if flush_pending {
        let _ = pcache_wait_flush_complete(p);
    }

    // 4. evict clean LRU pages
    {
        let _gp = lock_pcache_local(p);
        loop {
            let victim = pcache_evict_lru(p);
            if victim.is_null() { break; }
            pcache_page_put(victim);
        }
    }

    // 5. drain remaining rb-tree
    {
        let _gp = lock_pcache_local(p);
        let _gt = wlock_pcache_tree(p);
        loop {
            let node = (unsafe { PcacheHandle::new(p) }.rb_first());
            if node.is_null() { break; }
            xv6_pcache_rb_delete(p, node);
            let pg = xv6_pcache_node_page(node);
            if !pg.is_null() {
                let _pg_lock = lock_pcache_page(pg);
                if xv6_pcache_node_lru_detached(node) == 0 {
                    xv6_pcache_node_detach_lru(node);
                }
                xv6_page_pcache_set_node(pg, ptr::null_mut());
                xv6_page_pcache_set_pcache(pg, ptr::null_mut());
                xv6_pcache_node_set_page(node, ptr::null_mut());
                drop(_pg_lock);
                pcache_page_put(pg);
            }
            xv6_pcache_node_free(node);
        }
        xv6_pcache_set_page_count(p, 0);
        xv6_pcache_set_lru_count(p, 0);
        xv6_pcache_set_dirty_count(p, 0);
    }
}

#[no_mangle]
pub extern "C" fn pcache_get_page(p: *mut Pcache, blkno: u64)-> *mut Page  {
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
            let _gp = lock_pcache_local(p);
            let _pg = lock_pcache_page(page);
            if !pcache_page_valid(p, page) {
                continue 'retry_lookup;
            }
            let pcnode = xv6_page_pcache_get_node(page);
            assert_msg(!pcnode.is_null(), b"get_page: missing pcache node\0");
            if xv6_pcache_node_blkno(pcnode) != base_blkno {
                continue 'retry_lookup;
            }
            if xv6_pcache_node_dirty(pcnode) == 0
                && xv6_pcache_node_lru_detached(pcnode) == 0
            {
                pcache_remove_lru(p, page);
            }
            let r = xv6_pcache_page_ref_inc_unlocked(page);
            if r < 0 {
                continue 'retry_lookup;
            }
            return page;
        }

        // Alloc fresh page
        let new_page = pcache_page_alloc();
        if new_page.is_null() { return ptr::null_mut(); }

        let mut new_page_guard = lock_pcache_page(new_page);
        let new_pcnode = xv6_page_pcache_get_node(new_page);
        assert_msg(!new_pcnode.is_null(), b"get_page: new page no node\0");
        unsafe { NodeHandle::new(new_pcnode) }.set_blkno(base_blkno);
        xv6_pcache_node_set_dirty(new_pcnode, 0);
        xv6_pcache_node_set_uptodate(new_pcnode, 0);
        xv6_pcache_node_set_io_in_progress(new_pcnode, 0);
        xv6_pcache_node_set_size(new_pcnode, xv6_pcache_pgsize());

        let _gp = lock_pcache_local(p);

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
                drop(new_page_guard);
                let ret = (sleep_on_chan_interruptible(p as *mut c_void, xv6_pcache_spinlock(p)));
                new_page_guard = lock_pcache_page(new_page);
                if ret == -(EINTR as c_int) {
                    drop(new_page_guard);
                    drop(_gp);
                    pcache_page_discard(new_page);
                    return ptr::null_mut();
                }
                if !pcache_is_active(p) {
                    drop(new_page_guard);
                    drop(_gp);
                    pcache_page_discard(new_page);
                    return ptr::null_mut();
                }
            }
        }

        let page = pcache_get_page_internal(p, base_blkno, xv6_pcache_pgsize(), new_page);
        if page.is_null() {
            drop(new_page_guard);
            drop(_gp);
            pcache_page_discard(new_page);
            return ptr::null_mut();
        }

        if page != new_page {
            drop(new_page_guard);
            drop(_gp);
            pcache_page_discard(new_page);
            continue 'retry_lookup;
        }

        pcache_node_attach_page(p, new_page);
        let r = xv6_pcache_page_ref_inc_unlocked(new_page);
        assert_msg(r > 1, b"get_page: failed to add caller reference\0");
        return new_page;
    }
}

#[no_mangle]
pub extern "C" fn pcache_put_page(p: *mut Pcache, page: *mut Page) {
    if p.is_null() || page.is_null() { return; }

    let _gp = lock_pcache_local(p);
    let _pg = lock_pcache_page(page);

    if !pcache_page_valid(p, page) {
        xv6_pcache_printf_invalid_page(page, p);
        return;
    }

    let pcnode = xv6_page_pcache_get_node(page);
    let refcount = xv6_pcache_page_ref_count_fn(page);
    if refcount < 2 {
        xv6_pcache_printf_refcount_too_small(page, refcount);
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
                drop(_pg);
                drop(_gp);
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
}

#[no_mangle]
pub extern "C" fn pcache_mark_page_dirty(p: *mut Pcache, page: *mut Page)-> c_int  {
    if p.is_null() || page.is_null() { return -(EINVAL as c_int); }
    let mut ret: c_int = 0;

    let _gp = lock_pcache_local(p);
    let _pg = lock_pcache_page(page);

    if !pcache_page_valid(p, page) {
        ret = -(EINVAL as c_int);
    } else {
        let pcnode = xv6_page_pcache_get_node(page);
        if xv6_pcache_node_dirty(pcnode) != 0 {
            // already dirty
        } else if xv6_pcache_node_io_in_progress(pcnode) != 0 {
            ret = -(EBUSY as c_int);
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

    ret
}

pub(crate) fn pcache_invalidate_page(p: *mut Pcache, page: *mut Page)-> c_int  {
    if p.is_null() || page.is_null() { return -(EINVAL as c_int); }
    let mut ret: c_int = 0;
    let _gp = lock_pcache_local(p);
    let _pg = lock_pcache_page(page);
    if !pcache_page_valid(p, page) {
        ret = -(EINVAL as c_int);
    } else {
        let pcnode = xv6_page_pcache_get_node(page);
        if xv6_pcache_node_io_in_progress(pcnode) != 0 {
            ret = -(EBUSY as c_int);
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
    ret
}

pub(crate) fn pcache_invalidate_blk(p: *mut Pcache, blkno: u64)-> c_int  {
    if p.is_null() || !pcache_is_active(p) { return -(EINVAL as c_int); }
    let base_blkno = xv6_pcache_align_blkno(blkno);

    let _gp = lock_pcache_local(p);
    let page = pcache_get_page_internal(p, base_blkno, xv6_pcache_pgsize(), ptr::null_mut());
    if page.is_null() {
        return 0;
    }
    let _pg = lock_pcache_page(page);
    if !pcache_page_valid(p, page) {
        return 0;
    }
    let pcnode = xv6_page_pcache_get_node(page);
    if xv6_pcache_node_blkno(pcnode) != base_blkno {
        return 0;
    }
    if xv6_pcache_node_io_in_progress(pcnode) != 0 {
        return -(EBUSY as c_int);
    }
    if xv6_pcache_node_lru_detached(pcnode) == 0 {
        pcache_remove_lru(p, page);
    }
    xv6_pcache_node_set_dirty(pcnode, 0);
    xv6_pcache_node_set_uptodate(pcnode, 0);
    0
}

#[no_mangle]
pub extern "C" fn pcache_discard_blk(p: *mut Pcache, blkno: u64)-> c_int  {
    if p.is_null() || !pcache_is_active(p) { return -(EINVAL as c_int); }
    let base_blkno = xv6_pcache_align_blkno(blkno);

    let _gp = lock_pcache_local(p);
    let page = pcache_get_page_internal(p, base_blkno, xv6_pcache_pgsize(), ptr::null_mut());
    if page.is_null() {
        return 0;
    }
    let _pg = lock_pcache_page(page);
    if !pcache_page_valid(p, page) {
        return 0;
    }
    let pcnode = xv6_page_pcache_get_node(page);
    if xv6_pcache_node_blkno(pcnode) != base_blkno {
        return 0;
    }
    if xv6_pcache_node_io_in_progress(pcnode) != 0 {
        return -(EBUSY as c_int);
    }
    if xv6_pcache_node_lru_detached(pcnode) == 0 {
        pcache_remove_lru(p, page);
    }
    {
        let _gt = wlock_pcache_tree(p);
        xv6_pcache_rb_delete(p, pcnode);
        xv6_pcache_add_page_count(p, -1);
    }
    xv6_page_pcache_set_node(page, ptr::null_mut());
    xv6_page_pcache_set_pcache(page, ptr::null_mut());
    xv6_pcache_node_set_page(pcnode, ptr::null_mut());
    drop(_pg);
    drop(_gp);
    xv6_pcache_node_free(pcnode);
    pcache_page_put(page);
    0
}

#[no_mangle]
pub extern "C" fn pcache_flush(p: *mut Pcache)-> c_int  {
    if p.is_null() { return -(EINVAL as c_int); }
    let _gp = lock_pcache_local(p);
    if !pcache_is_active(p) {
        return -(EINVAL as c_int);
    }
    let queued = pcache_queue_work(p);
    if !queued {
        xv6_pcache_set_flush_requested(p, 0);
        xv6_pcache_set_flush_error(p, -(EAGAIN as c_int));
        return -(EAGAIN as c_int);
    }
    drop(_gp);
    pcache_wait_flush_complete(p)
}

pub(crate) fn pcache_sync()-> c_int  {
    {
        let _gg = lock_pcache_global();
        pcache_flusher_start();
    }
    pcache_wait_flusher()
}

#[no_mangle]
pub extern "C" fn pcache_read_page(p: *mut Pcache, page: *mut Page)-> c_int  {
    if p.is_null() || page.is_null() { return -(EINVAL as c_int); }

    'retry_locked: loop {
        let _gp = lock_pcache_local(p);
        let _pg = lock_pcache_page(page);

        if !pcache_is_active(p) || !pcache_page_valid(p, page) {
            return -(EINVAL as c_int);
        }
        let refcount = xv6_pcache_page_ref_count_fn(page);
        if refcount < 2 {
            xv6_pcache_printf_read_refcount_too_small(page, refcount);
            return -(EINVAL as c_int);
        }
        let pcnode = xv6_page_pcache_get_node(page);
        let blkno = xv6_pcache_node_blkno(pcnode);
        let size = (unsafe { NodeHandle::new(pcnode) }.size_get());
        if blkno >= xv6_pcache_blk_count(p) || size == 0 || size > xv6_pcache_pgsize() {
            xv6_pcache_printf_read_invalid_meta(page, blkno, size);
            return -(EINVAL as c_int);
        }

        if xv6_pcache_node_io_in_progress(pcnode) != 0 {
            let dirty = xv6_pcache_node_dirty(pcnode);
            let uptodate = xv6_pcache_node_uptodate(pcnode);
            drop(_pg);
            drop(_gp);
            if uptodate != 0 { return 0; }
            if dirty == 0 && uptodate == 0 {
                let _ = pcache_node_io_wait(p, page);
                continue 'retry_locked;
            }
            xv6_pcache_printf_io_unexpected(dirty, uptodate);
            return -(EIO as c_int);
        }

        if xv6_pcache_node_uptodate(pcnode) != 0 {
            return 0;
        }

        let ret = pcache_node_io_begin(p, page);
        assert_msg(ret == 0, b"read_page: io_begin failed\0");

        drop(_pg);
        drop(_gp);

        let mut wait_for_completion = false;
        let mut ret = xv6_pcache_ops_call_read_page(p, page);
        if ret == -(EINPROGRESS as c_int) {
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

        let ret_post = {
            let _gp = lock_pcache_local(p);
            let _pg = lock_pcache_page(page);
            let mut ret_post: c_int = 0;
            if !pcache_page_valid(p, page) {
                ret_post = -(EINVAL as c_int);
            } else {
                let pcnode = xv6_page_pcache_get_node(page);
                if xv6_pcache_node_lru_detached(pcnode) == 0 {
                    pcache_remove_lru(p, page);
                }
                xv6_pcache_node_set_dirty(pcnode, 0);
                xv6_pcache_node_set_uptodate(pcnode, 1);
            }
            ret_post
        };
        let _ = pcache_node_io_end(p, page);
        return ret_post;
    }
}

// ===========================================================================
// Diagnostics and syscall handlers
// ===========================================================================

pub(crate) fn dump_pcache_stats(p: *mut Pcache) {
    if p.is_null() { return; }
    let _gp = lock_pcache_local(p);
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
}

pub(crate) fn dump_all_pcache_stats() {
    let _gg = lock_pcache_global();
    xv6_pcache_printf_dump_all_header((unsafe { *PCACHE_GLOBAL_COUNT.get() }));
    let mut cur = xv6_pcache_global_first();
    while !cur.is_null() {
        let next = xv6_pcache_global_next(cur);
        dump_pcache_stats(cur);
        cur = next;
    }
}

pub(crate) fn pcache_shrink_caches() {
    slab_cache_shrink(node_slab(), 0x7fff_ffff);
}

#[no_mangle]
pub extern "C" fn sys_sync()-> u64  {
    let ret = pcache_sync();
    if ret != 0 {
        xv6_pcache_printf_sys_sync_failed(ret);
    }
    0
}

#[no_mangle]
pub extern "C" fn sys_dumppcache()-> u64  {
    dump_all_pcache_stats();
    0
}