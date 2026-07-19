//! xv6fs superblock operations — Rust port of `kernel/vfs/xv6fs/superblock.c`
//! (Phase 2 Wave 19, sub-wave A; see `super` module doc and
//! `docs/rustify/phase2_plan.md`).
//!
//! Owns the two slab caches backing every xv6fs inode/superblock
//! allocation, the per-inode pcache `read_page`/`write_page` callbacks
//! (bmap + `bio` I/O — data blocks bypass the log, see the module doc),
//! `struct vfs_superblock_ops xv6fs_superblock_ops`, the `xv6fs`
//! `vfs_fs_type` registration (`xv6fs_init`), and mounting xv6fs at
//! `/root` + chroot (`xv6fs_mount_root` — **the boot-critical path this
//! wave must not break**).
//!
//! # Style notes (rust-skills)
//!
//! Follows the established per-file convention (`tmpfs/superblock.rs`):
//! calls into symbols outside the xv6fs driver (vfs core, mm, proc, lock,
//! string, the classic buffer cache) are declared in a local `unsafe
//! extern "C"` block. Calls into sibling xv6fs submodules ([`super::inode`],
//! [`super::log`], [`super::truncate`], [`super::block_cache`]) use plain
//! Rust-path imports instead — those are genuinely the same logical driver
//! unit, matching `tmpfs/superblock.rs`'s own precedent (e.g. its
//! `super::inode::tmpfs_make_directory` call). `#[no_mangle]` is kept on
//! every symbol `xv6fs_private.h`/`block_cache.h` declare `extern` (see the
//! module doc) even where today's only caller is a sibling xv6fs submodule,
//! so the headers stay honest C-ABI contracts. Ops-vtable globals
//! (`xv6fs_superblock_ops`/`xv6fs_fs_type_ops`) become plain Rust `static`s
//! instead (Wave 18 precedent — verified via full-tree grep that no C
//! translation unit outside this driver ever references them by their
//! original extern names, unlike `xv6fs_inode_ops`'s sibling-module
//! dispatch which stays cross-file Rust-only too).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;

use crate::bindings::{
    bio, blkdev_t, buf, dinode, page_t, pcache, pcache_ops, slab_cache_t, statfs,
    vfs_fs_type, vfs_inode, vfs_superblock, xv6fs_inode,
    xv6fs_superblock, EINVAL, EIO, ENOMEM, ENOSPC, PGSIZE,
};
use crate::vfs::fs::{FsTypeOps, SuperblockOps};

use super::inode::XV6FS_INODE_OPS;
// Sub-wave B siblings -- plain Rust-path imports, not `extern "C"`; see
// `truncate.rs`'s module doc ("Intra-driver calls") for the convention.
use super::block_cache::{xv6fs_bcache_destroy, xv6fs_bcache_init};
use super::log::{xv6fs_begin_op, xv6fs_end_op, xv6fs_initlog, xv6fs_log_write};
use super::truncate::xv6fs_bmap_read;
use super::{xv6fs_iblock, xv6fs_type_to_mode, FSMAGIC, IPB, ROOTINO};

// ===========================================================================
// Native on-disk `superblock` — P3-4a nativization (user directive:
// remove the C-compatible interfaces; on-disk format scrutiny class).
// `OndiskSuperblock` is the canonical KERNEL-SIDE definition of
// `kernel/inc/vfs/xv6fs/ondisk.h`'s `struct superblock`: `build.rs`
// blocklists the bindgen emission and re-exports this type as
// `crate::bindings::superblock` (facade `pub use`, N2 pattern).
//
// *** ON-DISK LAYOUT — HANDLE WITH P3-4 SCRUTINY *** The C header
// STAYS: `mkfs/mkfs.c` includes `ondisk.h` and writes fs.img on the
// HOST from it, so this native and that header are two independent
// spellings of one persistent format. `__xv6fs_read_superblock` below
// `memmove`s disk block 1's bytes straight into this struct (and
// `__xv6fs_write_superblock` writes them back), so any drift between
// the two spellings is silent on-disk corruption. The byte-exact
// asserts below pin the native to the header layout; the
// cross-compiler probe (see "Layout evidence") additionally proved the
// header lays out IDENTICALLY on the rv64gc target and the x86_64
// host — the mkfs-vs-kernel half of the contract.
//
// DERIVE DECISION (P3-4a): Copy + Clone, exactly as the
// pre-nativization bindgen output derived (8 plain `uint`s).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen form + cross-compiler value probe (toolchain gcc
// rv64gc/lp64d AND host x86_64 gcc — scratchpad p3_4a_ondisk_probe.c);
// all three agree on every value asserted below.
// ===========================================================================

/// Native on-disk `struct superblock` (`kernel/inc/vfs/xv6fs/ondisk.h`)
/// — block 1 of every xv6fs image, written by host-side `mkfs` and
/// describing the disk layout (log, inode blocks, free bitmap, data).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct OndiskSuperblock {
    /// Must be `FSMAGIC`.
    pub magic: crate::bindings::uint,
    /// Size of file system image (blocks).
    pub size: crate::bindings::uint,
    /// Number of data blocks.
    pub nblocks: crate::bindings::uint,
    /// Number of inodes.
    pub ninodes: crate::bindings::uint,
    /// Number of log blocks.
    pub nlog: crate::bindings::uint,
    /// Block number of first log block.
    pub logstart: crate::bindings::uint,
    /// Block number of first inode block.
    pub inodestart: crate::bindings::uint,
    /// Block number of first free map block.
    pub bmapstart: crate::bindings::uint,
}

