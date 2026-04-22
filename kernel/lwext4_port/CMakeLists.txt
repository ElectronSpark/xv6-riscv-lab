# ==============================================================================
# lwext4 kernel port — CMakeLists.txt
# ==============================================================================
# Builds lwext4 core library + xv6 ext4fs VFS driver as a kernel OBJECT
# library, following the same pattern as the lwIP port.
# ==============================================================================

set(LWEXT4_DIR ${CMAKE_SOURCE_DIR}/kernel/lwext4)
set(EXT4FS_PORT_DIR ${CMAKE_SOURCE_DIR}/kernel/lwext4_port)

# --------------------------------------------------------------------------
# lwext4 library sources (all .c files from src/)
# --------------------------------------------------------------------------

set(LWEXT4_LIB_SRCS
    ${LWEXT4_DIR}/src/ext4.c
    ${LWEXT4_DIR}/src/ext4_balloc.c
    ${LWEXT4_DIR}/src/ext4_bcache.c
    ${LWEXT4_DIR}/src/ext4_bitmap.c
    ${LWEXT4_DIR}/src/ext4_block_group.c
    ${LWEXT4_DIR}/src/ext4_blockdev.c
    ${LWEXT4_DIR}/src/ext4_crc32.c
    ${LWEXT4_DIR}/src/ext4_debug.c
    ${LWEXT4_DIR}/src/ext4_dir.c
    ${LWEXT4_DIR}/src/ext4_dir_idx.c
    ${LWEXT4_DIR}/src/ext4_extent.c
    ${LWEXT4_DIR}/src/ext4_fs.c
    ${LWEXT4_DIR}/src/ext4_hash.c
    ${LWEXT4_DIR}/src/ext4_ialloc.c
    ${LWEXT4_DIR}/src/ext4_inode.c
    ${LWEXT4_DIR}/src/ext4_journal.c
    ${LWEXT4_DIR}/src/ext4_mbr.c
    ${LWEXT4_DIR}/src/ext4_mkfs.c
    ${LWEXT4_DIR}/src/ext4_super.c
    ${LWEXT4_DIR}/src/ext4_trans.c
    ${LWEXT4_DIR}/src/ext4_xattr.c
)

# --------------------------------------------------------------------------
# ext4fs VFS driver (port layer) sources
# --------------------------------------------------------------------------

set(EXT4FS_PORT_SRCS
    ${EXT4FS_PORT_DIR}/ext4fs_blockdev.c
    ${EXT4FS_PORT_DIR}/ext4fs_superblock.c
    ${EXT4FS_PORT_DIR}/ext4fs_inode.c
    ${EXT4FS_PORT_DIR}/ext4fs_file.c
    ${EXT4FS_PORT_DIR}/ext4fs_xattr_stubs.c
)

# --------------------------------------------------------------------------
# Build as kernel object library
# --------------------------------------------------------------------------

add_library(ext4fs OBJECT ${LWEXT4_LIB_SRCS} ${EXT4FS_PORT_SRCS})

# Include paths (order matters):
#  1. compat/     — shim headers (stdio.h, stdlib.h, string.h, inttypes.h)
#                   to shadow system headers for lwext4 library code
#  2. port dir    — for generated/ext4_config.h and ext4fs_private.h
#  3. lwext4/include/ — lwext4's own headers
#  4. kernel/inc/ — xv6 kernel headers (types.h, defs.h, dev/*, vfs/*, etc.)
target_include_directories(ext4fs PRIVATE
    ${EXT4FS_PORT_DIR}/compat
    ${EXT4FS_PORT_DIR}
    ${LWEXT4_DIR}/include
)

# Tell lwext4's ext4_config.h to include our generated/ext4_config.h
# instead of using its built-in defaults.
target_compile_definitions(ext4fs PRIVATE
    CONFIG_USE_DEFAULT_CFG=0
)

# Suppress warnings in lwext4 library code that would fail with -Werror
target_compile_options(ext4fs PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-type-limits
    -Wno-maybe-uninitialized
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    -Wno-pointer-to-int-cast
    -Wno-int-to-pointer-cast
    -Wno-old-style-definition
    -Wno-strict-prototypes
    -Wno-error    # lwext4 library code has many warnings; don't promote to errors
)
