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
//!
//! # Wave A free-fn -> associated-fn sweep
//!
//! Every relocatable free fn that used to live in this file is now an
//! associated fn: `impl TmpfsInode` (functions whose subject is a
//! concrete `*mut tmpfs_inode`), `impl TmpfsDentry` (dentry alloc/free/
//! unlink), or `impl Tmpfs` (the `vfs_inode_ops` callback bodies, which
//! take the *generic* `*mut vfs_inode`/`*mut vfs_dentry`/... and cast
//! down internally, plus the small scalar `neg` helper with no single
//! type home). [`Tmpfs`] is a ZST private to this file (NOT shared with
//! `superblock.rs`/`truncate.rs`'s own separate `Tmpfs` markers -- same
//! per-file-marker precedent as `xv6fs/inode.rs`'s `Xv6fs`). Every
//! relocated body is byte-identical to its old free-fn form -- only the
//! wrapping `impl` block, the fn name (leading `tmpfs_`/`__tmpfs_`
//! stripped), and call-site qualification changed; raw-pointer
//! parameters are unchanged (no first param became `&self`/`&T` -- see
//! this crate's freeze-noalias hazard note for why that would be
//! unsound for a lock-free-read `Freeze` type like `TmpfsInode`). The
//! `hlist_*` intrusive-hashlist primitives and the address-taken
//! `__tmpfs_dir_*_func` fn-pointer-table entries stay free fns (floor).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{
    bool_, dev_t, hlist_bucket_t, hlist_entry_t, hlist_t, ht_hash_t, list_node_t, loff_t, mode_t,
    stat, tmpfs_dentry, tmpfs_inode, vfs_dentry, vfs_dir_iter, vfs_inode, vfs_superblock,
    __IncompleteArrayField, EEXIST, EINVAL, ENOMEM,
};
use crate::hlist::HlistOps;
use crate::vfs::inode::InodeOps;

use super::{
    S_IFBLK, S_IFCHR, S_IFDIR, S_IFLNK, S_IFREG, TMPFS_HASH_BUCKETS,
    VFS_DENTRY_COOKIE_END, VFS_DENTRY_COOKIE_PARENT,
};

// P3-1C mesh sweep: vfs/{inode,fs}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures).
use crate::vfs::fs::{VfsSuperblock, vfs_release_dentry};
use crate::vfs::inode::VfsInode;

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

unsafe extern "C" {
    // printf.rs — C-variadic.

    // string.rs.
    safe fn strndup(s: *const c_char, n: usize) -> *mut c_char;
    safe fn strncmp(s1: *const c_char, s2: *const c_char, n: usize) -> c_int;
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
}

// P3-D3c: the hlist primitives (bucket lookup/insert/remove) are
// genuinely `unsafe fn`s in `crate::hlist` now that their `#[no_mangle]`
// exports are gone; this file's original extern declarations asserted
// `safe fn` (usual FFI-facade convention). Thin wrappers preserve that
// safe facade for the unchanged call sites. This file's own
// `hlist_first_entry`/`hlist_next_entry` below still reimplement the two
// non-exported `static inline` walk helpers `hlist.h` never turns into
// real symbols, same precedent as `hlist.rs`'s own module doc.
/// SAFETY: see [`crate::hlist::Hlist::init`]'s contract.
fn hlist_init(hlist: *mut hlist_t, bucket_cnt: u64, ops: Option<&'static dyn HlistOps>) -> c_int {
    unsafe { crate::hlist::Hlist::init(hlist, bucket_cnt, ops) }
}
/// SAFETY: see [`crate::hlist::Hlist::get`]'s contract.
fn hlist_get(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void {
    unsafe { crate::hlist::Hlist::get(hlist, node) }
}
/// SAFETY: see [`crate::hlist::Hlist::put`]'s contract.
fn hlist_put(hlist: *mut hlist_t, node: *mut c_void, replace: bool) -> *mut c_void {
    unsafe { crate::hlist::Hlist::put(hlist, node, replace) }
}
/// SAFETY: see [`crate::hlist::Hlist::pop`]'s contract.
fn hlist_pop(hlist: *mut hlist_t, node: *mut c_void) -> *mut c_void {
    unsafe { crate::hlist::Hlist::pop(hlist, node) }
}

// P3-D3a: `kmm_alloc`/`kmm_free` are genuinely `unsafe fn` in
// `crate::mm::kalloc` now that their `#[no_mangle]` exports are gone;
// `cffi::raw`'s existing thin safe wrappers (identical signatures)
// preserve the `safe fn` facade the old redeclarations asserted.
use crate::mm::cffi::raw::{kmm_alloc, kmm_free};

// `kassert!`'s canonical home is `crate::kstd`/crate root (P3-CS1
// centralization). See `superblock.rs`. P3-10b (ops-table redesign):
// the CS13-era `extern "C"` wrappers + `result_to_errptr` boundary
// conversions are GONE — every former callback body below is
// `KResult`-native and dispatched directly through the
// [`TmpfsInodeOps`] trait impl; the private helpers
// (`__tmpfs_alloc_link_inode`/`__tmpfs_dentry_name_copy`/
// `__tmpfs_do_link`/`__tmpfs_make_symlink_target`) speak `Errno` too,
// so no ERR_PTR or raw-errno encoding remains anywhere in this driver.
use crate::kassert;
use crate::kstd::{Errno, KResult};

/// ZST marker (Wave A free-fn -> associated-fn sweep, this file only)
/// for the `vfs_inode_ops` callback bodies -- they take the *generic*
/// `*mut vfs_inode`/`*mut vfs_dentry` (cast down to `*mut tmpfs_inode`
/// internally) and so don't belong to a single concrete tmpfs data
/// type -- plus the small scalar `neg` helper with no natural type
/// home. Private to this file, matching `superblock.rs`/`truncate.rs`'s
/// own separate `Tmpfs` markers (each file keeps its own -- same
/// precedent as `xv6fs/inode.rs`'s per-file `Xv6fs` marker).
struct Tmpfs;

impl Tmpfs {
    #[inline(always)]
    const fn neg(e: u32) -> c_int {
        -(e as c_int)
    }
}

// ===========================================================================
// Native tmpfs inode-side types — P3-N8 nativization (user directive:
// remove the C-compatible interfaces). `TmpfsInode` (+ its per-type data
// union) and `TmpfsDentry` are the canonical native definitions of
// `kernel/vfs/tmpfs/tmpfs_private.h`'s `struct tmpfs_inode`/`struct
// tmpfs_dentry`: `build.rs` blocklists the bindgen emissions and
// re-exports these types as `crate::bindings::tmpfs_inode`/
// `tmpfs_dentry` (facade `pub use`, N2 pattern). The C header stays
// unchanged (no C consumers remain — the kernel tree has zero `.c`
// files).
//
// The anonymous `{ dir; sym; file; }` union became the real Rust union
// [`TmpfsInodeData`] (field `u`; N6 `PageTypeData` precedent) with the
// named arms `TmpfsDirData` (struct) / `TmpfsSymData` / `TmpfsFileData`
// — bindgen had degraded the whole nest to `__BindgenUnionField` blob
// shells (`tmpfs_inode__bindgen_ty_1` + 3, all swept by the blocklist)
// because the `sym`/`file` arms embed zero-length arrays.
//
// DERIVE DECISIONS (P3-N8): `tmpfs_inode`/`tmpfs_dentry` and the outer
// union shell derived neither Copy nor Clone in the pre-nativization
// bindgen output — the natives faithfully have no derives (both are
// intrusive records: the embedded NONCOPY `vfs_inode`, live hash-list
// links). The `dir` arm derived Copy/Clone (POD hash-list storage) —
// kept. The `sym`/`file` arms' degraded shells had NO derives, but the
// native unions DO derive Copy/Clone — a deliberate, documented
// deviation: Rust union fields must be `Copy` (or `ManuallyDrop`-
// wrapped, which would poison every consumer expression), and both arms
// are plain POD (`*mut c_char` / zero-length arrays) for which bitwise
// copy is trivially sound; nothing ever `=`-copies them standalone
// (grep-verified — they exist only inside `TmpfsInodeData`).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + cross-compiler value probe (toolchain gcc,
// rv64gc/lp64d — scratchpad p3n8_probe_values.c); both agree on every
// value asserted below (no pipe-style divergence).
// ===========================================================================

/// Native form of the `dir` arm (directory inodes): the child-dentry
/// hash list plus its inline bucket storage (`hlist_t`'s own trailing
/// zero-length `buckets[]` overlays `children_buckets` — see
/// `superblock.rs`'s `hlist_bucket_at` doc for that trick).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct TmpfsDirData {
    pub children: hlist_t,
    pub children_buckets: [hlist_bucket_t; 15],
}

