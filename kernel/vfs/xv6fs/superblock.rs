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
    bio, blkdev_t, buf, completion_t, dinode, page_t, pcache, pcache_ops, slab_cache_t, statfs,
    vfs_fs_type, vfs_fs_type_ops, vfs_inode, vfs_superblock, vfs_superblock_ops, xv6fs_inode,
    xv6fs_superblock, EINVAL, EIO, ENOMEM, ENOSPC, PGSIZE,
};

use super::inode::XV6FS_INODE_OPS;
// Sub-wave B siblings -- plain Rust-path imports, not `extern "C"`; see
// `truncate.rs`'s module doc ("Intra-driver calls") for the convention.
use super::block_cache::{xv6fs_bcache_destroy, xv6fs_bcache_init};
use super::log::{xv6fs_begin_op, xv6fs_end_op, xv6fs_initlog, xv6fs_log_write};
use super::truncate::xv6fs_bmap_read;
use super::{xv6fs_iblock, xv6fs_type_to_mode, FSMAGIC, IPB, ROOTINO};

// ===========================================================================
// Externs — see the module doc above for the convention.
// ===========================================================================

unsafe extern "C" {
    // proc module.
    safe fn xv6_panic(msg: *const c_char) -> !;

    // printf.rs — C-variadic.
    fn printf(fmt: *const c_char, ...) -> c_int;

    // string.rs.
    safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

    // mm/slab.rs.
    safe fn slab_cache_init(cache: *mut slab_cache_t, name: *const c_char, obj_size: usize, flags: u64) -> c_int;
    safe fn slab_cache_shrink(cache: *mut slab_cache_t, nums: c_int) -> c_int;
    safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
    safe fn slab_free(obj: *mut c_void);

    // mm/pcache.rs.
    safe fn pcache_init(p: *mut pcache) -> c_int;
    // mm/page.rs -- `page->pcache.pcache_node` accessor (pre-existing).
    safe fn xv6_page_pcache_get_node(page: *mut page_t) -> *mut crate::bindings::pcache_node;

    // lock module (100% Rust).
    safe fn wait_for_completion(c: *mut completion_t);
    safe fn wait_for_completion_interruptible(c: *mut completion_t) -> c_int;

    // kernel/bio.c (classic xv6 buffer cache, unchanged C).
    safe fn bread(dev: u32, blockno: u32) -> *mut buf;
    safe fn brelse(b: *mut buf);
    safe fn bwrite(b: *mut buf);

    // dev/bio.c + dev/blkdev.c (block I/O submission path, unchanged C).
    safe fn bio_alloc(
        bdev: *mut blkdev_t,
        vec_length: i16,
        rw: crate::bindings::bool_,
        end_io: Option<unsafe extern "C" fn(bio: *mut bio)>,
        private_data: *mut c_void,
    ) -> *mut bio;
    safe fn bio_add_seg(bio: *mut bio, page: *mut page_t, idx: i16, len: u16, offset: u16) -> c_int;
    safe fn bio_release(bio: *mut bio) -> c_int;
    safe fn blkdev_submit_bio(blkdev: *mut blkdev_t, bio: *mut bio) -> c_int;
    safe fn blkdev_get(major: c_int, minor: c_int) -> *mut blkdev_t;
    safe fn blkdev_put(dev: *mut blkdev_t) -> c_int;

}

// P3-1C mesh sweep: vfs/{fs,inode}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures -- `mode_t`/`dev_t` are transparent `u32` type
// aliases, so the former `u32`-typed params are unaffected). `vfs_root_inode`
// is fs.rs's single dummy VFS-root `vfs_inode` instance.
use crate::vfs::fs::{
    vfs_fs_type_allocate, vfs_mount, vfs_mount_lock, vfs_mount_unlock, vfs_register_fs_type,
    vfs_root_inode, vfs_superblock_unlock, vfs_superblock_wlock,
};
use crate::vfs::inode::{vfs_chroot, vfs_ilock, vfs_iput, vfs_iunlock, vfs_mkdir, vfs_mknod};

