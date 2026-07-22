//! xv6fs — Rust port of `kernel/vfs/xv6fs/{superblock,inode,truncate,file,
//! log,block_cache}.c` (Phase 2 Wave 19, see `docs/rustify/phase2_plan.md`).
//!
//! xv6fs is the **root filesystem** mounted at `/root` on every QEMU boot
//! (`xv6fs_mount_root`, called from `start_kernel.c` after tmpfs/devtmpfs
//! are up) — a bug here bricks boot. It fills the C-ABI
//! `vfs_inode_ops`/`vfs_file_ops`/`vfs_superblock_ops`/`vfs_fs_type_ops`
//! vtables vfs core (100% Rust since Wave 17) dispatches through, exactly
//! like [`super::tmpfs`] (Wave 18) — every op fn here keeps the exact C
//! signature bindgen derived from `vfs/vfs_types.h`, and the vtable
//! statics move from C designated initializers to plain Rust `static`
//! values.
//!
//! Unlike tmpfs, xv6fs is a genuine *backend* filesystem: it persists
//! data on a block device (ramdisk by preference, virtio disk as
//! fallback — see [`superblock::xv6fs_mount_root`]) through three extra
//! subsystems tmpfs never needed:
//!
//! * [`log`] <- `log.c`: the write-ahead log, xv6's crash-consistency
//!   mechanism. `begin_op`/`end_op` bracket every filesystem-metadata
//!   change; `end_op` commits (write log blocks, write the commit header,
//!   install to home locations, erase the log) when the last outstanding
//!   operation completes. Block-count/outstanding-op accounting and
//!   commit ordering are ported **exactly** — a fidelity bug here
//!   deadlocks concurrent writers (log-full wait) or corrupts the
//!   filesystem across a crash.
//! * [`block_cache`] <- `block_cache.c`: an rb-tree (the Wave-1 Rust
//!   [`crate::rbtree`]/[`crate::bintree`] port) of free block *extents*
//!   (contiguous runs), replacing a linear on-disk-bitmap scan with
//!   O(log n) allocation + adjacent-extent merging on free. Boot prints
//!   `"xv6fs: block cache initialized: N data blocks, M free in K
//!   extents"`.
//! * `bio`/`blkdev` (`kernel/dev/bio.c`/`blkdev.c`, still C, untouched):
//!   the block-I/O submission path xv6fs's per-inode pcache
//!   `read_page`/`write_page` callbacks use to move data to/from the
//!   block device (data blocks bypass the log — only metadata is
//!   logged).
//!
//! # Sub-wave split (matches the C file split; see the plan doc)
//!
//! * Sub-wave A — [`superblock`] <- `superblock.c`: slab caches,
//!   inode/superblock alloc+free, on-disk superblock read/write,
//!   `vfs_superblock_ops`, the `xv6fs` `vfs_fs_type` registration, and
//!   mounting xv6fs at `/root` + chroot (`xv6fs_mount_root` — the boot
//!   path this wave must not break). [`inode`] <- `inode.c`: directory
//!   entry lookup/link/unlink (flat on-disk `struct dirent` array, no
//!   in-memory hash unlike tmpfs), every `vfs_inode_ops` callback, and
//!   `vfs_inode_ops` itself. Boot-critical: verified standalone (build +
//!   full boot gate) before sub-wave B started, with `log.c`/`truncate.c`/
//!   `file.c`/`block_cache.c` still C, calling into this sub-wave's
//!   `#[no_mangle]` symbols exactly as they called the original C
//!   (`xv6fs_private.h`, the C-ABI contract header, is unchanged in
//!   shape — only one `#include` line was narrowed, see its own doc
//!   comment).
//! * Sub-wave B — [`file`] <- `file.c`: per-inode pcache wiring
//!   (`read_page`/`write_page` via `bio`), every `vfs_file_ops` callback,
//!   and `vfs_file_ops` itself. [`truncate`] <- `truncate.c`: block
//!   mapping (`bmap`, direct/single-indirect/double-indirect), grow/shrink
//!   truncation in batched transactions. [`log`] <- `log.c`. [`block_cache`]
//!   <- `block_cache.c`.
//!
//! `xv6fs_smoketest.c`/`.h` were **not** ported: dead code (its one call
//! site, `xv6fs_run_all_smoketests()` in the old `superblock.c`, was
//! already commented out before this wave) — deleted outright, same
//! precedent as Wave 18's `tmpfs_smoketest.c`.
//!
//! # On-disk ABI
//!
//! `kernel/inc/vfs/xv6fs/ondisk.h` (`struct superblock`/`dinode`/
//! `dirent`, `BSIZE`/`FSMAGIC`/`NDIRECT`/`ROOTINO`/`DIRSIZ`/...) is
//! shared with `mkfs` (the host tool that builds `fs.img`) and is **not
//! touched** by this wave. Every on-disk struct this port reads/writes
//! was originally the real bindgen layout of that exact header (via
//! `kernel/vfs/xv6fs/xv6fs_private.h`'s `#include`, itself pulled into
//! `wrapper.h`); since P3-4a the trio is NATIVE (`superblock.rs`'s
//! `OndiskSuperblock`, `inode.rs`'s `Dinode`/`Dirent` — see their loud
//! P3-4-scrutiny notes), pinned byte-exact to the header by hardcoded
//! asserts + a two-arch gcc probe — still no hand-rolled byte offsets,
//! no opaque stand-ins. Small
//! integer `#define`s (`BSIZE`, `ROOTINO`, `DIRSIZ`, the on-disk type
//! tags) are hand-copied as local `const`s below rather than plumbed
//! through bindgen's `allowlist_var` macro-constant-folding, matching
//! this crate's established per-file convention (`tmpfs`'s own
//! `NAME_MAX`/`TMPFS_HASH_BUCKETS`) — `IPB` (inodes-per-block) is the one
//! exception: it depends on `sizeof(struct dinode)`, so it is a `const`
//! computed from the real `dinode` type (native since P3-4a, reached via
//! its unchanged `crate::bindings` facade path) via `core::mem::size_of`,
//! not a hand-copied number, so it tracks the struct layout automatically.
//!
//! # Wave B free-fn -> associated-fn sweep
//!
//! The mode/type scalar helpers (`s_isdir`/`s_isreg`/`s_islnk`/`s_ischr`/
//! `s_isblk`/`xv6fs_type_to_mode`/`xv6fs_mode_to_type`/`xv6fs_iblock`)
//! are now `impl Xv6fs` associated fns (dropping the `xv6fs_` prefix on
//! the last three), reached by every sibling submodule as
//! `super::Xv6fs::*`. `Xv6fs` here is a ZST at this file's own path
//! (`crate::vfs::xv6fs::Xv6fs`) -- distinct from the per-submodule
//! `Xv6fs` ZSTs `truncate.rs`/`log.rs`/`block_cache.rs`/`inode.rs`/
//! `superblock.rs` each declare privately for their own small helpers
//! (module-private, so same name / different type, no collision). Every
//! relocated body is byte-identical to its old free-fn form.
pub mod block_cache;
pub mod file;
pub mod inode;
pub mod log;
pub mod superblock;
pub mod truncate;

