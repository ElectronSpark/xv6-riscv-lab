//! xv6fs file operations — Rust port of `kernel/vfs/xv6fs/file.c`
//! (Phase 2 Wave 19, sub-wave B; see `super` module doc and
//! `docs/rustify/phase2_plan.md`).
//!
//! Every `vfs_file_ops` callback and `struct vfs_file_ops xv6fs_file_ops`
//! itself. Data goes through the per-inode pcache
//! ([`super::superblock`]'s `xv6fs_inode_pcache_init`/`read_page`/
//! `write_page`, wired up at mount and lazily at first open): user bytes
//! are copied into pcache pages marked dirty, and the pcache flusher
//! thread writes dirty pages back to disk via `bio` (data=writeback
//! semantics — data blocks are **not** logged). Metadata (block
//! allocation, inode size) still goes through the log for crash
//! consistency; each write transaction chunk covers `bmap` + `iupdate`
//! only.
//!
//! # Locking design: driver-managed inode locks
//!
//! VFS file operations (`vfs_fileread`/`vfs_filewrite`/...) do NOT
//! acquire the inode lock before calling into the driver. Instead, each
//! driver callback here is responsible for acquiring the inode lock when
//! needed. This is necessary because:
//! 1. [`__xv6fs_file_write`] needs to acquire a transaction (`begin_op`)
//!    BEFORE locking the inode, to match VFS lock ordering: transaction
//!    -> superblock -> inode.
//! 2. If VFS held the inode lock when calling write, and write called
//!    `begin_op`, it would deadlock with other paths that do
//!    `begin_op` -> `ilock`.
//!
//! Lock ordering: [`__xv6fs_file_write`] is `begin_op -> vfs_ilock ->
//! work -> vfs_iunlock -> end_op`; [`__xv6fs_file_read`]/
//! [`__xv6fs_file_llseek`] (`SEEK_END`) are `vfs_ilock -> read ->
//! vfs_iunlock` (no transaction needed). The VFS file lock (per-file
//! mutex) still serializes concurrent operations on the same file
//! descriptor and protects the file position.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings::{
    bool_, loff_t, page_t, pcache, pcache_node, thread, vfs_file, vfs_file_ops, vfs_inode, vma, EFAULT, EFBIG, EINTR,
    EINVAL, EIO, ENOMEM, ENOSPC,
};

use crate::proc::proc_shims::xv6_current_thread;

use super::log::{xv6fs_begin_op, xv6fs_end_op};
use super::truncate::xv6fs_bmap;

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention (this
// block: symbols outside the xv6fs driver).
// ===========================================================================

// P3-D2b: `signal_pending` (proc/signal.rs) is a plain crate-path item
// now that its `#[no_mangle]` export is gone. The old redeclaration here
// said `-> bool_` (c_uint); the real fn returns `bool` (same 0/1 in `a0`
// under the old C ABI), so the call sites drop their `!= 0`.
use crate::proc::signal_pending;

unsafe extern "C" {
    // string.rs.
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

}

// P3-D3a: the mm-cluster entry points are ordinary Rust fns now that
// their `#[no_mangle]` exports are gone; the vm/pcache ones are safe fns
// with identical signatures (pcache.rs's `Pcache`/`Page`/`PcacheNode`
// are aliases of the same bindgen `pcache`/`page_t`/`pcache_node` this
// file already uses) — plain `use` instead of the `extern "C"`
// redeclarations that used to sit in the block above.
use crate::mm::{
    pcache_flush, pcache_get_page, pcache_mark_page_dirty, pcache_put_page, pcache_read_page,
    vm_copyin, vm_copyout, xv6_page_pcache_get_node,
};

// `page_alloc`/`page_free` are genuinely `unsafe fn` in
// `crate::mm::page`; this file's original extern declarations asserted
// `safe fn` (usual FFI facade). Thin safe wrappers preserve that facade,
// per the `cffi::raw` precedent.
/// SAFETY: see [`crate::mm::page::page_alloc`]'s contract.
#[inline]
fn page_alloc(order: u64, flags: u64) -> *mut c_void {
    unsafe { crate::mm::page_alloc(order, flags) }
}
/// SAFETY: `ptr` must originate from `page_alloc` above.
#[inline]
fn page_free(ptr: *mut c_void, order: u64) {
    unsafe { crate::mm::page_free(ptr, order) };
}

// P3-1C mesh sweep: vfs/{fs,inode}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures).
use crate::vfs::fs::vfs_inode_deref;
use crate::vfs::inode::{vfs_ilock, vfs_iunlock};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}
#[inline(always)]
fn current() -> *mut thread {
    xv6_current_thread()
}

