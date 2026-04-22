#ifndef __KERNEL_MEMSTAT_H
#define __KERNEL_MEMSTAT_H

#define MEMSTAT_VERBOSE (1U << 0)
#define MEMSTAT_DETAILED (1U << 1)
#define MEMSTAT_INCLUDE_SLAB (1U << 2)
#define MEMSTAT_INCLUDE_BUDDY (1U << 3)
#define MEMSTAT_ADD_FREE (1U << 4)
#define MEMSTAT_ADD_USED (1U << 5)

#define MEMSTAT_DEFAULT_FLAGS                                                  \
    (MEMSTAT_VERBOSE | MEMSTAT_INCLUDE_SLAB | MEMSTAT_INCLUDE_BUDDY |          \
     MEMSTAT_ADD_FREE | MEMSTAT_ADD_USED)
#define MEMSTAT_ALL_FLAGS                                                      \
    (MEMSTAT_VERBOSE | MEMSTAT_DETAILED | MEMSTAT_INCLUDE_SLAB |               \
     MEMSTAT_INCLUDE_BUDDY | MEMSTAT_ADD_FREE | MEMSTAT_ADD_USED)

#endif /* __KERNEL_MEMSTAT_H */
