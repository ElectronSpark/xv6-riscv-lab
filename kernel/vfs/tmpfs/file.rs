//! tmpfs file operations — Rust port of `kernel/vfs/tmpfs/file.c`
//! (Phase 2 Wave 18, sub-wave B; see `super` module doc and
//! `docs/rustify/phase2_plan.md`).
//!
//! Implements the VFS file operations for tmpfs regular files, the
//! per-inode pcache wiring, and the ops instance itself (since wave
//! P3-10a a [`crate::vfs::file::FileOps`] trait implementor,
//! [`TMPFS_FILE_OPS`], replacing the C-style `struct vfs_file_ops`
//! fn-pointer table).
//!
//! # Locking design: driver-managed inode locks
//!
//! VFS file operations (`vfs_fileread`/`vfs_filewrite`/...) do NOT
//! acquire the inode lock before calling into the driver. Instead, each
//! driver callback is responsible for acquiring the inode lock when
//! needed. For tmpfs, the inode lock protects `size` and data access.
//! Unlike xv6fs, tmpfs doesn't have transactions, so the locking is
//! simpler.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings::{
    loff_t, page_t, pcache, pcache_node, pcache_ops, thread, tmpfs_inode, vfs_file,
    vfs_inode, vma, EINVAL,
};

// P3-10a: the ops table is the `FileOps` trait now (`&'static dyn`
// dispatch, see `vfs/file.rs`); the file callbacks return [`KResult`]
// natively. `EINVAL` (raw) survives only for `tmpfs_open` — an
// *inode*-ops callback, outside this wave's family.
use crate::kstd::{Errno, KResult};
use crate::vfs::file::FileOps;

use crate::proc::proc_shims::xv6_current_thread;

use super::{s_isdir, s_islnk, s_isreg, TMPFS_MAX_FILE_SIZE};

// P3-1C mesh sweep: vfs/{inode,fs}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures).
use crate::vfs::fs::FsStruct;
use crate::vfs::inode::VfsInode;

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

unsafe extern "C" {
    // string.rs.
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;

}

// P3-D3a: the mm-cluster entry points are ordinary Rust fns now that
// their `#[no_mangle]` exports are gone; the vm/pcache ones are safe fns
// with identical signatures (pcache.rs's `Pcache`/`Page`/`PcacheNode`
// are aliases of the same bindgen `pcache`/`page_t`/`pcache_node` this
// file already uses) — plain `use` instead of the `extern "C"`
// redeclarations that used to sit in the block above.
use crate::mm::{Pcache, Vm};

// `page_alloc`/`page_free` are genuinely `unsafe fn` in
// `crate::mm::page`; this file's original extern declarations asserted
// `safe fn` (usual FFI facade). Thin safe wrappers preserve that facade,
// per the `cffi::raw` precedent.
/// SAFETY: see [`crate::mm::page::page_alloc`]'s contract.
#[inline]
fn page_alloc(order: u64, flags: u64) -> *mut c_void {
    unsafe { crate::mm::page::Page::page_alloc(order, flags) }
}
/// SAFETY: `ptr` must originate from `page_alloc` above.
#[inline]
fn page_free(ptr: *mut c_void, order: u64) {
    unsafe { crate::mm::page::Page::page_free(ptr, order) };
}

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// `mm/vm.rs`'s local hardcoded constant (`PAGE_TYPE_ANON`, `#define`s
/// bindgen can't pick up) -- duplicated here per this crate's established
/// per-file self-contained convention (same value, same rationale as
/// `vm.rs`'s own copy).
const PAGE_TYPE_ANON: u64 = 0;

/* Convert block size to 512-byte units for pcache. */
const PCACHE_BLKS_PER_PAGE: u64 = crate::bindings::PGSIZE as u64 / 512;

#[inline(always)]
fn tmpfs_iblock(pos: loff_t) -> loff_t {
    pos >> crate::bindings::PAGE_SHIFT
}
#[inline(always)]
fn tmpfs_iblock_offset(pos: loff_t) -> loff_t {
    pos & (crate::bindings::PGSIZE as loff_t - 1)
}

