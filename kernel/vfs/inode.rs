//! VFS inode operations — Rust port of `kernel/vfs/inode.c` (Phase 2
//! Wave 13, see `docs/rustify/phase2_plan.md`).
//!
//! This is the inode cache/refcount/locking core of the VFS: the
//! `vfs_inode` lifecycle (`vfs_idup`/`vfs_iput`/`vfs_ilock`/`vfs_iunlock`),
//! the `vfs_inode_ops` vtable dispatch to filesystem drivers (tmpfs/
//! xv6fs/devtmpfs — all still C), directory lookup/iteration, and
//! `vfs_namei` path resolution. Every other file under `kernel/vfs/`
//! (`fs.c`, `file.c`, `fdtable.c`, `vfs_syscall.c`, `pipe.c`, and the
//! `tmpfs/`/`xv6fs/`/`devtmpfs/` filesystem drivers) remains C and is
//! called through the `unsafe extern "C"` block below, exactly as any
//! other C translation unit would call these functions.
//!
//! # Lock map
//!
//! 1. mount lock (`vfs_mount_lock`, fs.c) — never touched by this file.
//! 2. `vfs_superblock.lock` (rwsem) — via the C `vfs_superblock_{r,w}lock`/
//!    `vfs_superblock_unlock` entry points (fs.c owns the `rwsem_t`
//!    storage; this file never reaches into `sb->lock` directly).
//! 3. `vfs_inode.mutex` — via [`vfs_ilock`]/[`vfs_ilock_trylock`]/
//!    [`vfs_iunlock`] (this file owns and ports these).
//! 4. buffer mutex / 5. filesystem log spinlock — internal to the
//!    filesystem driver callbacks invoked through `vfs_inode_ops`/
//!    `vfs_superblock_ops`; opaque to this file.
//!
//! `vfs_ilock`/`vfs_ilock_trylock`/`vfs_iunlock` are a paired C-ABI
//! lock/unlock triple called from a dozen other C translation units
//! (tmpfs, xv6fs, `file.c`, `fs.c`, `vfs_syscall.c`, ...) exactly like
//! `vm_rlock`/`vm_runlock` from `kernel/mm/vm.rs` (mm WP1 precedent) —
//! they stay thin forwards to `mutex_lock`/`mutex_trylock`/`mutex_unlock`
//! rather than `crate::sync::KMutex` RAII, because the matching unlock
//! call happens in a *different* function invocation (often a different
//! C translation unit), which no RAII guard can span. Internal critical
//! sections in this file follow the exact lock/unlock placement of the
//! C original (including its retry loops and asymmetric unlock
//! ordering — e.g. `vfs_iput`'s `inode´ then `sb` order, `vfs_unlink`'s
//! `target` then `dir` then `sb` order) rather than being restructured
//! into RAII scopes, because several of them (`vfs_iput` above all) drop
//! and reacquire different lock *combinations* across a `retry:` loop in
//! a way that does not nest as stack-scoped guards. `fs_struct.lock` (a
//! plain spinlock with no other C caller of its own address) is the one
//! internal, cleanly-paired site in this file and does use
//! [`crate::sync::KSpinlock`] RAII (see [`fs_lock`]), matching the
//! `crate::sync` precedent already used by `tty/ptmx.rs`/`tty/tty.rs`/
//! `irq/irq_core.rs`.
//!
//! # Refcount map
//!
//! `vfs_inode.ref_count` (a plain C `int`) is manipulated both by atomic
//! RMW (`vfs_idup`/`vfs_idup_not_zero`/`vfs_iput`/mount-descend in
//! `vfs_namei`, all lock-free) and, in exactly the two spots the C
//! original does, by a non-atomic read protected by the inode+superblock
//! locks held at that point (`vfs_iput`'s post-decrement underflow
//! assert, folded here into the `fetch_sub` return value instead of a
//! second racy read; `vfs_unlink`'s orphan-marking `n_links==0 &&
//! ref_count>1` check, kept as a genuine plain read to match the C
//! exactly — see the comment at that call site). All atomic ops use
//! `Ordering::SeqCst` (CAS) / `Ordering::Acquire` (initial load),
//! mirroring `smp/atomic.h`'s `atomic_oper_cond` macro
//! (`__ATOMIC_SEQ_CST` compare-exchange over an `__ATOMIC_ACQUIRE`
//! initial load) verbatim — this is a line-faithful port, not an
//! independent re-derivation of weaker orderings, because refcounting
//! correctness here depends on matching the original exactly.
//!
//! # Style notes (rust-skills)
//!
//! Every `unsafe` block is scoped to the smallest expression that needs
//! it (`unsafe-minimize-scope`) with a `SAFETY:` comment at each
//! non-obvious site; the C `static inline` helpers this file depends on
//! (`list_entry_init`/`list_entry_detach`/`hlist_entry_init` from
//! `list.h`/`hlist.h`, `__vfs_inode_valid`/`vfs_inode_is_local_root`
//! from `vfs_private.h`/`fs.h`) have no external linkage and are
//! reimplemented natively here, same precedent as `kernel/kobject.rs`
//! (Wave 1). Bitfield flags (`valid`/`dirty`/`mount`/`orphan`/
//! `destroying`/`delay_put`) are accessed through bindgen's generated
//! `__bindgen_anon_1.field()`/`.set_field()` methods on a temporary
//! place expression (`(*ptr).__bindgen_anon_1...`), the same pattern
//! `kernel/mm/pcache.rs` uses — never by materializing a `&mut vfs_inode`
//! for the whole struct. Deliberate deviations from a byte-for-byte C
//! transliteration are called out inline (see `vfs_chroot`'s discarded
//! `vfs_chdir` return value, and the `panic!`-free tail-call use of
//! `xv6_panic`'s `-> !` return type replacing the C's dead
//! `return -EINVAL;`).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ops::Deref;
use core::ptr;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{
    dev_t, fs_struct, hlist_entry_t, list_node_t, loff_t, mode_t, mutex_t, thread, vfs_dentry,
    vfs_dir_iter, vfs_inode, vfs_inode_ref, vfs_superblock, EAGAIN, EBUSY, EINVAL, ENAMETOOLONG,
    ENOENT, ENOMEM, ENOSYS, ENOTDIR, ENOTEMPTY, EPERM, EXDEV,
};

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally rather than imported by Rust path (matches the established
// convention across the crate: `irq/trap.rs`, `irq/syscall.rs`,
// `tty/session.rs`, `mm/vm.rs`'s own externs).
//
// Nearly everything is marked `safe`: passing a raw pointer is never
// itself unsafe in Rust, and every one of these C entry points either
// null-checks internally (the `vfs_superblock_*` lock wrappers) or is
// called here only with pointers this file already proved non-null —
// the same `safe fn` convention `kernel/mm/vm.rs`/`kernel/sync/sync.rs`
// use for `rwsem_acquire_read` and friends. `printf` (C-variadic) is the
// one exception: every call site wraps it in `unsafe`, matching every
// other printf-calling module in the crate.
// ===========================================================================

unsafe extern "C" {
    // proc module (kernel/proc/thread.rs, kernel/proc/sched.rs).
    safe fn xv6_current_thread() -> *mut thread;
    safe fn scheduler_yield();
    safe fn xv6_panic(msg: *const c_char) -> !;

    // printf.rs — C-variadic.

    // lock/mutex.rs — `inode->mutex`.
    safe fn mutex_lock(m: *mut mutex_t);
    safe fn mutex_unlock(m: *mut mutex_t);
    safe fn mutex_trylock(m: *mut mutex_t) -> c_int;
    safe fn mutex_init(m: *mut mutex_t, name: *mut c_char);
    safe fn holding_mutex(m: *mut mutex_t) -> c_int;

    // mm/kalloc.rs
    safe fn kmm_alloc(size: usize) -> *mut c_void;
    safe fn kmm_free(ptr: *mut c_void);

    // string.rs
    safe fn strndup(s: *const c_char, n: usize) -> *mut c_char;
    safe fn strtok_r(s: *mut c_char, delim: *const c_char, saveptr: *mut *mut c_char) -> *mut c_char;
    safe fn strlen(s: *const c_char) -> usize;
}

// P3-1C mesh sweep: vfs/fs.rs is in scope for this wave; these were its
// superblock lock/refcount + inode-cache/dentry/orphan helpers,
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures, same `crate::bindings::*` types this file
// already imports). `vfs_root_inode` is fs.rs's single dummy VFS-root
// `vfs_inode` instance (no superblock; see `vfs_private.h`) -- only its
// address is ever used here (pointer identity checks), never its
// contents.
use crate::vfs::fs::{
    __vfs_final_unmount_cleanup, vfs_get_dentry_inode, vfs_inode_deref, vfs_inode_get_ref,
    vfs_inode_put_ref, vfs_make_orphan, vfs_release_dentry, vfs_remove_inode, vfs_root_inode,
    vfs_superblock_rlock, vfs_superblock_unlock, vfs_superblock_wholding, vfs_superblock_wlock,
};

// `kassert!`'s canonical home is crate root / `crate::kstd` (P3-CS2
// centralization).
use crate::kassert;

// ===========================================================================
// Small helpers: negative-errno constants, ERR_PTR family, mode bits,
// list/hlist "static inline" reimplementations, refcount atomics.
// ===========================================================================

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// `err_ptr`/`is_err`/`is_err_or_null`/`ptr_err`'s canonical home is now
// `crate::kstd` (P3-CS2 centralization; this file's old doc comment here
// used to explain why it kept its own copy "rather than reusing
// `kernel/proc/access.rs`'s equivalents" — see `kstd.rs`'s module doc for
// the full history). Note `kstd::ptr_err` returns `c_int`, not `isize` —
// every call site below already casts its result to `c_int` or compares
// it against another `c_int` (`neg(...)`), so the narrower return type is
// a no-op change (see `is_eagain_ptr`, whose `as isize` comparison cast
// is dropped accordingly).
use crate::kstd::{
    err_ptr, errptr_to_result, is_err, is_err_or_null, ptr_err, result_to_errptr,
    result_to_neg_errno, Errno, KResult,
};

#[inline(always)]
fn is_eagain_ptr<T>(p: *mut T) -> bool {
    ptr_err(p) == neg(EAGAIN)
}

// `uabi/stat.h`'s `S_IF*`/`S_IS*` macros.
const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;
#[inline(always)]
fn is_dir(mode: u32) -> bool {
    mode & S_IFMT == S_IFDIR
}
#[inline(always)]
fn is_reg(mode: u32) -> bool {
    mode & S_IFMT == S_IFREG
}
#[inline(always)]
fn is_lnk(mode: u32) -> bool {
    mode & S_IFMT == S_IFLNK
}

const VFS_DITER_INDEX_END: i64 = -1;
const VFS_DITER_INDEX_START: i64 = 0;
const VFS_DITER_INDEX_CURRENT: i64 = 1;
const VFS_DITER_INDEX_PARENT: i64 = 2;
const VFS_PATH_MAX: usize = 65535;
const VFS_INODE_MAX_REFCOUNT: i32 = 0x7FFF0000;
const VFS_NAMEI_MAX_RETRIES: i32 = 10;

#[inline(always)]
fn root_inode_ptr() -> *mut vfs_inode {
    // SAFETY: taking the address of an extern static is always sound;
    // this never reads through the pointer.
    unsafe { ptr::addr_of_mut!(vfs_root_inode) }
}

// ---------------------------------------------------------------------------
// `list.h`/`hlist.h` `static inline` primitives (no external linkage —
// reimplemented natively here, same precedent as `kernel/kobject.rs`).
// ---------------------------------------------------------------------------

/// Mirrors `list_entry_init()`.
///
/// # Safety
/// `e` must point to a live `list_node_t`.
#[inline(always)]
unsafe fn ln_init(e: *mut list_node_t) {
    unsafe {
        (*e).next = e;
        (*e).prev = e;
    }
}

/// Mirrors `list_entry_detach()`.
///
/// # Safety
/// `e` must point to a live, linked `list_node_t`.
#[inline(always)]
unsafe fn ln_detach(e: *mut list_node_t) {
    unsafe {
        let prev = (*e).prev;
        let next = (*e).next;
        (*prev).next = next;
        (*next).prev = prev;
        ln_init(e);
    }
}

/// Mirrors `hlist_entry_init()`.
///
/// # Safety
/// `e` must point to a live `hlist_entry_t`.
#[inline(always)]
unsafe fn hli_init(e: *mut hlist_entry_t) {
    unsafe {
        (*e).bucket = ptr::null_mut();
        ln_init(ptr::addr_of_mut!((*e).list_entry));
    }
}

// ---------------------------------------------------------------------------
// `ref_count` atomics — mirrors `smp/atomic.h`'s `atomic_dec_unless`/
// `atomic_inc_unless`/`atomic_inc_in_range`/`atomic_dec` macros exactly
// (see the module doc's "Refcount map" section for the ordering
// rationale).
// ---------------------------------------------------------------------------

/// # Safety
/// `inode` must point to a live, aligned `vfs_inode`.
#[inline(always)]
fn refcount_atomic<'a>(inode: *mut vfs_inode) -> &'a AtomicI32 {
    // SAFETY: `ref_count` is a plain C `int` field, same size/align as
    // `AtomicI32`; caller ensures `inode` is a live, aligned `vfs_inode`.
    unsafe { &*(ptr::addr_of_mut!((*inode).ref_count) as *const AtomicI32) }
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
#[inline(always)]
fn atomic_inc_unless(a: &AtomicI32, unless: i32) -> bool {
    atomic_oper_cond(a, |v| v + 1, move |v| v != unless)
}
#[inline(always)]
fn atomic_inc_in_range(a: &AtomicI32, min: i32, max: i32) -> bool {
    atomic_oper_cond(a, |v| v + 1, move |v| v > min && v < max)
}

/// Mirrors `fs.h`'s `static inline vfs_inode_refcount()` (`__ATOMIC_SEQ_CST`
/// load). Used only for the `vfs_unlink` `EBUSY` check, matching the one
/// call site in the C original that goes through the atomic helper rather
/// than a plain field read (see `vfs_unlink`'s orphan-marking check for
/// the contrasting plain read, preserved 1:1 from the C).
fn vfs_inode_refcount_locked(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return -1;
    }
    refcount_atomic(inode).load(Ordering::SeqCst)
}

fn fs_lock(fs: *mut fs_struct) -> crate::sync::KSpinGuard {
    // SAFETY: `fs` is a live `fs_struct` (caller-supplied, matching every
    // call site's precondition below); `lock` is its first field.
    crate::sync::KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*fs).lock) }).lock()
}

