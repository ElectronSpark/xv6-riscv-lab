//! Fixed-size disk-block cache, indexed by device and block number.
//!
//! The cache owns permanently allocated buffer pages. A lookup takes one
//! reference and returns its buffer mutex held; release unlocks and drops that
//! reference. Only clean, unreferenced buffers enter the recycling list. Dirty
//! buffers retain their identity and data until successful writeback.
//!
//! BCACHE's spinlock protects hash/list membership, dirty state and reference
//! counts. It is released before acquiring a buffer mutex or waiting for I/O.
//! Buffer payloads and validity are protected by their individual mutexes.
//! The permanent buffer array stays outside BCACHE so those mutexes can span
//! lookups, I/O and release without borrowing through a cache spinlock guard.
//!
//! A sleeping flush gate serializes complete writeback passes across mounts.
//! Callers release their buffer locks before entering it. A failed pass retains
//! dirty data, releases its references and gate, and returns an error; journal
//! callers must stop before publishing or clearing a transaction header.
//!
//! BIO creation and waiting use the shared dev::bio lifecycle. The cache pins
//! each buffer page and holds its device/BIO references until all transfers
//! stop, including when waiting is interrupted. Raw buffer-pointer interfaces
//! still require the existing caller-side lock and reference discipline.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_int, c_void};
use core::mem::offset_of;
use core::sync::atomic::Ordering;

use crate::bindings::{
    bio, blkdev_t, bool_, buf, completion_t, hlist_entry_t, hlist_t, list_node_t, mutex_t, page_t,
    PGSIZE,
};
use crate::hlist::HlistOps;
use crate::machine;
use crate::mm::cffi::container_of;
use crate::sync::SpinLock;

use crate::proc::proc_shims::xv6_panic;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N4.
//
// `Buf` IS the kernel-wide Rust definition of `kernel/inc/dev/buf.h`'s
// `struct buf` now: `build.rs` blocklists the bindgen-generated form and
// injects `pub use crate::bufcache::Buf as buf;` (no `_t` typedef exists),
// so every `crate::bindings::buf` path across the crate resolves here.
// The explicit `_pad0` field reproduces bindgen's `__bindgen_padding_0:
// [u64; 5]` (`blockno`+4 → 8-aligned 24 → 64): the C `mutex_t` *typedef*
// carries `__ALIGNED_CACHELINE`, and its native `RawMutex` is genuinely
// 128/64, so `lock` lands at 64 either way — the field is kept for
// byte-for-byte form fidelity with the pre-nativization output. The
// struct-level align(64) is C's own `__ALIGNED_CACHELINE` on `struct
// buf` itself (it also pads the size 264 → 320). Copy fidelity: `buf`
// derived Copy/Clone in the pre-nativization bindgen output (kept —
// `lookup_key` below returns a `buf` by value).
// ---------------------------------------------------------------------------

/// `struct buf` (`kernel/inc/dev/buf.h`).
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct Buf {
    pub valid: c_int,
    pub disk: c_int,
    pub dirty: c_int,
    pub dev: crate::bindings::dev_t,
    pub blockno: crate::bindings::uint,
    pub(crate) _pad0: [u64; 5],
    pub lock: mutex_t,
    pub refcnt: crate::bindings::uint,
    pub hlist_entry: hlist_entry_t,
    pub free_entry: list_node_t,
    pub dirty_entry: list_node_t,
    pub data: *mut crate::bindings::uchar,
}

// P3-N4 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct buf
// { valid/disk/dirty: c_int, dev: dev_t, blockno: uint,
// __bindgen_padding_0: [u64; 5], lock: mutex_t, refcnt: uint,
// hlist_entry: hlist_entry_t, free_entry: list_node_t, dirty_entry:
// list_node_t, data: *mut uchar }`, `#[repr(align(64))]`) and
// independently confirmed by a riscv64-unknown-elf-gcc `_Static_assert`
// probe (rv64gc/lp64d) against `kernel/inc/dev/buf.h` — see the P3-N4
// wave record: buf 320/64, offsets 0/4/8/12/16/64/192/200/224/240/256
// (mutex_t itself proven 128/64 by the same probe).
const _: () = {
    assert!(core::mem::size_of::<mutex_t>() == 128, "mutex_t size");
    assert!(core::mem::align_of::<mutex_t>() == 64, "mutex_t alignment");
    assert!(core::mem::size_of::<Buf>() == 320, "buf size");
    assert!(core::mem::align_of::<Buf>() == 64, "buf alignment");
    assert!(core::mem::offset_of!(Buf, valid) == 0, "buf.valid offset");
    assert!(core::mem::offset_of!(Buf, disk) == 4, "buf.disk offset");
    assert!(core::mem::offset_of!(Buf, dirty) == 8, "buf.dirty offset");
    assert!(core::mem::offset_of!(Buf, dev) == 12, "buf.dev offset");
    assert!(core::mem::offset_of!(Buf, blockno) == 16, "buf.blockno offset");
    assert!(core::mem::offset_of!(Buf, lock) == 64, "buf.lock offset");
    assert!(core::mem::offset_of!(Buf, refcnt) == 192, "buf.refcnt offset");
    assert!(core::mem::offset_of!(Buf, hlist_entry) == 200, "buf.hlist_entry offset");
    assert!(core::mem::offset_of!(Buf, free_entry) == 224, "buf.free_entry offset");
    assert!(core::mem::offset_of!(Buf, dirty_entry) == 240, "buf.dirty_entry offset");
    assert!(core::mem::offset_of!(Buf, data) == 256, "buf.data offset");
};

