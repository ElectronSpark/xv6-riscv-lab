#ifndef __KERNEL_FILE_H
#define __KERNEL_FILE_H

#include "compiler.h"
#include "slab_type.h"
#include "hlist_type.h"
#include "fs.h"

#ifndef __KERNEL_FILE_TYPES_H
#define __KERNEL_FILE_TYPES_H
 enum file_type {
  FD_NONE = (int)0,
  FD_PIPE,
  FD_INODE,
  FD_DEVICE,
  FD_SOCK
};
#endif // __KERNEL_FILE_TYPES_H

struct xv6_file {
  enum file_type type;
  int ref; // reference count
  char readable: 1;
  char writable: 1;
  union {
    struct pipe *pipe; // FD_PIPE
    struct xv6_inode *ip;  // FD_INODE and FD_DEVICE
    struct sock *sock; // FD_SOCK
  };
  uint off;          // FD_INODE
  short major;       // FD_DEVICE
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define	mkdev(m,n)  ((dev_t)((m)<<16| (n)))

// in-memory copy of an inode
struct xv6_inode {
  dev_t dev;          // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  hlist_entry_t hlist_entry; // for inode hash list
  int valid;          // inode has been read from disk?

  // cache of inode in disk
  struct xv6_dinode dinode; // contents of disk inode
};

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

#define CONSOLE 1

#define SYSFILE_SYM_LOOKUP_MAX_COUNT 10

#endif        /* __KERNEL_FILE_H */
