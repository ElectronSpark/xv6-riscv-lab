/**
 * @file mkfs_xv6fs.c
 * @brief mkfs.xv6fs - format a device or file with xv6 filesystem
 *
 * Usage: mkfs_xv6fs <device> [size_in_blocks] [ninodes]
 *
 * Creates an empty xv6 filesystem on the specified block device or file.
 * If size_in_blocks is not specified, attempts to determine device size
 * via lseek. Default ninodes is 200.
 *
 * Disk layout:
 *   [ boot block | super block | log | inode blocks | bitmap | data blocks ]
 */

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

/* ---- On-disk format constants (from ondisk.h) ---- */
#define BSIZE 1024
#define ROOTINO 1
#define FSMAGIC 0x10203040

#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

#define XV6_T_DIR 1

struct superblock {
    uint magic;
    uint size;
    uint nblocks;
    uint ninodes;
    uint nlog;
    uint logstart;
    uint inodestart;
    uint bmapstart;
};

struct dinode {
    short type;
    short major;
    short minor;
    short nlink;
    uint size;
    uint addrs[NDIRECT + 2];
};

#define IPB (BSIZE / sizeof(struct dinode))
#define IBLOCK(i, sb) ((i) / IPB + (sb).inodestart)
#define BPB (BSIZE * 8)
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

#define DIRSIZ 14

struct xv6_dirent {
    ushort inum;
    char name[DIRSIZ];
};

/* ---- Constants ---- */
#define DEFAULT_NINODES 200
#define DEFAULT_LOGSIZE 240  /* MAXOPBLOCKS(80) * 3 */
#define MIN_FSSIZE 100       /* minimum reasonable fs size */

/* ---- Globals ---- */
static int fsfd;
static struct superblock sb;
static char zeroes[BSIZE];
static uint freeinode = 1;
static uint freeblock;

static int nbitmap;
static int ninodeblocks;
static int nlog;
static int nmeta;
static int nblocks;

/* ---- Byte-order helpers (little-endian on disk) ---- */
static ushort xshort(ushort x)
{
    ushort y;
    uchar *a = (uchar *)&y;
    a[0] = x;
    a[1] = x >> 8;
    return y;
}

static uint xint(uint x)
{
    uint y;
    uchar *a = (uchar *)&y;
    a[0] = x;
    a[1] = x >> 8;
    a[2] = x >> 16;
    a[3] = x >> 24;
    return y;
}

/* ---- Low-level I/O ---- */
static void wsect(uint sec, void *buf)
{
    if (lseek(fsfd, (int64)sec * BSIZE, SEEK_SET) < 0) {
        fprintf(2, "mkfs_xv6fs: lseek error at sector %d\n", sec);
        exit(1);
    }
    if (write(fsfd, buf, BSIZE) != BSIZE) {
        fprintf(2, "mkfs_xv6fs: write error at sector %d\n", sec);
        exit(1);
    }
}

static void rsect(uint sec, void *buf)
{
    if (lseek(fsfd, (int64)sec * BSIZE, SEEK_SET) < 0) {
        fprintf(2, "mkfs_xv6fs: lseek error at sector %d\n", sec);
        exit(1);
    }
    if (read(fsfd, buf, BSIZE) != BSIZE) {
        fprintf(2, "mkfs_xv6fs: read error at sector %d\n", sec);
        exit(1);
    }
}

/* ---- Inode I/O ---- */
static void winode(uint inum, struct dinode *ip)
{
    char buf[BSIZE];
    uint bn = IBLOCK(inum, sb);
    rsect(bn, buf);
    struct dinode *dip = ((struct dinode *)buf) + (inum % IPB);
    *dip = *ip;
    wsect(bn, buf);
}

static void rinode(uint inum, struct dinode *ip)
{
    char buf[BSIZE];
    uint bn = IBLOCK(inum, sb);
    rsect(bn, buf);
    struct dinode *dip = ((struct dinode *)buf) + (inum % IPB);
    *ip = *dip;
}

/* ---- Allocators ---- */
static uint ialloc(ushort type)
{
    uint inum = freeinode++;
    struct dinode din;
    memset(&din, 0, sizeof(din));
    din.type = xshort(type);
    din.nlink = xshort(1);
    din.size = xint(0);
    winode(inum, &din);
    return inum;
}

static void balloc(int used)
{
    uchar buf[BSIZE];
    printf("balloc: first %d blocks have been allocated\n", used);

    for (int b = 0; b < nbitmap; b++) {
        memset(buf, 0, BSIZE);
        int base = b * BPB;
        for (int i = base; i < base + BPB && i < used; i++) {
            buf[(i - base) / 8] |= (0x1 << ((i - base) % 8));
        }
        wsect(xint(sb.bmapstart) + b, buf);
    }
}

/* ---- iappend: append data to inode ---- */
static void iappend(uint inum, void *xp, int n)
{
    char *p = (char *)xp;
    uint fbn, off, n1;
    struct dinode din;
    char buf[BSIZE];
    uint indirect[NINDIRECT];
    uint x;

    rinode(inum, &din);
    off = xint(din.size);

    while (n > 0) {
        fbn = off / BSIZE;
        if (fbn >= MAXFILE) {
            fprintf(2, "mkfs_xv6fs: file too large\n");
            exit(1);
        }
        if (fbn < NDIRECT) {
            if (xint(din.addrs[fbn]) == 0) {
                din.addrs[fbn] = xint(freeblock++);
            }
            x = xint(din.addrs[fbn]);
        } else {
            if (xint(din.addrs[NDIRECT]) == 0) {
                din.addrs[NDIRECT] = xint(freeblock++);
            }
            rsect(xint(din.addrs[NDIRECT]), (char *)indirect);
            if (indirect[fbn - NDIRECT] == 0) {
                indirect[fbn - NDIRECT] = xint(freeblock++);
                wsect(xint(din.addrs[NDIRECT]), (char *)indirect);
            }
            x = xint(indirect[fbn - NDIRECT]);
        }
        n1 = (fbn + 1) * BSIZE - off;
        if ((uint)n < n1)
            n1 = n;
        rsect(x, buf);
        memmove(buf + off - (fbn * BSIZE), p, n1);
        wsect(x, buf);
        n -= n1;
        off += n1;
        p += n1;
    }
    din.size = xint(off);
    winode(inum, &din);
}