// P3-4a hardcoded layout proof — the ON-DISK byte contract
// (`ondisk.h` `struct superblock`), every field. Values captured from
// the pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the two-arch gcc probe.
const _: () = {
    assert!(core::mem::size_of::<OndiskSuperblock>() == 32, "superblock size (ON-DISK)");
    assert!(core::mem::align_of::<OndiskSuperblock>() == 4, "superblock alignment");
    assert!(core::mem::offset_of!(OndiskSuperblock, magic) == 0, "sb.magic offset (ON-DISK)");
    assert!(core::mem::offset_of!(OndiskSuperblock, size) == 4, "sb.size offset (ON-DISK)");
    assert!(core::mem::offset_of!(OndiskSuperblock, nblocks) == 8, "sb.nblocks offset (ON-DISK)");
    assert!(core::mem::offset_of!(OndiskSuperblock, ninodes) == 12, "sb.ninodes offset (ON-DISK)");
    assert!(core::mem::offset_of!(OndiskSuperblock, nlog) == 16, "sb.nlog offset (ON-DISK)");
    assert!(core::mem::offset_of!(OndiskSuperblock, logstart) == 20, "sb.logstart offset (ON-DISK)");
    assert!(
        core::mem::offset_of!(OndiskSuperblock, inodestart) == 24,
        "sb.inodestart offset (ON-DISK)"
    );
    assert!(
        core::mem::offset_of!(OndiskSuperblock, bmapstart) == 28,
        "sb.bmapstart offset (ON-DISK)"
    );
};

// ===========================================================================
// Native `xv6fs_superblock` — P3-N8 nativization (user directive:
// remove the C-compatible interfaces). `Xv6fsSuperblock` is the
// canonical native definition of `kernel/vfs/xv6fs/xv6fs_private.h`'s
// `struct xv6fs_superblock`: `build.rs` blocklists the bindgen
// emission and re-exports this type as
// `crate::bindings::xv6fs_superblock` (facade `pub use`, N2 pattern).
//
// `disk_sb` (`struct superblock`, `kernel/inc/vfs/xv6fs/ondisk.h`) is
// the ON-DISK superblock record — native `OndiskSuperblock` above
// since P3-4a (it was still bindgen-emitted when this struct
// nativized in N8). `log`/`block_cache` are N8's own natives (via
// their facades).
//
// DERIVE DECISION (P3-N8): no derives — the bindgen emission derived
// neither Copy nor Clone (it embeds the NONCOPY `vfs_superblock` and
// `xv6fs_block_cache` by value); the native faithfully has no derives.
// `_pad0` reproduces bindgen's `__bindgen_padding_0` verbatim (places
// the cacheline-aligned `log` at 1536).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen form + cross-compiler value probe (toolchain gcc,
// rv64gc/lp64d — scratchpad p3n8_probe_values.c); both agree on every
// value asserted below (no pipe-style divergence).
// ===========================================================================

/// Native `struct xv6fs_superblock` (`kernel/vfs/xv6fs/xv6fs_private.h`)
/// — one mounted xv6fs instance: the generic VFS superblock, the cached
/// copy of the on-disk superblock, the backing block device, the
/// per-mount log, and the free-block extent cache.
#[repr(C, align(64))]
pub struct Xv6fsSuperblock {
    pub vfs_sb: vfs_superblock,
    /// Copy of the on-disk superblock (native [`OndiskSuperblock`]
    /// since P3-4a — ON-DISK record, P3-4 scrutiny class).
    pub disk_sb: OndiskSuperblock,
    pub blkdev: *mut blkdev_t,
    pub dirty: core::ffi::c_int,
    pub(crate) _pad0: [u64; 2],
    /// Per-mount write-ahead log control state. N-R7b: lock-owns-data —
    /// was `xv6fs_log` (a plain struct with an inline `spinlock_t` field
    /// hand-guarded by `begin_op`/`end_op`); now a data-owning
    /// [`SpinLock<LogInner>`](super::log::LogInner). This is an IN-MEMORY
    /// mount struct only (allocated from the `xv6fs_sb` slab, never
    /// written to disk at this layout — the on-disk superblock is the
    /// separate 32-byte [`OndiskSuperblock`] in `disk_sb`), so retyping
    /// `log` is layout-internal: `SpinLock<LogInner>` (align 8, ~1056 B)
    /// still starts at 1536, and the align-64 `block_cache` still lands
    /// at 2624, leaving the size/offset asserts below unchanged.
    pub log: crate::sync::SpinLock<super::log::LogInner>,
    /// Per-mount free-block extent cache. N-R7d: lock-owns-data — the
    /// lock-guarded metadata (rb-tree + bookkeeping) is now a data-owning
    /// [`SpinLock<BlockCacheInner>`](super::block_cache::BlockCacheInner)
    /// inside [`Xv6fsBlockCache`](super::block_cache::Xv6fsBlockCache),
    /// alongside two lock-free siblings: the `initialized` lifecycle flag
    /// (read without the lock by `__xv6fs_balloc` and `statfs`) and the
    /// `extent_cache` slab (independently synchronised by the slab
    /// allocator's own internal lock). The lock-owning struct is laid out to
    /// occupy exactly the historical 1472-byte slot, so `block_cache` still
    /// lands at 2624 and this IN-MEMORY mount struct stays 4096 (required:
    /// the `xv6fs_sb` slab caps objects at one page — `SLAB_OBJ_MAX_SIZE`).
    pub block_cache: crate::bindings::xv6fs_block_cache,
}

// P3-N8 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc probe (see the module
// note above).
const _: () = {
    assert!(core::mem::size_of::<Xv6fsSuperblock>() == 4096, "xv6fs_superblock size");
    assert!(core::mem::align_of::<Xv6fsSuperblock>() == 64, "xv6fs_superblock alignment");
    assert!(core::mem::offset_of!(Xv6fsSuperblock, vfs_sb) == 0, "xsb.vfs_sb offset");
    assert!(core::mem::offset_of!(Xv6fsSuperblock, disk_sb) == 1472, "xsb.disk_sb offset");
    assert!(core::mem::offset_of!(Xv6fsSuperblock, blkdev) == 1504, "xsb.blkdev offset");
    assert!(core::mem::offset_of!(Xv6fsSuperblock, dirty) == 1512, "xsb.dirty offset");
    assert!(core::mem::offset_of!(Xv6fsSuperblock, log) == 1536, "xsb.log offset");
    assert!(core::mem::offset_of!(Xv6fsSuperblock, block_cache) == 2624, "xsb.block_cache offset");
};

// ===========================================================================
// Externs — see the module doc above for the convention.
// ===========================================================================

unsafe extern "C" {
    // printf.rs — C-variadic.

    // string.rs.
    safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

}