// P3-1D mesh sweep: kernel/dev/{dev,blkdev,bio}.rs are in scope for this
// wave; signatures are identical, so these become plain crate-path
// imports instead of `extern "C"` redeclarations.
use crate::dev::blkdev::Blkdev;
use crate::dev::bio::Bio;

#[cfg(feature = "bio_test")]
pub(crate) mod runtime_test;
pub(crate) use crate::hlist::Hlist;
pub(crate) use crate::lock::completion::RawCompletion;
pub(crate) use crate::lock::mutex::RawMutex;

// `page_alloc`/`__pa_to_page` are genuinely `unsafe fn` in `crate::mm::page`
// (`pub(crate)` since P3-D3a); this file's original extern
// declaration asserted `pub safe fn` (this crate's usual "collapse the
// FFI boilerplate" facade) and additionally typed `__pa_to_page`'s return
// as `*mut page_t` (the bindgen type `bio_add_seg` below still expects)
// rather than `crate::mm::page::Page` (that module's own struct — same
// layout, different Rust name). Thin wrappers preserve both.
/// SAFETY: see [`crate::mm::page::page_alloc`]'s contract.
fn page_alloc(order: u64, flags: u64) -> *mut c_void {
    unsafe { crate::mm::page::Page::page_alloc(order, flags) }
}
/// SAFETY: see [`crate::mm::page::__pa_to_page`]'s contract.
fn __pa_to_page(physical: u64) -> *mut page_t {
    unsafe { crate::mm::page::Page::__pa_to_page(physical) as *mut page_t }
}

// ---------------------------------------------------------------------------
// Local constants -- see module doc's "Data structures" section.
// ---------------------------------------------------------------------------

/// `MAXOPBLOCKS` (`param.h`).
const MAXOPBLOCKS: u32 = 80;
/// `NBUF` (`param.h`, `MAXOPBLOCKS * 300`).
const NBUF: usize = (MAXOPBLOCKS * 300) as usize;
/// `BIO_HASH_BUCKETS` (`dev/buf.h`).
const BIO_HASH_BUCKETS: usize = 24007;
/// `BSIZE` (`vfs/xv6fs/ondisk.h`).
const BSIZE: u32 = 1024;
/// `PAGE_TYPE_ANON` (`mm/page_type.h`).
const PAGE_TYPE_ANON: u64 = 0;
/// `PAGE_MASK` (`mm/page.rs`'s `PAGE_SIZE - 1`; `PAGE_SIZE == PGSIZE`).
const PAGE_MASK: u64 = (PGSIZE as u64) - 1;
/// `GOLDEN_RATIO_PRIME_64` (`dev/hlist.h`).
const GOLDEN_RATIO_PRIME_64: u64 = 0x9e37fffffffc0001;

// `is_err`/`is_err_or_null`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::{is_err, is_err_or_null};

/// Mirrors `major(dev)` (`kernel/inc/defs.h`).
#[inline(always)]
fn dev_major(dev: u32) -> c_int {
    ((dev >> 20) & 0xFFF) as c_int
}
/// Mirrors `minor(dev)` (`kernel/inc/defs.h`).
#[inline(always)]
fn dev_minor(dev: u32) -> c_int {
    (dev & 0xFFFFF) as c_int
}

/// Mirrors `dev/hlist.h`'s `static inline ht_hash_t
/// hlist_hash_uint64(uint64 key)`.
#[inline(always)]
fn hlist_hash_uint64(key: u64) -> u64 {
    let ret = key.wrapping_mul(GOLDEN_RATIO_PRIME_64);
    if ret == 0 {
        GOLDEN_RATIO_PRIME_64
    } else {
        ret
    }
}

// ---------------------------------------------------------------------------
// `list.h` `static inline` primitives this file needs, composed from
// `crate::machine`'s canonical `list_node_t` building blocks (see
// module doc).
// ---------------------------------------------------------------------------

/// Mirrors `LIST_NODE_IS_DETACHED`/`LIST_IS_EMPTY` (same formula: a
/// self-linked node is either a detached entry or an empty list head).
#[inline(always)]
fn ln_is_detached(entry: *mut list_node_t) -> bool {
    machine::Riscv::list_entry_is_detached(entry as *const list_node_t)
}

/// Mirrors `LIST_IS_EMPTY(head)` -- same formula as [`ln_is_detached`],
/// distinct name kept for readability at "is this list empty" call
/// sites (`bget`'s free list, `bsync`'s dirty list).
#[inline(always)]
fn ln_is_empty(head: *mut list_node_t) -> bool {
    ln_is_detached(head)
}

