//! VFS mount/superblock registry — Rust port of `kernel/vfs/fs.c` (Phase 2
//! Wave 16, see `docs/rustify/phase2_plan.md`).
//!
//! # Scope note (deviates from the plan's guess)
//!
//! The wave plan describes fs.c as "mount/superblock registry vs path
//! lookup". In the actual source tree `vfs_namei`/`vfs_nameiparent`/
//! `vfs_chdir`/`vfs_chroot`/`vfs_curdir`/`vfs_curroot` (the real
//! path-lookup machinery) already live in `vfs/inode.c` and were ported
//! to [`super::inode`] in Wave 13 — confirmed against the pre-Wave-13 C
//! (`git show HEAD:kernel/vfs/inode.c`). `fs.c` itself never defined
//! them. So this file has no separate "path lookup" half; it is the
//! filesystem-type registry, the mount/unmount state machine, the
//! superblock lock/refcount/mountcount API (the `vfs_superblock_{r,w}lock`
//! pair `inode.rs` already calls as an extern), the per-superblock inode
//! hash cache (`vfs_get_inode_cached`/`vfs_add_inode`/`vfs_remove_inode`),
//! dentry-to-inode resolution, `fs_struct` lifecycle, and the debug-dump
//! functions. The two build+boot checkpoints called for by the plan were
//! done as: (a) this file compiling and linking cleanly against every
//! existing extern declaration in `inode.rs`/`file.rs`/`pipe.rs`/
//! `fdtable.rs`/`thread.rs`/`clone.rs`/`exit.rs` (a pure symbol-contract
//! check — nothing else in the crate changed), then (b) full boot +
//! mount + path-battery verification (see the wave's final report).
//!
//! # Lock order (unchanged from the C original's own comment)
//!
//! 1. mount mutex, [`vfs_mount_lock`]/[`vfs_mount_unlock`] (this file).
//! 2. `vfs_superblock.lock` (rwsem), [`vfs_superblock_rlock`]/
//!    [`vfs_superblock_wlock`]/[`vfs_superblock_unlock`] (this file —
//!    the same paired-C-ABI-lock/unlock convention as `inode.rs`'s
//!    `vfs_ilock`/`vfs_iunlock`, since the matching unlock call often
//!    happens in a different function/translation unit than the lock
//!    call, which no RAII guard could span).
//! 3. `vfs_inode.mutex`, via `inode.rs`'s `vfs_ilock`/`vfs_iunlock`.
//!
//! When multiple locks of the same type are needed: parent superblock
//! before child superblock; directory inode before child inode. Crossing
//! filesystems requires releasing an inode lock before acquiring another
//! filesystem's inode lock; inode locks are always acquired after any
//! superblock lock (including a mounted superblock's).
//!
//! # Style notes (rust-skills)
//!
//! Self-contained per this crate's established per-file convention (see
//! `inode.rs`'s externs-block doc): every cross-module C-ABI symbol is
//! declared locally rather than imported by Rust path, and small
//! `list.h`/`hlist.h` `static inline` helpers with no external linkage in
//! the C original (`list_entry_init`, `list_node_push_back/front`,
//! `list_node_detach`, `hlist_first_entry`/`hlist_next_entry` and
//! friends) are reimplemented natively here rather than borrowed from
//! `kernel/list.rs`/`kernel/hlist.rs`, matching `inode.rs`/`file.rs`/
//! `pipe.rs`/`fdtable.rs`. Bitfield flags go through bindgen's
//! `__bindgen_anon_N.field()`/`.set_field()` accessors on a temporary
//! place expression, never via a materialized `&mut` of the whole
//! struct. `unsafe` is scope-minimized with `SAFETY:` comments at each
//! non-obvious site. One **deliberate fidelity deviation**, already
//! established by `inode.rs`/`file.rs`/`pipe.rs`/`fdtable.rs`: the C
//! `assert(expr, fmt, ...)` macro's `printf`-style format arguments
//! (e.g. `"errno=%d"`) are dropped in favor of this crate's fixed-message
//! `xv6_panic(msg) -> !` (`__panic_start(); printf("%s\n", msg);
//! __panic_end();`, no variadic formatting) — these are unrecoverable
//! invariant violations either way, so the exact panic text doesn't
//! affect behavior. Genuine informational `printf(...)` calls (mount
//! status lines, the debug-dump functions) keep their real format
//! strings and arguments.
//!
//! `vfs_root_inode` is deliberately **not** run through `mutex_init`
//! anywhere (C original: `struct vfs_inode vfs_root_inode = {0};` +
//! `__vfs_rooti_init()`'s `memset(..., 0, ...)`, never `mutex_init`) —
//! its embedded `mutex_t`/`completion_t` stay all-zero-bytes for the
//! process lifetime. This is a pre-existing quirk of the root inode
//! being a VFS-synthesized sentinel with no backing filesystem, carried
//! over unchanged; not something this port introduces or fixes.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::mem::offset_of;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{
    fs_struct, hlist_bucket_t, hlist_entry_t, hlist_func_t, hlist_t, kobject, list_node_t,
    mutex_t, rwsem_t, slab_cache_t, spinlock_t, thread, vfs_dentry, vfs_fs_type,
    vfs_fs_type_ops, vfs_inode, vfs_inode_ref, vfs_superblock, vfs_superblock_ops, work_struct,
    workqueue, EAGAIN, EALREADY, EBUSY, EEXIST, EINVAL, ENODEV, ENOENT, ENOSPC, ENOTDIR, EPERM,
    RWLOCK_PRIO_READ,
};

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally per this crate's established convention (see `vfs/inode.rs`'s
// externs-block doc). Nearly everything is `safe`: passing a raw pointer
// is never itself unsafe, and every entry point here either null-checks
// internally or is called only with pointers this file already proved
// non-null. `printf` (C-variadic) is the one exception.
// ===========================================================================