// P3-D3c: `bufcache.rs`'s entry points are plain (safe) Rust fns now that
// their `#[no_mangle]` exports are gone; identical signatures, plain `use`.
use crate::bufcache::{bread, brelse, bwrite};

// P3-D3a: `pcache_init`/`xv6_page_pcache_get_node` (mm/pcache.rs) are
// ordinary (safe) Rust fns now that their `#[no_mangle]` exports are
// gone; identical signatures, plain `use`.
use crate::mm::{pcache_init, xv6_page_pcache_get_node};

// The slab entry points are genuinely `unsafe fn` in `crate::mm::slab`;
// this file's original extern declarations asserted `safe fn` (usual FFI
// facade) and typed the cache pointer as the bindgen `slab_cache_t`
// rather than `crate::mm::slab::SlabCache` (same layout — see
// `cffi::raw`'s identical note) and `name` as `*const c_char` rather
// than the real `*mut c_char` (the callee only reads it). Thin cast +
// safe-facade wrappers preserve all of this file's call-site types.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *const c_char, obj_size: usize, flags: u64,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(
            cache as *mut crate::mm::slab::SlabCache,
            name as *mut c_char,
            obj_size,
            flags,
        )
    }
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_cache_shrink(cache: *mut slab_cache_t, nums: c_int) -> c_int {
    unsafe { crate::mm::slab_cache_shrink(cache as *mut crate::mm::slab::SlabCache, nums) }
}
/// SAFETY: see `slab_cache_shrink` above.
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    unsafe { crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}
/// SAFETY: `obj` must originate from `slab_alloc` above.
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}
// P3-1D mesh sweep: dev/bio.rs + dev/blkdev.rs (block I/O submission
// path) are in scope for this wave; signatures are identical, so these
// become plain crate-path imports instead of `extern "C"` redeclarations.
use crate::dev::bio::{bio_add_seg, bio_alloc};
use crate::dev::blkdev::{blkdev_get, blkdev_put, blkdev_submit_bio};
// P3-D3b: lock/completion.rs's wait entry points are plain safe Rust fns
// now that their `#[no_mangle]` exports are gone; reached by crate path.
use crate::lock::completion::{wait_for_completion, wait_for_completion_interruptible};
// P3-9b: `KArc<bio>` replaces the manual `bio_alloc`/`bio_release` pairing
// in `xv6fs_pcache_read_page`/`xv6fs_pcache_write_page` below -- see
// `kernel/kobject.rs`'s `KArc` doc and `kernel/dev/bio.rs`'s `HasKobject`
// impl (P3-9's device family precedent, applied to the highest
// leak-density call site flagged by that wave).
use crate::kobject::KArc;

// P3-1C mesh sweep: vfs/{fs,inode}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures -- `mode_t`/`dev_t` are transparent `u32` type
// aliases, so the former `u32`-typed params are unaffected). `vfs_root_inode`
// is fs.rs's single dummy VFS-root `vfs_inode` instance.
use crate::vfs::fs::{
    vfs_fs_type_allocate, vfs_mount, vfs_mount_lock, vfs_mount_unlock, vfs_register_fs_type,
    vfs_root_inode, vfs_superblock_unlock, vfs_superblock_wlock,
};
use crate::vfs::inode::{vfs_chroot, vfs_ilock, vfs_iput, vfs_iunlock};

// `kassert!`/`is_err`/`is_err_or_null`'s canonical homes are
// `crate::kstd`/crate root (P3-CS1 centralization). P3-CS12: the two
// ERR_PTR-returning superblock-ops vtable methods (`xv6fs_alloc_inode`/
// `xv6fs_get_inode`) now build a `KResult<*mut vfs_inode>` internally and
// encode it to the ABI-fixed ERR_PTR exactly once at the `extern "C"`
// boundary via `result_to_errptr`; only the error-return encoding changed,
// the on-disk I/O / bread-brelse pairing is byte-identical.
use crate::kassert;
use crate::kstd::{is_err, is_err_or_null, ptr_err, Errno, KResult};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// Mirrors `mkdev(m, n)` (`kernel/inc/defs.h`) — hardcoded locally per
/// this crate's established per-file convention for small macros (same
/// value/rationale as every other `mkdev`/`major`/`minor` reimplementation
/// in this crate, e.g. `vfs/inode.rs`).
#[inline(always)]
const fn mkdev(major: i32, minor: i32) -> u32 {
    ((major << 20) | minor) as u32
}
/// Mirrors `major(dev)`.
#[inline(always)]
fn major(dev: u32) -> i32 {
    ((dev >> 20) & 0xFFF) as i32
}
/// Mirrors `minor(dev)`.
#[inline(always)]
fn minor(dev: u32) -> i32 {
    (dev & 0xFFFFF) as i32
}
/// `ROOTDEV` (`param.h`, `mkdev(2, 1)`) — virtio disk, xv6fs's fallback
/// root device.
const ROOTDEV: u32 = mkdev(2, 1);
/// `RAMDISK_DEV` (`param.h`, `mkdev(3, 1)`) — xv6fs's preferred root
/// device.
const RAMDISK_DEV: u32 = mkdev(3, 1);

