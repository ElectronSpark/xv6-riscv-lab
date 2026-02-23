/*
 * bits/mman.h — xv6 memory mapping constants for musl
 *
 * Must match kernel/inc/mm/vm_types.h.
 */

#define MAP_FAILED ((void *)-1)

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       MAP_ANONYMOUS

/* MAP_* flags not supported by xv6 but referenced by musl */
#define MAP_GROWSDOWN  0x00100
#define MAP_DENYWRITE  0x00800
#define MAP_EXECUTABLE 0x01000
#define MAP_LOCKED     0x02000
#define MAP_NORESERVE  0x04000
#define MAP_POPULATE   0x08000
#define MAP_NONBLOCK   0x10000
#define MAP_STACK      0x20000
#define MAP_HUGETLB    0x40000
#define MAP_FIXED_NOREPLACE 0x100000

#define PROT_NONE      0
#define PROT_READ      1
#define PROT_WRITE     2
#define PROT_EXEC      4
#define PROT_GROWSDOWN 0x01000000
#define PROT_GROWSUP   0x02000000

#define MS_ASYNC       1
#define MS_INVALIDATE  2
#define MS_SYNC        4

#define MADV_NORMAL      0
#define MADV_RANDOM      1
#define MADV_SEQUENTIAL  2
#define MADV_WILLNEED    3
#define MADV_DONTNEED    4
#define MADV_FREE        8

#define MCL_CURRENT    1
#define MCL_FUTURE     2
#define MCL_ONFAULT    4

#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
#define MREMAP_DONTUNMAP 4
