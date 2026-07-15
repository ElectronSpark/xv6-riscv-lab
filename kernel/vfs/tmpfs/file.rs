//! tmpfs file operations — Rust port of `kernel/vfs/tmpfs/file.c`
//! (Phase 2 Wave 18, sub-wave B; see `super` module doc and
//! `docs/rustify/phase2_plan.md`).
//!
//! Implements the VFS file operations for tmpfs regular files, the
//! per-inode pcache wiring, and `struct vfs_file_ops tmpfs_file_ops`
//! itself.
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
    loff_t, page_t, pcache, pcache_node, pcache_ops, thread, tmpfs_inode, vfs_file, vfs_file_ops,
    vfs_inode, vma, EFAULT, EFBIG, EINVAL, EIO, ENOMEM,
};

use crate::proc::proc_shims::xv6_current_thread;

use super::{s_isdir, s_islnk, s_isreg, TMPFS_MAX_FILE_SIZE};

// P3-1C mesh sweep: vfs/{inode,fs}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures).
use crate::vfs::fs::vfs_inode_deref;
use crate::vfs::inode::{vfs_ilock, vfs_iunlock};

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

unsafe extern "C" {
    // string.rs.
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    safe fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;

    // mm/vm.rs.
    safe fn vm_copyout(vm_ptr: *mut crate::bindings::vm, dstva: u64, src: *const c_void, len: u64) -> c_int;
    safe fn vm_copyin(vm_ptr: *mut crate::bindings::vm, dst: *mut c_void, srcva: u64, len: u64) -> c_int;

    // mm/page.rs.
    safe fn page_alloc(order: u64, flags: u64) -> *mut c_void;
    safe fn page_free(ptr: *mut c_void, order: u64);

    // mm/pcache.rs.
    safe fn pcache_init(p: *mut pcache) -> c_int;
    safe fn pcache_teardown(p: *mut pcache);
    safe fn pcache_get_page(p: *mut pcache, blkno: u64) -> *mut page_t;
    safe fn pcache_put_page(p: *mut pcache, page: *mut page_t);
    safe fn pcache_read_page(p: *mut pcache, page: *mut page_t) -> c_int;
    safe fn pcache_mark_page_dirty(p: *mut pcache, page: *mut page_t) -> c_int;

    // mm/page.rs -- `page->pcache.pcache_node` accessor (pre-existing).
    safe fn xv6_page_pcache_get_node(page: *mut page_t) -> *mut pcache_node;
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
    let pcn = xv6_page_pcache_get_node(page);
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

    let ret = pcache_init(pc);
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
    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() != 0 } {
        pcache_teardown(pc);
    }
}

fn current() -> *mut thread {
    xv6_current_thread()
}

extern "C" fn __tmpfs_file_read(file: *mut vfs_file, buf: *mut c_char, mut count: usize, user: crate::bindings::bool_) -> isize {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    let ti = inode as *mut tmpfs_inode;
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };

    // SAFETY: `inode` is live.
    if !s_isreg(unsafe { (*inode).mode }) {
        return neg(EINVAL) as isize;
    }

    // Acquire inode lock to safely read size and data. The file
    // reference guarantees the inode remains allocated.
    vfs_ilock(inode);

    // SAFETY: `file`/`inode` are live and locked.
    let mut pos = unsafe { (*file).__bindgen_anon_1.f_pos };
    let size = unsafe { (*inode).size };
    if pos >= size {
        vfs_iunlock(inode);
        return 0; // EOF
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
        let src = unsafe { super::inode::embedded_data(ti).add(pos as usize) };
        if user != 0 {
            // SAFETY: `current()` is the live running thread.
            if vm_copyout(unsafe { (*current()).vm }, buf as u64, src as *const c_void, count as u64) < 0 {
                vfs_iunlock(inode);
                return neg(EFAULT) as isize;
            }
        } else {
            unsafe { memmove(buf as *mut c_void, src as *const c_void, count) };
        }
        vfs_iunlock(inode);
        return count as isize;
    }

    // pcache-based read.
    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() == 0 } {
        vfs_iunlock(inode);
        return neg(EIO) as isize;
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
        let page = pcache_get_page(pc, blkno_512);
        if page.is_null() {
            vfs_iunlock(inode);
            if bytes_read == 0 {
                return neg(EIO) as isize;
            }
            return bytes_read as isize;
        }
        let ret = pcache_read_page(pc, page);
        if ret != 0 {
            pcache_put_page(pc, page);
            vfs_iunlock(inode);
            if bytes_read == 0 {
                return neg(EIO) as isize;
            }
            return bytes_read as isize;
        }
        let pcn = xv6_page_pcache_get_node(page);
        // SAFETY: `pcn` is live; `block_off < PGSIZE`.
        let data = unsafe { ((*pcn).data as *mut u8).add(block_off) };

        if user != 0 {
            // SAFETY: `current()` is the live running thread.
            if vm_copyout(unsafe { (*current()).vm }, unsafe { buf.add(bytes_read) } as u64, data as *const c_void, chunk as u64) < 0 {
                pcache_put_page(pc, page);
                vfs_iunlock(inode);
                if bytes_read == 0 {
                    return neg(EFAULT) as isize;
                }
                return bytes_read as isize;
            }
        } else {
            unsafe { memmove(buf.add(bytes_read) as *mut c_void, data as *const c_void, chunk) };
        }
        pcache_put_page(pc, page);

        bytes_read += chunk;
        pos += chunk as loff_t;
    }

    vfs_iunlock(inode);
    bytes_read as isize
}

