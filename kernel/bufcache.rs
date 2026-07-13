//! Classic xv6 buffer cache -- Rust port of `kernel/bio.c` (Phase 2 Wave
//! 22; see `docs/rustify/phase2_plan.md` and its §0 "Two distinct bio
//! concepts" note).
//!
//! **Not to be confused with [`crate::dev::bio`]** (`kernel/dev/bio.c`
//! -> `kernel/dev/bio.rs`), the Linux-style ref-counted block-I/O
//! request descriptor. This file is the *other* "bio": the original
//! xv6 `struct buf` disk-block cache -- a fixed-size, LRU-ish pool of
//! `NBUF` cache-line-aligned buffers, keyed by `(dev, blockno)` through
//! the Wave-1 Rust `hlist`, with a free list (O(1) LRU recycling) and a
//! dirty list (deferred writeback). Named `bufcache.rs`, not `bio.rs`,
//! specifically to avoid colliding with the sibling module's file name
//! -- every C-ABI symbol this file exports keeps its exact original
//! name (`binit`, `bread`, `bwrite`, `bwrite_async`, `bsync`,
//! `bdirty_count`, `brelse`, `bpin`, `bunpin`), matching every existing
//! extern declaration across the tree (`kernel/vfs/xv6fs/*.rs`,
//! `kernel/inc/defs.h`) unchanged.
//!
//! # Locking order (unchanged from the C original)
//!
//! 1. `bcache.lock` (spinlock) -- protects the LRU/dirty lists and the
//!    hash table. Held only for short, non-sleeping critical sections
//!    ([`crate::sync::KSpinlock`] RAII).
//! 2. `buf.lock` (mutex) -- protects one buffer's contents; `bread()`
//!    returns with it held, `brelse()` releases it. This lock crosses
//!    function-call (often thread) boundaries by design -- the same
//!    reason `kernel/vfs/inode.rs`'s `vfs_ilock`/`vfs_iunlock` and
//!    `kernel/mm/vm.rs`'s `vm_rlock`/`vm_runlock` stay thin
//!    `mutex_lock`/`mutex_unlock` forwards rather than
//!    `crate::sync::KMutex` RAII: no stack-scoped guard can span two
//!    separate C-ABI calls. Every acquire/release site below mirrors
//!    the C original's exact placement.
//! 3. Disk I/O completion, awaited via [`bio_await`] (this file's own
//!    reimplementation of `dev/bio.h`'s `static inline int
//!    bio_await(struct bio *)` -- see below).
//!
//! `bread()` may block (mutex sleep, then disk I/O wait); callers must
//! not hold other sleeping locks that the disk interrupt/completion path
//! could need.
//!
//! # Data structures
//!
//! `BCACHE` mirrors the C file-scope anonymous `struct { spinlock_t
//! lock; struct buf buf[NBUF]; list_node_t free_list, dirty_list; uint
//! dirty_count; hlist_t cached; hlist_bucket_t buckets[BIO_HASH_BUCKETS];
//! } bcache;` field-for-field, including the `hlist_t` + immediately-
//! adjacent `buckets` array trick: `hlist_t`'s own `buckets` member is a
//! zero-sized `__IncompleteArrayField` (bindgen's flexible-array-member
//! encoding), so laying a real `[list_node_t; BIO_HASH_BUCKETS]` field
//! directly after `cached: hlist_t` in this struct reproduces the same
//! layout `kernel/hlist.rs`'s `bucket_at()` (`buckets.as_mut_ptr()`,
//! i.e. "the address right after the header") expects -- identical to
//! the precedent already established by `kernel/proc/proc_shims.rs`'s
//! `ProcTable`.
//!
//! Storage is `static mut ... MaybeUninit<BCache> = MaybeUninit::zeroed()`
//! (same idiom as `kernel/console.rs`'s `CONSOLE_CDEV` /
//! `kernel/tty/ptmx.rs`'s `PTMX_CDEV`). Unlike those two, the zero
//! *value* is not just "never read before real init" here -- `bget()`'s
//! buffer-recycling path genuinely depends on a not-yet-recycled buffer
//! reading back `dev == 0 && blockno == 0` (the C static's implicit BSS
//! zero-init), so `zeroed()` (not `uninit()`) is required for
//! correctness, not merely permitted for soundness. `buf`/`list_node_t`/
//! `hlist_t`/`mutex_t`/`spinlock_t` are all plain integers and raw
//! pointers (no references, no niche types), so the all-zero bit pattern
//! is a valid value for every field -- sound by construction.
//!
//! `NBUF` (`param.h`, `MAXOPBLOCKS * 300` = 24000) and `BIO_HASH_BUCKETS`
//! (`dev/buf.h`, 24007) are plain `#define`s with no corresponding C
//! declaration for bindgen to capture; hand-mirrored as local constants,
//! same established per-file convention as `kernel/vfs/xv6fs/mod.rs`'s
//! `MAXOPBLOCKS`/`BSIZE`. `BSIZE` (`vfs/xv6fs/ondisk.h`, 1024) is
//! likewise re-declared locally rather than reached through
//! `crate::vfs::xv6fs` -- this file is not part of that driver, and the
//! C original's own `#include "vfs/xv6fs/ondisk.h" // for BSIZE` comment
//! already signals it's borrowing, not owning, the constant.
//!
//! # `hlist.h`/`list.h` `static inline` primitives
//!
//! Neither header has external linkage for the primitives this file
//! needs (`hlist_hash_uint64`, `list_node_push_front/back`,
//! `list_node_pop_front`, `LIST_NODE_IS_DETACHED`, `LIST_IS_EMPTY`) --
//! reimplemented locally below, reusing `crate::machine`'s existing
//! `list_entry_{init,detach,is_detached,insert_after,next,prev}`
//! primitives (the crate-wide canonical `list_node_t` helpers, already
//! used by `mm/vm.rs`/`timer/timer_core.rs`) rather than hand-rolling a
//! third copy of the raw pointer arithmetic. `crate::mm::cffi::
//! container_of` (the crate-wide generic `container_of`, already used
//! cross-module by `backtrace.rs`/`vfs/fs.rs`/`tty/ptmx.rs`) recovers a
//! `*mut buf` from a `*mut list_node_t`/`*mut hlist_entry_t` member
//! pointer.
//!
//! # `dev/bio.h`'s `bio_await` -- reimplemented, not called
//!
//! `bio_await()` is `static inline` in `dev/bio.h` (see
//! `kernel/dev/bio.rs`'s module doc) -- this file reimplements it
//! natively ([`bio_await`] below), matching its C body exactly.
//!
//! # Panic-message fidelity (deliberate simplification)
//!
//! The C original's invariant-violation sites use `panic(fmt, ...)`/
//! `assert(cond, fmt, ...)` (`kernel/inc/printf.h`), which prints an
//! `ASSERTION_FAILURE %s:%d: In function '%s':\n` preamble before the
//! caller's message. This port calls the crate's canonical
//! `xv6_panic(msg)` entry point instead (`kernel/proc/proc_shims.rs`,
//! already the standard C-ABI panic path used by ~14 other files
//! including `kernel/kobject.rs`'s own `kassert!`) -- same
//! simplification precedent, dropping only the boilerplate file/line/
//! function header. Every dynamic diagnostic value the C original
//! printed (buffer dev/blockno, `blkdev_put`'s error code) is still
//! printed via `printf` immediately before the panic, so no diagnostic
//! information is lost, only the redundant location preamble.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_int, c_void};
use core::mem::offset_of;
use core::sync::atomic::Ordering;

