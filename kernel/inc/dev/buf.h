#ifndef __KERNEL_BUF_H
#define __KERNEL_BUF_H

#include "compiler.h"
#include "list_type.h"
#include "hlist_type.h"
#include "lock/mutex_types.h"

struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  dev_t dev;
  uint blockno;
  mutex_t lock;
  uint refcnt;
  hlist_entry_t hlist_entry; // hash list entry
  list_node_t free_entry;    // Free list for O(1) free buffer lookup (LRU order)
  uchar *data;
} __ALIGNED_CACHELINE;

// Use a prime number close to NBUF for good distribution
// NBUF = MAXOPBLOCKS * 300 = 80 * 300 = 24000
// Use a prime ~NBUF for ~1 item per bucket on average
#define BIO_HASH_BUCKETS 24007

#endif      /* __KERNEL_BUF_H */