// ===========================================================================
// tmpfs pcache operations.
//
// For tmpfs (a backendless file system), the pcache IS the backing
// store:
//   - read_page: zero-fill the page (for holes/first access)
//   - write_page: no-op (data stays in memory, no disk to persist to)
// ===========================================================================

extern "C" fn tmpfs_pcache_read_page(_pcache: *mut pcache, page: *mut page_t) -> c_int {
    let pcn = Pcache::page_get_node(page);
    // SAFETY: `pcn` is a live `pcache_node` (caller's contract, matches
    // the C `pcache_ops.read_page` callback convention).
    unsafe { memset((*pcn).data, 0, crate::bindings::PGSIZE as usize) };
    0
}

extern "C" fn tmpfs_pcache_write_page(_pcache: *mut pcache, _page: *mut page_t) -> c_int {
    // No-op for tmpfs -- data stays in memory, nothing to persist.
    0
}

static TMPFS_PCACHE_OPS: pcache_ops = pcache_ops {
    read_page: Some(tmpfs_pcache_read_page),
    write_page: Some(tmpfs_pcache_write_page),
    write_begin: None,
    write_end: None,
    mark_dirty: None,
};

/// Initialize the embedded per-inode pcache (`i_data`) for tmpfs. Call
/// once for every regular-file inode after deciding to use pcache.
///
/// Kept `#[no_mangle]`/exported per `tmpfs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn tmpfs_inode_pcache_init(inode: *mut vfs_inode) {
    // SAFETY: `inode` is live (caller's contract).
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };
    // SAFETY: `pc` is embedded storage inside `inode`, exclusively owned
    // by this call (caller holds the inode mutex).
    unsafe {
        ptr::write_bytes(pc, 0, 1);
        (*pc).ops = ptr::addr_of!(TMPFS_PCACHE_OPS) as *mut pcache_ops;
        // blk_count in 512-byte units, rounded up to page boundary.
        (*pc).blk_count = (TMPFS_MAX_FILE_SIZE / 512 + PCACHE_BLKS_PER_PAGE - 1)
            & !(PCACHE_BLKS_PER_PAGE - 1);
    }

    let ret = Pcache::init(pc);
    if ret != 0 {
        return; // proceed without pcache
    }

    // pcache_init resets private_data, so set it after init.
    // SAFETY: `pc` is live.
    unsafe { (*pc).private_data = inode as *mut c_void };
}

/// Teardown the per-inode pcache for tmpfs. Call when destroying a
/// regular file inode.
///
/// Kept `#[no_mangle]`/exported per `tmpfs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn tmpfs_inode_pcache_teardown(inode: *mut vfs_inode) {
    // SAFETY: `inode` is live (caller's contract).
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };
    // SAFETY: `pc` is live embedded storage.
    if unsafe { (*pc).flags.bits.active() != 0 } {
        Pcache::teardown(pc);
    }
}

fn current() -> *mut thread {
    xv6_current_thread()
}