// ===========================================================================
// Shared constants — mirrors `xv6fs_private.h`'s `#define`s and, via it,
// `vfs/xv6fs/ondisk.h`'s. Centralized here rather than duplicated per
// submodule, matching `tmpfs/mod.rs`'s own precedent for this crate's
// filesystem-driver modules.
// ===========================================================================

/// `BSIZE` (`ondisk.h`) / `XV6FS_BSIZE` (`xv6fs_private.h`) — xv6fs block
/// size in bytes.
pub(crate) const BSIZE: u32 = 1024;
/// `ROOTINO` (`ondisk.h`) — the root directory is always inode 1.
pub(crate) const ROOTINO: u64 = 1;
/// `DIRSIZ` (`ondisk.h`) — max bytes of a directory entry name (no NUL
/// terminator stored on disk if the name fills the whole field).
pub(crate) const DIRSIZ: usize = 14;
/// `IPB` (`ondisk.h`, `BSIZE / sizeof(struct dinode)`) — inodes per disk
/// block. Computed from the real `dinode` layout (native since P3-4a,
/// unchanged facade path; not hand-copied) so it tracks the struct
/// automatically; matches classic xv6's value of 16 for the unchanged
/// on-disk format.
pub(crate) const IPB: u64 = BSIZE as u64 / core::mem::size_of::<crate::bindings::dinode>() as u64;
/// `FSMAGIC` (`ondisk.h`) — required value of `struct superblock.magic`.
pub(crate) const FSMAGIC: u32 = 0x1020_3040;

