/**
 * @file buffer_head.h
 * @brief Linux-style buffer_head for sub-page block I/O.
 *
 * A buffer_head (bh) describes a single filesystem-sized block (BSIZE)
 * within a page-cache folio.  It replaces the legacy xv6 struct buf /
 * buffer cache with a design that sits on top of the page cache (pcache)
 * and folio infrastructure.
 *
 * Each block device (or, more precisely, each xv6fs superblock) owns a
 * "metadata pcache" that caches disk blocks in page-sized granularity.
 * A buffer_head is a lightweight, slab-allocated descriptor that points
 * into a specific BSIZE-byte region of a pcache folio.
 *
 * API summary:
 *   sb_bread()       — get a locked buffer_head with disk data (≈ bread)
 *   bh_write()       — synchronous writeback                 (≈ bwrite)
 *   bh_write_async() — mark dirty for deferred writeback     (≈ bwrite_async)
 *   bh_release()     — release a locked buffer_head          (≈ brelse)
 *   bh_sync()        — flush all dirty metadata              (≈ bsync)
 *   bh_pin() / bh_unpin()  — pin/unpin underlying page      (≈ bpin/bunpin)
 */
#ifndef __KERNEL_BUFFER_HEAD_H
#define __KERNEL_BUFFER_HEAD_H

#include <types.h>
#include <lock/mutex_types.h>
#include <mm/folio_types.h>

/* Forward declarations */
struct pcache;
struct xv6fs_superblock;

/**
 * struct buffer_head - block-level I/O descriptor backed by pcache.
 *
 * @b_blocknr:  filesystem block number (in BSIZE-byte units)
 * @b_size:     block size in bytes (always BSIZE = 1024 for xv6fs)
 * @b_bdev:     device number (dev_t = major:minor)
 * @b_data:     pointer to the BSIZE-byte region within the backing folio
 * @b_folio:    backing folio (from metadata pcache)
 * @b_pcache:   the metadata pcache that owns the backing folio
 * @b_lock:     per-bh sleep lock (held between sb_bread and bh_release)
 */
typedef struct buffer_head {
    uint64 b_blocknr;
    uint32 b_size;
    dev_t b_bdev;
    uchar *b_data;
    folio_t *b_folio;
    struct pcache *b_pcache;
    mutex_t b_lock;
} buffer_head_t;

/* ======================================================================
 *  Core API
 * ====================================================================== */

/**
 * sb_bread - read a metadata block through the page cache.
 *
 * Looks up (or allocates) the backing pcache folio for the given block,
 * ensures the data is read from disk, and returns a locked buffer_head
 * whose b_data points to the correct BSIZE offset within the folio.
 *
 * Returns NULL on I/O error or OOM.  Callers must check the return value.
 */
buffer_head_t *sb_bread(struct xv6fs_superblock *xv6_sb, uint blockno);

/**
 * bh_write - synchronous write of the buffer_head's block to disk.
 *
 * Writes the BSIZE region referenced by @bh back to the device.
 * The caller must hold bh->b_lock.
 */
void bh_write(buffer_head_t *bh);

/**
 * bh_write_async - mark the buffer_head's backing folio dirty.
 *
 * The actual writeback happens later during bh_sync().
 * The caller must hold bh->b_lock.
 */
void bh_write_async(buffer_head_t *bh);

/**
 * bh_release - release a locked buffer_head.
 *
 * Unlocks the mutex and drops the pcache folio reference.
 * The buffer_head is returned to slab.  Do not use @bh after this call.
 */
void bh_release(buffer_head_t *bh);

/**
 * bh_sync - flush all dirty pages in the metadata pcache.
 *
 * Walks the pcache dirty list, writes each dirty page to disk,
 * and clears the dirty flag.
 */
void bh_sync(struct xv6fs_superblock *xv6_sb);

/**
 * bh_pin - pin the backing pcache page to prevent eviction.
 *
 * Increments the pcache folio reference count.  The caller need NOT
 * hold bh->b_lock (matches bpin semantics).
 */
void bh_pin(buffer_head_t *bh);

/**
 * bh_unpin - unpin the backing pcache page.
 *
 * Decrements the pcache folio reference count.  The caller need NOT
 * hold bh->b_lock (matches bunpin semantics).
 */
void bh_unpin(buffer_head_t *bh);

/**
 * bh_dirty_count - return the number of dirty pages in the meta pcache.
 */
uint bh_dirty_count(struct xv6fs_superblock *xv6_sb);

/* ======================================================================
 *  Metadata pcache lifecycle
 * ====================================================================== */

/**
 * meta_pcache_init - initialise the metadata page cache for a superblock.
 *
 * Called during xv6fs mount (before any sb_bread calls).  Sets up the
 * pcache that backs all buffer_head allocations for this superblock.
 */
void meta_pcache_init(struct xv6fs_superblock *xv6_sb);

/**
 * meta_pcache_destroy - tear down the metadata page cache.
 */
void meta_pcache_destroy(struct xv6fs_superblock *xv6_sb);

/**
 * bh_global_init - initialise the buffer_head slab cache.
 *
 * Called once during kernel boot (replaces binit).
 */
void bh_global_init(void);

#endif /* __KERNEL_BUFFER_HEAD_H */