fn __tmpfs_file_read(file: *mut vfs_file, buf: *mut c_char, mut count: usize, user: bool) -> KResult<isize> {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { FsStruct::vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    let ti = inode as *mut tmpfs_inode;
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };

    // SAFETY: `inode` is live.
    if !s_isreg(unsafe { (*inode).mode }) {
        return Err(Errno::Inval);
    }

    // Acquire inode lock to safely read size and data. The file
    // reference guarantees the inode remains allocated.
    VfsInode::vfs_ilock(inode);

    // SAFETY: `file`/`inode` are live and locked.
    let mut pos = unsafe { (*file).pos.f_pos };
    let size = unsafe { (*inode).size };
    if pos >= size {
        VfsInode::vfs_iunlock(inode);
        return Ok(0); // EOF
    }
    if pos + count as loff_t > size {
        count = (size - pos) as usize;
    }

    // Handle embedded data.
    // SAFETY: `ti` is live.
    if unsafe { (*ti).embedded } != 0 {
        if pos as usize + count > super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN {
            // This shouldn't happen -- embedded files are limited in size.
            count = super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN - pos as usize;
        }
        // SAFETY: `ti` is live and locked.
        let src = unsafe { super::inode::TmpfsInode::embedded_data(ti).add(pos as usize) };
        if user {
            // SAFETY: `current()` is the live running thread.
            if Vm::vm_copyout(unsafe { (*current()).vm }, buf as u64, src as *const c_void, count as u64) < 0 {
                VfsInode::vfs_iunlock(inode);
                return Err(Errno::Fault);
            }
        } else {
            unsafe { memmove(buf as *mut c_void, src as *const c_void, count) };
        }
        VfsInode::vfs_iunlock(inode);
        return Ok(count as isize);
    }

    // pcache-based read.
    if unsafe { (*pc).flags.bits.active() == 0 } {
        VfsInode::vfs_iunlock(inode);
        return Err(Errno::Io);
    }

    let mut bytes_read: usize = 0;
    while bytes_read < count {
        let block_idx = tmpfs_iblock(pos) as u64;
        let block_off = tmpfs_iblock_offset(pos) as usize;
        let mut chunk = crate::bindings::PGSIZE as usize - block_off;
        if chunk > count - bytes_read {
            chunk = count - bytes_read;
        }

        let blkno_512 = block_idx * PCACHE_BLKS_PER_PAGE;
        let page = Pcache::get_page(pc, blkno_512);
        if page.is_null() {
            VfsInode::vfs_iunlock(inode);
            if bytes_read == 0 {
                return Err(Errno::Io);
            }
            return Ok(bytes_read as isize);
        }
        let ret = Pcache::read_page(pc, page);
        if ret != 0 {
            Pcache::put_page(pc, page);
            VfsInode::vfs_iunlock(inode);
            if bytes_read == 0 {
                return Err(Errno::Io);
            }
            return Ok(bytes_read as isize);
        }
        let pcn = Pcache::page_get_node(page);
        // SAFETY: `pcn` is live; `block_off < PGSIZE`.
        let data = unsafe { ((*pcn).data as *mut u8).add(block_off) };

        if user {
            // SAFETY: `current()` is the live running thread.
            if Vm::vm_copyout(unsafe { (*current()).vm }, unsafe { buf.add(bytes_read) } as u64, data as *const c_void, chunk as u64) < 0 {
                Pcache::put_page(pc, page);
                VfsInode::vfs_iunlock(inode);
                if bytes_read == 0 {
                    return Err(Errno::Fault);
                }
                return Ok(bytes_read as isize);
            }
        } else {
            unsafe { memmove(buf.add(bytes_read) as *mut c_void, data as *const c_void, chunk) };
        }
        Pcache::put_page(pc, page);

        bytes_read += chunk;
        pos += chunk as loff_t;
    }

    VfsInode::vfs_iunlock(inode);
    Ok(bytes_read as isize)
}