/// `NDIRECT` (`ondisk.h`) — number of direct block pointers in a `dinode`.
pub(crate) const NDIRECT: u32 = 11;
/// `NINDIRECT` (`ondisk.h`, `BSIZE / sizeof(uint)`) — block pointers per
/// singly-indirect block. `sizeof(uint) == 4`, so `1024 / 4 == 256`.
pub(crate) const NINDIRECT: u32 = BSIZE / core::mem::size_of::<u32>() as u32;
/// `NDINDIRECT` (`ondisk.h`, `NINDIRECT * NINDIRECT`) — block pointers
/// reachable through the doubly-indirect block.
pub(crate) const NDINDIRECT: u32 = NINDIRECT * NINDIRECT;
/// `MAXFILE` (`ondisk.h`) — maximum file size in blocks
/// (`NDIRECT + NINDIRECT + NDINDIRECT`).
pub(crate) const MAXFILE: u32 = NDIRECT + NINDIRECT + NDINDIRECT;
/// `BPB` (`ondisk.h`, `BSIZE * 8`) — free-bitmap bits per disk block.
pub(crate) const BPB: u32 = BSIZE * 8;

/// `MAXOPBLOCKS` (`param.h`) — max number of distinct blocks any single
/// filesystem operation logs in one transaction.
pub(crate) const MAXOPBLOCKS: i32 = 80;
/// `XV6FS_LOGSIZE` (`xv6fs_private.h`, `LOGSIZE` in `param.h`,
/// `MAXOPBLOCKS * 3`) — max data blocks in the on-disk log.
pub(crate) const XV6FS_LOGSIZE: i32 = MAXOPBLOCKS * 3;

/// `XV6FS_T_DIR` (`xv6fs_private.h`) — on-disk `dinode.type_` tag: directory.
pub(crate) const XV6FS_T_DIR: i16 = 1;
/// `XV6FS_T_FILE` — on-disk `dinode.type_` tag: regular file.
pub(crate) const XV6FS_T_FILE: i16 = 2;
/// `XV6FS_T_CDEVICE` — on-disk `dinode.type_` tag: character device.
pub(crate) const XV6FS_T_CDEVICE: i16 = 3;
/// `XV6FS_T_SYMLINK` — on-disk `dinode.type_` tag: symbolic link.
pub(crate) const XV6FS_T_SYMLINK: i16 = 4;
/// `XV6FS_T_BLKDEVICE` — on-disk `dinode.type_` tag: block device.
pub(crate) const XV6FS_T_BLKDEVICE: i16 = 5;

/// `VFS_DENTRY_COOKIE_END` (`xv6fs_private.h`) — sentinel
/// `vfs_dentry.cookies` value meaning "end of directory" (used by
/// `dir_iter`).
pub(crate) const VFS_DENTRY_COOKIE_END: i64 = 0;
/// `VFS_DENTRY_COOKIE_PARENT` — sentinel `vfs_dentry.cookies` value
/// meaning "this dentry is the synthesized `..` entry".
pub(crate) const VFS_DENTRY_COOKIE_PARENT: i64 = 2;