#[inline(always)]
fn current_fs() -> *mut fs_struct {
    // SAFETY: `xv6_current_thread()` returns the live current thread in
    // every context this file is called from (matches the C `current`
    // macro's own assumption).
    unsafe { (*xv6_current_thread()).fs }
}

#[inline(always)]
fn current_proc_rooti() -> *mut vfs_inode {
    let fs = current_fs();
    // SAFETY: `fs` is live (see `current_fs`); `rooti` is a plain
    // embedded `vfs_inode_ref` field.
    unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).rooti)) }
}

/// Mirrors `fs.h`'s `static inline vfs_inode_is_local_root()`.
fn vfs_inode_is_local_root(inode: *mut vfs_inode) -> bool {
    if inode.is_null() {
        return false;
    }
    // SAFETY: non-null `inode`; only reads `sb`/`root_inode`/`parent`.
    unsafe {
        let sb = (*inode).sb;
        if sb.is_null() {
            return false;
        }
        if core::ptr::eq(inode, (*sb).root_inode) {
            kassert!(
                core::ptr::eq((*inode).parent, inode),
                "vfs_inode_is_local_root: root inode's parent is not itself"
            );
            return true;
        }
    }
    false
}

/// Mirrors `vfs_private.h`'s `static inline __vfs_inode_valid()`.
fn vfs_inode_valid_locked(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: non-null `inode`; only reads the mutex-held flag and the
    // valid/superblock-valid bitfields.
    unsafe {
        if holding_mutex(ptr::addr_of_mut!((*inode).mutex)) == 0 {
            return neg(EPERM);
        }
        if (*inode).__bindgen_anon_1.valid() == 0 {
            return neg(EINVAL);
        }
        if !core::ptr::eq(inode, root_inode_ptr()) {
            let sb = (*inode).sb;
            if sb.is_null() || (*sb).__bindgen_anon_2.valid() == 0 {
                crate::kprintln!("__vfs_inode_valid: inode's superblock is not valid");
                return neg(EINVAL);
            }
        }
    }
    0
}

// ===========================================================================
// Inode Private APIs
// ===========================================================================

/// Initialize VFS-managed inode fields. Called by `fs.c` (root inode init,
/// `vfs_alloc_inode`/`vfs_get_inode`) before adding a freshly allocated
/// inode to the superblock's inode hash list.
pub(crate) extern "C" fn __vfs_inode_init(inode: *mut vfs_inode) {
    // SAFETY: caller supplies a freshly allocated, otherwise-untouched
    // `vfs_inode` (matches the C original's documented precondition).
    unsafe {
        mutex_init(
            ptr::addr_of_mut!((*inode).mutex),
            c"vfs_inode_mutex".as_ptr() as *mut c_char,
        );
        hli_init(ptr::addr_of_mut!((*inode).hash_entry));
        ln_init(ptr::addr_of_mut!((*inode).orphan_entry));
        (*inode).__bindgen_anon_1.set_orphan(0);
        (*inode).ref_count = 1;
    }
}

// ===========================================================================
// Inode Public APIs
// ===========================================================================

pub(crate) extern "C" fn vfs_ilock(inode: *mut vfs_inode) {
    kassert!(!inode.is_null(), "vfs_ilock: inode is NULL");
    // SAFETY: non-null `inode`.
    mutex_lock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
}

pub(crate) extern "C" fn vfs_ilock_trylock(inode: *mut vfs_inode) -> c_int {
    kassert!(!inode.is_null(), "vfs_ilock_trylock: inode is NULL");
    // SAFETY: non-null `inode`.
    mutex_trylock(unsafe { ptr::addr_of_mut!((*inode).mutex) })
}

pub(crate) extern "C" fn vfs_iunlock(inode: *mut vfs_inode) {
    kassert!(!inode.is_null(), "vfs_iunlock: inode is NULL");
    // SAFETY: non-null `inode`.
    mutex_unlock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
}

/// Check if an inode is valid for use. Must be called while holding the
/// inode lock.
pub(crate) extern "C" fn vfs_inode_check_valid(inode: *mut vfs_inode) -> c_int {
    vfs_inode_valid_locked(inode)
}

/// Increment inode reference count. Atomic-only: no locks, no sleeping,
/// no allocation (may be called while holding the inode lock).
pub(crate) extern "C" fn vfs_idup(inode: *mut vfs_inode) {
    kassert!(!inode.is_null(), "vfs_idup: inode is NULL");
    // SAFETY: non-null `inode`; only reads `sb` for the assert.
    let sb_null = unsafe { (*inode).sb.is_null() };
    kassert!(!sb_null, "vfs_idup: inode's superblock is NULL");
    let success = atomic_inc_unless(refcount_atomic(inode), VFS_INODE_MAX_REFCOUNT);
    kassert!(success, "vfs_idup: inode refcount overflow");
}

/// Try to increment inode reference count if not zero (and not at max).
/// Use when obtaining a reference from a cache/lookup where the inode
/// might be dying.
pub(crate) extern "C" fn vfs_idup_not_zero(inode: *mut vfs_inode) -> bool {
    kassert!(!inode.is_null(), "vfs_idup_not_zero: inode is NULL");
    atomic_inc_in_range(refcount_atomic(inode), 0, VFS_INODE_MAX_REFCOUNT)
}

