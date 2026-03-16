#ifndef __KERNEL_PAGE_CACHE_H__
#define __KERNEL_PAGE_CACHE_H__

#include <mm/pcache_types.h>

void pcache_global_init(void);
int pcache_init(struct pcache *pcache);
void pcache_teardown(struct pcache *pcache);
int pcache_evict_all(struct pcache *pcache);

/* ── Folio-based page cache API ──────────────────────────────────────── */

folio_t *pcache_get_folio(struct pcache *pcache, uint64 blkno);
void pcache_put_folio(struct pcache *pcache, folio_t *folio);
void pcache_put_folio_refs(struct pcache *pcache, folio_t *folio, int n);
int pcache_read_folio(struct pcache *pcache, folio_t *folio);
int pcache_mark_folio_dirty(struct pcache *pcache, folio_t *folio);

/* ── Legacy page-based API (thin wrappers around folio API) ──────────── */

page_t *pcache_get_page(struct pcache *pcache, uint64 blkno);
void pcache_put_page(struct pcache *pcache, page_t *page);
int pcache_invalidate_page(struct pcache *pcache, page_t *page);
int pcache_flush(struct pcache *pcache);
int pcache_sync(void);
int pcache_read_page(struct pcache *pcache, page_t *page);
int pcache_prepare_write_page(struct pcache *pcache, page_t *page);
int pcache_begin_full_page_write(struct pcache *pcache, page_t *page);
void pcache_end_full_page_write(struct pcache *pcache, page_t *page,
                                bool success);
int pcache_mark_page_dirty(struct pcache *pcache, page_t *page);
int pcache_invalidate_blk(struct pcache *pcache, uint64 blkno);
int pcache_discard_blk(struct pcache *pcache, uint64 blkno);
void pcache_shrink_caches(void);

/* ── Non-blocking page cache operations ────────────────────────────────
 *
 * These variants return -EAGAIN instead of blocking when the requested
 * page is not already cached or when IO would be required.  They are
 * the building blocks for RWF_NOWAIT support in preadv2/pwritev2.
 * ──────────────────────────────────────────────────────────────────── */

/**
 * pcache_get_page_nowait - look up a cached page without blocking
 *
 * Returns the page with an elevated reference count if it is already
 * present in the page cache.  Returns NULL if the page is not cached
 * (does NOT allocate a new page or wait for eviction).
 */
page_t *pcache_get_page_nowait(struct pcache *pcache, uint64 blkno);

/**
 * pcache_read_page_nowait - ensure a page is up-to-date without blocking
 *
 * If the page is already up-to-date, returns 0.
 * If the page needs IO (not up-to-date, no IO in progress), returns -EAGAIN.
 * If another thread is performing IO on the page, returns -EAGAIN.
 */
int pcache_read_page_nowait(struct pcache *pcache, page_t *page);

/* ── Batch (vectorized) page cache operations ─────────────────────────
 *
 * Amortise per-page lock/unlock overhead by operating on batches of
 * pages in a single call.
 * ──────────────────────────────────────────────────────────────────── */

/**
 * pcache_put_pages - release a batch of pages
 *
 * Calls pcache_put_page for each entry in @pvec, then resets the vector.
 */
void pcache_put_pages(struct pcache *pcache, struct pcache_page_vec *pvec);

/* ── High-level vectorized read/write through the page cache ──────────
 *
 * These helpers encapsulate the common "iterate iov_iter, map file
 * offsets to pcache pages, copy data" loop that every pcache-backed
 * filesystem duplicates.  They respect RWF_NOWAIT in iter->flags.
 * ──────────────────────────────────────────────────────────────────── */

struct iov_iter; /* forward declaration */

/**
 * pcache_readv - vectorized read from page cache into user/kernel buffers
 * @pcache:  the page cache to read from
 * @iter:    iov_iter describing the destination scatter-gather list
 * @pos:     starting byte offset in the file (updated on return)
 * @isize:   current file size (reads are clamped to this)
 * @user:    true if iter buffers are user-space addresses
 *
 * Returns total bytes read, 0 for EOF, or negative errno.
 * On RWF_NOWAIT, returns -EAGAIN if any required page is not in cache.
 */
ssize_t pcache_readv(struct pcache *pcache, struct iov_iter *iter,
                     loff_t *ppos, loff_t isize, bool user);

/**
 * pcache_writev - vectorized write from user/kernel buffers into page cache
 * @pcache:  the page cache to write into
 * @iter:    iov_iter describing the source gather list
 * @pos:     starting byte offset in the file (updated on return)
 * @user:    true if iter buffers are user-space addresses
 *
 * Suitable for backendless filesystems (tmpfs) where no block allocation
 * is needed.  Block-backed filesystems (xv6fs) that require bmap before
 * each write should use the per-page primitives instead.
 *
 * Returns total bytes written or negative errno.
 * On RWF_NOWAIT, returns -EAGAIN if any required page is not in cache.
 */
ssize_t pcache_writev(struct pcache *pcache, struct iov_iter *iter,
                      loff_t *ppos, bool user);

#ifdef HOST_TEST
void pcache_test_run_flusher_round(uint64 round_start, bool force_round);
void pcache_test_unregister(struct pcache *pcache);
void pcache_test_set_retry_hook(void (*hook)(struct pcache *, uint64));
#endif

#endif /* __KERNEL_PAGE_CACHE_H__ */
