#ifndef __USER_ABI_LINUX_DIRENT64_H
#define __USER_ABI_LINUX_DIRENT64_H

#include "types.h"

// Linux-compatible dirent structure for getdents (used by ls)
struct linux_dirent64 {
    uint64 d_ino;    // Inode number
    int64 d_off;     // Offset to next structure
    uint16 d_reclen; // Size of this dirent
    uint8 d_type;    // File type
    char d_name[];   // Filename (null-terminated)
};

#define NAME_MAX 255

// File type constants
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

#endif // __USER_ABI_LINUX_DIRENT64_H