/// Decrease inode ref count; free the inode when the last reference is
/// dropped. Caller must not hold the inode lock or the superblock write
/// lock when calling.
pub(crate) extern "C" fn vfs_iput(inode_in: *mut vfs_inode) {
    kassert!(!inode_in.is_null(), "vfs_iput: inode is NULL");
    // SAFETY: non-null `inode_in`; preconditions mirror the C asserts.
    unsafe {
        let sb0 = (*inode_in).sb;
        let holds_wlock = !sb0.is_null() && vfs_superblock_wholding(sb0);
        kassert!(
            !holds_wlock,
            "vfs_iput: cannot hold superblock write lock when calling"
        );
        let holds_ilock = holding_mutex(ptr::addr_of_mut!((*inode_in).mutex)) != 0;
        kassert!(!holds_ilock, "vfs_iput: cannot hold inode lock when calling");
    }

    let mut inode = inode_in;
    // SAFETY: non-null `inode`.
    let mut sb = unsafe { (*inode).sb };
    let mut failed_clean = false;

    loop {
        // retry:
        if atomic_dec_unless(refcount_atomic(inode), 1) {
            return;
        }

        if sb.is_null() {
            vfs_iput_finalize(inode, None);
            return;
        }

        vfs_superblock_wlock(sb);
        // SAFETY: non-null `inode`.
        if mutex_trylock(unsafe { ptr::addr_of_mut!((*inode).mutex) }) == 0 {
            vfs_superblock_unlock(sb);
            scheduler_yield();
            continue;
        }

        if atomic_dec_unless(refcount_atomic(inode), 1) {
            // SAFETY: non-null `inode`.
            mutex_unlock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
            vfs_superblock_unlock(sb);
            return;
        }

        // SAFETY: non-null `sb`/`inode`; bitfield/plain-field reads only.
        let (attached, backendless) = unsafe {
            (
                (*sb).__bindgen_anon_2.attached() != 0,
                (*sb).__bindgen_anon_2.backendless() != 0,
            )
        };
        let n_links = unsafe { (*inode).n_links };
        let is_mount = unsafe { (*inode).__bindgen_anon_1.mount() != 0 };
        let is_root_inode = unsafe { core::ptr::eq(inode, (*sb).root_inode) };

        if attached && (n_links > 0 || is_mount) && (backendless || is_root_inode || is_mount) {
            let old = refcount_atomic(inode).fetch_sub(1, Ordering::SeqCst);
            kassert!(old - 1 >= 0, "vfs_iput: inode refcount underflow");
            // SAFETY: non-null `inode`.
            mutex_unlock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
            vfs_superblock_unlock(sb);
            return;
        }

        kassert!(!is_mount, "vfs_iput: refcount of mountpoint inode reached zero");

        // Orphan cleanup: remove from the in-memory orphan list and, for
        // backend filesystems, the on-disk orphan journal.
        let is_orphan = unsafe { (*inode).__bindgen_anon_1.orphan() != 0 };
        if is_orphan {
            // SAFETY: `inode` is on `sb->orphan_list` (orphan flag set);
            // `orphan_entry` is a live linked `list_node_t`.
            unsafe {
                ln_detach(ptr::addr_of_mut!((*inode).orphan_entry));
                (*sb).orphan_count -= 1;
                (*inode).__bindgen_anon_1.set_orphan(0);
                if let Some(remove_orphan) = (*(*sb).ops).remove_orphan {
                    let ret = remove_orphan(sb, inode);
                    if ret != 0 {
                        crate::kprintln!(
                            "vfs_iput: warning: failed to remove orphan inode {} from journal",
                            (*inode).ino as u64,
                        );
                    }
                }
            }
        }

        // If dirty and no cleanup has failed yet, try to sync before
        // freeing; retry from the top afterwards (someone else may have
        // grabbed a reference while locks were dropped for the sync).
        let dirty = unsafe { (*inode).__bindgen_anon_1.dirty() != 0 };
        let valid = unsafe { (*inode).__bindgen_anon_1.valid() != 0 };
        if dirty && valid && !failed_clean && attached {
            // SAFETY: non-null `inode`.
            mutex_unlock(unsafe { ptr::addr_of_mut!((*inode).mutex) });
            vfs_superblock_unlock(sb);
            failed_clean = vfs_sync_inode(inode) != 0;
            continue;
        }

        let mode = unsafe { (*inode).mode };
        let parent_field = unsafe { (*inode).parent };
        let mut parent: *mut vfs_inode = ptr::null_mut();
        if is_dir(mode) && !parent_field.is_null() && !core::ptr::eq(parent_field, inode) && attached
        {
            parent = parent_field;
        }

        // If no links remain (or the fs is detached), destroy the
        // on-disk data before freeing the in-memory inode.
        let n_links2 = unsafe { (*inode).n_links };
        if n_links2 == 0 || !attached {
            let destroy_inode = unsafe { (*(*inode).ops).destroy_inode };
            if let Some(destroy_fn) = destroy_inode {
                // Mark the inode as being destroyed so lookups don't try
                // to use it while destroy_inode is in progress; the
                // inode stays in the cache meanwhile.
                unsafe { (*inode).__bindgen_anon_1.set_destroying(1) };

                // Release locks before acquiring the transaction:
                // destroy_inode may do begin/end_op cycles internally
                // for batching and cannot run with these locks held.
                unsafe { mutex_unlock(ptr::addr_of_mut!((*inode).mutex)) };
                vfs_superblock_unlock(sb);

                let mut skip_destroy = false;
                if let Some(begin_fn) = unsafe { (*(*sb).ops).begin_transaction } {
                    let tx_ret = unsafe { begin_fn(sb) };
                    if tx_ret != 0 {
                        // Transaction failed — on-disk data remains,
                        // will be cleaned on next mount via orphan
                        // recovery. Still need to remove from cache.
                        unsafe { (*inode).__bindgen_anon_1.set_destroying(0) };
                        vfs_superblock_wlock(sb);
                        unsafe { mutex_lock(ptr::addr_of_mut!((*inode).mutex)) };
                        skip_destroy = true;
                    }
                }

                if !skip_destroy {
                    unsafe { destroy_fn(inode) };

                    if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
                        let end_ret = unsafe { end_fn(sb) };
                        if end_ret != 0 {
                            crate::kprintln!(
                                "vfs_iput: warning: end_transaction failed with error {}",
                                end_ret,
                            );
                        }
                    }

                    vfs_superblock_wlock(sb);
                    unsafe {
                        mutex_lock(ptr::addr_of_mut!((*inode).mutex));
                        // After destroy, on-disk data is freed: mark
                        // invalid/not-dirty so we never try to sync it.
                        (*inode).__bindgen_anon_1.set_valid(0);
                        (*inode).__bindgen_anon_1.set_dirty(0);
                        (*inode).__bindgen_anon_1.set_destroying(0);
                    }
                }
            }
        }

        // skip_destroy:
        // SAFETY: non-null `inode`/`sb`, both locked here.
        let ret = unsafe { vfs_remove_inode((*inode).sb, inode) };
        kassert!(
            ret == 0,
            "vfs_iput: failed to remove inode from superblock inode cache"
        );

        let should_free_sb = !attached && unsafe { (*sb).orphan_count } == 0;

        unsafe { mutex_unlock(ptr::addr_of_mut!((*inode).mutex)) };
        vfs_superblock_unlock(sb);

        // out:
        vfs_iput_finalize(inode, if should_free_sb { Some(sb) } else { None });

        // If this was a directory inode, decrease the parent's refcount
        // too. Avoid recursion (limited kernel stack): loop back to
        // `retry:` operating on the parent instead.
        if !parent.is_null() {
            inode = parent;
            sb = unsafe { (*inode).sb };
            failed_clean = false;
            continue;
        }
        return;
    }
}

/// Shared tail of every `vfs_iput` exit path: free the directory name (if
/// any) and hand the inode to its driver's `free_inode`, then run the
/// final detached-superblock cleanup if this was the last orphan.
fn vfs_iput_finalize(inode: *mut vfs_inode, sb_to_free: Option<*mut vfs_superblock>) {
    // SAFETY: non-null `inode`, no longer locked (matches every call
    // site, which unlocks before reaching here).
    unsafe {
        if !(*inode).name.is_null() {
            kmm_free((*inode).name as *mut c_void);
            (*inode).name = ptr::null_mut();
        }
        let free_inode = (*(*inode).ops)
            .free_inode
            .expect("vfs_iput: ops->free_inode is required");
        free_inode(inode);
    }
    if let Some(sb) = sb_to_free {
        __vfs_final_unmount_cleanup(sb);
    }
}

// ---------------------------------------------------------------------------
// `IRef` -- an RAII smart pointer over `vfs_inode.ref_count` (Phase 3
// P3-9d). Inode's refcount is a bespoke `AtomicI32` field manipulated by
// this file's own `vfs_idup`/`vfs_idup_not_zero`/`vfs_iput` (see the
// module doc's "Refcount map"), not a `struct kobject` -- it cannot use
// `KArc<T>` (`kernel/kobject.rs`). `IRef` deliberately mirrors `KArc<T>`'s
// shape (`Clone`/`Drop`/`Deref`/`from_raw`/`into_raw`/`as_ptr`) but is
// built directly on top of the get/put pair above rather than
// re-deriving the atomics -- it inherits their exact, already-proven
// ordering (`Ordering::SeqCst` CAS / `Ordering::Acquire` initial load,
// matching `smp/atomic.h`'s `atomic_oper_cond` verbatim) instead of a
// second, independently-reasoned-about copy. `vfs_iput`'s retry/destroy
// internals are untouched -- `IRef` only wraps the get/put pair at call
// sites, exactly as the KArc precedent wraps `kobject_get`/`kobject_put`
// rather than reimplementing them.
//
// # Invariant
// A live `IRef` owns exactly one held reference on the pointee
// `vfs_inode` (one increment of `ref_count` not yet matched by a
// `vfs_iput`). `Clone` (-> `vfs_idup`) and `Drop` (-> `vfs_iput`) are the
// only ways that invariant is created/destroyed internally;
// [`IRef::from_raw`]/[`IRef::try_from_raw`] accept one from the caller
// (the success postcondition of `vfs_namei`/`vfs_ilookup` +
// `vfs_get_dentry_inode`/`vfs_idup`-family constructors) and
// [`IRef::into_raw`] hands one back out -- the same `from_raw`/`into_raw`
// contract `Box`/`Arc`/`KArc` use, for the C-ABI boundary where a caller
// still juggles a raw `*mut vfs_inode` and manual `vfs_iput`.
// ---------------------------------------------------------------------------

/// See the module section doc above.
pub(crate) struct IRef {
    ptr: NonNull<vfs_inode>,
}

impl IRef {
    /// Take ownership of an existing, already-held inode reference.
    ///
    /// # Safety
    /// `ptr` must be non-null and point to a live `vfs_inode` whose
    /// `ref_count` currently carries a reference the caller is
    /// transferring to the returned `IRef` -- the caller must not also
    /// call `vfs_iput` (or drop another `IRef` over the same reference)
    /// on that same held count. This is exactly the success
    /// postcondition of `vfs_namei`/`vfs_nameiparent`/`vfs_ilookup` +
    /// `vfs_get_dentry_inode`/`vfs_idup`.
    pub(crate) unsafe fn from_raw(ptr: *mut vfs_inode) -> Self {
        debug_assert!(!ptr.is_null(), "IRef::from_raw: ptr is NULL");
        // SAFETY: caller guarantees `ptr` is non-null (also debug-checked
        // above) and owns an already-held reference (function contract).
        IRef { ptr: unsafe { NonNull::new_unchecked(ptr) } }
    }

