#ifndef __KERNEL_BUF_H
#define __KERNEL_BUF_H

#include "compiler.h"
#include "list_type.h"
#include "hlist_type.h"
#include "mutex_types.h"

struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  dev_t dev;
  uint blockno;
  mutex_t lock;
  uint refcnt;
  hlist_entry_t hlist_entry; // hash list entry
  list_node_t lru_entry;
  uchar *data;
} __ALIGNED_CACHELINE;

#define BIO_HASH_BUCKETS 63

#endif      /* __KERNEL_BUF_H */
