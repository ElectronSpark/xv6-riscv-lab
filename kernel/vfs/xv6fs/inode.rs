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
//! # Fidelity note: NULL-`Buf::read()` deref safety fix
//!
//! The C original's directory-entry scan helpers are inconsistent about
//! checking `Buf::read()`'s result for `NULL` before dereferencing it:
//! `xv6fs_lookup`/`xv6fs_dir_iter` check (`if (bp == NULL) continue;`);
//! `__xv6fs_dir_name_exists`/`__xv6fs_dirlink`/`xv6fs_create`'s
//! existing-name scan/`xv6fs_unlink` do **not** — a latent NULL-pointer
//! dereference on an out-of-memory `Buf::read()` failure (bread only returns
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
//! `Buf::read()` + explicit NULL check inline (not via `read_dirent`, which is
//! read-only), matching the C's re-read-for-write structure exactly.
//!
//! # Wave A free-fn -> associated-fn sweep
//!
//! Every free fn that used to live in this file is now an associated fn:
//! either an `impl Xv6fsInode` method (functions whose first meaningful
//! parameter was already the concrete `*mut xv6fs_inode` type — the
//! directory-entry read/scan/link family) or an `impl Xv6fs` method (the
//! `vfs_inode_ops` callback bodies, which take the *generic* `*mut
//! vfs_inode`/`*mut vfs_dentry`/... and cast down internally, plus the
//! small scalar `major`/`minor`/`mkdev`/`neg` helpers with no single type
//! home). [`Xv6fs`] is [`super::Xv6fs`] — one shared ZST marker with
//! `impl` blocks split across this file/`superblock.rs`/`truncate.rs`
//! (legal: inherent impls have no cross-file/orphan restriction within one
//! crate). `__xv6fs_dirlink`'s new home ([`Xv6fsInode::dirlink`]) is a
//! deliberate reading-over-literal-first-param call: its primary subject
//! is the directory inode `dp` (the superblock param is only needed for
//! `xv6fs_log_write`), matching the other three directory-entry-family
//! methods already on `Xv6fsInode`. Every relocated body is
//! byte-identical to its old free-fn form — only the wrapping `impl`
//! block and call-site qualification changed; raw-pointer parameters are
//! unchanged (no first param became `&self`/`&T` — see this crate's
//! freeze-noalias hazard note for why that would be unsound for a
//! lock-free-read `Freeze` type like `Xv6fsInode`). `slab_free` stays a
//! free fn (floor: address-taken precedent, see `crate::mm::slab_free`'s
//! own doc).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings::{
    dev_t, dinode, dirent, loff_t, mode_t, stat, vfs_dentry, vfs_dir_iter, vfs_file, vfs_inode,
    vfs_superblock, xv6fs_inode, xv6fs_superblock, EINVAL,
};
use crate::vfs::inode::InodeOps;

use super::DIRSIZ;

// ===========================================================================
// Native `xv6fs_inode` — P3-N8 nativization (user directive: remove the
// C-compatible interfaces). `Xv6fsInode` is the canonical native
// definition of `kernel/vfs/xv6fs/xv6fs_private.h`'s `struct
// xv6fs_inode`: `build.rs` blocklists the bindgen emission and
// re-exports this type as `crate::bindings::xv6fs_inode` (facade
// `pub use`, N2 pattern).
//
// `addrs[13]` is the IN-MEMORY copy of the on-disk `dinode.addrs`
// (`XV6FS_NDIRECT + 2` == `NDIRECT + 2` == 13: direct + indirect +
// double-indirect) — the on-disk `dinode` record itself is the native
// [`Dinode`] below since P3-4a (it was still bindgen-emitted when this
// struct nativized in N8); `xv6fs_iupdate`/inode-load copy
// element-by-element between the two.
//
// DERIVE DECISION (P3-N8): no derives — the bindgen emission derived
// neither Copy nor Clone (it embeds the NONCOPY `vfs_inode` by value);
// the native faithfully has no derives.
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen form + cross-compiler value probe (toolchain gcc,
// rv64gc/lp64d — scratchpad p3n8_probe_values.c); both agree on every
// value asserted below (no pipe-style divergence).
// ===========================================================================

/// Native `struct xv6fs_inode` (`kernel/vfs/xv6fs/xv6fs_private.h`).
#[repr(C, align(64))]
pub struct Xv6fsInode {
    pub vfs_inode: vfs_inode,
    /// Device number (for lookup).
    pub dev: crate::bindings::uint,
    /// Block addresses (direct + indirect + double indirect).
    pub addrs: [crate::bindings::uint; 13],
    /// Major device number (for device files).
    pub major: core::ffi::c_short,
    /// Minor device number (for device files).
    pub minor: core::ffi::c_short,
}

// P3-N8 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc probe (see the module
// note above).
const _: () = {
    assert!(core::mem::size_of::<Xv6fsInode>() == 1152, "xv6fs_inode size");
    assert!(core::mem::align_of::<Xv6fsInode>() == 64, "xv6fs_inode alignment");
    assert!(core::mem::offset_of!(Xv6fsInode, vfs_inode) == 0, "xi.vfs_inode offset");
    assert!(core::mem::offset_of!(Xv6fsInode, dev) == 1088, "xi.dev offset");
    assert!(core::mem::offset_of!(Xv6fsInode, addrs) == 1092, "xi.addrs offset");
    assert!(core::mem::offset_of!(Xv6fsInode, major) == 1144, "xi.major offset");
    assert!(core::mem::offset_of!(Xv6fsInode, minor) == 1146, "xi.minor offset");
};

// ===========================================================================
// Native on-disk `dinode` + `dirent` — P3-4a nativization (user
// directive: remove the C-compatible interfaces; on-disk format
// scrutiny class). `Dinode`/`Dirent` are the canonical KERNEL-SIDE
// definitions of `kernel/inc/vfs/xv6fs/ondisk.h`'s `struct dinode`/
// `struct dirent`: `build.rs` blocklists the bindgen emissions and
// re-exports these types as `crate::bindings::dinode`/`dirent`
// (facade `pub use`, N2 pattern).
//
// *** ON-DISK LAYOUT — HANDLE WITH P3-4 SCRUTINY *** The C header
// STAYS: `mkfs/mkfs.c` includes `ondisk.h` and writes every inode
// block and root-directory entry of fs.img on the HOST from it, so
// these natives and that header are two independent spellings of one
// persistent format. The kernel casts buffer-cache block bytes
// straight to `*mut dinode` (`xv6fs_iupdate`/`xv6fs_alloc_inode`/
// `xv6fs_get_inode`/`__xv6fs_free_disk_inode`, at `ino % IPB` element
// strides — `size_of::<dinode>()` IS the stride, so the size assert is
// load-bearing for every inode past the first) and `memmove`s
// directory blocks through `dirent` (`read_dirent`/`__xv6fs_dirlink`/
// `xv6fs_unlink`/`xv6fs_readdir`); any drift between the two
// spellings is silent on-disk corruption. The byte-exact asserts
// below pin the natives to the header layout; the cross-compiler
// probe (see "Layout evidence") additionally proved the header lays
// out IDENTICALLY on the rv64gc target and the x86_64 host — the
// mkfs-vs-kernel half of the contract.
//
// `type_` reproduces bindgen's rename of the C field `type` (reserved
// word) — every consumer already spells it that way. The array
// lengths reproduce bindgen's emission verbatim: `addrs[13]` ==
// `NDIRECT + 2` (`super::NDIRECT` is the same `ondisk.h` value,
// asserted below so the const and the layout can never drift apart)
// and `name[14]` == `DIRSIZ` (`super::DIRSIZ`, used directly).
//
// DERIVE DECISIONS (P3-4a): Copy + Clone on both, exactly as the
// pre-nativization bindgen output derived (plain-int PODs;
// `read_dirent` returns `dirent` by value).
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + cross-compiler value probe (toolchain gcc
// rv64gc/lp64d AND host x86_64 gcc — scratchpad p3_4a_ondisk_probe.c);
// all three agree on every value asserted below.
// ===========================================================================

/// Native on-disk `struct dinode` (`kernel/inc/vfs/xv6fs/ondisk.h`) —
/// one on-disk inode record, `IPB` (16) per inode block.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Dinode {
    /// File type (`XV6_T_DIR`/`XV6_T_FILE`/... — C field `type`).
    pub type_: core::ffi::c_short,
    /// Major device number (device inodes only).
    pub major: core::ffi::c_short,
    /// Minor device number (device inodes only).
    pub minor: core::ffi::c_short,
    /// Number of links to this inode in the file system.
    pub nlink: core::ffi::c_short,
    /// Size of file (bytes).
    pub size: crate::bindings::uint,
    /// Data block addresses (`NDIRECT` direct + indirect +
    /// double-indirect).
    pub addrs: [crate::bindings::uint; 13],
}

/// Native on-disk `struct dirent` (`kernel/inc/vfs/xv6fs/ondisk.h`) —
/// one directory entry; a directory's data blocks are a flat array of
/// these.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Dirent {
    /// Inode number (0 = free entry).
    pub inum: crate::bindings::ushort,
    /// Entry name, NUL-padded (no terminator if all `DIRSIZ` bytes are
    /// used).
    pub name: [c_char; DIRSIZ],
}

