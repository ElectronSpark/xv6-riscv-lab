//! tmpfs inode operations — Rust port of `kernel/vfs/tmpfs/inode.c`
//! (Phase 2 Wave 18, sub-wave A; see `super` module doc and
//! `docs/rustify/phase2_plan.md`).
//!
//! Owns the directory child hash list (`struct tmpfs_dentry` alloc/link/
//! unlink, name hashing/comparison via a local, non-shared reimplementation
//! of `hlist_first_entry`/`hlist_next_entry` — see [`super::superblock`]'s
//! own copy for why this crate duplicates rather than shares that walk),
//! every `vfs_inode_ops` callback, and `struct vfs_inode_ops
//! tmpfs_inode_ops` itself.
//!
//! # Locking
//!
//! Every inode-mutating callback here runs with the new/target inode's
//! mutex held by the generic `vfs_alloc_inode()`/`vfs/inode.rs` caller
//! (see that module's own lock-order doc) and, per `vfs_inode_ops`'s
//! header contract, the superblock write lock for `create`/`mkdir`/
//! `rmdir`/`unlink`/`mknod`/`move`/`destroy_inode`. Nothing in this file
//! acquires a lock of its own -- it only touches memory already
//! protected by locks the vfs-core caller holds, exactly like the C
//! original.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{
    dev_t, hlist_entry_t, hlist_func_struct, hlist_t, ht_hash_t, list_node_t, loff_t, mode_t,
    stat, tmpfs_dentry, tmpfs_inode, vfs_dentry, vfs_dir_iter, vfs_inode, vfs_inode_ops, EBUSY,
    EEXIST, EINVAL, ENAMETOOLONG, ENOENT, ENOMEM, EOPNOTSUPP,
};

use super::{
    s_isblk, s_ischr, S_IFBLK, S_IFCHR, S_IFDIR, S_IFLNK, S_IFREG, TMPFS_HASH_BUCKETS,
    VFS_DENTRY_COOKIE_END, VFS_DENTRY_COOKIE_PARENT,
};

// P3-1C mesh sweep: vfs/{inode,fs}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures).
use crate::vfs::fs::{vfs_alloc_inode, vfs_remove_inode, vfs_release_dentry};
use crate::vfs::inode::{vfs_ilock, vfs_iunlock};

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

