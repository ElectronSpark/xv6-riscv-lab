//! tmpfs truncate/grow/shrink — Rust port of
//! `kernel/vfs/tmpfs/truncate.c` (Phase 2 Wave 18, sub-wave B; see
//! `super` module doc and `docs/rustify/phase2_plan.md`).
//!
//! Grows or shrinks a tmpfs regular file: embedded <-> pcache migration
//! on grow, pcache page discard on shrink. Called with the target
//! inode's mutex held (via `vfs_inode_ops.truncate`'s documented
//! contract) -- nothing here acquires a lock of its own.
//!
//! `docs/rustify/phase2_plan.md` flags this file specially: the host
//! cmocka suite `test/src/ut_tmpfs_truncate_linked.c` `#include`s
//! `truncate.c` directly to unit-test it against a mock pcache. That
//! suite is disabled by this port (`test/CMakeLists.txt`, dated comment,
//! same precedent as `ut_hlist`/`ut_rbtree`) -- coverage is re-homed to
//! this crate's usual boot-time/QEMU verification instead, per
//! `docs/rustify/test_port_plan.md`.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_int, c_void};

use crate::bindings::{loff_t, page_t, pcache, pcache_node, tmpfs_inode, vfs_inode, EFBIG, ENOMEM, PAGE_SHIFT, PGSIZE};

use super::TMPFS_MAX_FILE_SIZE;

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

unsafe extern "C" {
    // string.rs.
    safe fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

    // mm/pcache.rs.
    safe fn pcache_get_page(p: *mut pcache, blkno: u64) -> *mut page_t;
    safe fn pcache_put_page(p: *mut pcache, page: *mut page_t);
    safe fn pcache_read_page(p: *mut pcache, page: *mut page_t) -> c_int;
    safe fn pcache_mark_page_dirty(p: *mut pcache, page: *mut page_t) -> c_int;
    safe fn pcache_discard_blk(p: *mut pcache, blkno: u64) -> c_int;
    safe fn pcache_teardown(p: *mut pcache);

    // mm/page.rs -- `page->pcache.pcache_node` accessor (pre-existing,
    // exported for `mm/vm.rs`'s own extern call site; reused here rather
    // than reaching into the bindgen union ourselves).
    safe fn xv6_page_pcache_get_node(page: *mut page_t) -> *mut pcache_node;
}

/* Convert block size to 512-byte units for pcache. */
const PCACHE_BLKS_PER_PAGE: u64 = PGSIZE as u64 / 512;

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

#[inline(always)]
fn tmpfs_iblock(pos: loff_t) -> loff_t {
    pos >> PAGE_SHIFT
}

/// `pcache.active` bit (`pcache__bindgen_ty_1.__bindgen_anon_1.active`,
/// a real Rust union since all its variants are Copy). Matches
/// `mm/pcache.rs`'s own `PcacheHandle::active()` access path exactly
/// (that helper is crate-private to `mm`, so this file reaches the
/// bindgen field directly rather than duplicating a public wrapper for
/// one boolean read).
///
/// # Safety
/// `pc` must point to a live `pcache`.
#[inline(always)]
unsafe fn pcache_active(pc: *mut pcache) -> bool {
    unsafe { (*pc).__bindgen_anon_1.__bindgen_anon_1.active() != 0 }
}

/// Shrink a tmpfs file to `new_size` by discarding pcache pages beyond
/// the new boundary. Embedded files have no pages to free.
fn __tmpfs_truncate_shrink(inode: *mut vfs_inode, new_size: loff_t) -> c_int {
    let ti = inode as *mut tmpfs_inode;
    // SAFETY: `inode`/`ti` are live (caller's contract).
    if unsafe { (*ti).embedded } != 0 {
        // Embedded data lives inside the inode struct; nothing to free.
        return 0;
    }

    // SAFETY: `inode` is live.
    let pc = unsafe { core::ptr::addr_of_mut!((*inode).i_data) };
    // SAFETY: `pc` is a live `pcache` embedded in `inode`.
    if !unsafe { pcache_active(pc) } {
        return 0;
    }

    // Discard every pcache page whose base offset >= new_size.
    let page_size = PGSIZE as loff_t;
    let first_discard = tmpfs_iblock(new_size + page_size - 1);
    // SAFETY: `inode` is live.
    let old_size = unsafe { (*inode).size };
    let old_block_cnt = tmpfs_iblock(old_size + page_size - 1);

    let mut blk = first_discard;
    while blk < old_block_cnt {
        let blkno_512 = (blk as u64) * PCACHE_BLKS_PER_PAGE;
        pcache_discard_blk(pc, blkno_512);
        blk += 1;
    }
    0
}