// P3-4a hardcoded layout proof — the ON-DISK byte contract (`ondisk.h`
// `struct dinode`/`struct dirent`), every field. Values captured from
// the pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the two-arch gcc probe.
const _: () = {
    assert!(core::mem::size_of::<Dinode>() == 64, "dinode size (ON-DISK, the IPB stride)");
    assert!(core::mem::align_of::<Dinode>() == 4, "dinode alignment");
    assert!(core::mem::offset_of!(Dinode, type_) == 0, "di.type offset (ON-DISK)");
    assert!(core::mem::offset_of!(Dinode, major) == 2, "di.major offset (ON-DISK)");
    assert!(core::mem::offset_of!(Dinode, minor) == 4, "di.minor offset (ON-DISK)");
    assert!(core::mem::offset_of!(Dinode, nlink) == 6, "di.nlink offset (ON-DISK)");
    assert!(core::mem::offset_of!(Dinode, size) == 8, "di.size offset (ON-DISK)");
    assert!(core::mem::offset_of!(Dinode, addrs) == 12, "di.addrs offset (ON-DISK)");
    assert!(super::NDIRECT as usize + 2 == 13, "dinode.addrs length == NDIRECT + 2 (ON-DISK)");

    assert!(core::mem::size_of::<Dirent>() == 16, "dirent size (ON-DISK)");
    assert!(core::mem::align_of::<Dirent>() == 2, "dirent alignment");
    assert!(core::mem::offset_of!(Dirent, inum) == 0, "de.inum offset (ON-DISK)");
    assert!(core::mem::offset_of!(Dirent, name) == 2, "de.name offset (ON-DISK)");
    assert!(DIRSIZ == 14, "dirent.name length == DIRSIZ (ON-DISK)");
};

// N-B1: POD byte-view markers for the two on-disk records reinterpreted
// out of locked block-cache buffers below. Both are verified truly POD:
// `#[repr(C)]`, only plain-integer fields (no pointers/references, no
// `bool`/`char`/enums/niches), and — per the byte-exact layout asserts
// above — NO padding (`dinode`: 8+4+52 == 64; `dirent`: 2+14 == 16), so
// `AsBytes` (write-back never persists an uninitialized byte) is sound in
// addition to `FromBytes`.
//
// SAFETY (`FromBytes`): every bit pattern of the record's bytes is a valid
// value — all fields are `c_short`/`uint`/`ushort`/`c_char` integers.
// SAFETY (`AsBytes`): `#[repr(C)]` with the padding-free layout asserted
// above, no pointers/references.
unsafe impl crate::kstd::FromBytes for Dinode {}
unsafe impl crate::kstd::AsBytes for Dinode {}
unsafe impl crate::kstd::FromBytes for Dirent {}
unsafe impl crate::kstd::AsBytes for Dirent {}

// Sub-wave B landed: xv6fs/{truncate,log,file}.rs are Rust siblings in
// this same driver now, so these are plain Rust-path imports (not
// `extern "C"`), matching this crate's established filesystem-driver
// convention -- see `truncate.rs`'s module doc ("Intra-driver calls") for
// the precedent (`tmpfs/superblock.rs`'s own `super::inode::
// tmpfs_make_directory` call).
use crate::proc::proc_shims::xv6_panic;

// Wave B free-fn -> associated-fn sweep (log.rs): `xv6fs_log_write` is now
// `Xv6fsSuperblock::log_write` (see `log.rs`'s own module doc).
use super::superblock::Xv6fsSuperblock;
// Wave A: `xv6fs_bmap`/`xv6fs_bmap_read`/`xv6fs_itrunc` are now
// `Xv6fsInode` associated fns (see `truncate.rs`'s own sweep);
// `Xv6fsInode` is already in scope (this file defines it), so no new
// import is needed for those. `xv6fs_truncate` -> `truncate::Xv6fs::truncate`
// and `xv6fs_inode_pcache_init` -> `superblock::Xv6fs::inode_pcache_init`
// are referenced via their defining file's own local `Xv6fs` marker (see
// this file's own `Xv6fs` below -- each of this driver's three Wave-A
// files gets its own private ZST, matching the established per-file
// `major`/`minor`/`mkdev`/`neg` convention; a single crate-wide marker
// would collide those same-named-but-differently-typed helpers).

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
use crate::bufcache::Buf;