/// Mirrors the C `assert(expr, fmt)` macro (`kernel/inc/printf.h`).
/// Reimplemented locally per this crate's established convention.
macro_rules! kassert {
    ($cond:expr, $msg:expr) => {
        if !($cond) {
            xv6_panic(concat!($msg, "\0").as_ptr() as *const c_char)
        }
    };
}

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}
#[inline(always)]
fn err_ptr<T>(errno: c_int) -> *mut T {
    errno as isize as *mut T
}
#[inline(always)]
fn is_err<T>(p: *mut T) -> bool {
    (p as usize) >= (-(4095isize)) as usize
}
#[inline(always)]
fn is_err_or_null<T>(p: *mut T) -> bool {
    p.is_null() || is_err(p)
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
            (*b).blkno = addr as u64 * BLK512_PER_BSIZE;
            let ret = bio_add_seg(b, page, 0, super::BSIZE as u16, (i * super::BSIZE as u64) as u16);
            if ret != 0 {
                bio_release(b);
                return ret;
            }
            let ret = blkdev_submit_bio(xv6_sb_blkdev(xv6_sb), b);
            if ret != 0 {
                bio_release(b);
                return ret;
            }
            let ret = bio_await(b);
            bio_release(b);
            if ret != 0 {
                return ret;
            }
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
            (*b).blkno = addr as u64 * BLK512_PER_BSIZE;
            let ret = bio_add_seg(b, page, 0, super::BSIZE as u16, (i * super::BSIZE as u64) as u16);
            if ret != 0 {
                bio_release(b);
                return ret;
            }
            let ret = blkdev_submit_bio(xv6_sb_blkdev(xv6_sb), b);
            if ret != 0 {
                bio_release(b);
                return ret;
            }
            let ret = bio_await(b);
            bio_release(b);
            if ret != 0 {
                return ret;
            }
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

fn __xv6fs_read_superblock(dev: u32, disk_sb: *mut crate::bindings::superblock) -> c_int {
    let bp = bread(dev, 1);
    if bp.is_null() {
        return neg(EIO);
    }
    // SAFETY: `bp` is a live, just-read buffer; `disk_sb` is a valid
    // exclusively-owned out-param.
    unsafe {
        memmove(disk_sb as *mut c_void, (*bp).data as *const c_void, core::mem::size_of::<crate::bindings::superblock>());
        brelse(bp);
        if (*disk_sb).magic != FSMAGIC {
            return neg(EINVAL);
        }
    }
    0
}

fn __xv6fs_write_superblock(dev: u32, disk_sb: *mut crate::bindings::superblock) -> c_int {
    let bp = bread(dev, 1);
    if bp.is_null() {
        return neg(EIO);
    }
    // SAFETY: `bp` is a live, just-read buffer; `disk_sb` is live.
    unsafe {
        memmove((*bp).data as *mut c_void, disk_sb as *const c_void, core::mem::size_of::<crate::bindings::superblock>());
        bwrite(bp);
        brelse(bp);
    }
    0
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
        (*xi).vfs_inode.ops = ptr::addr_of!(XV6FS_INODE_OPS) as *mut _;
    }
    xi
}

/// Mirrors `xv6fs_alloc_inode()`.
pub(crate) extern "C" fn xv6fs_alloc_inode(sb: *mut vfs_superblock) -> *mut vfs_inode {
    if sb.is_null() {
        return err_ptr(neg(EINVAL));
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
                return err_ptr(neg(EIO));
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
                    return err_ptr(neg(ENOMEM));
                }
                (*xi).dev = dev;
                (*xi).vfs_inode.ino = inum;
                // Note: Do NOT set vfs_inode.sb here -- VFS will set it in
                // vfs_add_inode.
                (*xi).vfs_inode.ref_count = 1;

                return ptr::addr_of_mut!((*xi).vfs_inode);
            }
            brelse(bp);
            inum += 1;
        }
    }

    err_ptr(neg(ENOSPC))
}

// ===========================================================================
// Get inode from disk
// ===========================================================================