    /// `vfs_idup_not_zero` analog: attempt to upgrade a bare,
    /// liveness-unproven `*mut vfs_inode` (e.g. one just read out of a
    /// cache/hash bucket under a lock, with no existing reference of its
    /// own) into an owned `IRef`. Returns `None` if the refcount had
    /// already reached zero (a concurrent final `Drop`/`vfs_iput` raced
    /// ahead of this call) or is already at `VFS_INODE_MAX_REFCOUNT`.
    ///
    /// # Safety
    /// `ptr` must be non-null and point to a `vfs_inode` that is still
    /// valid to dereference for the duration of this call -- the same
    /// precondition a raw call to `vfs_idup_not_zero` has.
    pub(crate) unsafe fn try_from_raw(ptr: *mut vfs_inode) -> Option<Self> {
        debug_assert!(!ptr.is_null(), "IRef::try_from_raw: ptr is NULL");
        if vfs_idup_not_zero(ptr) {
            // SAFETY: `ptr` was just proven non-null-derived and live by
            // the successful `vfs_idup_not_zero` above, which also
            // established the held reference this `IRef` now owns.
            Some(IRef { ptr: unsafe { NonNull::new_unchecked(ptr) } })
        } else {
            None
        }
    }

    /// Give up the owned reference without decrementing the refcount,
    /// returning the raw pointer -- the flip side of [`IRef::from_raw`],
    /// for a C-ABI boundary that expects to receive one held reference as
    /// a bare pointer (e.g. storing into `fs_struct.cwd`/`.rooti` via
    /// `vfs_inode_get_ref`, or a still-C-shaped vtable slot/out-param).
    pub(crate) fn into_raw(this: Self) -> *mut vfs_inode {
        let ptr = this.ptr.as_ptr();
        core::mem::forget(this);
        ptr
    }

    /// The raw pointer this `IRef` owns a reference on, without giving up
    /// ownership (contrast [`IRef::into_raw`]) -- for passing to the many
    /// still-C-ABI-shaped helpers in this module that borrow a
    /// `*mut vfs_inode` without taking or dropping a reference of their
    /// own (`vfs_ilock`, vtable dispatch functions, `vfs_readlink`, ...).
    pub(crate) fn as_ptr(this: &Self) -> *mut vfs_inode {
        this.ptr.as_ptr()
    }
}

impl Clone for IRef {
    fn clone(&self) -> Self {
        // Struct invariant: `self` owns a currently-held reference, so
        // `vfs_idup`'s precondition (an already-held reference to
        // increment from) holds; matches `KArc::clone`'s use of
        // `kobject_get` under the same reasoning.
        vfs_idup(self.ptr.as_ptr());
        IRef { ptr: self.ptr }
    }
}

impl Drop for IRef {
    fn drop(&mut self) {
        // Struct invariant: `self` owns exactly one held reference, which
        // this call gives up; `vfs_iput` runs its own retry/destroy path
        // if this was the last one (untouched by this wrapper -- see the
        // module section doc above). Every current `IRef` call site
        // upholds `vfs_iput`'s precondition (neither the inode lock nor
        // the superblock write lock held here): none call `into_raw`
        // while holding either, and none hold an `IRef` across a
        // `vfs_ilock`/`vfs_superblock_wlock` acquisition.
        vfs_iput(self.ptr.as_ptr());
    }
}

impl Deref for IRef {
    type Target = vfs_inode;
    fn deref(&self) -> &vfs_inode {
        // SAFETY: struct invariant -- a live `IRef` holds a reference,
        // which keeps the pointee allocated for as long as any `IRef`
        // over it exists (mirrors `KArc<T>::deref`'s reasoning). Shared
        // access only: mutation of individual `vfs_inode` fields
        // elsewhere in this crate is synchronized per-field by
        // `vfs_ilock`/atomics, not by unique ownership of the whole
        // struct -- the same discipline every existing `*mut vfs_inode`
        // call site already relies on.
        unsafe { self.ptr.as_ref() }
    }
}

/// Mark inode as dirty.
pub(crate) extern "C" fn vfs_dirty_inode(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() || unsafe { (*inode).sb.is_null() } {
        return neg(EINVAL);
    }
    let ret = vfs_inode_valid_locked(inode);
    if ret != 0 {
        return ret;
    }
    // SAFETY: non-null `inode`.
    match unsafe { (*(*inode).ops).dirty_inode } {
        Some(f) => unsafe { f(inode) },
        None => ret,
    }
}

/// Sync inode to disk.
pub(crate) extern "C" fn vfs_sync_inode(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() || unsafe { (*inode).sb.is_null() } {
        return neg(EINVAL);
    }
    let sb = unsafe { (*inode).sb };
    let mut ret: c_int = 0;

    // Begin transaction BEFORE acquiring the inode lock, to avoid
    // sleeping with locks held.
    if let Some(begin_fn) = unsafe { (*(*sb).ops).begin_transaction } {
        ret = unsafe { begin_fn(sb) };
        if ret != 0 {
            return ret;
        }
    }

    vfs_ilock(inode);
    ret = vfs_inode_valid_locked(inode);
    if ret != 0 {
        vfs_iunlock(inode);
        if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
            unsafe { end_fn(sb) };
        }
        return ret;
    }

    if let Some(f) = unsafe { (*(*inode).ops).sync_inode } {
        ret = unsafe { f(inode) };
    }
    vfs_iunlock(inode);

    if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
        let end_ret = unsafe { end_fn(sb) };
        if end_ret != 0 {
            crate::kprintln!(
                "vfs_sync_inode: warning: end_transaction failed with error {}",
                end_ret,
            );
        }
    }
    ret
}

/// Get the outermost mount layer. Caller must hold a reference to
/// `rooti` or one of its descendants.
fn get_mnt_recursive(rooti: *mut vfs_inode) -> *mut vfs_inode {
    let mut inode = rooti;
    // SAFETY: `rooti` is a live, referenced inode (caller precondition).
    let mut sb = unsafe { (*rooti).sb };
    let proc_rooti = current_proc_rooti();
    loop {
        if core::ptr::eq(inode, proc_rooti) {
            return inode;
        }
        if core::ptr::eq(inode, root_inode_ptr()) {
            return inode;
        }
        kassert!(!sb.is_null(), "__get_mnt_recursive: inode's superblock mismatch");
        // SAFETY: non-null `sb`.
        let sb_root = unsafe { (*sb).root_inode };
        if !core::ptr::eq(inode, sb_root) {
            kassert!(!sb_root.is_null(), "__get_mnt_recursive: superblock root inode is NULL");
            return inode;
        }
        // SAFETY: non-null `sb`.
        inode = unsafe { (*sb).mountpoint };
        kassert!(!inode.is_null(), "__get_mnt_recursive: mountpoint inode is NULL");
        // SAFETY: non-null `inode`.
        sb = unsafe { (*inode).sb };
    }
}

/// Get the parent inode of the mountpoint recursively. Caller must hold
/// a reference to `dir` or one of its descendants.
fn mountpoint_go_up(dir: *mut vfs_inode) -> *mut vfs_inode {
    let mut inode = dir;
    let proc_rooti = current_proc_rooti();
    loop {
        if core::ptr::eq(inode, proc_rooti) {
            return inode;
        }
        if core::ptr::eq(inode, root_inode_ptr()) {
            return inode;
        }
        // SAFETY: non-null `inode`.
        let parent = unsafe { (*inode).parent };
        if !core::ptr::eq(parent, inode) {
            kassert!(!parent.is_null(), "__mountpoint_go_up: inode's parent is NULL");
            return parent;
        }
        inode = get_mnt_recursive(inode);
    }
}

/// Resolve ".." for a directory inode. Returns `Some(target)` for the
/// synthesized-".." fast paths (process root, local fs root); `None`
/// means "fall through to driver lookup for a normal ..".
fn vfs_dotdot_target(dir: *mut vfs_inode) -> *mut vfs_inode {
    let proc_rooti = current_proc_rooti();
    if core::ptr::eq(dir, proc_rooti) {
        return dir;
    }
    if vfs_inode_is_local_root(dir) {
        let parent = mountpoint_go_up(dir);
        if core::ptr::eq(parent, root_inode_ptr()) {
            return dir;
        }
        return parent;
    }
    ptr::null_mut()
}

fn make_iter_present(iter: *mut vfs_dir_iter, ret_dentry: *mut vfs_dentry) -> c_int {
    let n = strndup(c".".as_ptr(), 1);
    if n.is_null() {
        return neg(ENOMEM);
    }
    // SAFETY: non-null `iter`/`ret_dentry` (caller precondition).
    unsafe {
        (*ret_dentry).name = n;
        (*ret_dentry).name_len = 1;
        (*ret_dentry).cookies = 0;
        (*iter).cookies = 0;
        (*iter).index = VFS_DITER_INDEX_CURRENT;
    }
    0
}

fn make_iter_parent(iter: *mut vfs_dir_iter, ret_dentry: *mut vfs_dentry) -> c_int {
    vfs_release_dentry(ret_dentry); // release "."
    let n = strndup(c"..".as_ptr(), 2);
    if n.is_null() {
        return neg(ENOMEM);
    }
    // SAFETY: non-null `iter`/`ret_dentry` (caller precondition).
    unsafe {
        (*ret_dentry).name = n;
        (*ret_dentry).name_len = 2;
        (*ret_dentry).cookies = 0;
        (*iter).cookies = 0;
        (*iter).index = VFS_DITER_INDEX_PARENT;
    }
    0
}