unsafe extern "C" {
    // proc module (kernel/proc/thread.rs, kernel/proc/sched.rs).
    safe fn xv6_current_thread() -> *mut thread;
    safe fn scheduler_yield();
    safe fn xv6_panic(msg: *const c_char) -> !;

    // printf.rs — C-variadic.
    fn printf(fmt: *const c_char, ...) -> c_int;

    // lock/mutex.rs.
    safe fn mutex_init(m: *mut mutex_t, name: *mut c_char);
    safe fn mutex_lock(m: *mut mutex_t);
    safe fn mutex_unlock(m: *mut mutex_t);
    safe fn holding_mutex(m: *mut mutex_t) -> c_int;

    // lock/rwsem.rs — `vfs_superblock.lock`.
    safe fn rwsem_init(lk: *mut rwsem_t, flags: u64, name: *const c_char) -> c_int;
    safe fn rwsem_acquire_read(lk: *mut rwsem_t) -> c_int;
    safe fn rwsem_acquire_write(lk: *mut rwsem_t) -> c_int;
    safe fn rwsem_release(lk: *mut rwsem_t);
    safe fn rwsem_is_write_holding(lk: *mut rwsem_t) -> bool;

    // lock/spinlock.rs — `vfs_superblock.spinlock`, `fs_struct.lock`.
    safe fn spin_init(l: *mut spinlock_t, name: *mut c_char);
    safe fn spin_lock(l: *mut spinlock_t);
    safe fn spin_unlock(l: *mut spinlock_t);

    // mm/slab.rs.
    safe fn slab_cache_init(
        cache: *mut slab_cache_t,
        name: *mut c_char,
        obj_size: usize,
        flags: u64,
    ) -> c_int;
    safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
    safe fn slab_free(obj: *mut c_void);
    safe fn slab_cache_shrink(cache: *mut slab_cache_t, nums: c_int) -> c_int;

    // mm/kalloc.rs.
    safe fn kmm_free(ptr: *mut c_void);

    // string.rs.
    safe fn strlen(s: *const c_char) -> usize;
    safe fn strndup(s: *const c_char, n: usize) -> *mut c_char;
    safe fn strncmp(p: *const c_char, q: *const c_char, n: usize) -> c_int;

    // hlist.rs (Wave 1) — generic hash-list primitives; the header's
    // `static inline` bucket/entry walkers (`hlist_first_entry`,
    // `hlist_next_entry`, `HLIST_FIRST_NODE`, ...) have no external
    // linkage in the C original and are reimplemented below,
    // specialized to `vfs_superblock.inodes`/`.inodes_buckets`.
    safe fn hlist_init(hlist: *mut hlist_t, bucket_cnt: u64, func: *mut hlist_func_t) -> i32;
    safe fn hlist_get(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void;
    safe fn hlist_put(hlist: *mut hlist_t, node: *mut c_void, replace: bool) -> *mut c_void;
    safe fn hlist_pop(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void;
    safe fn hlist_len(hlist: *mut hlist_t) -> usize;

    // kobject.rs (Wave 1) — `vfs_fs_type.kobj`.
    safe fn kobject_init(obj: *mut kobject);
    safe fn kobject_get(obj: *mut kobject);
    safe fn kobject_put(obj: *mut kobject);

    // proc/workqueue.rs — the deferred-iput workqueue.
    safe fn workqueue_create(name: *const c_char, max_active: c_int) -> *mut workqueue;
    safe fn queue_work(wq: *mut workqueue, work: *mut work_struct) -> bool;
    safe fn create_work_struct(
        func: Option<unsafe extern "C" fn(*mut work_struct)>,
        data: u64,
    ) -> *mut work_struct;
    safe fn free_work_struct(work: *mut work_struct);

}

// P3-1C mesh sweep: every submodule below (`inode`/`file`/`fdtable`/
// `tmpfs`/`xv6fs`/`devtmpfs`/`vfs_syscall`) is in scope for this wave, so
// these become plain crate-path imports instead of `extern "C"`
// redeclarations (all identical signatures -- same `crate::bindings::*`
// types this file already imports).
use crate::vfs::devtmpfs::superblock::{devtmpfs_init, devtmpfs_post_mount_populate};
use crate::vfs::fdtable::{__vfs_fdtable_global_init, vfs_fdtable_init};
use crate::vfs::file::{__vfs_file_init, __vfs_file_shrink_cache};
use crate::vfs::inode::{
    __vfs_inode_init, vfs_chroot, vfs_idup, vfs_idup_not_zero, vfs_ilock, vfs_iunlock, vfs_iput,
    vfs_mkdir, vfs_namei,
};
use crate::vfs::tmpfs::superblock::{tmpfs_init, tmpfs_mount_root};
use crate::vfs::vfs_syscall::vfs_mount_path;
use crate::vfs::xv6fs::superblock::{xv6fs_init, xv6fs_mount_root};

/// Mirrors the C `assert(expr, fmt)` macro (`kernel/inc/printf.h`) with
/// its format arguments dropped — see the module doc's "Style notes" for
/// why (already-established `inode.rs`/`file.rs`/`pipe.rs`/`fdtable.rs`
/// precedent).
macro_rules! kassert {
    ($cond:expr, $msg:expr) => {
        if !($cond) {
            xv6_panic(concat!($msg, "\0").as_ptr() as *const c_char)
        }
    };
}

// ===========================================================================
// Small helpers: negative-errno constants, ERR_PTR family, mode bits,
// CLONE_FS, list/hlist `static inline` reimplementations.
// ===========================================================================

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// `MAX_ERRNO`/`IS_ERR_VALUE`/`ERR_PTR`/`PTR_ERR`/`IS_ERR`/`IS_ERR_OR_NULL`
/// (`kernel/inc/errno.h`), generic over the pointee type. Reimplemented
/// locally per this file's established per-file-self-contained
/// convention (see the externs-block doc).
const MAX_ERRNO: isize = 4095;

#[inline(always)]
fn is_err_value(p: usize) -> bool {
    p >= (-(MAX_ERRNO)) as usize
}
#[inline(always)]
fn err_ptr<T>(errno: c_int) -> *mut T {
    errno as isize as *mut T
}
#[inline(always)]
fn is_err<T>(p: *mut T) -> bool {
    is_err_value(p as usize)
}
#[inline(always)]
fn is_err_or_null<T>(p: *mut T) -> bool {
    p.is_null() || is_err(p)
}
#[inline(always)]
fn ptr_err<T>(p: *mut T) -> isize {
    p as isize
}
#[inline(always)]
fn is_eagain_ptr<T>(p: *mut T) -> bool {
    ptr_err(p) == neg(EAGAIN) as isize
}

// `uabi/stat.h`'s `S_IF*`/`S_IS*` macros (full set — `__inode_mode_str`
// touches every file type).
const S_IFMT: u32 = 0o170000;
const S_IFIFO: u32 = 0o010000;
const S_IFCHR: u32 = 0o020000;
const S_IFDIR: u32 = 0o040000;
const S_IFBLK: u32 = 0o060000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;
const S_IFSOCK: u32 = 0o140000;
#[inline(always)]
fn is_dir(mode: u32) -> bool {
    mode & S_IFMT == S_IFDIR
}
#[inline(always)]
fn is_chr(mode: u32) -> bool {
    mode & S_IFMT == S_IFCHR
}
#[inline(always)]
fn is_blk(mode: u32) -> bool {
    mode & S_IFMT == S_IFBLK
}
#[inline(always)]
fn is_fifo(mode: u32) -> bool {
    mode & S_IFMT == S_IFIFO
}
#[inline(always)]
fn is_sock(mode: u32) -> bool {
    mode & S_IFMT == S_IFSOCK
}
#[inline(always)]
fn is_reg(mode: u32) -> bool {
    mode & S_IFMT == S_IFREG
}
#[inline(always)]
fn is_lnk(mode: u32) -> bool {
    mode & S_IFMT == S_IFLNK
}

/// `uabi/clone_flags.h`'s `CLONE_FS`.
const CLONE_FS: u64 = 0x00200000;

// ---------------------------------------------------------------------------
// `list.h` `static inline` primitives (no external linkage in the C
// original — reimplemented natively, same precedent as `inode.rs`'s
// `ln_init`/`ln_detach` and `file.rs`'s identical helpers).
// ---------------------------------------------------------------------------

/// Mirrors `list_entry_init()`.
///
/// # Safety
/// `head` must point to a live, writable `list_node_t`.
#[inline(always)]
unsafe fn ln_init(head: *mut list_node_t) {
    unsafe {
        (*head).next = head;
        (*head).prev = head;
    }
}

/// Mirrors `list_entry_push_back()`/`list_node_push_back()`.
///
/// # Safety
/// `head` must point to a live, circular `list_node_t` list head; `node`
/// must not already be linked into any list.
#[inline(always)]
unsafe fn ln_push_back(head: *mut list_node_t, node: *mut list_node_t) {
    unsafe {
        let tail = (*head).prev;
        (*node).prev = tail;
        (*node).next = head;
        (*tail).next = node;
        (*head).prev = node;
    }
}

/// Mirrors `list_entry_push_front()`/`list_node_push_front()`.
///
/// # Safety
/// Same as [`ln_push_back`].
#[inline(always)]
unsafe fn ln_push_front(head: *mut list_node_t, node: *mut list_node_t) {
    unsafe {
        let first = (*head).next;
        (*node).prev = head;
        (*node).next = first;
        (*first).prev = node;
        (*head).next = node;
    }
}

/// Mirrors `list_entry_detach()`/`list_node_detach()`.
///
/// # Safety
/// `node` must point to a live, linked `list_node_t`.
#[inline(always)]
unsafe fn ln_detach(node: *mut list_node_t) {
    unsafe {
        let prev = (*node).prev;
        let next = (*node).next;
        (*prev).next = next;
        (*next).prev = prev;
        (*node).prev = node;
        (*node).next = node;
    }
}

// ---------------------------------------------------------------------------
// `hlist.h` `static inline` bucket/entry walkers, specialized to
// `vfs_superblock.__bindgen_anon_1.{inodes,inodes_buckets}`. Unlike the
// general `hlist_t` (whose C `buckets` field is a flexible-array member
// that `kernel/hlist.rs`'s private `bucket_at` reaches via raw pointer
// arithmetic past the header, not exported), the superblock's buckets are
// addressable as a plain fixed-size `[hlist_bucket_t; 61]` array field
// (`vfs_superblock__bindgen_ty_1`), so this file indexes that array
// directly instead of re-deriving the header-relative arithmetic.
// `hlist_entry_t.list_entry` and `vfs_inode.hash_entry` are both their
// container's first field (offset 0, verified against the bindgen
// layout), so `*mut list_node_t`/`*mut hlist_entry_t`/`*mut vfs_inode`
// are freely interconvertible here without `container_of` arithmetic;
// each cast below is commented with that invariant.
// ---------------------------------------------------------------------------

const SB_HASH_BUCKETS: usize = 61; // VFS_SUPERBLOCK_HASH_BUCKETS

/// # Safety
/// `sb` must point to a live, initialized `vfs_superblock`.
#[inline(always)]
unsafe fn sb_buckets_base(sb: *mut vfs_superblock) -> *mut hlist_bucket_t {
    unsafe { (&raw mut (*sb).__bindgen_anon_1.inodes_buckets) as *mut hlist_bucket_t }
}

/// # Safety
/// `sb` must point to a live, initialized `vfs_superblock`; `idx` must be
/// `< SB_HASH_BUCKETS`.
#[inline(always)]
unsafe fn sb_bucket_at(sb: *mut vfs_superblock, idx: usize) -> *mut hlist_bucket_t {
    unsafe { sb_buckets_base(sb).add(idx) }
}

/// Mirrors `hlist_bucket_first_entry()`, returning the `vfs_inode`
/// directly (`hash_entry`/`list_entry` are both offset 0 — see the
/// section doc).
///
/// # Safety
/// `bucket` must point to a live, circular `hlist_bucket_t` list head.
#[inline(always)]
unsafe fn bucket_first_inode(bucket: *mut hlist_bucket_t) -> *mut vfs_inode {
    unsafe {
        let first = (*bucket).next;
        if first == bucket {
            ptr::null_mut()
        } else {
            first as *mut vfs_inode
        }
    }
}

/// Mirrors `hlist_first_entry()`/`HLIST_FIRST_NODE()`, specialized to
/// `sb->inodes`.
///
/// # Safety
/// `sb` must point to a live, initialized `vfs_superblock`.
unsafe fn sb_inodes_first(sb: *mut vfs_superblock) -> *mut vfs_inode {
    unsafe {
        for i in 0..SB_HASH_BUCKETS {
            let n = bucket_first_inode(sb_bucket_at(sb, i));
            if !n.is_null() {
                return n;
            }
        }
        ptr::null_mut()
    }
}

/// Mirrors `hlist_next_entry()`, specialized to `sb->inodes`.
///
/// # Safety
/// `sb` must point to a live, initialized `vfs_superblock`; `inode` must
/// be currently linked into `sb->inodes`.
unsafe fn sb_inodes_next(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> *mut vfs_inode {
    unsafe {
        // `inode` reinterpreted as `*mut hlist_entry_t`/`*mut list_node_t`
        // (both offset 0 within their respective containers — see the
        // section doc).
        let entry = inode as *mut hlist_entry_t;
        let bucket = (*entry).bucket;
        if bucket.is_null() {
            return ptr::null_mut();
        }
        let node = inode as *mut list_node_t;
        let next = (*node).next;
        if next != bucket {
            return next as *mut vfs_inode;
        }
        let base = sb_buckets_base(sb);
        let idx = (bucket as usize - base as usize) / core::mem::size_of::<hlist_bucket_t>();
        for i in (idx + 1)..SB_HASH_BUCKETS {
            let n = bucket_first_inode(sb_bucket_at(sb, i));
            if !n.is_null() {
                return n;
            }
        }
        ptr::null_mut()
    }
}

// ===========================================================================
// Module statics (all file-private in the C original — no `#[no_mangle]`).
// ===========================================================================

static mut VFS_FS_TYPE_CACHE: slab_cache_t = unsafe { core::mem::zeroed() };
static mut VFS_SUPERBLOCK_CACHE: slab_cache_t = unsafe { core::mem::zeroed() };
static mut VFS_STRUCT_CACHE: slab_cache_t = unsafe { core::mem::zeroed() };
static mut __MOUNT_MUTEX: mutex_t = unsafe { core::mem::zeroed() };
static mut VFS_FS_TYPES: list_node_t = list_node_t { prev: ptr::null_mut(), next: ptr::null_mut() };
static mut VFS_FS_TYPE_COUNT: u16 = 0;
const MAX_FS_TYPES: u16 = 256;

/// Workqueue for deferred `vfs_iput()` operations. `vfs_iput()` can block
/// on the superblock wlock, inode mutex, and filesystem transactions; it
/// must not be called directly from RCU callbacks (deadlock risk against
/// threads waiting on the same locks for a grace period). RCU callbacks
/// queue work here instead.
static mut __VFS_DEFERRED_IPUT_WQ: *mut workqueue = ptr::null_mut();

/// The absolute VFS root inode: a synthesized sentinel with no
/// superblock, data, or operations, serving only as the top of the mount
/// tree. See the module doc for why its embedded lock types are never
/// `mutex_init`-ed.
pub(crate) static mut vfs_root_inode: vfs_inode = unsafe { core::mem::zeroed() };

// ===========================================================================
// Superblock inode-hash function table (`__vfs_superblock_inode_*` in the
// C original — `static`, internal linkage, referenced only by address).
// ===========================================================================

unsafe extern "C" fn sb_inode_hash_fn(node: *mut c_void) -> u64 {
    // SAFETY: `node` is always a live `*mut vfs_inode` when the hash
    // table invokes this (the crate-wide hlist contract).
    unsafe { hlist_hash_uint64((*(node as *mut vfs_inode)).ino) }
}

unsafe extern "C" fn sb_inode_cmp_fn(
    _hlist: *mut hlist_t,
    node: *mut c_void,
    key: *mut c_void,
) -> c_int {
    // SAFETY: same contract as `sb_inode_hash_fn`.
    unsafe {
        let a = (*(node as *mut vfs_inode)).ino;
        let b = (*(key as *mut vfs_inode)).ino;
        if a > b {
            1
        } else if a < b {
            -1
        } else {
            0
        }
    }
}

unsafe extern "C" fn sb_inode_get_node_fn(entry: *mut hlist_entry_t) -> *mut c_void {
    // `hash_entry` is `vfs_inode`'s first field (offset 0).
    if entry.is_null() {
        ptr::null_mut()
    } else {
        entry as *mut c_void
    }
}

unsafe extern "C" fn sb_inode_get_entry_fn(node: *mut c_void) -> *mut hlist_entry_t {
    if node.is_null() {
        ptr::null_mut()
    } else {
        node as *mut hlist_entry_t
    }
}

static SB_INODE_HLIST_FUNCS: hlist_func_t = hlist_func_t {
    hash: Some(sb_inode_hash_fn),
    get_node: Some(sb_inode_get_node_fn),
    get_entry: Some(sb_inode_get_entry_fn),
    cmp_node: Some(sb_inode_cmp_fn),
};

/// Mirrors `hlist_hash_uint64()` (`kernel/inc/hlist.h`, `static inline`,
/// no external linkage).
#[inline(always)]
fn hlist_hash_uint64(key: u64) -> u64 {
    const GOLDEN_RATIO_PRIME_64: u64 = 0x9e37fffffffc0001;
    let ret = key.wrapping_mul(GOLDEN_RATIO_PRIME_64);
    if ret == 0 {
        GOLDEN_RATIO_PRIME_64
    } else {
        ret
    }
}

// ---------------------------------------------------------------------------
// Atomics — mirrors `smp/atomic.h`'s `atomic_oper_cond` family and plain
// `__atomic_*` builtins exactly (see `inode.rs`'s identical helpers and
// their ordering rationale: `Ordering::SeqCst` CAS over an
// `Ordering::Acquire` initial load, matching `atomic_oper_cond`
// verbatim — a line-faithful port, not an independent re-derivation of
// weaker orderings).
// ---------------------------------------------------------------------------

/// # Safety
/// `sb` must point to a live, aligned `vfs_superblock`.
#[inline(always)]
unsafe fn sb_refcount_atomic<'a>(sb: *mut vfs_superblock) -> &'a AtomicI32 {
    unsafe { &*(ptr::addr_of_mut!((*sb).refcount) as *const AtomicI32) }
}
/// # Safety
/// `sb` must point to a live, aligned `vfs_superblock`.
#[inline(always)]
unsafe fn sb_mountcount_atomic<'a>(sb: *mut vfs_superblock) -> &'a AtomicI32 {
    unsafe { &*(ptr::addr_of_mut!((*sb).mount_count) as *const AtomicI32) }
}
/// # Safety
/// `inode` must point to a live, aligned `vfs_inode`.
#[inline(always)]
unsafe fn inode_refcount_atomic<'a>(inode: *mut vfs_inode) -> &'a AtomicI32 {
    unsafe { &*(ptr::addr_of_mut!((*inode).ref_count) as *const AtomicI32) }
}
/// # Safety
/// `fs` must point to a live, aligned `fs_struct`.
#[inline(always)]
unsafe fn fs_refcount_atomic<'a>(fs: *mut fs_struct) -> &'a AtomicI32 {
    unsafe { &*(ptr::addr_of_mut!((*fs).ref_count) as *const AtomicI32) }
}

fn atomic_oper_cond(
    a: &AtomicI32,
    mut op: impl FnMut(i32) -> i32,
    mut cond: impl FnMut(i32) -> bool,
) -> bool {
    let mut val = a.load(Ordering::Acquire);
    while cond(val) {
        let new_val = op(val);
        match a.compare_exchange(val, new_val, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => return true,
            Err(cur) => val = cur,
        }
    }
    false
}
#[inline(always)]
fn atomic_dec_unless(a: &AtomicI32, unless: i32) -> bool {
    atomic_oper_cond(a, |v| v - 1, move |v| v != unless)
}

/// Mirrors `fs.h`'s `static inline vfs_inode_refcount()`.
///
/// # Safety
/// `inode` must be null or a live, aligned `vfs_inode`.
unsafe fn inode_refcount(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return -1;
    }
    unsafe { inode_refcount_atomic(inode).load(Ordering::SeqCst) }
}

/// Mirrors `fs.h`'s `static inline vfs_superblock_mountcount()`.
///
/// # Safety
/// `sb` must be null or a live, aligned `vfs_superblock`.
unsafe fn superblock_mountcount(sb: *mut vfs_superblock) -> c_int {
    if sb.is_null() {
        return -1;
    }
    unsafe { sb_mountcount_atomic(sb).load(Ordering::SeqCst) }
}

/// Mirrors `fs.h`'s `static inline vfs_inode_is_local_root()`.
///
/// # Safety
/// `inode` must be null or a live, aligned `vfs_inode`.
unsafe fn inode_is_local_root(inode: *mut vfs_inode) -> bool {
    unsafe {
        if inode.is_null() || (*inode).sb.is_null() {
            return false;
        }
        if inode == (*(*inode).sb).root_inode {
            kassert!(
                (*inode).parent == inode,
                "vfs_inode_is_local_root: root inode's parent is not itself"
            );
            return true;
        }
        false
    }
}

// ---------------------------------------------------------------------------
// `vfs_private.h`'s `static inline` validity checks (no external linkage
// in the C original).
// ---------------------------------------------------------------------------

/// Mirrors `vfs_private.h`'s `static inline __vfs_inode_valid()`.
///
/// # Safety
/// `inode`, if non-null, must be a live `vfs_inode`.
unsafe fn inode_valid(inode: *mut vfs_inode) -> c_int {
    unsafe {
        if inode.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut (*inode).mutex) == 0 {
            return neg(EPERM);
        }
        if (*inode).__bindgen_anon_1.valid() == 0 {
            return neg(EINVAL);
        }
        if !ptr::eq(inode, &raw const vfs_root_inode) {
            let sb = (*inode).sb;
            if sb.is_null() || (*sb).__bindgen_anon_2.valid() == 0 {
                printf(
                    c"__vfs_inode_valid: inode's superblock is not valid\n".as_ptr(),
                );
                return neg(EINVAL);
            }
        }
        0
    }
}

/// Mirrors `vfs_private.h`'s `static inline __vfs_dir_inode_valid_holding()`.
///
/// # Safety
/// `inode`, if non-null, must be a live `vfs_inode`.
unsafe fn dir_inode_valid_holding(inode: *mut vfs_inode) -> c_int {
    unsafe {
        if inode.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut (*inode).mutex) == 0 {
            return neg(EPERM);
        }
        if (*inode).__bindgen_anon_1.valid() == 0 {
            return neg(EINVAL);
        }
        if !is_dir((*inode).mode) {
            return neg(EINVAL);
        }
        if !ptr::eq(inode, &raw const vfs_root_inode) {
            let sb = (*inode).sb;
            if sb.is_null() || (*sb).__bindgen_anon_2.valid() == 0 {
                return neg(EINVAL);
            }
        }
        0
    }
}

