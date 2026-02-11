/*
 * ondisk.h - xv6 On-disk File System Format
 *
 * This header defines the on-disk structures for the xv6 file system.
 * It is used by:
 *   - kernel/vfs/xv6fs/ (VFS xv6fs driver)
 *   - mkfs/mkfs.c (filesystem image builder)
 *   - kernel/bio.c (block I/O layer, uses BSIZE)
 *   - user programs (for DIRSIZ)
 *
 * Note: This is NOT the legacy in-memory fs interface (which has been
 * removed). All runtime file operations now go through VFS.
 * These are purely on-disk format definitions.
 */
#ifndef KERNEL_VFS_XV6FS_ONDISK_H
#define KERNEL_VFS_XV6FS_ONDISK_H

#ifndef ON_HOST_OS
#include "compiler.h"
#endif

/*
 * Block size for xv6 file system.
 * Also used by bio.c for block I/O operations.
 */
#define BSIZE 1024

/*
 * Root inode number - always inode 1.
 */
#define ROOTINO 1

/*
 * Disk layout:
 * [ boot block | super block | log | inode blocks |
 *                                   free bit map | data blocks ]
 *
 * mkfs computes the super block and builds an initial file system.
 * The super block describes the disk layout:
 */
struct superblock {
    uint magic;      // Must be FSMAGIC
    uint size;       // Size of file system image (blocks)
    uint nblocks;    // Number of data blocks
    uint ninodes;    // Number of inodes
    uint nlog;       // Number of log blocks
    uint logstart;   // Block number of first log block
    uint inodestart; // Block number of first inode block
    uint bmapstart;  // Block number of first free map block
};

#define FSMAGIC 0x10203040

/*
 * Block address layout in inode:
 * - NDIRECT direct block pointers
 * - 1 single indirect block pointer
 * - 1 double indirect block pointer
 */
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define NDINDIRECT (NINDIRECT * NINDIRECT)
#define MAXFILE (NDIRECT + NINDIRECT + NDINDIRECT)

/*
 * xv6 on-disk inode type constants.
 * These values are stored in dinode.type on disk.
 * For host tools (mkfs, xv6fuse), use XV6_T_* prefix to avoid
 * conflicts with host system's stat.h definitions.
 */
#define XV6_T_DIR 1     // Directory
#define XV6_T_FILE 2    // Regular file
#define XV6_T_DEVICE 3  // Device (character or block)
#define XV6_T_SYMLINK 4 // Symbolic link

/*
 * On-disk inode structure.
 * The type field indicates file type (XV6_T_DIR, XV6_T_FILE, etc.)
 */
struct dinode {
    short type;              // File type
    short major;             // Major device number (XV6_T_DEVICE only)
    short minor;             // Minor device number (XV6_T_DEVICE only)
    short nlink;             // Number of links to inode in file system
    uint size;               // Size of file (bytes)
    uint addrs[NDIRECT + 2]; // Data block addresses
};

// Inodes per block
#define IPB (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

/*
 * Directory entry structure.
 * A directory is a file containing a sequence of dirent structures.
 */
#define DIRSIZ 14

struct dirent {
    ushort inum;
    char name[DIRSIZ];
};

#endif /* KERNEL_VFS_XV6FS_ONDISK_H */