/// Mirrors `xv6fs_get_inode()`.
pub(crate) extern "C" fn xv6fs_get_inode(sb: *mut vfs_superblock, ino: u64) -> *mut vfs_inode {
    if sb.is_null() || ino == 0 {
        return err_ptr(neg(EINVAL));
    }
    let xv6_sb = sb as *mut xv6fs_superblock;
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe {
        let disk_sb = ptr::addr_of_mut!((*xv6_sb).disk_sb);
        let dev = xv6fs_sb_dev(xv6_sb);

        if ino >= (*disk_sb).ninodes as u64 {
            return err_ptr(neg(crate::bindings::ENOENT));
        }

        let bp = bread(dev, xv6fs_iblock(ino, (*disk_sb).inodestart));
        if bp.is_null() {
            return err_ptr(neg(EIO));
        }

        let dip = ((*bp).data as *mut dinode).add((ino % IPB) as usize);
        if (*dip).type_ == 0 {
            brelse(bp);
            return err_ptr(neg(crate::bindings::ENOENT));
        }

        let xi = __xv6fs_alloc_inode_structure();
        if xi.is_null() {
            brelse(bp);
            return err_ptr(neg(ENOMEM));
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
            (*xi).vfs_inode.__bindgen_anon_2.bdev = devno;
        } else if (*dip).type_ == super::XV6FS_T_CDEVICE {
            let devno = mkdev((*xi).major as i32, (*xi).minor as i32);
            (*xi).vfs_inode.__bindgen_anon_2.cdev = devno;
        }

        brelse(bp);
        ptr::addr_of_mut!((*xi).vfs_inode)
    }
}

// ===========================================================================
// Sync operations
// ===========================================================================

/// Mirrors `xv6fs_sync_fs()`.
pub(crate) extern "C" fn xv6fs_sync_fs(sb: *mut vfs_superblock, _wait: c_int) -> c_int {
    if sb.is_null() {
        return neg(EINVAL);
    }
    let xv6_sb = sb as *mut xv6fs_superblock;
    // SAFETY: `xv6_sb`/`sb` are live (caller's contract).
    unsafe {
        if (*xv6_sb).dirty != 0 {
            let ret = __xv6fs_write_superblock(xv6fs_sb_dev(xv6_sb), ptr::addr_of_mut!((*xv6_sb).disk_sb));
            if ret != 0 {
                return ret;
            }
            (*xv6_sb).dirty = 0;
        }
        (*sb).__bindgen_anon_2.set_dirty(0);
    }
    0
}

/// Mirrors `xv6fs_unmount_begin()`.
pub(crate) extern "C" fn xv6fs_unmount_begin(sb: *mut vfs_superblock) {
    xv6fs_sync_fs(sb, 1);
}

extern "C" fn xv6fs_statfs(sb: *mut vfs_superblock, buf: *mut statfs) -> c_int {
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

        if (*xv6_sb).block_cache.initialized != 0 {
            (*buf).f_bfree = (*xv6_sb).block_cache.free_count as u64;
            (*buf).f_bavail = (*buf).f_bfree;
        }
    }
    0
}

// ===========================================================================
// Mount/Free operations
// ===========================================================================