// P3-D3a: `pcache_teardown` (mm/pcache.rs) is an ordinary (safe) Rust fn
// now that its `#[no_mangle]` export is gone; identical signature, plain
// `use`. `slab_free` is genuinely `unsafe fn` in `crate::mm::slab`; the
// thin wrapper preserves the `safe fn` facade the old redeclaration
// asserted (see `cffi::raw`'s identical note).
use crate::mm::Pcache;

/// FLOOR (Wave A sweep): stays a free fn -- address-taken precedent (see
/// this crate's other `slab_free` facades, e.g. `superblock.rs`'s own
/// copy); not converted to an associated fn.
///
/// SAFETY: `obj` must originate from the paired `slab_alloc`
/// (see [`crate::mm::slab::slab_free`]'s contract).
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}

// P3-1C mesh sweep: vfs/{fs,inode}.rs are in scope for this wave;
// converted from `extern "C"` redeclarations to plain crate-path items
// (identical signatures).
use crate::vfs::fs::vfs_release_dentry;
use crate::vfs::inode::VfsInode;

// `is_err_or_null`/`ptr_err`'s canonical home is `crate::kstd` (P3-CS1
// centralization). P3-CS12: the ERR_PTR-returning inode-ops vtable methods
// (`__xv6fs_create`/`__xv6fs_mkdir`/`__xv6fs_symlink`/`__xv6fs_mknod`) now
// build a `KResult<*mut vfs_inode>` internally and encode it to the
// ABI-fixed ERR_PTR exactly once at the `extern "C"` boundary via
// `result_to_errptr`; only the error-return encoding changed, the on-disk
// I/O / lock / cleanup control flow is byte-identical.
use crate::kstd::{BlockView, Errno, KResult};

/// ZST marker (Wave A free-fn -> associated-fn sweep, this file only) for
/// the `vfs_inode_ops` callback bodies -- they take the *generic* `*mut
/// vfs_inode`/`*mut vfs_dentry`/`*mut vfs_dir_iter` (cast down to
/// `*mut xv6fs_inode` internally) and so don't belong to a single concrete
/// xv6fs data type -- plus the small scalar `major`/`minor`/`mkdev`/`neg`
/// helpers with no natural type home. Private to this file, matching
/// `superblock.rs`/`truncate.rs`'s own separate `Xv6fs` markers (each
/// file keeps its own, mirroring the established per-file
/// `major`/`minor`/`mkdev`/`neg` duplication convention -- a single
/// crate-wide marker would collide same-named-but-differently-typed
/// helpers, e.g. this file's `major`/`minor` return `i16`,
/// `superblock.rs`'s return `i32`).
struct Xv6fs;

impl Xv6fs {
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
    #[inline(always)]
    const fn neg(e: u32) -> c_int {
        -(e as c_int)
    }
}

const DIRENT_SIZE: u32 = core::mem::size_of::<dirent>() as u32;

// ===========================================================================
// Inode update/sync
// ===========================================================================

impl Xv6fsInode {
    /// Mirrors `xv6fs_iupdate()`.
    ///
    /// # Safety
    /// `ip` must point to a live `xv6fs_inode` whose `vfs_inode.sb` is a live
    /// `xv6fs_superblock`, with a transaction active (caller's contract, same
    /// as the C original).
    pub(crate) extern "C" fn iupdate(ip: *mut xv6fs_inode) {
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

            let bp = Buf::read((*ip).dev, super::Xv6fs::iblock((*ip).vfs_inode.ino, (*disk_sb).inodestart));
            if bp.is_null() {
                // C original does not NULL-check here either -- but an
                // unconditional deref of a NULL `bp` below would be
                // instant UB in Rust (not just a C crash), so this port
                // bails out instead. Documented per the module doc's
                // "Fidelity note".
                return;
            }
            // SAFETY: `bp` is a locked (exclusively-borrowed) block-cache
            // buffer just returned by `bread`; `(*bp).data` points to `BSIZE`
            // page-aligned, initialized bytes borrowed only here until `brelse`.
            let mut view = BlockView::from_raw_parts_mut((*bp).data, super::BSIZE as usize);
            let dip = view.nth_mut::<dinode>(((*ip).vfs_inode.ino % super::IPB) as usize);

            dip.type_ = super::Xv6fs::mode_to_type((*ip).vfs_inode.mode);
            dip.major = (*ip).major;
            dip.minor = (*ip).minor;
            dip.nlink = (*ip).vfs_inode.n_links as i16;
            dip.size = (*ip).vfs_inode.size as u32;
            dip.addrs = (*ip).addrs;

            Xv6fsSuperblock::log_write(xv6_sb, bp);
            Buf::release(bp);
        }
    }
}

impl Xv6fs {
    /// Mirrors `xv6fs_sync_inode()` (P3-10b: `KResult`-native, reached
    /// through [`Xv6fsInodeOps`]).
    fn sync_inode(inode: *mut vfs_inode) -> KResult<()> {
        if inode.is_null() {
            return Err(Errno::Inval);
        }
        let ip = inode as *mut xv6fs_inode;
        Xv6fsInode::iupdate(ip);
        // SAFETY: `inode` is live.
        unsafe { (*inode).flags.set_dirty(0) };
        Ok(())
    }

    /// Mirrors `xv6fs_dirty_inode()` (P3-10b: `KResult`-native, reached
    /// through [`Xv6fsInodeOps`]).
    fn dirty_inode(inode: *mut vfs_inode) -> KResult<()> {
        if inode.is_null() {
            return Err(Errno::Inval);
        }
        // SAFETY: `inode` is live.
        unsafe { (*inode).flags.set_dirty(1) };
        Ok(())
    }
}

// ===========================================================================
// Directory-entry scan helper — see the module doc's "Fidelity note".
// ===========================================================================

impl Xv6fsInode {
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
            let addr = if alloc { Xv6fsInode::bmap(dp, bn) } else { Xv6fsInode::bmap_read(dp, bn) };
            if addr == 0 {
                return None;
            }
            let bp = Buf::read((*dp).dev, addr);
            if bp.is_null() {
                return None;
            }
            // SAFETY: `bp` is a locked (exclusively-borrowed) block-cache
            // buffer just returned by `bread`; `(*bp).data` points to `BSIZE`
            // page-aligned, initialized bytes borrowed only here until `brelse`.
            let view = BlockView::from_raw_parts_mut((*bp).data, super::BSIZE as usize);
            let de = *view.get::<dirent>(block_off);
            Buf::release(bp);
            Some(de)
        }
    }
}