/// `PAGE_TYPE_ANON` -- local hardcoded copy, same rationale/precedent as
/// `tmpfs/file.rs`'s own copy (`#define`s bindgen can't pick up).
const PAGE_TYPE_ANON: u64 = 0;

/// Blocks-per-page constants for pcache <-> xv6fs block address
/// translation, mirroring the C original's own per-file `#define`s (both
/// `superblock.c` and `file.c` independently defined the same two
/// constants -- this port preserves that redundancy rather than
/// centralizing, matching the C's own structure).
///
/// One pcache page (4KB) covers `BSIZE_PER_PAGE` (4) xv6fs blocks
/// (`BSIZE` = 1024). Block addresses from `bmap` are in `BSIZE` units;
/// pcache uses 512-byte units.
const BSIZE_PER_PAGE: u32 = crate::bindings::PGSIZE as u32 / super::BSIZE;
const BLK512_PER_BSIZE: u64 = super::BSIZE as u64 / 512;

// ===========================================================================
// File read
// ===========================================================================

extern "C" fn __xv6fs_file_read(file: *mut vfs_file, buf: *mut c_char, mut count: usize, user: bool_) -> isize {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    // SAFETY: `inode` is live.
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };

    if !super::s_isreg(unsafe { (*inode).mode }) {
        return neg(EINVAL) as isize;
    }

    // SAFETY: `pc` is live.
    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() } == 0 {
        return neg(EIO) as isize;
    }

    // Acquire inode lock to safely read size and prevent truncation
    // during read. Read doesn't need a transaction (no modifications),
    // so we can just lock the inode. The file reference guarantees the
    // inode remains allocated -- no validity check needed per Linux VFS
    // model (file holds inode reference).
    vfs_ilock(inode);

    // SAFETY: `file`/`inode` are live and `inode` is locked.
    let mut pos = unsafe { (*file).__bindgen_anon_1.f_pos };
    let size = unsafe { (*inode).size };
    if pos >= size {
        vfs_iunlock(inode);
        return 0; // EOF.
    }
    if pos + count as loff_t > size {
        count = (size - pos) as usize;
    }

    let mut bytes_read: usize = 0;
    while bytes_read < count {
        let bn = (pos as u32) / super::BSIZE;
        let off = (pos as u32) % super::BSIZE;
        let mut n = super::BSIZE - off;
        if n as usize > count - bytes_read {
            n = (count - bytes_read) as u32;
        }

        // Per-inode pcache path (keyed by logical file offset). The
        // read_page callback handles bmap + bio internally, including
        // zero-filling of sparse blocks, so no bmap call is needed here.
        let blkno_512 = bn as u64 * BLK512_PER_BSIZE;
        let page = pcache_get_page(pc, blkno_512);
        if page.is_null() {
            vfs_iunlock(inode);
            if bytes_read > 0 {
                return bytes_read as isize;
            }
            return if signal_pending(current()) { neg(EINTR) as isize } else { neg(EIO) as isize };
        }
        let ret = pcache_read_page(pc, page);
        if ret != 0 {
            pcache_put_page(pc, page);
            vfs_iunlock(inode);
            if bytes_read > 0 {
                return bytes_read as isize;
            }
            return if ret == neg(EINTR) { neg(EINTR) as isize } else { neg(EIO) as isize };
        }
        let pcn = xv6_page_pcache_get_node(page);
        let page_off = (bn % BSIZE_PER_PAGE) * super::BSIZE + off;
        // SAFETY: `pcn` is a live, just-populated pcache node; `page_off`
        // is within the page's `PGSIZE` bytes.
        let data = unsafe { ((*pcn).data as *mut u8).add(page_off as usize) };
        if user != 0 {
            // SAFETY: `current()` is the live running thread; `buf +
            // bytes_read` is a user-space destination checked by
            // `vm_copyout` itself.
            if vm_copyout(unsafe { (*current()).vm }, unsafe { buf.add(bytes_read) } as u64, data as *const c_void, n as u64) < 0 {
                pcache_put_page(pc, page);
                vfs_iunlock(inode);
                if bytes_read == 0 {
                    return neg(EFAULT) as isize;
                }
                return bytes_read as isize;
            }
        } else {
            // SAFETY: `buf + bytes_read` has at least `n` writable bytes
            // (caller's contract for `user == false`, matching the C
            // original).
            unsafe { memmove(buf.add(bytes_read) as *mut c_void, data as *const c_void, n as usize) };
        }
        pcache_put_page(pc, page);

        bytes_read += n as usize;
        pos += n as loff_t;
    }

    vfs_iunlock(inode);
    bytes_read as isize
}