/// Mirrors `list_node_push_front(head, node, member)`.
#[inline(always)]
fn ln_push_front(head: *mut list_node_t, node: *mut list_node_t) {
    machine::Riscv::list_entry_insert_after(head, node);
}

/// Mirrors `list_node_push_back(head, node, member)`.
#[inline(always)]
fn ln_push_back(head: *mut list_node_t, node: *mut list_node_t) {
    let tail = machine::Riscv::list_entry_prev(head);
    machine::Riscv::list_entry_insert_after(tail, node);
}

/// Mirrors `list_node_pop_front(head, type, member)`: detaches and
/// returns the first node, or null if `head` is empty.
#[inline(always)]
fn ln_pop_front(head: *mut list_node_t) -> *mut list_node_t {
    let first = machine::Riscv::list_entry_next(head);
    if first == head {
        return core::ptr::null_mut();
    }
    machine::Riscv::list_entry_detach(first);
    first
}

// ---------------------------------------------------------------------------
// Storage -- see module doc's "Data structures" section.
// ---------------------------------------------------------------------------

/// The lock-protected header: hash table + free/dirty lists. See module
/// doc's "Data structures" section for the full layout rationale
/// (`#[repr(C)]` field order/adjacency of `cached`/`buckets` is
/// load-bearing) and why the `buf` array lives outside this type.
#[repr(C)]
struct BCacheHash {
    free_list: list_node_t,
    dirty_list: list_node_t,
    dirty_count: u32,
    cached: hlist_t,
    buckets: [list_node_t; BIO_HASH_BUCKETS],
}

// SAFETY: these links refer to the fixed kernel buffer pool. Every list
// and hash-table access holds BCACHE; moving the metadata between harts
// transfers no thread-local ownership or unsynchronized pointee access.
unsafe impl Send for BCacheHash {}

/// Placeholder value for [`BCACHE`]'s `const fn SpinLock::new` -- not a
/// meaningful "initialized" state; see module doc.
const fn zeroed_bcache_hash() -> BCacheHash {
    // SAFETY: `BCacheHash`'s fields (`list_node_t`, `hlist_t`) are plain
    // integers/raw pointers/`Option<fn>` (null-niche), no references --
    // the all-zero bit pattern is a valid value for the type. Real
    // initialisation (`list_entry_init`/`hlist_init`) happens in
    // `binit`, matching the original static's implicit BSS zero-init
    // (module doc).
    unsafe { core::mem::MaybeUninit::zeroed().assume_init() }
}

static BCACHE: SpinLock<BCacheHash> = SpinLock::new(c"bcache", zeroed_bcache_hash());

/// Serializes sleeping flush passes across all mounted filesystems. An empty
/// dirty list must not let a second caller return while the first has popped
/// its buffers but has not yet completed their writes.
struct FlushGate {
    raw: core::cell::UnsafeCell<core::mem::MaybeUninit<mutex_t>>,
    ready: core::sync::atomic::AtomicBool,
}

// SAFETY: storage is permanent, initialized once before publication, and all
// subsequent mutation follows RawMutex's internal synchronization protocol.
unsafe impl Sync for FlushGate {}

impl FlushGate {
    /// # Safety
    /// Initialize once during exclusive buffer-cache boot setup.
    unsafe fn init(&'static self) {
        let raw = self.raw.get().cast::<mutex_t>();
        // SAFETY: exclusive aligned storage. RawMutex contains only integer,
        // raw-pointer and lock fields for which zero is a valid initial value.
        unsafe { raw.write_bytes(0, 1) };
        RawMutex::init(raw, c"bcache_flush".as_ptr().cast_mut());
        self.ready.store(true, Ordering::Release);
    }

    fn lock(&'static self) -> crate::sync::KMutexGuard {
        assert!(self.ready.load(Ordering::Acquire), "buffer cache not initialized");
        crate::sync::KMutex::from_ptr(self.raw.get().cast()).lock()
    }
}

static FLUSH_GATE: FlushGate = FlushGate {
    raw: core::cell::UnsafeCell::new(core::mem::MaybeUninit::uninit()),
    ready: core::sync::atomic::AtomicBool::new(false),
};

/// The `buf` array -- kept outside [`BCACHE`] (see module doc's "Data
/// structures" section for why). Same zero-init rationale as before:
/// `bget`'s buffer-recycling path depends on a not-yet-recycled buffer
/// reading back `dev == 0 && blockno == 0`.
static mut BUF_STORAGE: core::mem::MaybeUninit<[buf; NBUF]> = core::mem::MaybeUninit::zeroed();

/// Zero-sized marker hosting every whole-cache (no single-buffer receiver)
/// operation -- the hash table + free/dirty lists + `BUF_STORAGE` array,
/// as opposed to [`Buf`]'s per-buffer methods. Same namespacing role
/// `IrqCore`/`SessionTable` play elsewhere in the crate.
pub(crate) struct BufCache;

