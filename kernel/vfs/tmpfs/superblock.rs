//! tmpfs superblock + filesystem-type registration — Rust port of
//! `kernel/vfs/tmpfs/superblock.c` (Phase 2 Wave 18, sub-wave A; see
//! `super` module doc and `docs/rustify/phase2_plan.md`).
//!
//! Owns the two slab caches backing every tmpfs inode/superblock
//! allocation, `struct vfs_superblock_ops tmpfs_superblock_ops`, the
//! `tmpfs` `vfs_fs_type` registration (`tmpfs_init`), and mounting tmpfs
//! as the (ephemeral, RAM-only) VFS root (`tmpfs_mount_root`).
//!
//! # Style notes (rust-skills)
//!
//! Follows the established per-file convention (`vfs/inode.rs`,
//! `vfs/fs.rs`, `vfs/file.rs`): every cross-module C-ABI symbol is
//! declared in a local `unsafe extern "C"` block rather than imported by
//! Rust path, even though [`super::inode`]/[`super::truncate`]/
//! [`super::file`] are Rust siblings in the same crate — the tmpfs-driver
//! internal calls between the four submodules of this wave (e.g.
//! `super::inode::TMPFS_INODE_OPS`, `super::truncate::tmpfs_truncate`)
//! *do* use plain Rust module paths, since those are genuinely the same
//! logical unit (`tmpfs.c`'s old single-translation-unit boundary, now
//! `mod tmpfs`), not a cross-C-TU call. `#[no_mangle]` is kept on every
//! symbol `tmpfs_private.h` declares `extern` (see the module doc) even
//! where today's only caller is another tmpfs submodule, so the header
//! stays an honest contract for Wave 20 (devtmpfs).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;

use crate::bindings::{
    hlist_entry_t, hlist_t, list_node_t, slab_cache_t, statfs, tmpfs_inode, tmpfs_sb_private,
    tmpfs_superblock, vfs_fs_type, vfs_inode, vfs_superblock,
    EINVAL, ENOMEM, PGSIZE, SLAB_FLAG_DEBUG_BITMAP, SLAB_FLAG_STATIC,
};
use crate::vfs::fs::{FsTypeOps, SuperblockOps};

use super::inode::TMPFS_INODE_OPS;
use super::{NAME_MAX, TMPFS_MAX_FILE_SIZE};

// ===========================================================================
// Native tmpfs superblock-side types — P3-N8 nativization (user
// directive: remove the C-compatible interfaces). `TmpfsSuperblock`/
// `TmpfsSbPrivate` are the canonical native definitions of
// `kernel/vfs/tmpfs/tmpfs_private.h`'s `struct tmpfs_superblock`/
// `struct tmpfs_sb_private`: `build.rs` blocklists the bindgen
// emissions and re-exports these types as
// `crate::bindings::tmpfs_superblock`/`tmpfs_sb_private` (facade
// `pub use`, N2 pattern). The C header stays unchanged (no C consumers
// remain — the kernel tree has zero `.c` files).
//
// DERIVE DECISIONS (P3-N8): `tmpfs_sb_private` derived Copy/Clone in
// the pre-nativization bindgen output (a single `uint64`) — kept.
// `tmpfs_superblock` derived neither (it embeds the NONCOPY
// `vfs_superblock` by value) — the native faithfully has no derives.
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + cross-compiler value probe (toolchain gcc,
// rv64gc/lp64d — scratchpad p3n8_probe_values.c); both agree on every
// value asserted below (no pipe-style divergence).
// ===========================================================================

/// Native `struct tmpfs_sb_private` (`kernel/vfs/tmpfs/tmpfs_private.h`)
/// — tmpfs-specific superblock state: the next inode number to allocate.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct TmpfsSbPrivate {
    pub next_ino: crate::bindings::uint64,
}

