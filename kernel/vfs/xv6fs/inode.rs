//! xv6fs inode operations — Rust port of `kernel/vfs/xv6fs/inode.c`
//! (Phase 2 Wave 19, sub-wave A; see `super` module doc and
//! `docs/rustify/phase2_plan.md`).
//!
//! Directory entry lookup/link/unlink (a flat on-disk array of `struct
//! dirent`, linearly scanned — unlike tmpfs's in-memory hash list, xv6fs
//! has no directory cache), every `vfs_inode_ops` callback, and `struct
//! vfs_inode_ops xv6fs_inode_ops` itself. Also owns `xv6fs_iupdate`
//! (write an in-memory `xv6fs_inode`'s fields back to its on-disk
//! `dinode`), called from this file and, via a plain Rust-path call (see
//! `truncate.rs`'s module doc, "Intra-driver calls"), from
//! [`super::truncate`]/[`super::file`] (sub-wave B).
//!
//! # Locking (see the C original's own file-header comment, reproduced
//! here for the same reasons)
//!
//! 1. `vfs_superblock` rwsem (held by VFS layer for
//!    create/mkdir/unlink/etc)
//! 2. `vfs_inode` mutex (held by VFS layer before calling inode ops)
//! 3. `log->lock` spinlock (acquired by `xv6fs_begin_op`/`end_op`)
//! 4. buffer mutex (acquired by `bread`/`brelse`)
//!
//! CRITICAL: [`xv6fs_destroy_inode`] is called from `vfs_iput` while
//! holding superblock wlock + inode lock; the VFS-managed
//! `begin_transaction`/`end_transaction` hooks (`superblock.rs`) already
//! opened a transaction before that lock chain, per the class-level
//! "hybrid approach" documented in `superblock.rs`'s module doc.
//!
//! # Fidelity note: NULL-`bread()` deref safety fix
//!
//! The C original's directory-entry scan helpers are inconsistent about
//! checking `bread()`'s result for `NULL` before dereferencing it:
//! `xv6fs_lookup`/`xv6fs_dir_iter` check (`if (bp == NULL) continue;`);
//! `__xv6fs_dir_name_exists`/`__xv6fs_dirlink`/`xv6fs_create`'s
//! existing-name scan/`xv6fs_unlink` do **not** — a latent NULL-pointer
//! dereference on an out-of-memory `bread()` failure (bread only returns
//! NULL on OOM; see `kernel/bio.c`'s own doc comment). This port
//! consolidates every read-only directory-entry scan through one
//! [`read_dirent`] helper that **always** checks, converting the latent
//! C UB (a crash-via-null-deref in the unchecked call sites) into the
//! same defined "skip this entry" behavior the checked call sites already
//! had — a strictly safer, behavior-preserving-on-the-success-path
//! deviation from the unchecked sites, in the same spirit as this crate's
//! other documented safety fixes discovered mid-port (e.g. Wave 18's
//! tmpfs `__tmpfs_move` use-after-free fix). The two call sites that
//! *write* a directory entry after finding its offset (`__xv6fs_dirlink`'s
//! final write, `xv6fs_unlink`'s clear-entry write) keep their own
//! `bread()` + explicit NULL check inline (not via `read_dirent`, which is
//! read-only), matching the C's re-read-for-write structure exactly.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings::{
    dev_t, dinode, dirent, loff_t, mode_t, stat, vfs_dentry, vfs_dir_iter, vfs_file, vfs_inode,
    vfs_inode_ops, vfs_superblock, xv6fs_inode, xv6fs_superblock, EEXIST, EINVAL, EIO, ENOENT,
    ENOMEM, ENOSPC, ENOTDIR, EOPNOTSUPP, EPERM,
};

use super::{xv6fs_mode_to_type, DIRSIZ};

// Sub-wave B landed: xv6fs/{truncate,log,file}.rs are Rust siblings in
// this same driver now, so these are plain Rust-path imports (not
// `extern "C"`), matching this crate's established filesystem-driver
// convention -- see `truncate.rs`'s module doc ("Intra-driver calls") for
// the precedent (`tmpfs/superblock.rs`'s own `super::inode::
// tmpfs_make_directory` call).
use crate::proc::proc_shims::xv6_panic;

use super::log::xv6fs_log_write;
use super::truncate::{xv6fs_bmap, xv6fs_bmap_read, xv6fs_itrunc, xv6fs_truncate};

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention (this
// block: symbols outside the xv6fs driver).
// ===========================================================================

unsafe extern "C" {
    // string.rs.
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    safe fn strncmp(s1: *const c_char, s2: *const c_char, n: usize) -> c_int;
    safe fn strncpy(s: *mut c_char, t: *const c_char, n: usize) -> *mut c_char;
    safe fn strnlen(s: *const c_char, maxlen: usize) -> usize;
    safe fn strndup(s: *const c_char, n: usize) -> *mut c_char;

}

// P3-D3c: `bufcache.rs`'s entry points are plain (safe) Rust fns now that
// their `#[no_mangle]` exports are gone; identical signatures, plain `use`.
use crate::bufcache::{bread, brelse};

// P3-D3a: `pcache_teardown` (mm/pcache.rs) is an ordinary (safe) Rust fn
// now that its `#[no_mangle]` export is gone; identical signature, plain
// `use`. `slab_free` is genuinely `unsafe fn` in `crate::mm::slab`; the
// thin wrapper preserves the `safe fn` facade the old redeclaration
// asserted (see `cffi::raw`'s identical note).
use crate::mm::pcache_teardown;

/// SAFETY: `obj` must originate from the paired `slab_alloc`
/// (see [`crate::mm::slab::slab_free`]'s contract).
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}