// ===========================================================================
// File write
//
// Data goes through the per-inode pcache: user bytes are copied into
// pcache pages which are marked dirty. The pcache flusher thread writes
// dirty pages back to disk via bio (data=writeback semantics).
//
// Metadata (block allocation, inode size) still goes through the log for
// crash consistency. Each transaction chunk covers bmap + iupdate only;
// data blocks are NOT logged.
// ===========================================================================

extern "C" fn __xv6fs_file_write(file: *mut vfs_file, buf: *const c_char, count: usize, user: bool_) -> isize {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    let ip = inode as *mut crate::bindings::xv6fs_inode;
    // SAFETY: `inode` is live.
    let xv6_sb = unsafe { (*inode).sb as *mut crate::bindings::xv6fs_superblock };
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };

    if !super::s_isreg(unsafe { (*inode).mode }) {
        return neg(EINVAL) as isize;
    }

    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() } == 0 {
        return neg(EIO) as isize;
    }

    // SAFETY: `file` is live.
    let mut pos = unsafe { (*file).__bindgen_anon_1.f_pos };
    let end_pos = pos + count as loff_t;

    // Check file size limit.
    if end_pos > super::MAXFILE as loff_t * super::BSIZE as loff_t {
        return neg(EFBIG) as isize;
    }

    // Write in chunks to avoid exceeding log transaction size. Only
    // metadata (bmap allocations + iupdate) goes through the log now;
    // data blocks are written by the pcache flusher via bio.
    let max = ((super::MAXOPBLOCKS - 1 - 1 - 2) / 2) as usize * super::BSIZE as usize;
    let mut bytes_written: usize = 0;

    while bytes_written < count {
        let mut n = count - bytes_written;
        if n > max {
            n = max;
        }

        // Acquire transaction BEFORE inode lock to match VFS locking
        // order: transaction -> superblock -> inode. VFS releases the
        // inode lock before calling this function to avoid deadlock.
        let begin_ret = xv6fs_begin_op(xv6_sb);
        if begin_ret != 0 {
            if bytes_written == 0 {
                return begin_ret as isize;
            }
            return bytes_written as isize;
        }

        // Now acquire the inode lock to protect inode metadata during
        // write. The file reference guarantees the inode remains
        // allocated.
        vfs_ilock(inode);

        // `goto done;` in the C original always jumps straight to
        // `return bytes_written;` (the function's tail) with no other
        // work in between -- so every occurrence below is transliterated
        // as a direct `return bytes_written as isize;`, not a jump/break;
        // `bytes_written` has not yet been bumped by this (partial)
        // chunk's `chunk_written` at any of these points, matching what
        // the C's `done:` label would have returned too.
        let mut chunk_written: usize = 0;
        while chunk_written < n {
            let bn = (pos as u32) / super::BSIZE;
            let off = (pos as u32) % super::BSIZE;
            let mut chunk = super::BSIZE - off;
            if chunk as usize > n - chunk_written {
                chunk = (n - chunk_written) as u32;
            }

            // Ensure the block is allocated (may log indirect-block changes).
            let addr = xv6fs_bmap(ip, bn);
            if addr == 0 {
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if bytes_written == 0 {
                    return neg(ENOSPC) as isize;
                }
                return bytes_written as isize;
            }

            // Write data through the per-inode pcache.
            let blkno_512 = bn as u64 * BLK512_PER_BSIZE;
            let page = pcache_get_page(pc, blkno_512);
            if page.is_null() {
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if bytes_written > 0 {
                    return bytes_written as isize;
                }
                return if signal_pending(current()) { neg(EINTR) as isize } else { neg(EIO) as isize };
            }
            let ret = pcache_read_page(pc, page);
            if ret != 0 {
                pcache_put_page(pc, page);
                vfs_iunlock(inode);
                xv6fs_end_op(xv6_sb);
                if bytes_written > 0 {
                    return bytes_written as isize;
                }
                return if ret == neg(EINTR) { neg(EINTR) as isize } else { neg(EIO) as isize };
            }

            let pcn = xv6_page_pcache_get_node(page);
            let page_off = (bn % BSIZE_PER_PAGE) * super::BSIZE + off;
            // SAFETY: `pcn` is live; `page_off` is within `PGSIZE`.
            let data = unsafe { ((*pcn).data as *mut u8).add(page_off as usize) };

            if user != 0 {
                // SAFETY: `current()` is the live running thread.
                if vm_copyin(
                    unsafe { (*current()).vm },
                    data as *mut c_void,
                    unsafe { buf.add(bytes_written + chunk_written) } as u64,
                    chunk as u64,
                ) < 0
                {
                    pcache_put_page(pc, page);
                    vfs_iunlock(inode);
                    xv6fs_end_op(xv6_sb);
                    if bytes_written == 0 {
                        return neg(EFAULT) as isize;
                    }
                    return bytes_written as isize;
                }
            } else {
                // SAFETY: `buf + bytes_written + chunk_written` has at
                // least `chunk` readable bytes (caller's contract for
                // `user == false`).
                unsafe { memmove(data as *mut c_void, buf.add(bytes_written + chunk_written) as *const c_void, chunk as usize) };
            }
            pcache_mark_page_dirty(pc, page);
            pcache_put_page(pc, page);

            chunk_written += chunk as usize;
            pos += chunk as loff_t;
        }

        // Update size if we extended the file.
        // SAFETY: `inode` is live and locked.
        unsafe {
            if pos > (*inode).size {
                (*inode).size = pos;
            }
        }
        super::inode::xv6fs_iupdate(ip);

        // Release inode lock before ending transaction.
        vfs_iunlock(inode);
        xv6fs_end_op(xv6_sb);

        bytes_written += chunk_written;
    }

    bytes_written as isize
}

