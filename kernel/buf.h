#ifndef __KERNEL_BUF_H
#define __KERNEL_BUF_H

#include "compiler.h"
#include "list_type.h"
#include "hlist_type.h"

struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  mutex_t lock;
  uint refcnt;
  hlist_entry_t hlist_entry; // hash list entry
  list_node_t lru_entry;
  // struct buf *prev; // LRU cache list
  // struct buf *next;
  uchar data[BSIZE];
};

#define BIO_HASH_BUCKETS 63

#endif      /* __KERNEL_BUF_H */