// P3-1C mesh sweep: vfs/{fs,inode}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures).
use crate::vfs::fs::{vfs_alloc_inode, vfs_release_dentry};
use crate::vfs::inode::{vfs_ilock, vfs_iunlock};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}
// `is_err_or_null`/`ptr_err`'s canonical home is `crate::kstd` (P3-CS1
// centralization). P3-CS12: the ERR_PTR-returning inode-ops vtable methods
// (`__xv6fs_create`/`__xv6fs_mkdir`/`__xv6fs_symlink`/`__xv6fs_mknod`) now
// build a `KResult<*mut vfs_inode>` internally and encode it to the
// ABI-fixed ERR_PTR exactly once at the `extern "C"` boundary via
// `result_to_errptr`; only the error-return encoding changed, the on-disk
// I/O / lock / cleanup control flow is byte-identical.
use crate::kstd::{is_err_or_null, ptr_err, result_to_errptr, Errno, KResult};

/// Mirrors `major(dev)`/`minor(dev)` (`kernel/inc/defs.h`) — hardcoded
/// locally, same rationale/precedent as `superblock.rs`'s own copy.
#[inline(always)]
fn major(dev: u32) -> i16 {
    ((dev >> 20) & 0xFFF) as i16
}
#[inline(always)]
fn minor(dev: u32) -> i16 {
    (dev & 0xFFFFF) as i16
}
#[inline(always)]
fn mkdev(major: i16, minor: i16) -> u32 {
    (((major as i32) << 20) | (minor as i32)) as u32
}

const DIRENT_SIZE: u32 = core::mem::size_of::<dirent>() as u32;

// ===========================================================================
// Inode update/sync
// ===========================================================================

/// Mirrors `xv6fs_iupdate()`.
///
/// # Safety
/// `ip` must point to a live `xv6fs_inode` whose `vfs_inode.sb` is a live
/// `xv6fs_superblock`, with a transaction active (caller's contract, same
/// as the C original).
pub(crate) extern "C" fn xv6fs_iupdate(ip: *mut xv6fs_inode) {
    // SAFETY: `ip` is live (caller's contract).
    unsafe {
        let xv6_sb = (*ip).vfs_inode.sb as *mut xv6fs_superblock;
        // Defensive guard (not present in the C original, which
        // unconditionally dereferences `ip->vfs_inode.sb`): every call
        // site in this driver sets `sb` before reaching here (verified by
        // audit -- `vfs_alloc_inode`/`vfs_add_inode` set it before
        // returning a new inode, and `vfs_iput` never clears it before
        // invoking `destroy_inode`), so `xv6_sb` should never be null in
        // practice. A single unexplained null-`sb` crash was observed
        // once in ~30 stress runs of `usertests outofinodes` during this
        // port's verification and did not reproduce afterward; converting
        // the C's latent null-pointer UB into a loud, diagnosable panic
        // here is strictly safer than either silently corrupting memory
        // (the C behavior) or silently skipping the on-disk update.
        if xv6_sb.is_null() {
            xv6_panic(c"xv6fs_iupdate: inode has no superblock".as_ptr());
        }
        let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);

        let bp = bread((*ip).dev, super::xv6fs_iblock((*ip).vfs_inode.ino, (*disk_sb).inodestart));
        if bp.is_null() {
            // C original does not NULL-check here either -- but an
            // unconditional deref of a NULL `bp` below would be
            // instant UB in Rust (not just a C crash), so this port
            // bails out instead. Documented per the module doc's
            // "Fidelity note".
            return;
        }
        let dip = ((*bp).data as *mut dinode).add(((*ip).vfs_inode.ino % super::IPB) as usize);

        (*dip).type_ = xv6fs_mode_to_type((*ip).vfs_inode.mode);
        (*dip).major = (*ip).major;
        (*dip).minor = (*ip).minor;
        (*dip).nlink = (*ip).vfs_inode.n_links as i16;
        (*dip).size = (*ip).vfs_inode.size as u32;
        (*dip).addrs = (*ip).addrs;

        xv6fs_log_write(xv6_sb, bp);
        brelse(bp);
    }
}

/// Mirrors `xv6fs_sync_inode()`.
pub(crate) extern "C" fn xv6fs_sync_inode(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return neg(EINVAL);
    }
    let ip = inode as *mut xv6fs_inode;
    xv6fs_iupdate(ip);
    // SAFETY: `inode` is live.
    unsafe { (*inode).flags.set_dirty(0) };
    0
}

/// Mirrors `xv6fs_dirty_inode()`.
pub(crate) extern "C" fn xv6fs_dirty_inode(inode: *mut vfs_inode) -> c_int {
    if inode.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: `inode` is live.
    unsafe { (*inode).flags.set_dirty(1) };
    0
}

// ===========================================================================
// Directory-entry scan helper — see the module doc's "Fidelity note".
// ===========================================================================

