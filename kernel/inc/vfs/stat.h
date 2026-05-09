#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H

#ifdef HOST_LIBC_PROGRAM
#include <sys/stat.h>
#else

#include "compiler.h"
#include "types.h"

/*
 * Linux-compatible stat structure.
 *
 * The binary layout must match what musl expects for the target architecture
 * so stat/fstat/fstatat buffers can be copied out directly to userspace.
 */
struct stat {
#if defined(CONFIG_ARCH_X86_64)
    uint64 st_dev;
    uint64 st_ino;
    uint64 st_nlink;
    uint32 st_mode;
    uint32 st_uid;
    uint32 st_gid;
    uint32 __pad0;
    uint64 st_rdev;
    int64  st_size;
    int64  st_blksize;
    int64  st_blocks;
    int64  st_atime_sec;
    int64  st_atime_nsec;
    int64  st_mtime_sec;
    int64  st_mtime_nsec;
    int64  st_ctime_sec;
    int64  st_ctime_nsec;
    int64  __unused[3];
#elif defined(CONFIG_ARCH_RISCV)
    uint64 st_dev;
    uint64 st_ino;
    uint32 st_mode;
    uint32 st_nlink;
    uint32 st_uid;
    uint32 st_gid;
    uint64 st_rdev;
    uint64 __pad;
    int64  st_size;
    int32  st_blksize;
    int32  __pad2;
    int64  st_blocks;
    int64  st_atime_sec;
    int64  st_atime_nsec;
    int64  st_mtime_sec;
    int64  st_mtime_nsec;
    int64  st_ctime_sec;
    int64  st_ctime_nsec;
    uint32 __unused[2];
#else
#error "Unsupported architecture for struct stat"
#endif
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

#endif /* HOST_LIBC_PROGRAM */

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