/******************************************************************************
 * Private functions
 *****************************************************************************/

/// Mirrors `__vfs_rooti_init()`.
unsafe fn vfs_rooti_init() {
    unsafe {
        vfs_root_inode = core::mem::zeroed();
        vfs_root_inode.mode = S_IFDIR | 0o755;
        vfs_root_inode.__bindgen_anon_1.set_valid(1);
    }
}

/// Mirrors `__vfs_register_fs_type_locked()`.
unsafe fn register_fs_type_locked(fs_type: *mut vfs_fs_type) {
    unsafe {
        ln_push_back(&raw mut VFS_FS_TYPES, &raw mut (*fs_type).list_entry);
        (*fs_type).__bindgen_anon_1.set_registered(1);
        VFS_FS_TYPE_COUNT += 1;
        kassert!(
            VFS_FS_TYPE_COUNT <= MAX_FS_TYPES,
            "Exceeded maximum filesystem types"
        );
    }
}

/// Mirrors `__vfs_unregister_fs_type_locked()`.
unsafe fn unregister_fs_type_locked(fs_type: *mut vfs_fs_type) {
    unsafe {
        ln_detach(&raw mut (*fs_type).list_entry);
        (*fs_type).__bindgen_anon_1.set_registered(0);
        VFS_FS_TYPE_COUNT -= 1;
        kassert!(
            VFS_FS_TYPE_COUNT <= MAX_FS_TYPES,
            "Filesystem types count underflow"
        );
    }
}

/// Mirrors `__vfs_get_fs_type_locked()`. `vfs_fs_type.list_entry` is the
/// struct's first field (offset 0), so the walk casts `list_node_t`
/// pointers to `*mut vfs_fs_type` directly.
unsafe fn get_fs_type_locked(name: *const c_char) -> *mut vfs_fs_type {
    unsafe {
        let name_len = strlen(name);
        let head = &raw mut VFS_FS_TYPES;
        let mut pos = (*head).next;
        while pos != head {
            let fs_type = pos as *mut vfs_fs_type;
            if strncmp((*fs_type).name, name, name_len) == 0 {
                return fs_type;
            }
            pos = (*pos).next;
        }
        ptr::null_mut()
    }
}

/// Mirrors `__vfs_fs_type_kobj_release()`.
unsafe extern "C" fn vfs_fs_type_kobj_release(kobj: *mut kobject) {
    unsafe {
        let fs_type = crate::mm::cffi::container_of::<vfs_fs_type, kobject>(
            kobj,
            offset_of!(vfs_fs_type, kobj),
        );
        slab_free(fs_type as *mut c_void);
    }
}

/// Mirrors `__vfs_inode_hash_get()`.
unsafe fn inode_hash_get(sb: *mut vfs_superblock, ino: u64) -> *mut vfs_inode {
    unsafe {
        let mut key: vfs_inode = core::mem::zeroed();
        key.ino = ino;
        hlist_get(
            &raw mut (*sb).__bindgen_anon_1.inodes,
            (&raw mut key) as *mut c_void,
        ) as *mut vfs_inode
    }
}

/// Mirrors `__vfs_inode_hash_add()`.
unsafe fn inode_hash_add(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> *mut vfs_inode {
    unsafe {
        hlist_put(
            &raw mut (*sb).__bindgen_anon_1.inodes,
            inode as *mut c_void,
            false,
        ) as *mut vfs_inode
    }
}

/// Mirrors `__vfs_init_superblock_structure()`.
unsafe fn init_superblock_structure(sb: *mut vfs_superblock, fs_type: *mut vfs_fs_type) {
    unsafe {
        ln_init(&raw mut (*sb).siblings);
        ln_init(&raw mut (*sb).orphan_list);
        hlist_init(
            &raw mut (*sb).__bindgen_anon_1.inodes,
            SB_HASH_BUCKETS as u64,
            &SB_INODE_HLIST_FUNCS as *const hlist_func_t as *mut hlist_func_t,
        );
        (*sb).fs_type = fs_type;
        (*sb).orphan_count = 0;
        sb_refcount_atomic(sb).store(0, Ordering::SeqCst);
        sb_mountcount_atomic(sb).store(0, Ordering::SeqCst);
        rwsem_init(
            &raw mut (*sb).lock,
            RWLOCK_PRIO_READ as u64,
            c"vfs_superblock_lock".as_ptr(),
        );
        spin_init(
            &raw mut (*sb).spinlock,
            c"vfs_superblock_spinlock".as_ptr() as *mut c_char,
        );
    }
}

/// Mirrors `__vfs_init_sb_rooti()`.
unsafe fn init_sb_rooti(sb: *mut vfs_superblock) -> c_int {
    unsafe {
        __vfs_inode_init((*sb).root_inode);
        loop {
            let inode = vfs_add_inode(sb, (*sb).root_inode);
            if is_err_or_null(inode) {
                if is_eagain_ptr(inode) {
                    vfs_superblock_unlock(sb);
                    scheduler_yield();
                    vfs_superblock_wlock(sb);
                    if (*sb).__bindgen_anon_2.valid() == 0 && (*sb).__bindgen_anon_2.initialized() != 0 {
                        return neg(EINVAL);
                    }
                    continue;
                }
                if inode.is_null() {
                    return neg(ENOENT);
                }
                return ptr_err(inode) as c_int;
            }
            if inode != (*sb).root_inode {
                vfs_iunlock(inode);
                return neg(EEXIST);
            }
            (*(*sb).root_inode).parent = (*sb).root_inode;
            vfs_iunlock((*sb).root_inode);
            return 0;
        }
    }
}

/// Mirrors `__vfs_superblock_ops_valid()`.
unsafe fn superblock_ops_valid(sb: *mut vfs_superblock) -> bool {
    unsafe {
        let ops = (*sb).ops;
        if ops.is_null() {
            return false;
        }
        if (*ops).alloc_inode.is_none()
            || (*ops).get_inode.is_none()
            || (*ops).sync_fs.is_none()
            || (*ops).unmount_begin.is_none()
        {
            return false;
        }
        true
    }
}

/// Mirrors `__vfs_init_superblock_valid()`.
unsafe fn init_superblock_valid(sb: *mut vfs_superblock) -> bool {
    unsafe {
        if sb.is_null() {
            return false;
        }
        if (*sb).__bindgen_anon_2.valid() != 0 || (*sb).__bindgen_anon_2.dirty() != 0 {
            return false;
        }
        if !superblock_ops_valid(sb) {
            return false;
        }
        if !(*sb).mountpoint.is_null() || !(*sb).parent_sb.is_null() {
            return false;
        }
        true
    }
}

/// Mirrors `__vfs_attach_superblock_to_fstype()`. `vfs_superblock.siblings`
/// is the struct's first field (offset 0).
unsafe fn attach_superblock_to_fstype(sb: *mut vfs_superblock) {
    unsafe {
        let fs_type = (*sb).fs_type;
        ln_push_front(&raw mut (*fs_type).superblocks, &raw mut (*sb).siblings);
        (*fs_type).sb_count += 1;
        (*sb).__bindgen_anon_2.set_registered(1);
        kassert!(
            (*fs_type).sb_count > 0,
            "Filesystem type superblock count overflow"
        );
    }
}

/// Mirrors `__vfs_detach_superblock_from_fstype()`.
unsafe fn detach_superblock_from_fstype(sb: *mut vfs_superblock) {
    unsafe {
        ln_detach(&raw mut (*sb).siblings);
        let fs_type = (*sb).fs_type;
        (*fs_type).sb_count -= 1;
        (*sb).__bindgen_anon_2.set_registered(0);
        kassert!(
            (*fs_type).sb_count >= 0,
            "Filesystem type superblock count underflow"
        );
    }
}

/// Mirrors `__vfs_turn_mountpoint()`.
unsafe fn turn_mountpoint(mountpoint: *mut vfs_inode) -> c_int {
    unsafe {
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            kassert!(
                rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock),
                "Mountpoint inode's superblock lock must be write held to turn into mountpoint"
            );
        }
        kassert!(
            holding_mutex(&raw mut (*mountpoint).mutex) != 0,
            "Mountpoint inode lock must be held to turn into mountpoint"
        );
        if inode_refcount(mountpoint) > 2 {
            return neg(EBUSY);
        }
        if !is_dir((*mountpoint).mode) {
            return neg(ENOTDIR);
        }
        if inode_is_local_root(mountpoint) {
            return neg(EBUSY);
        }
        if (*mountpoint).__bindgen_anon_1.mount() != 0 {
            return neg(EBUSY);
        }
        (*mountpoint).__bindgen_anon_1.set_mount(1);
        (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_rooti = ptr::null_mut();
        (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_sb = ptr::null_mut();
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            vfs_superblock_mountcount_inc((*mountpoint).sb);
            vfs_idup(mountpoint);
        }
        0
    }
}

