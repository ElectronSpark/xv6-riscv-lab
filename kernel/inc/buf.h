#ifndef __KERNEL_BUF_H
#define __KERNEL_BUF_H

#include "compiler.h"
#include "mutex_types.h"

struct page_struct;

struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  mutex_t lock;
  uint refcnt;
  uchar *data;
  struct page_struct *page;
} __attribute__((aligned(64)));

#endif      /* __KERNEL_BUF_H */