/// Mirrors `xv6fs_sb_dev()` (`xv6fs_private.h`, macro).
///
/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock` with a non-null
/// `blkdev`.
#[inline(always)]
unsafe fn xv6fs_sb_dev(xv6_sb: *mut xv6fs_superblock) -> u32 {
    unsafe {
        let bdev = (*xv6_sb).blkdev;
        mkdev((*bdev).dev.major, (*bdev).dev.minor)
    }
}

// ===========================================================================
// Per-inode page cache operations for file data
//
// The pcache is keyed by logical file offset in 512-byte units. One
// pcache page (4KB) covers BSIZE_PER_PAGE (4) xv6fs blocks. read_page
// translates logical block -> physical via bmap, then reads via bio.
// write_page does the same then writes via bio (data=writeback: data
// blocks bypass the xv6fs log).
// ===========================================================================

/// `BSIZE_PER_PAGE` (`PGSIZE / BSIZE`) — 4.
const BSIZE_PER_PAGE: u32 = PGSIZE as u32 / super::BSIZE;
/// `BLK512_PER_BSIZE` (`BSIZE / 512`) — 2.
const BLK512_PER_BSIZE: u64 = super::BSIZE as u64 / 512;

/// Mirrors `bio_await()` (`dev/bio.h`, `static inline` -- no external
/// linkage, reimplemented here per this crate's established convention
/// for non-exported `static inline` helpers).
///
/// # Safety
/// `bio` must point to a live, submitted `struct bio`.
unsafe fn bio_await(bio: *mut bio) -> c_int {
    unsafe {
        let ret = wait_for_completion_interruptible(ptr::addr_of_mut!((*bio).io_completion));
        if ret == neg(crate::bindings::EINTR) {
            // Signal received but I/O is in flight -- must let it finish
            // so the bio/buffer resources are safe to release.
            wait_for_completion(ptr::addr_of_mut!((*bio).io_completion));
            return if (*bio).error != 0 { (*bio).error } else { neg(crate::bindings::EINTR) };
        }
        (*bio).error
    }
}

extern "C" fn xv6fs_pcache_read_page(pc: *mut pcache, page: *mut page_t) -> c_int {
    // SAFETY: `pc`/`page` are live (caller's contract, matches the C
    // `pcache_ops.read_page` callback convention); `private_data` was set
    // to a live `vfs_inode` by `xv6fs_inode_pcache_init`.
    unsafe {
        let inode = (*pc).private_data as *mut vfs_inode;
        let ip = inode as *mut xv6fs_inode;
        let xv6_sb = (*inode).sb as *mut xv6fs_superblock;
        let pcnode = xv6_page_pcache_get_node(page);
        let base_bn = (*pcnode).blkno / BLK512_PER_BSIZE;

        for i in 0..BSIZE_PER_PAGE as u64 {
            let addr = xv6fs_bmap_read(ip, (base_bn + i) as u32);
            if addr == 0 {
                memset(((*pcnode).data as *mut u8).add((i * super::BSIZE as u64) as usize) as *mut c_void, 0, super::BSIZE as usize);
                continue;
            }

            let b = bio_alloc(xv6_sb_blkdev(xv6_sb), 1, false as crate::bindings::bool_, None, ptr::null_mut());
            if is_err_or_null(b) {
                return neg(ENOMEM);
            }
            // SAFETY: `bio_alloc`'s success postcondition (one held
            // kobject reference) is exactly `KArc::from_raw`'s
            // precondition -- see `kernel/dev/bio.rs`'s `HasKobject`
            // impl. `b` (the KArc) now owns that reference; every exit
            // path below (the two error `return`s and the fallthrough to
            // the next loop iteration) drops it automatically, replacing
            // the three manual `bio_release(b)` calls the pre-P3-9b code
            // needed on this same set of paths.
            let b = KArc::<bio>::from_raw(b);
            let bp = KArc::as_ptr(&b);
            (*bp).blkno = addr as u64 * BLK512_PER_BSIZE;
            let ret = bio_add_seg(bp, page, 0, super::BSIZE as u16, (i * super::BSIZE as u64) as u16);
            if ret != 0 {
                return ret; // `b` drops here, releasing the reference.
            }
            let ret = blkdev_submit_bio(xv6_sb_blkdev(xv6_sb), bp);
            if ret != 0 {
                return ret; // `b` drops here, releasing the reference.
            }
            let ret = bio_await(bp);
            if ret != 0 {
                return ret; // `b` drops here (release either way).
            }
            // `b` drops at the end of this loop iteration -- releases the
            // reference exactly where the old unconditional
            // `bio_release(b)` after `bio_await` ran.
        }
        0
    }
}

extern "C" fn xv6fs_pcache_write_page(pc: *mut pcache, page: *mut page_t) -> c_int {
    // SAFETY: `pc`/`page` are live (caller's contract).
    unsafe {
        let inode = (*pc).private_data as *mut vfs_inode;
        // The flush worker may run concurrently with inode teardown. If the
        // inode has already been detached from its superblock
        // (inode->sb == NULL), skip the writeback -- the data is about to
        // be truncated anyway.
        if inode.is_null() || (*inode).sb.is_null() {
            return 0;
        }

        let ip = inode as *mut xv6fs_inode;
        let xv6_sb = (*inode).sb as *mut xv6fs_superblock;
        let pcnode = xv6_page_pcache_get_node(page);
        let base_bn = (*pcnode).blkno / BLK512_PER_BSIZE;

        for i in 0..BSIZE_PER_PAGE as u64 {
            let addr = xv6fs_bmap_read(ip, (base_bn + i) as u32);
            if addr == 0 {
                continue; // Sparse / beyond file -- nothing to write back.
            }

            let b = bio_alloc(xv6_sb_blkdev(xv6_sb), 1, true as crate::bindings::bool_, None, ptr::null_mut());
            if is_err_or_null(b) {
                return neg(ENOMEM);
            }
            // SAFETY: see the identical comment in
            // `xv6fs_pcache_read_page` above -- same postcondition, same
            // `HasKobject` impl.
            let b = KArc::<bio>::from_raw(b);
            let bp = KArc::as_ptr(&b);
            (*bp).blkno = addr as u64 * BLK512_PER_BSIZE;
            let ret = bio_add_seg(bp, page, 0, super::BSIZE as u16, (i * super::BSIZE as u64) as u16);
            if ret != 0 {
                return ret; // `b` drops here, releasing the reference.
            }
            let ret = blkdev_submit_bio(xv6_sb_blkdev(xv6_sb), bp);
            if ret != 0 {
                return ret; // `b` drops here, releasing the reference.
            }
            let ret = bio_await(bp);
            if ret != 0 {
                return ret; // `b` drops here (release either way).
            }
            // `b` drops at the end of this loop iteration -- releases the
            // reference exactly where the old unconditional
            // `bio_release(b)` after `bio_await` ran.
        }
        0
    }
}

/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock`.
#[inline(always)]
unsafe fn xv6_sb_blkdev(xv6_sb: *mut xv6fs_superblock) -> *mut blkdev_t {
    unsafe { (*xv6_sb).blkdev }
}

