/*
 * bits/stat.h — xv6 stat structure for musl
 *
 * IMPORTANT: This must match EXACTLY what the xv6 kernel puts into the
 * stat buffer via sys_fstat/sys_stat/sys_fstatat. The kernel's struct stat
 * is very compact (5 fields). musl expects a Linux-like struct stat.
 *
 * We define musl's struct stat to match xv6's layout, and provide conversion
 * shims where needed.
 */

/*
 * xv6 kernel struct stat layout (from kernel/inc/vfs/stat.h):
 *   int32  dev;      // offset 0,  size 4
 *   uint64 ino;      // offset 8,  size 8  (padding at offset 4)
 *   uint32 mode;     // offset 16, size 4
 *   uint32 nlink;    // offset 20, size 4
 *   uint64 size;     // offset 24, size 8
 *   // total: 32 bytes
 *
 * We define struct stat to match this layout.
 * Missing fields (uid, gid, times, etc.) are not provided by xv6.
 */
struct stat {
    int           st_dev;       /* offset 0 */
    unsigned long st_ino;       /* offset 8 */
    unsigned int  st_mode;      /* offset 16 */
    unsigned int  st_nlink;     /* offset 20 */
    unsigned long st_size;      /* offset 24 */
};

/* S_IF* type flags — must match xv6 kernel */
#define S_IFMT    0170000
#define S_IFSOCK  0140000
#define S_IFLNK   0120000
#define S_IFREG   0100000
#define S_IFBLK   0060000
#define S_IFDIR   0040000
#define S_IFCHR   0020000
#define S_IFIFO   0010000

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Permission bits */
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001
#define S_IRWXU  (S_IRUSR | S_IWUSR | S_IXUSR)
#define S_IRWXG  (S_IRGRP | S_IWGRP | S_IXGRP)
#define S_IRWXO  (S_IROTH | S_IWOTH | S_IXOTH)
