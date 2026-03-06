/**
 * @file uio.h
 * @brief Kernel vectored I/O (readv/writev) types and helpers
 *
 * Provides the kernel-internal iovec representation and helper routines
 * for scatter-gather I/O.  A dedicated slab cache backs iovec array
 * allocations so that hot-path readv/writev syscalls never fall into
 * the generic page allocator.
 */

#ifndef KERNEL_VFS_UIO_H
#define KERNEL_VFS_UIO_H

#include "types.h"

/* ── Maximum number of iovec entries per single readv/writev call ───────── */
#define UIO_MAXIOV    1024

/* Number of iovecs that fit on the kernel stack (fast path, no alloc) */
#define UIO_FASTIOV   8

/**
 * struct kernel_iovec - kernel-internal scatter-gather element
 * @iov_base:  User-space (or kernel) base address
 * @iov_len:   Number of bytes starting at @iov_base
 *
 * Matches the Linux / musl user-space struct iovec layout on LP64 so
 * that copyin can transfer the array directly.
 */
struct kernel_iovec {
    uint64 iov_base;   /* void __user * */
    uint64 iov_len;    /* size_t        */
};

/**
 * struct iov_iter - iterator over an iovec array
 * @iov:       Pointer to the first kernel_iovec element
 * @nr_segs:   Number of remaining segments
 * @count:     Total remaining bytes across all segments
 * @iov_off:   Byte offset consumed inside the current segment
 *
 * Filesystem readv/writev callbacks receive this iterator so they can
 * consume the buffer list incrementally without re-scanning from the
 * start on each call.
 */
struct iov_iter {
    const struct kernel_iovec *iov;
    int    nr_segs;
    size_t count;     /* total bytes remaining */
    size_t iov_off;   /* bytes already consumed in iov[0] */
};

/* ── iov_iter helpers ──────────────────────────────────────────────────── */

/**
 * iov_iter_init - initialise an iov_iter from a kernel_iovec array
 * @iter:     Iterator to initialise
 * @iov:      Array of kernel_iovec
 * @nr_segs:  Number of elements in @iov
 * @count:    Total byte count across all segments
 */
static inline void iov_iter_init(struct iov_iter *iter,
                                 const struct kernel_iovec *iov,
                                 int nr_segs, size_t count)
{
    iter->iov     = iov;
    iter->nr_segs = nr_segs;
    iter->count   = count;
    iter->iov_off = 0;
}

/**
 * iov_iter_advance - skip @bytes in the iterator
 *
 * Call after successfully consuming @bytes from the current position.
 * When @bytes is 0 and the current segment is fully consumed (or has
 * zero length), this still advances past that segment so callers can
 * use iov_iter_advance(iter, 0) to skip zero-length iovec entries.
 */
static inline void iov_iter_advance(struct iov_iter *iter, size_t bytes)
{
    iter->count -= bytes;
    while (iter->nr_segs > 0) {
        size_t seg_left = iter->iov->iov_len - iter->iov_off;
        if (bytes < seg_left) {
            iter->iov_off += bytes;
            return;
        }
        bytes -= seg_left;
        iter->iov++;
        iter->nr_segs--;
        iter->iov_off = 0;
    }
}

/* ── Slab-backed iovec array allocator ─────────────────────────────────── */

/**
 * uio_init - initialise the iovec slab cache (called once at boot)
 */
void uio_init(void);

/**
 * uio_iovec_alloc - allocate an iovec array from the slab cache
 * @nr_segs: number of segments needed
 *
 * For small counts (<= UIO_FASTIOV) callers should use a stack array;
 * this function is intended for the overflow path.
 * Returns NULL on allocation failure.
 */
struct kernel_iovec *uio_iovec_alloc(int nr_segs);

/**
 * uio_iovec_free - return an iovec array to the slab cache
 * @iov: pointer previously returned by uio_iovec_alloc()
 *
 * Safe to call with NULL.  Only valid when nr_segs <= UIO_SLAB_IOVECS
 * (the slab path).  For the general case, use uio_iovec_free_ex().
 */
void uio_iovec_free(struct kernel_iovec *iov);

/**
 * uio_iovec_free_ex - free an iovec array with knowledge of original count
 * @iov:      pointer previously returned by uio_iovec_alloc()
 * @nr_segs:  the nr_segs value originally passed to uio_iovec_alloc()
 *
 * Routes to slab_free or kvfree depending on the original array size.
 * Safe to call with NULL.
 */
void uio_iovec_free_ex(struct kernel_iovec *iov, int nr_segs);

/**
 * iov_iter_total_len - compute total byte count of an iovec array
 * @iov:      Array of kernel_iovec
 * @nr_segs:  Number of elements
 *
 * Returns the sum of all iov_len fields, or (size_t)-1 on overflow.
 */
static inline size_t iov_iter_total_len(const struct kernel_iovec *iov,
                                        int nr_segs)
{
    size_t total = 0;
    for (int i = 0; i < nr_segs; i++) {
        if (__builtin_add_overflow(total, iov[i].iov_len, &total))
            return (size_t)-1;
    }
    return total;
}

#endif /* KERNEL_VFS_UIO_H */
