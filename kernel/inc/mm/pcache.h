#ifndef __KERNEL_PAGE_CACHE_H__
#define __KERNEL_PAGE_CACHE_H__

#include <mm/pcache_types.h>

void pcache_global_init(void);
int pcache_init(struct pcache *pcache);
page_t *pcache_get_page(struct pcache *pcache, uint64 blkno);
void pcache_put_page(struct pcache *pcache, page_t *page);
int pcache_invalidate_page(struct pcache *pcache, page_t *page);
int pcache_flush(struct pcache *pcache);
int pcache_sync(void);
// void pcache_destroy(struct pcache *pcache);
// int pcache_evict_pages(struct pcache *pcache, size_t target_size);
// @TODO: do eviction in OOM
int pcache_read_page(struct pcache *pcache, page_t *page);
int pcache_mark_page_dirty(struct pcache *pcache, page_t *page);

#ifdef HOST_TEST
void pcache_test_run_flusher_round(uint64 round_start, bool force_round);
void pcache_test_unregister(struct pcache *pcache);
void pcache_test_set_retry_hook(void (*hook)(struct pcache *, uint64));
#endif

// ssize_t bread(dev_t dev, uint64 blockno, void *data, size_t size, bool user);
// ssize_t bwrite(dev_t dev, uint64 blockno, const void *data, size_t size, bool user);

#endif /* __KERNEL_PAGE_CACHE_H__ */