int main(int argc, char *argv[])
{
    uint fssize;
    uint user_ninodes = DEFAULT_NINODES;

    if (argc < 2) {
        fprintf(2, "Usage: mkfs_xv6fs <device> [size_in_blocks] [ninodes]\n");
        fprintf(2, "  device         block device or file to format\n");
        fprintf(2, "  size_in_blocks filesystem size in 1K blocks (default: auto)\n");
        fprintf(2, "  ninodes        number of inodes (default: %d)\n", DEFAULT_NINODES);
        exit(1);
    }

    /* Open device */
    fsfd = open(argv[1], O_RDWR);
    if (fsfd < 0) {
        fprintf(2, "mkfs_xv6fs: cannot open %s\n", argv[1]);
        exit(1);
    }

    /* Determine filesystem size */
    if (argc >= 3) {
        fssize = atoi(argv[2]);
        if (fssize < MIN_FSSIZE) {
            fprintf(2, "mkfs_xv6fs: size too small (min %d blocks)\n", MIN_FSSIZE);
            exit(1);
        }
    } else {
        /* Try to determine size from device via lseek */
        int64 end = lseek(fsfd, 0, SEEK_END);
        if (end <= 0) {
            fprintf(2, "mkfs_xv6fs: cannot determine device size, specify size_in_blocks\n");
            exit(1);
        }
        fssize = (uint)(end / BSIZE);
        if (fssize < MIN_FSSIZE) {
            fprintf(2, "mkfs_xv6fs: device too small (%d blocks, min %d)\n", fssize, MIN_FSSIZE);
            exit(1);
        }
        lseek(fsfd, 0, SEEK_SET);
    }

    if (argc >= 4) {
        user_ninodes = atoi(argv[3]);
        if (user_ninodes < 2) {
            fprintf(2, "mkfs_xv6fs: ninodes must be >= 2\n");
            exit(1);
        }
    }

    /* Compute layout */
    nlog = DEFAULT_LOGSIZE;
    ninodeblocks = user_ninodes / IPB + 1;
    nbitmap = fssize / BPB + 1;
    nmeta = 2 + nlog + ninodeblocks + nbitmap;
    nblocks = fssize - nmeta;

    if (nblocks < 1) {
        fprintf(2, "mkfs_xv6fs: not enough space for data blocks\n");
        exit(1);
    }

    /* Fill superblock */
    sb.magic = xint(FSMAGIC);
    sb.size = xint(fssize);
    sb.nblocks = xint(nblocks);
    sb.ninodes = xint(user_ninodes);
    sb.nlog = xint(nlog);
    sb.logstart = xint(2);
    sb.inodestart = xint(2 + nlog);
    sb.bmapstart = xint(2 + nlog + ninodeblocks);

    freeblock = nmeta;

    printf("mkfs_xv6fs: %s\n", argv[1]);
    printf("  total blocks: %d\n", fssize);
    printf("  meta blocks:  %d (boot=1, super=1, log=%d, inode=%d, bitmap=%d)\n",
           nmeta, nlog, ninodeblocks, nbitmap);
    printf("  data blocks:  %d\n", nblocks);
    printf("  inodes:       %d\n", user_ninodes);

    /* Zero all blocks */
    printf("  zeroing...\n");
    memset(zeroes, 0, BSIZE);
    for (uint i = 0; i < fssize; i++)
        wsect(i, zeroes);

    /* Write superblock at block 1 */
    {
        char buf[BSIZE];
        memset(buf, 0, sizeof(buf));
        memmove(buf, &sb, sizeof(sb));
        wsect(1, buf);
    }

    /* Create root inode (inum 1) */
    uint rootino = ialloc(XV6_T_DIR);
    if (rootino != ROOTINO) {
        fprintf(2, "mkfs_xv6fs: root inode is %d, expected %d\n", rootino, ROOTINO);
        exit(1);
    }

    /* Set root nlink to 2 (for "." and "..") */
    {
        struct dinode din;
        rinode(rootino, &din);
        din.nlink = xshort(2);
        winode(rootino, &din);
    }

    /* Add "." and ".." entries */
    {
        struct xv6_dirent de;

        memset(&de, 0, sizeof(de));
        de.inum = xshort(rootino);
        strcpy(de.name, ".");
        iappend(rootino, &de, sizeof(de));

        memset(&de, 0, sizeof(de));
        de.inum = xshort(rootino);
        strcpy(de.name, "..");
        iappend(rootino, &de, sizeof(de));
    }

    /* Round up root dir size to block boundary */
    {
        struct dinode din;
        rinode(rootino, &din);
        uint off = xint(din.size);
        off = ((off / BSIZE) + 1) * BSIZE;
        din.size = xint(off);
        winode(rootino, &din);
    }

    /* Mark used blocks in bitmap */
    balloc(freeblock);

    printf("  done.\n");

    close(fsfd);
    exit(0);
}