impl BufCache {
    /// Address of buffer slot `i`. Every call site indexes with `i < NBUF`
    /// (this file's own invariant, matching the original `(*bc_ptr).buf[i]`
    /// C-style indexing it replaces).
    #[inline(always)]
    fn at(i: usize) -> *mut buf {
        // SAFETY: `BUF_STORAGE` is `'static` storage; taking a derived
        // address never fails. Callers keep `i < NBUF` (documented above);
        // dereferencing the returned pointer is separately justified at
        // each call site (spinlock-protected, mutex-protected, or one-time
        // boot init per `BufCache::init`'s contract).
        unsafe { BUF_STORAGE.as_mut_ptr().cast::<buf>().add(i) }
    }
}

// ---------------------------------------------------------------------------
// TRAIT-OPS: the old `hlist_func_t` fn-pointer table (`bcache_hash_func`/
// `bcache_hlist_get_node`/`bcache_hlist_get_entry`/`bcache_hlist_cmp`,
// registered with `hlist_init` in `binit`) -> `BcacheHlistOps`, a ZST
// implementing `HlistOps`. Bodies are byte-identical to the old free fns;
// only the receiver (`&self`) and the `extern "C"`/`unsafe extern "C"`
// wrapper are gone.
// ---------------------------------------------------------------------------

struct BcacheHlistOps;

impl HlistOps for BcacheHlistOps {
    unsafe fn hash(&self, node: *mut c_void) -> u64 {
        let bnode = node as *mut buf;
        // SAFETY: `node` is either `bget`'s stack-local lookup key or a live
        // registered buffer -- both are valid `buf` pointers whose
        // `dev`/`blockno` fields are readable.
        let h = unsafe {
            hlist_hash_uint64((*bnode).blockno as u64).wrapping_add((*bnode).dev as u64)
        };
        hlist_hash_uint64(h)
    }

    unsafe fn get_node(&self, entry: *mut hlist_entry_t) -> *mut c_void {
        container_of::<buf, hlist_entry_t>(entry, offset_of!(buf, hlist_entry)) as *mut c_void
    }

    unsafe fn get_entry(&self, node: *mut c_void) -> *mut hlist_entry_t {
        let bnode = node as *mut buf;
        // SAFETY: see `hash`.
        unsafe { &raw mut (*bnode).hlist_entry }
    }

    unsafe fn cmp_node(&self, _hlist: *mut hlist_t, node1: *mut c_void, node2: *mut c_void) -> c_int {
        let b1 = node1 as *mut buf;
        let b2 = node2 as *mut buf;
        // SAFETY: see `hash`.
        unsafe {
            if (*b1).dev > (*b2).dev {
                return 1;
            }
            if (*b1).dev < (*b2).dev {
                return -1;
            }
            if (*b1).blockno > (*b2).blockno {
                return 1;
            }
            if (*b1).blockno < (*b2).blockno {
                return -1;
            }
        }
        0
    }
}

static BCACHE_HLIST_OPS: BcacheHlistOps = BcacheHlistOps;

impl Buf {
    /// Build a stack-local lookup key with the given `(dev, blockno)` and
    /// every other field zeroed. Mirrors `__bcache_hlist_get`/
    /// `__bcache_hlist_pop`'s `struct buf dummy = {0}; dummy.dev = dev;
    /// dummy.blockno = blockno;`.
    #[inline(always)]
    fn lookup_key(dev: u32, blockno: u32) -> buf {
        // SAFETY: `buf`'s fields are integers/raw pointers only (no
        // references, no niche types) -- the all-zero bit pattern is valid
        // for the type, matching the C stack-local `= {0}` initializer.
        let mut k: buf = unsafe { core::mem::MaybeUninit::zeroed().assume_init() };
        k.dev = dev;
        k.blockno = blockno;
        k
    }
}

// ---------------------------------------------------------------------------
// `dev/bio.h`'s `bio_await` -- reimplemented natively (see module doc).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// bio helpers. Mirrors `__buf_alloc_bio`/`__buf_bio_cleanup`. Relocated
// onto `impl Buf` (both take `b: *mut buf`/pair with a buffer's own
// read/write path); `bio_await`/`assert_blkdev_put_ok` just below stay
// FLOOR -- free fns with no natural `Buf`/`BufCache` receiver (the first
// is a file-local reimplementation of a foreign-header `static inline`
// keyed only on `*mut bio`, the second a generic panic helper keyed on a
// raw `c_int`).
// ---------------------------------------------------------------------------

impl Buf {
    fn alloc_bio(b: *mut buf, blkdev: *mut blkdev_t, write: bool) -> *mut bio {
        // SAFETY: the caller holds a live block-device reference.
        let bio_ptr = unsafe { Bio::alloc(blkdev, 1, write as bool_, None, core::ptr::null_mut()) };
        if is_err_or_null(bio_ptr) {
            return core::ptr::null_mut();
        }
        // SAFETY: `b` is the caller's live, locked buffer; `bio_ptr` is
        // freshly allocated and not yet visible to any other thread.
        unsafe {
            (*bio_ptr).blkno = (*b).blockno as u64 * (BSIZE / 512) as u64;
            let data = (*b).data as u64;
            let page = __pa_to_page(data & !PAGE_MASK);
            let page_offset = (data & PAGE_MASK) as u16;
            let ret = Bio::add_seg(bio_ptr, page, 0, BSIZE as u16, page_offset);
            if ret != 0 {
                Bio::release(bio_ptr);
                return core::ptr::null_mut();
            }
        }
        bio_ptr
    }

