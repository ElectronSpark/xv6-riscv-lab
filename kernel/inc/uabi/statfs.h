#ifndef __USER_ABI_STATFS_H
#define __USER_ABI_STATFS_H

#include "types.h"

struct statfs {
    uint64 f_type;
    uint64 f_bsize;
    uint64 f_blocks;
    uint64 f_bfree;
    uint64 f_bavail;
    uint64 f_files;
    uint64 f_ffree;
    struct {
        int val[2];
    } f_fsid;
    uint64 f_namelen;
    uint64 f_frsize;
    uint64 f_flags;
    uint64 f_spare[4];
};

#endif /* __USER_ABI_STATFS_H */