/// Native form of the `sym` arm (symlink inodes): the target path,
/// either heap-allocated (`symlink_target`) or embedded in place
/// (`data`, overlaying the rest of the union — see
/// [`TMPFS_INODE_EMBEDDED_DATA_LEN`]).
#[repr(C)]
#[derive(Copy, Clone)]
pub union TmpfsSymData {
    pub symlink_target: *mut c_char,
    pub data: [c_char; 0],
}

/// Native form of the `file` arm (regular files): the embedded-data
/// overlay used while `embedded == true` (non-embedded file data lives
/// in the per-inode pcache `vfs_inode.i_data`).
#[repr(C)]
#[derive(Copy, Clone)]
pub union TmpfsFileData {
    pub data: [u8; 0],
}

/// Native form of `struct tmpfs_inode`'s anonymous per-type data union
/// (bindgen shell: `tmpfs_inode__bindgen_ty_1`, 288/8).
#[repr(C)]
pub union TmpfsInodeData {
    pub dir: TmpfsDirData,
    pub sym: TmpfsSymData,
    pub file: TmpfsFileData,
}

/// Native `struct tmpfs_inode` (`kernel/vfs/tmpfs/tmpfs_private.h`).
/// The anonymous union became the named `u` field (consumers re-pointed
/// from bindgen's `__bindgen_anon_1`; `proc/access.rs` naming
/// precedent).
#[repr(C, align(64))]
pub struct TmpfsInode {
    pub vfs_inode: vfs_inode,
    pub embedded: bool_,
    pub u: TmpfsInodeData,
}

/// Native `struct tmpfs_dentry` (`kernel/vfs/tmpfs/tmpfs_private.h`) —
/// one directory entry: intrusive hash-list linkage plus the
/// inline-allocated name (`__name_start` is C's trailing flexible array
/// member, kept as bindgen's `__IncompleteArrayField` so the
/// `.as_mut_ptr()` consumer idiom is unchanged).
#[repr(C)]
pub struct TmpfsDentry {
    pub hash_entry: hlist_entry_t,
    pub parent: *mut TmpfsInode,
    pub sb: *mut vfs_superblock,
    pub inode: *mut TmpfsInode,
    pub name_len: usize,
    pub name: *mut c_char,
    pub __name_start: __IncompleteArrayField<c_char>,
}

