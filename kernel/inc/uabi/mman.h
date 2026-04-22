#ifndef __USER_ABI_MMAN_H
#define __USER_ABI_MMAN_H

#include "types.h"

/* mmap protection flags (POSIX-compatible) */
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

/* mmap mapping flags (POSIX-compatible) */
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

/* mmap failure return value */
#define MAP_FAILED ((void *)(uint64)-1)

/* mremap flags */
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

/* msync flags */
#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

/* madvise flags */
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8

#endif /* __USER_ABI_MMAN_H */