    fn bio_cleanup(bio_ptr: *mut bio) {
        if !bio_ptr.is_null() {
            // SAFETY: this helper consumes the caller's allocated BIO reference.
            unsafe { Bio::release(bio_ptr) };
        }
    }
}

/// Print + panic on an unexpected `blkdev_put` failure (`bread`/`bwrite`'s
/// `assert(ret == 0, "...: blkdev_put failed: %d", ret)` sites -- see
/// module doc's "Panic-message fidelity" note).
fn assert_blkdev_put_ok(ret: c_int) {
    if ret != 0 {
        crate::kprintln!("blkdev_put failed: {}", ret);
        xv6_panic(c"blkdev_put failed".as_ptr());
    }
}

// ---------------------------------------------------------------------------
// Preallocation. Mirrors `__buf_cache_prealloc`.
// ---------------------------------------------------------------------------

impl BufCache {
    fn prealloc() {
        let page_blocks = (PGSIZE / BSIZE) as usize;
        let pages_needed = NBUF.div_ceil(page_blocks);
        for i in 0..pages_needed {
            let pa = page_alloc(0, PAGE_TYPE_ANON) as *mut u8;
            if pa.is_null() {
                xv6_panic(c"__buf_cache_prealloc: page_alloc failed".as_ptr());
            }
            for j in 0..page_blocks {
                let buf_idx = i * page_blocks + j;
                if buf_idx >= NBUF {
                    break;
                }
                let b = Self::at(buf_idx);
                // SAFETY: `buf_idx < NBUF` (checked above); `pa` is a
                // freshly allocated page this buffer slot now owns
                // permanently (buffer-cache pages are never freed, matching
                // the C original).
                unsafe {
                    (*b).data = pa.add(j * BSIZE as usize);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Cache initialization, lookup and writeback. Per-buffer operations follow.
// ---------------------------------------------------------------------------

impl BufCache {
    /// Initialise the buffer cache.
    /// # Safety
    /// Call exactly once at boot before any other entry point in this file,
    /// with exclusive access to the cache and initialized page allocation.
    pub(crate) unsafe fn init() {
        // SAFETY: exclusive, once-only boot initialization, before any sync.
        unsafe { FLUSH_GATE.init() };
        // `BCACHE` starts life already in the "locked struct with zeroed
        // contents" state (`SpinLock::new` is a `const fn`) -- unlike the
        // pre-P3-8c version there is no separate `spin_init` call needed
        // before touching the guard.
        let mut bc = BCACHE.lock();
        // `list_entry_init`/`hlist_init` etc. below all take/dereference raw
        // `*mut` pointers derived from the held guard (`&raw mut bc.field`)
        // -- safe to form (address-of only), and called exactly once at
        // boot (function contract) before any other entry point in this
        // file can observe `BCACHE`/`BUF_STORAGE`, so no concurrent access
        // is possible yet.
        machine::Riscv::list_entry_init(&raw mut bc.free_list);
        machine::Riscv::list_entry_init(&raw mut bc.dirty_list);
        bc.dirty_count = 0;

        // SAFETY: `Hlist::init` is a genuinely unsafe FFI entry point (raw
        // hash-table-header initialisation); `&raw mut bc.cached` is live
        // storage reached through the held guard, `Some(&BCACHE_HLIST_OPS)`
        // a `'static` trait-object reference. The per-buffer loop below is
        // likewise safe only because `BufCache::init` runs exactly once at
        // boot, before any other entry point in this file can observe
        // `BCACHE`/`BUF_STORAGE` -- every `&raw mut (*b).field` is a raw
        // address-of, not an unchecked read/write, on `'static` storage
        // `i < NBUF` keeps in-bounds.
        unsafe {
            Hlist::init(&raw mut bc.cached, BIO_HASH_BUCKETS as u64, Some(&BCACHE_HLIST_OPS));

            for i in 0..NBUF {
                let b: *mut buf = Self::at(i);
                machine::Riscv::list_entry_init(&raw mut (*b).free_entry);
                machine::Riscv::list_entry_init(&raw mut (*b).dirty_entry);
                (*b).dirty = 0;
                RawMutex::init(&raw mut (*b).lock, c"buffer".as_ptr() as *mut core::ffi::c_char);
                ln_push_back(&raw mut bc.free_list, &raw mut (*b).free_entry);
            }
        }
        drop(bc);
        Self::prealloc();
    }

    /// Look through the buffer cache for block `(dev, blockno)`. If not
    /// found, recycle the least-recently-used free buffer. Returns a
    /// *locked* buffer either way (mirrors `bget`).
    fn get(dev: u32, blockno: u32) -> *mut buf {
        let mut bc = BCACHE.lock();

        // Is the block already cached?
        let mut key = Buf::lookup_key(dev, blockno);
        // SAFETY: `Hlist::get` is a genuinely unsafe FFI hash-table lookup;
        // `&raw mut bc.cached` is live `'static` storage reached through the
        // held guard, `key` is a stack-local lookup key alive for this call.
        let b = unsafe { Hlist::get(&raw mut bc.cached, (&mut key as *mut buf) as *mut c_void) } as *mut buf;
        if !b.is_null() {
            // Found it. Remove from free list if it's there (refcnt was 0).
            // SAFETY: `b` is a live, registered buffer (returned by
            // `Hlist::get` while `bc` is held).
            unsafe {
                let fe = &raw mut (*b).free_entry;
                if !ln_is_detached(fe) {
                    machine::Riscv::list_entry_detach(fe);
                }
                (*b).refcnt += 1;
            }
            // Release `bcache.lock` *before* acquiring the buffer's own
            // sleeplock -- see module doc's "Lock-ordering hazard" note.
            drop(bc);
            // SAFETY: `b` live.
            RawMutex::lock(unsafe { &raw mut (*b).lock });
            return b;
        }

        // Not cached. Get a free buffer from the free list (O(1), oldest
        // free buffer first for LRU behavior).
        if ln_is_empty(&raw mut bc.free_list) {
            xv6_panic(c"bget: no buffers".as_ptr());
        }
        let free_node = ln_pop_front(&raw mut bc.free_list);
        let b: *mut buf = container_of(free_node, offset_of!(buf, free_entry));

        // Remove from hash table if it was caching a different block.
        // SAFETY: `b` is the just-recycled buffer, exclusively owned here
        // (just popped from the free list while `bc` is held).
        let mut old_key = unsafe { Buf::lookup_key((*b).dev, (*b).blockno) };
        // SAFETY: see the `Hlist::get` call above.
        let b1 = unsafe { Hlist::pop(&raw mut bc.cached, (&mut old_key as *mut buf) as *mut c_void) } as *mut buf;
        if !b1.is_null() && b1 != b {
            // SAFETY: `b`/`b1` both live buffers.
            unsafe {
                if (*b).blockno != 0 || (*b).dev != 0 {
                    // Only unused buffers could clash, otherwise it is a bug.
                    crate::kprintln!(
                        "bget: found a buffer with blockno {}, dev {}, but it is not the same as the one we are recycling",
                        (*b1).blockno,
                        (*b1).dev,
                    );
                    xv6_panic(
                        c"bget: found a buffer that is not the same as the one we are recycling"
                            .as_ptr(),
                    );
                }
            }
            // The buffer b is unused, so we can put back b1 and safely use b.
            // SAFETY: see the `Hlist::get` call above.
            let ret = unsafe { Hlist::put(&raw mut bc.cached, b1 as *mut c_void, false) };
            if !ret.is_null() {
                xv6_panic(c"bget: failed to push cached buffer into hash list".as_ptr());
            }
        }

        // Ensure the buffer is detached before using it (mirrors the C
        // original's explicit `__atomic_thread_fence(__ATOMIC_SEQ_CST)`).
        core::sync::atomic::fence(Ordering::SeqCst);

        // SAFETY: `b` is exclusively owned (just popped from the free list
        // under `bc`, and not yet re-published into the hash table).
        unsafe {
            (*b).dev = dev;
            (*b).blockno = blockno;
            (*b).valid = 0;
            (*b).refcnt = 1;
        }
        // SAFETY: see the `Hlist::get` call above.
        let ret = unsafe { Hlist::put(&raw mut bc.cached, b as *mut c_void, false) };
        if !ret.is_null() {
            crate::kprintln!("dev: {}, blockno: {}", dev, blockno);
            xv6_panic(c"bget: failed to push recycled buffer into hash list".as_ptr());
        }
        // Release `bcache.lock` *before* acquiring the buffer's own
        // sleeplock -- see module doc's "Lock-ordering hazard" note (same
        // point the C original's `spin_unlock(&bcache.lock)` sat at).
        drop(bc);
        // SAFETY: `b` live.
        RawMutex::lock(unsafe { &raw mut (*b).lock });
        b
    }

    /// Flush dirty buffers, retaining failed writes and propagating errors.
    /// Callers must not hold buffer locks: the whole pass sleeps under a gate
    /// before taking individual buffer locks, then briefly takes BCACHE.
    pub(crate) fn sync() -> crate::kstd::KResult<()> {
        let _flush = FLUSH_GATE.lock();
        loop {
            let mut bc = BCACHE.lock();
            if ln_is_empty(&raw mut bc.dirty_list) {
                drop(bc);
                break;
            }
            let node = ln_pop_front(&raw mut bc.dirty_list);
            let b: *mut buf = container_of(node, offset_of!(buf, dirty_entry));
            // SAFETY: `b` live, `bc` held.
            unsafe { (*b).dirty = 0 };
            bc.dirty_count -= 1;

            // Increment refcnt to prevent buffer from being recycled.
            // SAFETY: `b` live, `bc` held.
            let (fe, refcnt_zero) = unsafe { (&raw mut (*b).free_entry, (*b).refcnt == 0) };
            if refcnt_zero && !ln_is_detached(fe) {
                machine::Riscv::list_entry_detach(fe);
            }
            // SAFETY: `b` live, `bc` held.
            unsafe { (*b).refcnt += 1 };
            // Release `bcache.lock` before the (potentially sleeping) mutex
            // acquire just below -- same ordering rule as `bget` (module
            // doc's "Lock-ordering hazard" note).
            drop(bc);

            // Lock buffer and write to disk.
            // SAFETY: `b` live.
            RawMutex::lock(unsafe { &raw mut (*b).lock });

            // SAFETY: `b` locked (mutex held by this thread, just above).
            let valid = unsafe { (*b).valid } != 0;
            let mut error = crate::kstd::Errno::Io.neg();
            if valid {
                // SAFETY: `b` locked.
                let dev = unsafe { (*b).dev };
                let blkdev = Blkdev::get(dev_major(dev), dev_minor(dev));
                if is_err(blkdev) {
                    error = crate::kstd::ptr_err(blkdev) as c_int;
                } else {
                    let bio_ptr = Buf::alloc_bio(b, blkdev, true);
                    if is_err_or_null(bio_ptr) {
                        error = crate::kstd::Errno::NoMem.neg();
                    } else {
                        // SAFETY: the locked, referenced buffer pins its page;
                        // both request and device references survive waiting.
                        error = unsafe { Blkdev::submit_bio(blkdev, bio_ptr) };
                        if error == 0 {
                            error = unsafe { Bio::wait(bio_ptr) };
                            // Cancellation was drained after submission.
                            if error == crate::kstd::Errno::Intr.neg() { error = 0; }
                        }
                        // wait drains DMA even when interrupted; no device
                        // error means the requested write still completed.
                        Buf::bio_cleanup(bio_ptr);
                    }
                    Blkdev::put(blkdev);
                }
            }

            if error != 0 {
                // Retain dirty data for a later sync instead of silently
                // discarding it. Stop this pass below to avoid an error loop.
                Buf::write_async(b);
            }

            // SAFETY: `b` live.
            RawMutex::unlock(unsafe { &raw mut (*b).lock });

            // Release our reference.
            let mut bc = BCACHE.lock();
            // SAFETY: `b` live, `bc` held.
            let (refcnt_zero, fe) = unsafe {
                (*b).refcnt -= 1;
                ((*b).refcnt == 0 && (*b).dirty == 0, &raw mut (*b).free_entry)
            };
            if refcnt_zero {
                ln_push_back(&raw mut bc.free_list, fe);
            }
            drop(bc);
            if error != 0 { return Err(crate::kstd::Errno::Raw(error)); }
        }
        Ok(())
    }

    /// Get the count of dirty buffers (for debugging/stats).
    pub(crate) fn dirty_count() -> u32 {
        BCACHE.lock().dirty_count
    }
}

impl Buf {
    /// Return a locked buffer with the contents of the indicated block.
    /// Returns null on OOM (bio allocation failure) or I/O error/interrupt
    /// -- callers must handle this gracefully.
    pub(crate) fn read(dev: u32, blockno: u32) -> *mut buf {
        let b = BufCache::get(dev, blockno);
        // SAFETY: `b` is locked (mutex held by this thread, per
        // `BufCache::get`'s postcondition) for the remainder of this
        // function.
        let valid = unsafe { (*b).valid } != 0;
        if !valid {
            let blkdev = Blkdev::get(dev_major(unsafe { (*b).dev }), dev_minor(unsafe { (*b).dev }));
            if is_err(blkdev) {
                xv6_panic(c"bread: blkdev_get failed".as_ptr());
            }
            let bio_ptr = Self::alloc_bio(b, blkdev, false);
            if is_err_or_null(bio_ptr) {
                // OOM during bio allocation -- release buffer and return
                // NULL. Callers should handle this gracefully.
                let ret = Blkdev::put(blkdev);
                assert_blkdev_put_ok(ret);
                Self::release(b);
                return core::ptr::null_mut();
            }
            // SAFETY: b is locked and its page stays pinned through the wait.
            let mut err = unsafe { Blkdev::submit_bio(blkdev, bio_ptr) };
            if err == 0 {
                err = unsafe { Bio::wait(bio_ptr) };
            }
            Self::bio_cleanup(bio_ptr);
            let ret = Blkdev::put(blkdev);
            assert_blkdev_put_ok(ret);
            if err != 0 {
                // I/O error or interrupted -- don't mark valid, release
                // buffer.
                Self::release(b);
                return core::ptr::null_mut();
            }
            unsafe { (*b).valid = 1 };
        }
        b
    }

    /// Write `b`'s contents to disk. Must be locked.
    pub(crate) fn write(b: *mut buf) {
        // `holding_mutex` is a safe FFI forward -- only the raw
        // address-of-field needs an (immediately-scoped) `unsafe`, matching
        // the C precondition.
        // SAFETY: `b` caller-owned.
        if RawMutex::is_holding(unsafe { &raw mut (*b).lock }) == 0 {
            xv6_panic(c"bwrite".as_ptr());
        }
        // SAFETY: `b` caller-owned and locked (just checked above).
        let dev = unsafe { (*b).dev };
        let blkdev = Blkdev::get(dev_major(dev), dev_minor(dev));
        if is_err(blkdev) {
            xv6_panic(c"bwrite: blkdev_get failed".as_ptr());
        }
        let bio_ptr = Self::alloc_bio(b, blkdev, true);
        if is_err_or_null(bio_ptr) {
            xv6_panic(c"bwrite: bio_alloc failed".as_ptr());
        }
        // SAFETY: b remains locked and referenced until all transfers finish.
        let mut error = unsafe { Blkdev::submit_bio(blkdev, bio_ptr) };
        if error == 0 {
            error = unsafe { Bio::wait(bio_ptr) };
            if error == crate::kstd::Errno::Intr.neg() { error = 0; }
        }
        Self::bio_cleanup(bio_ptr);
        let ret = Blkdev::put(blkdev);
        assert_blkdev_put_ok(ret);
        if error != 0 {
            // This synchronous interface cannot return an error to its
            // filesystem callers. Never report a failed write as clean.
            xv6_panic(c"bwrite: I/O failed".as_ptr());
        }

        // Clear dirty flag after successful write.
        {
            let mut bc = BCACHE.lock();
            // SAFETY: `b` live, `bc` held. `bc.dirty_count` -- a plain field
            // access through the guard -- needs no `unsafe`; nested here
            // only to match the C original's single conditional block.
            unsafe {
                if (*b).dirty != 0 {
                    (*b).dirty = 0;
                    let de = &raw mut (*b).dirty_entry;
                    if !ln_is_detached(de) {
                        machine::Riscv::list_entry_detach(de);
                        bc.dirty_count -= 1;
                    }
                }
            }
        }

    }

    /// Mark buffer as dirty for later writeback. Must be locked. Much
    /// faster than [`Buf::write`] since it doesn't block on disk I/O.
    pub(crate) fn write_async(b: *mut buf) {
        // See `write`: only the raw address-of-field needs `unsafe`.
        // SAFETY: `b` caller-owned.
        if RawMutex::is_holding(unsafe { &raw mut (*b).lock }) == 0 {
            xv6_panic(c"bwrite_async".as_ptr());
        }
        let mut bc = BCACHE.lock();
        // SAFETY: `b` live, `bc` held. `bc.dirty_list`/`bc.dirty_count` --
        // plain accesses through the guard -- need no `unsafe`; nested here
        // only to match the C original's single conditional block.
        unsafe {
            if (*b).dirty == 0 {
                (*b).dirty = 1;
                // Add to dirty list (at head for FIFO writeback order via
                // `ln_pop_front` in `BufCache::sync`, matching the C's
                // `list_node_push_front`).
                ln_push_front(&raw mut bc.dirty_list, &raw mut (*b).dirty_entry);
                bc.dirty_count += 1;
            }
        }
    }

    /// Release a locked buffer. Move to the free list if no longer
    /// referenced.
    pub(crate) fn release(b: *mut buf) {
        // SAFETY: `b` caller-owned; lock-holding checked, then released --
        // matches the C precondition, before `bcache.lock` is taken below
        // (`RawMutex::is_holding`/`RawMutex::unlock` are safe FFI forwards;
        // only the raw address-of-field genuinely needs `unsafe`).
        unsafe {
            if RawMutex::is_holding(&raw mut (*b).lock) == 0 {
                xv6_panic(c"brelse".as_ptr());
            }
            RawMutex::unlock(&raw mut (*b).lock);
        }

        let mut bc = BCACHE.lock();
        // SAFETY: `b` live, `bc` held. `bc.free_list` -- a plain access
        // through the guard -- needs no `unsafe`; nested here only to match
        // the C original's single conditional block.
        unsafe {
            (*b).refcnt -= 1;
            if (*b).refcnt == 0 && (*b).dirty == 0 {
                // Clean, unreferenced buffers may be recycled (most
                // recently used at head, oldest at tail).
                ln_push_back(&raw mut bc.free_list, &raw mut (*b).free_entry);
            }
        }
    }

    /// Pin a buffer in the cache (prevent it from being recycled while
    /// `refcnt == 0`).
    pub(crate) fn pin(b: *mut buf) {
        let _bc = BCACHE.lock();
        // SAFETY: `b` live, `_bc` held.
        unsafe {
            let fe = &raw mut (*b).free_entry;
            if (*b).refcnt == 0 && !ln_is_detached(fe) {
                machine::Riscv::list_entry_detach(fe);
            }
            (*b).refcnt += 1;
        }
    }

    /// Undo a prior [`Buf::pin`].
    pub(crate) fn unpin(b: *mut buf) {
        let mut bc = BCACHE.lock();
        // SAFETY: `b` live, `bc` held. `bc.free_list` -- a plain access
        // through the guard -- needs no `unsafe`; nested here only to match
        // the C original's single conditional block.
        unsafe {
            (*b).refcnt -= 1;
            if (*b).refcnt == 0 && (*b).dirty == 0 {
                ln_push_back(&raw mut bc.free_list, &raw mut (*b).free_entry);
            }
        }
    }
}