/// Mirrors `__vfs_set_mountpoint()`.
unsafe fn set_mountpoint(sb: *mut vfs_superblock, mountpoint: *mut vfs_inode) {
    unsafe {
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            kassert!(
                rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock),
                "Mountpoint inode's superblock lock must be write held to set mountpoint"
            );
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Superblock lock must be write held to set mountpoint"
        );
        kassert!(
            holding_mutex(&raw mut (*mountpoint).mutex) != 0,
            "Mountpoint inode lock must be held to set mountpoint"
        );
        kassert!(
            (*mountpoint).__bindgen_anon_1.mount() != 0,
            "Mountpoint inode is not marked as a mountpoint"
        );
        kassert!((*sb).mountpoint.is_null(), "Superblock mountpoint is already set");
        (*sb).mountpoint = mountpoint;
        (*sb).parent_sb = (*mountpoint).sb;
        (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_sb = sb;
        (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_rooti = (*sb).root_inode;
    }
}

/// Mirrors `__vfs_clear_mountpoint()`.
unsafe fn clear_mountpoint(mountpoint: *mut vfs_inode) {
    unsafe {
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            kassert!(
                rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock),
                "Mountpoint inode's superblock lock must be write held to clear mountpoint"
            );
        }
        kassert!(
            holding_mutex(&raw mut (*mountpoint).mutex) != 0,
            "Mountpoint inode lock must be held to clear mountpoint"
        );
        kassert!(
            (*mountpoint).__bindgen_anon_1.mount() != 0,
            "Mountpoint inode type is not MNT"
        );
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            vfs_superblock_mountcount_dec((*mountpoint).sb);
        }
        (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_sb = ptr::null_mut();
        (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_rooti = ptr::null_mut();
        (*mountpoint).__bindgen_anon_1.set_mount(0);
    }
}

const FS_STRUCT_LOCK_NAME: &[u8] = b"fs_struct_lock\0";

/// Mirrors `__vfs_struct_alloc_init()`.
unsafe fn struct_alloc_init() -> *mut fs_struct {
    unsafe {
        let fs = slab_alloc(&raw mut VFS_STRUCT_CACHE) as *mut fs_struct;
        if fs.is_null() {
            return ptr::null_mut();
        }
        ptr::write_bytes(fs, 0, 1);
        spin_init(&raw mut (*fs).lock, FS_STRUCT_LOCK_NAME.as_ptr() as *mut c_char);
        fs_refcount_atomic(fs).store(1, Ordering::Release);
        fs
    }
}

/// Mirrors `__vfs_struct_free()`.
unsafe fn struct_free(fs: *mut fs_struct) {
    unsafe { slab_free(fs as *mut c_void) };
}

/******************************************************************************
 * Files System Type / VFS init Public APIs
 *****************************************************************************/

/// Mirrors `vfs_init()`.
///
/// Locking: none.
pub(crate) extern "C" fn vfs_init() {
    unsafe {
        vfs_rooti_init();
        ln_init(&raw mut VFS_FS_TYPES);
        mutex_init(&raw mut __MOUNT_MUTEX, c"vfs_mount_mutex".as_ptr() as *mut c_char);
        __vfs_fdtable_global_init();
        let mut ret = slab_cache_init(
            &raw mut VFS_SUPERBLOCK_CACHE,
            c"vfs_superblock_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<vfs_superblock>(),
            (crate::bindings::SLAB_FLAG_STATIC | crate::bindings::SLAB_FLAG_DEBUG_BITMAP) as u64,
        );
        kassert!(
            ret == 0,
            "Failed to initialize vfs_superblock_cache slab cache"
        );
        ret = slab_cache_init(
            &raw mut VFS_FS_TYPE_CACHE,
            c"vfs_fs_type_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<vfs_fs_type>(),
            (crate::bindings::SLAB_FLAG_STATIC | crate::bindings::SLAB_FLAG_DEBUG_BITMAP) as u64,
        );
        kassert!(ret == 0, "Failed to initialize vfs_fs_type_cache slab cache");
        ret = slab_cache_init(
            &raw mut VFS_STRUCT_CACHE,
            c"vfs_struct_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<fs_struct>(),
            (crate::bindings::SLAB_FLAG_STATIC | crate::bindings::SLAB_FLAG_DEBUG_BITMAP) as u64,
        );
        kassert!(ret == 0, "Failed to initialize vfs_struct_cache slab cache");
        VFS_FS_TYPE_COUNT = 0;

        // Single-threaded workqueue for deferred iput (max_active=1
        // serializes all deferred iputs, avoiding a thundering-herd on
        // vfs_superblock_wlock). Must happen before any file operation
        // that might use RCU.
        __VFS_DEFERRED_IPUT_WQ =
            workqueue_create(c"vfs_iput_wq".as_ptr(), 1);
        kassert!(!__VFS_DEFERRED_IPUT_WQ.is_null(), "Failed to create vfs_iput workqueue");

        let thread = xv6_current_thread();
        kassert!(!thread.is_null(), "vfs_init must be called from a thread context");
        __vfs_inode_init(&raw mut vfs_root_inode);
        __vfs_file_init();
        (*thread).fs = vfs_struct_init();
        (*thread).fdtable = vfs_fdtable_init();

        // Initialize filesystem types (registers them with VFS).
        tmpfs_init();
        xv6fs_init();
        devtmpfs_init();

        // Mount filesystems.
        tmpfs_mount_root();
        xv6fs_mount_root();

        // Mount tmpfs at /tmp (after chroot to xv6fs).
        ret = vfs_mount_path(c"tmpfs".as_ptr(), c"/tmp".as_ptr(), 4, ptr::null(), 0);
        if ret == 0 {
            printf(c"tmpfs: mounted at /tmp\n".as_ptr());
        } else if ret == neg(ENOENT) {
            printf(c"tmpfs: /tmp directory not found\n".as_ptr());
        } else {
            printf(c"tmpfs: failed to mount at /tmp, errno=%d\n".as_ptr(), ret);
        }

        // Mount devtmpfs at /dev (auto-populated with device nodes).
        // Ensure /dev directory exists (create it if not present on root fs).
        let mut dev_dir = vfs_namei(c"/dev".as_ptr(), 4);
        if is_err_or_null(dev_dir) {
            let root = vfs_namei(c"/".as_ptr(), 1);
            if !is_err_or_null(root) {
                // vfs_mkdir handles its own locking -- do NOT lock root first.
                dev_dir = vfs_mkdir(root, 0o755, c"dev".as_ptr(), 3);
                vfs_iput(root);
                if !is_err_or_null(dev_dir) {
                    vfs_iput(dev_dir);
                }
            }
        } else {
            vfs_iput(dev_dir);
        }
        ret = vfs_mount_path(c"devtmpfs".as_ptr(), c"/dev".as_ptr(), 4, ptr::null(), 0);
        if ret == 0 {
            printf(c"devtmpfs: mounted at /dev\n".as_ptr());
            // Now that the superblock & root inode are fully VFS-initialised,
            // populate the registered device nodes using VFS-level APIs.
            devtmpfs_post_mount_populate();

            // Pre-create /dev/pts directory for PTY slaves.
            let dev_inode = vfs_namei(c"/dev".as_ptr(), 4);
            if !is_err_or_null(dev_inode) {
                let pts_dir = vfs_mkdir(dev_inode, 0o755, c"pts".as_ptr(), 3);
                if !is_err_or_null(pts_dir) {
                    vfs_iput(pts_dir);
                }
                vfs_iput(dev_inode);
            }
        } else if ret == neg(ENOENT) {
            printf(c"devtmpfs: /dev directory not found\n".as_ptr());
        } else {
            printf(c"devtmpfs: failed to mount at /dev, errno=%d\n".as_ptr(), ret);
        }

        // Smoke tests: not carried over (dead code -- see module doc /
        // wave report; the C original's call sites here were already
        // commented out).
    }
}

/// Mirrors `vfs_get_deferred_iput_wq()`.
pub(crate) extern "C" fn vfs_get_deferred_iput_wq() -> *mut workqueue {
    unsafe { __VFS_DEFERRED_IPUT_WQ }
}

unsafe extern "C" fn iput_work_func(work: *mut work_struct) {
    unsafe {
        let inode = (*work).data as *mut vfs_inode;
        vfs_iput(inode);
        free_work_struct(work);
    }
}

/// Mirrors `__vfs_queue_deferred_iput()`.
unsafe fn queue_deferred_iput(inode: *mut vfs_inode) {
    unsafe {
        let wq = vfs_get_deferred_iput_wq();
        if wq.is_null() {
            vfs_iput(inode);
            return;
        }
        let work = create_work_struct(Some(iput_work_func), inode as u64);
        if work.is_null() {
            printf(
                c"__vfs_queue_deferred_iput: failed to allocate work_struct, falling back to direct vfs_iput\n"
                    .as_ptr(),
            );
            vfs_iput(inode);
            return;
        }
        queue_work(wq, work);
    }
}

/// Mirrors `__vfs_shrink_caches()`. Called from `tmpfs`/`xv6fs` smoketest
/// C code when checking for leaks; real external linkage in the C
/// original (declared in `vfs_private.h`, a shared header).
pub(crate) extern "C" fn __vfs_shrink_caches() {
    unsafe {
        slab_cache_shrink(&raw mut VFS_SUPERBLOCK_CACHE, 0x7fffffff);
        slab_cache_shrink(&raw mut VFS_FS_TYPE_CACHE, 0x7fffffff);
        __vfs_file_shrink_cache();
    }
}

/// Mirrors `vfs_fs_type_allocate()`.
///
/// Locking: none.
pub(crate) extern "C" fn vfs_fs_type_allocate() -> *mut vfs_fs_type {
    unsafe {
        let fs_type = slab_alloc(&raw mut VFS_FS_TYPE_CACHE) as *mut vfs_fs_type;
        if fs_type.is_null() {
            return ptr::null_mut();
        }
        ptr::write_bytes(fs_type, 0, 1);
        ln_init(&raw mut (*fs_type).list_entry);
        ln_init(&raw mut (*fs_type).superblocks);
        fs_type
    }
}

pub(crate) extern "C" fn vfs_fs_type_free(fs_type: *mut vfs_fs_type) {
    unsafe { slab_free(fs_type as *mut c_void) };
}

/// Mirrors `vfs_register_fs_type()`.
///
/// Locking: caller must hold the mount mutex via [`vfs_mount_lock`].
pub(crate) extern "C" fn vfs_register_fs_type(fs_type: *mut vfs_fs_type) -> c_int {
    unsafe {
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            return neg(EPERM);
        }
        if fs_type.is_null() || (*fs_type).name.is_null() || (*fs_type).ops.is_null() {
            return neg(EINVAL);
        }
        if (*(*fs_type).ops).mount.is_none() || (*(*fs_type).ops).free.is_none() {
            return neg(EINVAL);
        }
        if (*fs_type).sb_count != 0 {
            return neg(EINVAL);
        }
        if (*fs_type).__bindgen_anon_1.registered() != 0 {
            return neg(EALREADY);
        }
        (*fs_type).kobj.ops.release = Some(vfs_fs_type_kobj_release);
        (*fs_type).kobj.name = c"fs_type".as_ptr();
        kobject_init(&raw mut (*fs_type).kobj);
        if VFS_FS_TYPE_COUNT >= MAX_FS_TYPES {
            return neg(ENOSPC);
        }
        let existing = get_fs_type_locked((*fs_type).name);
        if !existing.is_null() {
            return neg(EEXIST);
        }
        register_fs_type_locked(fs_type);
        0
    }
}

/// Mirrors `vfs_unregister_fs_type()`.
///
/// Locking: caller must hold the mount mutex via [`vfs_mount_lock`].
pub(crate) extern "C" fn vfs_unregister_fs_type(name: *const c_char) -> c_int {
    unsafe {
        if name.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            return neg(EPERM);
        }
        let pos = get_fs_type_locked(name);
        if !pos.is_null() {
            unregister_fs_type_locked(pos);
            kobject_put(&raw mut (*pos).kobj);
            return 0;
        }
        neg(ENOENT)
    }
}

/// Mirrors `vfs_mount_lock()`.
pub(crate) extern "C" fn vfs_mount_lock() {
    mutex_lock(unsafe { &raw mut __MOUNT_MUTEX });
}