static XV6FS_PCACHE_OPS: pcache_ops = pcache_ops {
    read_page: Some(xv6fs_pcache_read_page),
    write_page: Some(xv6fs_pcache_write_page),
    write_begin: None,
    write_end: None,
    mark_dirty: None,
};

/// Initialise the embedded per-inode pcache (`i_data`). Call once for
/// every regular-file inode after its mode is known.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_inode_pcache_init(inode: *mut vfs_inode) {
    // SAFETY: `inode` is live (caller's contract).
    unsafe {
        if !super::s_isreg((*inode).mode) {
            return;
        }
        let pc = ptr::addr_of_mut!((*inode).i_data);
        ptr::write_bytes(pc, 0, 1);
        (*pc).ops = ptr::addr_of!(XV6FS_PCACHE_OPS) as *mut pcache_ops;
        // blk_count in 512-byte units, rounded up to page boundary (8 blocks).
        let maxfile_blk512 = super::MAXFILE as u64 * BLK512_PER_BSIZE;
        (*pc).blk_count = (maxfile_blk512 + 7) & !7u64;

        let ret = pcache_init(pc);
        if ret != 0 {
            return; // proceed without pcache
        }
        // pcache_init resets private_data, so set it after init.
        (*pc).private_data = inode as *mut c_void;
    }
}

// ===========================================================================
// Slab cache initialization
// ===========================================================================

/// `static slab_cache_t __xv6fs_sb_cache` -- zero-initialized at link
/// time, real-initialized once by [`__xv6fs_init_cache`] (called from
/// [`xv6fs_init`]). Same `UnsafeCell<MaybeUninit<..>>` pattern as
/// `tmpfs/superblock.rs`'s `TmpfsSlabCell`.
#[repr(transparent)]
struct Xv6fsSlabCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: written in full by `slab_cache_init` (called once, before any
// `slab_alloc`/`slab_free` on the cache) and otherwise only touched
// through the C slab allocator's own internally-synchronized entry
// points -- same precedent as `tmpfs/superblock.rs`'s `TmpfsSlabCell`.
unsafe impl Sync for Xv6fsSlabCell {}

static __XV6FS_SB_CACHE: Xv6fsSlabCell = Xv6fsSlabCell(UnsafeCell::new(MaybeUninit::zeroed()));

#[inline]
fn xv6fs_sb_cache() -> *mut slab_cache_t {
    __XV6FS_SB_CACHE.0.get() as *mut slab_cache_t
}

/// `extern slab_cache_t __xv6fs_inode_cache;` -- declared `extern` by
/// `xv6fs_private.h` (kept as real backing storage for ABI honesty, same
/// precedent as `vfs/fs.rs`'s `vfs_root_inode`), even though no C
/// translation unit outside this driver references it today (verified by
/// full-tree grep).
pub(crate) static mut __xv6fs_inode_cache: slab_cache_t = unsafe { core::mem::zeroed() };

#[inline]
fn xv6fs_inode_cache() -> *mut slab_cache_t {
    &raw mut __xv6fs_inode_cache
}

fn __xv6fs_init_cache() -> c_int {
    let ret = slab_cache_init(
        xv6fs_inode_cache(),
        c"xv6fs_inode".as_ptr(),
        core::mem::size_of::<xv6fs_inode>(),
        (crate::bindings::SLAB_FLAG_STATIC | crate::bindings::SLAB_FLAG_DEBUG_BITMAP) as u64,
    );
    if ret != 0 {
        return ret;
    }
    slab_cache_init(
        xv6fs_sb_cache(),
        c"xv6fs_sb".as_ptr(),
        core::mem::size_of::<xv6fs_superblock>(),
        (crate::bindings::SLAB_FLAG_STATIC | crate::bindings::SLAB_FLAG_DEBUG_BITMAP) as u64,
    )
}

/// Shrink xv6fs slab caches to release unused pages.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_shrink_caches() {
    slab_cache_shrink(xv6fs_inode_cache(), 0x7fffffff);
    slab_cache_shrink(xv6fs_sb_cache(), 0x7fffffff);
}

// ===========================================================================
// Superblock read/write helpers
// ===========================================================================

fn __xv6fs_read_superblock(dev: u32, disk_sb: *mut OndiskSuperblock) -> KResult<()> {
    let bp = bread(dev, 1);
    if bp.is_null() {
        return Err(Errno::Io);
    }
    // SAFETY: `bp` is a live, just-read buffer; `disk_sb` is a valid
    // exclusively-owned out-param.
    unsafe {
        memmove(disk_sb as *mut c_void, (*bp).data as *const c_void, core::mem::size_of::<OndiskSuperblock>());
        brelse(bp);
        if (*disk_sb).magic != FSMAGIC {
            return Err(Errno::Inval);
        }
    }
    Ok(())
}

fn __xv6fs_write_superblock(dev: u32, disk_sb: *mut OndiskSuperblock) -> KResult<()> {
    let bp = bread(dev, 1);
    if bp.is_null() {
        return Err(Errno::Io);
    }
    // SAFETY: `bp` is a live, just-read buffer; `disk_sb` is live.
    unsafe {
        memmove((*bp).data as *mut c_void, disk_sb as *const c_void, core::mem::size_of::<OndiskSuperblock>());
        bwrite(bp);
        brelse(bp);
    }
    Ok(())
}

// ===========================================================================
// Inode allocation
// ===========================================================================

fn __xv6fs_alloc_inode_structure() -> *mut xv6fs_inode {
    let xi = slab_alloc(xv6fs_inode_cache()) as *mut xv6fs_inode;
    if xi.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `xi` is a freshly allocated, exclusively-owned
    // `xv6fs_inode`-sized block from the slab cache.
    unsafe {
        ptr::write_bytes(xi as *mut u8, 0, core::mem::size_of::<xv6fs_inode>());
        (*xi).vfs_inode.ops = Some(&XV6FS_INODE_OPS);
    }
    xi
}