// ===========================================================================
// Directory operations
// ===========================================================================

impl Xv6fs {
    /// Mirrors `xv6fs_lookup()`.
    fn lookup(dir: *mut vfs_inode, dentry: *mut vfs_dentry, name: *const c_char, mut name_len: usize) -> KResult<()> {
        if dir.is_null() || dentry.is_null() || name.is_null() {
            return Err(Errno::Inval);
        }
        // SAFETY: `dir` is live (caller's contract).
        if !super::Xv6fs::s_isdir(unsafe { (*dir).mode }) {
            return Err(Errno::NotDir);
        }
        if name_len > DIRSIZ {
            name_len = DIRSIZ;
        }

        let dp = dir as *mut xv6fs_inode;
        // Directory size is loop-invariant here (inode locked; the scan never
        // grows it). (Goal #2) fixed-stride dir-entry scan -> range iterator.
        // SAFETY: `dir` is live.
        let size = unsafe { (*dir).size } as u32;
        for off in (0..size).step_by(DIRENT_SIZE as usize) {
            // SAFETY: `dp` is live.
            if let Some(de) = unsafe { Xv6fsInode::read_dirent(dp, off, true) } {
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
                                return Err(Errno::NoMem);
                            }
                            (*dentry).name_len = name_len as u16;
                        }
                        return Ok(());
                    }
                }
            }
        }

        Err(Errno::NoEnt)
    }

    /// Mirrors `xv6fs_dir_iter()`.
    fn dir_iter(dir: *mut vfs_inode, iter: *mut vfs_dir_iter, ret_dentry: *mut vfs_dentry) -> KResult<()> {
        if dir.is_null() || iter.is_null() || ret_dentry.is_null() {
            return Err(Errno::Inval);
        }
        // SAFETY: `dir` is live.
        if !super::Xv6fs::s_isdir(unsafe { (*dir).mode }) {
            return Err(Errno::NotDir);
        }

        let dp = dir as *mut xv6fs_inode;

        // VFS handles "." via iter->index == 0. VFS calls driver with
        // iter->index == 1 for ".." on ordinary directories. VFS calls
        // driver with iter->index > 1 for regular entries.

        // SAFETY: `iter` is live.
        if unsafe { (*iter).index } == 1 {
            // Look up ".." in the on-disk directory to get parent inode number.
            // (Goal #2) fixed-stride dir-entry scan -> range iterator (size
            // loop-invariant, inode locked).
            // SAFETY: `dir` is live.
            let size = unsafe { (*dir).size } as u32;
            for off in (0..size).step_by(DIRENT_SIZE as usize) {
                // SAFETY: `dp` is live.
                if let Some(de) = unsafe { Xv6fsInode::read_dirent(dp, off, true) } {
                    if de.inum != 0 && de.name[0] == b'.' as c_char && de.name[1] == b'.' as c_char && de.name[2] == 0 {
                        // SAFETY: `ret_dentry`/`dir` are live.
                        unsafe {
                            vfs_release_dentry(ret_dentry);
                            (*ret_dentry).name = strndup(c"..".as_ptr(), 2);
                            if (*ret_dentry).name.is_null() {
                                return Err(Errno::NoMem);
                            }
                            (*ret_dentry).name_len = 2;
                            (*ret_dentry).ino = de.inum as u64;
                            (*ret_dentry).sb = (*dir).sb; // VFS doesn't set sb for index==1
                            (*ret_dentry).cookies = 0; // Will be reset by VFS for index > 1
                        }
                        return Ok(());
                    }
                }
            }
            // ".." not found on disk (shouldn't happen for valid dirs).
            return Err(Errno::NoEnt);
        }

        // Handle regular entries when index > 1.
        // (Goal #2) fixed-stride dir-entry scan from the continuation cookie ->
        // range iterator (size loop-invariant, inode locked).
        // SAFETY: `ret_dentry`/`dir` are live.
        let start_off = unsafe { (*ret_dentry).cookies } as u32;
        let size = unsafe { (*dir).size } as u32;
        for off in (start_off..size).step_by(DIRENT_SIZE as usize) {
            // SAFETY: `dp` is live.
            if let Some(de) = unsafe { Xv6fsInode::read_dirent(dp, off, true) } {
                if de.inum != 0 {
                    // Skip . and .. as VFS handles them.
                    let is_dot = de.name[0] == b'.' as c_char && de.name[1] == 0;
                    let is_dotdot = de.name[0] == b'.' as c_char && de.name[1] == b'.' as c_char && de.name[2] == 0;
                    if !is_dot && !is_dotdot {
                        let namelen = strnlen(de.name.as_ptr(), DIRSIZ);
                        let name = strndup(de.name.as_ptr(), namelen);
                        if name.is_null() {
                            return Err(Errno::NoMem);
                        }
                        // SAFETY: `ret_dentry` is live.
                        unsafe {
                            vfs_release_dentry(ret_dentry);
                            (*ret_dentry).ino = de.inum as u64;
                            (*ret_dentry).name = name;
                            (*ret_dentry).name_len = namelen as u16;
                            (*ret_dentry).cookies = (off + DIRENT_SIZE) as i64; // Next offset for continuation.
                        }
                        return Ok(());
                    }
                }
            }
        }

        // End of directory -- return 0 with name=NULL to signal end.
        // SAFETY: `ret_dentry` is live.
        unsafe {
            vfs_release_dentry(ret_dentry);
            (*ret_dentry).name = ptr::null_mut();
            (*ret_dentry).name_len = 0;
            (*ret_dentry).cookies = super::VFS_DENTRY_COOKIE_END;
        }
        Ok(())
    }
}

// ===========================================================================
// Create/Unlink operations
// ===========================================================================

impl Xv6fsInode {
    /// Mirrors `__xv6fs_dir_name_exists()`. Returns the inode number if
    /// found, 0 if not found.
    ///
    /// # Safety
    /// `dp` must point to a live `xv6fs_inode`; `name` must be a valid,
    /// NUL-terminated (within `DIRSIZ` bytes) C string.
    unsafe fn dir_name_exists(dp: *mut xv6fs_inode, name: *const c_char) -> u32 {
        unsafe {
            // (Goal #2) name-scan -> `.find_map()` (size loop-invariant, inode
            // locked). Returns the matching inum or 0.
            let size = (*dp).vfs_inode.size as u32;
            (0..size)
                .step_by(DIRENT_SIZE as usize)
                .find_map(|off| {
                    let de = Xv6fsInode::read_dirent(dp, off, false)?;
                    (de.inum != 0 && strncmp(de.name.as_ptr(), name, DIRSIZ) == 0).then_some(de.inum as u32)
                })
                .unwrap_or(0)
        }
    }

