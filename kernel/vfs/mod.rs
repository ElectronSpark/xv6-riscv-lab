//! Rust port of the kernel VFS layer. `vfs/inode.c` -> [`inode`] (Phase 2
//! Wave 13): the inode cache/refcount/locking core and path-resolution
//! (`vfs_namei`) machinery. `vfs/file.c` -> [`file`] and `vfs/pipe.c` ->
//! [`pipe`] (Phase 2 Wave 14): the `vfs_file` lifecycle/dispatch and the
//! pipe ring buffer. `vfs/fdtable.c` -> [`fdtable`] (Phase 2 Wave 15):
//! the per-thread integer-fd -> `vfs_file*` table.
//!
//! `vfs/fs.c` -> [`fs`] (Phase 2 Wave 16): the filesystem-type registry,
//! mount/unmount state machine, superblock lock/refcount/mountcount API,
//! per-superblock inode hash cache, dentry-to-inode resolution, and
//! `fs_struct` lifecycle.
//!
//! `vfs/vfs_syscall.c` -> [`vfs_syscall`] (Phase 2 Wave 17): every
//! filesystem-facing syscall (`open`/`read`/`write`/`stat`/`mkdir`/
//! `mount`/`ioctl`/`poll`/...), completing the `vfs` core port.
//!
//! `vfs/tmpfs/*.c` -> [`tmpfs`] (Phase 2 Wave 18): the first filesystem
//! *driver* (as opposed to vfs core) to move to Rust -- see that
//! module's own doc comment for the sub-wave breakdown.
//!
//! `vfs/xv6fs/*.c` -> [`xv6fs`] (Phase 2 Wave 19): the root filesystem.
//!
//! `vfs/devtmpfs/superblock.c` -> [`devtmpfs`] (Phase 2 Wave 20): the
//! `/dev` device-node filesystem, reusing `tmpfs`'s inode/dentry
//! infrastructure via the `tmpfs_private.h` C-ABI contract (see
//! `devtmpfs::superblock`'s module doc). This was the **last C file in
//! `kernel/vfs/`** -- as of this wave, the entire vfs tree (core, tmpfs,
//! xv6fs, devtmpfs) is 100% Rust; there is no more `kernel/vfs/
//! CMakeLists.txt` / `vfs` OBJECT library (same precedent as lock/,
//! timer/, ipi/, tty/, proc/).

pub mod devtmpfs;
pub mod fdtable;
pub mod file;
pub mod fs;
pub mod inode;
pub mod pipe;
pub mod tmpfs;
pub mod vfs_syscall;
pub mod xv6fs;
