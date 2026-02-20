#ifndef __KERNEL_BLOCK_IO_H
#define __KERNEL_BLOCK_IO_H

#include <dev/bio_types.h>
#include <lock/completion.h>
#include "errno.h"

#define BLK_SIZE_SHIFT 9 // Number of bits to represent block size (2^9 = 512)
#define BLK_SIZE (1UL << BLK_SIZE_SHIFT) // Block size in bytes, 512 bytes
#define BIO_MAX_VECS 128                 // Maximum number of bio_vec in a bio
#define BIO_MAX_SIZE (1U << 15) // Maximum size of a bio in bytes (32KB)

static inline void bio_iter_start(struct bio *bio, struct bio_iter *it) {
    it->blkno = bio->blkno;
    it->bvec_idx = 0;
    if (bio->vec_length > 0) {
        it->size = bio->bvecs[0].len;
        it->size_done = 0;
    } else {
        it->size = 0;
        it->size_done = 0;
    }
}

// Move the bio iterator forward by one segment
// Return true if the iterator is still valid, false if it reaches the end of
// the bio
static inline void bio_iter_next_seg(struct bio *bio, struct bio_iter *it) {
    int16 bvec_idx = it->bvec_idx + 1;
    if (bvec_idx > bio->vec_length || bvec_idx < 0) {
        return;
    }
    it->size -= bio->bvecs[bvec_idx].len;
    it->size_done += bio->bvecs[bvec_idx].len;
    bio->done_size += bio->bvecs[bvec_idx].len;
    it->blkno =
        bio->blkno + (bio->done_size >> (BLK_SIZE_SHIFT + bio->block_shift));
    it->bvec_idx = bvec_idx;
}

static inline bool bio_iter_copy_bvec(struct bio *bio, struct bio_iter *it,
                                      struct bio_vec *bvec) {
    if (it->bvec_idx >= bio->vec_length) {
        return false;
    }
    *bvec = bio->bvecs[it->bvec_idx];

    return true;
}

// bio_for_each_segment - iterate over all segments in a bio
#define bio_for_each_segment(bvec, bio, iter)                                  \
    for (bio_iter_start(bio, iter); bio_iter_copy_bvec(bio, iter, bvec);       \
         bio_iter_next_seg(bio, iter))

// rq_for_each_bio - @TODO: iterate over all bios in a request

// Get the direction of a bio: 0 for read, 1 for write
static inline int bio_dir_write(struct bio *bio) { return bio->rw; }

// Start I/O accounting for a bio
static inline void bio_start_io_acct(struct bio *bio) {
    bio->done = 0;
    bio->done_size = 0;
    bio->error = 0;
    completion_reinit(&bio->io_completion);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// End I/O accounting for a bio
static inline void bio_end_io_acct(struct bio *bio) {
    bio->done = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// bio_endio - end I/O for a bio, call the completion callback if any
static inline void bio_endio(struct bio *bio) {
    if (bio->end_io) {
        bio->end_io(bio);
    }
}

// bio_complete - signal that a bio's I/O has finished.
// Called from the block driver (interrupt handler or synchronous completion)
// after the data transfer is done.  Wakes any thread blocked in bio_await().
static inline void bio_complete(struct bio *bio) {
    bio_end_io_acct(bio);
    bio_endio(bio);
    complete_all(&bio->io_completion);
}

// bio_await - wait for a bio's I/O to finish, with signal awareness.
// The caller must have previously submitted the bio via blkdev_submit_bio().
//
// Because the device is already transferring data into (or from) the
// bio's pages, we cannot abandon the I/O mid-flight.  If a signal
// arrives while we are sleeping, we fall back to an uninterruptible
// wait (typically sub-millisecond) to let the device finish, then
// report -EINTR so the caller can short-circuit its work.
//
// Returns 0 on success, -EINTR if interrupted, or a negative errno
// from the device on I/O error.
static inline int bio_await(struct bio *bio) {
    int ret = wait_for_completion_interruptible(&bio->io_completion);
    if (ret == -EINTR) {
        // Signal received but I/O is in flight — must let it finish
        // so the bio/buffer resources are safe to release.
        wait_for_completion(&bio->io_completion);
        return bio->error ? bio->error : -EINTR;
    }
    return bio->error;
}

// Return a newly allocated bio when successful, or ERR_PTR on error
struct bio *bio_alloc(blkdev_t *bdev, int16 vec_length, bool rw,
                      void (*end_io)(struct bio *bio), void *private_data);
int bio_add_seg(struct bio *bio, page_t *page, int16 idx, uint16 len,
                uint16 offset);
int bio_dup(struct bio *bio);     // Increment the reference count of a bio
int bio_release(struct bio *bio); // Decrement the reference count of a bio,
                                  // free it if the count reaches zero
int bio_validate(struct bio *bio,
                 blkdev_t *blkdev); // Validate the fields of a bio against the
                                    // target block device

#endif // __KERNEL_BLOCK_IO_H