/// Read the `struct dirent` at byte offset `off` within directory inode
/// `dp`. `alloc` selects `xv6fs_bmap` (allocating) vs `xv6fs_bmap_read`
/// (read-only), matching whichever the C call site this mirrors used.
/// Returns `None` if the block is unmapped/sparse or unreadable (`bread`
/// failure) — the latter always checked here, see the module doc.
///
/// # Safety
/// `dp` must point to a live `xv6fs_inode`.
unsafe fn read_dirent(dp: *mut xv6fs_inode, off: u32, alloc: bool) -> Option<dirent> {
    unsafe {
        let bn = off / super::BSIZE;
        let block_off = (off % super::BSIZE) as usize;
        let addr = if alloc { xv6fs_bmap(dp, bn) } else { xv6fs_bmap_read(dp, bn) };
        if addr == 0 {
            return None;
        }
        let bp = bread((*dp).dev, addr);
        if bp.is_null() {
            return None;
        }
        let mut de: dirent = core::mem::zeroed();
        memmove(
            ptr::addr_of_mut!(de) as *mut c_void,
            (*bp).data.add(block_off) as *const c_void,
            core::mem::size_of::<dirent>(),
        );
        brelse(bp);
        Some(de)
    }
}

// ===========================================================================
// Directory operations
// ===========================================================================