fn __tmpfs_file_write(file: *mut vfs_file, buf: *const c_char, count_in: usize, user: bool) -> KResult<isize> {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { FsStruct::vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    let ti = inode as *mut tmpfs_inode;
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };
    let count = count_in;

    // SAFETY: `inode` is live.
    if !s_isreg(unsafe { (*inode).mode }) {
        return Err(Errno::Inval);
    }

    // Acquire inode lock to protect size and data. The file reference
    // guarantees the inode remains allocated.
    VfsInode::vfs_ilock(inode);

    // SAFETY: `file` is live and `inode` is locked.
    let mut pos = unsafe { (*file).pos.f_pos };
    let end_pos = pos + count as loff_t;

    // Check for file size limits.
    if end_pos > TMPFS_MAX_FILE_SIZE as loff_t {
        VfsInode::vfs_iunlock(inode);
        return Err(Errno::FBig);
    }

    // Handle embedded data.
    // SAFETY: `ti` is live.
    if unsafe { (*ti).embedded } != 0 {
        if end_pos as usize <= super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN {
            // Still fits in embedded storage.
            // SAFETY: `ti` is live and locked.
            let dst = unsafe { super::inode::TmpfsInode::embedded_data(ti).add(pos as usize) };
            if user {
                // SAFETY: `current()` is the live running thread.
                if Vm::vm_copyin(unsafe { (*current()).vm }, dst as *mut c_void, buf as u64, count as u64) < 0 {
                    VfsInode::vfs_iunlock(inode);
                    return Err(Errno::Fault);
                }
            } else {
                unsafe { memmove(dst as *mut c_void, buf as *const c_void, count) };
            }
            // SAFETY: `inode` is live.
            unsafe {
                if end_pos > (*inode).size {
                    (*inode).size = end_pos;
                }
            }
            VfsInode::vfs_iunlock(inode);
            return Ok(count as isize);
        }
        // Need to migrate to pcache storage.
        let ret = super::inode::TmpfsInode::migrate_to_allocated_blocks(ti);
        if ret != 0 {
            VfsInode::vfs_iunlock(inode);
            // Sibling-module already-negative `c_int` — passthrough
            // (`Errno::Raw` round-trips it losslessly).
            return Err(Errno::Raw(ret));
        }
    }

    // pcache-based write.
    if unsafe { (*pc).flags.bits.active() == 0 } {
        VfsInode::vfs_iunlock(inode);
        return Err(Errno::Io);
    }

    // Mirrors the C original's `while` loop + `goto done` early exits:
    // every early-exit branch below has already called `vfs_iunlock`, so
    // (unlike the C's `goto done: return bytes_written;`) it can just
    // `return bytes_written as isize` directly instead of jumping past
    // the size-update step.
    let mut bytes_written: usize = 0;
    while bytes_written < count {
        let block_idx = tmpfs_iblock(pos) as u64;
        let block_off = tmpfs_iblock_offset(pos) as usize;
        let mut chunk = crate::bindings::PGSIZE as usize - block_off;
        if chunk > count - bytes_written {
            chunk = count - bytes_written;
        }

        let blkno_512 = block_idx * PCACHE_BLKS_PER_PAGE;
        let page = Pcache::get_page(pc, blkno_512);
        if page.is_null() {
            VfsInode::vfs_iunlock(inode);
            if bytes_written == 0 {
                return Err(Errno::NoMem);
            }
            return Ok(bytes_written as isize);
        }
        let ret = Pcache::read_page(pc, page);
        if ret != 0 {
            Pcache::put_page(pc, page);
            VfsInode::vfs_iunlock(inode);
            if bytes_written == 0 {
                // Cross-module already-negative `c_int` — passthrough.
                return Err(Errno::Raw(ret));
            }
            return Ok(bytes_written as isize);
        }
        let pcn = Pcache::page_get_node(page);
        // SAFETY: `pcn` is live; `block_off < PGSIZE`.
        let data = unsafe { ((*pcn).data as *mut u8).add(block_off) };

        if user {
            // SAFETY: `current()` is the live running thread.
            if Vm::vm_copyin(unsafe { (*current()).vm }, data as *mut c_void, unsafe { buf.add(bytes_written) } as u64, chunk as u64) < 0 {
                Pcache::put_page(pc, page);
                VfsInode::vfs_iunlock(inode);
                if bytes_written == 0 {
                    return Err(Errno::Fault);
                }
                return Ok(bytes_written as isize);
            }
        } else {
            unsafe { memmove(data as *mut c_void, buf.add(bytes_written) as *const c_void, chunk) };
        }
        Pcache::mark_page_dirty(pc, page);
        Pcache::put_page(pc, page);

        bytes_written += chunk;
        pos += chunk as loff_t;
    }

    // Update size if we extended the file.
    // SAFETY: `inode` is live.
    unsafe {
        if pos > (*inode).size {
            (*inode).size = pos;
        }
    }
    VfsInode::vfs_iunlock(inode);
    Ok(bytes_written as isize)
}