/// Lookup a dentry in a directory inode. Assumes the VFS core already
/// handled ".".
fn vfs_ilookup_inner(
    dir: *mut vfs_inode,
    dentry: *mut vfs_dentry,
    name: *const c_char,
    name_len: usize,
) -> KResult<()> {
    if dir.is_null() || unsafe { (*dir).sb.is_null() } {
        return Err(Errno::Inval);
    }
    if dentry.is_null() || name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }
    // SAFETY: `name`/`name_len` describe a valid byte range (caller
    // precondition — the VFS convention for all name/name_len pairs).
    let name_bytes = unsafe { core::slice::from_raw_parts(name as *const u8, name_len) };

    if name_len == 1 && name_bytes[0] == b'.' {
        // SAFETY: non-null `dir`/`dentry`.
        unsafe {
            (*dentry).sb = (*dir).sb;
            (*dentry).ino = (*dir).ino;
            (*dentry).parent = dir;
        }
        let n = strndup(c".".as_ptr(), 1);
        if n.is_null() {
            return Err(Errno::NoMem);
        }
        unsafe {
            (*dentry).name = n;
            (*dentry).name_len = 1;
            (*dentry).cookies = 0;
        }
        return Ok(());
    }

    if name_len == 2 && name_bytes[0] == b'.' && name_bytes[1] == b'.' {
        let target = vfs_dotdot_target(dir);
        if !target.is_null() {
            let n = strndup(c"..".as_ptr(), 2);
            if n.is_null() {
                return Err(Errno::NoMem);
            }
            // SAFETY: non-null `target`/`dentry`.
            unsafe {
                (*dentry).sb = (*target).sb;
                (*dentry).ino = (*target).ino;
                (*dentry).parent = if core::ptr::eq(target, dir) { ptr::null_mut() } else { target };
                (*dentry).name = n;
                (*dentry).name_len = 2;
                (*dentry).cookies = 0;
            }
            return Ok(());
        }
        // fall through to driver lookup for normal ".."
    }

    // SAFETY: non-null `dir`.
    let sb = unsafe { (*dir).sb };
    vfs_superblock_rlock(sb);
    vfs_ilock(dir);

    let ret: c_int = 'out: {
        let v = vfs_inode_valid_locked(dir);
        if v != 0 {
            break 'out v;
        }
        if !is_dir(unsafe { (*dir).mode }) {
            break 'out neg(ENOTDIR);
        }
        match unsafe { (*(*dir).ops).lookup } {
            None => neg(ENOSYS),
            Some(f) => unsafe { f(dir, dentry, name, name_len) },
        }
    };

    vfs_iunlock(dir);
    vfs_superblock_unlock(sb);
    if ret == 0 { Ok(()) } else { Err(Errno::Raw(ret)) }
}

/// Lookup a dentry in a directory inode. Assumes the VFS core already
/// handled ".".
pub(crate) extern "C" fn vfs_ilookup(
    dir: *mut vfs_inode,
    dentry: *mut vfs_dentry,
    name: *const c_char,
    name_len: usize,
) -> c_int {
    result_to_neg_errno(vfs_ilookup_inner(dir, dentry, name, name_len))
}

/// Iterate over directory entries in a directory inode.
pub(crate) extern "C" fn vfs_dir_iter(
    dir: *mut vfs_inode,
    iter: *mut vfs_dir_iter,
    ret_dentry: *mut vfs_dentry,
) -> c_int {
    if dir.is_null() || unsafe { (*dir).sb.is_null() } {
        return neg(EINVAL);
    }
    if iter.is_null() || ret_dentry.is_null() {
        return neg(EINVAL);
    }

    // SAFETY: non-null `dir`.
    let sb = unsafe { (*dir).sb };
    vfs_superblock_rlock(sb);
    vfs_ilock(dir);

    let mut need_lookup = false;
    let ret: c_int = 'body: {
        let v = vfs_inode_valid_locked(dir);
        if v != 0 {
            break 'body v;
        }
        if !is_dir(unsafe { (*dir).mode }) {
            break 'body neg(ENOTDIR);
        }
        let dir_iter_fn = match unsafe { (*(*dir).ops).dir_iter } {
            None => break 'body neg(ENOSYS),
            Some(f) => f,
        };

        // SAFETY: non-null `iter`.
        let index = unsafe { (*iter).index };
        if index == VFS_DITER_INDEX_END {
            unsafe {
                (*ret_dentry).name = ptr::null_mut();
                (*ret_dentry).name_len = 0;
            }
            break 'body 0;
        }

        if index == VFS_DITER_INDEX_START {
            let r = make_iter_present(iter, ret_dentry);
            if r != 0 {
                break 'body r;
            }
            unsafe {
                (*ret_dentry).ino = (*dir).ino;
                (*ret_dentry).sb = (*dir).sb;
                (*ret_dentry).parent = ptr::null_mut();
            }
            break 'body 0;
        }

        if index == 1 {
            let proc_rooti = current_proc_rooti();
            if core::ptr::eq(dir, proc_rooti) {
                let r = make_iter_parent(iter, ret_dentry);
                if r != 0 {
                    break 'body r;
                }
                unsafe {
                    (*ret_dentry).ino = (*dir).ino;
                    (*ret_dentry).sb = (*dir).sb;
                    (*ret_dentry).parent = ptr::null_mut();
                }
                break 'body 0;
            } else if vfs_inode_is_local_root(dir) {
                let r = make_iter_parent(iter, ret_dentry);
                if r != 0 {
                    break 'body r;
                }
                unsafe { (*ret_dentry).parent = ptr::null_mut() };
                need_lookup = true;
                break 'body 0;
            }
            // Ordinary directory: fall through to the driver call
            // without modifying iter->index.
        }

        if unsafe { (*iter).index } >= VFS_DITER_INDEX_PARENT {
            unsafe {
                (*iter).index += 1;
                (*ret_dentry).sb = (*dir).sb;
                (*ret_dentry).parent = dir;
                (*ret_dentry).cookies = (*iter).cookies;
            }
        }
        let r = unsafe { dir_iter_fn(dir, iter, ret_dentry) };
        if r == 0 && unsafe { (*iter).index } == VFS_DITER_INDEX_CURRENT {
            unsafe { (*iter).index = VFS_DITER_INDEX_PARENT };
        }
        r
    };

    vfs_iunlock(dir);
    vfs_superblock_unlock(sb);

    if ret == 0 {
        if unsafe { (*iter).index } == VFS_DITER_INDEX_PARENT && need_lookup {
            // Synthesizing ".." for a mounted root: fill in the correct
            // parent inode now that locks are released.
            let target = vfs_dotdot_target(dir);
            unsafe {
                (*ret_dentry).ino = (*target).ino;
                (*ret_dentry).sb = (*target).sb;
                (*ret_dentry).parent = target;
            }
        }
        if unsafe { (*iter).index } > VFS_DITER_INDEX_PARENT {
            let name_present = unsafe { !(*ret_dentry).name.is_null() && (*ret_dentry).name_len > 0 };
            if name_present {
                if unsafe { (*ret_dentry).ino } == 0 {
                    return neg(EINVAL); // Driver did not fill ino
                }
                unsafe { (*iter).cookies = (*ret_dentry).cookies };
                return 0;
            }
            // Reached end of directory; reset the iterator.
            unsafe {
                (*iter).index = VFS_DITER_INDEX_END;
                (*iter).cookies = 0;
                (*ret_dentry).parent = ptr::null_mut();
                (*ret_dentry).cookies = 0;
                (*ret_dentry).ino = 0;
                (*ret_dentry).sb = ptr::null_mut();
            }
            return 0;
        }
    }

    ret
}

/// Check if a directory is empty (contains only "." and ".."). Caller
/// must hold the inode lock.
pub(crate) extern "C" fn vfs_dir_isempty(dir: *mut vfs_inode) -> c_int {
    if !is_dir(unsafe { (*dir).mode }) {
        return 0;
    }
    let dir_iter_fn = match unsafe { (*(*dir).ops).dir_iter } {
        None => return 0, // Can't check, assume not empty
        Some(f) => f,
    };

    let mut iter: vfs_dir_iter = unsafe { core::mem::zeroed() };
    let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
    iter.index = VFS_DITER_INDEX_PARENT; // Skip "." and ".."
    iter.cookies = 0;

    let ret = unsafe { dir_iter_fn(dir, &mut iter, &mut dentry) };
    if ret != 0 {
        vfs_release_dentry(&mut dentry);
        return 0; // Error, assume not empty
    }
    let is_empty = dentry.name.is_null();
    vfs_release_dentry(&mut dentry);
    is_empty as c_int
}

pub(crate) extern "C" fn vfs_readlink(inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> isize {
    if inode.is_null() || unsafe { (*inode).sb.is_null() } {
        return -(EINVAL as isize);
    }
    if buf.is_null() || buflen == 0 {
        return -(EINVAL as isize);
    }
    vfs_ilock(inode);
    let ret: isize = 'out: {
        let v = vfs_inode_valid_locked(inode);
        if v != 0 {
            break 'out v as isize;
        }
        if !is_lnk(unsafe { (*inode).mode }) {
            break 'out -(EINVAL as isize);
        }
        let f = match unsafe { (*(*inode).ops).readlink } {
            None => break 'out -(ENOSYS as isize),
            Some(f) => f,
        };
        let r = unsafe { f(inode, buf, buflen) };
        if r >= 0 && (r as usize) >= buflen {
            break 'out -(ENAMETOOLONG as isize);
        }
        r
    };
    vfs_iunlock(inode);
    ret
}

fn vfs_create_inner(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
) -> KResult<*mut vfs_inode> {
    if dir.is_null() || unsafe { (*dir).sb.is_null() } {
        return Err(Errno::Inval);
    }
    if name.is_null() || name_len == 0 {
        return Err(Errno::Inval);
    }

    loop {
        let sb = unsafe { (*dir).sb };
        if let Some(begin_fn) = unsafe { (*(*sb).ops).begin_transaction } {
            let r = unsafe { begin_fn(sb) };
            if r != 0 {
                return Err(Errno::Raw(r));
            }
        }

        vfs_superblock_wlock(sb);
        vfs_ilock(dir);

        // `ret_ptr` stays the transitional `ERR_PTR` encoding internally
        // (rather than `KResult`) because the EAGAIN retry check below
        // (`is_eagain_ptr`) and the driver `create` callback (a
        // cross-module C-ABI fn that itself speaks `ERR_PTR`) both need
        // the raw pointer form before this function's single boundary
        // conversion at the bottom of the loop body.
        let ret_ptr: *mut vfs_inode = {
            let v = vfs_inode_valid_locked(dir);
            if v != 0 {
                err_ptr(v)
            } else if !is_dir(unsafe { (*dir).mode }) {
                err_ptr(neg(ENOTDIR))
            } else {
                match unsafe { (*(*dir).ops).create } {
                    None => err_ptr(neg(ENOSYS)),
                    Some(f) => unsafe { f(dir, mode, name, name_len) },
                }
            }
        };

        // Handle EAGAIN: inode allocation collided with a destroying
        // inode. Release all locks and the transaction, yield, retry.
        if is_eagain_ptr(ret_ptr) {
            vfs_iunlock(dir);
            vfs_superblock_unlock(sb);
            if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
                unsafe { end_fn(sb) };
            }
            scheduler_yield();
            continue;
        }

        vfs_iunlock(dir);
        vfs_superblock_unlock(sb);

        if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
            let end_ret = unsafe { end_fn(sb) };
            if end_ret != 0 {
                crate::kprintln!(
                    "vfs_create: warning: end_transaction failed with error {}",
                    end_ret,
                );
            }
        }
        return if is_err(ret_ptr) { Err(Errno::Raw(ptr_err(ret_ptr))) } else { Ok(ret_ptr) };
    }
}

