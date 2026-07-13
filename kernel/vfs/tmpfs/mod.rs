//! tmpfs — Rust port of `kernel/vfs/tmpfs/{superblock,inode,truncate,file}.c`
//! (Phase 2 Wave 18, see `docs/rustify/phase2_plan.md`).
//!
//! tmpfs is the first *filesystem driver* (as opposed to vfs core, which
//! finished in Waves 13-17) to move to Rust. It fills the C-ABI
//! `vfs_inode_ops`/`vfs_file_ops`/`vfs_superblock_ops`/`vfs_fs_type_ops`
//! vtables that vfs core (100% Rust since Wave 17) dispatches through via
//! `extern "C"` function pointers — every op fn in this module keeps the
//! exact C signature bindgen derived from `vfs/vfs_types.h`, and the
//! vtable statics themselves move from C designated initializers to
//! plain Rust `static` values (see [`inode::TMPFS_INODE_OPS`],
//! [`file::TMPFS_FILE_OPS`], [`superblock::TMPFS_SUPERBLOCK_OPS`]).
//!
//! `kernel/vfs/tmpfs/tmpfs_private.h` is deliberately **not** deleted or
//! moved: `kernel/vfs/devtmpfs/superblock.c` (still C, Wave 20) `#include`s
//! it directly to reuse tmpfs's inode/superblock allocation and directory
//! helpers via `container_of`. The header keeps describing the exact same
//! C struct layouts (`struct tmpfs_inode`, `struct tmpfs_superblock`,
//! `struct tmpfs_sb_private`, `struct tmpfs_dentry`) and the same
//! `extern` function list; this port implements every symbol the header
//! declares as a real `#[no_mangle] pub extern "C" fn` (or `pub static`
//! for the three ops-vtable globals), even the few devtmpfs's *current*
//! `superblock.c` snapshot doesn't happen to call, so that header remains
//! an honest contract for whatever Wave 20 ends up needing. Bindgen
//! generates the real field layouts for these types from the same header
//! (`kernel/wrapper.h`'s `vfs/tmpfs/tmpfs_private.h` include, Wave 18) —
//! no opaque stand-ins, exactly like every other vfs-core wave.
//!
//! # Sub-wave split (matches the C file split)
//!
//! * [`superblock`] <- `superblock.c`: slab caches, inode/superblock
//!   alloc+free, the `tmpfs`/root-mount filesystem-type registration, and
//!   `vfs_superblock_ops` (`alloc_inode`/`get_inode`/`sync_fs`/
//!   `unmount_begin`/`statfs`).
//! * [`inode`] <- `inode.c`: the directory hash-list (dentry alloc/link/
//!   unlink, name hashing/comparison), every `vfs_inode_ops` callback
//!   (`lookup`/`dir_iter`/`readlink`/`create`/`link`/`unlink`/`mkdir`/
//!   `rmdir`/`mknod`/`move`/`symlink`/`destroy_inode`/`getattr`/
//!   `setattr`), and `vfs_inode_ops` itself.
//! * [`truncate`] <- `truncate.c`: grow/shrink a regular file (embedded
//!   <-> pcache migration, pcache page discard on shrink).
//! * [`file`] <- `file.c`: the per-inode pcache wiring
//!   (`tmpfs_inode_pcache_init`/`_teardown`, `tmpfs_pcache_ops`), every
//!   `vfs_file_ops` callback (`read`/`write`/`llseek`/`fault`), `tmpfs_open`,
//!   and `vfs_file_ops` itself.
//!
//! `tmpfs_smoketest.c`/`.h` were **not** ported: dead code (every call
//! site was already commented out before this wave — see Wave 16's
//! report and `docs/rustify/phase2_plan.md` \S0) — deleted outright, per
//! `docs/rustify/test_port_plan.md`.
//!
//! # Layout fidelity
//!
//! `sizeof(struct tmpfs_inode)` and `TMPFS_INODE_EMBEDDED_DATA_LEN` are
//! computed at runtime in [`superblock::tmpfs_init`] via
//! `core::mem::size_of`/`core::mem::offset_of!` against the real bindgen
//! type (not hand-copied constants), so the boot-log line this module
//! prints is byte-for-byte derived from the same struct layout the C
//! compiler would have produced for the unmodified header.
pub mod file;
pub mod inode;
pub mod superblock;
pub mod truncate;

// ===========================================================================
// Shared constants — mirrors the `#define`s at the top of
// `tmpfs_private.h`. Centralized here (rather than duplicated per
// submodule) since, unlike this crate's cross-C-TU externs convention,
// these four files are genuine Rust siblings of one logical unit.
// ===========================================================================

/// `VFS_DENTRY_COOKIE_END` — sentinel `vfs_dentry.cookies` value meaning
/// "end of directory" (used by `dir_iter`).
pub(crate) const VFS_DENTRY_COOKIE_END: i64 = 0;
/// `VFS_DENTRY_COOKIE_SELF` — declared in the C header; unused by any
/// tmpfs `.c` file (verified by inspection), kept for header fidelity.
#[allow(dead_code)]
pub(crate) const VFS_DENTRY_COOKIE_SELF: i64 = 1;
/// `VFS_DENTRY_COOKIE_PARENT` — sentinel `vfs_dentry.cookies` value
/// meaning "this dentry is the synthesized `..` entry".
pub(crate) const VFS_DENTRY_COOKIE_PARENT: i64 = 2;

/// `TMPFS_HASH_BUCKETS` — bucket count for every tmpfs directory's
/// child hash list.
pub(crate) const TMPFS_HASH_BUCKETS: u64 = 15;

/// `TMPFS_MAX_FILE_SIZE` — maximum file size supported by tmpfs (1 GB).
/// All non-embedded file data is stored in the per-inode pcache
/// (`i_data`), which allocates pages on demand.
pub(crate) const TMPFS_MAX_FILE_SIZE: u64 = 1 * 1024 * 1024 * 1024;

/// `NAME_MAX` (`uabi/linux_dirent64.h`) — hardcoded locally rather than
/// pulled through bindgen (a plain, untyped `#define`, and the header
/// defining it is not otherwise needed by wrapper.h), matching this
/// crate's convention of hand-copying small integer constants (e.g.
/// `vfs/inode.rs`'s `S_IFMT`/`S_IFDIR` family).
pub(crate) const NAME_MAX: u64 = 255;

// ===========================================================================
// `uabi/stat.h`'s `S_IF*`/`S_IS*` macros — hardcoded locally (same
// rationale/precedent as `vfs/inode.rs`'s own copy: this crate's
// established per-file convention keeps small C-ABI constant sets
// self-contained rather than centralizing them behind bindgen). Shared
// here at the `tmpfs` module level (rather than duplicated per
// submodule) since `inode.rs`, `file.rs`, and `truncate.rs` all need
// several of them.
// ===========================================================================
pub(crate) const S_IFMT: u32 = 0o170000;
pub(crate) const S_IFIFO: u32 = 0o010000;
pub(crate) const S_IFCHR: u32 = 0o020000;
pub(crate) const S_IFDIR: u32 = 0o040000;
pub(crate) const S_IFBLK: u32 = 0o060000;
pub(crate) const S_IFREG: u32 = 0o100000;
pub(crate) const S_IFLNK: u32 = 0o120000;

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
#[inline(always)]
pub(crate) fn s_isfifo(mode: u32) -> bool {
    mode & S_IFMT == S_IFIFO
}