// P3-N8 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc probe (see the module
// note above).
const _: () = {
    // TRAIT-OPS: `hlist_t`'s old `hlist_func_struct` fn-pointer table (32
    // bytes) is gone, replaced by a 16-byte `Option<&dyn HlistOps>` fat
    // pointer -- `hlist_t`/`children` shrinks 48 -> 32, so
    // `children_buckets` (immediately following it) shifts 48 -> 32 and
    // `TmpfsDirData`/`TmpfsInodeData` shrink 288 -> 272. `TmpfsInode`'s own
    // size/offsets (below) are UNCHANGED: `u`'s tail alignment padding
    // (`align(64)`) absorbs the union's 16-byte shrink exactly as it
    // absorbed the pre-shrink slack (1096 + 272 = 1368, still <= the
    // existing 1408 = 22 * 64 boundary).
    assert!(core::mem::size_of::<TmpfsDirData>() == 272, "tmpfs dir arm size");
    assert!(core::mem::align_of::<TmpfsDirData>() == 8, "tmpfs dir arm alignment");
    assert!(core::mem::offset_of!(TmpfsDirData, children) == 0, "dir.children offset");
    assert!(core::mem::offset_of!(TmpfsDirData, children_buckets) == 32, "dir.children_buckets offset");
    assert!(core::mem::size_of::<TmpfsSymData>() == 8, "tmpfs sym arm size");
    assert!(core::mem::align_of::<TmpfsSymData>() == 8, "tmpfs sym arm alignment");
    assert!(core::mem::size_of::<TmpfsFileData>() == 0, "tmpfs file arm size");
    assert!(core::mem::align_of::<TmpfsFileData>() == 1, "tmpfs file arm alignment");
    assert!(core::mem::size_of::<TmpfsInodeData>() == 272, "tmpfs inode union size");
    assert!(core::mem::align_of::<TmpfsInodeData>() == 8, "tmpfs inode union alignment");

    assert!(core::mem::size_of::<TmpfsInode>() == 1408, "tmpfs_inode size");
    assert!(core::mem::align_of::<TmpfsInode>() == 64, "tmpfs_inode alignment");
    assert!(core::mem::offset_of!(TmpfsInode, vfs_inode) == 0, "tmpfs_inode.vfs_inode offset");
    assert!(core::mem::offset_of!(TmpfsInode, embedded) == 1088, "tmpfs_inode.embedded offset");
    assert!(core::mem::offset_of!(TmpfsInode, u) == 1096, "tmpfs_inode anon union offset");

    assert!(core::mem::size_of::<TmpfsDentry>() == 64, "tmpfs_dentry size");
    assert!(core::mem::align_of::<TmpfsDentry>() == 8, "tmpfs_dentry alignment");
    assert!(core::mem::offset_of!(TmpfsDentry, hash_entry) == 0, "tmpfs_dentry.hash_entry offset");
    assert!(core::mem::offset_of!(TmpfsDentry, parent) == 24, "tmpfs_dentry.parent offset");
    assert!(core::mem::offset_of!(TmpfsDentry, sb) == 32, "tmpfs_dentry.sb offset");
    assert!(core::mem::offset_of!(TmpfsDentry, inode) == 40, "tmpfs_dentry.inode offset");
    assert!(core::mem::offset_of!(TmpfsDentry, name_len) == 48, "tmpfs_dentry.name_len offset");
    assert!(core::mem::offset_of!(TmpfsDentry, name) == 56, "tmpfs_dentry.name offset");
    assert!(core::mem::offset_of!(TmpfsDentry, __name_start) == 64, "tmpfs_dentry.__name_start offset");
};

// ===========================================================================
// `TMPFS_INODE_EMBEDDED_DATA_LEN` -- mirrors the C macro
// `sizeof(struct tmpfs_inode) - offsetof(struct tmpfs_inode, sym.data)`.
// `sym.data` sits at the very start of the anonymous `dir`/`sym`/`file`
// union (field `u`), i.e. the same offset as the union field itself --
// computed here from the real native type via `core::mem::offset_of!`,
// not hand-copied, so it tracks the struct layout automatically if it
// ever changes.
// ===========================================================================
pub(crate) const TMPFS_INODE_EMBEDDED_DATA_LEN: usize =
    core::mem::size_of::<tmpfs_inode>() - core::mem::offset_of!(tmpfs_inode, u);

// ===========================================================================
// Raw access into `tmpfs_inode`'s `{ dir; sym; file; }` union (field
// `u`). Since every variant starts at the union's own offset 0 (that's
// what a union *is*), this file reinterprets the whole `u` field
// directly as whichever concrete type the call site needs -- the same
// "offset-0 reinterpretation, not `container_of` arithmetic" pattern
// `hlist.rs`'s module doc already documents and blesses for this crate
// (pre-N8, the same casts targeted bindgen's degraded
// `__bindgen_anon_1` blob).
// ===========================================================================

/// `&mut tmpfs_inode->dir.children` (a full `hlist_t`, sharing memory
/// with `dir.children_buckets[]` immediately following it in the struct
/// via `hlist_t`'s own trailing zero-length `buckets` array -- see
/// `superblock.rs`'s `hlist_bucket_at` doc for that trick).
///
/// # Safety
/// `ti` must point to a live `tmpfs_inode` that is a directory.
impl TmpfsInode {
    #[inline(always)]
    unsafe fn dir_hlist(ti: *mut tmpfs_inode) -> *mut hlist_t {
        unsafe { ptr::addr_of_mut!((*ti).u) as *mut hlist_t }
    }
}

/// Base pointer of the embedded-data byte region (`sym.data`/`file.data`
/// in the C union).
///
/// # Safety
/// `ti` must point to a live `tmpfs_inode`.
impl TmpfsInode {
    #[inline(always)]
    pub(crate) unsafe fn embedded_data(ti: *mut tmpfs_inode) -> *mut u8 {
        unsafe { ptr::addr_of_mut!((*ti).u) as *mut u8 }
    }
}

/// `&mut tmpfs_inode->sym.symlink_target`.
///
/// # Safety
/// `ti` must point to a live `tmpfs_inode` that is a symlink whose
/// target is allocated (not embedded).
impl TmpfsInode {
    #[inline(always)]
    unsafe fn symlink_target_slot(ti: *mut tmpfs_inode) -> *mut *mut c_char {
        unsafe { ptr::addr_of_mut!((*ti).u) as *mut *mut c_char }
    }
}

// ===========================================================================
// Symlink / regular-file inode initialization.
// ===========================================================================

impl TmpfsInode {
    /// Initialize a tmpfs inode as a symlink with embedded target.
    fn make_symlink_target_embedded(ti: *mut tmpfs_inode, target: *const c_char, len: usize) {
        // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode` (caller's
        // contract); `len < TMPFS_INODE_EMBEDDED_DATA_LEN` (caller's contract,
        // matching the C original's own call-site guard).
        unsafe {
            let data = TmpfsInode::embedded_data(ti);
            memmove(data as *mut c_void, target as *const c_void, len);
            if len < TMPFS_INODE_EMBEDDED_DATA_LEN {
                ptr::write_bytes(data.add(len), 0, TMPFS_INODE_EMBEDDED_DATA_LEN - len);
            }
            (*ti).vfs_inode.size = len as loff_t;
            (*ti).vfs_inode.mode = S_IFLNK | 0o777;
        }
    }

    /// Initialize a tmpfs inode as a symlink with allocated target.
    fn make_symlink_target(ti: *mut tmpfs_inode, target: *const c_char, len: usize) -> KResult<()> {
        let allocated = strndup(target, len);
        if allocated.is_null() {
            return Err(Errno::NoMem);
        }
        // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode`.
        unsafe {
            *TmpfsInode::symlink_target_slot(ti) = allocated;
            (*ti).vfs_inode.size = len as loff_t;
            (*ti).vfs_inode.mode = S_IFLNK | 0o777;
        }
        Ok(())
    }
}