    /// Mirrors `__xv6fs_dirlink()`. Add a directory entry.
    ///
    /// # Safety
    /// `xv6_sb`/`dp` must point to live, exclusively-accessed structures with
    /// a transaction active; `name` must be a valid C string no longer than
    /// `DIRSIZ` bytes.
    unsafe fn dirlink(xv6_sb: *mut xv6fs_superblock, dp: *mut xv6fs_inode, name: *const c_char, inum: u32) -> KResult<()> {
        unsafe {
            // Look for an empty directory slot. (Goal #2) empty-slot scan ->
            // `.find()` (size loop-invariant, inode locked).
            let size = (*dp).vfs_inode.size as u32;
            let found_off = (0..size)
                .step_by(DIRENT_SIZE as usize)
                .find(|&off| Xv6fsInode::read_dirent(dp, off, true).map_or(false, |de| de.inum == 0));
            // No empty slot found -- extend directory.
            let off = found_off.unwrap_or((*dp).vfs_inode.size as u32);

            let bn = off / super::BSIZE;
            let block_off = (off % super::BSIZE) as usize;
            let addr = Xv6fsInode::bmap(dp, bn);
            if addr == 0 {
                return Err(Errno::NoSpc);
            }
            let bp = Buf::read((*dp).dev, addr);
            if bp.is_null() {
                // See the module doc's "Fidelity note": C does not NULL-check
                // here, but an unconditional deref would be Rust UB.
                return Err(Errno::Io);
            }
            // Build the entry directly in the (exclusively-borrowed, locked)
            // block buffer — byte-identical to the old stack-build + memmove.
            // SAFETY: `bp` is a locked block-cache buffer just returned by
            // `bread`; `(*bp).data` is `BSIZE` page-aligned initialized bytes
            // borrowed only here until `brelse`.
            let mut view = BlockView::from_raw_parts_mut((*bp).data, super::BSIZE as usize);
            let de = view.get_mut::<dirent>(block_off);
            *de = core::mem::zeroed();
            strncpy(de.name.as_mut_ptr(), name, DIRSIZ);
            de.inum = inum as u16;
            Xv6fsSuperblock::log_write(xv6_sb, bp);
            Buf::release(bp);

            if off as i64 >= (*dp).vfs_inode.size {
                (*dp).vfs_inode.size = (off + DIRENT_SIZE) as i64;
                Xv6fsInode::iupdate(dp);
            }

            Ok(())
        }
    }
}