pub(crate) extern "C" fn vfs_create(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
) -> *mut vfs_inode {
    result_to_errptr(vfs_create_inner(dir, mode, name, name_len))
}

pub(crate) extern "C" fn vfs_mknod(
    dir: *mut vfs_inode,
    mode: mode_t,
    dev: dev_t,
    name: *const c_char,
    name_len: usize,
) -> *mut vfs_inode {
    if dir.is_null() || unsafe { (*dir).sb.is_null() } {
        return err_ptr(neg(EINVAL));
    }
    if name.is_null() || name_len == 0 {
        return err_ptr(neg(EINVAL));
    }

    loop {
        let sb = unsafe { (*dir).sb };
        if let Some(begin_fn) = unsafe { (*(*sb).ops).begin_transaction } {
            let r = unsafe { begin_fn(sb) };
            if r != 0 {
                return err_ptr(r);
            }
        }

        vfs_superblock_wlock(sb);
        vfs_ilock(dir);

        let ret_ptr: *mut vfs_inode = {
            let v = vfs_inode_valid_locked(dir);
            if v != 0 {
                err_ptr(v)
            } else if !is_dir(unsafe { (*dir).mode }) {
                err_ptr(neg(ENOTDIR))
            } else {
                match unsafe { (*(*dir).ops).mknod } {
                    None => err_ptr(neg(ENOSYS)),
                    Some(f) => unsafe { f(dir, mode, dev, name, name_len) },
                }
            }
        };

        if is_eagain_ptr(ret_ptr) {
            vfs_iunlock(dir);
            vfs_superblock_unlock(sb);
            if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
                unsafe { end_fn(sb) };
            }
            scheduler_yield();
            continue;
        }

        vfs_iunlock(dir);
        vfs_superblock_unlock(sb);

        if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
            let end_ret = unsafe { end_fn(sb) };
            if end_ret != 0 {
                crate::kprintln!(
                    "vfs_mknod: warning: end_transaction failed with error {}",
                    end_ret,
                );
            }
        }
        return ret_ptr;
    }
}

pub(crate) extern "C" fn vfs_link(
    old: *mut vfs_dentry,
    dir: *mut vfs_inode,
    name: *const c_char,
    name_len: usize,
) -> c_int {
    if dir.is_null() || unsafe { (*dir).sb.is_null() } {
        return neg(EINVAL);
    }
    if name.is_null() || name_len == 0 || old.is_null() {
        return neg(EINVAL);
    }

    let target = vfs_get_dentry_inode(old);
    if is_err(target) {
        return ptr_err(target) as c_int;
    }
    kassert!(!target.is_null(), "vfs_link: old dentry inode is NULL");
    // `IRef::from_raw`: `vfs_get_dentry_inode` succeeded (non-null,
    // non-error), whose postcondition is an owned, already-held
    // reference -- `target`'s `Drop` now replaces every manual
    // `vfs_iput(target)` below, including the two error-path calls this
    // wave deletes (P3-9d).
    // SAFETY: see the comment above.
    let target = unsafe { IRef::from_raw(target) };
    if target.sb != unsafe { (*dir).sb } {
        return neg(EXDEV); // Cross-device hard link not supported
    }

    let sb = unsafe { (*dir).sb };
    let mut ret: c_int = 0;
    if let Some(begin_fn) = unsafe { (*(*sb).ops).begin_transaction } {
        ret = unsafe { begin_fn(sb) };
        if ret != 0 {
            return ret;
        }
    }

    vfs_superblock_wlock(sb);
    'out_unlock_sb: {
        if is_dir(target.mode) {
            ret = neg(EPERM); // Cannot create hard link to a directory
            break 'out_unlock_sb;
        }
        if !is_dir(unsafe { (*dir).mode }) {
            ret = neg(ENOTDIR);
            break 'out_unlock_sb;
        }
        vfs_ilock_two_nondirectories(dir, IRef::as_ptr(&target));
        ret = 'out: {
            let v = vfs_inode_valid_locked(dir);
            if v != 0 {
                break 'out v;
            }
            let v = vfs_inode_valid_locked(IRef::as_ptr(&target));
            if v != 0 {
                break 'out v;
            }
            match unsafe { (*(*dir).ops).link } {
                None => neg(ENOSYS),
                Some(f) => unsafe { f(IRef::as_ptr(&target), dir, name, name_len) },
            }
        };
        vfs_iunlock_two(IRef::as_ptr(&target), dir);
    }
    vfs_superblock_unlock(sb);

    if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
        let end_ret = unsafe { end_fn(sb) };
        if end_ret != 0 {
            crate::kprintln!(
                "vfs_link: warning: end_transaction failed with error {}",
                end_ret,
            );
        }
    }

    // `target` (an `IRef`) drops here, calling `vfs_iput` exactly once --
    // matches the deleted final `vfs_iput(target)`; unlike the original,
    // every early `return` above also runs this same drop (no separate
    // manual call to remember).
    ret
}

pub(crate) extern "C" fn vfs_unlink(dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> c_int {
    if dir.is_null() || unsafe { (*dir).sb.is_null() } {
        return neg(EINVAL);
    }
    if name.is_null() || name_len == 0 {
        return neg(EINVAL);
    }

    // SAFETY: `name`/`name_len` describe a valid byte range.
    let name_bytes = unsafe { core::slice::from_raw_parts(name as *const u8, name_len) };
    if (name_len == 1 && name_bytes == b".") || (name_len == 2 && name_bytes == b"..") {
        return neg(EINVAL); // Cannot unlink "." or ".."
    }

    let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
    let lret = vfs_ilookup(dir, &mut dentry, name, name_len);
    if lret != 0 {
        return lret;
    }

    let target = vfs_get_dentry_inode(&mut dentry);
    if is_err(target) {
        let e = ptr_err(target) as c_int;
        vfs_release_dentry(&mut dentry);
        return e;
    }

    let sb = unsafe { (*dir).sb };
    let mut ret: c_int = 0;
    if let Some(begin_fn) = unsafe { (*(*sb).ops).begin_transaction } {
        ret = unsafe { begin_fn(sb) };
        if ret != 0 {
            vfs_iput(target); // Drop our reference from lookup
            vfs_release_dentry(&mut dentry);
            return ret;
        }
    }

    vfs_superblock_wlock(sb);
    vfs_ilock(dir);
    vfs_ilock(target);

    ret = 'out: {
        let v = vfs_inode_valid_locked(dir);
        if v != 0 {
            break 'out v;
        }
        let v = vfs_inode_valid_locked(target);
        if v != 0 {
            break 'out v;
        }
        if is_dir(unsafe { (*target).mode }) {
            let rmdir_fn = match unsafe { (*(*dir).ops).rmdir } {
                None => break 'out neg(ENOSYS),
                Some(f) => f,
            };
            if vfs_dir_isempty(target) == 0 {
                break 'out neg(ENOTEMPTY);
            }
            // Directory in use (refcount > 1 means someone else has it
            // open) — uses the atomic refcount helper, matching the C
            // original's one call site that does so (see the plain read
            // just below for the contrasting orphan-marking check).
            if vfs_inode_refcount_locked(target) > 1 {
                break 'out neg(EBUSY);
            }
            unsafe { rmdir_fn(&mut dentry, target) }
        } else {
            let unlink_fn = match unsafe { (*(*dir).ops).unlink } {
                None => break 'out neg(ENOSYS),
                Some(f) => f,
            };
            unsafe { unlink_fn(&mut dentry, target) }
        }
    };

    // If unlink succeeded and the inode still has references beyond
    // ours, mark it as orphan (checked while still holding the locks).
    // Preserved 1:1 from the C original: a genuine *plain* (non-atomic)
    // `ref_count` read here, unlike the atomic helper used for the
    // `EBUSY` check above — both locks are held at this point so no
    // concurrent atomic writer can race this specific read/decision.
    let n_links = unsafe { (*target).n_links };
    let refcount = unsafe { (*target).ref_count };
    let is_orphan = unsafe { (*target).__bindgen_anon_1.orphan() != 0 };
    if n_links == 0 && refcount > 1 && !is_orphan {
        vfs_make_orphan(target);
    }

    vfs_iunlock(target);
    vfs_iunlock(dir);
    vfs_superblock_unlock(sb);

    if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
        let end_ret = unsafe { end_fn(sb) };
        if end_ret != 0 {
            crate::kprintln!(
                "vfs_unlink: warning: end_transaction failed with error {}",
                end_ret,
            );
        }
    }
    vfs_iput(target); // Drop our reference from lookup
    vfs_release_dentry(&mut dentry);
    ret
}

pub(crate) extern "C" fn vfs_mkdir(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
) -> *mut vfs_inode {
    if dir.is_null() || unsafe { (*dir).sb.is_null() } {
        return err_ptr(neg(EINVAL));
    }
    if name.is_null() || name_len == 0 {
        return err_ptr(neg(EINVAL));
    }

    loop {
        let sb = unsafe { (*dir).sb };
        if let Some(begin_fn) = unsafe { (*(*sb).ops).begin_transaction } {
            let r = unsafe { begin_fn(sb) };
            if r != 0 {
                return err_ptr(r);
            }
        }

        vfs_superblock_wlock(sb);
        vfs_ilock(dir);

        let ret_ptr: *mut vfs_inode = {
            let v = vfs_inode_valid_locked(dir);
            if v != 0 {
                err_ptr(v)
            } else if !is_dir(unsafe { (*dir).mode }) {
                err_ptr(neg(ENOTDIR))
            } else {
                match unsafe { (*(*dir).ops).mkdir } {
                    None => err_ptr(neg(ENOSYS)),
                    Some(f) => unsafe { f(dir, mode, name, name_len) },
                }
            }
        };

        if is_eagain_ptr(ret_ptr) {
            vfs_iunlock(dir);
            vfs_superblock_unlock(sb);
            if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
                unsafe { end_fn(sb) };
            }
            scheduler_yield();
            continue;
        }

        if !is_err(ret_ptr) {
            vfs_ilock(ret_ptr);
            unsafe { (*ret_ptr).parent = dir };
            vfs_idup(dir); // increase parent dir refcount
            vfs_iunlock(ret_ptr);
        }

        vfs_iunlock(dir);
        vfs_superblock_unlock(sb);

        if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
            let end_ret = unsafe { end_fn(sb) };
            if end_ret != 0 {
                crate::kprintln!(
                    "vfs_mkdir: warning: end_transaction failed with error {}",
                    end_ret,
                );
            }
        }
        return ret_ptr;
    }
}