// ===========================================================================
// Inherent methods on the native `TmpfsInode` (goal 1: methods on structs,
// not free functions over `*mut tmpfs_inode`). Each of these leaf
// initializers touches only plain fields of an exclusively-owned,
// freshly-allocated inode, so its body is fully SAFE — the single audited
// `unsafe { &mut *ti }` boundary hoist happens once at the caller
// (N-S1 structural conversion). Writing a `Copy` union field
// (`vfs_inode.dev_mnt.cdev`/`.bdev`) through `&mut self` is itself safe;
// only *reading* a union field would require `unsafe`.
// ===========================================================================
impl TmpfsInode {
    /// Initialize this inode as a regular file. (Was `__tmpfs_make_regfile`.)
    fn make_regfile(&mut self) {
        // NOTE (fidelity): the C original ends with
        // `memset(&tmpfs_inode->file, 0, sizeof(tmpfs_inode->file));`. The
        // `file` union variant is `union { uint8 data[0]; }` -- a struct
        // containing only a zero-length array, i.e. `sizeof(...) == 0`
        // (bindgen agrees: it generated a genuinely empty type for this
        // variant). That memset is therefore a zero-byte, provably-inert
        // no-op in the C original itself, not something this port needs to
        // (or safely could, in the same "zero specific bytes" sense)
        // reproduce; every caller (`__tmpfs_create`) already receives a
        // freshly `memset`-to-zero `tmpfs_inode` from
        // `__tmpfs_alloc_inode_structure`, so the union storage is already
        // zero regardless. Intentionally dropped, not silently lost --
        // documented per this crate's fidelity discipline.
        self.vfs_inode.size = 0;
        self.embedded = 1;
        self.vfs_inode.mode = S_IFREG | 0o644;
    }

    /// Initialize this inode as a character device node.
    /// (Was `tmpfs_make_cdev`.)
    fn make_cdev(&mut self, cdev: dev_t) {
        self.vfs_inode.mode = S_IFCHR | 0o644;
        self.vfs_inode.size = 0;
        self.vfs_inode.dev_mnt.cdev = cdev;
    }