/// Mirrors `xv6fs_alloc_inode()` (P3-10b: dispatched directly through
/// [`SuperblockOps`]; the old `extern "C"` + `result_to_errptr` wrapper
/// is gone).
fn xv6fs_alloc_inode_inner(sb: *mut vfs_superblock) -> KResult<*mut vfs_inode> {
    if sb.is_null() {
        return Err(Errno::Inval);
    }
    let xv6_sb = sb as *mut xv6fs_superblock;
    // SAFETY: `xv6_sb` is a live, exclusively-accessed superblock (caller
    // holds the superblock write lock, matching the C original's
    // documented lock-order contract).
    unsafe {
        let disk_sb = ptr::addr_of_mut!((*xv6_sb).disk_sb);
        let dev = xv6fs_sb_dev(xv6_sb);

        let mut inum: u64 = 1;
        while inum < (*disk_sb).ninodes as u64 {
            let bp = bread(dev, xv6fs_iblock(inum, (*disk_sb).inodestart));
            if bp.is_null() {
                return Err(Errno::Io);
            }
            let dip = ((*bp).data as *mut dinode).add((inum % IPB) as usize);
            if (*dip).type_ == 0 {
                // Found a free inode.
                ptr::write_bytes(dip as *mut u8, 0, core::mem::size_of::<dinode>());
                // Mark as allocated but type will be set by caller.
                xv6fs_log_write(xv6_sb, bp);
                brelse(bp);

                let xi = __xv6fs_alloc_inode_structure();
                if xi.is_null() {
                    return Err(Errno::NoMem);
                }
                (*xi).dev = dev;
                (*xi).vfs_inode.ino = inum;
                // Note: Do NOT set vfs_inode.sb here -- VFS will set it in
                // vfs_add_inode.
                (*xi).vfs_inode.ref_count = 1;

                return Ok(ptr::addr_of_mut!((*xi).vfs_inode));
            }
            brelse(bp);
            inum += 1;
        }
    }

    Err(Errno::NoSpc)
}

// ===========================================================================
// Get inode from disk
// ===========================================================================

/// Mirrors `xv6fs_get_inode()` (P3-10b: dispatched directly through
/// [`SuperblockOps`]; the old `extern "C"` + `result_to_errptr` wrapper
/// is gone).
fn xv6fs_get_inode_inner(sb: *mut vfs_superblock, ino: u64) -> KResult<*mut vfs_inode> {
    if sb.is_null() || ino == 0 {
        return Err(Errno::Inval);
    }
    let xv6_sb = sb as *mut xv6fs_superblock;
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe {
        let disk_sb = ptr::addr_of_mut!((*xv6_sb).disk_sb);
        let dev = xv6fs_sb_dev(xv6_sb);

        if ino >= (*disk_sb).ninodes as u64 {
            return Err(Errno::NoEnt);
        }

        let bp = bread(dev, xv6fs_iblock(ino, (*disk_sb).inodestart));
        if bp.is_null() {
            return Err(Errno::Io);
        }

        let dip = ((*bp).data as *mut dinode).add((ino % IPB) as usize);
        if (*dip).type_ == 0 {
            brelse(bp);
            return Err(Errno::NoEnt);
        }

        let xi = __xv6fs_alloc_inode_structure();
        if xi.is_null() {
            brelse(bp);
            return Err(Errno::NoMem);
        }

        (*xi).dev = dev;
        (*xi).vfs_inode.ino = ino;
        // Note: Do NOT set vfs_inode.sb here -- VFS will set it when
        // adding to hash.
        (*xi).vfs_inode.ref_count = 1;
        (*xi).vfs_inode.mode = xv6fs_type_to_mode((*dip).type_);
        (*xi).vfs_inode.n_links = (*dip).nlink as u32;
        (*xi).vfs_inode.size = (*dip).size as i64;

        (*xi).major = (*dip).major;
        (*xi).minor = (*dip).minor;
        (*xi).addrs = (*dip).addrs;

        // For device inodes, set the appropriate device number field.
        if (*dip).type_ == super::XV6FS_T_BLKDEVICE {
            let devno = mkdev((*xi).major as i32, (*xi).minor as i32);
            (*xi).vfs_inode.dev_mnt.bdev = devno;
        } else if (*dip).type_ == super::XV6FS_T_CDEVICE {
            let devno = mkdev((*xi).major as i32, (*xi).minor as i32);
            (*xi).vfs_inode.dev_mnt.cdev = devno;
        }

        brelse(bp);
        Ok(ptr::addr_of_mut!((*xi).vfs_inode))
    }
}

// ===========================================================================
// Sync operations
// ===========================================================================

/// Mirrors `xv6fs_sync_fs()` (P3-10b: `KResult`-native, dispatched
/// through [`SuperblockOps`] and called directly by `unmount_begin`).
fn xv6fs_sync_fs_impl(sb: *mut vfs_superblock, _wait: c_int) -> KResult<()> {
    if sb.is_null() {
        return Err(Errno::Inval);
    }
    let xv6_sb = sb as *mut xv6fs_superblock;
    // SAFETY: `xv6_sb`/`sb` are live (caller's contract).
    unsafe {
        if (*xv6_sb).dirty != 0 {
            __xv6fs_write_superblock(xv6fs_sb_dev(xv6_sb), ptr::addr_of_mut!((*xv6_sb).disk_sb))?;
            (*xv6_sb).dirty = 0;
        }
        (*sb).flags.set_dirty(0);
    }
    Ok(())
}

// ===========================================================================
// Mount/Free operations
// ===========================================================================

/// xv6fs's [`FsTypeOps`] implementor (P3-10b). `mount` mirrors
/// `xv6fs_mount()` (the old `ret_sb` out-param + errno return became
/// `KResult<*mut vfs_superblock>`); `free` mirrors `xv6fs_free()`.
struct Xv6fsFsTypeOps;

impl FsTypeOps for Xv6fsFsTypeOps {
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

    // Get the block device from the device inode. The device inode's
    // bdev field contains the device number (major:minor). If no device
    // inode is provided, xv6fs does not support that (unlike tmpfs).
    // SAFETY: `device` checked non-null before deref.
    let dev_num = unsafe {
        if device.is_null() || !super::s_isblk((*device).mode) {
            return Err(Errno::Inval); // xv6fs does not support block device inode
        }
        (*device).dev_mnt.bdev
    };