fn __tmpfs_file_llseek(file: *mut vfs_file, offset: loff_t, whence: c_int) -> KResult<loff_t> {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { FsStruct::vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };

    const SEEK_SET: c_int = 0;
    const SEEK_CUR: c_int = 1;
    const SEEK_END: c_int = 2;

    let new_pos = match whence {
        SEEK_SET => offset,
        SEEK_CUR => {
            // SAFETY: `file` is live.
            unsafe { (*file).pos.f_pos + offset }
        }
        SEEK_END => {
            // Need to lock inode to safely read size.
            VfsInode::vfs_ilock(inode);
            // SAFETY: `inode` is live and locked.
            let sz = unsafe { (*inode).size };
            VfsInode::vfs_iunlock(inode);
            sz + offset
        }
        _ => return Err(Errno::Inval),
    };

    if new_pos < 0 {
        return Err(Errno::Inval);
    }
    Ok(new_pos)
}

/// Demand-page a single page for a file-backed mapping.
///
/// Allocates a fresh anonymous page and populates it with data from the
/// tmpfs file at the faulting offset. Handles both the embedded-data path
/// (small files stored inline in the inode) and the pcache path.
///
/// The inode lock is held while reading size/data to prevent races with
/// concurrent truncate or write.
fn __tmpfs_file_fault(file: *mut vfs_file, vma_ptr: *mut vma, va: u64) -> *mut c_void {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { FsStruct::vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    if inode.is_null() {
        return ptr::null_mut();
    }
    let ti = inode as *mut tmpfs_inode;
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };

    // file_off is always page-aligned (both pgoff and va are page-aligned).
    // SAFETY: `vma_ptr` is live (caller's contract).
    let file_off = unsafe { (*vma_ptr).pgoff + (va - (*vma_ptr).start) };

    let pa = page_alloc(0, PAGE_TYPE_ANON);
    if pa.is_null() {
        return ptr::null_mut();
    }

    VfsInode::vfs_ilock(inode);

    // SAFETY: `inode` is live and locked.
    let size = unsafe { (*inode).size } as u64;

    // Entirely beyond EOF -- return a zero page.
    if file_off >= size {
        VfsInode::vfs_iunlock(inode);
        unsafe { memset(pa, 0, crate::bindings::PGSIZE as usize) };
        return pa;
    }

    let mut bytes_to_read = crate::bindings::PGSIZE as u64;
    if file_off + crate::bindings::PGSIZE as u64 > size {
        bytes_to_read = size - file_off;
    }

    // ---- embedded data path (small files inline in the inode) ----
    // SAFETY: `ti` is live.
    if unsafe { (*ti).embedded } != 0 {
        if (file_off as usize) < super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN {
            let avail = super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN as u64 - file_off;
            if bytes_to_read > avail {
                bytes_to_read = avail;
            }
            // SAFETY: `ti` is live and locked; `file_off + bytes_to_read
            // <= TMPFS_INODE_EMBEDDED_DATA_LEN`.
            unsafe {
                memmove(
                    pa,
                    super::inode::TmpfsInode::embedded_data(ti).add(file_off as usize) as *const c_void,
                    bytes_to_read as usize,
                );
            }
        } else {
            bytes_to_read = 0;
        }
        VfsInode::vfs_iunlock(inode);
        if bytes_to_read < crate::bindings::PGSIZE as u64 {
            unsafe {
                memset(
                    (pa as *mut u8).add(bytes_to_read as usize) as *mut c_void,
                    0,
                    crate::bindings::PGSIZE as usize - bytes_to_read as usize,
                );
            }
        }
        return pa;
    }

    // ---- pcache path ----
    if unsafe { (*pc).flags.bits.active() == 0 } {
        VfsInode::vfs_iunlock(inode);
        page_free(pa, 0);
        return ptr::null_mut();
    }

    let block_idx = tmpfs_iblock(file_off as loff_t) as u64;
    let blkno_512 = block_idx * PCACHE_BLKS_PER_PAGE;

    let pcpage = Pcache::get_page(pc, blkno_512);
    if pcpage.is_null() {
        VfsInode::vfs_iunlock(inode);
        page_free(pa, 0);
        return ptr::null_mut();
    }
    let ret = Pcache::read_page(pc, pcpage);
    if ret != 0 {
        Pcache::put_page(pc, pcpage);
        VfsInode::vfs_iunlock(inode);
        page_free(pa, 0);
        return ptr::null_mut();
    }

    let pcn = Pcache::page_get_node(pcpage);
    // SAFETY: `pcn` is a live `pcache_node` just populated above.
    unsafe { memmove(pa, (*pcn).data as *const c_void, bytes_to_read as usize) };
    if bytes_to_read < crate::bindings::PGSIZE as u64 {
        unsafe {
            memset(
                (pa as *mut u8).add(bytes_to_read as usize) as *mut c_void,
                0,
                crate::bindings::PGSIZE as usize - bytes_to_read as usize,
            );
        }
    }

    Pcache::put_page(pc, pcpage);
    VfsInode::vfs_iunlock(inode);
    pa
}