/// Mirrors `vfs_mount_unlock()`.
pub(crate) extern "C" fn vfs_mount_unlock() {
    mutex_unlock(unsafe { &raw mut __MOUNT_MUTEX });
}

/******************************************************************************
 * Superblock Public APIs
 *****************************************************************************/

/// Mirrors `vfs_mount()`.
pub(crate) extern "C" fn vfs_mount(
    type_: *const c_char,
    mountpoint: *mut vfs_inode,
    device: *mut vfs_inode,
    flags: c_int,
    data: *const c_char,
) -> c_int {
    unsafe {
        let mut fs_type: *mut vfs_fs_type = ptr::null_mut();
        let mut sb: *mut vfs_superblock = ptr::null_mut();
        let mut ret_val: c_int;

        if type_.is_null() || mountpoint.is_null() {
            printf(c"vfs_mount: invalid arguments\n".as_ptr());
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            printf(c"vfs_mount: mount mutex not held\n".as_ptr());
            return neg(EPERM);
        }

        ret_val = dir_inode_valid_holding(mountpoint);
        if ret_val != 0 {
            printf(c"vfs_mount: mountpoint inode not valid, errno=%d\n".as_ptr(), ret_val);
            return ret_val;
        }
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            if !rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock) {
                printf(c"vfs_mount: mountpoint superblock write lock not held\n".as_ptr());
                return neg(EPERM);
            }
            if (*(*mountpoint).sb).__bindgen_anon_2.valid() == 0 {
                printf(c"vfs_mount: mountpoint superblock is not valid\n".as_ptr());
                return neg(EINVAL);
            }
            if !is_dir((*mountpoint).mode) {
                printf(c"vfs_mount: mountpoint is not a directory\n".as_ptr());
                return neg(EINVAL);
            }
        }

        ret_val = turn_mountpoint(mountpoint);
        if ret_val != 0 {
            printf(c"vfs_mount: failed to turn mountpoint, errno=%d\n".as_ptr(), ret_val);
            return ret_val;
        }

        'cleanup: {
            fs_type = vfs_get_fs_type(type_);
            if fs_type.is_null() {
                printf(c"vfs_mount: filesystem type '%s' not found\n".as_ptr(), type_);
                ret_val = neg(ENODEV);
                break 'cleanup;
            }
            if (*fs_type).__bindgen_anon_1.registered() == 0 {
                printf(c"vfs_mount: filesystem type '%s' not registered\n".as_ptr(), type_);
                ret_val = neg(ENODEV);
                break 'cleanup;
            }
            // Ask the filesystem type to allocate/initialize a new
            // superblock. Private to the filesystem until attached, so no
            // locking is needed yet.
            let mount_fn = (*(*fs_type).ops).mount.unwrap();
            ret_val = mount_fn(mountpoint, device, flags, data, &mut sb);
            if ret_val != 0 {
                printf(
                    c"vfs_mount: filesystem type '%s' mount failed, errno=%d\n".as_ptr(),
                    type_,
                    ret_val,
                );
                break 'cleanup;
            }
            if !init_superblock_valid(sb) {
                printf(c"vfs_mount: invalid superblock returned by mount\n".as_ptr());
                ret_val = neg(EINVAL);
                break 'cleanup;
            }
            if (*sb).total_blocks != 0 && (*sb).used_blocks > (*sb).total_blocks {
                printf(c"vfs_mount: superblock used_blocks exceeds total_blocks\n".as_ptr());
                ret_val = neg(EINVAL);
                break 'cleanup;
            }
            if (*sb).root_inode.is_null() {
                printf(c"vfs_mount: superblock has no root inode\n".as_ptr());
                ret_val = neg(EINVAL);
                break 'cleanup;
            }
            if (*(*sb).root_inode).__bindgen_anon_1.valid() != 0 {
                printf(c"vfs_mount: root inode already marked valid\n".as_ptr());
                ret_val = neg(EINVAL);
                break 'cleanup;
            }
            init_superblock_structure(sb, fs_type);
            vfs_superblock_wlock(sb); // Must hold sb lock to init root inode.
            ret_val = init_sb_rooti(sb);
            if ret_val != 0 {
                printf(
                    c"vfs_mount: failed to initialize superblock root inode, errno=%d\n".as_ptr(),
                    ret_val,
                );
                break 'cleanup;
            }

            attach_superblock_to_fstype(sb);
            (*sb).device = device;
            set_mountpoint(sb, mountpoint);
            (*(*sb).root_inode).sb = sb;
            ret_val = 0;
        }

        if ret_val != 0 {
            if !sb.is_null() {
                if !(*sb).root_inode.is_null() {
                    let free_inode = (*(*(*sb).root_inode).ops).free_inode.unwrap();
                    free_inode((*sb).root_inode);
                }
                let free_fn = (*(*fs_type).ops).free.unwrap();
                free_fn(sb);
            }
            clear_mountpoint(mountpoint);
            vfs_iunlock(mountpoint);
            if !(*mountpoint).sb.is_null() {
                vfs_superblock_unlock((*mountpoint).sb);
            }
            if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
                vfs_iput(mountpoint);
            }
            vfs_put_fs_type(fs_type);
            return ret_val;
        } else if rwsem_is_write_holding(&raw mut (*sb).lock) {
            (*sb).__bindgen_anon_2.set_initialized(1);
            (*sb).__bindgen_anon_2.set_valid(1);
            (*sb).__bindgen_anon_2.set_attached(1);
            vfs_superblock_unlock(sb);
        }
        vfs_put_fs_type(fs_type);
        ret_val
    }
}

/// Mirrors `__vfs_evict_unused_inodes()`.
///
/// Locking: caller must hold the superblock write lock.
unsafe fn evict_unused_inodes(sb: *mut vfs_superblock) -> usize {
    unsafe {
        let mut evicted = 0usize;
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Superblock lock must be write held to evict inodes"
        );

        let mut pos = sb_inodes_first(sb);
        while !pos.is_null() {
            let tmp = sb_inodes_next(sb, pos);
            let inode = pos;

            'skip: {
                if (*inode).ref_count > 1 {
                    break 'skip;
                }
                if (*inode).__bindgen_anon_1.destroying() != 0 {
                    break 'skip;
                }
                if (*inode).__bindgen_anon_1.valid() == 0 {
                    break 'skip;
                }
                if (*inode).__bindgen_anon_1.mount() != 0 {
                    break 'skip;
                }

                vfs_ilock(inode);

                if (*inode).ref_count > 1
                    || (*inode).__bindgen_anon_1.destroying() != 0
                    || (*inode).__bindgen_anon_1.valid() == 0
                    || (*inode).__bindgen_anon_1.mount() != 0
                {
                    vfs_iunlock(inode);
                    break 'skip;
                }

                if (*inode).__bindgen_anon_1.dirty() != 0 {
                    if let Some(sync_inode) = (*(*inode).ops).sync_inode {
                        sync_inode(inode);
                    }
                }

                if (*inode).ref_count == 1 {
                    inode_refcount_atomic(inode).fetch_sub(1, Ordering::SeqCst);
                }
                (*inode).__bindgen_anon_1.set_valid(0);
                vfs_remove_inode(sb, inode);
                vfs_iunlock(inode);

                let free_inode = (*(*inode).ops).free_inode.unwrap();
                free_inode(inode);
                evicted += 1;
            }

            pos = tmp;
        }

        evicted
    }
}

/// Mirrors `vfs_unmount()`.
pub(crate) extern "C" fn vfs_unmount(mountpoint: *mut vfs_inode) -> c_int {
    unsafe {
        if mountpoint.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            return neg(EPERM);
        }
        if holding_mutex(&raw mut (*mountpoint).mutex) == 0 {
            return neg(EPERM);
        }
        let mut ret_val = inode_valid(mountpoint);
        if ret_val != 0 {
            return ret_val;
        }
        if !rwsem_is_write_holding(&raw mut (*(*mountpoint).sb).lock) {
            return neg(EPERM);
        }
        if (*(*mountpoint).sb).__bindgen_anon_2.valid() == 0 {
            return neg(EINVAL);
        }
        if !is_dir((*mountpoint).mode) {
            return neg(ENOTDIR);
        }
        if (*mountpoint).__bindgen_anon_1.mount() == 0 {
            return neg(EINVAL);
        }
        let sb = (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_sb;
        if sb.is_null() {
            return neg(EINVAL);
        }
        let mounted_inode = (*sb).root_inode;
        if mounted_inode.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut (*mounted_inode).mutex) == 0 {
            return neg(EPERM);
        }
        ret_val = inode_valid(mounted_inode);
        if ret_val != 0 {
            return ret_val;
        }
        if !rwsem_is_write_holding(&raw mut (*sb).lock) {
            return neg(EPERM);
        }
        if (*sb).__bindgen_anon_2.valid() == 0 {
            return neg(EINVAL);
        }
        let mc = superblock_mountcount(sb);
        if mc > 0 {
            printf(c"vfs_unmount: mount_count=%d\n".as_ptr(), mc);
            return neg(EBUSY);
        }
        if (*sb).__bindgen_anon_2.dirty() != 0 {
            printf(
                c"vfs_unmount: sb valid=%u dirty=%u\n".as_ptr(),
                (*sb).__bindgen_anon_2.valid() as c_int,
                (*sb).__bindgen_anon_2.dirty() as c_int,
            );
            return neg(EBUSY);
        }

        // Begin unmounting.
        if let Some(unmount_begin) = (*(*sb).ops).unmount_begin {
            unmount_begin(sb);
        }

        // Evict all unreferenced inodes from the cache before checking.
        evict_unused_inodes(sb);

        // Superblock should have no active inodes except the root inode.
        let remaining_inodes = hlist_len(&raw mut (*sb).__bindgen_anon_1.inodes);
        if remaining_inodes > 1 {
            printf(
                c"vfs_unmount: remaining inodes=%lu (expected 1 for root)\n".as_ptr(),
                remaining_inodes,
            );
            return neg(EBUSY);
        }
        if remaining_inodes == 1 {
            let only_inode = sb_inodes_first(sb);
            if only_inode != mounted_inode {
                printf(
                    c"vfs_unmount: remaining inode is not root (ino=%lu)\n".as_ptr(),
                    (*only_inode).ino,
                );
                return neg(EBUSY);
            }
        }

        // Do NOT call destroy_inode on the root inode during unmount --
        // that would corrupt the on-disk filesystem. Just tear down the
        // in-memory state.
        (*mounted_inode).__bindgen_anon_1.set_valid(0);
        vfs_remove_inode(sb, mounted_inode);

        detach_superblock_from_fstype(sb);
        clear_mountpoint(mountpoint);

        vfs_iunlock(mounted_inode);
        // Free the root inode (one ref from `set_mountpoint`'s `vfs_idup`
        // plus the creation ref -- freed directly since already removed
        // from cache).
        let free_inode = (*(*mounted_inode).ops).free_inode.unwrap();
        free_inode(mounted_inode);
        (*sb).root_inode = ptr::null_mut();

        let fs_type = (*sb).fs_type;
        vfs_superblock_unlock(sb);

        vfs_iunlock(mountpoint);
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) && !(*mountpoint).sb.is_null() {
            vfs_superblock_unlock((*mountpoint).sb);
        }
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            vfs_iput(mountpoint);
        }

        let free_fn = (*(*fs_type).ops).free.unwrap();
        free_fn(sb);

        0
    }
}

/// Mirrors `vfs_make_orphan()`.
///
/// Locking: caller must hold the superblock write lock and the inode
/// mutex.
pub(crate) extern "C" fn vfs_make_orphan(inode: *mut vfs_inode) -> c_int {
    unsafe {
        if inode.is_null() {
            return neg(EINVAL);
        }
        let sb = (*inode).sb;
        if sb.is_null() {
            return neg(EINVAL);
        }

        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Must hold sb wlock to make orphan"
        );
        kassert!(
            holding_mutex(&raw mut (*inode).mutex) != 0,
            "Must hold inode lock to make orphan"
        );

        if (*inode).__bindgen_anon_1.orphan() != 0 {
            return 0; // Already orphan.
        }
        if (*inode).n_links != 0 {
            return neg(EINVAL); // Not unlinked yet.
        }

        (*inode).__bindgen_anon_1.set_orphan(1);
        ln_push_back(&raw mut (*sb).orphan_list, &raw mut (*inode).orphan_entry);
        (*sb).orphan_count += 1;

        // For backend fs: persist to on-disk orphan journal.
        if let Some(add_orphan) = (*(*sb).ops).add_orphan {
            let ret = add_orphan(sb, inode);
            if ret != 0 {
                printf(
                    c"vfs: warning: failed to persist orphan inode %lu, errno=%d\n".as_ptr(),
                    (*inode).ino,
                    ret,
                );
            }
        }

        0
    }
}

