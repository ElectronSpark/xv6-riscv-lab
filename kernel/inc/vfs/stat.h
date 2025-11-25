#ifndef KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H

#include "compiler.h"
#include "types.h"

typedef enum {
    VFS_I_TYPE_NONE = 0,
    VFS_I_TYPE_PIPE,
    VFS_I_TYPE_DIR,
    VFS_I_TYPE_MNT,
    VFS_I_TYPE_ROOT,
    VFS_I_TYPE_FILE,
    VFS_I_TYPE_SYMLINK,
    VFS_I_TYPE_CDEV,
    VFS_I_TYPE_BDEV,
    VFS_I_TYPE_SOCK
} vfs_inode_type_t;

struct vfs_stat {
  int32 dev;        // File system's disk device
  uint64 ino;       // Inode number
  mode_t mode;      // Permission and type bits
  uint32 nlink;     // Number of links to file
  uint64 size;      // Size of file in bytes
};

#define	S_IFMT      0170000	/* type of file */
#define S_IFDIR	    0040000	/* directory */
#define S_IFCHR	    0020000	/* character special */
#define S_IFBLK	    0060000	/* block special */
#define S_IFREG	    0100000	/* regular */
#define S_IFLNK	    0120000	/* symbolic link */
#define S_IFSOCK    0140000	/* socket */
#define S_IFIFO	    0010000	/* fifo */

static inline mode_t vfs_inode_type_to_mode(vfs_inode_type_t type) {
    switch(type) {
        case VFS_I_TYPE_DIR:
        case VFS_I_TYPE_MNT:
        case VFS_I_TYPE_ROOT:
            return S_IFDIR;
        case VFS_I_TYPE_CDEV:
            return S_IFCHR;
        case VFS_I_TYPE_BDEV:
            return S_IFBLK;
        case VFS_I_TYPE_FILE:
            return S_IFREG;
        case VFS_I_TYPE_SYMLINK:
            return S_IFLNK;
        case VFS_I_TYPE_SOCK:
            return S_IFSOCK;
        case VFS_I_TYPE_PIPE:
            return S_IFIFO;
        default:
            return 0;
    }
}

#define	S_ISBLK(m)	(((m)&S_IFMT) == S_IFBLK)
#define	S_ISCHR(m)	(((m)&S_IFMT) == S_IFCHR)
#define	S_ISDIR(m)	(((m)&S_IFMT) == S_IFDIR)
#define	S_ISFIFO(m)	(((m)&S_IFMT) == S_IFIFO)
#define	S_ISREG(m)	(((m)&S_IFMT) == S_IFREG)
#define	S_ISLNK(m)	(((m)&S_IFMT) == S_IFLNK)
#define	S_ISSOCK(m)	(((m)&S_IFMT) == S_IFSOCK)

#endif // KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
