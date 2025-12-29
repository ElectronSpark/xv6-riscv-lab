add_library(vfs_core OBJECT
    fs.c
    inode.c
    file.c
    fdtable.c
    vfs_syscall.c
)

# Pull in filesystem implementations
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tmpfs)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/xv6fs)

# Aggregate VFS objects into a single object library for the parent.
add_library(vfs OBJECT
    $<TARGET_OBJECTS:vfs_core>
    $<TARGET_OBJECTS:vfs_tmpfs>
    $<TARGET_OBJECTS:vfs_xv6fs>
)