impl Xv6fs {
    fn create_inner(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
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

        // (Goal #2) existing-name scan -> `.find_map()` (size loop-invariant,
        // inode locked).
        // SAFETY: `dir`/`dp` are live.
        let existing = unsafe {
            let size = (*dir).size as u32;
            (0..size).step_by(DIRENT_SIZE as usize).find_map(|off| {
                let de = Xv6fsInode::read_dirent(dp, off, false)?;
                (de.inum != 0 && strncmp(de.name.as_ptr(), name_buf.as_ptr() as *const c_char, DIRSIZ) == 0)
                    .then_some(de.inum)
            })
        };

        if existing.is_some() {
            return Err(Errno::Exist);
        }

        // SAFETY: `dir` is live. (P3-10b: `KResult`-native VFS-core call —
        // no ERR_PTR decode, and the old dead null -> `ENOMEM` branch had
        // no producer.)
        let new_inode = crate::vfs::fs::VfsSuperblock::vfs_alloc_inode_inner(unsafe { (*dir).sb })?;
        // vfs_alloc_inode_inner returns the inode locked.

        let ip = new_inode as *mut xv6fs_inode;
        // SAFETY: `ip`/`new_inode`/`dp` are live.
        unsafe {
            (*ip).dev = (*dp).dev;
            (*new_inode).mode = mode | super::S_IFREG;
            (*new_inode).n_links = 1;
            (*new_inode).size = 0;
        }
        Xv6fsInode::iupdate(ip);

        // Add directory entry (name_buf already prepared above).
        if let Err(e) = unsafe { Xv6fsInode::dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*new_inode).ino as u32) } {
            // TODO: Free inode on failure.
            VfsInode::vfs_iunlock(new_inode);
            return Err(e);
        }

        VfsInode::vfs_iunlock(new_inode); // VFS's vfs_create will re-lock it.
        Ok(new_inode)
    }

    fn mkdir_inner(dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        if dir.is_null() || name.is_null() || name_len == 0 || name_len > DIRSIZ {
            return Err(Errno::Inval);
        }

        let dp = dir as *mut xv6fs_inode;
        // SAFETY: `dir` is live.
        let xv6_sb = unsafe { (*dir).sb as *mut xv6fs_superblock };

        let mut name_buf = [0u8; DIRSIZ];
        // SAFETY: `name` has at least `name_len` readable bytes.
        unsafe { memmove(name_buf.as_mut_ptr() as *mut c_void, name as *const c_void, name_len) };

        if unsafe { Xv6fsInode::dir_name_exists(dp, name_buf.as_ptr() as *const c_char) } != 0 {
            return Err(Errno::Exist);
        }

        // SAFETY: `dir` is live. (P3-10b: `KResult`-native VFS-core call.)
        let new_inode = crate::vfs::fs::VfsSuperblock::vfs_alloc_inode_inner(unsafe { (*dir).sb })?;
        // vfs_alloc_inode_inner returns the inode locked.

        let ip = new_inode as *mut xv6fs_inode;
        // SAFETY: `ip`/`new_inode`/`dp` are live.
        unsafe {
            (*ip).dev = (*dp).dev;
            (*new_inode).mode = mode | super::S_IFDIR;
            (*new_inode).n_links = 1;
            (*new_inode).size = 0;
        }

        // Create . and .. entries. (Any failure maps to `-EIO`, exactly as
        // the old `< 0` checks did.)
        // SAFETY: `xv6_sb`/`ip`/`dir` are live.
        unsafe {
            if Xv6fsInode::dirlink(xv6_sb, ip, c".".as_ptr(), (*new_inode).ino as u32).is_err()
                || Xv6fsInode::dirlink(xv6_sb, ip, c"..".as_ptr(), (*dir).ino as u32).is_err()
            {
                // TODO: Cleanup on failure.
                VfsInode::vfs_iunlock(new_inode);
                return Err(Errno::Io);
            }
        }

        Xv6fsInode::iupdate(ip);

        // Add directory entry in parent (name_buf already set earlier).
        if unsafe { Xv6fsInode::dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*new_inode).ino as u32) }.is_err() {
            VfsInode::vfs_iunlock(new_inode);
            return Err(Errno::Io);
        }

        // Update parent's link count for "..".
        // SAFETY: `dir`/`dp` are live.
        unsafe { (*dir).n_links += 1 };
        Xv6fsInode::iupdate(dp);

        VfsInode::vfs_iunlock(new_inode); // VFS's vfs_mkdir will re-lock it.
        Ok(new_inode)
    }

    fn unlink(dentry: *mut vfs_dentry, target: *mut vfs_inode) -> KResult<()> {
        if dentry.is_null() || target.is_null() {
            return Err(Errno::Inval);
        }
        // SAFETY: `dentry`/`target` are live (caller's contract).
        unsafe {
            if (*dentry).sb.is_null() || (*dentry).sb != (*target).sb {
                return Err(Errno::Inval);
            }
            if (*dentry).ino != (*target).ino {
                return Err(Errno::Inval);
            }

            // VFS core handled checking for "." and "..".
            let dp = (*dentry).parent as *mut xv6fs_inode;

            // (Goal #2) entry-scan -> range iterator (size loop-invariant,
            // inode locked).
            let size = (*(*dentry).parent).size as u32;
            for off in (0..size).step_by(DIRENT_SIZE as usize) {
                if let Some(de) = Xv6fsInode::read_dirent(dp, off, true) {
                    if de.inum != 0 {
                        let de_name_len = strnlen(de.name.as_ptr(), DIRSIZ);
                        if (*dentry).name_len as usize == de_name_len && strncmp((*dentry).name, de.name.as_ptr(), (*dentry).name_len as usize) == 0 {
                            // Found -- clear the entry.
                            if de.inum as u64 != (*target).ino {
                                return Err(Errno::Inval); // Inode number mismatch.
                            }

                            let bn = off / super::BSIZE;
                            let block_off = (off % super::BSIZE) as usize;
                            let addr = Xv6fsInode::bmap(dp, bn);
                            let bp = Buf::read((*dp).dev, addr);
                            if bp.is_null() {
                                // See the module doc's "Fidelity note".
                                return Err(Errno::Io);
                            }
                            // Clear the entry in place — byte-identical to the
                            // old zeroed-stack-copy + memmove write-back.
                            // SAFETY: `bp` is a locked block-cache buffer just
                            // returned by `bread`; `(*bp).data` is `BSIZE`
                            // page-aligned initialized bytes borrowed only here
                            // until `brelse`.
                            let mut view = BlockView::from_raw_parts_mut((*bp).data, super::BSIZE as usize);
                            *view.get_mut::<dirent>(block_off) = core::mem::zeroed();
                            let xv6_sb = (*dentry).sb as *mut xv6fs_superblock;
                            Xv6fsSuperblock::log_write(xv6_sb, bp);
                            Buf::release(bp);

                            // inode is already locked by vfs_get_inode.
                            (*target).n_links -= 1;

                            let tip = target as *mut xv6fs_inode;
                            Xv6fsInode::iupdate(tip);

                            // Return the target inode -- VFS will call
                            // vfs_iput on it after releasing the superblock
                            // lock. This handles both cases: n_links == 0
                            // (inode freed when refcount reaches 0) or
                            // n_links > 0 (just releases the reference from
                            // vfs_get_inode).
                            return Ok(());
                        }
                    }
                }
            }

            Err(Errno::NoEnt)
        }
    }

    fn rmdir(dentry: *mut vfs_dentry, target: *mut vfs_inode) -> KResult<()> {
        let ret = Xv6fs::unlink(dentry, target);
        if ret.is_ok() {
            // Decrement parent's link count (for the ".." entry in the
            // removed directory). This mirrors xv6fs_mkdir which increments
            // parent's n_links when creating a subdir.
            // SAFETY: `dentry` is live (unlink succeeded above, so it was
            // valid).
            unsafe {
                let dp = (*dentry).parent as *mut xv6fs_inode;
                (*(*dentry).parent).n_links -= 1;
                Xv6fsInode::iupdate(dp);
            }
        }
        ret
    }

    fn link(old: *mut vfs_inode, dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> KResult<()> {
        if old.is_null() || dir.is_null() || name.is_null() || name_len > DIRSIZ {
            return Err(Errno::Inval);
        }
        // SAFETY: `old` is live.
        if super::Xv6fs::s_isdir(unsafe { (*old).mode }) {
            return Err(Errno::Perm); // Can't hard link directories.
        }

        let dp = dir as *mut xv6fs_inode;
        let ip = old as *mut xv6fs_inode;
        // SAFETY: `dir` is live.
        let xv6_sb = unsafe { (*dir).sb as *mut xv6fs_superblock };

        let mut name_buf = [0u8; DIRSIZ];
        // SAFETY: `name` has at least `name_len` readable bytes.
        unsafe { memmove(name_buf.as_mut_ptr() as *mut c_void, name as *const c_void, name_len) };

        if unsafe { Xv6fsInode::dir_name_exists(dp, name_buf.as_ptr() as *const c_char) } != 0 {
            return Err(Errno::Exist);
        }

        // SAFETY: `old` is live.
        unsafe { (*old).n_links += 1 };
        Xv6fsInode::iupdate(ip);

        if let Err(e) = unsafe { Xv6fsInode::dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*old).ino as u32) } {
            // SAFETY: `old` is live.
            unsafe { (*old).n_links -= 1 };
            Xv6fsInode::iupdate(ip);
            return Err(e);
        }

        Ok(())
    }
}

// ===========================================================================
// Symlink operations
// ===========================================================================