unsafe extern "C" {
    // printf.rs — C-variadic.

    // string.rs.
    safe fn strndup(s: *const c_char, n: usize) -> *mut c_char;
    safe fn strncmp(s1: *const c_char, s2: *const c_char, n: usize) -> c_int;
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

    // hlist.rs (Wave 1) -- the generic exported hash-table primitives
    // (bucket lookup/insert/remove); this file's own `hlist_first_entry`/
    // `hlist_next_entry` below reimplement the two non-exported
    // `static inline` walk helpers `hlist.h` never turns into real
    // symbols, same precedent as `hlist.rs`'s own module doc.
    safe fn hlist_init(hlist: *mut hlist_t, bucket_cnt: u64, func: *mut hlist_func_struct) -> c_int;
    safe fn hlist_get(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void;
    safe fn hlist_put(hlist: *mut hlist_t, node: *mut c_void, replace: bool) -> *mut c_void;
    safe fn hlist_pop(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void;
}

// P3-D3a: `kmm_alloc`/`kmm_free` are genuinely `unsafe fn` in
// `crate::mm::kalloc` now that their `#[no_mangle]` exports are gone;
// `cffi::raw`'s existing thin safe wrappers (identical signatures)
// preserve the `safe fn` facade the old redeclarations asserted.
use crate::mm::cffi::raw::{kmm_alloc, kmm_free};

// `kassert!`/`is_err`'s canonical homes are `crate::kstd`/crate root
// (P3-CS1 centralization). See `superblock.rs`. P3-CS13: the four
// ERR_PTR-returning inode-ops vtable methods (`__tmpfs_create`/
// `__tmpfs_mkdir`/`__tmpfs_mknod`/`__tmpfs_symlink`) now build a
// `KResult<*mut vfs_inode>` internally and encode it to the ABI-fixed
// ERR_PTR exactly once at the `extern "C"` boundary via
// `result_to_errptr`; only the error-return encoding changed, the
// alloc/link/cleanup control flow is byte-identical. The already-negative
// `c_int` returned by `__tmpfs_alloc_link_inode`/`__tmpfs_make_symlink_target`
// (which own the cross-module `vfs_alloc_inode` ERR_PTR / `ENOMEM`
// encoding) is carried through losslessly as `Errno::Raw(..)`.
use crate::kassert;
use crate::kstd::{is_err, result_to_errptr, Errno, KResult};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// ===========================================================================
// `TMPFS_INODE_EMBEDDED_DATA_LEN` -- mirrors the C macro
// `sizeof(struct tmpfs_inode) - offsetof(struct tmpfs_inode, sym.data)`.
// `sym.data` sits at the very start of the anonymous `dir`/`sym`/`file`
// union (`__bindgen_anon_1`), i.e. the same offset as the union field
// itself -- computed here from the real bindgen type via
// `core::mem::offset_of!`, not hand-copied, so it tracks the struct
// layout automatically if it ever changes.
// ===========================================================================
pub(crate) const TMPFS_INODE_EMBEDDED_DATA_LEN: usize =
    core::mem::size_of::<tmpfs_inode>() - core::mem::offset_of!(tmpfs_inode, __bindgen_anon_1);

// ===========================================================================
// Raw access into `tmpfs_inode`'s anonymous `{ dir; sym; file; }` union.
//
// Bindgen could not derive a native Rust `union` for this type (every
// variant embeds a zero-length/incomplete array: `dir.children`'s
// `hlist_t` has its own trailing `buckets[0]`, `sym.data`/`file.data`
// are themselves `[T; 0]`), so it fell back to the `__BindgenUnionField`
// emulation (`.as_ref()`/`.as_mut()` transmutes). Since every variant
// starts at the union's own offset 0 (that's what a union *is*), this
// file instead reinterprets the whole `__bindgen_anon_1` field directly
// as whichever concrete type the call site needs -- the same "offset-0
// reinterpretation, not `container_of` arithmetic" pattern `hlist.rs`'s
// module doc already documents and blesses for this crate.
// ===========================================================================

/// `&mut tmpfs_inode->dir.children` (a full `hlist_t`, sharing memory
/// with `dir.children_buckets[]` immediately following it in the struct
/// via `hlist_t`'s own trailing zero-length `buckets` array -- see
/// `superblock.rs`'s `hlist_bucket_at` doc for that trick).
///
/// # Safety
/// `ti` must point to a live `tmpfs_inode` that is a directory.
#[inline(always)]
unsafe fn dir_hlist(ti: *mut tmpfs_inode) -> *mut hlist_t {
    unsafe { ptr::addr_of_mut!((*ti).__bindgen_anon_1) as *mut hlist_t }
}

/// Base pointer of the embedded-data byte region (`sym.data`/`file.data`
/// in the C union).
///
/// # Safety
/// `ti` must point to a live `tmpfs_inode`.
#[inline(always)]
pub(crate) unsafe fn embedded_data(ti: *mut tmpfs_inode) -> *mut u8 {
    unsafe { ptr::addr_of_mut!((*ti).__bindgen_anon_1) as *mut u8 }
}

/// `&mut tmpfs_inode->sym.symlink_target`.
///
/// # Safety
/// `ti` must point to a live `tmpfs_inode` that is a symlink whose
/// target is allocated (not embedded).
#[inline(always)]
unsafe fn symlink_target_slot(ti: *mut tmpfs_inode) -> *mut *mut c_char {
    unsafe { ptr::addr_of_mut!((*ti).__bindgen_anon_1) as *mut *mut c_char }
}

// ===========================================================================
// Symlink / regular-file inode initialization.
// ===========================================================================

/// Initialize a tmpfs inode as a symlink with embedded target.
fn __tmpfs_make_symlink_target_embedded(ti: *mut tmpfs_inode, target: *const c_char, len: usize) {
    // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode` (caller's
    // contract); `len < TMPFS_INODE_EMBEDDED_DATA_LEN` (caller's contract,
    // matching the C original's own call-site guard).
    unsafe {
        let data = embedded_data(ti);
        memmove(data as *mut c_void, target as *const c_void, len);
        if len < TMPFS_INODE_EMBEDDED_DATA_LEN {
            ptr::write_bytes(data.add(len), 0, TMPFS_INODE_EMBEDDED_DATA_LEN - len);
        }
        (*ti).vfs_inode.size = len as loff_t;
        (*ti).vfs_inode.mode = S_IFLNK | 0o777;
    }
}

/// Initialize a tmpfs inode as a symlink with allocated target.
fn __tmpfs_make_symlink_target(ti: *mut tmpfs_inode, target: *const c_char, len: usize) -> c_int {
    let allocated = strndup(target, len);
    if allocated.is_null() {
        return neg(ENOMEM);
    }
    // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode`.
    unsafe {
        *symlink_target_slot(ti) = allocated;
        (*ti).vfs_inode.size = len as loff_t;
        (*ti).vfs_inode.mode = S_IFLNK | 0o777;
    }
    0
}

/// Initialize a tmpfs inode as a regular file.
fn __tmpfs_make_regfile(ti: *mut tmpfs_inode) {
    // NOTE (fidelity): the C original ends with
    // `memset(&tmpfs_inode->file, 0, sizeof(tmpfs_inode->file));`. The
    // `file` union variant is `union { uint8 data[0]; }` -- a struct
    // containing only a zero-length array, i.e. `sizeof(...) == 0` (bindgen
    // agrees: it generated a genuinely empty type for this variant). That
    // memset is therefore a zero-byte, provably-inert no-op in the C
    // original itself, not something this port needs to (or safely could,
    // in the same "zero specific bytes" sense) reproduce; every caller
    // (`__tmpfs_create`) already receives a freshly `memset`-to-zero
    // `tmpfs_inode` from `__tmpfs_alloc_inode_structure`, so the union
    // storage is already zero regardless. Intentionally dropped, not
    // silently lost -- documented per this crate's fidelity discipline.
    //
    // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode`.
    unsafe {
        (*ti).vfs_inode.size = 0;
        (*ti).embedded = 1;
        (*ti).vfs_inode.mode = S_IFREG | 0o644;
    }
}

// ===========================================================================
// Tmpfs dir entry helpers.
// ===========================================================================

/// Mirrors `hlist_entry_init()` (`kernel/inc/hlist.h`, `static inline`,
/// no external linkage).
fn hlist_entry_init(entry: *mut hlist_entry_t) {
    if entry.is_null() {
        return;
    }
    // SAFETY: `entry` is non-null and points to a live `hlist_entry_t`.
    unsafe {
        (*entry).bucket = ptr::null_mut();
        let ln = ptr::addr_of_mut!((*entry).list_entry);
        (*ln).next = ln;
        (*ln).prev = ln;
    }
}

fn __tmpfs_alloc_dentry(name_len: usize) -> *mut tmpfs_dentry {
    let size = core::mem::size_of::<tmpfs_dentry>() + name_len + 1;
    let dentry = kmm_alloc(size) as *mut tmpfs_dentry;
    if dentry.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `dentry` is a freshly allocated, exclusively-owned block of
    // `size` bytes.
    unsafe {
        ptr::write_bytes(dentry as *mut u8, 0, size);
        (*dentry).name_len = name_len;
        (*dentry).name = (*dentry).__name_start.as_mut_ptr();
        hlist_entry_init(ptr::addr_of_mut!((*dentry).hash_entry));
    }
    dentry
}

fn __tmpfs_free_dentry(dentry: *mut tmpfs_dentry) {
    if !dentry.is_null() {
        kmm_free(dentry as *mut c_void);
    }
}

/// Allocate and copy a tmpfs directory entry name.
fn __tmpfs_dentry_name_copy(name: *const c_char, name_len: usize) -> Result<*mut tmpfs_dentry, c_int> {
    let dentry = __tmpfs_alloc_dentry(name_len);
    if dentry.is_null() {
        return Err(neg(ENOMEM));
    }
    // SAFETY: `dentry` was just allocated with `name_len + 1` bytes of
    // trailing storage for `__name_start`/`name`.
    unsafe {
        memmove((*dentry).name as *mut c_void, name as *const c_void, name_len);
        *(*dentry).name.add(name_len) = 0;
    }
    Ok(dentry)
}

// ===========================================================================
// Tmpfs directory hash list functions.
// ===========================================================================

const GOLDEN_RATIO_PRIME_64: u64 = 0x9e37fffffffc0001;

/// Mirrors `hlist_hash_str()` (`kernel/inc/hlist.h`, `static inline`, no
/// external linkage) -- reimplemented locally per this crate's
/// established convention (see `hlist.rs`'s own module doc for the
/// precedent). The C original reads 8-byte words via
/// `*(ht_hash_t *)(str + i)`, a cast-and-deref that is technically UB
/// for a misaligned `str + i`; this port uses `ptr::read_unaligned`
/// instead, producing the exact same bytes-read/XOR/multiply sequence
/// (same value, same little-endian target) without that latent UB.
///
/// # Safety
/// `s` must point to at least `len` readable bytes.
unsafe fn hlist_hash_str(s: *const c_char, len: usize) -> ht_hash_t {
    unsafe {
        let bytes = s as *const u8;
        let mut ret = GOLDEN_RATIO_PRIME_64.wrapping_mul(len as u64);
        let tail_size = len % core::mem::size_of::<u64>();
        let mut i = 0usize;
        while i < len - tail_size {
            let word = ptr::read_unaligned(bytes.add(i) as *const u64);
            ret ^= word.wrapping_mul(GOLDEN_RATIO_PRIME_64);
            i += core::mem::size_of::<u64>();
        }
        if tail_size > 0 {
            let mut tail: u64 = 0;
            for j in (len - tail_size)..len {
                tail = (tail << 8) | u64::from(*bytes.add(j));
            }
            ret ^= tail.wrapping_mul(GOLDEN_RATIO_PRIME_64);
        }
        if ret == 0 {
            ret = GOLDEN_RATIO_PRIME_64; // Ensure non-zero hash
        }
        ret
    }
}

extern "C" fn __tmpfs_dir_hash_func(data: *mut c_void) -> ht_hash_t {
    let dentry = data as *mut tmpfs_dentry;
    // SAFETY: `data` is a live `tmpfs_dentry*` (hlist callback contract);
    // `name` points to `name_len` readable bytes.
    unsafe { hlist_hash_str((*dentry).name, (*dentry).name_len) }
}

extern "C" fn __tmpfs_dir_name_cmp_func(_hlist: *mut hlist_t, node: *mut c_void, key: *mut c_void) -> c_int {
    let dentry_node = node as *mut tmpfs_dentry;
    let dentry_key = key as *mut tmpfs_dentry;
    // SAFETY: `node`/`key` are live `tmpfs_dentry*` (hlist callback
    // contract).
    unsafe {
        let min_len = core::cmp::min((*dentry_node).name_len, (*dentry_key).name_len);
        let cmp = strncmp((*dentry_node).name, (*dentry_key).name, min_len);
        if cmp != 0 {
            return cmp;
        }
        if (*dentry_node).name_len > (*dentry_key).name_len {
            1
        } else if (*dentry_node).name_len < (*dentry_key).name_len {
            -1
        } else {
            0
        }
    }
}

extern "C" fn __tmpfs_dir_get_node_func(entry: *mut hlist_entry_t) -> *mut c_void {
    if entry.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `hash_entry` is the first field of `tmpfs_dentry` (offset 0).
    entry as *mut c_void
}

extern "C" fn __tmpfs_dir_get_entry_func(node: *mut c_void) -> *mut hlist_entry_t {
    if node.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `hash_entry` is the first field of `tmpfs_dentry` (offset 0).
    node as *mut hlist_entry_t
}

// Immutable: `hlist_init()` only *reads* `*func` (copies it into the
// hlist's own `func` field) -- never writes through the pointer -- so a
// plain `static` (not `static mut`) plus a cast-away-const pointer at the
// one call site avoids `static_mut_refs` entirely.
static __TMPFS_DIR_HLIST_FUNCS: hlist_func_struct = hlist_func_struct {
    hash: Some(__tmpfs_dir_hash_func),
    get_node: Some(__tmpfs_dir_get_node_func),
    get_entry: Some(__tmpfs_dir_get_entry_func),
    cmp_node: Some(__tmpfs_dir_name_cmp_func),
};

/// Mirrors `tmpfs_make_directory()`. Kept `#[no_mangle]`/exported per
/// `tmpfs_private.h`'s `extern` declaration -- `kernel/vfs/devtmpfs/
/// superblock.c` (still C) calls this directly on its own root inode.
pub(crate) extern "C" fn tmpfs_make_directory(ti: *mut tmpfs_inode) {
    // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode` (caller's
    // contract, matching the C original).
    unsafe {
        (*ti).vfs_inode.size = 0;
        (*ti).vfs_inode.mode = S_IFDIR | 0o755;
        let ret = hlist_init(
            dir_hlist(ti),
            TMPFS_HASH_BUCKETS,
            ptr::addr_of!(__TMPFS_DIR_HLIST_FUNCS) as *mut hlist_func_struct,
        );
        kassert!(
            ret == 0,
            "Failed to initialize tmpfs directory children hash list"
        );
    }
}

fn tmpfs_make_cdev(ti: *mut tmpfs_inode, cdev: dev_t) {
    // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode`.
    unsafe {
        (*ti).vfs_inode.mode = S_IFCHR | 0o644;
        (*ti).vfs_inode.size = 0;
        (*ti).vfs_inode.__bindgen_anon_2.cdev = cdev;
    }
}

fn tmpfs_make_bdev(ti: *mut tmpfs_inode, bdev: dev_t) {
    // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode`.
    unsafe {
        (*ti).vfs_inode.mode = S_IFBLK | 0o644;
        (*ti).vfs_inode.size = 0;
        (*ti).vfs_inode.__bindgen_anon_2.bdev = bdev;
    }
}

/// Lookup a child inode by name in a tmpfs directory inode.
fn __tmpfs_dir_lookup_by_name(ti: *mut tmpfs_inode, name: *const c_char, name_len: usize) -> *mut tmpfs_dentry {
    let mut tmp: tmpfs_dentry = unsafe { core::mem::zeroed() };
    tmp.name = name as *mut c_char;
    tmp.name_len = name_len;
    // SAFETY: `ti` is a live directory `tmpfs_inode`; `tmp` is a
    // stack-local dummy key node (never inserted, `hash_entry` unused by
    // `cmp_node`/`hash`).
    unsafe { hlist_get(dir_hlist(ti), ptr::addr_of_mut!(tmp) as *mut c_void) as *mut tmpfs_dentry }
}

/// Allocate a dentry and link a target inode into a tmpfs directory with
/// the given name. Will increase the link count of the target inode.
fn __tmpfs_do_link(dir: *mut tmpfs_inode, dentry: *mut tmpfs_dentry) -> c_int {
    // SAFETY: `dir` is a live directory `tmpfs_inode`; `dentry` is a
    // live, detached `tmpfs_dentry` (caller's contract).
    unsafe {
        let ret = hlist_put(dir_hlist(dir), dentry as *mut c_void, false) as *mut tmpfs_dentry;
        if ret == dentry {
            (*(*dentry).inode).vfs_inode.n_links += 1;
            return 0;
        }
        if !ret.is_null() {
            return neg(EEXIST); // Entry already exists
        }
        (*dentry).parent = dir;
        (*dentry).sb = (*dir).vfs_inode.sb;
        0
    }
}

/// Unlink a dentry from its parent tmpfs directory. Will decrease the
/// link count of the target inode.
fn __tmpfs_do_unlink(dentry: *mut tmpfs_dentry) {
    // SAFETY: `dentry` is a live, linked `tmpfs_dentry` (caller's
    // contract).
    unsafe {
        let popped = hlist_pop(dir_hlist((*dentry).parent), dentry as *mut c_void) as *mut tmpfs_dentry;
        kassert!(popped == dentry, "Tmpfs unlink: popped dentry does not match");
    }
}

/// Allocate and link a new inode in the given tmpfs directory. Caller
/// should hold the dir inode lock. Will not release the lock of the new
/// inode -- caller should do it.
fn __tmpfs_alloc_link_inode(
    dir: *mut tmpfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
) -> Result<(*mut tmpfs_inode, *mut tmpfs_dentry), c_int> {
    let dentry = match __tmpfs_dentry_name_copy(name, name_len) {
        Ok(d) => d,
        Err(e) => return Err(e),
    };
    let ret = __tmpfs_do_link(dir, dentry);
    if ret != 0 {
        __tmpfs_free_dentry(dentry);
        return Err(ret);
    }
    // SAFETY: `dir` is a live directory `tmpfs_inode`.
    let sb = unsafe { (*dir).vfs_inode.sb };
    let vfs_inode = vfs_alloc_inode(sb);
    if is_err(vfs_inode) {
        let ret = vfs_inode as isize as c_int;
        __tmpfs_do_unlink(dentry);
        __tmpfs_free_dentry(dentry);
        return Err(ret);
    }
    // SAFETY: `vfs_inode` is the first field of `tmpfs_inode` (offset 0);
    // `vfs_alloc_inode` returned a live, locked, ref-count-1 inode
    // allocated by `tmpfs_alloc_inode` (this driver's own
    // `vfs_superblock_ops.alloc_inode`), so this reinterpretation is sound.
    let ti = vfs_inode as *mut tmpfs_inode;
    // SAFETY: `dentry`/`ti` are live and exclusively owned here.
    unsafe {
        (*dentry).inode = ti;
        (*ti).vfs_inode.mode = mode;
        (*ti).vfs_inode.n_links = 1;
        // Backendless inodes are kept alive by n_links > 0, so refcount
        // of 1 (set by `vfs_alloc_inode`) suffices; no extra `vfs_ilock`
        // needed here (matches the C original's commented-out call).
    }
    Ok((ti, dentry))
}

/// Free symlink target path string if allocated. Do nothing if the
/// target is embedded. Assumes `ti` is a symlink and non-NULL.
///
/// Kept `#[no_mangle]`/exported per `tmpfs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn tmpfs_free_symlink_target(ti: *mut tmpfs_inode) {
    // SAFETY: `ti` is a live symlink `tmpfs_inode` (caller's contract).
    unsafe {
        if (*ti).vfs_inode.size as usize >= TMPFS_INODE_EMBEDDED_DATA_LEN {
            let slot = symlink_target_slot(ti);
            if !(*slot).is_null() {
                kmm_free(*slot as *mut c_void);
                *slot = ptr::null_mut();
                (*ti).vfs_inode.size = 0;
            }
        }
    }
}

// ===========================================================================
// tmpfs inode callbacks.
// ===========================================================================

extern "C" fn __tmpfs_lookup(dir: *mut vfs_inode, dentry: *mut vfs_dentry, name: *const c_char, name_len: usize) -> c_int {
    let tmpfs_dir = dir as *mut tmpfs_inode;

    // VFS handles "." and ".." for process root and local root. Driver
    // only sees ".." for ordinary (non-root) directories.
    if name_len == 2 {
        // SAFETY: `name` is a valid `name_len`-byte buffer.
        let is_dotdot = unsafe { strncmp(name, c"..".as_ptr(), 2) == 0 };
        if is_dotdot {
            // SAFETY: `dir`/`dentry` are live (caller's contract).
            unsafe {
                (*dentry).sb = (*dir).sb;
                (*dentry).name = strndup(name, name_len);
                if (*dentry).name.is_null() {
                    return neg(ENOMEM);
                }
                (*dentry).name_len = 2;
                (*dentry).ino = (*(*dir).parent).ino;
                (*dentry).cookies = VFS_DENTRY_COOKIE_PARENT;
            }
            return 0;
        }
    }

    let child_dentry = __tmpfs_dir_lookup_by_name(tmpfs_dir, name, name_len);
    if child_dentry.is_null() {
        return neg(ENOENT); // Not found
    }
    // SAFETY: `dir`/`dentry`/`child_dentry` are all live.
    unsafe {
        (*dentry).ino = (*(*child_dentry).inode).vfs_inode.ino;
        (*dentry).sb = (*dir).sb;
        (*dentry).parent = dir;
        (*dentry).name = strndup(name, name_len);
        if (*dentry).name.is_null() {
            return neg(ENOMEM);
        }
        (*dentry).name_len = name_len as u16;
        (*dentry).cookies = child_dentry as i64;
    }
    0
}

// ---------------------------------------------------------------------------
// Directory hash-list first/next-entry walk -- see `superblock.rs`'s copy
// for why this is duplicated rather than shared.
// ---------------------------------------------------------------------------

unsafe fn hlist_bucket_at(hlist: *mut hlist_t, idx: u64) -> *mut list_node_t {
    unsafe { (*hlist).buckets.as_mut_ptr().add(idx as usize) }
}

unsafe fn hlist_next_bucket(hlist: *mut hlist_t, bucket: *mut list_node_t) -> *mut list_node_t {
    unsafe {
        if hlist.is_null() || bucket.is_null() {
            return ptr::null_mut();
        }
        let base = (*hlist).buckets.as_mut_ptr();
        let offset = (bucket as usize - base as usize) / core::mem::size_of::<list_node_t>();
        if offset >= (*hlist).bucket_cnt as usize {
            return ptr::null_mut();
        }
        let next = offset + 1;
        if next < (*hlist).bucket_cnt as usize {
            hlist_bucket_at(hlist, next as u64)
        } else {
            ptr::null_mut()
        }
    }
}

unsafe fn bucket_first_entry(bucket: *mut list_node_t) -> *mut hlist_entry_t {
    unsafe {
        let next = (*bucket).next;
        if next == bucket {
            ptr::null_mut()
        } else {
            next as *mut hlist_entry_t
        }
    }
}

unsafe fn hlist_first_entry(hlist: *mut hlist_t) -> *mut hlist_entry_t {
    unsafe {
        if hlist.is_null() {
            return ptr::null_mut();
        }
        for i in 0..(*hlist).bucket_cnt {
            let e = bucket_first_entry(hlist_bucket_at(hlist, i));
            if !e.is_null() {
                return e;
            }
        }
        ptr::null_mut()
    }
}

unsafe fn hlist_next_entry(hlist: *mut hlist_t, entry: *mut hlist_entry_t) -> *mut hlist_entry_t {
    unsafe {
        if hlist.is_null() || entry.is_null() || (*entry).bucket.is_null() {
            return ptr::null_mut();
        }
        let mut bucket = (*entry).bucket;
        let node = entry as *mut list_node_t;
        let next_node = (*node).next;
        let mut next = if next_node == bucket {
            ptr::null_mut()
        } else {
            next_node as *mut hlist_entry_t
        };
        while next.is_null() {
            bucket = hlist_next_bucket(hlist, bucket);
            if bucket.is_null() {
                return ptr::null_mut();
            }
            next = bucket_first_entry(bucket);
        }
        next
    }
}

/// VFS synthesizes "." at index 0 and ".." for process/local roots at
/// index 1. Driver handles ".." for ordinary dirs (index 1) and all
/// children (index > 2).
extern "C" fn __tmpfs_dir_iter(dir: *mut vfs_inode, iter: *mut vfs_dir_iter, dentry: *mut vfs_dentry) -> c_int {
    let tmpfs_dir = dir as *mut tmpfs_inode;

    // SAFETY: `dir`/`iter`/`dentry` are live (caller's contract).
    unsafe {
        if (*iter).index == 1 {
            // VFS passes ".." to driver only for non-root directories.
            if (*dir).parent.is_null() {
                return neg(ENOENT); // No parent (should not happen for non-root)
            }
            vfs_release_dentry(dentry); // Release any previous name
            (*dentry).name = strndup(c"..".as_ptr(), 2);
            if (*dentry).name.is_null() {
                return neg(ENOMEM);
            }
            (*dentry).name_len = 2;
            (*dentry).cookies = VFS_DENTRY_COOKIE_PARENT;
            (*dentry).ino = (*(*dir).parent).ino;
            return 0;
        }

        // index > 2: iterate over directory children.
        let current: *mut tmpfs_dentry;
        if (*dentry).cookies == VFS_DENTRY_COOKIE_END || (*dentry).cookies == VFS_DENTRY_COOKIE_PARENT {
            current = hlist_first_entry(dir_hlist(tmpfs_dir)) as *mut tmpfs_dentry;
        } else {
            let prev = (*dentry).cookies as *mut tmpfs_dentry;
            current = hlist_next_entry(dir_hlist(tmpfs_dir), prev as *mut hlist_entry_t) as *mut tmpfs_dentry;
        }

        if current.is_null() {
            // End of directory.
            vfs_release_dentry(dentry); // Release any previous name
            (*dentry).name = ptr::null_mut();
            (*dentry).cookies = VFS_DENTRY_COOKIE_END;
            return 0;
        }

        let name = strndup((*current).name, (*current).name_len);
        if name.is_null() {
            return neg(ENOMEM);
        }
        vfs_release_dentry(dentry); // Release any previous name
        (*dentry).name = name;
        (*dentry).name_len = (*current).name_len as u16;
        (*dentry).ino = (*(*current).inode).vfs_inode.ino;
        (*dentry).cookies = current as i64;
        0
    }
}

extern "C" fn __tmpfs_readlink(inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> isize {
    let ti = inode as *mut tmpfs_inode;
    // SAFETY: `inode`/`buf` are live (caller's contract).
    unsafe {
        let link_len = (*inode).size as usize;
        if link_len + 1 > buflen {
            return neg(ENAMETOOLONG) as isize; // Buffer too small
        }
        if link_len < TMPFS_INODE_EMBEDDED_DATA_LEN {
            memmove(buf as *mut c_void, embedded_data(ti) as *const c_void, link_len);
        } else {
            memmove(buf as *mut c_void, *symlink_target_slot(ti) as *const c_void, link_len);
        }
        *buf.add(link_len) = 0; // Null-terminate the string
        link_len as isize
    }
}

extern "C" fn __tmpfs_create(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> *mut vfs_inode {
    result_to_errptr(__tmpfs_create_inner(dir, mode, name, name_len))
}

fn __tmpfs_create_inner(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
    let tmpfs_dir = dir as *mut tmpfs_inode;
    match __tmpfs_alloc_link_inode(tmpfs_dir, mode, name, name_len) {
        Err(e) => Err(Errno::Raw(e)),
        Ok((ti, _dentry)) => {
            __tmpfs_make_regfile(ti);
            // SAFETY: `ti` is a live, locked `tmpfs_inode`.
            vfs_iunlock(unsafe { ptr::addr_of_mut!((*ti).vfs_inode) });
            Ok(unsafe { ptr::addr_of_mut!((*ti).vfs_inode) })
        }
    }
}

extern "C" fn __tmpfs_unlink(dentry: *mut vfs_dentry, target: *mut vfs_inode) -> c_int {
    // SAFETY: `dentry`/`target` are live (caller's contract).
    unsafe {
        let tmpfs_dir = (*dentry).parent as *mut tmpfs_inode;
        // We need to lookup the dentry again to get the tmpfs_dentry.
        let tmpfs_dentry = __tmpfs_dir_lookup_by_name(tmpfs_dir, (*dentry).name, (*dentry).name_len as usize);
        if tmpfs_dentry.is_null() {
            return neg(ENOENT); // Entry not found
        }
        if ptr::addr_of_mut!((*(*tmpfs_dentry).inode).vfs_inode) != target {
            return neg(EINVAL); // Target inode does not match
        }

        // Remove directory entry -- this makes the file inaccessible by
        // name even if it's still open (Unix semantics).
        (*target).n_links -= 1;
        __tmpfs_do_unlink(tmpfs_dentry);
        __tmpfs_free_dentry(tmpfs_dentry);

        // VFS layer will call vfs_iput on target after we return.
        0
    }
}

extern "C" fn __tmpfs_link(target: *mut vfs_inode, dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> c_int {
    let tmpfs_dir = dir as *mut tmpfs_inode;
    let tmpfs_target = target as *mut tmpfs_inode;

    // SAFETY: `target` is a live `vfs_inode` (caller's contract).
    unsafe { (*target).n_links += 1 };

    let new_entry = match __tmpfs_dentry_name_copy(name, name_len) {
        Ok(d) => d,
        Err(e) => {
            unsafe { (*target).n_links -= 1 };
            return e;
        }
    };

    // SAFETY: `new_entry` is a live, detached `tmpfs_dentry`.
    unsafe { (*new_entry).inode = tmpfs_target };
    let ret = __tmpfs_do_link(tmpfs_dir, new_entry);
    if ret != 0 {
        unsafe { (*target).n_links -= 1 };
        __tmpfs_free_dentry(new_entry);
    }
    ret
}

extern "C" fn __tmpfs_mkdir(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> *mut vfs_inode {
    result_to_errptr(__tmpfs_mkdir_inner(dir, mode, name, name_len))
}

fn __tmpfs_mkdir_inner(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
    let tmpfs_dir = dir as *mut tmpfs_inode;
    match __tmpfs_alloc_link_inode(tmpfs_dir, mode, name, name_len) {
        Err(e) => Err(Errno::Raw(e)),
        Ok((ti, _dentry)) => {
            tmpfs_make_directory(ti);
            // SAFETY: `ti`/`dir` are live.
            unsafe {
                // Directory has n_links=2 for "." and ".." entries.
                (*ti).vfs_inode.n_links = 2;
                // Increment parent's n_links for this subdir's ".." entry.
                (*dir).n_links += 1;
                vfs_iunlock(ptr::addr_of_mut!((*ti).vfs_inode));
                Ok(ptr::addr_of_mut!((*ti).vfs_inode))
            }
        }
    }
}

extern "C" fn __tmpfs_rmdir(dentry: *mut vfs_dentry, target: *mut vfs_inode) -> c_int {
    // SAFETY: `dentry`/`target` are live (caller's contract).
    unsafe {
        let tmpfs_dir = (*dentry).parent as *mut tmpfs_inode;
        let tmpfs_dentry = __tmpfs_dir_lookup_by_name(tmpfs_dir, (*dentry).name, (*dentry).name_len as usize);
        if tmpfs_dentry.is_null() {
            return neg(ENOENT); // Entry not found
        }
        if ptr::addr_of_mut!((*(*tmpfs_dentry).inode).vfs_inode) != target {
            return neg(EINVAL); // Target inode does not match
        }
        // VFS core already verified directory is empty and not in use.
        // Directory n_links should be 2 (for "." and "..") when empty.
        kassert!((*target).n_links == 2, "Tmpfs rmdir: directory link count is not 2");
        (*target).n_links -= 2; // Remove both "." and ".." links
        // Decrement parent's n_links for this subdir's ".." entry.
        (*(*dentry).parent).n_links -= 1;
        __tmpfs_do_unlink(tmpfs_dentry);
        __tmpfs_free_dentry(tmpfs_dentry);
        // VFS layer will call vfs_iput on target after we return.
        0
    }
}

extern "C" fn __tmpfs_mknod(dir: *mut vfs_inode, mode: mode_t, dev: dev_t, name: *const c_char, name_len: usize) -> *mut vfs_inode {
    result_to_errptr(__tmpfs_mknod_inner(dir, mode, dev, name, name_len))
}

fn __tmpfs_mknod_inner(dir: *mut vfs_inode, mode: mode_t, dev: dev_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
    let tmpfs_dir = dir as *mut tmpfs_inode;
    if !s_isblk(mode) && !s_ischr(mode) {
        // TODO: Support FIFO, socket, and other special files.
        return Err(Errno::Inval); // Mknod can only create block/char device files
    }
    match __tmpfs_alloc_link_inode(tmpfs_dir, mode, name, name_len) {
        Err(e) => Err(Errno::Raw(e)),
        Ok((ti, _dentry)) => {
            if s_isblk(mode) {
                tmpfs_make_bdev(ti, dev);
            } else if s_ischr(mode) {
                tmpfs_make_cdev(ti, dev);
            }
            // SAFETY: `ti` is a live, locked `tmpfs_inode`.
            vfs_iunlock(unsafe { ptr::addr_of_mut!((*ti).vfs_inode) });
            Ok(unsafe { ptr::addr_of_mut!((*ti).vfs_inode) })
        }
    }
}

/// Mirrors `fs.h`'s `static inline vfs_inode_refcount()` (`SeqCst` load).
fn vfs_inode_refcount(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return -1;
    }
    // SAFETY: `ref_count` is a plain C `int` field, same size/align as
    // `AtomicI32`; `inode` is non-null and live.
    unsafe { (*(ptr::addr_of_mut!((*inode).ref_count) as *const AtomicI32)).load(Ordering::SeqCst) }
}

extern "C" fn __tmpfs_move(
    old_dir: *mut vfs_inode,
    old_dentry: *mut vfs_dentry,
    new_dir: *mut vfs_inode,
    name: *const c_char,
    name_len: usize,
) -> c_int {
    let tmpfs_old_dir = old_dir as *mut tmpfs_inode;
    let tmpfs_new_dir = new_dir as *mut tmpfs_inode;

    // SAFETY: `old_dentry` is live (caller's contract).
    let tmpfs_old_dentry =
        unsafe { __tmpfs_dir_lookup_by_name(tmpfs_old_dir, (*old_dentry).name, (*old_dentry).name_len as usize) };
    if tmpfs_old_dentry.is_null() {
        return neg(ENOENT); // Old entry not found
    }

    // Increase the link count and refcount of the old inode.
    // SAFETY: `tmpfs_old_dentry` is live.
    let target = unsafe { ptr::addr_of_mut!((*(*tmpfs_old_dentry).inode).vfs_inode) };
    let refcount = vfs_inode_refcount(target);
    if refcount > 2 {
        crate::kprintln!("Tmpfs move: target inode is busy, {}", refcount);
        return neg(EBUSY); // Target inode is busy
    }
    // SAFETY: `target` is live.
    unsafe { (*target).n_links += 1 };

    // Create a new dentry in the new directory.
    let mut ret: c_int;
    let mut new_entry: *mut tmpfs_dentry = ptr::null_mut();
    match __tmpfs_dentry_name_copy(name, name_len) {
        Err(e) => ret = e,
        Ok(d) => {
            new_entry = d;
            // SAFETY: `new_entry`/`tmpfs_old_dentry` are live.
            unsafe { (*new_entry).inode = (*tmpfs_old_dentry).inode };
            ret = __tmpfs_do_link(tmpfs_new_dir, new_entry);
            if ret == 0 {
                __tmpfs_do_unlink(tmpfs_old_dentry);
            }
        }
    }

    // SAFETY: `target` is live.
    unsafe { (*target).n_links -= 1 };
    // NOTE (deliberate deviation): the C original is
    // `if (ret != 0 && new_entry != NULL) free(new_entry); else
    // free(tmpfs_old_dentry);` -- an unconditional `else`. When
    // `__tmpfs_dentry_name_copy` itself fails (`ret != 0`, `new_entry ==
    // NULL`, the earliest failure point above), that `else` still runs
    // and frees `tmpfs_old_dentry` -- which was never unlinked from its
    // parent's hash list on this path (`__tmpfs_do_unlink` only runs
    // after a successful `__tmpfs_do_link`) -- a genuine
    // free-while-still-linked bug (dangling pointer left in the hash
    // bucket, later use-after-free). This port closes that hole with an
    // explicit `else if ret == 0` instead of a bare `else`: the two
    // *correct* C paths (new-dentry-alloc-or-link failed -> free
    // new_entry; full success -> free the now-unlinked old dentry) are
    // preserved exactly, and the third, buggy path now correctly frees
    // nothing (`tmpfs_old_dentry` is still live and linked, so leaving
    // it alone is the only safe outcome) instead of corrupting the hash
    // list.
    if ret != 0 && !new_entry.is_null() {
        __tmpfs_free_dentry(new_entry);
    } else if ret == 0 {
        __tmpfs_free_dentry(tmpfs_old_dentry);
    }
    ret
}

extern "C" fn __tmpfs_symlink(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
    target: *const c_char,
    target_len: usize,
) -> *mut vfs_inode {
    result_to_errptr(__tmpfs_symlink_inner(dir, mode, name, name_len, target, target_len))
}

fn __tmpfs_symlink_inner(
    dir: *mut vfs_inode,
    mode: mode_t,
    name: *const c_char,
    name_len: usize,
    target: *const c_char,
    target_len: usize,
) -> KResult<*mut vfs_inode> {
    let tmpfs_dir = dir as *mut tmpfs_inode;
    let (new_inode, dentry) = match __tmpfs_alloc_link_inode(tmpfs_dir, mode, name, name_len) {
        Err(e) => return Err(Errno::Raw(e)),
        Ok(v) => v,
    };
    if target_len < TMPFS_INODE_EMBEDDED_DATA_LEN {
        __tmpfs_make_symlink_target_embedded(new_inode, target, target_len);
    } else {
        let ret = __tmpfs_make_symlink_target(new_inode, target, target_len);
        if ret != 0 {
            __tmpfs_do_unlink(dentry);
            // SAFETY: `dir`/`new_inode` are live.
            let rm_ret = unsafe { vfs_remove_inode((*dir).sb, ptr::addr_of_mut!((*new_inode).vfs_inode)) };
            kassert!(
                rm_ret == 0,
                "Tmpfs symlink: failed to remove inode after symlink target allocation failure"
            );
            __tmpfs_free_dentry(dentry);
            // Inode is locked and detached from superblock; directly free it.
            // SAFETY: `new_inode` is live and locked.
            unsafe {
                vfs_iunlock(ptr::addr_of_mut!((*new_inode).vfs_inode));
                super::superblock::tmpfs_free_inode(ptr::addr_of_mut!((*new_inode).vfs_inode));
            }
            return Err(Errno::Raw(ret));
        }
    }
    // SAFETY: `new_inode` is live and locked.
    vfs_iunlock(unsafe { ptr::addr_of_mut!((*new_inode).vfs_inode) });
    Ok(unsafe { ptr::addr_of_mut!((*new_inode).vfs_inode) })
}

/// Destroy inode data when the last reference is dropped and `n_links ==
/// 0`. Called with inode locked and superblock write-locked.
extern "C" fn __tmpfs_destroy_inode(inode: *mut vfs_inode) {
    // SAFETY: `inode` is live (caller's contract).
    unsafe {
        let mode = (*inode).mode;
        if super::s_isreg(mode) {
            // For regular files, teardown pcache (frees all cached
            // pages). Embedded files have no pcache, so this is a no-op.
            super::file::tmpfs_inode_pcache_teardown(inode);
        } else if super::s_islnk(mode) {
            // For symlinks, free the target string if allocated externally.
            tmpfs_free_symlink_target(inode as *mut tmpfs_inode);
        }
        // For directories, they must be empty before rmdir, nothing to do.
        // For device nodes/pipes/sockets, no data to free.
    }
}

extern "C" fn __tmpfs_getattr(inode: *mut vfs_inode, stat: *mut stat) -> c_int {
    if inode.is_null() || stat.is_null() {
        return neg(EINVAL);
    }
    vfs_ilock(inode);
    // SAFETY: `inode`/`stat` are live and `inode` is now locked.
    unsafe {
        ptr::write_bytes(stat, 0, 1);
        (*stat).dev = if !(*inode).sb.is_null() { (*inode).sb as u64 as i32 } else { 0 };
        (*stat).ino = (*inode).ino;
        (*stat).mode = (*inode).mode;
        (*stat).nlink = (*inode).n_links;
        (*stat).size = (*inode).size as u64;
    }
    vfs_iunlock(inode);
    0
}

extern "C" fn __tmpfs_setattr(_inode: *mut vfs_inode, _stat: *const stat) -> c_int {
    neg(EOPNOTSUPP)
}

pub(crate) static TMPFS_INODE_OPS: vfs_inode_ops = vfs_inode_ops {
    lookup: Some(__tmpfs_lookup),
    dir_iter: Some(__tmpfs_dir_iter),
    readlink: Some(__tmpfs_readlink),
    create: Some(__tmpfs_create),
    getattr: Some(__tmpfs_getattr),
    setattr: Some(__tmpfs_setattr),
    link: Some(__tmpfs_link),
    unlink: Some(__tmpfs_unlink),
    mkdir: Some(__tmpfs_mkdir),
    rmdir: Some(__tmpfs_rmdir),
    mknod: Some(__tmpfs_mknod),
    move_: Some(__tmpfs_move),
    symlink: Some(__tmpfs_symlink),
    truncate: Some(super::truncate::__tmpfs_truncate),
    destroy_inode: Some(__tmpfs_destroy_inode),
    free_inode: Some(super::superblock::tmpfs_free_inode),
    dirty_inode: None,
    sync_inode: None,
    open: Some(super::file::tmpfs_open),
};
