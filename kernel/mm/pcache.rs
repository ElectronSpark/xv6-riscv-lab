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
//! grepping every `.c`/`.h` file in the tree) is gone. KERNEL-OO further
//! collapsed what was left of it: the `mod ffi` block's thin
//! `spin_lock`/`rwlock_*`/`rb_*`/`slab_*`/`page_*` safe-wrapper facades
//! (each just forwarding its args unchanged to the real `unsafe fn` in
//! its owning module) were deleted and every call site now calls straight
//! through in its own `unsafe {}` block (`slab_alloc` had zero callers
//! left and was deleted outright — the node-alloc path moved to the
//! `SlabCacheRef`/`SlabBox` API in `Pcache::page_alloc`). The
//! `xv6_pcache_*`/`xv6_pcache_node_*`/`xv6_page_*` accessor/mutator
//! forwarders, the global-state helpers, and the whole ~1700-line
//! algorithm body (register/teardown/get_page/flush coordination/LRU
//! eviction/…) are now namespaced as `impl Pcache` / `impl PcacheNode`
//! associated fns (the 8283168 "namespacing only" strategy: byte-identical
//! bodies, still taking the raw `*mut Pcache`/`*mut PcacheNode` pointer as
//! the first param rather than `&self` — forming a reference to either
//! Freeze type is the frozen-noalias hazard `PcacheHandle`/`NodeHandle`
//! above already avoid, see `git show ab4404f`). Public entry points
//! (`Pcache::init`/`get_page`/`put_page`/`read_page`/`teardown`/…, called
//! from `vfs`/`xv6fs`/`tmpfs`) keep their old free-fn names minus the
//! `pcache_` prefix, now reached as `Pcache::name(...)`. The 8 `xv6_e*`
//! errno forwarders are gone; call sites use `crate::bindings::E*`
//! directly.
//!
//! GENUINE FLOOR (still free fns, documented at each definition below):
//! `pcache_rb_compare`/`pcache_rb_get_key` (address-taken in the
//! `PCACHE_RB_OPTS` fn-ptr table), `pcache_flush_worker` (address-taken
//! via `WorkStruct::init`), `flusher_thread` (address-taken via
//! `kthread_create`), `sys_sync`/`sys_dumppcache` (address-taken in
//! `irq/syscall.rs`'s dispatch table), the generic `list_node_t`
//! primitives (`list_init`/`list_detach`/`list_insert`/`list_push_front`/
//! `list_is_empty`/`list_entry_is_detached`, not pcache/node-specific),
//! and the generic panic/assert helpers (`pcache_assert`/`assert_msg`/
//! `xv6_pcache_panic`/`ops_call_optional`).
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
    completion_t, list_node_t, page_t, rb_node, rb_root, rb_root_opts,
    rwlock, sleep_callback_t, slab_cache_t, spinlock_t, thread, thread_state_THREAD_UNINTERRUPTIBLE,
    tq_t, wakeup_callback_t, work_struct, workqueue, EAGAIN, EBUSY, EINPROGRESS, EINTR, EINVAL,
    EIO, SLAB_FLAG_EMBEDDED,
};
use crate::machine;
use crate::mm::mm_safe::{SlabBox, SlabCacheRef};

// ---------------------------------------------------------------------------
// Canonical types. `Pcache`/`PcacheNode`/`PcacheOps` are the hand-written
// natives below (P3-N6); the remaining aliases stay direct re-namings of
// the bindgen/facade structs. Every pointer in this file is `*mut Pcache`
// / `*mut PcacheNode` / `*mut Page` etc., with the real C layout, not an
// opaque zero-sized stand-in.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N6 (mm type family, pcache slice).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/mm/
// pcache_types.h`'s `struct pcache` / `struct pcache_node` / `struct
// pcache_ops` now: `build.rs` blocklists the bindgen-generated forms and
// injects `pub use crate::mm::pcache::... as ...;` facade re-exports (no
// `_t` typedefs exist). Field names/types reproduce bindgen's exactly:
// `_pad0`/`_pad1` reproduce `pcache`'s `__bindgen_padding_0/1` verbatim
// (the C `__ALIGNED_CACHELINE` rides the `spinlock_t` typedef; the
// native `RawSpinlock` is align 8 in Rust, so the explicit pads +
// `completion_t`/`rwlock`'s own native `align(64)` carry the 64-byte
// placements exactly as bindgen emitted them). The anonymous C
// flags union became the named real Rust union [`PcacheFlags`] (field
// `flags`, bit holder [`PcacheFlagBits`]); `pcache_node`'s anonymous C
// bitfield struct became the named 8/8 `{bits,_pad}` holder
// [`PcacheNodeFlagBits`] (field `flags`) — both with safe masking
// accessors bit-identical to bindgen's little-endian unit (N5's
// `VfsInodeFlagBits` precedent); consumers re-pointed from
// `__bindgen_anon_1`. The ops-table fn-pointer fields reproduce
// bindgen's `Option<unsafe extern "C" fn>` forms exactly
// (trait-ification is P3-10's job). Copy fidelity: `pcache_ops` derived
// Copy/Clone in the pre-nativization bindgen output; `pcache` and
// `pcache_node` derived NEITHER, so the natives deliberately have no
// derives — the native `VfsInode` embeds `pcache` BY VALUE (`i_data`)
// and has no derives either (accurate NONCOPY answer in build.rs).
//
// Layout evidence (P3-N6): temporary in-tree `offset_of!` gate on the
// live bindgen forms + cross-compiler `_Static_assert` probe (toolchain
// gcc, rv64gc/lp64d — scratchpad p3n6_static_assert_probe.c); the two
// agree on every size/align/offset.
// ---------------------------------------------------------------------------

/// Native replacement for the anonymous C bitfield struct inside
/// `struct pcache`'s flags union (bindgen's
/// `pcache__bindgen_ty_1__bindgen_ty_1`, 8/8): the pcache state bits.
/// Bit order (LE unit byte 0): active=0, flush_requested=1 — identical
/// to bindgen's `get(N,1)`/`set(N,1)` accessors.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct PcacheFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

macro_rules! pcache_flag_bit {
    ($holder:ident { $($get:ident, $set:ident, $bit:expr;)+ }) => {
        impl $holder {
            $(
                #[inline]
                pub(crate) fn $get(&self) -> u64 {
                    ((self.bits >> $bit) & 0b1) as u64
                }
                #[inline]
                pub(crate) fn $set(&mut self, val: u64) {
                    self.bits = (self.bits & !(1u8 << $bit)) | (((val as u8) & 0b1) << $bit);
                }
            )+
        }
    };
}

pcache_flag_bit!(PcacheFlagBits {
    active, set_active, 0;
    flush_requested, set_flush_requested, 1;
});

/// Native replacement for the anonymous C union inside `struct pcache`
/// (bindgen's `pcache__bindgen_ty_1`, 8/8): the whole flags word
/// (`flags`, the C member name) overlaid with the state bit holder
/// (`bits`).
#[repr(C)]
#[derive(Copy, Clone)]
pub union PcacheFlags {
    pub flags: crate::bindings::uint64,
    pub bits: PcacheFlagBits,
}

/// Native replacement for the anonymous C bitfield struct inside
/// `struct pcache_node` (bindgen's `pcache_node__bindgen_ty_1`, 8/8):
/// the per-node page state bits. Bit order (LE unit byte 0): dirty=0,
/// uptodate=1, io_in_progress=2.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct PcacheNodeFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

pcache_flag_bit!(PcacheNodeFlagBits {
    dirty, set_dirty, 0;
    uptodate, set_uptodate, 1;
    io_in_progress, set_io_in_progress, 2;
});

/// `struct pcache_ops` (`kernel/inc/mm/pcache_types.h`) — the
/// filesystem-facing page IO vtable.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcacheOps {
    pub read_page: Option<
        unsafe extern "C" fn(pcache: *mut Pcache, page: *mut page_t) -> c_int,
    >,
    pub write_page: Option<
        unsafe extern "C" fn(pcache: *mut Pcache, page: *mut page_t) -> c_int,
    >,
    pub write_begin: Option<
        unsafe extern "C" fn(pcache: *mut Pcache, page: *mut page_t) -> c_int,
    >,
    pub write_end: Option<
        unsafe extern "C" fn(pcache: *mut Pcache, page: *mut page_t) -> c_int,
    >,
    pub mark_dirty: Option<unsafe extern "C" fn(pcache: *mut Pcache, page: *mut page_t)>,
}

/// `struct pcache` (`kernel/inc/mm/pcache_types.h`) — one page cache.
/// The anonymous flags union became the named `flags` field
/// ([`PcacheFlags`]); consumers re-pointed from bindgen's
/// `__bindgen_anon_1`.
#[repr(C, align(64))]
pub struct Pcache {
    pub spinlock: spinlock_t,
    pub list_entry: list_node_t,
    pub lru: list_node_t,
    pub dirty_list: list_node_t,
    pub dirty_rate: crate::bindings::uint8,
    pub lru_count: crate::bindings::int64,
    pub dirty_count: crate::bindings::int64,
    pub page_count: crate::bindings::int64,
    pub max_pages: crate::bindings::uint64,
    pub blk_count: crate::bindings::uint64,
    pub last_request: crate::bindings::uint64,
    pub last_flushed: crate::bindings::uint64,
    pub(crate) _pad0: [u64; 2],
    pub flush_completion: completion_t,
    pub private_data: *mut c_void,
    pub flags: PcacheFlags,
    pub(crate) _pad1: [u64; 6],
    pub tree_lock: rwlock,
    pub page_map: rb_root,
    pub gfp_flags: crate::bindings::uint64,
    pub ops: *mut PcacheOps,
    pub flush_work: work_struct,
    pub flush_error: c_int,
    pub wait_refcount: crate::bindings::uint32,
}

/// `struct pcache_node` (`kernel/inc/mm/pcache_types.h`) — one cached
/// page's tree/LRU linkage + state. The anonymous bitfield struct
/// became the named `flags` field ([`PcacheNodeFlagBits`]); consumers
/// re-pointed from bindgen's `__bindgen_anon_1`.
#[repr(C)]
pub struct PcacheNode {
    pub tree_entry: rb_node,
    pub lru_entry: list_node_t,
    pub pcache: *mut Pcache,
    pub page: *mut page_t,
    pub data: *mut c_void,
    pub page_count: crate::bindings::int64,
    pub last_request: crate::bindings::uint64,
    pub last_flushed: crate::bindings::uint64,
    pub flags: PcacheNodeFlagBits,
    pub blkno: crate::bindings::uint64,
    pub size: usize,
    pub io_waiters: tq_t,
}