/// Mirrors `xv6fs_lookup()`.
extern "C" fn __xv6fs_lookup(dir: *mut vfs_inode, dentry: *mut vfs_dentry, name: *const c_char, mut name_len: usize) -> c_int {
    if dir.is_null() || dentry.is_null() || name.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: `dir` is live (caller's contract).
    if !super::s_isdir(unsafe { (*dir).mode }) {
        return neg(ENOTDIR);
    }
    if name_len > DIRSIZ {
        name_len = DIRSIZ;
    }

    let dp = dir as *mut xv6fs_inode;
    let mut off: u32 = 0;
    // SAFETY: `dir` is live.
    while (off as i64) < unsafe { (*dir).size } {
        // SAFETY: `dp` is live.
        if let Some(de) = unsafe { read_dirent(dp, off, true) } {
            if de.inum != 0 {
                let de_name_len = strnlen(de.name.as_ptr(), DIRSIZ);
                if name_len == de_name_len && strncmp(name, de.name.as_ptr(), name_len) == 0 {
                    // Found.
                    // SAFETY: `dentry`/`dir` are live.
                    unsafe {
                        (*dentry).ino = de.inum as u64;
                        (*dentry).sb = (*dir).sb;
                        (*dentry).parent = dir;
                        (*dentry).name = strndup(name, name_len);
                        if (*dentry).name.is_null() {
                            return neg(ENOMEM);
                        }
                        (*dentry).name_len = name_len as u16;
                    }
                    return 0;
                }
            }
        }
        off += DIRENT_SIZE;
    }

    neg(ENOENT)
}

/// Mirrors `xv6fs_dir_iter()`.
extern "C" fn __xv6fs_dir_iter(dir: *mut vfs_inode, iter: *mut vfs_dir_iter, ret_dentry: *mut vfs_dentry) -> c_int {
    if dir.is_null() || iter.is_null() || ret_dentry.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: `dir` is live.
    if !super::s_isdir(unsafe { (*dir).mode }) {
        return neg(ENOTDIR);
    }

    let dp = dir as *mut xv6fs_inode;

    // VFS handles "." via iter->index == 0. VFS calls driver with
    // iter->index == 1 for ".." on ordinary directories. VFS calls
    // driver with iter->index > 1 for regular entries.

    // SAFETY: `iter` is live.
    if unsafe { (*iter).index } == 1 {
        // Look up ".." in the on-disk directory to get parent inode number.
        let mut off: u32 = 0;
        while (off as i64) < unsafe { (*dir).size } {
            // SAFETY: `dp` is live.
            if let Some(de) = unsafe { read_dirent(dp, off, true) } {
                if de.inum != 0 && de.name[0] == b'.' as c_char && de.name[1] == b'.' as c_char && de.name[2] == 0 {
                    // SAFETY: `ret_dentry`/`dir` are live.
                    unsafe {
                        vfs_release_dentry(ret_dentry);
                        (*ret_dentry).name = strndup(c"..".as_ptr(), 2);
                        if (*ret_dentry).name.is_null() {
                            return neg(ENOMEM);
                        }
                        (*ret_dentry).name_len = 2;
                        (*ret_dentry).ino = de.inum as u64;
                        (*ret_dentry).sb = (*dir).sb; // VFS doesn't set sb for index==1
                        (*ret_dentry).cookies = 0; // Will be reset by VFS for index > 1
                    }
                    return 0;
                }
            }
            off += DIRENT_SIZE;
        }
        // ".." not found on disk (shouldn't happen for valid dirs).
        return neg(ENOENT);
    }

    // Handle regular entries when index > 1.
    // SAFETY: `ret_dentry` is live.
    let start_off = unsafe { (*ret_dentry).cookies } as u32;
    let mut off = start_off;
    while (off as i64) < unsafe { (*dir).size } {
        // SAFETY: `dp` is live.
        if let Some(de) = unsafe { read_dirent(dp, off, true) } {
            if de.inum != 0 {
                // Skip . and .. as VFS handles them.
                let is_dot = de.name[0] == b'.' as c_char && de.name[1] == 0;
                let is_dotdot = de.name[0] == b'.' as c_char && de.name[1] == b'.' as c_char && de.name[2] == 0;
                if !is_dot && !is_dotdot {
                    let namelen = strnlen(de.name.as_ptr(), DIRSIZ);
                    let name = strndup(de.name.as_ptr(), namelen);
                    if name.is_null() {
                        return neg(ENOMEM);
                    }
                    // SAFETY: `ret_dentry` is live.
                    unsafe {
                        vfs_release_dentry(ret_dentry);
                        (*ret_dentry).ino = de.inum as u64;
                        (*ret_dentry).name = name;
                        (*ret_dentry).name_len = namelen as u16;
                        (*ret_dentry).cookies = (off + DIRENT_SIZE) as i64; // Next offset for continuation.
                    }
                    return 0;
                }
            }
        }
        off += DIRENT_SIZE;
    }

    // End of directory -- return 0 with name=NULL to signal end.
    // SAFETY: `ret_dentry` is live.
    unsafe {
        vfs_release_dentry(ret_dentry);
        (*ret_dentry).name = ptr::null_mut();
        (*ret_dentry).name_len = 0;
        (*ret_dentry).cookies = super::VFS_DENTRY_COOKIE_END;
    }
    0
}

// ===========================================================================
// Create/Unlink operations
// ===========================================================================

/// Mirrors `__xv6fs_dir_name_exists()`. Returns the inode number if
/// found, 0 if not found.
///
/// # Safety
/// `dp` must point to a live `xv6fs_inode`; `name` must be a valid,
/// NUL-terminated (within `DIRSIZ` bytes) C string.
unsafe fn __xv6fs_dir_name_exists(dp: *mut xv6fs_inode, name: *const c_char) -> u32 {
    unsafe {
        let mut off: u32 = 0;
        while (off as i64) < (*dp).vfs_inode.size {
            if let Some(de) = read_dirent(dp, off, false) {
                if de.inum != 0 && strncmp(de.name.as_ptr(), name, DIRSIZ) == 0 {
                    return de.inum as u32;
                }
            }
            off += DIRENT_SIZE;
        }
        0
    }
}

/// Mirrors `__xv6fs_dirlink()`. Add a directory entry.
///
/// # Safety
/// `xv6_sb`/`dp` must point to live, exclusively-accessed structures with
/// a transaction active; `name` must be a valid C string no longer than
/// `DIRSIZ` bytes.
unsafe fn __xv6fs_dirlink(xv6_sb: *mut xv6fs_superblock, dp: *mut xv6fs_inode, name: *const c_char, inum: u32) -> c_int {
    unsafe {
        // Look for an empty directory slot.
        let mut off: u32 = 0;
        let mut found_off: Option<u32> = None;
        while (off as i64) < (*dp).vfs_inode.size {
            if let Some(de) = read_dirent(dp, off, true) {
                if de.inum == 0 {
                    found_off = Some(off);
                    break;
                }
            }
            off += DIRENT_SIZE;
        }
        // No empty slot found -- extend directory.
        let off = found_off.unwrap_or((*dp).vfs_inode.size as u32);

        let mut de: dirent = core::mem::zeroed();
        strncpy(de.name.as_mut_ptr(), name, DIRSIZ);
        de.inum = inum as u16;

        let bn = off / super::BSIZE;
        let block_off = (off % super::BSIZE) as usize;
        let addr = xv6fs_bmap(dp, bn);
        if addr == 0 {
            return neg(ENOSPC);
        }
        let bp = bread((*dp).dev, addr);
        if bp.is_null() {
            // See the module doc's "Fidelity note": C does not NULL-check
            // here, but an unconditional deref would be Rust UB.
            return neg(EIO);
        }
        memmove(
            (*bp).data.add(block_off) as *mut c_void,
            ptr::addr_of!(de) as *const c_void,
            core::mem::size_of::<dirent>(),
        );
        xv6fs_log_write(xv6_sb, bp);
        brelse(bp);

        if off as i64 >= (*dp).vfs_inode.size {
            (*dp).vfs_inode.size = (off + DIRENT_SIZE) as i64;
            xv6fs_iupdate(dp);
        }

        0
    }
}

extern "C" fn __xv6fs_create(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> *mut vfs_inode {
    result_to_errptr(__xv6fs_create_inner(dir, mode, name, name_len))
}

fn __xv6fs_create_inner(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
    if dir.is_null() || name.is_null() || name_len == 0 || name_len > DIRSIZ {
        return Err(Errno::Inval);
    }

    let dp = dir as *mut xv6fs_inode;
    // SAFETY: `dir` is live.
    let xv6_sb = unsafe { (*dir).sb as *mut xv6fs_superblock };

    // Check if file already exists.
    let mut name_buf = [0u8; DIRSIZ];
    // SAFETY: `name` has at least `name_len` readable bytes.
    unsafe { memmove(name_buf.as_mut_ptr() as *mut c_void, name as *const c_void, name_len) };

    let mut existing_ino: u32 = 0;
    let mut off: u32 = 0;
    // SAFETY: `dir` is live.
    while (off as i64) < unsafe { (*dir).size } {
        // SAFETY: `dp` is live.
        if let Some(de) = unsafe { read_dirent(dp, off, false) } {
            if de.inum != 0 && strncmp(de.name.as_ptr(), name_buf.as_ptr() as *const c_char, DIRSIZ) == 0 {
                existing_ino = de.inum as u32;
                break;
            }
        }
        off += DIRENT_SIZE;
    }

    if existing_ino != 0 {
        return Err(Errno::Exist);
    }

    // SAFETY: `dir` is live.
    let new_inode = vfs_alloc_inode(unsafe { (*dir).sb });
    if is_err_or_null(new_inode) {
        return if new_inode.is_null() { Err(Errno::NoMem) } else { Err(Errno::Raw(ptr_err(new_inode))) };
    }
    // vfs_alloc_inode returns the inode locked.

    let ip = new_inode as *mut xv6fs_inode;
    // SAFETY: `ip`/`new_inode`/`dp` are live.
    unsafe {
        (*ip).dev = (*dp).dev;
        (*new_inode).mode = mode | super::S_IFREG;
        (*new_inode).n_links = 1;
        (*new_inode).size = 0;
    }
    xv6fs_iupdate(ip);

    // Add directory entry (name_buf already prepared above).
    let ret = unsafe { __xv6fs_dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*new_inode).ino as u32) };
    if ret != 0 {
        // TODO: Free inode on failure.
        vfs_iunlock(new_inode);
        return Err(Errno::Raw(ret));
    }

    vfs_iunlock(new_inode); // VFS's vfs_create will re-lock it.
    Ok(new_inode)
}

