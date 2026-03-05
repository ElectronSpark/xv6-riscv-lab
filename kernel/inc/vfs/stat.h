#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H

#include "compiler.h"
#include "types.h"

/*
 * POSIX-style stat structure — musl riscv64 ABI compatible
 *
 * This layout MUST match musl's arch/riscv64/bits/stat.h exactly so that
 * stat/fstat/fstatat syscalls can copyout this struct directly to userspace.
 *
 * Fields the kernel does not populate are zeroed via memset() before filling.
 */
struct stat {
    uint64 st_dev;           // device ID
    uint64 st_ino;           // inode number
    uint32 st_mode;          // permission and type bits (S_IS* macros)
    uint32 st_nlink;         // number of hard links
    uint32 st_uid;           // owner user ID
    uint32 st_gid;           // owner group ID
    uint64 st_rdev;          // device ID (for special files)
    uint64 __pad;            // padding (matches musl layout)
    int64  st_size;          // size in bytes
    int32  st_blksize;       // preferred I/O block size
    int32  __pad2;           // padding
    int64  st_blocks;        // number of 512-byte blocks allocated
    int64  st_atime_sec;     // last access time (seconds)
    int64  st_atime_nsec;    // last access time (nanoseconds)
    int64  st_mtime_sec;     // last modification time (seconds)
    int64  st_mtime_nsec;    // last modification time (nanoseconds)
    int64  st_ctime_sec;     // last status change time (seconds)
    int64  st_ctime_nsec;    // last status change time (nanoseconds)
    uint32 __unused[2];      // reserved
};

#ifndef S_IRUSR
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#endif

#ifndef S_ISUID
#define S_ISUID 04000  /* Set-user-ID on execution */
#define S_ISGID 02000  /* Set-group-ID on execution */
#define S_ISVTX 01000  /* Sticky bit */
#endif

#define S_IFMT 0170000   /* type of file */
#define S_IFDIR 0040000  /* directory */
#define S_IFCHR 0020000  /* character special */
#define S_IFBLK 0060000  /* block special */
#define S_IFREG 0100000  /* regular */
#define S_IFLNK 0120000  /* symbolic link */
#define S_IFSOCK 0140000 /* socket */
#define S_IFIFO 0010000  /* fifo */

#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
