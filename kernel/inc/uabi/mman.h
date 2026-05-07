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
#define MAP_SHARED_VALIDATE 0x03
#define MAP_TYPE 0x0f
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_NONBLOCK 0x10000
#define MAP_STACK 0x20000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FILE 0

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
#define MADV_REMOVE 9
#define MADV_DONTFORK 10
#define MADV_DOFORK 11
#define MADV_MERGEABLE 12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE 14
#define MADV_NOHUGEPAGE 15
#define MADV_DONTDUMP 16
#define MADV_DODUMP 17
#define MADV_WIPEONFORK 18
#define MADV_KEEPONFORK 19
#define MADV_COLD 20
#define MADV_PAGEOUT 21
#define MADV_POPULATE_READ 22
#define MADV_POPULATE_WRITE 23
#define MADV_DONTNEED_LOCKED 24
#define MADV_COLLAPSE 25

#endif /* __USER_ABI_MMAN_H */