extern "C" fn __xv6fs_mkdir(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> *mut vfs_inode {
    result_to_errptr(__xv6fs_mkdir_inner(dir, mode, name, name_len))
}

fn __xv6fs_mkdir_inner(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
    if dir.is_null() || name.is_null() || name_len == 0 || name_len > DIRSIZ {
        return Err(Errno::Inval);
    }

    let dp = dir as *mut xv6fs_inode;
    // SAFETY: `dir` is live.
    let xv6_sb = unsafe { (*dir).sb as *mut xv6fs_superblock };

    let mut name_buf = [0u8; DIRSIZ];
    // SAFETY: `name` has at least `name_len` readable bytes.
    unsafe { memmove(name_buf.as_mut_ptr() as *mut c_void, name as *const c_void, name_len) };

    if unsafe { __xv6fs_dir_name_exists(dp, name_buf.as_ptr() as *const c_char) } != 0 {
        return Err(Errno::Exist);
    }

    // SAFETY: `dir` is live.
    let new_inode = vfs_alloc_inode(unsafe { (*dir).sb });
    if is_err_or_null(new_inode) {
        return if new_inode.is_null() { Err(Errno::NoMem) } else { Err(Errno::Raw(ptr_err(new_inode))) };
    }
    // vfs_alloc_inode returns the inode locked.

    let ip = new_inode as *mut xv6fs_inode;
    // SAFETY: `ip`/`new_inode`/`dp` are live.
    unsafe {
        (*ip).dev = (*dp).dev;
        (*new_inode).mode = mode | super::S_IFDIR;
        (*new_inode).n_links = 1;
        (*new_inode).size = 0;
    }

    // Create . and .. entries.
    // SAFETY: `xv6_sb`/`ip`/`dir` are live.
    unsafe {
        if __xv6fs_dirlink(xv6_sb, ip, c".".as_ptr(), (*new_inode).ino as u32) < 0
            || __xv6fs_dirlink(xv6_sb, ip, c"..".as_ptr(), (*dir).ino as u32) < 0
        {
            // TODO: Cleanup on failure.
            vfs_iunlock(new_inode);
            return Err(Errno::Raw(neg(EIO)));
        }
    }

    xv6fs_iupdate(ip);

    // Add directory entry in parent (name_buf already set earlier).
    let ret = unsafe { __xv6fs_dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*new_inode).ino as u32) };
    if ret < 0 {
        vfs_iunlock(new_inode);
        return Err(Errno::Raw(neg(EIO)));
    }

    // Update parent's link count for "..".
    // SAFETY: `dir`/`dp` are live.
    unsafe { (*dir).n_links += 1 };
    xv6fs_iupdate(dp);

    vfs_iunlock(new_inode); // VFS's vfs_mkdir will re-lock it.
    Ok(new_inode)
}

extern "C" fn __xv6fs_unlink(dentry: *mut vfs_dentry, target: *mut vfs_inode) -> c_int {
    if dentry.is_null() || target.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: `dentry`/`target` are live (caller's contract).
    unsafe {
        if (*dentry).sb.is_null() || (*dentry).sb != (*target).sb {
            return neg(EINVAL);
        }
        if (*dentry).ino != (*target).ino {
            return neg(EINVAL);
        }

        // VFS core handled checking for "." and "..".
        let dp = (*dentry).parent as *mut xv6fs_inode;

        let mut off: u32 = 0;
        while (off as i64) < (*(*dentry).parent).size {
            if let Some(mut de) = read_dirent(dp, off, true) {
                if de.inum != 0 {
                    let de_name_len = strnlen(de.name.as_ptr(), DIRSIZ);
                    if (*dentry).name_len as usize == de_name_len && strncmp((*dentry).name, de.name.as_ptr(), (*dentry).name_len as usize) == 0 {
                        // Found -- clear the entry.
                        if de.inum as u64 != (*target).ino {
                            return neg(EINVAL); // Inode number mismatch.
                        }

                        de = core::mem::zeroed();

                        let bn = off / super::BSIZE;
                        let block_off = (off % super::BSIZE) as usize;
                        let addr = xv6fs_bmap(dp, bn);
                        let bp = bread((*dp).dev, addr);
                        if bp.is_null() {
                            // See the module doc's "Fidelity note".
                            return neg(EIO);
                        }
                        memmove(
                            (*bp).data.add(block_off) as *mut c_void,
                            ptr::addr_of!(de) as *const c_void,
                            core::mem::size_of::<dirent>(),
                        );
                        let xv6_sb = (*dentry).sb as *mut xv6fs_superblock;
                        xv6fs_log_write(xv6_sb, bp);
                        brelse(bp);

                        // inode is already locked by vfs_get_inode.
                        (*target).n_links -= 1;

                        let tip = target as *mut xv6fs_inode;
                        xv6fs_iupdate(tip);

                        // Return the target inode -- VFS will call
                        // vfs_iput on it after releasing the superblock
                        // lock. This handles both cases: n_links == 0
                        // (inode freed when refcount reaches 0) or
                        // n_links > 0 (just releases the reference from
                        // vfs_get_inode).
                        return 0;
                    }
                }
            }
            off += DIRENT_SIZE;
        }

        neg(ENOENT)
    }
}

extern "C" fn __xv6fs_rmdir(dentry: *mut vfs_dentry, target: *mut vfs_inode) -> c_int {
    let ret = __xv6fs_unlink(dentry, target);
    if ret == 0 {
        // Decrement parent's link count (for the ".." entry in the
        // removed directory). This mirrors xv6fs_mkdir which increments
        // parent's n_links when creating a subdir.
        // SAFETY: `dentry` is live (unlink succeeded above, so it was
        // valid).
        unsafe {
            let dp = (*dentry).parent as *mut xv6fs_inode;
            (*(*dentry).parent).n_links -= 1;
            xv6fs_iupdate(dp);
        }
    }
    ret
}

extern "C" fn __xv6fs_link(old: *mut vfs_inode, dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> c_int {
    if old.is_null() || dir.is_null() || name.is_null() || name_len > DIRSIZ {
        return neg(EINVAL);
    }
    // SAFETY: `old` is live.
    if super::s_isdir(unsafe { (*old).mode }) {
        return neg(EPERM); // Can't hard link directories.
    }

    let dp = dir as *mut xv6fs_inode;
    let ip = old as *mut xv6fs_inode;
    // SAFETY: `dir` is live.
    let xv6_sb = unsafe { (*dir).sb as *mut xv6fs_superblock };

    let mut name_buf = [0u8; DIRSIZ];
    // SAFETY: `name` has at least `name_len` readable bytes.
    unsafe { memmove(name_buf.as_mut_ptr() as *mut c_void, name as *const c_void, name_len) };

    if unsafe { __xv6fs_dir_name_exists(dp, name_buf.as_ptr() as *const c_char) } != 0 {
        return neg(EEXIST);
    }

    // SAFETY: `old` is live.
    unsafe { (*old).n_links += 1 };
    xv6fs_iupdate(ip);

    let ret = unsafe { __xv6fs_dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*old).ino as u32) };
    if ret != 0 {
        // SAFETY: `old` is live.
        unsafe { (*old).n_links -= 1 };
        xv6fs_iupdate(ip);
        return ret;
    }

    0
}