    // `blkdev_get` is a dev-layer ERR_PTR boundary this driver does not
    // own — decode it once here (`Errno::Raw` passthrough, kstd's
    // sanctioned cross-family case).
    let blkdev = blkdev_get(major(dev_num), minor(dev_num));
    if is_err(blkdev) {
        return Err(Errno::Raw(ptr_err(blkdev)));
    }

    let xv6_sb = slab_alloc(xv6fs_sb_cache()) as *mut xv6fs_superblock;
    if xv6_sb.is_null() {
        blkdev_put(blkdev);
        return Err(Errno::NoMem);
    }
    // SAFETY: `xv6_sb` is a freshly allocated, exclusively-owned block.
    unsafe {
        ptr::write_bytes(xv6_sb as *mut u8, 0, core::mem::size_of::<xv6fs_superblock>());
        (*xv6_sb).blkdev = blkdev;

        if let Err(e) = __xv6fs_read_superblock(xv6fs_sb_dev(xv6_sb), ptr::addr_of_mut!((*xv6_sb).disk_sb)) {
            blkdev_put(blkdev);
            slab_free(xv6_sb as *mut c_void);
            return Err(e);
        }

        (*xv6_sb).dirty = 0;

        // Initialize logging layer.
        xv6fs_initlog(xv6_sb);

        // Initialize block allocation cache.
        let ret = xv6fs_bcache_init(xv6_sb);
        if ret != 0 {
            crate::kprintln!(
                "xv6fs: warning: block cache init failed ({}), using fallback",
                ret,
            );
            // Don't fail mount -- the fallback linear scan will still work.
        }

        // Initialize VFS superblock.
        (*xv6_sb).vfs_sb.block_size = super::BSIZE as usize;
        (*xv6_sb).vfs_sb.total_blocks = (*xv6_sb).disk_sb.size as u64;
        // xv6fs is a backend filesystem -- inodes can be evicted from
        // cache when refcount reaches 0 since they can be re-read from
        // disk. Root inodes and mountpoint inodes are protected in
        // vfs_iput.
        (*xv6_sb).vfs_sb.flags.set_backendless(0);
        (*xv6_sb).vfs_sb.ops = Some(&XV6FS_SUPERBLOCK_OPS);
        (*xv6_sb).vfs_sb.fs_data = xv6_sb as *mut c_void;

        // Load root inode (inode 1 in xv6). (P3-10b: `KResult`-native;
        // the old dead null -> `ENOMEM` branch had no producer.)
        let root_inode = match xv6fs_get_inode_inner(ptr::addr_of_mut!((*xv6_sb).vfs_sb), ROOTINO) {
            Ok(i) => i,
            Err(e) => {
                blkdev_put(blkdev);
                slab_free(xv6_sb as *mut c_void);
                return Err(e);
            }
        };

        (*xv6_sb).vfs_sb.root_inode = root_inode;

        Ok(ptr::addr_of_mut!((*xv6_sb).vfs_sb))
    }
    }

    /// Mirrors `xv6fs_free()`.
    unsafe fn free(&self, sb: *mut vfs_superblock) {
        let xv6_sb = sb as *mut xv6fs_superblock;
        // SAFETY: `xv6_sb` is live and being torn down (caller's contract).
        unsafe {
            xv6fs_bcache_destroy(xv6_sb);
            if !(*xv6_sb).blkdev.is_null() {
                blkdev_put((*xv6_sb).blkdev);
            }
        }
        slab_free(xv6_sb as *mut c_void);
    }
}

// ===========================================================================
// VFS operations structures (P3-10b: `SuperblockOps`/`FsTypeOps` trait
// impls over zero-sized unit structs; the fn-pointer tables are gone).
// ===========================================================================

/// xv6fs's [`SuperblockOps`] implementor (P3-10b).
///
/// The old table's always-0 orphan stubs (`xv6fs_add_orphan`/
/// `xv6fs_remove_orphan`/`xv6fs_recover_orphans` — placeholders until a
/// persistent orphan journal exists; if the system crashes with orphan
/// inodes they leak until fsck) are DELETED: the trait's `Ok(())`
/// defaults are behaviorally identical (success, no warning printed).
///
/// Transaction callbacks: see the C original's design-choice comment —
/// xv6fs registers `begin`/`end_transaction` for metadata operations
/// (create/unlink/etc, single transaction each); file operations
/// (write/truncate) manage transactions internally because they need
/// batching for large files.
pub(crate) struct Xv6fsSuperblockOps;

impl SuperblockOps for Xv6fsSuperblockOps {
    unsafe fn alloc_inode(&self, sb: *mut vfs_superblock) -> KResult<*mut vfs_inode> {
        xv6fs_alloc_inode_inner(sb)
    }
    unsafe fn get_inode(&self, sb: *mut vfs_superblock, ino: u64) -> KResult<*mut vfs_inode> {
        xv6fs_get_inode_inner(sb, ino)
    }
    unsafe fn sync_fs(&self, sb: *mut vfs_superblock, wait: c_int) -> KResult<()> {
        xv6fs_sync_fs_impl(sb, wait)
    }
    /// Mirrors `xv6fs_unmount_begin()` (sync, result ignored — exactly
    /// the old void callback's behavior).
    unsafe fn unmount_begin(&self, sb: *mut vfs_superblock) {
        let _ = xv6fs_sync_fs_impl(sb, 1);
    }
    unsafe fn statfs(&self, sb: *mut vfs_superblock, buf: *mut statfs) -> KResult<()> {
        let xv6_sb = sb as *mut xv6fs_superblock;
        // SAFETY: `sb`/`buf` are live (caller's contract).
        unsafe {
            let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);
            (*buf).f_type = FSMAGIC as u64;
            (*buf).f_bsize = super::BSIZE as u64;
            (*buf).f_frsize = super::BSIZE as u64;
            (*buf).f_blocks = (*disk_sb).size as u64;
            (*buf).f_namelen = super::DIRSIZ as u64;
            (*buf).f_files = (*disk_sb).ninodes as u64;

            // `initialized` is the lock-free lifecycle flag; `free_count`
            // lives inside the lock-owning `SpinLock<BlockCacheInner>`, so
            // read it under the guard (N-R7d — was a lock-free read).
            if (*xv6_sb).block_cache.initialized != 0 {
                let cache = (*xv6_sb).block_cache.cache.lock();
                (*buf).f_bfree = cache.free_count as u64;
                (*buf).f_bavail = (*buf).f_bfree;
            }
        }
        Ok(())
    }
    unsafe fn begin_transaction(&self, sb: *mut vfs_superblock) -> KResult<()> {
        let xv6_sb = sb as *mut xv6fs_superblock;
        let ret = xv6fs_begin_op(xv6_sb);
        if ret == 0 { Ok(()) } else { Err(Errno::Raw(ret)) }
    }
    unsafe fn end_transaction(&self, sb: *mut vfs_superblock) -> KResult<()> {
        let xv6_sb = sb as *mut xv6fs_superblock;
        xv6fs_end_op(xv6_sb);
        Ok(())
    }
}