/// Mirrors `__vfs_final_unmount_cleanup()`. Called from `vfs_iput`
/// (`inode.rs`) when the last orphan inode is freed on a detached fs.
pub(crate) extern "C" fn __vfs_final_unmount_cleanup(sb: *mut vfs_superblock) {
    unsafe {
        if sb.is_null() {
            return;
        }

        kassert!(
            (*sb).__bindgen_anon_2.registered() == 0,
            "__vfs_final_unmount_cleanup: sb still attached"
        );
        kassert!((*sb).orphan_count == 0, "__vfs_final_unmount_cleanup: orphans remain");

        vfs_mount_lock();
        vfs_superblock_wlock(sb);

        if (*sb).__bindgen_anon_2.registered() != 0 {
            detach_superblock_from_fstype(sb);
        }

        if !(*sb).root_inode.is_null() {
            let rooti = (*sb).root_inode;
            vfs_ilock(rooti);
            if let Some(destroy_inode) = (*(*rooti).ops).destroy_inode {
                destroy_inode(rooti);
            }
            (*rooti).__bindgen_anon_1.set_valid(0);
            vfs_remove_inode(sb, rooti);
            vfs_iunlock(rooti);
            let free_inode = (*(*rooti).ops).free_inode.unwrap();
            free_inode(rooti);
            (*sb).root_inode = ptr::null_mut();
        }

        let fs_type = (*sb).fs_type;
        vfs_superblock_unlock(sb);
        vfs_mount_unlock();

        let free_fn = (*(*fs_type).ops).free.unwrap();
        free_fn(sb);
    }
}

/// Mirrors `vfs_unmount_lazy()`.
pub(crate) extern "C" fn vfs_unmount_lazy(mountpoint: *mut vfs_inode) -> c_int {
    unsafe {
        if mountpoint.is_null() {
            return neg(EINVAL);
        }
        if holding_mutex(&raw mut __MOUNT_MUTEX) == 0 {
            return neg(EPERM);
        }
        if holding_mutex(&raw mut (*mountpoint).mutex) == 0 {
            return neg(EPERM);
        }

        let parent_sb = (*mountpoint).sb;
        if !parent_sb.is_null() && !rwsem_is_write_holding(&raw mut (*parent_sb).lock) {
            return neg(EPERM);
        }

        let ret = inode_valid(mountpoint);
        if ret != 0 {
            return ret;
        }

        if !is_dir((*mountpoint).mode) {
            return neg(ENOTDIR);
        }
        if (*mountpoint).__bindgen_anon_1.mount() == 0 {
            return neg(EINVAL);
        }

        let sb = (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_sb;
        if sb.is_null() {
            return neg(EINVAL);
        }

        // Phase 1: check for child mounts.
        vfs_superblock_wlock(sb);

        if superblock_mountcount(sb) > 0 {
            vfs_superblock_unlock(sb);
            return neg(EBUSY);
        }

        // Block new operations.
        (*sb).__bindgen_anon_2.set_unmounting(1);

        // Phase 2: detach from mount tree.
        clear_mountpoint(mountpoint);
        (*sb).mountpoint = ptr::null_mut();
        (*sb).parent_sb = ptr::null_mut();
        (*sb).__bindgen_anon_2.set_attached(0);
        (*sb).__bindgen_anon_2.set_valid(0); // Prevent new lookups.

        vfs_iunlock(mountpoint);
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) && !(*mountpoint).sb.is_null() {
            vfs_superblock_unlock((*mountpoint).sb);
        }
        if !ptr::eq(mountpoint, &raw const vfs_root_inode) {
            vfs_iput(mountpoint);
        }

        // Phase 3: sync if needed (backend filesystems).
        if (*sb).__bindgen_anon_2.backendless() == 0 && (*sb).__bindgen_anon_2.dirty() != 0 {
            (*sb).__bindgen_anon_2.set_syncing(1);
            let sync_fs = (*(*sb).ops).sync_fs.unwrap();
            let sret = sync_fs(sb, 1);
            (*sb).__bindgen_anon_2.set_syncing(0);
            if sret != 0 {
                printf(
                    c"vfs_unmount_lazy: warning: sync failed, errno=%d\n".as_ptr(),
                    sret,
                );
            }
        }

        if let Some(unmount_begin) = (*(*sb).ops).unmount_begin {
            unmount_begin(sb);
        }

        // Phase 4: mark all referenced inodes as orphans.
        let rooti = (*sb).root_inode;
        let mut pos = sb_inodes_first(sb);
        while !pos.is_null() {
            let tmp = sb_inodes_next(sb, pos);
            let inode = pos;
            if inode != rooti && (*inode).ref_count > 0 {
                if (*inode).__bindgen_anon_1.orphan() == 0 {
                    vfs_ilock(inode);
                    (*inode).__bindgen_anon_1.set_orphan(1);
                    ln_push_back(&raw mut (*sb).orphan_list, &raw mut (*inode).orphan_entry);
                    (*sb).orphan_count += 1;
                    vfs_iunlock(inode);
                }
            }
            pos = tmp;
        }

        // Phase 5: immediate cleanup if no orphans.
        if (*sb).orphan_count == 0 {
            detach_superblock_from_fstype(sb);

            if !rooti.is_null() {
                vfs_ilock(rooti);
                if let Some(destroy_inode) = (*(*rooti).ops).destroy_inode {
                    destroy_inode(rooti);
                }
                (*rooti).__bindgen_anon_1.set_valid(0);
                vfs_remove_inode(sb, rooti);
                vfs_iunlock(rooti);
                let free_inode = (*(*rooti).ops).free_inode.unwrap();
                free_inode(rooti);
                (*sb).root_inode = ptr::null_mut();
            }

            let fs_type = (*sb).fs_type;
            vfs_superblock_unlock(sb);
            let free_fn = (*(*fs_type).ops).free.unwrap();
            free_fn(sb);
        } else {
            // Orphans exist -- cleanup deferred to vfs_iput.
            vfs_superblock_unlock(sb);
        }

        0
    }
}

/// Mirrors `vfs_get_mnt_rooti()`.
pub(crate) extern "C" fn vfs_get_mnt_rooti(
    mountpoint: *mut vfs_inode,
    ret_rooti: *mut *mut vfs_inode,
) -> c_int {
    unsafe {
        if mountpoint.is_null() || ret_rooti.is_null() {
            return neg(EINVAL);
        }
        let ret_val;
        vfs_ilock(mountpoint);
        ret_val = dir_inode_valid_holding(mountpoint);
        if ret_val != 0 {
            vfs_iunlock(mountpoint);
            return ret_val;
        }
        if !is_dir((*mountpoint).mode) {
            vfs_iunlock(mountpoint);
            return neg(ENOTDIR);
        }
        if (*mountpoint).__bindgen_anon_1.mount() == 0 {
            vfs_iunlock(mountpoint);
            return neg(EINVAL);
        }
        let sb = (*mountpoint).__bindgen_anon_2.__bindgen_anon_1.mnt_sb;
        if sb.is_null() {
            vfs_iunlock(mountpoint);
            return neg(EINVAL);
        }
        let rooti = (*sb).root_inode;
        if rooti.is_null() {
            vfs_iunlock(mountpoint);
            return neg(EINVAL);
        }
        // Take a reference to root inode BEFORE unlocking mountpoint.
        if !vfs_idup_not_zero(rooti) {
            vfs_iunlock(mountpoint);
            return neg(EINVAL);
        }
        vfs_iunlock(mountpoint);

        // Now we hold a reference, safe to lock.
        vfs_ilock(rooti);
        *ret_rooti = rooti;
        ret_val
    }
}

/// Mirrors `vfs_superblock_rlock()`.
pub(crate) extern "C" fn vfs_superblock_rlock(sb: *mut vfs_superblock) {
    if !sb.is_null() {
        unsafe { rwsem_acquire_read(&raw mut (*sb).lock) };
    }
}

/// Mirrors `vfs_superblock_wlock()`.
pub(crate) extern "C" fn vfs_superblock_wlock(sb: *mut vfs_superblock) {
    if !sb.is_null() {
        unsafe { rwsem_acquire_write(&raw mut (*sb).lock) };
    }
}

/// Mirrors `vfs_superblock_wholding()`.
pub(crate) extern "C" fn vfs_superblock_wholding(sb: *mut vfs_superblock) -> bool {
    if sb.is_null() {
        return false;
    }
    unsafe { rwsem_is_write_holding(&raw mut (*sb).lock) }
}

/// Mirrors `vfs_superblock_unlock()`.
pub(crate) extern "C" fn vfs_superblock_unlock(sb: *mut vfs_superblock) {
    if !sb.is_null() {
        unsafe { rwsem_release(&raw mut (*sb).lock) };
    }
}

/// Mirrors `vfs_superblock_spin_lock()`.
pub(crate) extern "C" fn vfs_superblock_spin_lock(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when acquiring spinlock");
        spin_lock(&raw mut (*sb).spinlock);
    }
}

/// Mirrors `vfs_superblock_spin_unlock()`.
pub(crate) extern "C" fn vfs_superblock_spin_unlock(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when releasing spinlock");
        spin_unlock(&raw mut (*sb).spinlock);
    }
}

/// Mirrors `vfs_superblock_mountcount_inc()`.
pub(crate) extern "C" fn vfs_superblock_mountcount_inc(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when incrementing mount count");
        let cnt = sb_mountcount_atomic(sb).fetch_add(1, Ordering::SeqCst) + 1;
        kassert!(cnt > 0, "Superblock mount count overflow");
    }
}

/// Mirrors `vfs_superblock_mountcount_dec()`.
pub(crate) extern "C" fn vfs_superblock_mountcount_dec(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when decrementing mount count");
        let cnt = sb_mountcount_atomic(sb).fetch_sub(1, Ordering::SeqCst) - 1;
        kassert!(cnt >= 0, "Superblock mount count underflow");
    }
}

/// Mirrors `vfs_superblock_dup()`.
///
/// # Safety notes (preserved from the C original)
/// Uses only atomic operations: does not acquire locks, sleep, or
/// allocate. Critical because `vfs_inode_get_ref()` may call this while
/// holding the inode lock -- any blocking here would risk deadlock.
pub(crate) extern "C" fn vfs_superblock_dup(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when duplicating");
        let ret = sb_refcount_atomic(sb).fetch_add(1, Ordering::SeqCst) + 1;
        kassert!(ret > 0, "Superblock refcount overflow");
    }
}

/// Mirrors `vfs_superblock_put()`.
pub(crate) extern "C" fn vfs_superblock_put(sb: *mut vfs_superblock) {
    unsafe {
        kassert!(!sb.is_null(), "Superblock cannot be NULL when putting");
        kassert!(
            !vfs_superblock_wholding(sb),
            "Cannot put superblock while holding its lock"
        );
        kassert!(
            holding_mutex(&raw mut __MOUNT_MUTEX) == 0,
            "Cannot put superblock while holding mount mutex"
        );
        kassert!(
            atomic_dec_unless(sb_refcount_atomic(sb), 0),
            "Superblock refcount underflow"
        );
    }
}

/// Mirrors `vfs_alloc_inode()`.
pub(crate) extern "C" fn vfs_alloc_inode(sb: *mut vfs_superblock) -> *mut vfs_inode {
    unsafe {
        if sb.is_null() {
            return err_ptr(neg(EINVAL));
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "vfs_alloc_inode: must hold superblock write lock"
        );
        if (*sb).__bindgen_anon_2.valid() == 0 {
            return err_ptr(neg(EINVAL));
        }
        let alloc_inode = (*(*sb).ops).alloc_inode.unwrap();
        let inode = alloc_inode(sb);
        if is_err(inode) {
            return inode;
        }
        __vfs_inode_init(inode);
        let existing = vfs_add_inode(sb, inode);
        if is_err_or_null(existing) {
            if is_eagain_ptr(existing) {
                let free_inode = (*(*inode).ops).free_inode.unwrap();
                free_inode(inode);
                return err_ptr(neg(EAGAIN));
            }
            let free_inode = (*(*inode).ops).free_inode.unwrap();
            free_inode(inode);
            if existing.is_null() {
                return err_ptr(neg(ENOENT));
            }
            return existing;
        }
        inode // locked
    }
}

