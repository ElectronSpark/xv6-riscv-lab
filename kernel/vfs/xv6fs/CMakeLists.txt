add_library(vfs_xv6fs OBJECT
    superblock.c
    inode.c
    file.c
    log.c
    xv6fs_smoketest.c
)