    /// Initialize this inode as a block device node.
    /// (Was `tmpfs_make_bdev`.)
    fn make_bdev(&mut self, bdev: dev_t) {
        self.vfs_inode.mode = S_IFBLK | 0o644;
        self.vfs_inode.size = 0;
        self.vfs_inode.dev_mnt.bdev = bdev;
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

impl TmpfsDentry {
    fn alloc(name_len: usize) -> *mut tmpfs_dentry {
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

    fn free(dentry: *mut tmpfs_dentry) {
        if !dentry.is_null() {
            kmm_free(dentry as *mut c_void);
        }
    }

    /// Allocate and copy a tmpfs directory entry name.
    fn name_copy(name: *const c_char, name_len: usize) -> Result<*mut tmpfs_dentry, Errno> {
        let dentry = TmpfsDentry::alloc(name_len);
        if dentry.is_null() {
            return Err(Errno::NoMem);
        }
        // SAFETY: `dentry` was just allocated with `name_len + 1` bytes of
        // trailing storage for `__name_start`/`name`.
        unsafe {
            memmove((*dentry).name as *mut c_void, name as *const c_void, name_len);
            *(*dentry).name.add(name_len) = 0;
        }
        Ok(dentry)
    }
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

// TRAIT-OPS: the old `hlist_func_struct` fn-pointer table
// (`__tmpfs_dir_hash_func`/`__tmpfs_dir_get_node_func`/
// `__tmpfs_dir_get_entry_func`/`__tmpfs_dir_name_cmp_func`) ->
// `TmpfsDirHlistOps`, a ZST implementing `HlistOps`. Bodies are
// byte-identical to the old free fns; only the receiver (`&self`) and the
// `extern "C"` wrapper are gone.
struct TmpfsDirHlistOps;

impl HlistOps for TmpfsDirHlistOps {
    unsafe fn hash(&self, data: *mut c_void) -> ht_hash_t {
        let dentry = data as *mut tmpfs_dentry;
        // SAFETY: `data` is a live `tmpfs_dentry*` (hlist callback contract);
        // `name` points to `name_len` readable bytes.
        unsafe { hlist_hash_str((*dentry).name, (*dentry).name_len) }
    }

    unsafe fn cmp_node(&self, _hlist: *mut hlist_t, node: *mut c_void, key: *mut c_void) -> c_int {
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

    unsafe fn get_node(&self, entry: *mut hlist_entry_t) -> *mut c_void {
        if entry.is_null() {
            return ptr::null_mut();
        }
        // SAFETY: `hash_entry` is the first field of `tmpfs_dentry` (offset 0).
        entry as *mut c_void
    }

    unsafe fn get_entry(&self, node: *mut c_void) -> *mut hlist_entry_t {
        if node.is_null() {
            return ptr::null_mut();
        }
        // SAFETY: `hash_entry` is the first field of `tmpfs_dentry` (offset 0).
        node as *mut hlist_entry_t
    }
}

static __TMPFS_DIR_HLIST_OPS: TmpfsDirHlistOps = TmpfsDirHlistOps;

impl TmpfsInode {
    /// Mirrors `tmpfs_make_directory()`. Kept `#[no_mangle]`/exported per
    /// `tmpfs_private.h`'s `extern` declaration -- `kernel/vfs/devtmpfs/
    /// superblock.c` (still C) calls this directly on its own root inode.
    pub(crate) extern "C" fn make_directory(ti: *mut tmpfs_inode) {
        // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode` (caller's
        // contract, matching the C original).
        unsafe {
            (*ti).vfs_inode.size = 0;
            (*ti).vfs_inode.mode = S_IFDIR | 0o755;
            let ret = hlist_init(
                TmpfsInode::dir_hlist(ti),
                TMPFS_HASH_BUCKETS,
                Some(&__TMPFS_DIR_HLIST_OPS),
            );
            kassert!(
                ret == 0,
                "Failed to initialize tmpfs directory children hash list"
            );
        }
    }

    // `tmpfs_make_cdev`/`tmpfs_make_bdev` are now the safe inherent methods
    // `TmpfsInode::make_cdev`/`make_bdev` (see the `impl TmpfsInode` block
    // above); their sole caller `__tmpfs_mknod_inner` hoists one `&mut *ti`.

    /// Lookup a child inode by name in a tmpfs directory inode.
    fn dir_lookup_by_name(ti: *mut tmpfs_inode, name: *const c_char, name_len: usize) -> *mut tmpfs_dentry {
        let mut tmp: tmpfs_dentry = unsafe { core::mem::zeroed() };
        tmp.name = name as *mut c_char;
        tmp.name_len = name_len;
        // SAFETY: `ti` is a live directory `tmpfs_inode`; `tmp` is a
        // stack-local dummy key node (never inserted, `hash_entry` unused by
        // `cmp_node`/`hash`).
        unsafe { hlist_get(TmpfsInode::dir_hlist(ti), ptr::addr_of_mut!(tmp) as *mut c_void) as *mut tmpfs_dentry }
    }

    /// Allocate a dentry and link a target inode into a tmpfs directory with
    /// the given name. Will increase the link count of the target inode.
    fn do_link(dir: *mut tmpfs_inode, dentry: *mut tmpfs_dentry) -> KResult<()> {
        // SAFETY: `dir` is a live directory `tmpfs_inode`; `dentry` is a
        // live, detached `tmpfs_dentry` (caller's contract).
        unsafe {
            let ret = hlist_put(TmpfsInode::dir_hlist(dir), dentry as *mut c_void, false) as *mut tmpfs_dentry;
            if ret == dentry {
                (*(*dentry).inode).vfs_inode.n_links += 1;
                return Ok(());
            }
            if !ret.is_null() {
                return Err(Errno::Exist); // Entry already exists
            }
            (*dentry).parent = dir;
            (*dentry).sb = (*dir).vfs_inode.sb;
            Ok(())
        }
    }

    /// Allocate and link a new inode in the given tmpfs directory. Caller
    /// should hold the dir inode lock. Will not release the lock of the new
    /// inode -- caller should do it.
    fn alloc_link_inode(
        dir: *mut tmpfs_inode,
        mode: mode_t,
        name: *const c_char,
        name_len: usize,
    ) -> Result<(*mut tmpfs_inode, *mut tmpfs_dentry), Errno> {
        let dentry = match TmpfsDentry::name_copy(name, name_len) {
            Ok(d) => d,
            Err(e) => return Err(e),
        };
        if let Err(e) = TmpfsInode::do_link(dir, dentry) {
            TmpfsDentry::free(dentry);
            return Err(e);
        }
        // SAFETY: `dir` is a live directory `tmpfs_inode`. (P3-10b:
        // `KResult`-native VFS-core call, no ERR_PTR decode.)
        let sb = unsafe { (*dir).vfs_inode.sb };
        let vfs_inode = match crate::vfs::fs::VfsSuperblock::vfs_alloc_inode_inner(sb) {
            Ok(i) => i,
            Err(e) => {
                TmpfsDentry::do_unlink(dentry);
                TmpfsDentry::free(dentry);
                return Err(e);
            }
        };
        // SAFETY: `vfs_inode` is the first field of `tmpfs_inode` (offset 0);
        // `vfs_alloc_inode_inner` returned a live, locked, ref-count-1 inode
        // allocated by this driver's own `SuperblockOps::alloc_inode`, so
        // this reinterpretation is sound.
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
    pub(crate) extern "C" fn free_symlink_target(ti: *mut tmpfs_inode) {
        // SAFETY: `ti` is a live symlink `tmpfs_inode` (caller's contract).
        unsafe {
            if (*ti).vfs_inode.size as usize >= TMPFS_INODE_EMBEDDED_DATA_LEN {
                let slot = TmpfsInode::symlink_target_slot(ti);
                if !(*slot).is_null() {
                    kmm_free(*slot as *mut c_void);
                    *slot = ptr::null_mut();
                    (*ti).vfs_inode.size = 0;
                }
            }
        }
    }
}

impl TmpfsDentry {
    /// Unlink a dentry from its parent tmpfs directory. Will decrease the
    /// link count of the target inode.
    fn do_unlink(dentry: *mut tmpfs_dentry) {
        // SAFETY: `dentry` is a live, linked `tmpfs_dentry` (caller's
        // contract).
        unsafe {
            let popped = hlist_pop(TmpfsInode::dir_hlist((*dentry).parent), dentry as *mut c_void) as *mut tmpfs_dentry;
            kassert!(popped == dentry, "Tmpfs unlink: popped dentry does not match");
        }
    }
}

// ===========================================================================
// tmpfs inode callbacks.
// ===========================================================================

impl Tmpfs {
    fn lookup(dir: *mut vfs_inode, dentry: *mut vfs_dentry, name: *const c_char, name_len: usize) -> KResult<()> {
        let tmpfs_dir = dir as *mut tmpfs_inode;

        // VFS handles "." and ".." for process root and local root. Driver
        // only sees ".." for ordinary (non-root) directories.
        if name_len == 2 {
            // `strncmp` is a `safe fn` in this file's `unsafe extern` FFI facade,
            // so comparing the first two bytes needs no `unsafe` block.
            let is_dotdot = strncmp(name, c"..".as_ptr(), 2) == 0;
            if is_dotdot {
                // SAFETY: `dir`/`dentry` are live (caller's contract).
                unsafe {
                    (*dentry).sb = (*dir).sb;
                    (*dentry).name = strndup(name, name_len);
                    if (*dentry).name.is_null() {
                        return Err(Errno::NoMem);
                    }
                    (*dentry).name_len = 2;
                    (*dentry).ino = (*(*dir).parent).ino;
                    (*dentry).cookies = VFS_DENTRY_COOKIE_PARENT;
                }
                return Ok(());
            }
        }

        let child_dentry = TmpfsInode::dir_lookup_by_name(tmpfs_dir, name, name_len);
        if child_dentry.is_null() {
            return Err(Errno::NoEnt); // Not found
        }
        // SAFETY: `dir`/`dentry`/`child_dentry` are all live.
        unsafe {
            (*dentry).ino = (*(*child_dentry).inode).vfs_inode.ino;
            (*dentry).sb = (*dir).sb;
            (*dentry).parent = dir;
            (*dentry).name = strndup(name, name_len);
            if (*dentry).name.is_null() {
                return Err(Errno::NoMem);
            }
            (*dentry).name_len = name_len as u16;
            (*dentry).cookies = child_dentry as i64;
        }
        Ok(())
    }
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
impl Tmpfs {
    /// VFS synthesizes "." at index 0 and ".." for process/local roots at
    /// index 1. Driver handles ".." for ordinary dirs (index 1) and all
    /// children (index > 2).
    fn dir_iter(dir: *mut vfs_inode, iter: *mut vfs_dir_iter, dentry: *mut vfs_dentry) -> KResult<()> {
        let tmpfs_dir = dir as *mut tmpfs_inode;

        // SAFETY: `dir`/`iter`/`dentry` are live (caller's contract).
        unsafe {
            if (*iter).index == 1 {
                // VFS passes ".." to driver only for non-root directories.
                if (*dir).parent.is_null() {
                    return Err(Errno::NoEnt); // No parent (should not happen for non-root)
                }
                vfs_release_dentry(dentry); // Release any previous name
                (*dentry).name = strndup(c"..".as_ptr(), 2);
                if (*dentry).name.is_null() {
                    return Err(Errno::NoMem);
                }
                (*dentry).name_len = 2;
                (*dentry).cookies = VFS_DENTRY_COOKIE_PARENT;
                (*dentry).ino = (*(*dir).parent).ino;
                return Ok(());
            }

            // index > 2: iterate over directory children.
            let current: *mut tmpfs_dentry;
            if (*dentry).cookies == VFS_DENTRY_COOKIE_END || (*dentry).cookies == VFS_DENTRY_COOKIE_PARENT {
                current = hlist_first_entry(TmpfsInode::dir_hlist(tmpfs_dir)) as *mut tmpfs_dentry;
            } else {
                let prev = (*dentry).cookies as *mut tmpfs_dentry;
                current = hlist_next_entry(TmpfsInode::dir_hlist(tmpfs_dir), prev as *mut hlist_entry_t) as *mut tmpfs_dentry;
            }

            if current.is_null() {
                // End of directory.
                vfs_release_dentry(dentry); // Release any previous name
                (*dentry).name = ptr::null_mut();
                (*dentry).cookies = VFS_DENTRY_COOKIE_END;
                return Ok(());
            }

            let name = strndup((*current).name, (*current).name_len);
            if name.is_null() {
                return Err(Errno::NoMem);
            }
            vfs_release_dentry(dentry); // Release any previous name
            (*dentry).name = name;
            (*dentry).name_len = (*current).name_len as u16;
            (*dentry).ino = (*(*current).inode).vfs_inode.ino;
            (*dentry).cookies = current as i64;
            Ok(())
        }
    }
}

impl Tmpfs {
    fn readlink(inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> KResult<isize> {
        let ti = inode as *mut tmpfs_inode;
        // SAFETY: `inode`/`buf` are live (caller's contract).
        unsafe {
            let link_len = (*inode).size as usize;
            if link_len + 1 > buflen {
                return Err(Errno::NameTooLong); // Buffer too small
            }
            if link_len < TMPFS_INODE_EMBEDDED_DATA_LEN {
                memmove(buf as *mut c_void, TmpfsInode::embedded_data(ti) as *const c_void, link_len);
            } else {
                memmove(buf as *mut c_void, *TmpfsInode::symlink_target_slot(ti) as *const c_void, link_len);
            }
            *buf.add(link_len) = 0; // Null-terminate the string
            Ok(link_len as isize)
        }
    }
}

impl Tmpfs {
    fn create_inner(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        let tmpfs_dir = dir as *mut tmpfs_inode;
        match TmpfsInode::alloc_link_inode(tmpfs_dir, mode, name, name_len) {
            Err(e) => Err(e),
            Ok((ti, _dentry)) => {
                // SAFETY: `ti` is the freshly-allocated, locked, still-unpublished
                // inode just returned by `__tmpfs_alloc_link_inode` (not yet
                // reachable by any other hart), so a `&mut` is exclusive. This
                // single hoist covers the safe method init and both former
                // `addr_of_mut` raw derefs (N-S1).
                let ti = unsafe { &mut *ti };
                ti.make_regfile();
                let vi = ptr::addr_of_mut!(ti.vfs_inode);
                VfsInode::vfs_iunlock(vi);
                Ok(vi)
            }
        }
    }
}

impl Tmpfs {
    fn unlink(dentry: *mut vfs_dentry, target: *mut vfs_inode) -> KResult<()> {
        // SAFETY: `dentry`/`target` are live (caller's contract).
        unsafe {
            let tmpfs_dir = (*dentry).parent as *mut tmpfs_inode;
            // We need to lookup the dentry again to get the tmpfs_dentry.
            let tmpfs_dentry = TmpfsInode::dir_lookup_by_name(tmpfs_dir, (*dentry).name, (*dentry).name_len as usize);
            if tmpfs_dentry.is_null() {
                return Err(Errno::NoEnt); // Entry not found
            }
            if ptr::addr_of_mut!((*(*tmpfs_dentry).inode).vfs_inode) != target {
                return Err(Errno::Inval); // Target inode does not match
            }

            // Remove directory entry -- this makes the file inaccessible by
            // name even if it's still open (Unix semantics).
            (*target).n_links -= 1;
            TmpfsDentry::do_unlink(tmpfs_dentry);
            TmpfsDentry::free(tmpfs_dentry);

            // VFS layer will call vfs_iput on target after we return.
            Ok(())
        }
    }
}

impl Tmpfs {
    fn link(target: *mut vfs_inode, dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> KResult<()> {
        let tmpfs_dir = dir as *mut tmpfs_inode;
        let tmpfs_target = target as *mut tmpfs_inode;

        // SAFETY: `target` is a live `vfs_inode` (caller's contract).
        unsafe { (*target).n_links += 1 };

        let new_entry = match TmpfsDentry::name_copy(name, name_len) {
            Ok(d) => d,
            Err(e) => {
                unsafe { (*target).n_links -= 1 };
                return Err(e);
            }
        };

        // SAFETY: `new_entry` is a live, detached `tmpfs_dentry`.
        unsafe { (*new_entry).inode = tmpfs_target };
        let ret = TmpfsInode::do_link(tmpfs_dir, new_entry);
        if ret.is_err() {
            unsafe { (*target).n_links -= 1 };
            TmpfsDentry::free(new_entry);
        }
        ret
    }
}

impl Tmpfs {
    fn mkdir_inner(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        let tmpfs_dir = dir as *mut tmpfs_inode;
        match TmpfsInode::alloc_link_inode(tmpfs_dir, mode, name, name_len) {
            Err(e) => Err(e),
            Ok((ti, _dentry)) => {
                TmpfsInode::make_directory(ti);
                // SAFETY: `ti`/`dir` are live.
                unsafe {
                    // Directory has n_links=2 for "." and ".." entries.
                    (*ti).vfs_inode.n_links = 2;
                    // Increment parent's n_links for this subdir's ".." entry.
                    (*dir).n_links += 1;
                    VfsInode::vfs_iunlock(ptr::addr_of_mut!((*ti).vfs_inode));
                    Ok(ptr::addr_of_mut!((*ti).vfs_inode))
                }
            }
        }
    }
}

impl Tmpfs {
    fn rmdir(dentry: *mut vfs_dentry, target: *mut vfs_inode) -> KResult<()> {
        // SAFETY: `dentry`/`target` are live (caller's contract).
        unsafe {
            let tmpfs_dir = (*dentry).parent as *mut tmpfs_inode;
            let tmpfs_dentry = TmpfsInode::dir_lookup_by_name(tmpfs_dir, (*dentry).name, (*dentry).name_len as usize);
            if tmpfs_dentry.is_null() {
                return Err(Errno::NoEnt); // Entry not found
            }
            if ptr::addr_of_mut!((*(*tmpfs_dentry).inode).vfs_inode) != target {
                return Err(Errno::Inval); // Target inode does not match
            }
            // VFS core already verified directory is empty and not in use.
            // Directory n_links should be 2 (for "." and "..") when empty.
            kassert!((*target).n_links == 2, "Tmpfs rmdir: directory link count is not 2");
            (*target).n_links -= 2; // Remove both "." and ".." links
            // Decrement parent's n_links for this subdir's ".." entry.
            (*(*dentry).parent).n_links -= 1;
            TmpfsDentry::do_unlink(tmpfs_dentry);
            TmpfsDentry::free(tmpfs_dentry);
            // VFS layer will call vfs_iput on target after we return.
            Ok(())
        }
    }
}

impl Tmpfs {
    fn mknod_inner(dir: *mut vfs_inode, mode: mode_t, dev: dev_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        let tmpfs_dir = dir as *mut tmpfs_inode;
        if !super::Tmpfs::s_isblk(mode) && !super::Tmpfs::s_ischr(mode) {
            // TODO: Support FIFO, socket, and other special files.
            return Err(Errno::Inval); // Mknod can only create block/char device files
        }
        match TmpfsInode::alloc_link_inode(tmpfs_dir, mode, name, name_len) {
            Err(e) => Err(e),
            Ok((ti, _dentry)) => {
                // SAFETY: freshly-allocated, locked, still-unpublished inode, so a
                // `&mut` is exclusive. One hoist covers the safe device-node init
                // methods and both former `addr_of_mut` raw derefs (N-S1).
                let ti = unsafe { &mut *ti };
                if super::Tmpfs::s_isblk(mode) {
                    ti.make_bdev(dev);
                } else if super::Tmpfs::s_ischr(mode) {
                    ti.make_cdev(dev);
                }
                let vi = ptr::addr_of_mut!(ti.vfs_inode);
                VfsInode::vfs_iunlock(vi);
                Ok(vi)
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

    fn move_(
        old_dir: *mut vfs_inode,
        old_dentry: *mut vfs_dentry,
        new_dir: *mut vfs_inode,
        name: *const c_char,
        name_len: usize,
    ) -> KResult<()> {
        let tmpfs_old_dir = old_dir as *mut tmpfs_inode;
        let tmpfs_new_dir = new_dir as *mut tmpfs_inode;

        // SAFETY: `old_dentry` is live (caller's contract).
        let tmpfs_old_dentry =
            unsafe { TmpfsInode::dir_lookup_by_name(tmpfs_old_dir, (*old_dentry).name, (*old_dentry).name_len as usize) };
        if tmpfs_old_dentry.is_null() {
            return Err(Errno::NoEnt); // Old entry not found
        }

        // Increase the link count and refcount of the old inode.
        // SAFETY: `tmpfs_old_dentry` is live.
        let target = unsafe { ptr::addr_of_mut!((*(*tmpfs_old_dentry).inode).vfs_inode) };
        let refcount = Tmpfs::vfs_inode_refcount(target);
        if refcount > 2 {
            crate::kprintln!("Tmpfs move: target inode is busy, {}", refcount);
            return Err(Errno::Busy); // Target inode is busy
        }
        // SAFETY: `target` is live.
        unsafe { (*target).n_links += 1 };

        // Create a new dentry in the new directory.
        let mut ret: KResult<()>;
        let mut new_entry: *mut tmpfs_dentry = ptr::null_mut();
        match TmpfsDentry::name_copy(name, name_len) {
            Err(e) => ret = Err(e),
            Ok(d) => {
                new_entry = d;
                // SAFETY: `new_entry`/`tmpfs_old_dentry` are live.
                unsafe { (*new_entry).inode = (*tmpfs_old_dentry).inode };
                ret = TmpfsInode::do_link(tmpfs_new_dir, new_entry);
                if ret.is_ok() {
                    TmpfsDentry::do_unlink(tmpfs_old_dentry);
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
        if ret.is_err() && !new_entry.is_null() {
            TmpfsDentry::free(new_entry);
        } else if ret.is_ok() {
            TmpfsDentry::free(tmpfs_old_dentry);
        }
        ret
    }
}

impl Tmpfs {
    fn symlink_inner(
        dir: *mut vfs_inode,
        mode: mode_t,
        name: *const c_char,
        name_len: usize,
        target: *const c_char,
        target_len: usize,
    ) -> KResult<*mut vfs_inode> {
        let tmpfs_dir = dir as *mut tmpfs_inode;
        let (new_inode, dentry) = match TmpfsInode::alloc_link_inode(tmpfs_dir, mode, name, name_len) {
            Err(e) => return Err(e),
            Ok(v) => v,
        };
        if target_len < TMPFS_INODE_EMBEDDED_DATA_LEN {
            TmpfsInode::make_symlink_target_embedded(new_inode, target, target_len);
        } else if let Err(e) = TmpfsInode::make_symlink_target(new_inode, target, target_len) {
            {
                TmpfsDentry::do_unlink(dentry);
                // SAFETY: `new_inode` is the freshly-allocated, locked inode; it
                // is now detached from its directory (`__tmpfs_do_unlink` above)
                // and unpublished, so a `&mut` is exclusive. `dir.sb` stays a raw
                // read (`dir` is a shared, refcounted parent — see the
                // reference-hoist screen in the module notes). The `vi` reborrow
                // feeds the safe `vfs_remove_inode`/`vfs_iunlock`/
                // `tmpfs_free_inode` calls (N-S1).
                let ni = unsafe { &mut *new_inode };
                let vi = ptr::addr_of_mut!(ni.vfs_inode);
                let dsb = unsafe { (*dir).sb };
                let rm_ret = VfsSuperblock::vfs_remove_inode(dsb, vi);
                kassert!(
                    rm_ret == 0,
                    "Tmpfs symlink: failed to remove inode after symlink target allocation failure"
                );
                TmpfsDentry::free(dentry);
                // Inode is locked and detached from superblock; directly free it.
                VfsInode::vfs_iunlock(vi);
                super::superblock::TmpfsSuperblock::free_inode(vi);
                return Err(e);
            }
        }
        // SAFETY: `new_inode` is the live, locked, still-unpublished symlink
        // inode; one `&mut` hoist replaces the two former `addr_of_mut` derefs.
        let ni = unsafe { &mut *new_inode };
        let vi = ptr::addr_of_mut!(ni.vfs_inode);
        VfsInode::vfs_iunlock(vi);
        Ok(vi)
    }
}

impl Tmpfs {
    /// Destroy inode data when the last reference is dropped and `n_links ==
    /// 0`. Called with inode locked and superblock write-locked.
    fn destroy_inode(inode: *mut vfs_inode) {
        // SAFETY: `inode` is live (caller's contract).
        unsafe {
            let mode = (*inode).mode;
            if super::Tmpfs::s_isreg(mode) {
                // For regular files, teardown pcache (frees all cached
                // pages). Embedded files have no pcache, so this is a no-op.
                super::file::tmpfs_inode_pcache_teardown(inode);
            } else if super::Tmpfs::s_islnk(mode) {
                // For symlinks, free the target string if allocated externally.
                TmpfsInode::free_symlink_target(inode as *mut tmpfs_inode);
            }
            // For directories, they must be empty before rmdir, nothing to do.
            // For device nodes/pipes/sockets, no data to free.
        }
    }

    fn getattr(inode: *mut vfs_inode, stat: *mut stat) -> KResult<()> {
        if inode.is_null() || stat.is_null() {
            return Err(Errno::Inval);
        }
        VfsInode::vfs_ilock(inode);
        // SAFETY: `inode`/`stat` are live and `inode` is now locked.
        unsafe {
            ptr::write_bytes(stat, 0, 1);
            (*stat).dev = if !(*inode).sb.is_null() { (*inode).sb as u64 as i32 } else { 0 };
            (*stat).ino = (*inode).ino;
            (*stat).mode = (*inode).mode;
            (*stat).nlink = (*inode).n_links;
            (*stat).size = (*inode).size as u64;
        }
        VfsInode::vfs_iunlock(inode);
        Ok(())
    }
}

/// tmpfs's [`InodeOps`] implementor (P3-10b) — a zero-sized unit
/// struct whose single `static` instance ([`TMPFS_INODE_OPS`]) is
/// installed into every tmpfs (and devtmpfs) inode's `ops` as
/// `Some(&STATIC)` by `__tmpfs_alloc_inode_structure`. The old table's
/// `None` slots (`dirty_inode`/`sync_inode`) are the trait's
/// do-nothing `Ok(())` defaults; every other method delegates to the
/// (now `KResult`-native) former callback bodies in this module and
/// its siblings.
pub(crate) struct TmpfsInodeOps;

impl InodeOps for TmpfsInodeOps {
    unsafe fn lookup(&self, dir: *mut vfs_inode, dentry: *mut vfs_dentry, name: *const c_char, name_len: usize) -> KResult<()> {
        Tmpfs::lookup(dir, dentry, name, name_len)
    }
    unsafe fn dir_iter(&self, dir: *mut vfs_inode, iter: *mut vfs_dir_iter, ret_dentry: *mut vfs_dentry) -> KResult<()> {
        Tmpfs::dir_iter(dir, iter, ret_dentry)
    }
    unsafe fn readlink(&self, inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> KResult<isize> {
        Tmpfs::readlink(inode, buf, buflen)
    }
    unsafe fn create(&self, dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        Tmpfs::create_inner(dir, mode, name, name_len)
    }
    unsafe fn getattr(&self, inode: *mut vfs_inode, stat: *mut crate::bindings::stat) -> KResult<()> {
        Tmpfs::getattr(inode, stat)
    }
    /// Mirrors `__tmpfs_setattr` — not supported (no VFS dispatch site
    /// exists yet either).
    unsafe fn setattr(&self, _inode: *mut vfs_inode, _stat: *const crate::bindings::stat) -> KResult<()> {
        Err(Errno::OpNotSupp)
    }
    unsafe fn link(&self, old: *mut vfs_inode, dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> KResult<()> {
        Tmpfs::link(old, dir, name, name_len)
    }
    unsafe fn unlink(&self, dentry: *mut vfs_dentry, target: *mut vfs_inode) -> KResult<()> {
        Tmpfs::unlink(dentry, target)
    }
    unsafe fn mkdir(&self, dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        Tmpfs::mkdir_inner(dir, mode, name, name_len)
    }
    unsafe fn rmdir(&self, dentry: *mut vfs_dentry, target: *mut vfs_inode) -> KResult<()> {
        Tmpfs::rmdir(dentry, target)
    }
    unsafe fn mknod(&self, dir: *mut vfs_inode, mode: mode_t, dev: dev_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        Tmpfs::mknod_inner(dir, mode, dev, name, name_len)
    }
    unsafe fn move_(&self, old_dir: *mut vfs_inode, old_dentry: *mut vfs_dentry, new_dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> KResult<()> {
        Tmpfs::move_(old_dir, old_dentry, new_dir, name, name_len)
    }
    unsafe fn symlink(&self, dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize, target: *const c_char, target_len: usize) -> KResult<*mut vfs_inode> {
        Tmpfs::symlink_inner(dir, mode, name, name_len, target, target_len)
    }
    unsafe fn truncate(&self, inode: *mut vfs_inode, new_size: loff_t) -> KResult<()> {
        TmpfsInode::truncate(inode, new_size)
    }
    unsafe fn destroy_inode(&self, inode: *mut vfs_inode) {
        Tmpfs::destroy_inode(inode);
    }
    unsafe fn free_inode(&self, inode: *mut vfs_inode) {
        super::superblock::TmpfsSuperblock::free_inode(inode);
    }
    unsafe fn open(&self, inode: *mut vfs_inode, file: *mut crate::bindings::vfs_file, f_flags: c_int) -> KResult<()> {
        super::file::TmpfsFile::open(inode, file, f_flags)
    }
}

pub(crate) static TMPFS_INODE_OPS: TmpfsInodeOps = TmpfsInodeOps;