/// Migrate from embedded storage to pcache-backed storage. Copies up to
/// `inode->size` bytes of embedded data into the first pcache page.
///
/// Kept `#[no_mangle]`/exported per `tmpfs_private.h`'s `extern`
/// declaration.
#[no_mangle]
pub extern "C" fn __tmpfs_migrate_to_allocated_blocks(ti: *mut tmpfs_inode) -> c_int {
    // SAFETY: `ti` is a live, exclusively-owned `tmpfs_inode` (caller
    // holds the inode mutex).
    let inode = unsafe { core::ptr::addr_of_mut!((*ti).vfs_inode) };
    let size = unsafe { (*inode).size };

    // Snapshot embedded data before clearing the union below (`dir`/
    // `sym`/`file` all start at the same offset within `tmpfs_inode`).
    let mut embedded_copy = [0u8; super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN];
    if size > 0 {
        // SAFETY: `ti` is live; `size <= TMPFS_INODE_EMBEDDED_DATA_LEN` is
        // an invariant of the embedded representation (caller's contract,
        // matching the C original -- embedded files never exceed this
        // bound before migrating).
        unsafe {
            memcpy(
                embedded_copy.as_mut_ptr() as *mut c_void,
                super::inode::embedded_data(ti) as *const c_void,
                size as usize,
            );
        }
    }

    // Clear the union (embedded data and pcache overlap in the same
    // memory -- `dir`/`sym`/`file` are all offset 0 within `tmpfs_inode`,
    // same as `__tmpfs_make_regfile`'s own reasoning).
    // SAFETY: `ti` is live and exclusively owned.
    unsafe {
        core::ptr::write_bytes(
            super::inode::embedded_data(ti),
            0,
            core::mem::size_of::<tmpfs_inode>() - core::mem::offset_of!(tmpfs_inode, __bindgen_anon_1),
        );
        (*ti).embedded = 0;
    }

    // Bring up the per-inode pcache.
    super::file::tmpfs_inode_pcache_init(inode);

    // SAFETY: `inode` is live.
    let pc = unsafe { core::ptr::addr_of_mut!((*inode).i_data) };
    if !unsafe { pcache_active(pc) } {
        return neg(ENOMEM);
    }

    // Copy the old embedded data into the first page.
    if size > 0 {
        let page = pcache_get_page(pc, 0);
        if page.is_null() {
            pcache_teardown(pc);
            return neg(ENOMEM);
        }
        let ret = pcache_read_page(pc, page);
        if ret != 0 {
            pcache_put_page(pc, page);
            pcache_teardown(pc);
            return ret;
        }
        let pcn = xv6_page_pcache_get_node(page);
        // SAFETY: `pcn` is a live `pcache_node` just populated by
        // `pcache_read_page`.
        unsafe {
            memcpy((*pcn).data, embedded_copy.as_ptr() as *const c_void, size as usize);
        }
        pcache_mark_page_dirty(pc, page);
        pcache_put_page(pc, page);
    }

    0
}

/// Grow a tmpfs file to `new_size`. For embedded files that still fit,
/// just zero-fill the gap. For pcache files, nothing to pre-allocate
/// (pages are demand-allocated).
fn __tmpfs_truncate_grow(inode: *mut vfs_inode, new_size: loff_t) -> c_int {
    let ti = inode as *mut tmpfs_inode;

    // SAFETY: `ti`/`inode` are live.
    if unsafe { (*ti).embedded } != 0 {
        if (new_size as usize) <= super::inode::TMPFS_INODE_EMBEDDED_DATA_LEN {
            // Still fits in embedded storage -- zero the gap.
            let old_size = unsafe { (*inode).size };
            // SAFETY: `ti` is live; `old_size <= new_size <=
            // TMPFS_INODE_EMBEDDED_DATA_LEN`, so the gap lies entirely
            // within the embedded-data region.
            unsafe {
                core::ptr::write_bytes(
                    super::inode::embedded_data(ti).add(old_size as usize),
                    0,
                    (new_size - old_size) as usize,
                );
            }
            return 0;
        }
        // Outgrew embedded; migrate to pcache.
        let ret = __tmpfs_migrate_to_allocated_blocks(ti);
        if ret != 0 {
            return ret;
        }
    }

    // pcache pages are allocated on demand -- nothing more to do.
    0
}

/// Top-level truncate: grow or shrink a tmpfs regular file.
///
/// Kept `#[no_mangle]`/exported per `tmpfs_private.h`'s `extern`
/// declaration (`int __tmpfs_truncate(...)`), and it is also the
/// `.truncate` entry of [`super::inode::TMPFS_INODE_OPS`].
#[no_mangle]
pub extern "C" fn __tmpfs_truncate(inode: *mut vfs_inode, new_size: loff_t) -> c_int {
    if new_size > TMPFS_MAX_FILE_SIZE as loff_t {
        return neg(EFBIG);
    }
    let mut ret = 0;
    // SAFETY: `inode` is live (caller holds the inode mutex).
    let old_size = unsafe { (*inode).size };
    if old_size < new_size {
        ret = __tmpfs_truncate_grow(inode, new_size);
        if ret != 0 {
            // Grow failed -- undo any partial work.
            __tmpfs_truncate_shrink(inode, old_size);
        }
    } else if old_size > new_size {
        ret = __tmpfs_truncate_shrink(inode, new_size);
    }
    if ret == 0 {
        // SAFETY: `inode` is live.
        unsafe { (*inode).size = new_size };
    }
    ret
}
