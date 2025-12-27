add_library(xv6fs OBJECT
    superblock.c
    inode.c
    file.c
    log.c
    xv6fs_smoketest.c
)

target_include_directories(xv6fs PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/inc
    ${CMAKE_SOURCE_DIR}/kernel/vfs
)