// ===========================================================================
// Symlink operations
// ===========================================================================

extern "C" fn __xv6fs_readlink(inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> isize {
    if inode.is_null() || buf.is_null() {
        return neg(EINVAL) as isize;
    }
    // SAFETY: `inode` is live.
    if !super::s_islnk(unsafe { (*inode).mode }) {
        return neg(EINVAL) as isize;
    }

    let ip = inode as *mut xv6fs_inode;
    // SAFETY: `inode` is live.
    let link_len = unsafe { (*inode).size } as usize;

    if link_len + 1 > buflen {
        return neg(crate::bindings::ENAMETOOLONG) as isize;
    }

    // Read symlink target from data blocks.
    let mut bytes_read: usize = 0;
    while bytes_read < link_len {
        let bn = (bytes_read as u32) / super::BSIZE;
        let off = (bytes_read as u32) % super::BSIZE;
        let mut n = super::BSIZE - off;
        if n as usize > link_len - bytes_read {
            n = (link_len - bytes_read) as u32;
        }

        let addr = xv6fs_bmap(ip, bn);
        if addr == 0 {
            return neg(EIO) as isize;
        }
        // SAFETY: `ip` is live.
        let bp = bread(unsafe { (*ip).dev }, addr);
        if bp.is_null() {
            return neg(EIO) as isize;
        }
        // SAFETY: `bp`/`buf` are live.
        unsafe {
            memmove(
                buf.add(bytes_read) as *mut c_void,
                (*bp).data.add(off as usize) as *const c_void,
                n as usize,
            );
            brelse(bp);
        }

        bytes_read += n as usize;
    }

    // SAFETY: `buf` has at least `link_len + 1` writable bytes (checked
    // above).
    unsafe { *buf.add(link_len) = 0 };
    link_len as isize
}

extern "C" fn __xv6fs_symlink(
    dir: *mut vfs_inode,
    _mode: mode_t,
    name: *const c_char,
    name_len: usize,
    target: *const c_char,
    target_len: usize,
) -> *mut vfs_inode {
    result_to_errptr(__xv6fs_symlink_inner(dir, _mode, name, name_len, target, target_len))
}

fn __xv6fs_symlink_inner(
    dir: *mut vfs_inode,
    _mode: mode_t,
    name: *const c_char,
    name_len: usize,
    target: *const c_char,
    target_len: usize,
) -> KResult<*mut vfs_inode> {
    if dir.is_null() || name.is_null() || name_len == 0 || name_len > DIRSIZ || target.is_null() || target_len == 0 {
        return Err(Errno::Inval);
    }

    let dp = dir as *mut xv6fs_inode;
    // SAFETY: `dir` is live.
    let xv6_sb = unsafe { (*dir).sb as *mut xv6fs_superblock };

    let mut name_buf = [0u8; DIRSIZ];
    // SAFETY: `name` has at least `name_len` readable bytes.
    unsafe { memmove(name_buf.as_mut_ptr() as *mut c_void, name as *const c_void, name_len) };

    if unsafe { __xv6fs_dir_name_exists(dp, name_buf.as_ptr() as *const c_char) } != 0 {
        return Err(Errno::Exist);
    }

    // SAFETY: `dir` is live.
    let new_inode = vfs_alloc_inode(unsafe { (*dir).sb });
    if is_err_or_null(new_inode) {
        return if new_inode.is_null() { Err(Errno::NoMem) } else { Err(Errno::Raw(ptr_err(new_inode))) };
    }
    // vfs_alloc_inode returns the inode locked.

    let ip = new_inode as *mut xv6fs_inode;
    // SAFETY: `ip`/`new_inode`/`dp` are live.
    unsafe {
        (*ip).dev = (*dp).dev;
        (*new_inode).mode = super::S_IFLNK | 0o777;
        (*new_inode).n_links = 1;
        (*new_inode).size = 0;
    }

    // Write symlink target to data blocks.
    let mut bytes_written: usize = 0;
    while bytes_written < target_len {
        let bn = (bytes_written as u32) / super::BSIZE;
        let off = (bytes_written as u32) % super::BSIZE;
        let mut n = super::BSIZE - off;
        if n as usize > target_len - bytes_written {
            n = (target_len - bytes_written) as u32;
        }

        let addr = xv6fs_bmap(ip, bn);
        if addr == 0 {
            // Failed to allocate block -- cleanup.
            xv6fs_itrunc(ip);
            vfs_iunlock(new_inode);
            return Err(Errno::Raw(neg(ENOSPC)));
        }

        // SAFETY: `ip` is live.
        let bp = bread(unsafe { (*ip).dev }, addr);
        if bp.is_null() {
            xv6fs_itrunc(ip);
            vfs_iunlock(new_inode);
            return Err(Errno::Raw(neg(EIO)));
        }
        // SAFETY: `bp`/`target` are live.
        unsafe {
            memmove(
                (*bp).data.add(off as usize) as *mut c_void,
                target.add(bytes_written) as *const c_void,
                n as usize,
            );
        }
        xv6fs_log_write(xv6_sb, bp);
        brelse(bp);

        bytes_written += n as usize;
    }

    // SAFETY: `new_inode` is live.
    unsafe { (*new_inode).size = target_len as loff_t };
    xv6fs_iupdate(ip);

    // Add directory entry (name_buf already set earlier).
    let ret = unsafe { __xv6fs_dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*new_inode).ino as u32) };
    if ret != 0 {
        xv6fs_itrunc(ip);
        vfs_iunlock(new_inode);
        return Err(Errno::Raw(ret));
    }

    vfs_iunlock(new_inode);
    Ok(new_inode)
}