// P3-N6 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (verified in-tree by the temporary
// `offset_of!` gate) and independently confirmed by the cross-compiler
// `_Static_assert` probe (toolchain gcc agrees on every value).
const _: () = {
    assert!(core::mem::size_of::<PcacheFlags>() == 8, "pcache flags union size");
    assert!(core::mem::align_of::<PcacheFlags>() == 8, "pcache flags union align");
    assert!(core::mem::size_of::<PcacheFlagBits>() == 8, "pcache flag bits size");
    assert!(core::mem::align_of::<PcacheFlagBits>() == 8, "pcache flag bits align");
    assert!(core::mem::size_of::<PcacheNodeFlagBits>() == 8, "pcache_node flag bits size");
    assert!(core::mem::align_of::<PcacheNodeFlagBits>() == 8, "pcache_node flag bits align");

    assert!(core::mem::size_of::<Pcache>() == 576, "pcache size");
    assert!(core::mem::align_of::<Pcache>() == 64, "pcache align");
    assert!(core::mem::offset_of!(Pcache, spinlock) == 0, "pcache.spinlock");
    assert!(core::mem::offset_of!(Pcache, list_entry) == 24, "pcache.list_entry");
    assert!(core::mem::offset_of!(Pcache, lru) == 40, "pcache.lru");
    assert!(core::mem::offset_of!(Pcache, dirty_list) == 56, "pcache.dirty_list");
    assert!(core::mem::offset_of!(Pcache, dirty_rate) == 72, "pcache.dirty_rate");
    assert!(core::mem::offset_of!(Pcache, lru_count) == 80, "pcache.lru_count");
    assert!(core::mem::offset_of!(Pcache, dirty_count) == 88, "pcache.dirty_count");
    assert!(core::mem::offset_of!(Pcache, page_count) == 96, "pcache.page_count");
    assert!(core::mem::offset_of!(Pcache, max_pages) == 104, "pcache.max_pages");
    assert!(core::mem::offset_of!(Pcache, blk_count) == 112, "pcache.blk_count");
    assert!(core::mem::offset_of!(Pcache, last_request) == 120, "pcache.last_request");
    assert!(core::mem::offset_of!(Pcache, last_flushed) == 128, "pcache.last_flushed");
    assert!(core::mem::offset_of!(Pcache, flush_completion) == 192, "pcache.flush_completion");
    assert!(core::mem::offset_of!(Pcache, private_data) == 320, "pcache.private_data");
    assert!(core::mem::offset_of!(Pcache, flags) == 328, "pcache.flags");
    assert!(core::mem::offset_of!(Pcache, tree_lock) == 384, "pcache.tree_lock");
    assert!(core::mem::offset_of!(Pcache, page_map) == 448, "pcache.page_map");
    assert!(core::mem::offset_of!(Pcache, gfp_flags) == 464, "pcache.gfp_flags");
    assert!(core::mem::offset_of!(Pcache, ops) == 472, "pcache.ops");
    assert!(core::mem::offset_of!(Pcache, flush_work) == 480, "pcache.flush_work");
    assert!(core::mem::offset_of!(Pcache, flush_error) == 528, "pcache.flush_error");
    assert!(core::mem::offset_of!(Pcache, wait_refcount) == 532, "pcache.wait_refcount");

    assert!(core::mem::size_of::<PcacheNode>() == 160, "pcache_node size");
    assert!(core::mem::align_of::<PcacheNode>() == 8, "pcache_node align");
    assert!(core::mem::offset_of!(PcacheNode, tree_entry) == 0, "pcache_node.tree_entry");
    assert!(core::mem::offset_of!(PcacheNode, lru_entry) == 24, "pcache_node.lru_entry");
    assert!(core::mem::offset_of!(PcacheNode, pcache) == 40, "pcache_node.pcache");
    assert!(core::mem::offset_of!(PcacheNode, page) == 48, "pcache_node.page");
    assert!(core::mem::offset_of!(PcacheNode, data) == 56, "pcache_node.data");
    assert!(core::mem::offset_of!(PcacheNode, page_count) == 64, "pcache_node.page_count");
    assert!(core::mem::offset_of!(PcacheNode, last_request) == 72, "pcache_node.last_request");
    assert!(core::mem::offset_of!(PcacheNode, last_flushed) == 80, "pcache_node.last_flushed");
    assert!(core::mem::offset_of!(PcacheNode, flags) == 88, "pcache_node.flags");
    assert!(core::mem::offset_of!(PcacheNode, blkno) == 96, "pcache_node.blkno");
    assert!(core::mem::offset_of!(PcacheNode, size) == 104, "pcache_node.size");
    assert!(core::mem::offset_of!(PcacheNode, io_waiters) == 112, "pcache_node.io_waiters");

    assert!(core::mem::size_of::<PcacheOps>() == 40, "pcache_ops size");
    assert!(core::mem::align_of::<PcacheOps>() == 8, "pcache_ops align");
    assert!(core::mem::offset_of!(PcacheOps, read_page) == 0, "pcache_ops.read_page");
    assert!(core::mem::offset_of!(PcacheOps, write_page) == 8, "pcache_ops.write_page");
    assert!(core::mem::offset_of!(PcacheOps, write_begin) == 16, "pcache_ops.write_begin");
    assert!(core::mem::offset_of!(PcacheOps, write_end) == 24, "pcache_ops.write_end");
    assert!(core::mem::offset_of!(PcacheOps, mark_dirty) == 32, "pcache_ops.mark_dirty");
};

pub type Page = page_t;
pub type WorkStruct = work_struct;
pub type Thread = thread;
pub type Workqueue = workqueue;

// ---------------------------------------------------------------------------
// Real C-ABI surface this file depends on. KERNEL-OO: the spinlock/rwlock/
// rbtree/slab/page-level primitives this file needs (`spin_lock`,
// `rwlock_rlock`, `rb_find_key`, `slab_alloc`, `page_lock_acquire`, …) used
// to be re-declared here as thin `pub fn` safe facades, each one just
// forwarding its args unchanged into the real `unsafe fn` in its owning
// module (`crate::lock::spinlock::RawSpinlock::lock`, `crate::mm::page::
// page_lock_acquire`, …). Per the wrapper-removal directive every call site
// (all of them inside `impl PcacheHandle`/`impl NodeHandle` methods, plus a
// handful of `impl Pcache` assoc fns below) now calls straight through to
// the real path in its own `unsafe {}` block; `slab_alloc` had zero callers
// left in this file (superseded by the `SlabCacheRef`/`SlabBox` API used in
// `Pcache::page_alloc`) and was deleted outright. Thread/scheduling/
// workqueue/tq primitives that are pure-Rust and already re-exported at the
// crate root (`kthread_create`, `wakeup`, `sleep_on_chan*`, `tq_*`,
// `workqueue_create`, `queue_work`, `init_work_struct`,
// `rwlock_r_{sleep,wake}_cb`) are called directly as `crate::xxx(...)`
// below instead of being re-declared.
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;
    unsafe extern "C" {
        // No genuine C-ABI imports remain (RB tree is Rust now too, see
        // `crate::bintree`/`crate::rbtree`); kept as an explicit empty
        // block documenting that this file has no real `extern "C"` import
        // surface left.
    }
// P3-D3c: timer (`timer/{timer_core,sched_timer}.rs`) and panic plumbing
// (`printf.rs`) are plain safe Rust fns now that their `#[no_mangle]`
// exports are gone; re-exported here so the module-wide `use ffi::*`
// keeps resolving them (the extern redeclarations above are deleted).
pub(crate) use crate::printf::{__panic_end, __panic_start};
pub(crate) use crate::timer::sched_timer::sleep_ms;
pub(crate) use crate::timer::timer_core::get_jiffs;
pub(crate) use crate::lock::completion::RawCompletion;
pub(crate) use crate::lock::rwlock::{rwlock_r_sleep_cb, rwlock_r_wake_cb};
// P3-D3b: `kthread_create` (proc/thread.rs) and the proc/workqueue.rs
// entry points are plain safe Rust fns now that their `#[no_mangle]`
// exports are gone; re-exported here so the module-wide `use ffi::*`
// keeps resolving them (the extern redeclarations above are deleted).
// P3-D2a: proc/sched.rs wake/sleep entry points and proc/thread_queue.rs
// primitives, reached as plain crate-path items instead of the `extern
// "C"` redeclarations that used to sit in the block above.
pub(crate) use crate::proc::{sleep_on_chan, sleep_on_chan_interruptible, Scheduler};
// NO-STANDALONE-FN: the `tq_init`/`tq_wait_cb`/`tq_wakeup_all` free-fn
// delegators were deleted; the sites below build a `TqRef` handle via
// `from_ptr` and invoke the corresponding inherent method.
pub(crate) use crate::proc::access::TqRef;
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

impl Pcache {
    fn blks_per_page() -> usize {
        PGSIZE >> BLK_SIZE_SHIFT
    }


    fn align_blkno(blkno: u64) -> u64 {
        blkno & !((PGSIZE as u64 >> BLK_SIZE_SHIFT) - 1)
    }


    fn pgsize() -> usize {
        PGSIZE
    }


    fn flush_interval_jiffs() -> u64 {
        PCACHE_FLUSH_INTERVAL_JIFFS
    }


    fn hz() -> u64 {
        HZ
    }


    fn kernel_stack_order() -> c_int {
        KERNEL_STACK_ORDER
    }
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
// either mutated exclusively under `Pcache::global_spinlock()`'s lock or
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

impl Pcache {
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



    fn set_flusher_thread(t: *mut Thread) {
        unsafe {
            *PCACHE_FLUSHER_THREAD_PCB.get() = t;
        }
    }


    fn set_flush_wq(wq: *mut Workqueue) {
        unsafe {
            *PCACHE_GLOBAL_FLUSH_WQ.get() = wq;
        }
    }


    fn inc_global_count() {
        unsafe {
            *PCACHE_GLOBAL_COUNT.get() += 1;
        }
    }


    fn dec_global_count() {
        unsafe {
            let c = PCACHE_GLOBAL_COUNT.get();
            if *c > 0 {
                *c -= 1;
            }
        }
    }


    fn get_flusher_running() -> bool {
        unsafe { *PCACHE_GLOBAL_FLUSHER_RUNNING.get() }
    }


    fn set_flusher_running(v: bool) {
        unsafe {
            *PCACHE_GLOBAL_FLUSHER_RUNNING.get() = v;
        }
    }
}

static SPINLOCK_NAME: &[u8] = b"pcache_global_spinlock\0";
static PCACHE_LOCK_NAME: &[u8] = b"pcache_lock\0";
static PCACHE_TREE_LOCK_NAME: &[u8] = b"pcache_tree_lock\0";
static PCACHE_IO_NAME: &[u8] = b"pcache_io\0";
static PCACHE_NODE_NAME: &[u8] = b"pcache_node\0";
static PCACHE_FLUSH_WQ_NAME: &[u8] = b"pcache_flush_wq\0";

impl Pcache {
    /// One-shot global initialization invoked from `pcache_global_init`.
    fn globals_init() {
        list_init(Pcache::global_list_head());
        unsafe { crate::lock::spinlock::RawSpinlock::init(Pcache::global_spinlock(), SPINLOCK_NAME.as_ptr() as *const c_char as *mut c_char) };
    }
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

impl Pcache {
    // container_of-style helpers, in terms of the one canonical
    // `crate::mm::cffi::container_of` (pure pointer arithmetic, itself safe;
    // only the eventual dereference at the call site is unsafe and documented
    // there).
    #[inline]
    fn from_list_entry(node: *mut list_node_t) -> *mut Pcache {
        crate::mm::cffi::container_of(node, offset_of!(Pcache, list_entry))
    }
}
impl PcacheNode {
    #[inline]
    fn from_lru_entry(node: *mut list_node_t) -> *mut PcacheNode {
        crate::mm::cffi::container_of(node, offset_of!(PcacheNode, lru_entry))
    }


    #[inline]
    fn from_tree_entry(node: *mut rb_node) -> *mut PcacheNode {
        crate::mm::cffi::container_of(node, offset_of!(PcacheNode, tree_entry))
    }
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
    let pcnode = PcacheNode::from_tree_entry(node);
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
        unsafe { crate::lock::spinlock::RawSpinlock::init(self.spinlock_ptr(), PCACHE_LOCK_NAME.as_ptr() as *const c_char as *mut c_char) };
    }
    #[inline]
    fn spin_lock(self) {
        unsafe { crate::lock::spinlock::RawSpinlock::lock(self.spinlock_ptr()) };
    }
    #[inline]
    fn spin_unlock(self) {
        unsafe { crate::lock::spinlock::RawSpinlock::unlock(self.spinlock_ptr()) };
    }
    #[inline]
    fn spin_assert_holding(self) {
        pcache_assert(
            unsafe { crate::lock::spinlock::RawSpinlock::is_holding(self.spinlock_ptr()) } != 0,
            b"pcache_spin_assert_holding: pcache spinlock not held\0",
        );
    }