// ===========================================================================
// File seek
// ===========================================================================

extern "C" fn __xv6fs_file_llseek(file: *mut vfs_file, offset: loff_t, whence: c_int) -> loff_t {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };

    const SEEK_SET: c_int = 0;
    const SEEK_CUR: c_int = 1;
    const SEEK_END: c_int = 2;

    let new_pos = match whence {
        SEEK_SET => offset,
        SEEK_CUR => unsafe { (*file).__bindgen_anon_1.f_pos + offset },
        SEEK_END => {
            // Need to lock the inode to safely read size.
            vfs_ilock(inode);
            let sz = unsafe { (*inode).size };
            vfs_iunlock(inode);
            sz + offset
        }
        _ => return neg(EINVAL) as loff_t,
    };

    if new_pos < 0 {
        return neg(EINVAL) as loff_t;
    }

    new_pos
}

// ===========================================================================
// File fsync/fflush -- flush (a range of) dirty pcache pages to disk
// ===========================================================================

extern "C" fn __xv6fs_file_fsync(file: *mut vfs_file, _start: loff_t, _len: loff_t) -> c_int {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    if inode.is_null() {
        return 0;
    }
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };
    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() } == 0 {
        return 0;
    }
    // TODO: implement range-based flush (matches the C original's own TODO).
    pcache_flush(pc)
}

extern "C" fn __xv6fs_file_fflush(file: *mut vfs_file) -> c_int {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    if inode.is_null() {
        return 0;
    }
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };
    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() } == 0 {
        return 0;
    }
    pcache_flush(pc)
}