pub(crate) static XV6FS_SUPERBLOCK_OPS: Xv6fsSuperblockOps = Xv6fsSuperblockOps;

static XV6FS_FS_TYPE_OPS: Xv6fsFsTypeOps = Xv6fsFsTypeOps;

// ===========================================================================
// Filesystem type initialization
// ===========================================================================

/// Initialize xv6fs caches and register the filesystem type. Does NOT
/// mount the filesystem -- call [`xv6fs_mount_root`] for that.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration; called from `vfs/fs.rs`'s `vfs_init()`.
pub(crate) extern "C" fn xv6fs_init() {
    let ret = __xv6fs_init_cache();
    kassert!(ret == 0, "xv6fs_init: __xv6fs_init_cache failed");

    let fs_type = vfs_fs_type_allocate();
    kassert!(!fs_type.is_null(), "xv6fs_init: vfs_fs_type_allocate failed");
    // SAFETY: `fs_type` is freshly allocated and exclusively owned here.
    unsafe {
        (*fs_type).name = c"xv6fs".as_ptr();
        (*fs_type).ops = Some(&XV6FS_FS_TYPE_OPS);
    }

    vfs_mount_lock();
    let ret = vfs_register_fs_type(fs_type);
    kassert!(ret == 0, "xv6fs_init: vfs_register_fs_type failed");
    vfs_mount_unlock();

    crate::kprintln!("xv6fs: filesystem type registered");
}

/// Mount xv6fs at `/root` and chroot into it. Requires tmpfs already
/// mounted as initial root (`vfs_root_inode.mnt_rooti` set).
///
/// Prefers ramdisk (major 3) if available, falls back to virtio disk
/// (major 2).
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration; called from `start_kernel.c`.
pub(crate) extern "C" fn xv6fs_mount_root() {
    // SAFETY: `vfs_root_inode` is the crate-wide dummy VFS-root static
    // (`vfs/fs.rs`), always live.
    let tmpfs_root = unsafe { vfs_root_inode.dev_mnt.mnt.mnt_rooti };
    if tmpfs_root.is_null() {
        crate::kprintln!("xv6fs: no root filesystem to mount onto");
        return;
    }

    // Create /root directory in tmpfs root (vfs_mkdir handles its own
    // locking; P3-10b: `KResult`-native, no ERR_PTR decode).
    let Ok(root_dir) = crate::vfs::inode::vfs_mkdir_inner(tmpfs_root, 0o755, c"root".as_ptr(), 4)
    else {
        crate::kprintln!("xv6fs: failed to create /root directory");
        return;
    };

    // Select root device: prefer ramdisk if available.
    let ramdisk = blkdev_get(major(RAMDISK_DEV), minor(RAMDISK_DEV));
    let root_dev = if !ramdisk.is_null() && !is_err(ramdisk) { RAMDISK_DEV } else { ROOTDEV };

    // Create a block device inode for root device. (P3-10b:
    // `KResult`-native; the old dead null -> `ENOMEM` branch had no
    // producer.)
    let dev_inode = match crate::vfs::inode::vfs_mknod_inner(
        tmpfs_root,
        super::S_IFBLK | 0o600,
        root_dev,
        c"rootdev".as_ptr(),
        7,
    ) {
        Ok(i) => i,
        Err(e) => {
            crate::kprintln!("xv6fs: failed to create device inode, errno={}", e.neg() as i64);
            vfs_iput(root_dir);
            return;
        }
    };

    // Mount xv6fs at /root. vfs_mount requires: mount mutex, superblock
    // write lock, and inode lock. On success, caller must release locks.
    // On failure, vfs_mount releases them.
    vfs_mount_lock();
    // SAFETY: `root_dir` is a live, referenced inode; `.sb` is valid.
    unsafe { vfs_superblock_wlock((*root_dir).sb) };
    vfs_ilock(root_dir);
    let ret = vfs_mount(c"xv6fs".as_ptr(), root_dir, dev_inode, 0, ptr::null());
    if ret == 0 {
        // Success: caller releases locks.
        vfs_iunlock(root_dir);
        // SAFETY: same as above.
        unsafe { vfs_superblock_unlock((*root_dir).sb) };
    }
    // On failure, vfs_mount already released locks.
    vfs_mount_unlock();

    // Release device inode reference (mount holds its own if needed).
    vfs_iput(dev_inode);

    if ret == 0 {
        crate::kprintln!("xv6fs: mounted at /root");

        // Now chroot into the xv6fs root.
        // SAFETY: `root_dir` is live.
        let xv6fs_root = unsafe { (*root_dir).dev_mnt.mnt.mnt_rooti };
        if !xv6fs_root.is_null() {
            let ret = vfs_chroot(xv6fs_root);
            if ret == 0 {
                crate::kprintln!("xv6fs: chroot to /root successful");
            } else {
                crate::kprintln!("xv6fs: chroot to /root failed, errno={}", ret);
            }
        }
    } else {
        crate::kprintln!("xv6fs: failed to mount at /root, errno={}", ret);
    }
    vfs_iput(root_dir);

    // xv6fs_run_all_smoketests() -- dead code, not ported (see module doc).
}