    #[inline]
    fn tree_lock_ptr(self) -> *mut rwlock {
        unsafe { addr_of_mut!((*self.raw()).tree_lock) }
    }
    #[inline]
    fn tree_lock_init(self) {
        unsafe { crate::lock::rwlock::rwlock_init(self.tree_lock_ptr(), PCACHE_TREE_LOCK_NAME.as_ptr() as *const c_char) };
    }
    #[inline]
    fn tree_rlock(self) {
        unsafe { crate::lock::rwlock::rwlock_rlock(self.tree_lock_ptr()) };
    }
    #[inline]
    fn tree_runlock(self) {
        unsafe { crate::lock::rwlock::rwlock_runlock(self.tree_lock_ptr()) };
    }
    #[inline]
    fn tree_wlock(self) {
        unsafe { crate::lock::rwlock::rwlock_wlock(self.tree_lock_ptr()) };
    }
    #[inline]
    fn tree_wunlock(self) {
        unsafe { crate::lock::rwlock::rwlock_wunlock(self.tree_lock_ptr()) };
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
        unsafe { list_push_front(Pcache::global_list_head(), addr_of_mut!((*self.raw()).list_entry)) };
    }
    #[inline]
    fn detach_global(self) {
        unsafe { list_detach(addr_of_mut!((*self.raw()).list_entry)) };
    }
    #[inline]
    fn next_in_global_list(self) -> *mut Pcache {
        let head = Pcache::global_list_head();
        // SAFETY: `self` is linked into the global list.
        let next = unsafe { (*self.raw()).list_entry.next };
        if next == head {
            ptr::null_mut()
        } else {
            Pcache::from_list_entry(next)
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
        unsafe { (*self.raw()).flags.flags = v };
    }
    #[inline]
    fn active(self) -> bool {
        unsafe { (*self.raw()).flags.bits.active() != 0 }
    }
    #[inline]
    fn set_active(self, v: bool) {
        unsafe {
            (*self.raw()).flags.bits.set_active(v as u64);
        }
    }
    #[inline]
    fn flush_requested(self) -> bool {
        unsafe { (*self.raw()).flags.bits.flush_requested() != 0 }
    }
    #[inline]
    fn set_flush_requested(self, v: bool) {
        unsafe {
            (*self.raw()).flags.bits.set_flush_requested(v as u64);
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
        RawCompletion::init(self.flush_completion_ptr());
    }
    #[inline]
    fn flush_completion_reinit(self) {
        RawCompletion::reinit(self.flush_completion_ptr());
    }
    #[inline]
    fn flush_completion_complete_all(self) {
        RawCompletion::complete_all(self.flush_completion_ptr());
    }
    #[inline]
    fn flush_completion_wait(self) {
        RawCompletion::wait(self.flush_completion_ptr());
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
        let node = unsafe { crate::bintree::rb_find_key(self.page_map_ptr(), blkno) };
        if node.is_null() {
            ptr::null_mut()
        } else {
            PcacheNode::from_tree_entry(node)
        }
    }
    fn rb_insert(self, pcnode: *mut PcacheNode) -> *mut PcacheNode {
        let entry = unsafe { addr_of_mut!((*pcnode).tree_entry) };
        let node = unsafe { crate::rbtree::rb_insert_color(self.page_map_ptr(), entry) };
        if node.is_null() {
            ptr::null_mut()
        } else {
            PcacheNode::from_tree_entry(node)
        }
    }
    fn rb_delete(self, pcnode: *mut PcacheNode) {
        let entry = unsafe { addr_of_mut!((*pcnode).tree_entry) };
        let removed = unsafe { crate::rbtree::rb_delete_node_color(self.page_map_ptr(), entry) };
        pcache_assert(removed == entry, b"xv6_pcache_rb_delete: removed mismatch\0");
    }
    fn rb_first(self) -> *mut PcacheNode {
        let node = unsafe { crate::bintree::rb_first_node(self.page_map_ptr()) };
        if node.is_null() {
            ptr::null_mut()
        } else {
            PcacheNode::from_tree_entry(node)
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
            PcacheNode::from_lru_entry(unsafe { (*head).prev })
        }
    }
    fn dirty_last(self) -> *mut PcacheNode {
        let head = self.dirty_head();
        if list_is_empty(head) {
            ptr::null_mut()
        } else {
            PcacheNode::from_lru_entry(unsafe { (*head).prev })
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
        WorkStruct::init(w, Some(func), self.raw() as u64);
    }
    fn queue_flush_work(self) -> bool {
        let wq = unsafe { *PCACHE_GLOBAL_FLUSH_WQ.get() };
        if wq.is_null() {
            return false;
        }
        let w = unsafe { addr_of_mut!((*self.raw()).flush_work) };
        Workqueue::queue(wq, w)
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
                && (*p).flags.flags == 0
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
        if let Some(r) = TqRef::from_ptr(unsafe { addr_of_mut!((*n).io_waiters) }) {
            r.init(PCACHE_IO_NAME.as_ptr() as *const c_char, ptr::null_mut());
        }
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
        unsafe { (*self.raw()).flags.dirty() != 0 }
    }
    #[inline]
    fn set_dirty(self, v: bool) {
        unsafe { (*self.raw()).flags.set_dirty(v as u64) };
    }
    #[inline]
    fn uptodate(self) -> bool {
        unsafe { (*self.raw()).flags.uptodate() != 0 }
    }
    #[inline]
    fn set_uptodate(self, v: bool) {
        unsafe { (*self.raw()).flags.set_uptodate(v as u64) };
    }
    #[inline]
    fn io_in_progress(self) -> bool {
        unsafe { (*self.raw()).flags.io_in_progress() != 0 }
    }
    #[inline]
    fn set_io_in_progress(self, v: bool) {
        unsafe { (*self.raw()).flags.set_io_in_progress(v as u64) };
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
        let _ = TqRef::from_ptr(unsafe { addr_of_mut!((*self.raw()).io_waiters) }).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait_cb(
            Some(rwlock_r_sleep_cb),
            Some(rwlock_r_wake_cb),
            unsafe { addr_of_mut!((*p).tree_lock) as *mut c_void },
            ptr::null_mut(),
        ));
        0
    }
    fn io_wakeup_all(self) {
        let _ = TqRef::from_ptr(unsafe { addr_of_mut!((*self.raw()).io_waiters) }).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wakeup_all(0, 0));
    }
}

impl Pcache {
    // ===========================================================================
    // `xv6_pcache_*` / `xv6_page_*` delegators — kept under their original
    // names/signatures so the algorithm body below is untouched call-site;
    // each is now a direct call into `PcacheHandle`/`NodeHandle` or a small
    // self-contained implementation (no more C-ABI round trip).
    // ===========================================================================
    fn spinlock(p: *mut Pcache) -> *mut spinlock_t {
        unsafe { PcacheHandle::new(p) }.spinlock_ptr()
    }


    fn spin_assert_holding(p: *mut Pcache) {
        unsafe { PcacheHandle::new(p) }.spin_assert_holding();
    }


    fn global_spin_assert_holding() {
        pcache_assert(
            unsafe { crate::lock::spinlock::RawSpinlock::is_holding(Pcache::global_spinlock()) } != 0,
            b"pcache_global_spin_assert_holding: not held\0",
        );
    }


    fn list_entry_is_detached(p: *mut Pcache) -> c_int {
        if unsafe { PcacheHandle::new(p) }.list_entry_is_detached() { 1 } else { 0 }
    }


    fn global_first() -> *mut Pcache {
        let head = Pcache::global_list_head();
        if list_is_empty(head) {
            ptr::null_mut()
        } else {
            Pcache::from_list_entry(unsafe { (*head).next })
        }
    }


    fn global_next(cur: *mut Pcache) -> *mut Pcache {
        unsafe { PcacheHandle::new(cur) }.next_in_global_list()
    }



    /// Front-to-back iterator over the registered pcaches on the global list,
    /// yielding each live `*mut Pcache`.
    ///
    /// The caller MUST hold the global pcache spinlock for the whole walk
    /// (every call site below either asserts it via
    /// `xv6_pcache_global_spin_assert_holding` or holds a `PcGlobalGuard`), so
    /// the list linkage — and therefore each lazily-computed successor — is
    /// stable across the iteration: no loop body here detaches `cur` from the
    /// global list, and per-`pcache` spinlocks taken inside a body guard only
    /// that pcache's own fields, never the global linkage. Replaces the three
    /// hand-rolled `let mut cur = global_first(); while !cur.is_null() { let
    /// next = global_next(cur); …; cur = next; }` walks (mirrors the
    /// `core::iter::successors` list-scan idiom already used in vfs/file.rs).
    fn global_iter() -> impl Iterator<Item = *mut Pcache> {
        let first = Pcache::global_first();
        core::iter::successors(
            if first.is_null() { None } else { Some(first) },
            |&cur| {
                let next = Pcache::global_next(cur);
                if next.is_null() { None } else { Some(next) }
            },
        )
    }



    fn blk_count(p: *mut Pcache) -> u64 {
        unsafe { PcacheHandle::new(p) }.blk_count()
    }


    fn page_count(p: *mut Pcache) -> i64 {
        unsafe { PcacheHandle::new(p) }.page_count()
    }


    fn set_page_count(p: *mut Pcache, v: i64) {
        unsafe { PcacheHandle::new(p) }.set_page_count(v);
    }


    fn add_page_count(p: *mut Pcache, d: i64) {
        unsafe { PcacheHandle::new(p) }.add_page_count(d);
    }


    fn lru_count(p: *mut Pcache) -> i64 {
        unsafe { PcacheHandle::new(p) }.lru_count()
    }


    fn set_lru_count(p: *mut Pcache, v: i64) {
        unsafe { PcacheHandle::new(p) }.set_lru_count(v);
    }


    fn dec_lru_count(p: *mut Pcache) {
        unsafe { PcacheHandle::new(p) }.dec_lru_count();
    }


    fn dirty_count(p: *mut Pcache) -> i64 {
        unsafe { PcacheHandle::new(p) }.dirty_count()
    }


    fn set_dirty_count(p: *mut Pcache, v: i64) {
        unsafe { PcacheHandle::new(p) }.set_dirty_count(v);
    }


    fn dec_dirty_count(p: *mut Pcache) {
        unsafe { PcacheHandle::new(p) }.dec_dirty_count();
    }


    fn max_pages(p: *mut Pcache) -> u64 {
        unsafe { PcacheHandle::new(p) }.max_pages()
    }


    fn dirty_rate(p: *mut Pcache) -> u8 {
        unsafe { PcacheHandle::new(p) }.dirty_rate()
    }


    fn set_last_request(p: *mut Pcache, v: u64) {
        unsafe { PcacheHandle::new(p) }.set_last_request(v);
    }


    fn set_last_flushed(p: *mut Pcache, v: u64) {
        unsafe { PcacheHandle::new(p) }.set_last_flushed(v);
    }


    fn active(p: *mut Pcache) -> c_int {
        if unsafe { PcacheHandle::new(p) }.active() { 1 } else { 0 }
    }


    fn set_active(p: *mut Pcache, v: c_int) {
        unsafe { PcacheHandle::new(p) }.set_active(v != 0);
    }


    fn flush_requested(p: *mut Pcache) -> c_int {
        if unsafe { PcacheHandle::new(p) }.flush_requested() { 1 } else { 0 }
    }


    fn set_flush_requested(p: *mut Pcache, v: c_int) {
        unsafe { PcacheHandle::new(p) }.set_flush_requested(v != 0);
    }


    fn flush_error(p: *mut Pcache) -> c_int {
        unsafe { PcacheHandle::new(p) }.flush_error()
    }


    fn set_flush_error(p: *mut Pcache, v: c_int) {
        unsafe { PcacheHandle::new(p) }.set_flush_error(v);
    }



    fn flush_completion_complete_all(p: *mut Pcache) {
        unsafe { PcacheHandle::new(p) }.flush_completion_complete_all();
    }


    fn global_flusher_complete_all() {
        RawCompletion::complete_all(Pcache::global_completion());
    }




    fn rb_delete(p: *mut Pcache, n: *mut PcacheNode) {
        unsafe { PcacheHandle::new(p) }.rb_delete(n);
    }




    fn init_flush_work(p: *mut Pcache, func: unsafe extern "C" fn(*mut WorkStruct)) {
        unsafe { PcacheHandle::new(p) }.init_flush_work(func);
    }
}

impl PcacheNode {
    // -- pcache_node accessors --------------------------------------------------
    fn pcache(n: *mut PcacheNode) -> *mut Pcache {
        unsafe { NodeHandle::new(n) }.pcache()
    }


    fn set_pcache(n: *mut PcacheNode, p: *mut Pcache) {
        unsafe { NodeHandle::new(n) }.set_pcache(p);
    }


    fn page(n: *mut PcacheNode) -> *mut Page {
        unsafe { NodeHandle::new(n) }.page()
    }


    fn set_page(n: *mut PcacheNode, page: *mut Page) {
        unsafe { NodeHandle::new(n) }.set_page(page);
    }


    /// C-ABI export: `vm.rs`'s `xv6_vm_call_vma_fault` calls this through its
    /// own `extern "C"` declaration. Pre-existing regression from the WP2
    /// pcache-shim collapse -- this getter was dropped as a "pure accessor"
    /// without checking `vm.rs`'s extern reference; restored here (paired
    /// with the existing `set_data`/`NodeHandle::set_data`) so the kernel
    /// links. Out of scope for the WP3 mm/page/slab/vm_pgtab refactor this
    /// file is otherwise part of.
    pub(crate) fn data(n: *mut PcacheNode) -> *mut c_void {
        unsafe { NodeHandle::new(n) }.data()
    }


    fn set_page_count(n: *mut PcacheNode, c: i64) {
        unsafe { NodeHandle::new(n) }.set_page_count(c);
    }


    fn blkno(n: *mut PcacheNode) -> u64 {
        unsafe { NodeHandle::new(n) }.blkno()
    }


    fn set_size(n: *mut PcacheNode, v: usize) {
        unsafe { NodeHandle::new(n) }.set_size(v);
    }


    fn dirty(n: *mut PcacheNode) -> c_int {
        if unsafe { NodeHandle::new(n) }.dirty() { 1 } else { 0 }
    }