// ===========================================================================
// Page fault handler for file-backed mmap
// ===========================================================================

/// Allocates a fresh anonymous page and populates it with file data read
/// from the per-inode pcache.
///
/// Because the faulting offset is always page-aligned (both `vma->pgoff`
/// and `va` are page-aligned), the logical block number (`bn = file_off /
/// BSIZE`) is always a multiple of `BSIZE_PER_PAGE`. This means the
/// pcache page starts at offset 0 within the data, and a full `PGSIZE`
/// (or less for the last partial page) can be copied directly from
/// `pcn->data`.
///
/// The inode lock is held while reading size and pcache data to prevent
/// races with concurrent truncate or write.
extern "C" fn __xv6fs_file_fault(file: *mut vfs_file, vma_ptr: *mut vma, va: u64) -> *mut c_void {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    if inode.is_null() {
        return ptr::null_mut();
    }
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };

    // file_off is always page-aligned (both pgoff and va are page-aligned).
    // SAFETY: `vma_ptr` is live (caller's contract).
    let file_off = unsafe { (*vma_ptr).pgoff + (va - (*vma_ptr).start) };

    let pa = page_alloc(0, PAGE_TYPE_ANON);
    if pa.is_null() {
        return ptr::null_mut();
    }

    vfs_ilock(inode);

    // Entirely beyond EOF -- return a zero page.
    if file_off >= unsafe { (*inode).size } as u64 {
        vfs_iunlock(inode);
        // SAFETY: `pa` is a freshly allocated, exclusively-owned page.
        unsafe { ptr::write_bytes(pa as *mut u8, 0, crate::bindings::PGSIZE as usize) };
        return pa;
    }

    let mut bytes_to_read = crate::bindings::PGSIZE as u64;
    if file_off + crate::bindings::PGSIZE as u64 > unsafe { (*inode).size } as u64 {
        bytes_to_read = unsafe { (*inode).size } as u64 - file_off;
    }

    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() } == 0 {
        vfs_iunlock(inode);
        page_free(pa, 0);
        return ptr::null_mut();
    }

    // Convert file offset to xv6fs pcache key (512-byte units). Since
    // file_off is page-aligned, bn is a multiple of BSIZE_PER_PAGE, so
    // the target data begins at offset 0 within the pcache page.
    let bn = (file_off / super::BSIZE as u64) as u32;
    let blkno_512 = bn as u64 * BLK512_PER_BSIZE;

    let pcpage = pcache_get_page(pc, blkno_512);
    if pcpage.is_null() {
        vfs_iunlock(inode);
        page_free(pa, 0);
        return ptr::null_mut();
    }
    let ret = pcache_read_page(pc, pcpage);
    if ret != 0 {
        pcache_put_page(pc, pcpage);
        vfs_iunlock(inode);
        page_free(pa, 0);
        return ptr::null_mut();
    }

    let pcn = xv6_page_pcache_get_node(pcpage);
    // SAFETY: `pcn` is live and just populated; `pa` is a freshly
    // allocated, exclusively-owned page of `PGSIZE` bytes.
    unsafe {
        memmove(pa, (*pcn).data as *const c_void, bytes_to_read as usize);
        if bytes_to_read < crate::bindings::PGSIZE as u64 {
            ptr::write_bytes(
                (pa as *mut u8).add(bytes_to_read as usize),
                0,
                crate::bindings::PGSIZE as usize - bytes_to_read as usize,
            );
        }
    }

    pcache_put_page(pc, pcpage);
    vfs_iunlock(inode);
    pa
}

// ===========================================================================
// VFS file operations structure
// ===========================================================================

pub(crate) static XV6FS_FILE_OPS: vfs_file_ops = vfs_file_ops {
    read: Some(__xv6fs_file_read),
    write: Some(__xv6fs_file_write),
    llseek: Some(__xv6fs_file_llseek),
    release: None,
    fsync: Some(__xv6fs_file_fsync),
    fflush: Some(__xv6fs_file_fflush),
    poll: None,
    ioctl: None,
    fault: Some(__xv6fs_file_fault),
};