/// Mirrors `vfs_get_inode()`.
pub(crate) extern "C" fn vfs_get_inode(sb: *mut vfs_superblock, ino: u64) -> *mut vfs_inode {
    unsafe {
        if sb.is_null() {
            return err_ptr(neg(EINVAL));
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "vfs_get_inode: must hold superblock write lock"
        );
        if (*sb).__bindgen_anon_2.valid() == 0 {
            return err_ptr(neg(EINVAL));
        }
        let get_inode = (*(*sb).ops).get_inode.unwrap();
        let inode = get_inode(sb, ino);
        if is_err(inode) {
            return inode;
        }
        __vfs_inode_init(inode);
        let existing = vfs_add_inode(sb, inode);
        if is_err_or_null(existing) {
            if is_eagain_ptr(existing) {
                let free_inode = (*(*inode).ops).free_inode.unwrap();
                free_inode(inode);
                return err_ptr(neg(EAGAIN));
            }
            let free_inode = (*(*inode).ops).free_inode.unwrap();
            free_inode(inode);
            if existing.is_null() {
                return err_ptr(neg(ENOENT));
            }
            return existing;
        }
        if existing != inode {
            // Found existing inode in hash -- free the newly loaded one.
            let free_inode = (*(*inode).ops).free_inode.unwrap();
            free_inode(inode);
            return existing; // locked
        }
        inode // locked
    }
}

/// Mirrors `vfs_sync_superblock()`.
pub(crate) extern "C" fn vfs_sync_superblock(sb: *mut vfs_superblock, wait: c_int) -> c_int {
    unsafe {
        if sb.is_null() {
            return neg(EINVAL);
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "vfs_sync_superblock: must hold superblock write lock"
        );
        if (*sb).__bindgen_anon_2.valid() == 0 {
            return neg(EINVAL);
        }
        if (*sb).__bindgen_anon_2.dirty() == 0 {
            return 0; // Already clean.
        }
        let sync_fs = (*(*sb).ops).sync_fs.unwrap();
        let ret = sync_fs(sb, wait);
        if ret == 0 {
            (*sb).__bindgen_anon_2.set_dirty(0);
        }
        ret
    }
}

/// Mirrors `vfs_get_fs_type()`.
///
/// Locking: caller must hold the mount mutex via [`vfs_mount_lock`].
pub(crate) extern "C" fn vfs_get_fs_type(name: *const c_char) -> *mut vfs_fs_type {
    unsafe {
        if name.is_null() {
            return ptr::null_mut();
        }
        kassert!(
            holding_mutex(&raw mut __MOUNT_MUTEX) != 0,
            "vfs_put_fs_type: must hold mount mutex"
        );
        let fs_type = get_fs_type_locked(name);
        if !fs_type.is_null() {
            kobject_get(&raw mut (*fs_type).kobj);
        }
        fs_type
    }
}

/// Mirrors `vfs_put_fs_type()`.
///
/// Locking: caller must hold the mount mutex via [`vfs_mount_lock`].
pub(crate) extern "C" fn vfs_put_fs_type(fs_type: *mut vfs_fs_type) {
    unsafe {
        if fs_type.is_null() {
            return;
        }
        kassert!(
            holding_mutex(&raw mut __MOUNT_MUTEX) != 0,
            "vfs_put_fs_type: must hold mount mutex"
        );
        kobject_put(&raw mut (*fs_type).kobj);
    }
}

/// Mirrors `__vfs_dentry_get_self_inode()`.
///
/// Locking: none required -- the parent inode is guaranteed alive as long
/// as the dentry is (VFS always caches ancestor directories).
unsafe fn dentry_get_self_inode(dentry: *mut vfs_dentry) -> *mut vfs_inode {
    unsafe {
        if dentry.is_null() || (*dentry).parent.is_null() {
            return ptr::null_mut();
        }
        if (*(*dentry).parent).sb == (*dentry).sb && (*(*dentry).parent).ino == (*dentry).ino {
            vfs_idup((*dentry).parent);
            return (*dentry).parent;
        }
        ptr::null_mut()
    }
}

/// Mirrors `__vfs_set_parent_from_dentry()`.
///
/// Locking: caller holds the inode lock.
unsafe fn set_parent_from_dentry(inode: *mut vfs_inode, parent: *mut vfs_inode) {
    unsafe {
        if !parent.is_null() && is_dir((*inode).mode) {
            if inode != parent {
                (*inode).parent = parent;
                vfs_idup(parent);
            }
        }
    }
}

/// Mirrors `__vfs_set_name_if_null()`.
///
/// Locking: caller holds the inode lock.
unsafe fn set_name_if_null(inode: *mut vfs_inode, dentry: *mut vfs_dentry) {
    unsafe {
        if is_dir((*inode).mode)
            && (*inode).name.is_null()
            && !(*dentry).name.is_null()
            && (*dentry).name_len > 0
        {
            (*inode).name = strndup((*dentry).name, (*dentry).name_len as usize);
        }
    }
}

/// Mirrors `__vfs_get_dentry_inode_impl()`.
///
/// Locking: caller holds the dentry's superblock read lock on entry; this
/// helper may drop the read lock and acquire the write lock internally.
unsafe fn get_dentry_inode_impl(dentry: *mut vfs_dentry) -> *mut vfs_inode {
    unsafe {
        let sb = (*dentry).sb;

        // vfs_get_inode_cached returns with refcount already incremented.
        let mut inode = vfs_get_inode_cached(sb, (*dentry).ino);
        if !is_err_or_null(inode) {
            set_name_if_null(inode, dentry);
            vfs_iunlock(inode);
            return inode;
        }
        if ptr_err(inode) != neg(ENOENT) as isize {
            return inode;
        }

        if !rwsem_is_write_holding(&raw mut (*sb).lock) {
            vfs_superblock_unlock(sb);
            vfs_superblock_wlock(sb);
        }

        if (*sb).__bindgen_anon_2.valid() == 0 {
            return err_ptr(neg(EINVAL));
        }

        inode = vfs_get_inode_cached(sb, (*dentry).ino);
        if !is_err_or_null(inode) {
            set_name_if_null(inode, dentry);
            vfs_iunlock(inode);
            return inode;
        }
        if ptr_err(inode) != neg(ENOENT) as isize {
            return inode;
        }

        inode = vfs_get_inode(sb, (*dentry).ino);
        if is_err_or_null(inode) {
            return inode;
        }

        // "." and ".." are synthesized by VFS and should always hit the
        // cache.
        kassert!(
            !((*dentry).name_len == 1 && *(*dentry).name.offset(0) == b'.' as c_char),
            "__vfs_get_dentry_inode_impl: \".\" should not reach fresh load path"
        );
        kassert!(
            !((*dentry).name_len == 2
                && *(*dentry).name.offset(0) == b'.' as c_char
                && *(*dentry).name.offset(1) == b'.' as c_char),
            "__vfs_get_dentry_inode_impl: \"..\" should not reach fresh load path"
        );

        set_parent_from_dentry(inode, (*dentry).parent);
        set_name_if_null(inode, dentry);
        vfs_iunlock(inode);
        inode
    }
}

/// Mirrors `vfs_get_dentry_inode_locked()`.
pub(crate) extern "C" fn vfs_get_dentry_inode_locked(dentry: *mut vfs_dentry) -> *mut vfs_inode {
    unsafe {
        if dentry.is_null() {
            return err_ptr(neg(EINVAL));
        }
        if (*dentry).sb.is_null() {
            return err_ptr(neg(EINVAL));
        }
        if (*(*dentry).sb).__bindgen_anon_2.valid() == 0 {
            return err_ptr(neg(EINVAL));
        }

        let inode = dentry_get_self_inode(dentry);
        if !inode.is_null() {
            return inode;
        }

        get_dentry_inode_impl(dentry)
    }
}

/// Mirrors `vfs_get_dentry_inode()`.
pub(crate) extern "C" fn vfs_get_dentry_inode(dentry: *mut vfs_dentry) -> *mut vfs_inode {
    unsafe {
        if dentry.is_null() {
            return err_ptr(neg(EINVAL));
        }
        if (*dentry).sb.is_null() {
            return err_ptr(neg(EINVAL));
        }

        let inode = dentry_get_self_inode(dentry);
        if !inode.is_null() {
            return inode;
        }

        let sb = (*dentry).sb;
        vfs_superblock_rlock(sb);
        if (*sb).__bindgen_anon_2.valid() == 0 {
            vfs_superblock_unlock(sb);
            return err_ptr(neg(EINVAL));
        }
        let inode = get_dentry_inode_impl(dentry);
        vfs_superblock_unlock(sb);
        inode
    }
}

/******************************************************************************
 * Module scope private functions (declared in vfs_private.h; real
 * external linkage -- other C translation units under kernel/vfs/
 * include that header).
 *****************************************************************************/

/// Mirrors `vfs_get_inode_cached()`.
///
/// Locking: caller holds the superblock read or write lock for the
/// entire call. On success, the returned inode is locked.
pub(crate) extern "C" fn vfs_get_inode_cached(sb: *mut vfs_superblock, ino: u64) -> *mut vfs_inode {
    unsafe {
        if sb.is_null() {
            return err_ptr(neg(EINVAL));
        }
        if (*sb).__bindgen_anon_2.valid() == 0 {
            return err_ptr(neg(EINVAL));
        }
        let inode = inode_hash_get(sb, ino);
        if inode.is_null() {
            return err_ptr(neg(ENOENT));
        }
        // CRITICAL: take a reference BEFORE locking to prevent
        // use-after-free. Backendless filesystems keep refcount=0,
        // n_links>0 inodes alive in cache; allow bumping from 0 in that
        // case, otherwise the inode is unreachable.
        if !vfs_idup_not_zero(inode) {
            if (*sb).__bindgen_anon_2.backendless() != 0
                && (*inode).n_links > 0
                && (*inode).__bindgen_anon_1.valid() != 0
                && (*inode).__bindgen_anon_1.destroying() == 0
            {
                inode_refcount_atomic(inode).fetch_add(1, Ordering::SeqCst);
            } else {
                return err_ptr(neg(ENOENT)); // Inode is dying.
            }
        }
        vfs_ilock(inode);
        if (*inode).__bindgen_anon_1.valid() == 0 || (*inode).__bindgen_anon_1.destroying() != 0 {
            // Invalidated or being destroyed after being fetched from the
            // cache. Can't call vfs_iput here (caller may hold sb wlock);
            // queue to the workqueue instead.
            vfs_iunlock(inode);
            queue_deferred_iput(inode);
            return err_ptr(neg(ENOENT));
        }
        inode
    }
}

