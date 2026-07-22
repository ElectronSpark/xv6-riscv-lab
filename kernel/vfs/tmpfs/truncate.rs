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
//!
//! # Wave A free-fn -> associated-fn sweep
//!
//! Every free fn that used to live in this file is now an associated
//! fn: `impl TmpfsInode` (`truncate`/`truncate_grow`/`truncate_shrink`/
//! `migrate_to_allocated_blocks` -- every one's subject is the target
//! regular-file inode) or `impl Tmpfs` (the small scalar `neg`/
//! `iblock`/`pcache_active` helpers with no inode-shaped subject).
//! [`Tmpfs`] is a ZST private to this file (NOT shared with
//! `inode.rs`/`superblock.rs`'s own separate `Tmpfs` markers -- same
//! per-file-marker precedent as `xv6fs`'s `Xv6fs`). Zero floor: this
//! file has no slab facade or `hlist_*` primitives to keep as free fns.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_int, c_void};

use crate::bindings::{loff_t, pcache, tmpfs_inode, vfs_inode, ENOMEM, PAGE_SHIFT, PGSIZE};
use crate::kstd::{Errno, KResult};

use super::TMPFS_MAX_FILE_SIZE;
// Wave A: the bmap/truncate family below are now `TmpfsInode` associated
// fns (matching `xv6fs/truncate.rs`'s own `use super::inode::Xv6fsInode;`
// precedent) -- `tmpfs_inode` (the bindgen facade alias of this same
// native type, already imported above) stays the raw-pointer param type
// in every signature; `TmpfsInode` is only used for the `impl` path.
use super::inode::TmpfsInode;

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

unsafe extern "C" {
    // string.rs.
    safe fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

}

// P3-D3a: the mm/pcache.rs entry points are ordinary (safe) Rust fns now
// that their `#[no_mangle]` exports are gone, with identical signatures
// (pcache.rs's `Pcache`/`Page`/`PcacheNode` are aliases of the same
// bindgen `pcache`/`page_t`/`pcache_node` this file already uses) —
// plain `use` instead of the `extern "C"` redeclarations that used to
// sit in the block above.
use crate::mm::Pcache;

/* Convert block size to 512-byte units for pcache. */
const PCACHE_BLKS_PER_PAGE: u64 = PGSIZE as u64 / 512;

/// ZST marker (Wave A free-fn -> associated-fn sweep, this file only)
/// for the small scalar `neg`/`iblock`/`pcache_active` helpers with no
/// natural type home (`iblock` takes a bare `loff_t`, not an inode).
/// Private to this file, matching `inode.rs`/`superblock.rs`'s own
/// separate `Tmpfs` markers (same precedent as `xv6fs`'s per-file
/// `Xv6fs`).
struct Tmpfs;

impl Tmpfs {
    #[inline(always)]
    const fn neg(e: u32) -> c_int {
        -(e as c_int)
    }

    #[inline(always)]
    fn iblock(pos: loff_t) -> loff_t {
        pos >> PAGE_SHIFT
    }

    /// `pcache.active` bit (the native `PcacheFlags` union's
    /// `PcacheFlagBits::active()` accessor since P3-N6 — bit-identical to
    /// the bindgen `__bindgen_anon_1` path it replaced). Matches
    /// `mm/pcache.rs`'s own `PcacheHandle::active()` access path exactly
    /// (that helper is crate-private to `mm`, so this file reaches the
    /// field directly rather than duplicating a public wrapper for
    /// one boolean read).
    ///
    /// # Safety
    /// `pc` must point to a live `pcache`.
    #[inline(always)]
    unsafe fn pcache_active(pc: *mut pcache) -> bool {
        unsafe { (*pc).flags.bits.active() != 0 }
    }
}

impl TmpfsInode {
    /// Shrink a tmpfs file to `new_size` by discarding pcache pages beyond
    /// the new boundary. Embedded files have no pages to free.
    fn truncate_shrink(inode: *mut vfs_inode, new_size: loff_t) -> c_int {
        let ti = inode as *mut tmpfs_inode;
        // SAFETY: `inode`/`ti` are live (caller's contract).
        if unsafe { (*ti).embedded } != 0 {
            // Embedded data lives inside the inode struct; nothing to free.
            return 0;
        }

        // SAFETY: `inode` is live.
        let pc = unsafe { core::ptr::addr_of_mut!((*inode).i_data) };
        // SAFETY: `pc` is a live `pcache` embedded in `inode`.
        if !unsafe { Tmpfs::pcache_active(pc) } {
            return 0;
        }

        // Discard every pcache page whose base offset >= new_size.
        let page_size = PGSIZE as loff_t;
        let first_discard = Tmpfs::iblock(new_size + page_size - 1);
        // SAFETY: `inode` is live.
        let old_size = unsafe { (*inode).size };
        let old_block_cnt = Tmpfs::iblock(old_size + page_size - 1);

        // Fixed-stride block-index walk (both bounds captured once above under
        // the inode mutex; `pcache_discard_blk` discards by block number and
        // never mutates the iteration variable) -> range, matching xv6fs
        // `xv6fs_itrunc`'s `for j in 0..N` block-walk style (e3cb8a0).
        for blk in first_discard..old_block_cnt {
            let blkno_512 = (blk as u64) * PCACHE_BLKS_PER_PAGE;
            Pcache::discard_blk(pc, blkno_512);
        }
        0
    }
}

