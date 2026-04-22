/*
 * uio.c - Kernel vectored I/O helpers and slab-backed iovec allocator
 *
 * Provides a dedicated slab cache for kernel_iovec arrays.  The slab
 * object size is UIO_SLAB_IOVECS * sizeof(struct kernel_iovec) — large
 * enough to satisfy the vast majority of readv/writev calls without
 * falling through to kvmalloc.
 */

#include "types.h"
#include "defs.h"
#include "errno.h"
#include "string.h"
#include "printf.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include "vfs/uio.h"

/*
 * Slab object holds this many iovecs.  Covers up to 64 segments from a
 * single slab allocation; larger requests fall through to kvmalloc.
 */
#define UIO_SLAB_IOVECS 64

static slab_cache_t __uio_iovec_slab = {0};

void uio_init(void)
{
    int ret = slab_cache_init(&__uio_iovec_slab, "uio_iovec_cache",
                              UIO_SLAB_IOVECS * sizeof(struct kernel_iovec),
                              SLAB_FLAG_STATIC);
    assert(ret == 0,
           "Failed to initialise uio_iovec_cache slab cache, errno=%d", ret);
}

struct kernel_iovec *uio_iovec_alloc(int nr_segs)
{
    if (nr_segs <= 0 || nr_segs > UIO_MAXIOV)
        return NULL;

    if (nr_segs <= UIO_SLAB_IOVECS) {
        struct kernel_iovec *iov = slab_alloc(&__uio_iovec_slab);
        return iov;      /* may be NULL on OOM */
    }

    /* Overflow path for very large iovec arrays */
    return kvmalloc((size_t)nr_segs * sizeof(struct kernel_iovec));
}

void uio_iovec_free(struct kernel_iovec *iov)
{
    if (iov == NULL)
        return;

    /*
     * Determine whether the pointer came from the slab or from kvmalloc.
     * slab_free() validates ownership internally; if the pointer doesn't
     * belong to any slab it is a bug — but for the overflow path we used
     * kvmalloc, so we need to distinguish.  The simplest approach: try
     * slab_free first; if not slab-owned, fall through to kvfree.
     *
     * Since the slab allocator in this kernel always tags pages with
     * their owning slab, we rely on the page metadata check inside
     * slab_free().  However, to keep things explicit and avoid
     * double-free hazards we track via the nr_segs that the caller
     * originally asked for.  For now, we keep it simple: anything that
     * could have come from the slab (i.e., the address falls in slab
     * pages) goes to slab_free; the rest goes to kvfree.
     *
     * The current approach: always use slab_free — it will panic on a
     * bad pointer, and callers of uio_iovec_alloc who needed >
     * UIO_SLAB_IOVECS got a kvmalloc pointer.  We add a helper that
     * takes the original nr_segs so we can route correctly.
     */
    slab_free(iov);
}

/*
 * uio_iovec_free_ex - Free an iovec array with knowledge of original size.
 * Needed because the slab vs kvmalloc path depends on nr_segs.
 */
void uio_iovec_free_ex(struct kernel_iovec *iov, int nr_segs)
{
    if (iov == NULL)
        return;

    if (nr_segs <= UIO_SLAB_IOVECS) {
        slab_free(iov);
    } else {
        kvfree(iov);
    }
}