pub(crate) extern "C" fn vfs_move(
    old_dir: *mut vfs_inode,
    old_dentry: *mut vfs_dentry,
    new_dir: *mut vfs_inode,
    name: *const c_char,
    name_len: usize,
) -> c_int {
    if old_dir.is_null()
        || unsafe { (*old_dir).sb.is_null() }
        || new_dir.is_null()
        || unsafe { (*new_dir).sb.is_null() }
    {
        return neg(EINVAL);
    }
    if old_dentry.is_null() || name.is_null() || name_len == 0 {
        return neg(EINVAL);
    }
    let mut ret = vfs_inode_valid_locked(old_dir);
    if ret != 0 && ret != neg(EPERM) {
        return ret;
    }
    ret = vfs_inode_valid_locked(new_dir);
    if ret != 0 && ret != neg(EPERM) {
        return ret;
    }
    let sb = unsafe { (*old_dir).sb };
    if sb != unsafe { (*new_dir).sb } {
        return neg(EXDEV); // Cross-device move not supported
    }

    vfs_superblock_wlock(sb);
    let result: c_int = 'out: {
        if !is_dir(unsafe { (*old_dir).mode }) {
            break 'out neg(ENOTDIR);
        }
        if !is_dir(unsafe { (*new_dir).mode }) {
            break 'out neg(ENOTDIR);
        }
        // Return value intentionally discarded, matching the C original:
        // `old_dir`/`new_dir` are already proven same-filesystem above,
        // so `vfs_ilock_two_directories` cannot fail (`-EXDEV`) here.
        let _ = vfs_ilock_two_directories(old_dir, new_dir);

        let old_move = unsafe { (*(*old_dir).ops).move_ };
        let new_move = unsafe { (*(*new_dir).ops).move_ };
        if old_move.is_none() || old_move != new_move {
            vfs_iunlock_two(old_dir, new_dir);
            break 'out neg(ENOSYS);
        }
        let move_fn = old_move.unwrap();
        let r = unsafe { move_fn(old_dir, old_dentry, new_dir, name, name_len) };
        vfs_iunlock_two(old_dir, new_dir);
        r
    };
    vfs_superblock_unlock(sb);
    result
}

pub(crate) extern "C" fn vfs_symlink(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
    target: *const c_char,
    target_len: usize,
) -> *mut vfs_inode {
    if dir.is_null() || unsafe { (*dir).sb.is_null() } {
        return err_ptr(neg(EINVAL));
    }
    if target.is_null() || target_len == 0 || target_len > VFS_PATH_MAX {
        return err_ptr(neg(EINVAL));
    }
    if name.is_null() || name_len == 0 {
        return err_ptr(neg(EINVAL));
    }

    loop {
        let sb = unsafe { (*dir).sb };
        if let Some(begin_fn) = unsafe { (*(*sb).ops).begin_transaction } {
            let r = unsafe { begin_fn(sb) };
            if r != 0 {
                return err_ptr(r);
            }
        }

        vfs_superblock_wlock(sb);
        vfs_ilock(dir);

        let ret_ptr: *mut vfs_inode = {
            let v = vfs_inode_valid_locked(dir);
            if v != 0 {
                err_ptr(v)
            } else if !is_dir(unsafe { (*dir).mode }) {
                err_ptr(neg(ENOTDIR))
            } else {
                match unsafe { (*(*dir).ops).symlink } {
                    None => err_ptr(neg(ENOSYS)),
                    Some(f) => unsafe { f(dir, mode, name, name_len, target, target_len) },
                }
            }
        };

        if is_eagain_ptr(ret_ptr) {
            vfs_iunlock(dir);
            vfs_superblock_unlock(sb);
            if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
                unsafe { end_fn(sb) };
            }
            scheduler_yield();
            continue;
        }

        // Note: symlinks don't need a parent reference (no ".."
        // traversal needed).
        vfs_iunlock(dir);
        vfs_superblock_unlock(sb);

        if let Some(end_fn) = unsafe { (*(*sb).ops).end_transaction } {
            let end_ret = unsafe { end_fn(sb) };
            if end_ret != 0 {
                crate::kprintln!(
                    "vfs_symlink: warning: end_transaction failed with error {}",
                    end_ret,
                );
            }
        }
        return ret_ptr;
    }
}

pub(crate) extern "C" fn vfs_itruncate(inode: *mut vfs_inode, new_size: loff_t) -> c_int {
    if inode.is_null() || unsafe { (*inode).sb.is_null() } {
        return neg(EINVAL);
    }
    vfs_ilock(inode);
    let ret: c_int = 'out: {
        let v = vfs_inode_valid_locked(inode);
        if v != 0 {
            break 'out v;
        }
        if !is_reg(unsafe { (*inode).mode }) {
            break 'out neg(EINVAL);
        }
        let f = match unsafe { (*(*inode).ops).truncate } {
            None => break 'out neg(ENOSYS),
            Some(f) => f,
        };
        unsafe { f(inode, new_size) }
    };
    vfs_iunlock(inode);
    ret
}

/// Lock two non-directory inodes to prevent deadlock (lower address
/// first).
pub(crate) extern "C" fn vfs_ilock_two_nondirectories(inode1: *mut vfs_inode, inode2: *mut vfs_inode) {
    kassert!(
        !inode1.is_null() && !inode2.is_null(),
        "vfs_ilock_two_nondirectories: inode is NULL"
    );
    if (inode1 as usize) < (inode2 as usize) {
        vfs_ilock(inode1);
        vfs_ilock(inode2);
    } else if (inode1 as usize) > (inode2 as usize) {
        vfs_ilock(inode2);
        vfs_ilock(inode1);
    } else {
        vfs_ilock(inode1);
    }
}

/// Lock two directory inodes to prevent deadlock. Caller must hold the
/// superblock read lock and ensure both inodes are directories.
pub(crate) extern "C" fn vfs_ilock_two_directories(inode1: *mut vfs_inode, inode2: *mut vfs_inode) -> c_int {
    if core::ptr::eq(inode1, inode2) {
        vfs_ilock(inode1);
        return 0;
    }
    if unsafe { (*inode1).sb } != unsafe { (*inode2).sb } {
        return neg(EXDEV); // Cross-filesystem locking not supported
    }

    // Borrowed from Linux kernel's lockdep strategy.
    let mut p = inode1;
    let mut q;
    let mut r: *mut vfs_inode;
    loop {
        r = unsafe { (*p).parent };
        if core::ptr::eq(r, inode2) || core::ptr::eq(r, p) {
            break;
        }
        p = r;
    }
    if core::ptr::eq(r, inode2) {
        // inode2 is the ancestor of inode1
        vfs_ilock(inode2);
        vfs_ilock(inode1);
        return 0;
    }
    q = inode2;
    loop {
        r = unsafe { (*q).parent };
        if core::ptr::eq(r, inode1) || core::ptr::eq(r, q) || core::ptr::eq(r, p) {
            break;
        }
        q = r;
    }
    if core::ptr::eq(r, inode1) {
        // inode1 is the ancestor of inode2
        vfs_ilock(inode1);
        vfs_ilock(inode2);
        return 0;
    } else if core::ptr::eq(r, p) {
        // inode1 and inode2 are in different branches
        if (inode1 as usize) < (inode2 as usize) {
            vfs_ilock(inode1);
            vfs_ilock(inode2);
        } else {
            vfs_ilock(inode2);
            vfs_ilock(inode1);
        }
        return 0;
    }

    // Since both inodes are on the same filesystem, they must share a
    // common ancestor (the fs root).
    xv6_panic(c"vfs_ilock_two_directories: unexpected condition".as_ptr());
}

pub(crate) extern "C" fn vfs_iunlock_two(inode1: *mut vfs_inode, inode2: *mut vfs_inode) {
    if !inode1.is_null() {
        vfs_iunlock(inode1);
    }
    if !inode2.is_null() && !core::ptr::eq(inode2, inode1) {
        vfs_iunlock(inode2);
    }
}

pub(crate) extern "C" fn vfs_chdir(new_cwd: *mut vfs_inode) -> c_int {
    if new_cwd.is_null() || unsafe { (*new_cwd).sb.is_null() } {
        return neg(EINVAL);
    }
    if core::ptr::eq(new_cwd, root_inode_ptr()) {
        return neg(EINVAL); // not allowed to change to the dummy root
    }
    let fs = current_fs();
    if core::ptr::eq(new_cwd, unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).cwd)) }) {
        return 0; // No change
    }

    let sb = unsafe { (*new_cwd).sb };
    vfs_superblock_rlock(sb);
    vfs_ilock(new_cwd);

    let mut old: vfs_inode_ref = unsafe { core::mem::zeroed() };

    let v = vfs_inode_valid_locked(new_cwd);
    if v != 0 {
        vfs_iunlock(new_cwd);
        vfs_superblock_unlock(sb);
        vfs_inode_put_ref(&mut old);
        return v;
    }
    if !is_dir(unsafe { (*new_cwd).mode }) {
        vfs_iunlock(new_cwd);
        vfs_superblock_unlock(sb);
        vfs_inode_put_ref(&mut old);
        return neg(ENOTDIR);
    }

    let mut new_ref: vfs_inode_ref = unsafe { core::mem::zeroed() };
    let r = vfs_inode_get_ref(new_cwd, &mut new_ref);
    if r != 0 {
        vfs_iunlock(new_cwd);
        vfs_superblock_unlock(sb);
        vfs_inode_put_ref(&mut old);
        return r;
    }
    vfs_iunlock(new_cwd);
    vfs_superblock_unlock(sb);

    // To keep it simple, fs_struct is only locked around the swap itself
    // (matches the C original's own comment).
    {
        let _g = fs_lock(fs);
        // SAFETY: `fs` is live; `cwd` is a plain embedded field.
        old = unsafe { (*fs).cwd };
        unsafe { (*fs).cwd = new_ref };
    }
    vfs_inode_put_ref(&mut old);
    0
}

pub(crate) extern "C" fn vfs_chroot(new_root: *mut vfs_inode) -> c_int {
    // NOTE (preserved 1:1 from the C original): `vfs_chdir`'s return
    // value is discarded here — if it fails (e.g. `new_root` is not a
    // directory or is invalid), `vfs_chroot` still proceeds to change
    // the root. Flagged, not fixed, per port fidelity.
    let _ = vfs_chdir(new_root);

    if core::ptr::eq(new_root, root_inode_ptr()) {
        return neg(EINVAL); // not allowed to change to the dummy root
    }
    let fs = current_fs();
    if core::ptr::eq(new_root, unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).rooti)) }) {
        return 0; // No change
    }

    let mut new_ref: vfs_inode_ref = unsafe { core::mem::zeroed() };
    let r = vfs_inode_get_ref(new_root, &mut new_ref);
    if r != 0 {
        return r;
    }
    vfs_idup(new_root);

    let mut old: vfs_inode_ref;
    {
        let _g = fs_lock(fs);
        old = unsafe { (*fs).rooti };
        unsafe { (*fs).rooti = new_ref };
    }
    vfs_inode_put_ref(&mut old);
    0
}