impl Xv6fs {
    fn readlink(inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> KResult<isize> {
        if inode.is_null() || buf.is_null() {
            return Err(Errno::Inval);
        }
        // SAFETY: `inode` is live.
        if !super::Xv6fs::s_islnk(unsafe { (*inode).mode }) {
            return Err(Errno::Inval);
        }

        let ip = inode as *mut xv6fs_inode;
        // SAFETY: `inode` is live.
        let link_len = unsafe { (*inode).size } as usize;

        if link_len + 1 > buflen {
            return Err(Errno::NameTooLong);
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

            let addr = Xv6fsInode::bmap(ip, bn);
            if addr == 0 {
                return Err(Errno::Io);
            }
            // SAFETY: `ip` is live.
            let bp = Buf::read(unsafe { (*ip).dev }, addr);
            if bp.is_null() {
                return Err(Errno::Io);
            }
            // SAFETY: `bp`/`buf` are live.
            unsafe {
                memmove(
                    buf.add(bytes_read) as *mut c_void,
                    (*bp).data.add(off as usize) as *const c_void,
                    n as usize,
                );
                Buf::release(bp);
            }

            bytes_read += n as usize;
        }

        // SAFETY: `buf` has at least `link_len + 1` writable bytes (checked
        // above).
        unsafe { *buf.add(link_len) = 0 };
        Ok(link_len as isize)
    }

    fn symlink_inner(
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

        if unsafe { Xv6fsInode::dir_name_exists(dp, name_buf.as_ptr() as *const c_char) } != 0 {
            return Err(Errno::Exist);
        }

        // SAFETY: `dir` is live. (P3-10b: `KResult`-native VFS-core call.)
        let new_inode = crate::vfs::fs::VfsSuperblock::vfs_alloc_inode_inner(unsafe { (*dir).sb })?;
        // vfs_alloc_inode_inner returns the inode locked.

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

            let addr = Xv6fsInode::bmap(ip, bn);
            if addr == 0 {
                // Failed to allocate block -- cleanup.
                Xv6fsInode::itrunc(ip);
                VfsInode::vfs_iunlock(new_inode);
                return Err(Errno::NoSpc);
            }

            // SAFETY: `ip` is live.
            let bp = Buf::read(unsafe { (*ip).dev }, addr);
            if bp.is_null() {
                Xv6fsInode::itrunc(ip);
                VfsInode::vfs_iunlock(new_inode);
                return Err(Errno::Io);
            }
            // SAFETY: `bp`/`target` are live.
            unsafe {
                memmove(
                    (*bp).data.add(off as usize) as *mut c_void,
                    target.add(bytes_written) as *const c_void,
                    n as usize,
                );
            }
            Xv6fsSuperblock::log_write(xv6_sb, bp);
            Buf::release(bp);

            bytes_written += n as usize;
        }

        // SAFETY: `new_inode` is live.
        unsafe { (*new_inode).size = target_len as loff_t };
        Xv6fsInode::iupdate(ip);

        // Add directory entry (name_buf already set earlier).
        if let Err(e) = unsafe { Xv6fsInode::dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*new_inode).ino as u32) } {
            Xv6fsInode::itrunc(ip);
            VfsInode::vfs_iunlock(new_inode);
            return Err(e);
        }

        VfsInode::vfs_iunlock(new_inode);
        Ok(new_inode)
    }
}

// ===========================================================================
// Device file operations (mknod)
// ===========================================================================

impl Xv6fs {
    fn mknod_inner(dir: *mut vfs_inode, mode: mode_t, dev: dev_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        if dir.is_null() || name.is_null() || name_len == 0 || name_len > DIRSIZ {
            return Err(Errno::Inval);
        }
        // xv6 only supports character and block devices.
        if !super::Xv6fs::s_isblk(mode) && !super::Xv6fs::s_ischr(mode) {
            return Err(Errno::Inval);
        }

        let dp = dir as *mut xv6fs_inode;
        // SAFETY: `dir` is live.
        let xv6_sb = unsafe { (*dir).sb as *mut xv6fs_superblock };

        // SAFETY: `dir` is live. (P3-10b: `KResult`-native VFS-core call.)
        let new_inode = crate::vfs::fs::VfsSuperblock::vfs_alloc_inode_inner(unsafe { (*dir).sb })?;
        // vfs_alloc_inode_inner returns the inode locked.

        let ip = new_inode as *mut xv6fs_inode;
        // SAFETY: `ip`/`new_inode`/`dp` are live.
        unsafe {
            (*ip).dev = (*dp).dev;
            (*new_inode).mode = mode;
            (*new_inode).n_links = 1;
            (*new_inode).size = 0;

            (*ip).major = Xv6fs::major(dev);
            (*ip).minor = Xv6fs::minor(dev);
            if super::Xv6fs::s_ischr(mode) {
                (*new_inode).dev_mnt.cdev = dev;
            } else if super::Xv6fs::s_isblk(mode) {
                (*new_inode).dev_mnt.bdev = dev;
            }
        }

        Xv6fsInode::iupdate(ip);

        let mut name_buf = [0u8; DIRSIZ];
        // SAFETY: `name` has at least `name_len` readable bytes.
        unsafe { memmove(name_buf.as_mut_ptr() as *mut c_void, name as *const c_void, name_len) };

        if let Err(e) = unsafe { Xv6fsInode::dirlink(xv6_sb, dp, name_buf.as_ptr() as *const c_char, (*new_inode).ino as u32) } {
            // TODO: Free inode on failure.
            VfsInode::vfs_iunlock(new_inode);
            return Err(e);
        }

        VfsInode::vfs_iunlock(new_inode); // VFS's vfs_mknod will re-lock it.
        Ok(new_inode)
    }
}

// ===========================================================================
// Inode lifecycle
// ===========================================================================

impl Xv6fs {
    /// Mirrors `xv6fs_destroy_inode()` (P3-10b: reached through
    /// [`Xv6fsInodeOps`]).
    fn destroy_inode(inode: *mut vfs_inode) {
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
                Pcache::teardown(ptr::addr_of_mut!((*inode).i_data));
            }

            // Note: Transaction is managed by VFS layer (vfs_iput calls
            // begin/end_transaction).
            Xv6fsInode::itrunc(ip);

            // Mark inode as free on disk.
            let bp = Buf::read((*ip).dev, super::Xv6fs::iblock((*inode).ino, (*xv6_sb).disk_sb.inodestart));
            if bp.is_null() {
                // See the module doc's "Fidelity note": the C original does
                // not NULL-check here either, but an unconditional deref
                // would be Rust UB.
                return;
            }
            // SAFETY: `bp` is a locked (exclusively-borrowed) block-cache
            // buffer just returned by `bread`; `(*bp).data` points to `BSIZE`
            // page-aligned, initialized bytes borrowed only here until `brelse`.
            let mut view = BlockView::from_raw_parts_mut((*bp).data, super::BSIZE as usize);
            view.nth_mut::<dinode>(((*inode).ino % super::IPB) as usize).type_ = 0;
            Xv6fsSuperblock::log_write(xv6_sb, bp);
            Buf::release(bp);
        }
    }

    /// Mirrors `xv6fs_free_inode()` (P3-10b: reached through
    /// [`Xv6fsInodeOps`]).
    fn free_inode(inode: *mut vfs_inode) {
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
                Pcache::teardown(ptr::addr_of_mut!((*inode).i_data));
            }
        }

        let ip = inode as *mut xv6fs_inode;
        slab_free(ip as *mut c_void);
    }
}

// ===========================================================================
// Open callback
// ===========================================================================