impl TmpfsInode {
    /// Migrate from embedded storage to pcache-backed storage. Copies up to
    /// `inode->size` bytes of embedded data into the first pcache page.
    ///
    /// Kept `#[no_mangle]`/exported per `tmpfs_private.h`'s `extern`
    /// declaration.
    pub(crate) extern "C" fn migrate_to_allocated_blocks(ti: *mut tmpfs_inode) -> c_int {
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
                    TmpfsInode::embedded_data(ti) as *const c_void,
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
                TmpfsInode::embedded_data(ti),
                0,
                core::mem::size_of::<tmpfs_inode>() - core::mem::offset_of!(tmpfs_inode, u),
            );
            (*ti).embedded = 0;
        }

        // Bring up the per-inode pcache.
        super::file::tmpfs_inode_pcache_init(inode);

        // SAFETY: `inode` is live.
        let pc = unsafe { core::ptr::addr_of_mut!((*inode).i_data) };
        if !unsafe { Tmpfs::pcache_active(pc) } {
            return Tmpfs::neg(ENOMEM);
        }

        // Copy the old embedded data into the first page.
        if size > 0 {
            let page = Pcache::get_page(pc, 0);
            if page.is_null() {
                Pcache::teardown(pc);
                return Tmpfs::neg(ENOMEM);
            }
            let ret = Pcache::read_page(pc, page);
            if ret != 0 {
                Pcache::put_page(pc, page);
                Pcache::teardown(pc);
                return ret;
            }
            let pcn = Pcache::page_get_node(page);
            // SAFETY: `pcn` is a live `pcache_node` just populated by
            // `pcache_read_page`.
            unsafe {
                memcpy((*pcn).data, embedded_copy.as_ptr() as *const c_void, size as usize);
            }
            Pcache::mark_page_dirty(pc, page);
            Pcache::put_page(pc, page);
        }

        0
    }
}

impl TmpfsInode {
    /// Grow a tmpfs file to `new_size`. For embedded files that still fit,
    /// just zero-fill the gap. For pcache files, nothing to pre-allocate
    /// (pages are demand-allocated).
    fn truncate_grow(inode: *mut vfs_inode, new_size: loff_t) -> c_int {
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
                        TmpfsInode::embedded_data(ti).add(old_size as usize),
                        0,
                        (new_size - old_size) as usize,
                    );
                }
                return 0;
            }
            // Outgrew embedded; migrate to pcache.
            let ret = TmpfsInode::migrate_to_allocated_blocks(ti);
            if ret != 0 {
                return ret;
            }
        }

        // pcache pages are allocated on demand -- nothing more to do.
        0
    }
}

impl TmpfsInode {
    /// Top-level truncate: grow or shrink a tmpfs regular file.
    ///
    /// P3-10b: `KResult`-native, reached through
    /// [`super::inode::TmpfsInodeOps`]'s `truncate` (the old `extern "C"`
    /// C-ABI spelling is gone with the fn-pointer table). The internal
    /// grow/shrink helpers keep their `c_int` returns (`-ENOMEM` on page
    /// allocation failure), decoded once here.
    pub(crate) fn truncate(inode: *mut vfs_inode, new_size: loff_t) -> KResult<()> {
        if new_size > TMPFS_MAX_FILE_SIZE as loff_t {
            return Err(Errno::FBig);
        }
        let mut ret = 0;
        // SAFETY: `inode` is live (caller holds the inode mutex).
        let old_size = unsafe { (*inode).size };
        if old_size < new_size {
            ret = TmpfsInode::truncate_grow(inode, new_size);
            if ret != 0 {
                // Grow failed -- undo any partial work.
                TmpfsInode::truncate_shrink(inode, old_size);
            }
        } else if old_size > new_size {
            ret = TmpfsInode::truncate_shrink(inode, new_size);
        }
        if ret == 0 {
            // SAFETY: `inode` is live.
            unsafe { (*inode).size = new_size };
            Ok(())
        } else {
            // Grow/shrink fail with `-ENOMEM` only (page allocation).
            Err(Errno::NoMem)
        }
    }
}
