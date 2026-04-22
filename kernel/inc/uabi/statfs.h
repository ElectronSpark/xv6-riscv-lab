#ifndef __USER_ABI_STATFS_H
#define __USER_ABI_STATFS_H

#include "types.h"

struct statfs {
    uint64 f_type;    /* Type of filesystem */
    uint64 f_bsize;   /* Optimal transfer block size */
    uint64 f_blocks;  /* Total data blocks in filesystem */
    uint64 f_bfree;   /* Free blocks in filesystem */
    uint64 f_bavail;  /* Free blocks available to unprivileged user */
    uint64 f_files;   /* Total inodes in filesystem */
    uint64 f_ffree;   /* Free inodes in filesystem */
    uint64 f_namelen; /* Maximum length of filenames */
    uint64 f_frsize;  /* Fragment size */
};

#endif /* __USER_ABI_STATFS_H */