impl Xv6fs {
    /// Mirrors `xv6fs_open()` (P3-10b: `KResult`-native, reached through
    /// [`Xv6fsInodeOps`]).
    fn open(inode: *mut vfs_inode, file: *mut vfs_file, _f_flags: c_int) -> KResult<()> {
        if inode.is_null() || file.is_null() {
            return Err(Errno::Inval);
        }

        // SAFETY: `inode` is live.
        let mode = unsafe { (*inode).mode };

        if super::Xv6fs::s_isreg(mode) {
            // SAFETY: `file` is live.
            unsafe {
                (*file).ops = Some(&super::file::XV6FS_FILE_OPS);
                // Lazily initialise the per-inode page cache on first open.
                // VFS calls open with the inode locked, so this is race-free.
                if (*inode).i_data.flags.bits.active() == 0 {
                    super::superblock::Xv6fs::inode_pcache_init(inode);
                }
            }
            return Ok(());
        }

        if super::Xv6fs::s_isdir(mode) {
            // Directories use dir_iter for reading.
            // SAFETY: `file` is live.
            unsafe { (*file).ops = Some(&super::file::XV6FS_FILE_OPS) };
            return Ok(());
        }

        if super::Xv6fs::s_islnk(mode) {
            // FIX (preserved from C): allow opening symlinks with O_NOFOLLOW
            // flag. POSIX requires that symlinks can be opened with
            // O_NOFOLLOW to allow fstat() on the symlink itself (not its
            // target). This is needed by programs like ls that want to
            // display symlink information.
            // SAFETY: `file` is live.
            unsafe { (*file).ops = Some(&super::file::XV6FS_FILE_OPS) };
            return Ok(());
        }

        // Character/block devices are handled by VFS core.
        if super::Xv6fs::s_ischr(mode) || super::Xv6fs::s_isblk(mode) {
            return Err(Errno::Inval); // Should be handled by VFS.
        }

        Err(Errno::NoSys)
    }

    fn getattr(inode: *mut vfs_inode, stat: *mut stat) -> KResult<()> {
        if inode.is_null() || stat.is_null() {
            return Err(Errno::Inval);
        }
        let ip = inode as *mut xv6fs_inode;
        VfsInode::vfs_ilock(inode);
        // SAFETY: `inode`/`stat` are live and `inode` is now locked.
        unsafe {
            ptr::write_bytes(stat, 0, 1);
            (*stat).dev = (*ip).dev as i32;
            (*stat).ino = (*inode).ino;
            (*stat).mode = (*inode).mode;
            (*stat).nlink = (*inode).n_links;
            (*stat).size = (*inode).size as u64;
        }
        VfsInode::vfs_iunlock(inode);
        Ok(())
    }
}

// ===========================================================================
// VFS inode operations structure (P3-10b: `InodeOps` trait impl over a
// zero-sized unit struct; the 19-slot fn-pointer table is gone).
// ===========================================================================

/// xv6fs's [`InodeOps`] implementor (P3-10b). The old table's `None`
/// `move_` slot is the trait's `Err(NoSys)` default (TODO: implement
/// move/rename); every other method delegates to the (now
/// `KResult`-native) former callback bodies in this module and its
/// siblings.
pub(crate) struct Xv6fsInodeOps;

impl InodeOps for Xv6fsInodeOps {
    unsafe fn lookup(&self, dir: *mut vfs_inode, dentry: *mut vfs_dentry, name: *const c_char, name_len: usize) -> KResult<()> {
        Xv6fs::lookup(dir, dentry, name, name_len)
    }
    unsafe fn dir_iter(&self, dir: *mut vfs_inode, iter: *mut vfs_dir_iter, ret_dentry: *mut vfs_dentry) -> KResult<()> {
        Xv6fs::dir_iter(dir, iter, ret_dentry)
    }
    unsafe fn readlink(&self, inode: *mut vfs_inode, buf: *mut c_char, buflen: usize) -> KResult<isize> {
        Xv6fs::readlink(inode, buf, buflen)
    }
    unsafe fn create(&self, dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        Xv6fs::create_inner(dir, mode, name, name_len)
    }
    unsafe fn getattr(&self, inode: *mut vfs_inode, stat: *mut stat) -> KResult<()> {
        Xv6fs::getattr(inode, stat)
    }
    /// Mirrors `__xv6fs_setattr` — not supported (no VFS dispatch site
    /// exists yet either).
    unsafe fn setattr(&self, _inode: *mut vfs_inode, _stat: *const stat) -> KResult<()> {
        Err(Errno::OpNotSupp)
    }
    unsafe fn link(&self, old: *mut vfs_inode, dir: *mut vfs_inode, name: *const c_char, name_len: usize) -> KResult<()> {
        Xv6fs::link(old, dir, name, name_len)
    }
    unsafe fn unlink(&self, dentry: *mut vfs_dentry, target: *mut vfs_inode) -> KResult<()> {
        Xv6fs::unlink(dentry, target)
    }
    unsafe fn mkdir(&self, dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        Xv6fs::mkdir_inner(dir, mode, name, name_len)
    }
    unsafe fn rmdir(&self, dentry: *mut vfs_dentry, target: *mut vfs_inode) -> KResult<()> {
        Xv6fs::rmdir(dentry, target)
    }
    unsafe fn mknod(&self, dir: *mut vfs_inode, mode: mode_t, dev: dev_t, name: *const c_char, name_len: usize) -> KResult<*mut vfs_inode> {
        Xv6fs::mknod_inner(dir, mode, dev, name, name_len)
    }
    unsafe fn symlink(&self, dir: *mut vfs_inode, mode: mode_t, name: *const c_char, name_len: usize, target: *const c_char, target_len: usize) -> KResult<*mut vfs_inode> {
        Xv6fs::symlink_inner(dir, mode, name, name_len, target, target_len)
    }
    unsafe fn truncate(&self, inode: *mut vfs_inode, new_size: loff_t) -> KResult<()> {
        super::truncate::Xv6fs::truncate(inode, new_size)
    }
    unsafe fn destroy_inode(&self, inode: *mut vfs_inode) {
        Xv6fs::destroy_inode(inode);
    }
    unsafe fn free_inode(&self, inode: *mut vfs_inode) {
        Xv6fs::free_inode(inode);
    }
    unsafe fn dirty_inode(&self, inode: *mut vfs_inode) -> KResult<()> {
        Xv6fs::dirty_inode(inode)
    }
    unsafe fn sync_inode(&self, inode: *mut vfs_inode) -> KResult<()> {
        Xv6fs::sync_inode(inode)
    }
    unsafe fn open(&self, inode: *mut vfs_inode, file: *mut vfs_file, f_flags: c_int) -> KResult<()> {
        Xv6fs::open(inode, file, f_flags)
    }
}

pub(crate) static XV6FS_INODE_OPS: Xv6fsInodeOps = Xv6fsInodeOps;
