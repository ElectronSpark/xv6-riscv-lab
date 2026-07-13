# NOTE: inode.c (Phase 2 Wave 13), file.c/pipe.c (Phase 2 Wave 14),
# fdtable.c (Phase 2 Wave 15), fs.c (Phase 2 Wave 16), and
# vfs_syscall.c (Phase 2 Wave 17) have all been rewritten in Rust
# (kernel/vfs/{inode,file,pipe,fdtable,fs,vfs_syscall}.rs). Their
# symbols are linked in via the xv6_rust staticlib (see
# kernel/CMakeLists.txt's RUST_CRATE_SOURCES / RUST_FORCE_UNDEFINED).
# kernel/vfs/ therefore has no top-level C sources left, so there is no
# `vfs_core` OBJECT library anymore (same precedent as lock/, timer/,
# ipi/, tty/, proc/).
#
# NOTE: kernel/vfs/tmpfs/ has no C sources left either (Phase 2 Wave 18
# -- superblock.c/inode.c/truncate.c/file.c rewritten in Rust, see
# kernel/vfs/tmpfs/*.rs; tmpfs_smoketest.c/.h deleted outright, dead
# code). Its `vfs_tmpfs` OBJECT library and add_subdirectory() are gone
# (same precedent as vfs core above).
# `kernel/vfs/tmpfs/tmpfs_private.h` is NOT deleted: it stays a real C
# header describing struct tmpfs_inode/tmpfs_superblock/tmpfs_dentry
# layouts and the tmpfs_alloc_inode/tmpfs_alloc_superblock/
# tmpfs_make_directory/tmpfs_free extern contract that
# kernel/vfs/devtmpfs/superblock.c (still C) `#include`s directly.
#
# NOTE: kernel/vfs/xv6fs/ has no C sources left either (Phase 2 Wave 19
# -- superblock.c/inode.c (sub-wave A) + truncate.c/file.c/log.c/
# block_cache.c (sub-wave B) all rewritten in Rust, see
# kernel/vfs/xv6fs/*.rs; xv6fs_smoketest.c/.h deleted outright, dead
# code). Its `vfs_xv6fs` OBJECT library and add_subdirectory() are gone
# (same precedent as vfs core / tmpfs above) -- only the devtmpfs
# filesystem driver subdirectory still builds C.
# `kernel/vfs/xv6fs/xv6fs_private.h`/`block_cache.h` are NOT deleted:
# they stay real C headers describing the on-disk-adjacent in-memory
# layouts (`struct xv6fs_superblock`/`xv6fs_inode`/`xv6fs_log`/
# `xv6fs_block_cache`/`free_extent`) that `kernel/wrapper.h` includes for
# bindgen (real field layouts, no opaque stand-ins -- see wrapper.h's own
# comment). `vfs/xv6fs/ondisk.h` (shared with mkfs) is untouched.

# Pull in filesystem implementations
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/devtmpfs)

# Aggregate VFS objects into a single object library for the parent.
add_library(vfs OBJECT
    $<TARGET_OBJECTS:vfs_devtmpfs>
)