impl TmpfsSbPrivate {
    /// Mirrors `tmpfs_ino_alloc()`: return the next inode number and
    /// post-increment. Goal #1 method on the native tmpfs superblock-private
    /// struct (`&mut self` exclusive receiver — no shared-`&` freeze/noalias
    /// hazard; the C original's plain `next_ino++` under the alloc path).
    #[inline]
    fn ino_alloc(&mut self) -> u64 {
        let ino = self.next_ino;
        self.next_ino += 1;
        ino
    }
}

/// Native `struct tmpfs_superblock` (`kernel/vfs/tmpfs/tmpfs_private.h`).
#[repr(C, align(64))]
pub struct TmpfsSuperblock {
    pub vfs_sb: vfs_superblock,
    pub private_data: TmpfsSbPrivate,
}

// P3-N8 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc probe (see the module
// note above).
const _: () = {
    assert!(core::mem::size_of::<TmpfsSbPrivate>() == 8, "tmpfs_sb_private size");
    assert!(core::mem::align_of::<TmpfsSbPrivate>() == 8, "tmpfs_sb_private alignment");
    assert!(core::mem::offset_of!(TmpfsSbPrivate, next_ino) == 0, "tmpfs_sb_private.next_ino offset");
    assert!(core::mem::size_of::<TmpfsSuperblock>() == 1536, "tmpfs_superblock size");
    assert!(core::mem::align_of::<TmpfsSuperblock>() == 64, "tmpfs_superblock alignment");
    assert!(core::mem::offset_of!(TmpfsSuperblock, vfs_sb) == 0, "tmpfs_superblock.vfs_sb offset");
    assert!(
        core::mem::offset_of!(TmpfsSuperblock, private_data) == 1472,
        "tmpfs_superblock.private_data offset"
    );
};

// ===========================================================================
// Externs — see the module doc above for the convention.
// ===========================================================================

unsafe extern "C" {
    // printf.rs — C-variadic.

}

// P3-D3a: the slab entry points are genuinely `unsafe fn` in
// `crate::mm::slab` now that their `#[no_mangle]` exports are gone; this
// file's original extern declarations asserted `safe fn` (usual FFI
// facade) and typed the cache pointer as the bindgen `slab_cache_t`
// rather than `crate::mm::slab::SlabCache` (same layout — see
// `cffi::raw`'s identical note). Thin cast + safe-facade wrappers
// preserve both.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
    }
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    unsafe { crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}
/// SAFETY: `obj` must originate from `slab_alloc` above.
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_cache_shrink(cache: *mut slab_cache_t, nums: c_int) -> c_int {
    unsafe { crate::mm::slab_cache_shrink(cache as *mut crate::mm::slab::SlabCache, nums) }
}

// P3-1C mesh sweep: vfs/{fs,inode}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures). `vfs_root_inode` is fs.rs's single dummy
// VFS-root `vfs_inode` instance.
use crate::vfs::fs::{Vfs, VfsFsType, VfsSuperblock, vfs_root_inode};
use crate::vfs::inode::{vfs_chroot, vfs_ilock, vfs_iunlock, VfsInode};

// `kassert!`'s canonical home is `crate::kstd`/crate root (P3-CS1
// centralization). P3-10b (ops-table redesign): the CS13-era
// `extern "C"` wrappers + their `result_to_errptr` boundary
// conversions are GONE — the `KResult`-native bodies below are now
// dispatched directly through the `SuperblockOps`/`FsTypeOps` trait
// impls (and reused by devtmpfs's delegating impls); no ERR_PTR
// encoding remains anywhere in this driver.
use crate::kassert;
use crate::kstd::{Errno, KResult};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// ===========================================================================
// tmpfs Cache operations
// ===========================================================================

