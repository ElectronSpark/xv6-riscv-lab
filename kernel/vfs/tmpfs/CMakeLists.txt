add_library(vfs_tmpfs OBJECT
    superblock.c
    inode.c
    truncate.c
    file.c
    tmpfs_smoketest.c
)