/// Mirrors `vfs_add_inode()`.
///
/// Locking: caller holds the superblock write lock. On success, the
/// returned inode is locked.
pub(crate) extern "C" fn vfs_add_inode(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> *mut vfs_inode {
    unsafe {
        if sb.is_null() || inode.is_null() {
            return err_ptr(neg(EINVAL));
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Superblock lock must be write held to add inode"
        );
        if (*sb).__bindgen_anon_2.valid() == 0 && (*sb).__bindgen_anon_2.initialized() != 0 {
            return err_ptr(neg(EINVAL));
        }
        if !(*inode).sb.is_null() {
            return err_ptr(neg(EINVAL));
        }
        if (*inode).__bindgen_anon_1.valid() != 0 {
            return err_ptr(neg(EINVAL));
        }
        let existing = inode_hash_get(sb, (*inode).ino);
        if !existing.is_null() {
            // Check destroying WITHOUT locking (to avoid deadlock -- see
            // the C original's comment: vfs_iput holds the inode lock,
            // releases sb lock, calls destroy_inode; we hold sb lock, so
            // if it's set, the destroying thread has released sb lock and
            // is in destroy_inode).
            if (*existing).__bindgen_anon_1.destroying() != 0 {
                return err_ptr(neg(EAGAIN));
            }
            vfs_ilock(existing);
            if (*existing).__bindgen_anon_1.destroying() != 0
                || (*existing).__bindgen_anon_1.valid() == 0
            {
                vfs_iunlock(existing);
                return err_ptr(neg(EAGAIN));
            }
            return existing;
        }
        let popped = inode_hash_add(sb, inode);
        if !popped.is_null() {
            xv6_panic(
                c"vfs_add_inode: inode hash add returned existing inode unexpectedly".as_ptr(),
            );
        }
        (*inode).__bindgen_anon_1.set_valid(1);
        (*inode).sb = sb;
        vfs_ilock(inode);
        inode
    }
}

/// Mirrors `vfs_remove_inode()`.
///
/// Locking: caller holds the superblock write lock and the inode mutex.
pub(crate) extern "C" fn vfs_remove_inode(sb: *mut vfs_superblock, inode: *mut vfs_inode) -> c_int {
    unsafe {
        if sb.is_null() || inode.is_null() {
            return neg(EINVAL);
        }
        kassert!(
            rwsem_is_write_holding(&raw mut (*sb).lock),
            "Superblock lock must be write held to remove inode"
        );
        kassert!(
            holding_mutex(&raw mut (*inode).mutex) != 0,
            "Inode lock must be held to remove inode"
        );
        // Allow removal from detached superblocks (lazy unmount cleanup).
        if (*sb).__bindgen_anon_2.valid() == 0 && (*sb).__bindgen_anon_2.attached() != 0 {
            return neg(EINVAL);
        }

        let already_destroyed = (*inode).__bindgen_anon_1.valid() == 0;

        let existing = inode_hash_get(sb, (*inode).ino);
        if existing.is_null() {
            return neg(ENOENT);
        }
        if existing != inode {
            return neg(ENOENT);
        }
        let popped = hlist_pop(
            &raw mut (*sb).__bindgen_anon_1.inodes,
            inode as *mut c_void,
        ) as *mut vfs_inode;
        if popped != inode {
            xv6_panic(c"vfs_remove_inode: inode hash pop returned unexpected inode".as_ptr());
        }

        if !already_destroyed {
            (*inode).__bindgen_anon_1.set_valid(0);
        }

        (*inode).sb = ptr::null_mut();
        0
    }
}

/// Mirrors `vfs_release_dentry()`.
pub(crate) extern "C" fn vfs_release_dentry(dentry: *mut vfs_dentry) {
    unsafe {
        if dentry.is_null() {
            return;
        }
        if !(*dentry).name.is_null() {
            kmm_free((*dentry).name as *mut c_void);
            (*dentry).name = ptr::null_mut();
            (*dentry).name_len = 0;
        }
    }
}

/// Mirrors `vfs_struct_init()`.
pub(crate) extern "C" fn vfs_struct_init() -> *mut fs_struct {
    unsafe {
        let fs = struct_alloc_init();
        kassert!(!fs.is_null(), "idle_thread_init: failed to create fs_struct");
        // Preserved 1:1 from the C original: `__vfs_struct_alloc_init`
        // already performs this exact `spin_init`/refcount-store pair;
        // `vfs_struct_init` redoes it (harmless -- both are idempotent
        // full re-initializations of freshly allocated, still-exclusive
        // memory).
        fs_refcount_atomic(fs).store(1, Ordering::Release);
        spin_init(&raw mut (*fs).lock, FS_STRUCT_LOCK_NAME.as_ptr() as *mut c_char);
        (*fs).rooti.sb = ptr::null_mut();
        (*fs).rooti.inode = ptr::null_mut();
        (*fs).cwd.sb = ptr::null_mut();
        (*fs).cwd.inode = ptr::null_mut();
        fs
    }
}

/// Mirrors `vfs_struct_clone()`.
pub(crate) extern "C" fn vfs_struct_clone(old_fs: *mut fs_struct, clone_flags: u64) -> *mut fs_struct {
    unsafe {
        if old_fs.is_null() {
            return err_ptr(neg(EINVAL));
        }

        if clone_flags & CLONE_FS != 0 {
            // Share the fs_struct.
            fs_refcount_atomic(old_fs).fetch_add(1, Ordering::SeqCst);
            return old_fs;
        }

        let new_fs = struct_alloc_init();
        if new_fs.is_null() {
            return err_ptr(neg(crate::bindings::ENOMEM));
        }

        // Get inode pointers under spinlock, but take references outside
        // (vfs_inode_get_ref may acquire the inode mutex).
        spin_lock(&raw mut (*old_fs).lock);
        let rooti = (*old_fs).rooti.inode;
        let cwdi = (*old_fs).cwd.inode;
        let rooti_ok = !rooti.is_null() && vfs_idup_not_zero(rooti);
        let cwdi_ok = !cwdi.is_null() && vfs_idup_not_zero(cwdi);
        spin_unlock(&raw mut (*old_fs).lock);

        let mut ret;
        if rooti_ok {
            ret = vfs_inode_get_ref(rooti, &raw mut (*new_fs).rooti);
            vfs_iput(rooti);
            if ret != 0 {
                if cwdi_ok {
                    vfs_iput(cwdi);
                }
                vfs_inode_put_ref(&raw mut (*new_fs).rooti);
                vfs_inode_put_ref(&raw mut (*new_fs).cwd);
                struct_free(new_fs);
                return err_ptr(ret);
            }
        }
        if cwdi_ok {
            ret = vfs_inode_get_ref(cwdi, &raw mut (*new_fs).cwd);
            vfs_iput(cwdi);
            if ret != 0 {
                vfs_inode_put_ref(&raw mut (*new_fs).rooti);
                vfs_inode_put_ref(&raw mut (*new_fs).cwd);
                struct_free(new_fs);
                return err_ptr(ret);
            }
        }
        new_fs
    }
}

/// Mirrors `vfs_struct_put()`.
pub(crate) extern "C" fn vfs_struct_put(fs: *mut fs_struct) {
    unsafe {
        if fs.is_null() {
            return;
        }
        if !atomic_dec_unless(fs_refcount_atomic(fs), 1) {
            vfs_inode_put_ref(&raw mut (*fs).rooti);
            vfs_inode_put_ref(&raw mut (*fs).cwd);
            struct_free(fs);
        }
    }
}

/// Mirrors `vfs_inode_get_ref()`.
pub(crate) extern "C" fn vfs_inode_get_ref(inode: *mut vfs_inode, r: *mut vfs_inode_ref) -> c_int {
    unsafe {
        if inode.is_null() || r.is_null() {
            return neg(EINVAL);
        }
        let sb = (*inode).sb;
        if sb.is_null() {
            return neg(EINVAL);
        }
        // Caller must already hold a reference, so these can't fail.
        vfs_superblock_dup(sb);
        vfs_idup(inode);
        (*r).sb = sb;
        (*r).inode = inode;
        0
    }
}

/// Mirrors `vfs_inode_put_ref()`.
pub(crate) extern "C" fn vfs_inode_put_ref(r: *mut vfs_inode_ref) {
    unsafe {
        if r.is_null() {
            return;
        }
        if !(*r).inode.is_null() {
            vfs_iput((*r).inode);
            (*r).inode = ptr::null_mut();
        }
        if !(*r).sb.is_null() {
            vfs_superblock_put((*r).sb);
            (*r).sb = ptr::null_mut();
        }
    }
}

/// Mirrors `vfs_inode_deref()`.
pub(crate) extern "C" fn vfs_inode_deref(r: *mut vfs_inode_ref) -> *mut vfs_inode {
    if r.is_null() {
        ptr::null_mut()
    } else {
        unsafe { (*r).inode }
    }
}

/******************************************************************************
 * Debug: dump all active inodes
 ******************************************************************************/

fn inode_mode_str(mode: u32) -> *const c_char {
    if is_dir(mode) {
        return c"DIR".as_ptr();
    }
    if is_reg(mode) {
        return c"REG".as_ptr();
    }
    if is_lnk(mode) {
        return c"LNK".as_ptr();
    }
    if is_chr(mode) {
        return c"CHR".as_ptr();
    }
    if is_blk(mode) {
        return c"BLK".as_ptr();
    }
    if is_fifo(mode) {
        return c"FIFO".as_ptr();
    }
    if is_sock(mode) {
        return c"SOCK".as_ptr();
    }
    c"???".as_ptr()
}

/// Mirrors `__dump_sb_inodes()`.
///
/// Locking: caller must hold the superblock read lock.
unsafe fn dump_sb_inodes(sb: *mut vfs_superblock) {
    unsafe {
        let mut inode_count = 0;
        let mut active_count = 0;

        let mut pos = sb_inodes_first(sb);
        while !pos.is_null() {
            inode_count += 1;
            if (*pos).ref_count > 0 {
                active_count += 1;
            }
            pos = sb_inodes_next(sb, pos);
        }

        printf(
            c"  Superblock %p: valid=%d attached=%d backendless=%d inodes: total=%d active=%d\n"
                .as_ptr(),
            sb,
            (*sb).__bindgen_anon_2.valid() as c_int,
            (*sb).__bindgen_anon_2.attached() as c_int,
            (*sb).__bindgen_anon_2.backendless() as c_int,
            inode_count,
            active_count,
        );

        pos = sb_inodes_first(sb);
        while !pos.is_null() {
            let inode = pos;
            pos = sb_inodes_next(sb, pos);
            if (*inode).ref_count > 0 || (*inode).n_links > 0 {
                printf(
                    c"    ino=%lu type=%s ref=%d n_links=%d valid=%d dirty=%d destroying=%d orphan=%d"
                        .as_ptr(),
                    (*inode).ino,
                    inode_mode_str((*inode).mode),
                    (*inode).ref_count,
                    (*inode).n_links,
                    (*inode).__bindgen_anon_1.valid() as c_int,
                    (*inode).__bindgen_anon_1.dirty() as c_int,
                    (*inode).__bindgen_anon_1.destroying() as c_int,
                    (*inode).__bindgen_anon_1.orphan() as c_int,
                );
                if is_dir((*inode).mode) {
                    if !(*inode).name.is_null() {
                        printf(c" name=\"%s\"".as_ptr(), (*inode).name);
                    }
                    if !(*inode).parent.is_null() {
                        printf(c" parent_ino=%lu".as_ptr(), (*(*inode).parent).ino);
                    }
                }
                if (*inode).__bindgen_anon_1.mount() != 0 {
                    printf(
                        c" [mountpoint mnt_sb=%p]".as_ptr(),
                        (*inode).__bindgen_anon_2.__bindgen_anon_1.mnt_sb,
                    );
                } else if is_chr((*inode).mode) {
                    printf(c" cdev=%u".as_ptr(), (*inode).__bindgen_anon_2.cdev);
                } else if is_blk((*inode).mode) {
                    printf(c" bdev=%u".as_ptr(), (*inode).__bindgen_anon_2.bdev);
                }
                printf(c"\n".as_ptr());
            }
        }
    }
}

/// Mirrors `vfs_dump_sb_inodes()`.
pub(crate) extern "C" fn vfs_dump_sb_inodes(sb: *mut vfs_superblock) {
    unsafe {
        if sb.is_null() {
            printf(c"vfs_dump_sb_inodes: NULL superblock\n".as_ptr());
            return;
        }

        printf(c"\n=== VFS Superblock Inode Dump ===\n".as_ptr());
        let fs_type = (*sb).fs_type;
        if fs_type.is_null() {
            printf(c"Filesystem type: (null)\n".as_ptr());
        } else {
            printf(c"Filesystem type: %s\n".as_ptr(), (*fs_type).name);
        }

        vfs_superblock_rlock(sb);
        dump_sb_inodes(sb);
        vfs_superblock_unlock(sb);

        printf(c"\n=== End of Superblock Inode Dump ===\n\n".as_ptr());
    }
}

/// Mirrors `vfs_dump_inodes()`. `vfs_fs_type.list_entry` and
/// `vfs_superblock.siblings` are both their struct's first field (offset
/// 0), so these walks cast `list_node_t` pointers directly (see the
/// hlist-walker section doc for the same reasoning).
pub(crate) extern "C" fn vfs_dump_inodes() {
    unsafe {
        printf(c"\n=== VFS Inode Dump ===\n".as_ptr());

        vfs_mount_lock();

        let fs_head = &raw mut VFS_FS_TYPES;
        let mut fpos = (*fs_head).next;
        while fpos != fs_head {
            let fstype = fpos as *mut vfs_fs_type;
            fpos = (*fpos).next;

            if (*fstype).sb_count == 0 {
                continue;
            }

            printf(
                c"\nFilesystem type: %s (superblocks: %d)\n".as_ptr(),
                (*fstype).name,
                (*fstype).sb_count,
            );

            let sb_head = &raw mut (*fstype).superblocks;
            let mut spos = (*sb_head).next;
            while spos != sb_head {
                let sb = spos as *mut vfs_superblock;
                spos = (*spos).next;

                vfs_superblock_rlock(sb);
                dump_sb_inodes(sb);
                vfs_superblock_unlock(sb);
            }
        }

        vfs_mount_unlock();
        printf(c"\n=== End of Inode Dump ===\n\n".as_ptr());
    }
}