/// `static slab_cache_t __tmpfs_inode_cache`/`__tmpfs_sb_cache` —
/// zero-initialized at link time, real-initialized once by
/// [`__tmpfs_init_cache`] (called from [`tmpfs_init`]), same
/// `UnsafeCell<MaybeUninit<..>>` pattern as `vfs/pipe.rs`'s
/// `PipeSlabCell`/`vfs/file.rs`'s `FileSlabCell`.
#[repr(transparent)]
struct TmpfsSlabCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: written in full by `slab_cache_init` (called once, before any
// `slab_alloc`/`slab_free` on the cache) and otherwise only touched
// through the C slab allocator's own internally-synchronized entry
// points — same precedent as `vfs/pipe.rs`'s `PipeSlabCell`.
unsafe impl Sync for TmpfsSlabCell {}

static __TMPFS_INODE_CACHE: TmpfsSlabCell = TmpfsSlabCell(UnsafeCell::new(MaybeUninit::zeroed()));
static __TMPFS_SB_CACHE: TmpfsSlabCell = TmpfsSlabCell(UnsafeCell::new(MaybeUninit::zeroed()));

#[inline]
fn tmpfs_inode_cache() -> *mut slab_cache_t {
    __TMPFS_INODE_CACHE.0.get() as *mut slab_cache_t
}
#[inline]
fn tmpfs_sb_cache() -> *mut slab_cache_t {
    __TMPFS_SB_CACHE.0.get() as *mut slab_cache_t
}

fn __tmpfs_init_cache() -> c_int {
    let ret = slab_cache_init(
        tmpfs_inode_cache(),
        c"tmpfs_inode_cache".as_ptr() as *mut c_char,
        core::mem::size_of::<tmpfs_inode>(),
        (SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP) as u64,
    );
    if ret != 0 {
        return ret; // Failed to initialize tmpfs inode cache
    }
    slab_cache_init(
        tmpfs_sb_cache(),
        c"tmpfs_superblock_cache".as_ptr() as *mut c_char,
        core::mem::size_of::<tmpfs_superblock>(),
        (SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP) as u64,
    )
}

/// Shrink all tmpfs slab caches to release unused memory.
///
/// Kept `#[no_mangle]`/exported per `tmpfs_private.h`'s `extern`
/// declaration even though — like the C original — nothing calls it
/// today (its only caller, `tmpfs_smoketest.c`, was dead code and is
/// deleted with this port; see the module doc).
pub(crate) extern "C" fn tmpfs_shrink_caches() {
    slab_cache_shrink(tmpfs_inode_cache(), 0x7fffffff);
    slab_cache_shrink(tmpfs_sb_cache(), 0x7fffffff);
}

fn __tmpfs_alloc_inode_structure() -> *mut tmpfs_inode {
    let ti = slab_alloc(tmpfs_inode_cache()) as *mut tmpfs_inode;
    if ti.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `ti` is a freshly allocated, exclusively-owned
    // `tmpfs_inode`-sized block from the slab cache.
    unsafe {
        ptr::write_bytes(ti as *mut u8, 0, core::mem::size_of::<tmpfs_inode>());
        (*ti).vfs_inode.ops = Some(&TMPFS_INODE_OPS);
    }
    ti
}

/// Mirrors `tmpfs_alloc_inode()`. `pub(crate)`: reached through
/// [`SuperblockOps`] for tmpfs itself and reused directly by
/// devtmpfs's delegating trait impl (P3-10b).
pub(crate) fn tmpfs_alloc_inode_inner(sb: *mut vfs_superblock) -> KResult<*mut vfs_inode> {
    if sb.is_null() {
        return Err(Errno::Inval);
    }
    // SAFETY: non-null `sb`, `fs_data` checked before deref.
    let private_data = unsafe { (*sb).fs_data as *mut tmpfs_sb_private };
    if private_data.is_null() {
        return Err(Errno::Inval);
    }
    let ti = __tmpfs_alloc_inode_structure();
    if ti.is_null() {
        return Err(Errno::NoMem);
    }
    // SAFETY: caller (`tmpfs_alloc_inode`) holds a live superblock whose
    // `fs_data` was set to `&sb->private_data` by `tmpfs_alloc_superblock`.
    let ino = unsafe { (*private_data).ino_alloc() };
    // SAFETY: `ti` was just allocated above and is exclusively owned here.
    unsafe {
        (*ti).vfs_inode.ino = ino;
    }
    if ino == 0 {
        slab_free(ti as *mut c_void);
        return Err(Errno::NoEnt);
    }
    // Add the inode to the superblock's inode hash list -- done by the
    // generic `vfs_alloc_inode()` caller in `vfs/fs.rs`, not here (matches
    // the C original's own comment).
    // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode`;
    // `vfs_inode` is its first field (offset 0).
    Ok(unsafe { ptr::addr_of_mut!((*ti).vfs_inode) })
}

/// Mirrors `tmpfs_free_inode()`.
///
/// VFS guarantees inodes are truncated to zero (regular files) or empty
/// (directories) before this is called. `inode` must be a live
/// `vfs_inode` embedded in a `tmpfs_inode` allocated by
/// [`tmpfs_alloc_inode`] (`vfs_inode` is its first field, offset 0).
pub(crate) fn tmpfs_free_inode(inode: *mut vfs_inode) {
    let ti = inode as *mut tmpfs_inode;
    slab_free(ti as *mut c_void);
}

/// Mirrors `tmpfs_alloc_superblock()`.
pub(crate) extern "C" fn tmpfs_alloc_superblock() -> *mut tmpfs_superblock {
    let sb = slab_alloc(tmpfs_sb_cache()) as *mut tmpfs_superblock;
    if sb.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `sb` is a freshly allocated, exclusively-owned
    // `tmpfs_superblock`-sized block from the slab cache.
    unsafe {
        ptr::write_bytes(sb as *mut u8, 0, core::mem::size_of::<tmpfs_superblock>());
        (*sb).vfs_sb.flags.set_backendless(1); // tmpfs is backendless
        (*sb).vfs_sb.fs_data = ptr::addr_of_mut!((*sb).private_data) as *mut c_void;
    }
    sb
}

/// Mirrors `tmpfs_free()`. `sb` must be a live `vfs_superblock` embedded
/// in a `tmpfs_superblock` allocated by [`tmpfs_alloc_superblock`]
/// (`vfs_sb` is its first field, offset 0).
pub(crate) fn tmpfs_free_impl(sb: *mut vfs_superblock) {
    let tsb = sb as *mut tmpfs_superblock;
    slab_free(tsb as *mut c_void);
}

// ===========================================================================
// Superblock inode operations
// ===========================================================================

/// Mirrors `tmpfs_get_inode()`. tmpfs does not persist inodes, so it
/// cannot load an inode by number -- always `-ENOENT`. `pub(crate)`:
/// see [`tmpfs_alloc_inode_inner`].
pub(crate) fn tmpfs_get_inode_inner(sb: *mut vfs_superblock, _ino: u64) -> KResult<*mut vfs_inode> {
    if sb.is_null() {
        return Err(Errno::Inval);
    }
    Err(Errno::NoEnt)
}

/// Mirrors `tmpfs_sync_fs()`. tmpfs is an in-memory filesystem, nothing
/// to sync. `pub(crate)`: see [`tmpfs_alloc_inode_inner`].
pub(crate) fn tmpfs_sync_fs_impl(sb: *mut vfs_superblock, _wait: c_int) -> KResult<()> {
    // SAFETY: `sb` is a live `vfs_superblock` (caller's contract, same
    // as the `SuperblockOps::sync_fs` callback convention).
    unsafe { (*sb).flags.set_dirty(0) };
    Ok(())
}

/// `tmpfs_unmount_begin` — prepare tmpfs for unmount by evicting
/// unreferenced inodes.
///
/// For strict unmount to succeed, every cached inode with `ref_count ==
/// 0` must be evicted from the hash list: backendless filesystems keep
/// inodes alive in the cache as long as `n_links > 0`, so they need
/// explicit cleanup before unmounting.
///
/// Locking: caller holds the superblock write lock.
///
/// Walks `sb`'s generic inode hash list (`vfs_superblock.inodes`, NOT
/// tmpfs's own directory hash) via a local, non-RCU
/// first/next-entry walk mirroring `hlist_foreach_node_safe` (the C
/// macro precomputes `next` before running the loop body, so removing
/// `pos` mid-iteration is safe) -- reimplemented here rather than
/// shared with `inode.rs`'s directory-hash walk because the C original
/// never shared a function either (both are macro-expansion call
/// sites), matching this crate's established per-file self-contained
/// convention.
pub(crate) fn tmpfs_unmount_begin_impl(sb: *mut vfs_superblock) {
    if sb.is_null() {
        return;
    }
    // SAFETY: non-null `sb`, caller holds the superblock write lock.
    unsafe {
        let root_inode = (*sb).root_inode;
        let hlist = ptr::addr_of_mut!((*sb).inodes);

        let mut cur = hlist_first_entry(hlist);
        while !cur.is_null() {
            let next = hlist_next_entry(hlist, cur);
            // `hash_entry` is the first field of `vfs_inode` (offset 0).
            let inode = cur as *mut vfs_inode;

            // Skip root inode -- handled by vfs_unmount.
            if inode != root_inode {
                // Only evict inodes with no references.
                if (*inode).ref_count == 0 {
                    vfs_ilock(inode);
                    // Double-check ref_count under lock.
                    if (*inode).ref_count == 0 {
                        // Destroy the inode's data if it has any.
                        // (P3-10b: `destroy_inode`/`free_inode` are
                        // required trait methods — the old `None`-slot
                        // skips had no live instance.)
                        crate::vfs::inode::inode_ops(inode).destroy_inode(inode);
                        // Remove from hash and mark invalid.
                        (*inode).flags.set_valid(0);
                        VfsSuperblock::vfs_remove_inode(sb, inode);
                        vfs_iunlock(inode);
                        // Free the inode structure.
                        crate::vfs::inode::inode_ops(inode).free_inode(inode);
                    } else {
                        vfs_iunlock(inode);
                    }
                }
            }
            cur = next;
        }
    }
}

// `hlist_bucket_t` and `list_node_t` are both bindgen typedefs of the
// same underlying `struct list_node` (`kernel/inc/hlist_type.h`'s
// `typedef struct list_node hlist_bucket_t;`), so every helper below
// uses `list_node_t` uniformly for bucket heads -- no cast needed.

/// Mirrors `hlist->buckets + idx` (`hlist_t`'s own trailing
/// zero-length-array bucket storage).
///
/// # Safety
/// `hlist` must point to a live `hlist_t` with at least `idx + 1`
/// buckets.
unsafe fn hlist_bucket_at(hlist: *mut hlist_t, idx: u64) -> *mut list_node_t {
    unsafe { (*hlist).buckets.as_mut_ptr().add(idx as usize) }
}

/// Mirrors `hlist_next_bucket()`.
///
/// # Safety
/// `hlist` must point to a live `hlist_t`; `bucket` must be null or one
/// of `hlist`'s own bucket heads.
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

/// Mirrors `LIST_FIRST_NODE(bucket, hlist_entry_t, list_entry)`: the
/// first entry in one bucket's ring, or null if empty.
///
/// # Safety
/// `bucket` must point to a live, initialized `list_node_t` ring head.
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

/// Mirrors `hlist_first_entry()`.
///
/// # Safety
/// `hlist` must point to a live `hlist_t`.
unsafe fn hlist_first_entry(hlist: *mut hlist_t) -> *mut hlist_entry_t {
    unsafe {
        if hlist.is_null() {
            return ptr::null_mut();
        }
        // First non-empty bucket's first entry (`bucket_cnt` is a
        // loop-invariant field read once) -> range `.find()`.
        (0..(*hlist).bucket_cnt)
            .map(|i| unsafe { bucket_first_entry(hlist_bucket_at(hlist, i)) })
            .find(|e| !e.is_null())
            .unwrap_or(ptr::null_mut())
    }
}

/// Mirrors `hlist_next_entry()`.
///
/// # Safety
/// `hlist` must point to a live `hlist_t`; `entry` must be null or a
/// live entry currently linked into one of `hlist`'s buckets.
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

/// tmpfs's [`SuperblockOps`] implementor (P3-10b) — a zero-sized unit
/// struct whose single `static` instance ([`TMPFS_SUPERBLOCK_OPS`]) is
/// installed into every tmpfs superblock's `ops` as `Some(&STATIC)`.
/// The old table's `None` slots (`add_orphan`/`remove_orphan`/
/// `recover_orphans`/`begin_transaction`/`end_transaction`) are the
/// trait's do-nothing defaults.
pub struct TmpfsSuperblockOps;

impl SuperblockOps for TmpfsSuperblockOps {
    unsafe fn alloc_inode(&self, sb: *mut vfs_superblock) -> KResult<*mut vfs_inode> {
        tmpfs_alloc_inode_inner(sb)
    }
    unsafe fn get_inode(&self, sb: *mut vfs_superblock, ino: u64) -> KResult<*mut vfs_inode> {
        tmpfs_get_inode_inner(sb, ino)
    }
    unsafe fn sync_fs(&self, sb: *mut vfs_superblock, wait: c_int) -> KResult<()> {
        tmpfs_sync_fs_impl(sb, wait)
    }
    unsafe fn unmount_begin(&self, sb: *mut vfs_superblock) {
        tmpfs_unmount_begin_impl(sb);
    }
    unsafe fn statfs(&self, _sb: *mut vfs_superblock, buf: *mut statfs) -> KResult<()> {
        // SAFETY: `buf` is a live `statfs` (caller's contract, matches
        // the `SuperblockOps::statfs` callback convention).
        unsafe {
            (*buf).f_bsize = PGSIZE as u64;
            (*buf).f_frsize = PGSIZE as u64;
            (*buf).f_namelen = NAME_MAX;
        }
        Ok(())
    }
}

pub static TMPFS_SUPERBLOCK_OPS: TmpfsSuperblockOps = TmpfsSuperblockOps;

// ===========================================================================
// tmpfs filesystem type implementation
// ===========================================================================

/// tmpfs's [`FsTypeOps`] implementor (P3-10b). `mount` mirrors
/// `tmpfs_mount()` (the old `ret_sb` out-param + errno return became
/// `KResult<*mut vfs_superblock>`); `free` mirrors `tmpfs_free()`.
struct TmpfsFsTypeOps;

impl FsTypeOps for TmpfsFsTypeOps {
    unsafe fn mount(
        &self,
        mountpoint: *mut vfs_inode,
        device: *mut vfs_inode,
        _flags: c_int,
        _data: *const c_char,
    ) -> KResult<*mut vfs_superblock> {
        if mountpoint.is_null() {
            return Err(Errno::Inval);
        }
        if !device.is_null() {
            return Err(Errno::Inval); // tmpfs does not support device inode
        }
        let sb = tmpfs_alloc_superblock();
        if sb.is_null() {
            return Err(Errno::NoMem);
        }
        let root_inode = __tmpfs_alloc_inode_structure();
        if root_inode.is_null() {
            // SAFETY: `sb` is a live, exclusively-owned `tmpfs_superblock`;
            // `vfs_sb` is its first field (offset 0).
            tmpfs_free_impl(unsafe { ptr::addr_of_mut!((*sb).vfs_sb) });
            return Err(Errno::NoMem);
        }
        super::inode::tmpfs_make_directory(root_inode);
        // SAFETY: `sb`/`root_inode` are freshly allocated and exclusively
        // owned here.
        unsafe {
            (*root_inode).vfs_inode.ino = 1; // Root inode number is 1
            (*root_inode).vfs_inode.n_links = 2; // "." and ".." both point to self
            (*sb).vfs_sb.block_size = PGSIZE as usize;
            (*sb).vfs_sb.root_inode = ptr::addr_of_mut!((*root_inode).vfs_inode);
            (*sb).private_data.next_ino = 2;
            (*sb).vfs_sb.flags.set_backendless(1);
            (*sb).vfs_sb.ops = Some(&TMPFS_SUPERBLOCK_OPS);
            Ok(ptr::addr_of_mut!((*sb).vfs_sb))
        }
    }

    unsafe fn free(&self, sb: *mut vfs_superblock) {
        tmpfs_free_impl(sb);
    }
}

static TMPFS_FS_TYPE_OPS: TmpfsFsTypeOps = TmpfsFsTypeOps;

/// `tmpfs_init` - Initialize tmpfs filesystem type (caches and
/// registration).
///
/// This only initializes the tmpfs infrastructure. It does NOT mount
/// anything. Call this once during kernel initialization before any
/// tmpfs can be mounted.
pub(crate) extern "C" fn tmpfs_init() {
    let ret = __tmpfs_init_cache();
    kassert!(ret == 0, "tmpfs_init: __tmpfs_init_cache failed");

    let fs_type = VfsFsType::vfs_fs_type_allocate();
    kassert!(!fs_type.is_null(), "tmpfs_init: vfs_fs_type_allocate failed");
    // SAFETY: `fs_type` is freshly allocated and exclusively owned here.
    unsafe {
        (*fs_type).name = c"tmpfs".as_ptr();
        (*fs_type).ops = Some(&TMPFS_FS_TYPE_OPS);
    }

    Vfs::vfs_mount_lock();
    let ret = VfsFsType::vfs_register_fs_type(fs_type);
    kassert!(ret == 0, "tmpfs_init: vfs_register_fs_type failed");
    Vfs::vfs_mount_unlock();

    // SAFETY: `printf` is C-variadic; arguments match the `%lu %lu`/`%lu`
    // format strings exactly.
    unsafe {
        crate::kprintln!(
            "sizeof(tmpfs_inode)={}, TMPFS_INODE_EMBEDDED_DATA_LEN={}",
            core::mem::size_of::<tmpfs_inode>() as u64,
            super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN as u64,
        );
        crate::kprintln!(
            "tmpfs max file size={} bytes",
            super::TMPFS_MAX_FILE_SIZE as u64,
        );
    }
}

/// `tmpfs_mount_root` - Mount tmpfs as the root filesystem.
///
/// This mounts a fresh tmpfs instance at the VFS root inode and sets it
/// as the process root. Call this after [`tmpfs_init`] during early boot.
pub(crate) extern "C" fn tmpfs_mount_root() {
    Vfs::vfs_mount_lock();
    // SAFETY: `vfs_root_inode` is the crate-wide dummy VFS-root static
    // (`vfs/fs.rs`), always live.
    unsafe { vfs_ilock(ptr::addr_of_mut!(vfs_root_inode)) };

    // SAFETY: same as above.
    let ret = unsafe {
        VfsInode::vfs_mount(
            c"tmpfs".as_ptr(),
            ptr::addr_of_mut!(vfs_root_inode),
            ptr::null_mut(),
            0,
            ptr::null(),
        )
    };
    kassert!(ret == 0, "tmpfs_mount_root: vfs_mount failed");
    // On success, release locks. On failure, vfs_mount already released
    // them (matches the C original's own comment).
    if ret == 0 {
        unsafe { vfs_iunlock(ptr::addr_of_mut!(vfs_root_inode)) };
    }
    Vfs::vfs_mount_unlock();

    // SAFETY: same as above.
    let mnt_rooti = unsafe { vfs_root_inode.dev_mnt.mnt.mnt_rooti };
    let ret = vfs_chroot(mnt_rooti);
    kassert!(ret == 0, "tmpfs_mount_root: vfs_chroot failed");
}