// P3-10a `FileOps` trait implementor (was the `struct vfs_file_ops`
// fn-pointer table). The old table's `None` slots (`release`/`fsync`/
// `fflush`/`poll`/`ioctl`) are not overridden: the trait defaults
// reproduce the null-slot dispatch outcomes exactly (see
// `vfs/file.rs`'s trait doc).

/// Zero-sized `FileOps` implementor for tmpfs files.
struct TmpfsFileOps;

/// The single shared instance `tmpfs_open` installs as
/// `file.ops = Some(&TMPFS_FILE_OPS)`.
static TMPFS_FILE_OPS: TmpfsFileOps = TmpfsFileOps;

impl FileOps for TmpfsFileOps {
    unsafe fn read(&self, file: *mut vfs_file, buf: *mut c_char, count: usize, user: bool)
        -> KResult<isize> {
        __tmpfs_file_read(file, buf, count, user)
    }

    unsafe fn write(&self, file: *mut vfs_file, buf: *const c_char, count: usize, user: bool)
        -> KResult<isize> {
        __tmpfs_file_write(file, buf, count, user)
    }

    unsafe fn llseek(&self, file: *mut vfs_file, offset: loff_t, whence: c_int)
        -> KResult<loff_t> {
        __tmpfs_file_llseek(file, offset, whence)
    }

    unsafe fn fault(&self, file: *mut vfs_file, vma: *mut vma, va: u64) -> Option<*mut c_void> {
        // `Some(..)` = "this driver handles mmap faults"; the payload is
        // the old callback's raw result (null on failure), returned to
        // the faulting path as-is.
        Some(__tmpfs_file_fault(file, vma, va))
    }
}

/// Open callback for tmpfs inodes. Sets up file operations based on
/// inode type.
///
/// P3-10b: `KResult`-native, reached through
/// [`super::inode::TmpfsInodeOps`]'s `open` (the old `extern "C"`
/// C-ABI spelling is gone with the fn-pointer table).
pub(crate) fn tmpfs_open(inode: *mut vfs_inode, file: *mut vfs_file, _f_flags: c_int) -> KResult<()> {
    if inode.is_null() || file.is_null() {
        return Err(Errno::Inval);
    }

    // SAFETY: `inode` is live (caller's contract).
    let mode = unsafe { (*inode).mode };

    if s_isreg(mode) {
        // SAFETY: `file` is live.
        unsafe { (*file).ops = Some(&TMPFS_FILE_OPS) };
        return Ok(());
    }

    if s_isdir(mode) {
        // Directories don't need special file ops -- they use dir_iter.
        unsafe { (*file).ops = Some(&TMPFS_FILE_OPS) };
        return Ok(());
    }

    if s_islnk(mode) {
        // Allow opening symlinks with O_NOFOLLOW flag. POSIX requires
        // that symlinks can be opened with O_NOFOLLOW to allow fstat()
        // on the symlink itself (not its target). This is needed by
        // programs like ls and symlinktest that want to stat symlink info.
        unsafe { (*file).ops = Some(&TMPFS_FILE_OPS) };
        return Ok(());
    }

    // Character/block devices and pipes are handled by VFS core. They
    // should not reach here as vfs_fileopen handles them.
    if super::s_ischr(mode) || super::s_isblk(mode) || super::s_isfifo(mode) {
        return Err(Errno::Inval); // Should be handled by VFS
    }

    Err(Errno::NoSys)
}