use crate::bindings::{
    bio, blkdev_t, bool_, buf, completion_t, hlist_entry_t, hlist_func_t, hlist_t, list_node_t,
    mutex_t, page_t, spinlock_t, PGSIZE,
};
use crate::machine;
use crate::mm::cffi::container_of;
use crate::sync::KSpinlock;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block, matching the
// established convention (`kernel/dev/blkdev.rs`, `kernel/vfs/inode.rs`,
// `kernel/vfs/xv6fs/superblock.rs`) of declaring cross-module C-ABI
// calls locally rather than reaching through a shared facade, even for
// symbols defined elsewhere in this same crate.
// ---------------------------------------------------------------------------

unsafe extern "C" {
    safe fn xv6_panic(msg: *const core::ffi::c_char) -> !;
    safe fn printf(fmt: *const core::ffi::c_char, ...) -> c_int;

    // kernel/hlist.rs (Phase 2 Wave 1). Non-RCU variants: this file uses
    // `bcache.lock` for mutual exclusion, not RCU (matches the C
    // original -- `kernel/dev/fdt.c`, still C, is the only other live
    // caller of these same three entry points, via the RCU-agnostic
    // non-`_rcu` names).
    safe fn hlist_init(hlist: *mut hlist_t, bucket_cnt: u64, func: *mut hlist_func_t) -> i32;
    safe fn hlist_get(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void;
    safe fn hlist_put(hlist: *mut hlist_t, node: *mut c_void, replace: bool) -> *mut c_void;
    safe fn hlist_pop(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void;

    // kernel/lock/mutex.rs.
    safe fn mutex_init(m: *mut mutex_t, name: *mut core::ffi::c_char);
    safe fn mutex_lock(m: *mut mutex_t);
    safe fn mutex_unlock(m: *mut mutex_t);
    safe fn holding_mutex(m: *mut mutex_t) -> c_int;

    // kernel/lock/completion.rs.
    safe fn wait_for_completion(c: *mut completion_t);
    safe fn wait_for_completion_interruptible(c: *mut completion_t) -> c_int;

    // kernel/mm/page.rs.
    safe fn page_alloc(order: u64, flags: u64) -> *mut c_void;
    safe fn __pa_to_page(physical: u64) -> *mut page_t;

    // kernel/dev/dev.rs + kernel/dev/blkdev.rs (Phase 2 Wave 21).
    safe fn blkdev_get(major: c_int, minor: c_int) -> *mut blkdev_t;
    safe fn blkdev_put(dev: *mut blkdev_t) -> c_int;
    safe fn blkdev_submit_bio(blkdev: *mut blkdev_t, bio: *mut bio) -> c_int;

    // kernel/dev/bio.rs (this wave's sibling file).
    safe fn bio_alloc(
        bdev: *mut blkdev_t,
        vec_length: i16,
        rw: bool_,
        end_io: Option<unsafe extern "C" fn(bio: *mut bio)>,
        private_data: *mut c_void,
    ) -> *mut bio;
    safe fn bio_add_seg(bio: *mut bio, page: *mut page_t, idx: i16, len: u16, offset: u16) -> c_int;
    safe fn bio_release(bio: *mut bio) -> c_int;
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

/// `ERR_PTR`/`IS_ERR`/`IS_ERR_OR_NULL` (`kernel/inc/errno.h`), generic
/// over the pointee type. Reimplemented locally, matching this crate's
/// established per-file convention (see `kernel/dev/blkdev.rs`).
const MAX_ERRNO: isize = 4095;
#[inline(always)]
fn is_err_value(p: usize) -> bool {
    p >= (-(MAX_ERRNO)) as usize
}
#[inline(always)]
fn is_err<T>(p: *mut T) -> bool {
    is_err_value(p as usize)
}
#[inline(always)]
fn is_err_or_null<T>(p: *mut T) -> bool {
    p.is_null() || is_err(p)
}

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
    machine::list_entry_is_detached(entry as *const list_node_t)
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
    machine::list_entry_insert_after(head, node);
}

/// Mirrors `list_node_push_back(head, node, member)`.
#[inline(always)]
fn ln_push_back(head: *mut list_node_t, node: *mut list_node_t) {
    let tail = machine::list_entry_prev(head);
    machine::list_entry_insert_after(tail, node);
}

/// Mirrors `list_node_pop_front(head, type, member)`: detaches and
/// returns the first node, or null if `head` is empty.
#[inline(always)]
fn ln_pop_front(head: *mut list_node_t) -> *mut list_node_t {
    let first = machine::list_entry_next(head);
    if first == head {
        return core::ptr::null_mut();
    }
    machine::list_entry_detach(first);
    first
}

// ---------------------------------------------------------------------------
// Storage -- see module doc's "Data structures" section.
// ---------------------------------------------------------------------------

#[repr(C)]
struct BCache {
    lock: spinlock_t,
    buf: [buf; NBUF],
    free_list: list_node_t,
    dirty_list: list_node_t,
    dirty_count: u32,
    cached: hlist_t,
    buckets: [list_node_t; BIO_HASH_BUCKETS],
}

static mut BCACHE: core::mem::MaybeUninit<BCache> = core::mem::MaybeUninit::zeroed();

#[inline(always)]
fn bc() -> *mut BCache {
    // SAFETY: `BCACHE` is `'static` storage; taking its address never
    // fails. Every field access through the returned pointer is
    // separately justified at its call site (spinlock-protected, or
    // one-time boot init per `binit`'s contract).
    unsafe { BCACHE.as_mut_ptr() }
}

#[inline(always)]
fn bcache_lock() -> KSpinlock {
    KSpinlock::from_bindings(unsafe { &raw mut (*bc()).lock })
}

// ---------------------------------------------------------------------------
// hlist callbacks, registered with `hlist_init` in `binit`. Mirrors
// `__bcache_hash_func`/`__bcache_hlist_get_node`/`__bcache_hlist_get_entry`/
// `__bcache_hlist_cmp`.
// ---------------------------------------------------------------------------

unsafe extern "C" fn bcache_hash_func(node: *mut c_void) -> u64 {
    let bnode = node as *mut buf;
    // SAFETY: `node` is either `bget`'s stack-local lookup key or a live
    // registered buffer -- both are valid `buf` pointers whose
    // `dev`/`blockno` fields are readable.
    let h = unsafe {
        hlist_hash_uint64((*bnode).blockno as u64).wrapping_add((*bnode).dev as u64)
    };
    hlist_hash_uint64(h)
}

unsafe extern "C" fn bcache_hlist_get_node(entry: *mut hlist_entry_t) -> *mut c_void {
    container_of::<buf, hlist_entry_t>(entry, offset_of!(buf, hlist_entry)) as *mut c_void
}

unsafe extern "C" fn bcache_hlist_get_entry(node: *mut c_void) -> *mut hlist_entry_t {
    let bnode = node as *mut buf;
    // SAFETY: see `bcache_hash_func`.
    unsafe { &raw mut (*bnode).hlist_entry }
}

unsafe extern "C" fn bcache_hlist_cmp(
    _hlist: *mut hlist_t,
    node1: *mut c_void,
    node2: *mut c_void,
) -> c_int {
    let b1 = node1 as *mut buf;
    let b2 = node2 as *mut buf;
    // SAFETY: see `bcache_hash_func`.
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

// ---------------------------------------------------------------------------
// `dev/bio.h`'s `bio_await` -- reimplemented natively (see module doc).
// ---------------------------------------------------------------------------

fn bio_await(bio_ptr: *mut bio) -> c_int {
    /// `EINTR` (`kernel/inc/errno.h`).
    const EINTR: c_int = 4;
    // SAFETY: `bio_ptr` is a live bio this file just submitted and owns
    // exclusively until this wait completes; `io_completion` is a plain
    // embedded field.
    let ret = unsafe { wait_for_completion_interruptible(&raw mut (*bio_ptr).io_completion) };
    if ret == -EINTR {
        unsafe { wait_for_completion(&raw mut (*bio_ptr).io_completion) };
        let err = unsafe { (*bio_ptr).error };
        return if err != 0 { err } else { -EINTR };
    }
    unsafe { (*bio_ptr).error }
}

// ---------------------------------------------------------------------------
// bio helpers. Mirrors `__buf_alloc_bio`/`__buf_bio_cleanup`.
// ---------------------------------------------------------------------------

fn buf_alloc_bio(b: *mut buf, blkdev: *mut blkdev_t, write: bool) -> *mut bio {
    let bio_ptr = bio_alloc(blkdev, 1, write as bool_, None, core::ptr::null_mut());
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
        let ret = bio_add_seg(bio_ptr, page, 0, BSIZE as u16, page_offset);
        if ret != 0 {
            bio_release(bio_ptr);
            return core::ptr::null_mut();
        }
    }
    bio_ptr
}

fn buf_bio_cleanup(bio_ptr: *mut bio) {
    if !bio_ptr.is_null() {
        bio_release(bio_ptr);
    }
}

/// Print + panic on an unexpected `blkdev_put` failure (`bread`/`bwrite`'s
/// `assert(ret == 0, "...: blkdev_put failed: %d", ret)` sites -- see
/// module doc's "Panic-message fidelity" note).
fn assert_blkdev_put_ok(ret: c_int) {
    if ret != 0 {
        printf(c"blkdev_put failed: %d\n".as_ptr(), ret);
        xv6_panic(c"blkdev_put failed".as_ptr());
    }
}

// ---------------------------------------------------------------------------
// Preallocation. Mirrors `__buf_cache_prealloc`.
// ---------------------------------------------------------------------------

fn buf_cache_prealloc() {
    let page_blocks = (PGSIZE / BSIZE) as usize;
    let pages_needed = NBUF.div_ceil(page_blocks);
    let bc_ptr = bc();
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
            // SAFETY: `buf_idx < NBUF`; `pa` is a freshly allocated page
            // this buffer slot now owns permanently (buffer-cache pages
            // are never freed, matching the C original).
            unsafe {
                (*bc_ptr).buf[buf_idx].data = pa.add(j * BSIZE as usize);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public C ABI -- exact symbol/signature parity with `kernel/inc/defs.h`.
// ---------------------------------------------------------------------------

/// Initialise the buffer cache. Must be called exactly once, at boot,
/// before any other entry point in this file.
#[no_mangle]
pub extern "C" fn binit() {
    let bc_ptr = bc();
    bcache_lock().init(c"bcache".as_ptr());
    // SAFETY: called exactly once at boot (function contract above) --
    // no concurrent access to `BCACHE` is possible yet.
    unsafe {
        machine::list_entry_init(&raw mut (*bc_ptr).free_list);
        machine::list_entry_init(&raw mut (*bc_ptr).dirty_list);
        (*bc_ptr).dirty_count = 0;

        let mut func = hlist_func_t {
            hash: Some(bcache_hash_func),
            get_node: Some(bcache_hlist_get_node),
            get_entry: Some(bcache_hlist_get_entry),
            cmp_node: Some(bcache_hlist_cmp),
        };
        hlist_init(&raw mut (*bc_ptr).cached, BIO_HASH_BUCKETS as u64, &raw mut func);

        for i in 0..NBUF {
            let b: *mut buf = &raw mut (*bc_ptr).buf[i];
            machine::list_entry_init(&raw mut (*b).free_entry);
            machine::list_entry_init(&raw mut (*b).dirty_entry);
            (*b).dirty = 0;
            mutex_init(&raw mut (*b).lock, c"buffer".as_ptr() as *mut core::ffi::c_char);
            ln_push_back(&raw mut (*bc_ptr).free_list, &raw mut (*b).free_entry);
        }
    }
    buf_cache_prealloc();
}

/// Look through the buffer cache for block `(dev, blockno)`. If not
/// found, recycle the least-recently-used free buffer. Returns a
/// *locked* buffer either way (mirrors `bget`).
fn bget(dev: u32, blockno: u32) -> *mut buf {
    let bc_ptr = bc();
    let guard = bcache_lock().lock();

    // Is the block already cached?
    let mut key = lookup_key(dev, blockno);
    // SAFETY: `bc_ptr` is valid `'static` storage; `key` is a
    // stack-local lookup key alive for this call.
    let b = unsafe {
        hlist_get(&raw mut (*bc_ptr).cached, (&mut key as *mut buf) as *mut c_void) as *mut buf
    };
    if !b.is_null() {
        // Found it. Remove from free list if it's there (refcnt was 0).
        // SAFETY: `b` is a live, registered buffer.
        unsafe {
            let fe = &raw mut (*b).free_entry;
            if !ln_is_detached(fe) {
                machine::list_entry_detach(fe);
            }
            (*b).refcnt += 1;
        }
        drop(guard);
        unsafe { mutex_lock(&raw mut (*b).lock) };
        return b;
    }

    // Not cached. Get a free buffer from the free list (O(1), oldest
    // free buffer first for LRU behavior).
    if ln_is_empty(unsafe { &raw mut (*bc_ptr).free_list }) {
        xv6_panic(c"bget: no buffers".as_ptr());
    }
    let free_node = ln_pop_front(unsafe { &raw mut (*bc_ptr).free_list });
    let b: *mut buf = container_of(free_node, offset_of!(buf, free_entry));

    // Remove from hash table if it was caching a different block.
    // SAFETY: `b` is the just-recycled buffer, exclusively owned here.
    let mut old_key = unsafe { lookup_key((*b).dev, (*b).blockno) };
    let b1 = unsafe {
        hlist_pop(&raw mut (*bc_ptr).cached, (&mut old_key as *mut buf) as *mut c_void) as *mut buf
    };
    if !b1.is_null() && b1 != b {
        // SAFETY: `b`/`b1` both live buffers.
        unsafe {
            if (*b).blockno != 0 || (*b).dev != 0 {
                // Only unused buffers could clash, otherwise it is a bug.
                printf(
                    c"bget: found a buffer with blockno %d, dev %d, but it is not the same as the one we are recycling\n"
                        .as_ptr(),
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
        let ret = unsafe { hlist_put(&raw mut (*bc_ptr).cached, b1 as *mut c_void, false) };
        if !ret.is_null() {
            xv6_panic(c"bget: failed to push cached buffer into hash list".as_ptr());
        }
    }

    // Ensure the buffer is detached before using it (mirrors the C
    // original's explicit `__atomic_thread_fence(__ATOMIC_SEQ_CST)`).
    core::sync::atomic::fence(Ordering::SeqCst);

    // SAFETY: `b` is exclusively owned (just popped from the free list
    // under `bcache.lock`, and not yet re-published into the hash
    // table).
    unsafe {
        (*b).dev = dev;
        (*b).blockno = blockno;
        (*b).valid = 0;
        (*b).refcnt = 1;
    }
    let ret = unsafe { hlist_put(&raw mut (*bc_ptr).cached, b as *mut c_void, false) };
    if !ret.is_null() {
        printf(c"dev: %d, blockno: %d\n".as_ptr(), dev, blockno);
        xv6_panic(c"bget: failed to push recycled buffer into hash list".as_ptr());
    }
    drop(guard);
    unsafe { mutex_lock(&raw mut (*b).lock) };
    b
}

/// Return a locked buffer with the contents of the indicated block.
/// Returns null on OOM (bio allocation failure) or I/O error/interrupt
/// -- callers must handle this gracefully.
#[no_mangle]
pub extern "C" fn bread(dev: u32, blockno: u32) -> *mut buf {
    let b = bget(dev, blockno);
    // SAFETY: `b` is locked (mutex held by this thread, per `bget`'s
    // postcondition) for the remainder of this function.
    let valid = unsafe { (*b).valid } != 0;
    if !valid {
        let blkdev = blkdev_get(dev_major(unsafe { (*b).dev }), dev_minor(unsafe { (*b).dev }));
        if is_err(blkdev) {
            xv6_panic(c"bread: blkdev_get failed".as_ptr());
        }
        let bio_ptr = buf_alloc_bio(b, blkdev, false);
        if is_err_or_null(bio_ptr) {
            // OOM during bio allocation -- release buffer and return
            // NULL. Callers should handle this gracefully.
            let ret = blkdev_put(blkdev);
            assert_blkdev_put_ok(ret);
            brelse(b);
            return core::ptr::null_mut();
        }
        let mut err = blkdev_submit_bio(blkdev, bio_ptr);
        if err == 0 {
            err = bio_await(bio_ptr);
        }
        buf_bio_cleanup(bio_ptr);
        let ret = blkdev_put(blkdev);
        assert_blkdev_put_ok(ret);
        if err != 0 {
            // I/O error or interrupted -- don't mark valid, release
            // buffer.
            brelse(b);
            return core::ptr::null_mut();
        }
        unsafe { (*b).valid = 1 };
    }
    b
}

/// Write `b`'s contents to disk. Must be locked.
#[no_mangle]
pub extern "C" fn bwrite(b: *mut buf) {
    // SAFETY: `b` caller-owned; lock-holding checked immediately below,
    // matching the C precondition.
    unsafe {
        if holding_mutex(&raw mut (*b).lock) == 0 {
            xv6_panic(c"bwrite".as_ptr());
        }
    }
    let dev = unsafe { (*b).dev };
    let blkdev = blkdev_get(dev_major(dev), dev_minor(dev));
    if is_err(blkdev) {
        xv6_panic(c"bwrite: blkdev_get failed".as_ptr());
    }
    let bio_ptr = buf_alloc_bio(b, blkdev, true);
    if is_err_or_null(bio_ptr) {
        xv6_panic(c"bwrite: bio_alloc failed".as_ptr());
    }
    blkdev_submit_bio(blkdev, bio_ptr);
    bio_await(bio_ptr);
    buf_bio_cleanup(bio_ptr);

    // Clear dirty flag after successful write.
    {
        let _g = bcache_lock().lock();
        // SAFETY: `b` live; `bc()` valid `'static` storage.
        unsafe {
            if (*b).dirty != 0 {
                (*b).dirty = 0;
                let de = &raw mut (*b).dirty_entry;
                if !ln_is_detached(de) {
                    machine::list_entry_detach(de);
                    (*bc()).dirty_count -= 1;
                }
            }
        }
    }

    let ret = blkdev_put(blkdev);
    assert_blkdev_put_ok(ret);
}

/// Mark buffer as dirty for later writeback. Must be locked. Much
/// faster than [`bwrite`] since it doesn't block on disk I/O.
#[no_mangle]
pub extern "C" fn bwrite_async(b: *mut buf) {
    // SAFETY: see `bwrite`.
    unsafe {
        if holding_mutex(&raw mut (*b).lock) == 0 {
            xv6_panic(c"bwrite_async".as_ptr());
        }
    }
    let _g = bcache_lock().lock();
    // SAFETY: `b` live, `bcache.lock` held.
    unsafe {
        if (*b).dirty == 0 {
            (*b).dirty = 1;
            // Add to dirty list (at head for FIFO writeback order via
            // `ln_pop_front` in `bsync`, matching the C's
            // `list_node_push_front`).
            ln_push_front(&raw mut (*bc()).dirty_list, &raw mut (*b).dirty_entry);
            (*bc()).dirty_count += 1;
        }
    }
}

/// Flush all dirty buffers to disk. Called periodically or on `sync()`.
#[no_mangle]
pub extern "C" fn bsync() {
    loop {
        let guard = bcache_lock().lock();
        // SAFETY: `bcache.lock` held.
        let empty = unsafe { ln_is_empty(&raw mut (*bc()).dirty_list) };
        if empty {
            drop(guard);
            break;
        }
        let node = ln_pop_front(unsafe { &raw mut (*bc()).dirty_list });
        let b: *mut buf = container_of(node, offset_of!(buf, dirty_entry));
        // SAFETY: `b` live, `bcache.lock` held.
        unsafe {
            (*b).dirty = 0;
            (*bc()).dirty_count -= 1;

            // Increment refcnt to prevent buffer from being recycled.
            let fe = &raw mut (*b).free_entry;
            if (*b).refcnt == 0 && !ln_is_detached(fe) {
                machine::list_entry_detach(fe);
            }
            (*b).refcnt += 1;
        }
        drop(guard);

        // Lock buffer and write to disk.
        unsafe { mutex_lock(&raw mut (*b).lock) };

        let valid = unsafe { (*b).valid } != 0;
        if valid {
            let dev = unsafe { (*b).dev };
            let blkdev = blkdev_get(dev_major(dev), dev_minor(dev));
            if !is_err(blkdev) {
                let bio_ptr = buf_alloc_bio(b, blkdev, true);
                if !is_err_or_null(bio_ptr) {
                    blkdev_submit_bio(blkdev, bio_ptr);
                    bio_await(bio_ptr);
                    buf_bio_cleanup(bio_ptr);
                }
                blkdev_put(blkdev);
            }
        }

        unsafe { mutex_unlock(&raw mut (*b).lock) };

        // Release our reference.
        let guard2 = bcache_lock().lock();
        // SAFETY: `bcache.lock` held.
        unsafe {
            (*b).refcnt -= 1;
            if (*b).refcnt == 0 {
                ln_push_back(&raw mut (*bc()).free_list, &raw mut (*b).free_entry);
            }
        }
        drop(guard2);
    }
}

/// Get the count of dirty buffers (for debugging/stats).
#[no_mangle]
pub extern "C" fn bdirty_count() -> u32 {
    let _g = bcache_lock().lock();
    // SAFETY: `bcache.lock` held.
    unsafe { (*bc()).dirty_count }
}

/// Release a locked buffer. Move to the free list if no longer
/// referenced.
#[no_mangle]
pub extern "C" fn brelse(b: *mut buf) {
    // SAFETY: `b` caller-owned, lock-holding checked below.
    unsafe {
        if holding_mutex(&raw mut (*b).lock) == 0 {
            xv6_panic(c"brelse".as_ptr());
        }
        mutex_unlock(&raw mut (*b).lock);
    }
    let _g = bcache_lock().lock();
    // SAFETY: `b` live, `bcache.lock` held.
    unsafe {
        (*b).refcnt -= 1;
        if (*b).refcnt == 0 {
            // No one is waiting for it -- add to free list (most
            // recently used at head, oldest at tail).
            ln_push_back(&raw mut (*bc()).free_list, &raw mut (*b).free_entry);
        }
    }
}

/// Pin a buffer in the cache (prevent it from being recycled while
/// `refcnt == 0`).
#[no_mangle]
pub extern "C" fn bpin(b: *mut buf) {
    let _g = bcache_lock().lock();
    // SAFETY: `b` live, `bcache.lock` held.
    unsafe {
        let fe = &raw mut (*b).free_entry;
        if (*b).refcnt == 0 && !ln_is_detached(fe) {
            machine::list_entry_detach(fe);
        }
        (*b).refcnt += 1;
    }
}

/// Undo a prior [`bpin`].
#[no_mangle]
pub extern "C" fn bunpin(b: *mut buf) {
    let _g = bcache_lock().lock();
    // SAFETY: `b` live, `bcache.lock` held.
    unsafe {
        (*b).refcnt -= 1;
        if (*b).refcnt == 0 {
            ln_push_back(&raw mut (*bc()).free_list, &raw mut (*b).free_entry);
        }
    }
}