extern "C" fn __tmpfs_file_write(file: *mut vfs_file, buf: *const c_char, count_in: usize, user: crate::bindings::bool_) -> isize {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
    let ti = inode as *mut tmpfs_inode;
    let pc = unsafe { ptr::addr_of_mut!((*inode).i_data) };
    let count = count_in;

    // SAFETY: `inode` is live.
    if !s_isreg(unsafe { (*inode).mode }) {
        return neg(EINVAL) as isize;
    }

    // Acquire inode lock to protect size and data. The file reference
    // guarantees the inode remains allocated.
    vfs_ilock(inode);

    // SAFETY: `file` is live and `inode` is locked.
    let mut pos = unsafe { (*file).__bindgen_anon_1.f_pos };
    let end_pos = pos + count as loff_t;

    // Check for file size limits.
    if end_pos > TMPFS_MAX_FILE_SIZE as loff_t {
        vfs_iunlock(inode);
        return neg(EFBIG) as isize;
    }

    // Handle embedded data.
    // SAFETY: `ti` is live.
    if unsafe { (*ti).embedded } != 0 {
        if end_pos as usize <= super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN {
            // Still fits in embedded storage.
            // SAFETY: `ti` is live and locked.
            let dst = unsafe { super::inode::embedded_data(ti).add(pos as usize) };
            if user != 0 {
                // SAFETY: `current()` is the live running thread.
                if vm_copyin(unsafe { (*current()).vm }, dst as *mut c_void, buf as u64, count as u64) < 0 {
                    vfs_iunlock(inode);
                    return neg(EFAULT) as isize;
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
            vfs_iunlock(inode);
            return count as isize;
        }
        // Need to migrate to pcache storage.
        let ret = super::truncate::__tmpfs_migrate_to_allocated_blocks(ti);
        if ret != 0 {
            vfs_iunlock(inode);
            return ret as isize;
        }
    }

    // pcache-based write.
    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() == 0 } {
        vfs_iunlock(inode);
        return neg(EIO) as isize;
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
        let page = pcache_get_page(pc, blkno_512);
        if page.is_null() {
            vfs_iunlock(inode);
            if bytes_written == 0 {
                return neg(ENOMEM) as isize;
            }
            return bytes_written as isize;
        }
        let ret = pcache_read_page(pc, page);
        if ret != 0 {
            pcache_put_page(pc, page);
            vfs_iunlock(inode);
            if bytes_written == 0 {
                return ret as isize;
            }
            return bytes_written as isize;
        }
        let pcn = xv6_page_pcache_get_node(page);
        // SAFETY: `pcn` is live; `block_off < PGSIZE`.
        let data = unsafe { ((*pcn).data as *mut u8).add(block_off) };

        if user != 0 {
            // SAFETY: `current()` is the live running thread.
            if vm_copyin(unsafe { (*current()).vm }, data as *mut c_void, unsafe { buf.add(bytes_written) } as u64, chunk as u64) < 0 {
                pcache_put_page(pc, page);
                vfs_iunlock(inode);
                if bytes_written == 0 {
                    return neg(EFAULT) as isize;
                }
                return bytes_written as isize;
            }
        } else {
            unsafe { memmove(data as *mut c_void, buf.add(bytes_written) as *const c_void, chunk) };
        }
        pcache_mark_page_dirty(pc, page);
        pcache_put_page(pc, page);

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
    vfs_iunlock(inode);
    bytes_written as isize
}

extern "C" fn __tmpfs_file_llseek(file: *mut vfs_file, offset: loff_t, whence: c_int) -> loff_t {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };

    const SEEK_SET: c_int = 0;
    const SEEK_CUR: c_int = 1;
    const SEEK_END: c_int = 2;

    let new_pos = match whence {
        SEEK_SET => offset,
        SEEK_CUR => {
            // SAFETY: `file` is live.
            unsafe { (*file).__bindgen_anon_1.f_pos + offset }
        }
        SEEK_END => {
            // Need to lock inode to safely read size.
            vfs_ilock(inode);
            // SAFETY: `inode` is live and locked.
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

/// Demand-page a single page for a file-backed mapping.
///
/// Allocates a fresh anonymous page and populates it with data from the
/// tmpfs file at the faulting offset. Handles both the embedded-data path
/// (small files stored inline in the inode) and the pcache path.
///
/// The inode lock is held while reading size/data to prevent races with
/// concurrent truncate or write.
extern "C" fn __tmpfs_file_fault(file: *mut vfs_file, vma_ptr: *mut vma, va: u64) -> *mut c_void {
    // SAFETY: `file` is live (caller's contract).
    let inode = unsafe { vfs_inode_deref(ptr::addr_of_mut!((*file).inode)) };
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

    vfs_ilock(inode);

    // SAFETY: `inode` is live and locked.
    let size = unsafe { (*inode).size } as u64;

    // Entirely beyond EOF -- return a zero page.
    if file_off >= size {
        vfs_iunlock(inode);
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
                    super::inode::embedded_data(ti).add(file_off as usize) as *const c_void,
                    bytes_to_read as usize,
                );
            }
        } else {
            bytes_to_read = 0;
        }
        vfs_iunlock(inode);
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
    if unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() == 0 } {
        vfs_iunlock(inode);
        page_free(pa, 0);
        return ptr::null_mut();
    }

    let block_idx = tmpfs_iblock(file_off as loff_t) as u64;
    let blkno_512 = block_idx * PCACHE_BLKS_PER_PAGE;

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

    pcache_put_page(pc, pcpage);
    vfs_iunlock(inode);
    pa
}

static TMPFS_FILE_OPS: vfs_file_ops = vfs_file_ops {
    read: Some(__tmpfs_file_read),
    write: Some(__tmpfs_file_write),
    llseek: Some(__tmpfs_file_llseek),
    release: None,
    fsync: None,
    fflush: None,
    poll: None,
    ioctl: None,
    fault: Some(__tmpfs_file_fault),
};

/// Open callback for tmpfs inodes. Sets up file operations based on
/// inode type.
///
/// Kept `#[no_mangle]`/exported per `tmpfs_private.h`'s `extern`
/// declaration, and it is also the `.open` entry of
/// [`super::inode::TMPFS_INODE_OPS`].
pub(crate) extern "C" fn tmpfs_open(inode: *mut vfs_inode, file: *mut vfs_file, _f_flags: c_int) -> c_int {
    if inode.is_null() || file.is_null() {
        return neg(EINVAL);
    }

    // SAFETY: `inode` is live (caller's contract).
    let mode = unsafe { (*inode).mode };

    if s_isreg(mode) {
        // SAFETY: `file` is live.
        unsafe { (*file).ops = ptr::addr_of!(TMPFS_FILE_OPS) as *mut vfs_file_ops };
        return 0;
    }

    if s_isdir(mode) {
        // Directories don't need special file ops -- they use dir_iter.
        unsafe { (*file).ops = ptr::addr_of!(TMPFS_FILE_OPS) as *mut vfs_file_ops };
        return 0;
    }

    if s_islnk(mode) {
        // Allow opening symlinks with O_NOFOLLOW flag. POSIX requires
        // that symlinks can be opened with O_NOFOLLOW to allow fstat()
        // on the symlink itself (not its target). This is needed by
        // programs like ls and symlinktest that want to stat symlink info.
        unsafe { (*file).ops = ptr::addr_of!(TMPFS_FILE_OPS) as *mut vfs_file_ops };
        return 0;
    }

    // Character/block devices and pipes are handled by VFS core. They
    // should not reach here as vfs_fileopen handles them.
    if super::s_ischr(mode) || super::s_isblk(mode) || super::s_isfifo(mode) {
        return neg(EINVAL); // Should be handled by VFS
    }

    neg(crate::bindings::ENOSYS)
}