// ===========================================================================
// Device file operations (mknod)
// ===========================================================================

extern "C" fn __xv6fs_mknod(dir: *mut vfs_inode, mode: mode_t, dev: dev_t, name: *const c_char, name_len: usize) -> *mut vfs_inode {
    result_to_errptr(__xv6fs_mknod_inner(dir, mode, dev, name, name_len))
}

fn __xv6fs_mknod_inner(dir: *mut vfs_inode, mode: mode_t, dev: dev_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
    if dir.is_null() || name.is_null() || name_len == 0 || name_len > DIRSIZ {
        return Err(Errno::Inval);
    }
    // xv6 only supports character and block devices.
    if !super::s_isblk(mode) && !super::s_ischr(mode) {
        return Err(Errno::Inval);
    }

    let dp = dir as *mut xv6fs_inode;
    // SAFETY: `dir` is live.
    let xv6_sb = unsafe { (*dir).sb as *mut xv6fs_superblock };

    // SAFETY: `dir` is live.
    let new_inode = vfs_alloc_inode(unsafe { (*dir).sb });
    if is_err_or_null(new_inode) {
        return if new_inode.is_null() { Err(Errno::NoMem) } else { Err(Errno::Raw(ptr_err(new_inode))) };
    }
    // vfs_alloc_inode returns the inode locked.

    let ip = new_inode as *mut xv6fs_inode;
    // SAFETY: `ip`/`new_inode`/`dp` are live.
    unsafe {
        (*ip).dev = (*dp).dev;
        (*new_inode).mode = mode;
        (*new_inode).n_links = 1;
        (*new_inode).size = 0;

        (*ip).major = major(dev);
        (*ip).minor = minor(dev);
        if super::s_ischr(mode) {
            (*new_inode).dev_mnt.cdev = dev;
        } else if super::s_isblk(mode) {
            (*new_inode).dev_mnt.bdev = dev;
        }
    }

    xv6fs_iupdate(ip);

    let mut name_buf = [0u8; DIRSIZ];
    // SAFETY: `name` has at least `name_len` readable bytes.
    unsafe { memmove(name_buf.as_mut_ptr() as *mut c_void, name as *const c_void, name_len) };

    let ret = unsafe { __xv6fs_dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*new_inode).ino as u32) };
    if ret != 0 {
        // TODO: Free inode on failure.
        vfs_iunlock(new_inode);
        return Err(Errno::Raw(ret));
    }

    vfs_iunlock(new_inode); // VFS's vfs_mknod will re-lock it.
    Ok(new_inode)
}

// ===========================================================================
// Inode lifecycle
// ===========================================================================