// ===========================================================================
// `uabi/stat.h`'s `S_IF*`/`S_IS*` macros — hardcoded locally, same
// rationale/precedent as `tmpfs/mod.rs`'s own copy (this crate's
// established per-file convention keeps small C-ABI constant sets
// self-contained rather than centralizing them behind bindgen).
// ===========================================================================
pub(crate) const S_IFMT: u32 = 0o170000;
pub(crate) const S_IFCHR: u32 = 0o020000;
pub(crate) const S_IFDIR: u32 = 0o040000;
pub(crate) const S_IFBLK: u32 = 0o060000;
pub(crate) const S_IFREG: u32 = 0o100000;
pub(crate) const S_IFLNK: u32 = 0o120000;

/// ZST marker (Wave B free-fn -> associated-fn sweep, this file only) --
/// holds the mode/type scalar helpers with no natural shared-type home
/// (they take/return bare `u32`/`i16`, not a pointer to any xv6fs
/// struct). Distinct from the per-submodule `Xv6fs` ZSTs in
/// `truncate.rs`/`log.rs`/`block_cache.rs`/`inode.rs`/`superblock.rs`
/// (each is module-private, so the same name in different files is a
/// different type -- no collision); this one lives at the crate path
/// `crate::vfs::xv6fs::Xv6fs`, reached by every sibling submodule as
/// `super::Xv6fs::*`.
pub(crate) struct Xv6fs;

impl Xv6fs {
    #[inline(always)]
    pub(crate) fn s_isdir(mode: u32) -> bool {
        mode & S_IFMT == S_IFDIR
    }
    #[inline(always)]
    pub(crate) fn s_isreg(mode: u32) -> bool {
        mode & S_IFMT == S_IFREG
    }
    #[inline(always)]
    pub(crate) fn s_islnk(mode: u32) -> bool {
        mode & S_IFMT == S_IFLNK
    }
    #[inline(always)]
    pub(crate) fn s_ischr(mode: u32) -> bool {
        mode & S_IFMT == S_IFCHR
    }
    #[inline(always)]
    pub(crate) fn s_isblk(mode: u32) -> bool {
        mode & S_IFMT == S_IFBLK
    }

    /// Mirrors `xv6fs_type_to_mode()` (`xv6fs_private.h`, `static inline`).
    #[inline(always)]
    pub(crate) fn type_to_mode(type_: i16) -> u32 {
        match type_ {
            XV6FS_T_DIR => S_IFDIR | 0o755,
            XV6FS_T_FILE => S_IFREG | 0o644,
            XV6FS_T_CDEVICE => S_IFCHR | 0o666,
            XV6FS_T_BLKDEVICE => S_IFBLK | 0o660,
            XV6FS_T_SYMLINK => S_IFLNK | 0o777,
            _ => 0,
        }
    }

    /// Mirrors `xv6fs_mode_to_type()` (`xv6fs_private.h`, `static inline`).
    #[inline(always)]
    pub(crate) fn mode_to_type(mode: u32) -> i16 {
        if Xv6fs::s_isdir(mode) {
            return XV6FS_T_DIR;
        }
        if Xv6fs::s_isreg(mode) {
            return XV6FS_T_FILE;
        }
        if Xv6fs::s_ischr(mode) {
            return XV6FS_T_CDEVICE;
        }
        if Xv6fs::s_isblk(mode) {
            return XV6FS_T_BLKDEVICE;
        }
        if Xv6fs::s_islnk(mode) {
            return XV6FS_T_SYMLINK;
        }
        0
    }

    /// `XV6FS_IBLOCK(ino, sb)` (`xv6fs_private.h`) — the disk block number
    /// containing inode `ino`'s `struct dinode`.
    ///
    /// `inodestart` is `uint` (`u32`) on the real `superblock` type;
    /// widened to `u64` before the divide/add to match the C macro's
    /// (unsigned, but effectively infinite-precision for realistic disk
    /// sizes) arithmetic without a wraparound hazard.
    #[inline(always)]
    pub(crate) fn iblock(ino: u64, inodestart: u32) -> u32 {
        ((ino / IPB) + inodestart as u64) as u32
    }
}
