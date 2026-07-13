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
//! The rest of `kernel/vfs/` (`xv6fs/`, `devtmpfs/`) remains C for now
//! (Waves 19-20; see `kernel/vfs/CMakeLists.txt`), so those filesystem
//! drivers are compiled and linked together into the same `vfs` object
//! library, exactly like `kernel/dev/` during its own partial-port
//! period.

pub mod fdtable;
pub mod file;
pub mod fs;
pub mod inode;
pub mod pipe;
pub mod tmpfs;
pub mod vfs_syscall;
pub mod xv6fs;
