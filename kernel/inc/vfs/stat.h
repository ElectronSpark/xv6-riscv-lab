#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H

#include "compiler.h"
#include "types.h"

struct vfs_stat {
  int32 dev;        // File system's disk device
  uint64 ino;       // Inode number
  mode_t mode;      // Permission and type bits
  uint32 nlink;     // Number of links to file
  uint64 size;      // Size of file in bytes
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

#define	S_IFMT      0170000	/* type of file */
#define S_IFDIR	    0040000	/* directory */
#define S_IFCHR	    0020000	/* character special */
#define S_IFBLK	    0060000	/* block special */
#define S_IFREG	    0100000	/* regular */
#define S_IFLNK	    0120000	/* symbolic link */
#define S_IFSOCK    0140000	/* socket */
#define S_IFIFO	    0010000	/* fifo */

#define	S_ISBLK(m)	(((m)&S_IFMT) == S_IFBLK)
#define	S_ISCHR(m)	(((m)&S_IFMT) == S_IFCHR)
#define	S_ISDIR(m)	(((m)&S_IFMT) == S_IFDIR)
#define	S_ISFIFO(m)	(((m)&S_IFMT) == S_IFIFO)
#define	S_ISREG(m)	(((m)&S_IFMT) == S_IFREG)
#define	S_ISLNK(m)	(((m)&S_IFMT) == S_IFLNK)
#define	S_ISSOCK(m)	(((m)&S_IFMT) == S_IFSOCK)

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
