#ifndef __KERNEL_BUF_H
#define __KERNEL_BUF_H

#include "compiler.h"
#include "pcache.h"

/*
 * New buffer-cache facade built on top of the page cache.  Callers interact
 * with physical pages directly; helper routines below translate block numbers
 * into in-memory addresses inside the cached page.
 */

page_t *bread(uint dev, uint blockno, void **data_out);
int bwrite(uint dev, uint blockno, page_t *page);
void brelse(page_t *page);
void bpin(page_t *page);
void bunpin(page_t *page);
void bmark_dirty(page_t *page);
void *block_data(page_t *page, uint blockno);

#endif      /* __KERNEL_BUF_H */