    fn set_dirty(n: *mut PcacheNode, v: c_int) {
        unsafe { NodeHandle::new(n) }.set_dirty(v != 0);
    }


    fn uptodate(n: *mut PcacheNode) -> c_int {
        if unsafe { NodeHandle::new(n) }.uptodate() { 1 } else { 0 }
    }


    fn set_uptodate(n: *mut PcacheNode, v: c_int) {
        unsafe { NodeHandle::new(n) }.set_uptodate(v != 0);
    }


    fn io_in_progress(n: *mut PcacheNode) -> c_int {
        if unsafe { NodeHandle::new(n) }.io_in_progress() { 1 } else { 0 }
    }


    fn set_io_in_progress(n: *mut PcacheNode, v: c_int) {
        unsafe { NodeHandle::new(n) }.set_io_in_progress(v != 0);
    }


    fn lru_detached(n: *mut PcacheNode) -> c_int {
        if unsafe { NodeHandle::new(n) }.lru_detached() { 1 } else { 0 }
    }


    fn detach_lru(n: *mut PcacheNode) {
        unsafe { NodeHandle::new(n) }.detach_lru();
    }
}

// ---------------------------------------------------------------------------
// `page_t` <-> pcache union accessors (the `pcache`/`pcache_node` arm of
// `page_t`'s type-tagged union — see `kernel/inc/mm/page_type.h`).
// ---------------------------------------------------------------------------
impl Pcache {
    fn page_get_pcache(page: *mut Page) -> *mut Pcache {
        unsafe { (*page).type_data.pcache.pcache }
    }


    fn page_set_pcache(page: *mut Page, p: *mut Pcache) {
        unsafe { (*page).type_data.pcache.pcache = p };
    }


    /// C-ABI export: `vm.rs`'s `xv6_vm_call_vma_fault` calls this through its
    /// own `extern "C"` declaration. Pre-existing regression from the WP2
    /// pcache-shim collapse (`pcache_shims.rs` used to export this under the
    /// same name; the replacement below was accidentally left private) —
    /// fixed here as a minimal, behavior-preserving re-export so the kernel
    /// links. `vm.rs`/`pcache.rs` are otherwise out of scope for the WP3
    /// mm/page/slab/vm_pgtab refactor this file is part of.
    pub(crate) fn page_get_node(page: *mut Page) -> *mut PcacheNode {
        unsafe { (*page).type_data.pcache.pcache_node }
    }


    fn page_set_node(page: *mut Page, n: *mut PcacheNode) {
        unsafe { (*page).type_data.pcache.pcache_node = n };
    }


    fn page_is_type(page: *mut Page) -> c_int {
        if page.is_null() {
            return 0;
        }
        let flags = unsafe { (*page).flags };
        if (flags & PAGE_FLAG_TYPE_MASK) == PAGE_TYPE_PCACHE as u64 { 1 } else { 0 }
    }
}

// ---------------------------------------------------------------------------
// Page-level primitives (lock / refcount / alloc), redeclared over the
// bindgen `page_t` view (see the `mod ffi` doc comment above).
// ---------------------------------------------------------------------------
impl Pcache {
    fn page_lock_release(p: *mut Page) {
        unsafe { crate::mm::page::Page::page_lock_release(p as *mut crate::mm::page::Page) };
    }


    fn page_lock_assert_holding(p: *mut Page) {
        unsafe { crate::mm::page::Page::page_lock_assert_holding(p as *mut crate::mm::page::Page) };
    }


    fn page_ref_inc_unlocked(p: *mut Page) -> c_int {
        unsafe { crate::mm::page::Page::page_ref_inc_unlocked(p as *mut crate::mm::page::Page) }
    }


    fn page_ref_dec_unlocked(p: *mut Page) -> c_int {
        unsafe { crate::mm::page::Page::page_ref_dec_unlocked(p as *mut crate::mm::page::Page) }
    }


    fn page_ref_count(p: *mut Page) -> c_int {
        unsafe { crate::mm::page::Page::page_ref_count(p as *mut crate::mm::page::Page) }
    }


    fn page_ref_dec(p: *mut Page) -> c_int {
        unsafe { crate::mm::page::Page::__page_ref_dec(p as *mut crate::mm::page::Page) }
    }
}

// ---------------------------------------------------------------------------
// pcache_ops vtable call helpers, tightened to a null-check-and-call
// (read_page/write_page are guaranteed present by `pcache_init_validate`
// before a pcache is ever registered; write_begin/write_end/mark_dirty
// are optional and share one generic caller).
// ---------------------------------------------------------------------------
impl Pcache {
    fn ops_call_read_page(p: *mut Pcache, page: *mut Page) -> c_int {
        let f = unsafe { PcacheHandle::new(p) }.ops_read_page();
        // SAFETY: `pcache_init_validate` rejects any pcache whose
        // `ops->read_page` is null before the pcache is registered.
        unsafe { (f.unwrap_unchecked())(p, page) }
    }


    fn ops_call_write_page(p: *mut Pcache, page: *mut Page) -> c_int {
        let f = unsafe { PcacheHandle::new(p) }.ops_write_page();
        // SAFETY: see `xv6_pcache_ops_call_read_page`.
        unsafe { (f.unwrap_unchecked())(p, page) }
    }
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
impl Pcache {
    fn ops_call_write_end(p: *mut Pcache, page: *mut Page) -> c_int {
        ops_call_optional(unsafe { PcacheHandle::new(p) }.ops_write_end(), p, page)
    }


    fn ops_call_mark_dirty(p: *mut Pcache, page: *mut Page) {
        if let Some(f) = unsafe { PcacheHandle::new(p) }.ops_mark_dirty() {
            unsafe { f(p, page) };
        }
    }
}

// ---------------------------------------------------------------------------
// Thread / scheduling shims. `kthread_create`/`wakeup*`/`sleep_on_chan*`
// are real Rust functions re-exported at the crate root (`kernel/proc/`);
// `get_jiffs`/`sleep_ms` are Rust too (`kernel/timer/`), imported via the
// `ffi` module's crate-path re-exports (P3-D3c).
// ---------------------------------------------------------------------------
impl Pcache {
    fn get_jiffs() -> u64 {
        get_jiffs()
    }


    fn sleep_ms(ms: u64) {
        sleep_ms(ms);
    }


    fn wakeup(t: *mut Thread) {
        Scheduler::wakeup(t);
    }


    fn wakeup_on_chan(chan: *mut c_void) {
        Scheduler::wakeup_on_chan(chan);
    }


    fn kthread_create(
        name: *const c_char,
        entry: unsafe extern "C" fn(u64, u64),
        a1: u64,
        a2: u64,
        stack_order: c_int,
    ) -> *mut Thread {
        let np = crate::proc::thread::Thread::kthread_create(name, entry as *mut c_void, a1, a2, stack_order);
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
}

// ---------------------------------------------------------------------------
// Slab helpers (kernel/mm/slab.rs).
// ---------------------------------------------------------------------------
impl Pcache {
    fn node_slab_init() -> c_int {
        unsafe { crate::mm::slab::slab_cache_init(Pcache::node_slab() as *mut crate::mm::slab::SlabCache, PCACHE_NODE_NAME.as_ptr() as *const c_char as *mut c_char, size_of::<PcacheNode>(), SLAB_FLAG_EMBEDDED as u64, ) }
    }
}
impl PcacheNode {
    fn free(p: *mut PcacheNode) {
        unsafe { crate::mm::slab::slab_free(p as *mut c_void) };
    }
}

// ---------------------------------------------------------------------------
// Diagnostic printf wrappers.
// ---------------------------------------------------------------------------
impl Pcache {
    fn printf_flush_queue_warn(p: *mut Pcache) {
        unsafe {
            crate::kprintln!(
                "warning: flusher failed to queue work for pcache {}",
                crate::printf::Ptr(p as u64),
            );
        }
    }


    fn printf_flush_wait_err(p: *mut Pcache, ret: c_int) {
        unsafe {
            crate::kprintln!(
                "warning: __pcache_wait_for_pending_flushes: pcache {} flush error {}",
                crate::printf::Ptr(p as u64),
                ret,
            );
        }
    }


    fn printf_default_page_invalid() {
        crate::kprintln!(
            "__pcache_get_page: default_page is not from the given pcache",
        );
    }


    fn printf_invalid_page(page: *mut Page, p: *mut Pcache) {
        unsafe {
            crate::kprintln!(
                "Pcache::put_page(): invalid page {} for cache {}",
                crate::printf::Ptr(page as u64),
                crate::printf::Ptr(p as u64),
            );
        }
    }


    fn printf_refcount_too_small(page: *mut Page, refcount: c_int) {
        unsafe {
            crate::kprintln!(
                "Pcache::put_page(): page {} refcount {} is too small to drop",
                crate::printf::Ptr(page as u64),
                refcount,
            );
        }
    }


    fn printf_read_refcount_too_small(page: *mut Page, refcount: c_int) {
        unsafe {
            crate::kprintln!(
                "Pcache::read_page(): page {} refcount {} is too small to read",
                crate::printf::Ptr(page as u64),
                refcount,
            );
        }
    }


    fn printf_read_invalid_meta(page: *mut Page, blkno: u64, sz: usize) {
        unsafe {
            crate::kprintln!(
                "Pcache::read_page(): invalid metadata for page {} (blkno={} size={})",
                crate::printf::Ptr(page as u64),
                blkno,
                sz as u64,
            );
        }
    }


    fn printf_io_unexpected(dirty: c_int, uptodate: c_int) {
        unsafe {
            crate::kprintln!(
                "Pcache::read_page(): io in progress with unexpected state (dirty={} uptodate={})",
                dirty,
                uptodate,
            );
        }
    }


    fn printf_sys_sync_failed(ret: c_int) {
        unsafe {
            crate::kprintln!("sys_sync: pcache_sync failed with error {}", ret);
        }
    }


    fn printf_stats_header(p: *mut Pcache) {
        unsafe {
            crate::kprintln!("Pcache {} stats:", crate::printf::Ptr(p as u64));
        }
    }