/// Mirrors `xv6fs_destroy_inode()`.
pub(crate) extern "C" fn xv6fs_destroy_inode(inode: *mut vfs_inode) {
    if inode.is_null() {
        return;
    }
    let ip = inode as *mut xv6fs_inode;
    // SAFETY: `inode` is live (caller's contract).
    unsafe {
        let xv6_sb = (*inode).sb as *mut xv6fs_superblock;

        // Tear down the pcache (unregister + evict all pages). pcache_teardown
        // waits for any in-flight flush workers to complete. We don't call
        // pcache_flush here because: 1. fflush was called in vfs_fput before
        // releasing the file reference. 2. The data is being truncated below
        // anyway.
        if (*inode).i_data.flags.bits.active() != 0 {
            pcache_teardown(ptr::addr_of_mut!((*inode).i_data));
        }

        // Note: Transaction is managed by VFS layer (vfs_iput calls
        // begin/end_transaction).
        xv6fs_itrunc(ip);

        // Mark inode as free on disk.
        let bp = bread((*ip).dev, super::xv6fs_iblock((*inode).ino, (*xv6_sb).disk_sb.inodestart));
        if bp.is_null() {
            // See the module doc's "Fidelity note": the C original does
            // not NULL-check here either, but an unconditional deref
            // would be Rust UB.
            return;
        }
        let dip = ((*bp).data as *mut dinode).add(((*inode).ino % super::IPB) as usize);
        (*dip).type_ = 0;
        xv6fs_log_write(xv6_sb, bp);
        brelse(bp);
    }
}

/// Mirrors `xv6fs_free_inode()`.
pub(crate) extern "C" fn xv6fs_free_inode(inode: *mut vfs_inode) {
    if inode.is_null() {
        return;
    }
    // Tear down the pcache (unregister + evict all pages). pcache_teardown
    // waits for any in-flight flush workers to complete. We don't call
    // pcache_flush here because fflush was called in vfs_fput before
    // releasing the file reference, so pages should already be clean.
    // SAFETY: `inode` is live.
    unsafe {
        if (*inode).i_data.flags.bits.active() != 0 {
            pcache_teardown(ptr::addr_of_mut!((*inode).i_data));
        }
    }

    let ip = inode as *mut xv6fs_inode;
    slab_free(ip as *mut c_void);
}

// ===========================================================================
// Open callback
// ===========================================================================

/// Mirrors `xv6fs_open()`.
pub(crate) extern "C" fn xv6fs_open(inode: *mut vfs_inode, file: *mut vfs_file, _f_flags: c_int) -> c_int {
    if inode.is_null() || file.is_null() {
        return neg(EINVAL);
    }

    // SAFETY: `inode` is live.
    let mode = unsafe { (*inode).mode };

    if super::s_isreg(mode) {
        // SAFETY: `file` is live.
        unsafe {
            (*file).ops = ptr::addr_of!(super::file::XV6FS_FILE_OPS) as *mut _;
            // Lazily initialise the per-inode page cache on first open.
            // VFS calls open with the inode locked, so this is race-free.
            if (*inode).i_data.flags.bits.active() == 0 {
                super::superblock::xv6fs_inode_pcache_init(inode);
            }
        }
        return 0;
    }

    if super::s_isdir(mode) {
        // Directories use dir_iter for reading.
        // SAFETY: `file` is live.
        unsafe { (*file).ops = ptr::addr_of!(super::file::XV6FS_FILE_OPS) as *mut _ };
        return 0;
    }

    if super::s_islnk(mode) {
        // FIX (preserved from C): allow opening symlinks with O_NOFOLLOW
        // flag. POSIX requires that symlinks can be opened with
        // O_NOFOLLOW to allow fstat() on the symlink itself (not its
        // target). This is needed by programs like ls that want to
        // display symlink information.
        // SAFETY: `file` is live.
        unsafe { (*file).ops = ptr::addr_of!(super::file::XV6FS_FILE_OPS) as *mut _ };
        return 0;
    }

    // Character/block devices are handled by VFS core.
    if super::s_ischr(mode) || super::s_isblk(mode) {
        return neg(EINVAL); // Should be handled by VFS.
    }

    neg(crate::bindings::ENOSYS)
}

extern "C" fn __xv6fs_getattr(inode: *mut vfs_inode, stat: *mut stat) -> c_int {
    if inode.is_null() || stat.is_null() {
        return neg(EINVAL);
    }
    let ip = inode as *mut xv6fs_inode;
    vfs_ilock(inode);
    // SAFETY: `inode`/`stat` are live and `inode` is now locked.
    unsafe {
        ptr::write_bytes(stat, 0, 1);
        (*stat).dev = (*ip).dev as i32;
        (*stat).ino = (*inode).ino;
        (*stat).mode = (*inode).mode;
        (*stat).nlink = (*inode).n_links;
        (*stat).size = (*inode).size as u64;
    }
    vfs_iunlock(inode);
    0
}

extern "C" fn __xv6fs_setattr(_inode: *mut vfs_inode, _stat: *const stat) -> c_int {
    neg(EOPNOTSUPP)
}

// ===========================================================================
// VFS inode operations structure
// ===========================================================================

pub(crate) static XV6FS_INODE_OPS: vfs_inode_ops = vfs_inode_ops {
    lookup: Some(__xv6fs_lookup),
    dir_iter: Some(__xv6fs_dir_iter),
    readlink: Some(__xv6fs_readlink),
    getattr: Some(__xv6fs_getattr),
    setattr: Some(__xv6fs_setattr),
    create: Some(__xv6fs_create),
    link: Some(__xv6fs_link),
    unlink: Some(__xv6fs_unlink),
    mkdir: Some(__xv6fs_mkdir),
    rmdir: Some(__xv6fs_rmdir),
    mknod: Some(__xv6fs_mknod),
    move_: None, // TODO: Implement move/rename.
    symlink: Some(__xv6fs_symlink),
    truncate: Some(xv6fs_truncate),
    destroy_inode: Some(xv6fs_destroy_inode),
    free_inode: Some(xv6fs_free_inode),
    dirty_inode: Some(xv6fs_dirty_inode),
    sync_inode: Some(xv6fs_sync_inode),
    open: Some(xv6fs_open),
};