/// Mirrors `xv6fs_free()`.
pub(crate) extern "C" fn xv6fs_free(sb: *mut vfs_superblock) {
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

/// Mirrors `xv6fs_mount()`.
extern "C" fn xv6fs_mount(
    mountpoint: *mut vfs_inode,
    device: *mut vfs_inode,
    _flags: c_int,
    _data: *const c_char,
    ret_sb: *mut *mut vfs_superblock,
) -> c_int {
    if mountpoint.is_null() || ret_sb.is_null() {
        return neg(EINVAL);
    }

    // Get the block device from the device inode. The device inode's
    // bdev field contains the device number (major:minor). If no device
    // inode is provided, xv6fs does not support that (unlike tmpfs).
    // SAFETY: `device` checked non-null before deref.
    let dev_num = unsafe {
        if device.is_null() || !super::s_isblk((*device).mode) {
            return neg(EINVAL); // xv6fs does not support block device inode
        }
        (*device).__bindgen_anon_2.bdev
    };

    let blkdev = blkdev_get(major(dev_num), minor(dev_num));
    if is_err(blkdev) {
        return blkdev as isize as c_int;
    }

    let xv6_sb = slab_alloc(xv6fs_sb_cache()) as *mut xv6fs_superblock;
    if xv6_sb.is_null() {
        blkdev_put(blkdev);
        return neg(ENOMEM);
    }
    // SAFETY: `xv6_sb` is a freshly allocated, exclusively-owned block.
    unsafe {
        ptr::write_bytes(xv6_sb as *mut u8, 0, core::mem::size_of::<xv6fs_superblock>());
        (*xv6_sb).blkdev = blkdev;

        let ret = __xv6fs_read_superblock(xv6fs_sb_dev(xv6_sb), ptr::addr_of_mut!((*xv6_sb).disk_sb));
        if ret != 0 {
            blkdev_put(blkdev);
            slab_free(xv6_sb as *mut c_void);
            return ret;
        }

        (*xv6_sb).dirty = 0;

        // Initialize logging layer.
        xv6fs_initlog(xv6_sb);

        // Initialize block allocation cache.
        let ret = xv6fs_bcache_init(xv6_sb);
        if ret != 0 {
            printf(
                c"xv6fs: warning: block cache init failed (%d), using fallback\n".as_ptr(),
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
        (*xv6_sb).vfs_sb.__bindgen_anon_2.set_backendless(0);
        (*xv6_sb).vfs_sb.ops = ptr::addr_of!(XV6FS_SUPERBLOCK_OPS) as *mut _;
        (*xv6_sb).vfs_sb.fs_data = xv6_sb as *mut c_void;

        // Load root inode (inode 1 in xv6).
        let root_inode = xv6fs_get_inode(ptr::addr_of_mut!((*xv6_sb).vfs_sb), ROOTINO);
        if is_err_or_null(root_inode) {
            blkdev_put(blkdev);
            slab_free(xv6_sb as *mut c_void);
            return if root_inode.is_null() { neg(ENOMEM) } else { root_inode as isize as c_int };
        }

        (*xv6_sb).vfs_sb.root_inode = root_inode;

        *ret_sb = ptr::addr_of_mut!((*xv6_sb).vfs_sb);
    }
    0
}

// ===========================================================================
// Orphan inode operations
//
// TODO: Implement persistent orphan journal. For now, these are stubs
// that allow the VFS unmount path to work correctly. If the system
// crashes with orphan inodes, those inodes will leak until fsck is run.
// ===========================================================================

extern "C" fn xv6fs_add_orphan(_sb: *mut vfs_superblock, _inode: *mut vfs_inode) -> c_int {
    0
}
extern "C" fn xv6fs_remove_orphan(_sb: *mut vfs_superblock, _inode: *mut vfs_inode) -> c_int {
    0
}
extern "C" fn xv6fs_recover_orphans(_sb: *mut vfs_superblock) -> c_int {
    0
}

// ===========================================================================
// Transaction Callbacks for VFS-managed operations
//
// See the C original's design-choice comment (reproduced in
// `vfs/vfs_types.h`'s `vfs_superblock_ops.begin_transaction` doc): xv6fs
// registers callbacks for metadata operations (create/unlink/etc, single
// transaction each); file operations (write/truncate) manage transactions
// internally because they need batching for large files.
// ===========================================================================

extern "C" fn xv6fs_begin_transaction_op(sb: *mut vfs_superblock) -> c_int {
    let xv6_sb = sb as *mut xv6fs_superblock;
    xv6fs_begin_op(xv6_sb)
}
extern "C" fn xv6fs_end_transaction_op(sb: *mut vfs_superblock) -> c_int {
    let xv6_sb = sb as *mut xv6fs_superblock;
    xv6fs_end_op(xv6_sb);
    0
}

// ===========================================================================
// VFS operations structures
// ===========================================================================

pub(crate) static XV6FS_SUPERBLOCK_OPS: vfs_superblock_ops = vfs_superblock_ops {
    alloc_inode: Some(xv6fs_alloc_inode),
    get_inode: Some(xv6fs_get_inode),
    sync_fs: Some(xv6fs_sync_fs),
    unmount_begin: Some(xv6fs_unmount_begin),
    add_orphan: Some(xv6fs_add_orphan),
    remove_orphan: Some(xv6fs_remove_orphan),
    recover_orphans: Some(xv6fs_recover_orphans),
    statfs: Some(xv6fs_statfs),
    begin_transaction: Some(xv6fs_begin_transaction_op),
    end_transaction: Some(xv6fs_end_transaction_op),
};

static XV6FS_FS_TYPE_OPS: vfs_fs_type_ops = vfs_fs_type_ops {
    mount: Some(xv6fs_mount),
    free: Some(xv6fs_free),
};

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
        (*fs_type).ops = ptr::addr_of!(XV6FS_FS_TYPE_OPS) as *mut _;
    }

    vfs_mount_lock();
    let ret = vfs_register_fs_type(fs_type);
    kassert!(ret == 0, "xv6fs_init: vfs_register_fs_type failed");
    vfs_mount_unlock();

    // SAFETY: `printf` is C-variadic; no format arguments.
    unsafe { printf(c"xv6fs: filesystem type registered\n".as_ptr()) };
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
    let tmpfs_root = unsafe { vfs_root_inode.__bindgen_anon_2.__bindgen_anon_1.mnt_rooti };
    if tmpfs_root.is_null() {
        // SAFETY: no format arguments.
        unsafe { printf(c"xv6fs: no root filesystem to mount onto\n".as_ptr()) };
        return;
    }

    // Create /root directory in tmpfs root (vfs_mkdir handles its own locking).
    let root_dir = vfs_mkdir(tmpfs_root, 0o755, c"root".as_ptr(), 4);
    if is_err_or_null(root_dir) {
        // SAFETY: no format arguments.
        unsafe { printf(c"xv6fs: failed to create /root directory\n".as_ptr()) };
        return;
    }

    // Select root device: prefer ramdisk if available.
    let ramdisk = blkdev_get(major(RAMDISK_DEV), minor(RAMDISK_DEV));
    let root_dev = if !ramdisk.is_null() && !is_err(ramdisk) { RAMDISK_DEV } else { ROOTDEV };

    // Create a block device inode for root device.
    let dev_inode = vfs_mknod(tmpfs_root, super::S_IFBLK | 0o600, root_dev, c"rootdev".as_ptr(), 7);
    if is_err_or_null(dev_inode) {
        let errno = if dev_inode.is_null() { neg(ENOMEM) } else { dev_inode as isize as c_int };
        // SAFETY: format matches the single %ld argument.
        unsafe { printf(c"xv6fs: failed to create device inode, errno=%ld\n".as_ptr(), errno as i64) };
        vfs_iput(root_dir);
        return;
    }

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
        // SAFETY: no format arguments.
        unsafe { printf(c"xv6fs: mounted at /root\n".as_ptr()) };

        // Now chroot into the xv6fs root.
        // SAFETY: `root_dir` is live.
        let xv6fs_root = unsafe { (*root_dir).__bindgen_anon_2.__bindgen_anon_1.mnt_rooti };
        if !xv6fs_root.is_null() {
            let ret = vfs_chroot(xv6fs_root);
            if ret == 0 {
                // SAFETY: no format arguments.
                unsafe { printf(c"xv6fs: chroot to /root successful\n".as_ptr()) };
            } else {
                // SAFETY: format matches the single %d argument.
                unsafe { printf(c"xv6fs: chroot to /root failed, errno=%d\n".as_ptr(), ret) };
            }
        }
    } else {
        // SAFETY: format matches the single %d argument.
        unsafe { printf(c"xv6fs: failed to mount at /root, errno=%d\n".as_ptr(), ret) };
    }
    vfs_iput(root_dir);

    // xv6fs_run_all_smoketests() -- dead code, not ported (see module doc).
}