    fn printf_stats_body(
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


    fn printf_dump_all_header(total: c_int) {
        unsafe {
            crate::kprintln!("Dumping all pcache stats:");
            crate::kprintln!("Total pcaches: {}", total);
        }
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

impl Pcache {
    #[inline]
    fn is_active(p: *mut Pcache) -> bool {
        Pcache::active(p) != 0
    }



    fn init_validate(p: *mut Pcache) -> Result<(), crate::mm::cffi::Errno> {
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
        if Pcache::blk_count(p) == 0 {
            return Err(Errno::Inval);
        }
        if !unsafe { PcacheHandle::new(p) }.is_zero_inited() {
            return Err(Errno::Inval);
        }
        Ok(())
    }



    #[inline]
    fn page_valid(p: *mut Pcache, page: *mut Page) -> bool {
        if p.is_null() || page.is_null() {
            return false;
        }
        if Pcache::page_is_type(page) == 0 {
            return false;
        }
        Pcache::page_get_pcache(page) == p && !Pcache::page_get_node(page).is_null()
    }
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

// RAII drop guards for the pcache's various locking primitives. Each
// guard releases its matching shim on Drop, so callers can use scoped
// `let _g = lock_*()` patterns instead of manual lock/unlock pairs.
//
// `PcLocalGuard`/`PcTreeReadGuard`/`PcTreeWriteGuard` additionally *carry*
// the already-validated `PcacheHandle` for the locked `pcache` via `Deref`.
// Before this, every call site that needed a field/method under one of
// these locks had to separately reconstruct `unsafe { PcacheHandle::new(p)
// }` on the same pointer the guard was already holding, each repeating the
// same "non-null, live pcache" SAFETY argument. Now that argument is made
// exactly once, in the guard's constructor below, and every call site that
// holds the guard variable writes `guard.method()` (`PcacheHandle` is
// `Copy`, so this works through the `Deref` chain like any other
// pointer-to-handle smart pointer). Mirrors the `VmReadGuard`/
// `VmWriteGuard`/`VmPgtableGuard` pattern in `vm.rs`.
#[must_use = "global pcache lock is released immediately if the guard is dropped"]
struct PcGlobalGuard;
impl Drop for PcGlobalGuard {
    fn drop(&mut self) { unsafe { crate::lock::spinlock::RawSpinlock::unlock(Pcache::global_spinlock()) }; }
}
impl PcGlobalGuard {
    /// Read `PCACHE_GLOBAL_COUNT` (registered-pcache count) under the held
    /// global lock.
    fn global_count(&self) -> c_int {
        // SAFETY: `PcGlobalGuard` is only ever constructed by
        // `lock_pcache_global`, which has just acquired `global_spinlock`;
        // `PCACHE_GLOBAL_COUNT` is only ever mutated under this same lock
        // (see `xv6_pcache_inc_global_count`/`_dec_global_count`), so a
        // read through `&self` -- whose lifetime is bound to the lock hold,
        // released in `Drop` -- cannot race a concurrent writer.
        unsafe { *PCACHE_GLOBAL_COUNT.get() }
    }
}
impl Pcache {
    fn lock_global() -> PcGlobalGuard {
        unsafe { crate::lock::spinlock::RawSpinlock::lock(Pcache::global_spinlock()) };
        PcGlobalGuard
    }
}

/// RAII guard for a pcache's per-instance spinlock
/// (`xv6_pcache_spin_lock`/`_unlock`). Carries the locked pcache's
/// [`PcacheHandle`] via `Deref` -- see the module comment above this block.
#[must_use = "pcache lock is released immediately if the guard is dropped"]
struct PcLocalGuard {
    handle: PcacheHandle,
}
impl Drop for PcLocalGuard {
    fn drop(&mut self) { self.handle.spin_unlock(); }
}
impl core::ops::Deref for PcLocalGuard {
    type Target = PcacheHandle;
    fn deref(&self) -> &PcacheHandle {
        &self.handle
    }
}
impl Pcache {
    fn lock_local(p: *mut Pcache) -> PcLocalGuard {
        // SAFETY: every call site passes a `p` already known non-null/live --
        // either checked directly by the caller just above (e.g.
        // `pcache_register`, `pcache_teardown`, the `#[no_mangle]` public API
        // functions all null-check `p` on entry) or a `p`/`cur` freshly
        // returned from the global pcache list (`xv6_pcache_global_first`/
        // `_next`, themselves walking a list that only ever holds live
        // `pcache` pointers). Identical precondition to the bare-newtype
        // guard this replaces, which reconstructed the same `PcacheHandle` in
        // `Drop`.
        let handle = unsafe { PcacheHandle::new(p) };
        handle.spin_lock();
        PcLocalGuard { handle }
    }
}

/// RAII guard for a pcache's rb-tree read lock. See [`PcLocalGuard`] for
/// the `Deref`-carries-`PcacheHandle` rationale.
#[must_use = "pcache tree read lock is released when the guard is dropped"]
struct PcTreeReadGuard {
    handle: PcacheHandle,
}
impl Drop for PcTreeReadGuard {
    fn drop(&mut self) { self.handle.tree_runlock(); }
}
impl core::ops::Deref for PcTreeReadGuard {
    type Target = PcacheHandle;
    fn deref(&self) -> &PcacheHandle {
        &self.handle
    }
}
impl Pcache {
    fn rlock_tree(p: *mut Pcache) -> PcTreeReadGuard {
        // SAFETY: see `lock_pcache_local`.
        let handle = unsafe { PcacheHandle::new(p) };
        handle.tree_rlock();
        PcTreeReadGuard { handle }
    }
}

/// RAII guard for a pcache's rb-tree write lock. See [`PcLocalGuard`] for
/// the `Deref`-carries-`PcacheHandle` rationale.
#[must_use = "pcache tree write lock is released when the guard is dropped"]
struct PcTreeWriteGuard {
    handle: PcacheHandle,
}
impl Drop for PcTreeWriteGuard {
    fn drop(&mut self) { self.handle.tree_wunlock(); }
}
impl core::ops::Deref for PcTreeWriteGuard {
    type Target = PcacheHandle;
    fn deref(&self) -> &PcacheHandle {
        &self.handle
    }
}
impl Pcache {
    fn wlock_tree(p: *mut Pcache) -> PcTreeWriteGuard {
        // SAFETY: see `lock_pcache_local`.
        let handle = unsafe { PcacheHandle::new(p) };
        handle.tree_wlock();
        PcTreeWriteGuard { handle }
    }
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
    fn drop(&mut self) { Pcache::page_lock_release(self.0); }
}
impl PcPageGuard {
    /// Release ownership without unlocking — the page lock is being
    /// handed off to the caller of the current function.
    fn forget(self) { core::mem::forget(self); }
}
impl Pcache {
    fn lock_page(page: *mut Page) -> PcPageGuard {
        unsafe { crate::mm::page::Page::page_lock_acquire(page as *mut crate::mm::page::Page) };
        PcPageGuard(page)
    }


    /// Wrap a page pointer that is already locked (lock ownership handed in
    /// from elsewhere, e.g. `pcache_pop_lru`/`pcache_pop_dirty`) in a guard
    /// so this function's remaining unlock paths become RAII.
    fn adopt_page_lock(page: *mut Page) -> PcPageGuard {
        PcPageGuard(page)
    }



    fn register(p: *mut Pcache) {
        if p.is_null() {
            return;
        }
        let _gg = Pcache::lock_global();
        let _gp = Pcache::lock_local(p);
        if Pcache::list_entry_is_detached(p) != 0 {
            _gp.push_global();
            Pcache::inc_global_count();
        } else {
            crate::kprint!("warning: __pcache_register: pcache already registered");
        }
    }
}

// ---------------------------------------------------------------------------
// Flush coordination
// ---------------------------------------------------------------------------
impl Pcache {
    fn notify_flush_complete(p: *mut Pcache) {
        if p.is_null() { return; }
        Pcache::flush_completion_complete_all(p);
    }



    fn wait_flush_complete(p: *mut Pcache) -> c_int {
        if p.is_null() { return -(EINVAL as c_int); }
        unsafe { PcacheHandle::new(p) }.flush_completion_wait();
        Pcache::flush_error(p)
    }



    fn queue_work(p: *mut Pcache) -> bool {
        if p.is_null() || (unsafe { *PCACHE_GLOBAL_FLUSH_WQ.get() }).is_null() {
            return false;
        }
        Pcache::spin_assert_holding(p);
        if Pcache::flush_requested(p) != 0 {
            return true;
        }
        Pcache::init_flush_work(p, pcache_flush_worker);
        let queued = (unsafe { PcacheHandle::new(p) }.queue_flush_work());
        if queued {
            Pcache::set_flush_requested(p, 1);
            Pcache::set_last_request(p, Pcache::get_jiffs());
            Pcache::set_flush_error(p, 0);
            unsafe { PcacheHandle::new(p) }.flush_completion_reinit();
        }
        queued
    }



    fn flush_done(p: *mut Pcache) {
        Pcache::spin_assert_holding(p);
        Pcache::set_flush_requested(p, 0);
        Pcache::set_last_flushed(p, Pcache::get_jiffs());
        Pcache::notify_flush_complete(p);
    }



    fn flusher_start() {
        Pcache::global_spin_assert_holding();
        if Pcache::get_flusher_running() {
            return;
        }
        Pcache::set_flusher_running(true);
        RawCompletion::reinit(Pcache::global_completion());
        let pcb = (unsafe { *PCACHE_FLUSHER_THREAD_PCB.get() });
        if machine::current_thread_ptr() != pcb {
            Pcache::wakeup(pcb);
        }
    }



    fn wait_flusher() -> c_int {
        if Pcache::get_flusher_running() {
            RawCompletion::wait(Pcache::global_completion());
        }
        0
    }



    fn flusher_done() {
        Pcache::global_spin_assert_holding();
        Pcache::set_flusher_running(false);
        Pcache::global_flusher_complete_all();
    }
}

// ---------------------------------------------------------------------------
// Workqueue worker
// ---------------------------------------------------------------------------
extern "C" fn pcache_flush_worker(work: *mut WorkStruct) {
    let p = (unsafe { (*work).data }) as *mut Pcache;
    let start_jiffs = Pcache::get_jiffs();

    if p.is_null() {
        crate::kprintln!("__pcache_flush_worker: pcache is NULL");
        return;
    }

    // `p_guard` spans the whole loop (each iteration's top,
    // `pcache_pop_dirty`, asserts it is held) and is explicitly
    // dropped/reacquired around the out-of-lock IO calls below.
    let mut p_guard = Pcache::lock_local(p);
    loop {
        let page = Pcache::pop_dirty(p, start_jiffs);
        if page.is_null() { break; }
        // `page` comes back page-locked (ownership handed off by
        // `pcache_pop_dirty`); adopt it into a guard.
        let page_guard = Pcache::adopt_page_lock(page);

        let mut r = Pcache::page_ref_inc_unlocked(page);
        assert_msg(r > 1, b"flush_worker: failed to increment page ref\0");

        r = Pcache::node_io_begin(p, page);
        assert_msg(r == 0, b"flush_worker: failed to begin IO\0");

        drop(page_guard);
        drop(p_guard);

        // Real write outside lock
        let mut ret = (ops_call_optional(unsafe { PcacheHandle::new(p) }.ops_write_begin(), p, page));
        if ret != 0 {
            p_guard = Pcache::lock_local(p);
            Pcache::set_flush_error(p, ret);
            Pcache::flush_err_continue(p, page);
            continue;
        }
        ret = Pcache::ops_call_write_page(p, page);
        if ret != 0 {
            let _ = Pcache::ops_call_write_end(p, page);
            p_guard = Pcache::lock_local(p);
            Pcache::set_flush_error(p, ret);
            Pcache::flush_err_continue(p, page);
            continue;
        }
        ret = Pcache::ops_call_write_end(p, page);

        p_guard = Pcache::lock_local(p);
        let page_guard = Pcache::lock_page(page);
        if ret != 0 {
            Pcache::set_flush_error(p, ret);
        }
        let pcnode = Pcache::page_get_node(page);
        assert_msg(!pcnode.is_null(), b"flush_worker: page missing pcache_node\0");
        PcacheNode::set_dirty(pcnode, 0);
        PcacheNode::set_uptodate(pcnode, 1);
        let r2 = Pcache::node_io_end(p, page);
        assert_msg(r2 == 0, b"flush_worker: failed to end IO\0");
        let r3 = Pcache::page_ref_dec_unlocked(page);
        assert_msg(r3 >= 1, b"flush_worker: page refcount underflow\0");
        if r3 == 1 && PcacheNode::lru_detached(pcnode) != 0 {
            Pcache::push_lru(p, page);
            Pcache::wakeup_on_chan(p as *mut c_void);
        }
        drop(page_guard);
    }
    Pcache::flush_done(p);
    drop(p_guard);
}

impl Pcache {
    /// err_continue arm of the flush worker (assumes spinlock held).
    fn flush_err_continue(p: *mut Pcache, page: *mut Page) {
        Pcache::spin_assert_holding(p);
        let _pg = Pcache::lock_page(page);
        let r = Pcache::node_io_end(p, page);
        assert_msg(r == 0, b"flush_worker: failed to end IO (err)\0");
        Pcache::push_dirty(p, page);
        let r2 = Pcache::page_ref_dec_unlocked(page);
        assert_msg(r2 > 0, b"flush_worker: failed to decrement ref (err)\0");
    }



    fn schedule_flushes_locked(round_start: u64, force_round: bool) -> bool {
        let mut pending_flush = false;
        for cur in Pcache::global_iter() {
            let _g = Pcache::lock_local(cur);
            if !Pcache::is_active(cur) {
                continue;
            }
            let mut should_flush = false;
            let dirty_count = Pcache::dirty_count(cur);
            if dirty_count > 0 {
                if force_round {
                    should_flush = true;
                } else {
                    let page_count = Pcache::page_count(cur);
                    let dirty_rate = Pcache::dirty_rate(cur) as i64;
                    let mut dirty_threshold: i64 = 0;
                    if page_count > 0 && dirty_rate > 0 {
                        dirty_threshold = (page_count * dirty_rate) / 100;
                    }
                    if dirty_threshold == 0 && dirty_count > 0 {
                        dirty_threshold = 1;
                    }
                    let last_flushed = _g.last_flushed();
                    if dirty_threshold > 0 && dirty_count >= dirty_threshold {
                        should_flush = true;
                    } else if round_start >= last_flushed
                        && round_start - last_flushed >= Pcache::flush_interval_jiffs()
                    {
                        should_flush = true;
                    }
                }
            }
            if should_flush {
                if !Pcache::queue_work(cur) {
                    if Pcache::flush_requested(cur) == 0 {
                        Pcache::printf_flush_queue_warn(cur);
                    }
                }
            }
            if Pcache::flush_requested(cur) != 0 {
                pending_flush = true;
            }
        }
        pending_flush
    }



    fn pick_pending_before(jiffs: u64) -> *mut Pcache {
        Pcache::global_spin_assert_holding();
        for cur in Pcache::global_iter() {
            let found = {
                let _g = Pcache::lock_local(cur);
                if Pcache::flush_requested(cur) != 0 {
                    let last_request = _g.last_request();
                    last_request <= jiffs
                } else {
                    false
                }
            };
            if found {
                return cur;
            }
        }
        ptr::null_mut()
    }



    fn wait_for_pending_flushes() {
        let start_jiffs = Pcache::get_jiffs();
        loop {
            let p = {
                let _gg = Pcache::lock_global();
                let p = Pcache::pick_pending_before(start_jiffs);
                if !p.is_null() {
                    let _gp = Pcache::lock_local(p);
                    _gp.inc_wait_refcount();
                }
                p
            };
            if p.is_null() {
                break;
            }

            let ret = Pcache::wait_flush_complete(p);
            if ret != 0 {
                Pcache::printf_flush_wait_err(p, ret);
            }

            {
                let _gp = Pcache::lock_local(p);
                _gp.dec_wait_refcount();
                Pcache::wakeup_on_chan(p as *mut c_void);
            }

            Pcache::sleep_ms(10);
        }
    }
}

extern "C" fn flusher_thread(_a1: u64, _a2: u64) {
    crate::kprintln!("pcache flusher thread started");
    loop {
        let round_start = Pcache::get_jiffs();
        let pending = {
            let _gg = Pcache::lock_global();
            let force_round = Pcache::get_flusher_running();
            Pcache::flusher_start();
            Pcache::schedule_flushes_locked(round_start, force_round)
        };
        if pending {
            Pcache::wait_for_pending_flushes();
        }
        {
            let _gg = Pcache::lock_global();
            Pcache::flusher_done();
        }

        let sleep_ticks = Pcache::flush_interval_jiffs();
        let mut sleep_ms_val = (sleep_ticks * 1000) / Pcache::hz();
        if sleep_ms_val == 0 { sleep_ms_val = 1; }
        Pcache::sleep_ms(sleep_ms_val);
    }
}

impl Pcache {
    fn create_flusher_thread() {
        let name = b"pcache_flusher\0".as_ptr() as *const c_char;
        let stack = Pcache::kernel_stack_order();
        let np = Pcache::kthread_create(name, flusher_thread, 0, 0, stack);
        assert_msg(!np.is_null(), b"Failed to create pcache flusher thread\0");
        Pcache::set_flusher_thread(np);
        Pcache::wakeup(np);
    }
}

// ---------------------------------------------------------------------------
// Tree helpers (Rust-level — call into rb shims)
// ---------------------------------------------------------------------------
impl Pcache {
    fn get_page_internal(
        p: *mut Pcache,
        blkno: u64,
        _size: usize,
        default_page: *mut Page,
    ) -> *mut Page {
        let blk_count = Pcache::blk_count(p);
        if blkno >= blk_count { return ptr::null_mut(); }
        if blkno + Pcache::blks_per_page() as u64 > blk_count {
            return ptr::null_mut();
        }

        if !default_page.is_null() {
            Pcache::page_lock_assert_holding(default_page);
            let dp_pcache = Pcache::page_get_pcache(default_page);
            let dp_node = Pcache::page_get_node(default_page);
            if Pcache::page_is_type(default_page) == 0
                || !dp_pcache.is_null()
                || dp_node.is_null()
                || PcacheNode::page(dp_node) != default_page
            {
                Pcache::printf_default_page_invalid();
                return ptr::null_mut();
            }
        }

        let found_node: *mut PcacheNode;
        if !default_page.is_null() {
            let _g = Pcache::wlock_tree(p);
            let dp_node = Pcache::page_get_node(default_page);
            found_node = _g.rb_insert(dp_node);
            if found_node != dp_node {
                return PcacheNode::page(found_node);
            }
        } else {
            let _g = Pcache::rlock_tree(p);
            found_node = _g.rb_find(blkno);
            if found_node.is_null() {
                return ptr::null_mut();
            }
        }
        PcacheNode::page(found_node)
    }



    fn remove_node(p: *mut Pcache, page: *mut Page) {
        Pcache::page_lock_assert_holding(page);
        let _g = Pcache::wlock_tree(p);
        let pcnode = Pcache::page_get_node(page);
        assert_msg(!pcnode.is_null(), b"remove_node: page has no node\0");
        assert_msg(PcacheNode::page(pcnode) == page,
            b"remove_node: pcnode mismatch\0");
        assert_msg(PcacheNode::lru_detached(pcnode) != 0,
            b"remove_node: must be detached from lru/dirty\0");
        Pcache::rb_delete(p, pcnode);
    }
}

// ---------------------------------------------------------------------------
// page alloc / discard
// ---------------------------------------------------------------------------
impl Pcache {
    fn page_alloc() -> *mut Page {
        // Borrow the node cache through the layout-verified bindgen<->slab.rs
        // view (same pointer-cast pattern the `ffi::slab_alloc` redeclaration
        // above already relies on; sizes are cross-checked by static asserts
        // in slab.rs). SAFETY: `Pcache::node_slab()` is initialised once at pcache
        // subsystem startup (`xv6_pcache_node_slab_init`) and lives for the
        // rest of the kernel's lifetime — the `'static`-equivalent borrow is
        // always valid.
        let cache_ref = unsafe {
            SlabCacheRef::from_raw(&*(Pcache::node_slab() as *mut crate::mm::slab::SlabCache))
        };
        // SAFETY: the cache was created with `size_of::<PcacheNode>()` as its
        // object size (see `xv6_pcache_node_slab_init` above), matching `T`.
        let node_box: SlabBox<'_, MaybeUninit<PcacheNode>> = match unsafe { cache_ref.alloc_uninit() } {
            Some(b) => b,
            None => return ptr::null_mut(),
        };
        let pcnode = node_box.as_ptr() as *mut PcacheNode;
        let page = ((unsafe { crate::mm::page::Page::__page_alloc(0, PAGE_TYPE_PCACHE as u64) } as *mut page_t));
        if page.is_null() {
            // `node_box` drops here -> unsafe { crate::mm::slab::slab_free(pcnode) }; replaces the old
            // manual `PcacheNode::free(pcnode)` call.
            return ptr::null_mut();
        }
        unsafe { NodeHandle::new(pcnode) }.init();
        PcacheNode::set_page(pcnode, page);
        PcacheNode::set_page_count(pcnode, 1);
        PcacheNode::set_size(pcnode, Pcache::pgsize());
        unsafe { NodeHandle::new(pcnode) }.set_data((unsafe { crate::mm::page::Page::__page_to_pa(page as *mut crate::mm::page::Page) } as *mut c_void));
        Pcache::page_set_node(page, pcnode);
        Pcache::page_set_pcache(page, ptr::null_mut());
        // `pcnode` is now reachable and owned via `page->pcache.pcache_node`
        // (a C-visible pointer walked from many other sites via
        // `Pcache::page_get_node`); its lifetime is no longer scoped to
        // this function, so hand ownership off the RAII box.
        //
        // SAFETY: every field the type requires has just been written above
        // via `NodeHandle::init()` + the explicit `set_*` calls.
        let node_box: SlabBox<'_, PcacheNode> = unsafe { node_box.assume_init() };
        let _ = node_box.into_raw();
        page
    }



    fn page_put(page: *mut Page) {
        if page.is_null() { return; }
        Pcache::page_ref_dec(page);
    }



    fn page_discard(page: *mut Page) {
        if page.is_null() { return; }
        let pcnode = Pcache::page_get_node(page);
        if !pcnode.is_null() {
            Pcache::page_set_node(page, ptr::null_mut());
            PcacheNode::free(pcnode);
        }
        Pcache::page_ref_dec(page);
    }



    fn node_attach_page(p: *mut Pcache, page: *mut Page) {
        Pcache::page_lock_assert_holding(page);
        Pcache::spin_assert_holding(p);
        let pcnode = Pcache::page_get_node(page);
        assert_msg(!pcnode.is_null(), b"attach: page has no node\0");
        assert_msg(PcacheNode::page(pcnode) == page, b"attach: mismatch\0");
        assert_msg(PcacheNode::pcache(pcnode).is_null(),
            b"attach: pcache_node's pcache must be NULL\0");
        PcacheNode::set_page_count(pcnode, 1);
        PcacheNode::set_pcache(pcnode, p);
        Pcache::page_set_pcache(page, p);
        Pcache::page_set_node(page, pcnode);
        Pcache::add_page_count(p, 1);
    }



    fn node_detach_page(p: *mut Pcache, page: *mut Page) {
        Pcache::page_lock_assert_holding(page);
        Pcache::spin_assert_holding(p);
        let pcnode = Pcache::page_get_node(page);
        assert_msg(!pcnode.is_null(), b"detach: page has no node\0");
        assert_msg(PcacheNode::page(pcnode) == page, b"detach: mismatch\0");
        assert_msg(PcacheNode::pcache(pcnode) == p, b"detach: pcache mismatch\0");
        assert_msg(PcacheNode::lru_detached(pcnode) != 0,
            b"detach: must be detached from lru/dirty\0");
        Pcache::page_set_pcache(page, ptr::null_mut());
        PcacheNode::set_pcache(pcnode, ptr::null_mut());
        Pcache::add_page_count(p, -1);
        assert_msg(Pcache::page_count(p) >= 0, b"detach: page_count negative\0");
    }
}

// ---------------------------------------------------------------------------
// IO sync
// ---------------------------------------------------------------------------
impl Pcache {
    fn node_io_begin(p: *mut Pcache, page: *mut Page) -> c_int {
        let _g = Pcache::rlock_tree(p);
        let node = Pcache::page_get_node(page);
        if PcacheNode::io_in_progress(node) != 0 {
            return -(EINVAL as c_int); // We use EALREADY in C; same numeric handling not critical
        }
        PcacheNode::set_io_in_progress(node, 1);
        unsafe { NodeHandle::new(node) }.set_last_request(Pcache::get_jiffs());
        0
    }



    fn node_io_end(p: *mut Pcache, page: *mut Page) -> c_int {
        let _g = Pcache::rlock_tree(p);
        let node = Pcache::page_get_node(page);
        if PcacheNode::io_in_progress(node) == 0 {
            return -(EINVAL as c_int);
        }
        PcacheNode::set_io_in_progress(node, 0);
        unsafe { NodeHandle::new(node) }.io_wakeup_all();
        0
    }



    fn node_io_wait(p: *mut Pcache, page: *mut Page) -> c_int {
        let _g = Pcache::rlock_tree(p);
        let node = Pcache::page_get_node(page);
        while PcacheNode::io_in_progress(node) != 0 {
            let _ = (unsafe { NodeHandle::new(node) }.io_wait(p));
        }
        0
    }
}

// ---------------------------------------------------------------------------
// LRU / dirty list
// ---------------------------------------------------------------------------
impl Pcache {
    fn push_lru(p: *mut Pcache, page: *mut Page) {
        Pcache::spin_assert_holding(p);
        Pcache::page_lock_assert_holding(page);
        let pcnode = Pcache::page_get_node(page);
        assert_msg(!pcnode.is_null(), b"push_lru: no node\0");
        assert_msg(PcacheNode::dirty(pcnode) == 0, b"push_lru: dirty\0");
        assert_msg(PcacheNode::pcache(pcnode) == p, b"push_lru: pcache mismatch\0");
        assert_msg(PcacheNode::page(pcnode) == page, b"push_lru: page mismatch\0");
        assert_msg(Pcache::page_ref_count(page) == 1, b"push_lru: ref != 1\0");
        assert_msg(PcacheNode::lru_detached(pcnode) != 0,
            b"push_lru: already in list\0");
        unsafe { NodeHandle::new(pcnode) }.push_lru(p);
        unsafe { PcacheHandle::new(p) }.inc_lru_count();
    }



    fn pop_lru(p: *mut Pcache) -> *mut Page {
        Pcache::spin_assert_holding(p);
        if unsafe { PcacheHandle::new(p) }.lru_is_empty() {
            return ptr::null_mut();
        }
        loop {
            let pcnode = (unsafe { PcacheHandle::new(p) }.lru_last());
            if pcnode.is_null() {
                return ptr::null_mut();
            }
            let page = PcacheNode::page(pcnode);
            assert_msg(!page.is_null(), b"pop_lru: no page\0");
            let guard = Pcache::lock_page(page);
            if PcacheNode::lru_detached(pcnode) != 0 {
                drop(guard);
                continue;
            }
            assert_msg(PcacheNode::pcache(pcnode) == p,
                b"pop_lru: pcache mismatch\0");
            Pcache::dec_lru_count(p);
            assert_msg(Pcache::lru_count(p) >= 0, b"pop_lru: count underflow\0");
            PcacheNode::detach_lru(pcnode);
            // Returned page-locked — ownership handed to the caller.
            guard.forget();
            return page;
        }
    }



    fn remove_lru(p: *mut Pcache, page: *mut Page) {
        Pcache::spin_assert_holding(p);
        Pcache::page_lock_assert_holding(page);
        let pcnode = Pcache::page_get_node(page);
        assert_msg(!pcnode.is_null(), b"remove_lru: no node\0");
        assert_msg(PcacheNode::page(pcnode) == page, b"remove_lru: page mismatch\0");
        assert_msg(PcacheNode::pcache(pcnode) == p,
            b"remove_lru: pcache mismatch\0");
        assert_msg(PcacheNode::lru_detached(pcnode) == 0,
            b"remove_lru: not in list\0");
        PcacheNode::detach_lru(pcnode);
        if PcacheNode::dirty(pcnode) != 0 {
            Pcache::dec_dirty_count(p);
            assert_msg(Pcache::dirty_count(p) >= 0,
                b"remove_lru: dirty count underflow\0");
        } else {
            Pcache::dec_lru_count(p);
            assert_msg(Pcache::lru_count(p) >= 0,
                b"remove_lru: lru count underflow\0");
        }
    }



    fn push_dirty(p: *mut Pcache, page: *mut Page) {
        Pcache::spin_assert_holding(p);
        Pcache::page_lock_assert_holding(page);
        let pcnode = Pcache::page_get_node(page);
        assert_msg(!pcnode.is_null(), b"push_dirty: no node\0");
        assert_msg(PcacheNode::dirty(pcnode) != 0,
            b"push_dirty: not dirty\0");
        assert_msg(PcacheNode::pcache(pcnode) == p,
            b"push_dirty: pcache mismatch\0");
        assert_msg(PcacheNode::page(pcnode) == page,
            b"push_dirty: page mismatch\0");
        if PcacheNode::lru_detached(pcnode) != 0 {
            unsafe { PcacheHandle::new(p) }.inc_dirty_count();
        } else {
            PcacheNode::detach_lru(pcnode);
        }
        unsafe { NodeHandle::new(pcnode) }.push_dirty(p);
    }



    fn pop_dirty(p: *mut Pcache, latest_flush_jiffs: u64) -> *mut Page {
        Pcache::spin_assert_holding(p);
        if unsafe { PcacheHandle::new(p) }.dirty_is_empty() {
            return ptr::null_mut();
        }
        loop {
            let pcnode = (unsafe { PcacheHandle::new(p) }.dirty_last());
            if pcnode.is_null() {
                return ptr::null_mut();
            }
            let page = PcacheNode::page(pcnode);
            assert_msg(!page.is_null(), b"pop_dirty: no page\0");
            let guard = Pcache::lock_page(page);
            let last_flushed = (unsafe { NodeHandle::new(pcnode) }.last_flushed());
            if last_flushed > latest_flush_jiffs && latest_flush_jiffs != 0 {
                return ptr::null_mut();
            }
            if PcacheNode::lru_detached(pcnode) != 0 {
                drop(guard);
                continue;
            }
            assert_msg(PcacheNode::pcache(pcnode) == p,
                b"pop_dirty: pcache mismatch\0");
            assert_msg(PcacheNode::dirty(pcnode) != 0,
                b"pop_dirty: not dirty\0");
            assert_msg(PcacheNode::io_in_progress(pcnode) == 0,
                b"pop_dirty: io in progress\0");
            Pcache::dec_dirty_count(p);
            assert_msg(Pcache::dirty_count(p) >= 0,
                b"pop_dirty: dirty count underflow\0");
            PcacheNode::detach_lru(pcnode);
            // Returned page-locked — ownership handed to the caller.
            guard.forget();
            return page;
        }
    }



    fn evict_lru(p: *mut Pcache) -> *mut Page {
        let page = Pcache::pop_lru(p);
        if page.is_null() { return ptr::null_mut(); }
        let pcnode = Pcache::page_get_node(page);
        Pcache::remove_node(p, page);
        Pcache::node_detach_page(p, page);
        Pcache::page_set_node(page, ptr::null_mut());
        PcacheNode::set_page(pcnode, ptr::null_mut());
        Pcache::page_lock_release(page);
        PcacheNode::free(pcnode);
        page
    }


    // ===========================================================================
    // Public API
    // ===========================================================================


    pub(crate) fn global_init() {
        Pcache::globals_init();
        let ret = Pcache::node_slab_init();
        assert_msg(ret == 0, b"Failed to initialize pcache node slab\0");
        let wq = (Workqueue::create(PCACHE_FLUSH_WQ_NAME.as_ptr() as *const c_char, WORKQUEUE_DEFAULT_MAX_ACTIVE));
        assert_msg(!wq.is_null(), b"Failed to create global pcache flush workqueue\0");
        Pcache::set_flush_wq(wq);
        crate::kprintln!("Page cache subsystem initialized");
        RawCompletion::init(Pcache::global_completion());
        Pcache::global_flusher_complete_all();
        Pcache::create_flusher_thread();
    }



    pub(crate) fn init(p: *mut Pcache)-> c_int  {
        // C-ABI boundary: convert `Result<(), Errno>` to a negative-errno
        // `c_int` exactly once, here.
        if let Err(e) = Pcache::init_validate(p) {
            return e.neg();
        }
        unsafe { PcacheHandle::new(p) }.list_entries_init();
        Pcache::set_dirty_count(p, 0);
        Pcache::set_lru_count(p, 0);
        Pcache::set_page_count(p, 0);
        unsafe { PcacheHandle::new(p) }.set_flags(0);
        unsafe { PcacheHandle::new(p) }.rb_init();
        if (unsafe { PcacheHandle::new(p) }.gfp_flags()) == 0 {
            unsafe { PcacheHandle::new(p) }.set_gfp_flags(0);
        }
        unsafe { PcacheHandle::new(p) }.spin_init();
        unsafe { PcacheHandle::new(p) }.tree_lock_init();
        unsafe { PcacheHandle::new(p) }.flush_completion_init();
        Pcache::flush_completion_complete_all(p);
        unsafe { PcacheHandle::new(p) }.set_private_data(ptr::null_mut());
        Pcache::set_flush_error(p, 0);
        unsafe { PcacheHandle::new(p) }.set_wait_refcount(0);
        Pcache::set_active(p, 1);
        Pcache::set_flush_requested(p, 0);
        if Pcache::max_pages(p) == 0 {
            unsafe { PcacheHandle::new(p) }.set_max_pages(PCACHE_DEFAULT_MAX_PAGES);
        }
        let rate = Pcache::dirty_rate(p);
        if rate == 0 || rate > 100 {
            unsafe { PcacheHandle::new(p) }.set_dirty_rate(PCACHE_DEFAULT_DIRTY_RATE);
        }
        let now = Pcache::get_jiffs();
        Pcache::set_last_flushed(p, now);
        Pcache::set_last_request(p, now);
        Pcache::register(p);
        0
    }



    pub(crate) fn teardown(p: *mut Pcache) {
        if p.is_null() { return; }

        // 1. unregister
        {
            let _gg = Pcache::lock_global();
            let _gp = Pcache::lock_local(p);
            if Pcache::list_entry_is_detached(p) == 0 {
                _gp.detach_global();
                Pcache::dec_global_count();
            }
        }

        // 2. wait wait_refcount drain
        {
            let _gp = Pcache::lock_local(p);
            while _gp.wait_refcount() > 0 {
                sleep_on_chan(p as *mut c_void, Pcache::spinlock(p));
            }
        }

        // 3. mark inactive
        let flush_pending = {
            let _gp = Pcache::lock_local(p);
            Pcache::set_active(p, 0);
            let flush_pending = Pcache::flush_requested(p) != 0;
            Pcache::wakeup_on_chan(p as *mut c_void);
            flush_pending
        };

        if flush_pending {
            let _ = Pcache::wait_flush_complete(p);
        }

        // 4. evict clean LRU pages
        {
            let _gp = Pcache::lock_local(p);
            loop {
                let victim = Pcache::evict_lru(p);
                if victim.is_null() { break; }
                Pcache::page_put(victim);
            }
        }

        // 5. drain remaining rb-tree
        {
            let _gp = Pcache::lock_local(p);
            let _gt = Pcache::wlock_tree(p);
            loop {
                let node = _gt.rb_first();
                if node.is_null() { break; }
                Pcache::rb_delete(p, node);
                let pg = PcacheNode::page(node);
                if !pg.is_null() {
                    let _pg_lock = Pcache::lock_page(pg);
                    if PcacheNode::lru_detached(node) == 0 {
                        PcacheNode::detach_lru(node);
                    }
                    Pcache::page_set_node(pg, ptr::null_mut());
                    Pcache::page_set_pcache(pg, ptr::null_mut());
                    PcacheNode::set_page(node, ptr::null_mut());
                    drop(_pg_lock);
                    Pcache::page_put(pg);
                }
                PcacheNode::free(node);
            }
            Pcache::set_page_count(p, 0);
            Pcache::set_lru_count(p, 0);
            Pcache::set_dirty_count(p, 0);
        }
    }



    pub(crate) fn get_page(p: *mut Pcache, blkno: u64)-> *mut Page  {
        if p.is_null() || !Pcache::is_active(p) { return ptr::null_mut(); }
        let base_blkno = Pcache::align_blkno(blkno);
        let blk_count = Pcache::blk_count(p);
        if base_blkno >= blk_count { return ptr::null_mut(); }
        if base_blkno + Pcache::blks_per_page() as u64 > blk_count {
            return ptr::null_mut();
        }

        'retry_lookup: loop {
            let page = Pcache::get_page_internal(p, base_blkno, Pcache::pgsize(), ptr::null_mut());
            if !page.is_null() {
                let _gp = Pcache::lock_local(p);
                let _pg = Pcache::lock_page(page);
                if !Pcache::page_valid(p, page) {
                    continue 'retry_lookup;
                }
                let pcnode = Pcache::page_get_node(page);
                assert_msg(!pcnode.is_null(), b"get_page: missing pcache node\0");
                if PcacheNode::blkno(pcnode) != base_blkno {
                    continue 'retry_lookup;
                }
                if PcacheNode::dirty(pcnode) == 0
                    && PcacheNode::lru_detached(pcnode) == 0
                {
                    Pcache::remove_lru(p, page);
                }
                let r = Pcache::page_ref_inc_unlocked(page);
                if r < 0 {
                    continue 'retry_lookup;
                }
                return page;
            }

            // Alloc fresh page
            let new_page = Pcache::page_alloc();
            if new_page.is_null() { return ptr::null_mut(); }

            let mut new_page_guard = Pcache::lock_page(new_page);
            let new_pcnode = Pcache::page_get_node(new_page);
            assert_msg(!new_pcnode.is_null(), b"get_page: new page no node\0");
            unsafe { NodeHandle::new(new_pcnode) }.set_blkno(base_blkno);
            PcacheNode::set_dirty(new_pcnode, 0);
            PcacheNode::set_uptodate(new_pcnode, 0);
            PcacheNode::set_io_in_progress(new_pcnode, 0);
            PcacheNode::set_size(new_pcnode, Pcache::pgsize());

            let _gp = Pcache::lock_local(p);

            if Pcache::max_pages(p) > 0 {
                while Pcache::page_count(p) as u64 >= Pcache::max_pages(p) {
                    let victim = Pcache::evict_lru(p);
                    if !victim.is_null() {
                        // Release `new_page`'s page lock across the victim
                        // free. Dropping the victim's last reference can free
                        // it back to the buddy allocator, whose merge step
                        // (`buddy_merge_and_insert` -> `lock_get_buddy`) locks
                        // the victim's physically-adjacent buddy to inspect it
                        // -- and that buddy may *be* `new_page`. Holding
                        // `new_page`'s lock here would then re-enter it on the
                        // same hart ("spin_lock reentry"). `new_page` is not
                        // published anywhere yet (not in the tree, not
                        // attached), so releasing its lock momentarily is safe;
                        // it stays allocated (refcount >= 1, off the free
                        // list), so the buddy allocator inspects and correctly
                        // declines to merge it. Mirrors the existing
                        // drop/re-acquire around the sleep below.
                        drop(new_page_guard);
                        Pcache::page_put(victim);
                        new_page_guard = Pcache::lock_page(new_page);
                        continue;
                    }
                    if Pcache::dirty_count(p) > 0 {
                        Pcache::queue_work(p);
                    }
                    drop(new_page_guard);
                    let ret = (sleep_on_chan_interruptible(p as *mut c_void, Pcache::spinlock(p)));
                    new_page_guard = Pcache::lock_page(new_page);
                    if ret == -(EINTR as c_int) {
                        drop(new_page_guard);
                        drop(_gp);
                        Pcache::page_discard(new_page);
                        return ptr::null_mut();
                    }
                    if !Pcache::is_active(p) {
                        drop(new_page_guard);
                        drop(_gp);
                        Pcache::page_discard(new_page);
                        return ptr::null_mut();
                    }
                }
            }

            let page = Pcache::get_page_internal(p, base_blkno, Pcache::pgsize(), new_page);
            if page.is_null() {
                drop(new_page_guard);
                drop(_gp);
                Pcache::page_discard(new_page);
                return ptr::null_mut();
            }

            if page != new_page {
                drop(new_page_guard);
                drop(_gp);
                Pcache::page_discard(new_page);
                continue 'retry_lookup;
            }

            Pcache::node_attach_page(p, new_page);
            let r = Pcache::page_ref_inc_unlocked(new_page);
            assert_msg(r > 1, b"get_page: failed to add caller reference\0");
            return new_page;
        }
    }



    pub(crate) fn put_page(p: *mut Pcache, page: *mut Page) {
        if p.is_null() || page.is_null() { return; }

        let _gp = Pcache::lock_local(p);
        let _pg = Pcache::lock_page(page);

        if !Pcache::page_valid(p, page) {
            Pcache::printf_invalid_page(page, p);
            return;
        }

        let pcnode = Pcache::page_get_node(page);
        let refcount = Pcache::page_ref_count(page);
        if refcount < 2 {
            Pcache::printf_refcount_too_small(page, refcount);
            return;
        }
        let new_refcount = Pcache::page_ref_dec_unlocked(page);
        assert_msg(new_refcount >= 1, b"put_page: refcount underflow\0");

        if new_refcount == 1 {
            let dirty = PcacheNode::dirty(pcnode) != 0;
            let detached = PcacheNode::lru_detached(pcnode) != 0;
            let uptodate = PcacheNode::uptodate(pcnode) != 0;

            if dirty && detached {
                Pcache::push_dirty(p, page);
            } else if !dirty && detached {
                if !uptodate {
                    Pcache::remove_node(p, page);
                    Pcache::node_detach_page(p, page);
                    Pcache::page_set_node(page, ptr::null_mut());
                    PcacheNode::set_page(pcnode, ptr::null_mut());
                    Pcache::wakeup_on_chan(p as *mut c_void);
                    drop(_pg);
                    drop(_gp);
                    PcacheNode::free(pcnode);
                    Pcache::page_put(page);
                    return;
                }
                Pcache::push_lru(p, page);
                Pcache::wakeup_on_chan(p as *mut c_void);
            } else {
                if dirty {
                    assert_msg(!detached, b"put_page: dirty page lost from dirty list\0");
                } else if !uptodate {
                    if !detached {
                        Pcache::remove_lru(p, page);
                    }
                }
            }
        }
    }



    pub(crate) fn mark_page_dirty(p: *mut Pcache, page: *mut Page)-> c_int  {
        if p.is_null() || page.is_null() { return -(EINVAL as c_int); }
        let mut ret: c_int = 0;

        let _gp = Pcache::lock_local(p);
        let _pg = Pcache::lock_page(page);

        if !Pcache::page_valid(p, page) {
            ret = -(EINVAL as c_int);
        } else {
            let pcnode = Pcache::page_get_node(page);
            if PcacheNode::dirty(pcnode) != 0 {
                // already dirty
            } else if PcacheNode::io_in_progress(pcnode) != 0 {
                ret = -(EBUSY as c_int);
            } else {
                if PcacheNode::lru_detached(pcnode) == 0
                    && PcacheNode::dirty(pcnode) == 0
                {
                    Pcache::remove_lru(p, page);
                }
                PcacheNode::set_dirty(pcnode, 1);
                PcacheNode::set_uptodate(pcnode, 1);
                Pcache::ops_call_mark_dirty(p, page);
                Pcache::push_dirty(p, page);
            }
        }

        ret
    }



    pub(crate) fn invalidate_page(p: *mut Pcache, page: *mut Page)-> c_int  {
        if p.is_null() || page.is_null() { return -(EINVAL as c_int); }
        let mut ret: c_int = 0;
        let _gp = Pcache::lock_local(p);
        let _pg = Pcache::lock_page(page);
        if !Pcache::page_valid(p, page) {
            ret = -(EINVAL as c_int);
        } else {
            let pcnode = Pcache::page_get_node(page);
            if PcacheNode::io_in_progress(pcnode) != 0 {
                ret = -(EBUSY as c_int);
            } else {
                if PcacheNode::lru_detached(pcnode) == 0 {
                    Pcache::remove_lru(p, page);
                }
                if PcacheNode::dirty(pcnode) != 0 {
                    PcacheNode::set_dirty(pcnode, 0);
                }
                PcacheNode::set_uptodate(pcnode, 0);
            }
        }
        ret
    }



    pub(crate) fn invalidate_blk(p: *mut Pcache, blkno: u64)-> c_int  {
        if p.is_null() || !Pcache::is_active(p) { return -(EINVAL as c_int); }
        let base_blkno = Pcache::align_blkno(blkno);

        let _gp = Pcache::lock_local(p);
        let page = Pcache::get_page_internal(p, base_blkno, Pcache::pgsize(), ptr::null_mut());
        if page.is_null() {
            return 0;
        }
        let _pg = Pcache::lock_page(page);
        if !Pcache::page_valid(p, page) {
            return 0;
        }
        let pcnode = Pcache::page_get_node(page);
        if PcacheNode::blkno(pcnode) != base_blkno {
            return 0;
        }
        if PcacheNode::io_in_progress(pcnode) != 0 {
            return -(EBUSY as c_int);
        }
        if PcacheNode::lru_detached(pcnode) == 0 {
            Pcache::remove_lru(p, page);
        }
        PcacheNode::set_dirty(pcnode, 0);
        PcacheNode::set_uptodate(pcnode, 0);
        0
    }



    pub(crate) fn discard_blk(p: *mut Pcache, blkno: u64)-> c_int  {
        if p.is_null() || !Pcache::is_active(p) { return -(EINVAL as c_int); }
        let base_blkno = Pcache::align_blkno(blkno);

        let _gp = Pcache::lock_local(p);
        let page = Pcache::get_page_internal(p, base_blkno, Pcache::pgsize(), ptr::null_mut());
        if page.is_null() {
            return 0;
        }
        let _pg = Pcache::lock_page(page);
        if !Pcache::page_valid(p, page) {
            return 0;
        }
        let pcnode = Pcache::page_get_node(page);
        if PcacheNode::blkno(pcnode) != base_blkno {
            return 0;
        }
        if PcacheNode::io_in_progress(pcnode) != 0 {
            return -(EBUSY as c_int);
        }
        if PcacheNode::lru_detached(pcnode) == 0 {
            Pcache::remove_lru(p, page);
        }
        {
            let _gt = Pcache::wlock_tree(p);
            Pcache::rb_delete(p, pcnode);
            Pcache::add_page_count(p, -1);
        }
        Pcache::page_set_node(page, ptr::null_mut());
        Pcache::page_set_pcache(page, ptr::null_mut());
        PcacheNode::set_page(pcnode, ptr::null_mut());
        drop(_pg);
        drop(_gp);
        PcacheNode::free(pcnode);
        Pcache::page_put(page);
        0
    }



    pub(crate) fn flush(p: *mut Pcache)-> c_int  {
        if p.is_null() { return -(EINVAL as c_int); }
        let _gp = Pcache::lock_local(p);
        if !Pcache::is_active(p) {
            return -(EINVAL as c_int);
        }
        let queued = Pcache::queue_work(p);
        if !queued {
            Pcache::set_flush_requested(p, 0);
            Pcache::set_flush_error(p, -(EAGAIN as c_int));
            return -(EAGAIN as c_int);
        }
        drop(_gp);
        Pcache::wait_flush_complete(p)
    }



    pub(crate) fn sync()-> c_int  {
        {
            let _gg = Pcache::lock_global();
            Pcache::flusher_start();
        }
        Pcache::wait_flusher()
    }



    pub(crate) fn read_page(p: *mut Pcache, page: *mut Page)-> c_int  {
        if p.is_null() || page.is_null() { return -(EINVAL as c_int); }

        'retry_locked: loop {
            let _gp = Pcache::lock_local(p);
            let _pg = Pcache::lock_page(page);

            if !Pcache::is_active(p) || !Pcache::page_valid(p, page) {
                return -(EINVAL as c_int);
            }
            let refcount = Pcache::page_ref_count(page);
            if refcount < 2 {
                Pcache::printf_read_refcount_too_small(page, refcount);
                return -(EINVAL as c_int);
            }
            let pcnode = Pcache::page_get_node(page);
            let blkno = PcacheNode::blkno(pcnode);
            let size = (unsafe { NodeHandle::new(pcnode) }.size_get());
            if blkno >= Pcache::blk_count(p) || size == 0 || size > Pcache::pgsize() {
                Pcache::printf_read_invalid_meta(page, blkno, size);
                return -(EINVAL as c_int);
            }

            if PcacheNode::io_in_progress(pcnode) != 0 {
                let dirty = PcacheNode::dirty(pcnode);
                let uptodate = PcacheNode::uptodate(pcnode);
                drop(_pg);
                drop(_gp);
                if uptodate != 0 { return 0; }
                if dirty == 0 && uptodate == 0 {
                    let _ = Pcache::node_io_wait(p, page);
                    continue 'retry_locked;
                }
                Pcache::printf_io_unexpected(dirty, uptodate);
                return -(EIO as c_int);
            }

            if PcacheNode::uptodate(pcnode) != 0 {
                return 0;
            }

            let ret = Pcache::node_io_begin(p, page);
            assert_msg(ret == 0, b"read_page: io_begin failed\0");

            drop(_pg);
            drop(_gp);

            let mut wait_for_completion = false;
            let mut ret = Pcache::ops_call_read_page(p, page);
            if ret == -(EINPROGRESS as c_int) {
                wait_for_completion = true;
                ret = 0;
            } else if ret != 0 {
                let _ = Pcache::node_io_end(p, page);
                return ret;
            }

            if wait_for_completion {
                let r = Pcache::node_io_wait(p, page);
                if r != 0 {
                    let _ = Pcache::node_io_end(p, page);
                    return r;
                }
            }

            let ret_post = {
                let _gp = Pcache::lock_local(p);
                let _pg = Pcache::lock_page(page);
                let mut ret_post: c_int = 0;
                if !Pcache::page_valid(p, page) {
                    ret_post = -(EINVAL as c_int);
                } else {
                    let pcnode = Pcache::page_get_node(page);
                    if PcacheNode::lru_detached(pcnode) == 0 {
                        Pcache::remove_lru(p, page);
                    }
                    PcacheNode::set_dirty(pcnode, 0);
                    PcacheNode::set_uptodate(pcnode, 1);
                }
                ret_post
            };
            let _ = Pcache::node_io_end(p, page);
            return ret_post;
        }
    }


    // ===========================================================================
    // Diagnostics and syscall handlers
    // ===========================================================================


    pub(crate) fn dump_stats(p: *mut Pcache) {
        if p.is_null() { return; }
        let _gp = Pcache::lock_local(p);
        Pcache::printf_stats_header(p);
        Pcache::printf_stats_body(
            Pcache::active(p),
            Pcache::blk_count(p),
            Pcache::dirty_count(p),
            Pcache::lru_count(p),
            Pcache::page_count(p),
            Pcache::max_pages(p) as i64,
            Pcache::dirty_rate(p) as c_int,
            Pcache::flush_requested(p),
            Pcache::flush_error(p),
        );
    }



    pub(crate) fn dump_all_stats() {
        let _gg = Pcache::lock_global();
        Pcache::printf_dump_all_header(_gg.global_count());
        for cur in Pcache::global_iter() {
            Pcache::dump_stats(cur);
        }
    }



    pub(crate) fn shrink_caches() {
        unsafe { crate::mm::slab::slab_cache_shrink(Pcache::node_slab() as *mut crate::mm::slab::SlabCache, 0x7fff_ffff) };
    }
}

pub(crate) extern "C" fn sys_sync()-> u64  {
    let ret = Pcache::sync();
    if ret != 0 {
        Pcache::printf_sys_sync_failed(ret);
    }
    0
}

pub(crate) extern "C" fn sys_dumppcache()-> u64  {
    Pcache::dump_all_stats();
    0
}