/// Get current working directory inode of the current process. Caller
/// must call `vfs_iput` on the returned inode when done.
pub(crate) extern "C" fn vfs_curdir() -> *mut vfs_inode {
    let fs = current_fs();
    // SAFETY: `fs` is live; `cwd` is a plain embedded field. Only the
    // current process can change its own cwd, so no lock is needed here
    // (matches the C original).
    let cwd = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).cwd)) };
    kassert!(!cwd.is_null(), "vfs_curdir: current working directory inode is NULL");
    vfs_idup(cwd);
    cwd
}

/// Get current root directory inode of the current process. Caller must
/// call `vfs_iput` on the returned inode when done.
pub(crate) extern "C" fn vfs_curroot() -> *mut vfs_inode {
    let fs = current_fs();
    // SAFETY: see `vfs_curdir`.
    let rooti = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*fs).rooti)) };
    kassert!(!rooti.is_null(), "vfs_curroot: current root directory inode is NULL");
    vfs_idup(rooti);
    rooti
}

/// Internal path lookup implementation. Returns `Err(Errno::Again)` on a
/// transient race (e.g. inode freed during mount traversal); the caller
/// ([`vfs_namei_inner`]) retries.
fn vfs_namei_once(path: *const c_char, path_len: usize) -> KResult<*mut vfs_inode> {
    if path.is_null() || path_len == 0 {
        return Err(Errno::Inval);
    }
    if path_len > VFS_PATH_MAX {
        return Err(Errno::NameTooLong);
    }

    let pathbuf = kmm_alloc(path_len + 1) as *mut c_char;
    if pathbuf.is_null() {
        return Err(Errno::NoMem);
    }

    // Get current root for ".." at root handling.
    let mut rooti = vfs_curroot();
    if is_err_or_null(rooti) {
        kmm_free(pathbuf as *mut c_void);
        if rooti.is_null() {
            return Err(Errno::Inval);
        }
        // `vfs_curroot` is a cross-module-callable C-ABI fn (not owned by
        // this cluster); its `ERR_PTR` encoding is preserved verbatim via
        // `Errno::Raw`.
        return Err(Errno::Raw(ptr_err(rooti)));
    }

    if unsafe { (*rooti).__bindgen_anon_1.mount() != 0 } {
        // SAFETY: `rooti` is a mountpoint inode; the mount union arm is
        // the live one.
        let mnt_rooti = unsafe { (*rooti).__bindgen_anon_2.__bindgen_anon_1.mnt_rooti };
        if mnt_rooti.is_null() {
            vfs_iput(rooti);
            kmm_free(pathbuf as *mut c_void);
            return Err(Errno::Inval); // Mounted root inode has no mounted root
        }
        if !atomic_inc_in_range(refcount_atomic(mnt_rooti), 0, VFS_INODE_MAX_REFCOUNT) {
            vfs_iput(rooti);
            kmm_free(pathbuf as *mut c_void);
            return Err(Errno::Again); // Mounted root is dying, retry
        }
        vfs_iput(rooti);
        rooti = mnt_rooti;
    }

    let mut path = path;
    let mut path_len = path_len;
    let mut pos: *mut vfs_inode;
    if unsafe { *path } == b'/' as c_char {
        // Absolute path, start from root.
        pos = rooti;
        vfs_idup(pos);
        path = unsafe { path.add(1) }; // skip leading '/'
        path_len -= 1;
    } else {
        // Relative path, start from cwd.
        pos = vfs_curdir();
        if is_err(pos) {
            let e = Errno::Raw(ptr_err(pos));
            vfs_iput(rooti);
            kmm_free(pathbuf as *mut c_void);
            return Err(e);
        }
    }

    // Copy the path since strtok_r modifies the string.
    if path_len > 0 {
        // SAFETY: `pathbuf` was allocated with `path_len + 1` bytes;
        // `path`/`path_len` describe a valid, disjoint byte range.
        unsafe { core::ptr::copy(path as *const u8, pathbuf as *mut u8, path_len) };
    }
    unsafe { *pathbuf.add(path_len) = 0 };

    let mut saveptr: *mut c_char = ptr::null_mut();
    let mut token = strtok_r(pathbuf, c"/".as_ptr(), &mut saveptr);
    // Mirrors the C original's `ret_inode`/`errored` pair: `result` tracks
    // both the in-flight success pointer and any error, set immediately
    // before each `break` exactly where the original set `ret_inode` +
    // `errored = true`.
    let mut result: KResult<*mut vfs_inode> = Ok(pos);

    while !token.is_null() {
        let token_len = strlen(token);

        let mut dentry: vfs_dentry = unsafe { core::mem::zeroed() };
        let lret = vfs_ilookup(pos, &mut dentry, token, token_len);
        if lret != 0 {
            vfs_iput(pos);
            pos = ptr::null_mut();
            result = Err(Errno::Raw(lret));
            break;
        }

        let next = vfs_get_dentry_inode(&mut dentry);
        vfs_release_dentry(&mut dentry);
        if is_err(next) {
            vfs_iput(pos);
            pos = ptr::null_mut();
            result = Err(Errno::Raw(ptr_err(next)));
            break;
        }

        vfs_iput(pos);
        pos = next;

        let mut mount_race = false;
        loop {
            let is_mount = unsafe { (*pos).__bindgen_anon_1.mount() != 0 };
            let mnt_rooti = unsafe { (*pos).__bindgen_anon_2.__bindgen_anon_1.mnt_rooti };
            if !(is_mount && !mnt_rooti.is_null()) {
                break;
            }
            if !atomic_inc_in_range(refcount_atomic(mnt_rooti), 0, VFS_INODE_MAX_REFCOUNT) {
                // Mount root is dying, need to retry the entire lookup.
                vfs_iput(pos);
                pos = ptr::null_mut();
                result = Err(Errno::Again);
                mount_race = true;
                break;
            }
            vfs_iput(pos);
            pos = mnt_rooti;
        }
        if mount_race {
            break;
        }

        result = Ok(pos);
        token = strtok_r(ptr::null_mut(), c"/".as_ptr(), &mut saveptr);
    }

    vfs_iput(rooti);
    kmm_free(pathbuf as *mut c_void);
    match result {
        Ok(p) if p.is_null() => Err(Errno::NoEnt),
        other => other,
    }
}

/// Resolve a path to an inode, handling retry logic for transient race
/// conditions (e.g. inode freed during mount traversal).
fn vfs_namei_inner(path: *const c_char, path_len: usize) -> KResult<*mut vfs_inode> {
    let mut retries = 0;
    loop {
        match vfs_namei_once(path, path_len) {
            Err(Errno::Again) => {
                // Transient race condition, yield and retry.
                scheduler_yield();
                retries += 1;
                if retries >= VFS_NAMEI_MAX_RETRIES {
                    // Too many retries, return the error.
                    return Err(Errno::Again);
                }
            }
            other => return other,
        }
    }
}

/// Resolve a path to an inode. Public wrapper handling retry logic for
/// transient race conditions (e.g. inode freed during mount traversal).
///
/// Returns an inode pointer with its refcount incremented on success, or
/// `ERR_PTR(errno)` on failure.
pub(crate) extern "C" fn vfs_namei(path: *const c_char, path_len: usize) -> *mut vfs_inode {
    result_to_errptr(vfs_namei_inner(path, path_len))
}

/// Resolve the parent directory of a path and copy the final name
/// component into `name`. Returns the parent directory inode with a
/// reference held on success, or `Err` on failure.
fn vfs_nameiparent_inner(
    path: *const c_char,
    path_len: usize,
    name: *mut c_char,
    name_size: usize,
) -> KResult<*mut vfs_inode> {
    if path.is_null() || path_len == 0 || name.is_null() || name_size == 0 {
        return Err(Errno::Inval);
    }
    if path_len > VFS_PATH_MAX {
        return Err(Errno::NameTooLong);
    }

    // Find the last path component.
    let mut end = path_len;
    // SAFETY: `path`/`path_len` describe a valid byte range.
    while end > 0 && unsafe { *path.add(end - 1) } == b'/' as c_char {
        end -= 1;
    }
    if end == 0 {
        // Path is just "/" or empty after trimming.
        return Err(Errno::Inval);
    }

    // Find the start of the last component.
    let mut name_start = end;
    while name_start > 0 && unsafe { *path.add(name_start - 1) } != b'/' as c_char {
        name_start -= 1;
    }

    // Extract the name component, truncating to fit the buffer (xv6
    // compatibility).
    let mut final_name_len = end - name_start;
    if final_name_len >= name_size {
        final_name_len = name_size - 1;
    }
    // SAFETY: `name` has room for `name_size` bytes (caller contract);
    // `path[name_start..name_start+final_name_len]` is in-bounds.
    unsafe {
        core::ptr::copy(
            path.add(name_start) as *const u8,
            name as *mut u8,
            final_name_len,
        );
        *name.add(final_name_len) = 0;
    }

    // Now get the parent path.
    let mut parent_len = name_start;
    while parent_len > 0 && unsafe { *path.add(parent_len - 1) } == b'/' as c_char {
        parent_len -= 1;
    }

    if parent_len == 0 {
        // Parent is root.
        return Ok(if unsafe { *path } == b'/' as c_char {
            vfs_curroot()
        } else {
            // Relative path with just one component, parent is cwd.
            vfs_curdir()
        });
    }

    // Resolve the parent path. `vfs_namei` is this same file's public
    // C-ABI wrapper (still `ERR_PTR`-encoded, unchanged); bridge back to
    // `KResult` at this internal call site via `errptr_to_result`.
    errptr_to_result(vfs_namei(path, parent_len)).map_err(Errno::Raw)
}

/// Resolve the parent directory of a path and copy the final name
/// component into `name`. Returns the parent directory inode with a
/// reference held on success, or `ERR_PTR` on failure.
pub(crate) extern "C" fn vfs_nameiparent(
    path: *const c_char,
    path_len: usize,
    name: *mut c_char,
    name_size: usize,
) -> *mut vfs_inode {
    result_to_errptr(vfs_nameiparent_inner(path, path_len, name, name_size))
}
