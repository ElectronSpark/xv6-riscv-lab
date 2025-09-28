#include <param.h>
#include <types.h>
#include <riscv.h>
#include <defs.h>
#include <blkdev.h>
#include <bio.h>
#include <page.h>
#include <errno.h>

int bio_alloc(
    blkdev_t *bdev, 
    int16 vec_length, 
    bool rw, 
    void (*end_io)(struct bio *bio), 
    void *private_data, 
    struct bio **ret_bio
) {
    struct bio *bio = NULL;
    if (bdev == NULL || vec_length <= 0 || vec_length > BIO_MAX_VECS || ret_bio == NULL) {
        return -EINVAL; // Invalid arguments
    }
    size_t bio_size = sizeof(struct bio) + vec_length * sizeof(struct bio_vec);
    bio = (struct bio *)kmm_alloc(bio_size);
    if (bio == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    memset(bio, 0, bio_size);
    bio->bdev = bdev;
    bio->block_shift = bdev->block_shift;
    bio->vec_length = vec_length;
    bio->rw = rw;
    bio->end_io = end_io;
    bio->private_data = private_data;
    bio->ref_count = 1; // Initial reference count
    *ret_bio = bio;
    return 0;
}

int bio_add_seg(struct bio *bio, page_t *page, int16 idx, uint16 len, uint16 offset) {
    if (bio == NULL || page == NULL || len == 0 || (offset + len) > PGSIZE) {
        return -EINVAL; // Invalid arguments
    }
    if (bio->valid || bio->done) {
        return -EIO; // Cannot add segment to a submitted or completed bio
    }
    if (bio->vec_length <= 0 || bio->vec_length > BIO_MAX_VECS) {
        return -EINVAL; // Invalid bio vector length
    }
    if (idx < 0 || idx >= bio->vec_length) {
        return -EINVAL; // Invalid index
    }
    uint16 total_size = bio->size - bio->bvecs[idx].len;
    total_size += len;
    if (total_size > BIO_MAX_SIZE) {
        return -E2BIG; // Total size exceeds maximum allowed
    }
    bio->bvecs[idx].bv_page = page;
    bio->bvecs[idx].len = len;
    bio->bvecs[idx].offset = offset;
    bio->size = total_size;
    return 0;
}

int bio_dup(struct bio *bio) {
    if (bio == NULL) {
        return -EINVAL; // Invalid argument
    }
    __atomic_add_fetch(&bio->ref_count, 1, __ATOMIC_SEQ_CST);
    return 0;     
}

int bio_release(struct bio *bio) {
    if (bio == NULL) {
        return -EINVAL; // Invalid argument
    }
    int ref_count = __atomic_sub_fetch(&bio->ref_count, 1, __ATOMIC_SEQ_CST);
    assert(ref_count >= 0, "bio_release: negative ref_count");
    // Release pages
    for (int i = 0; i < bio->vec_length; i++) {
        if (bio->bvecs[i].bv_page) {
            __page_ref_dec(bio->bvecs[i].bv_page);
            bio->bvecs[i].bv_page = NULL;
        }
    }
    if (ref_count == 0) {
        kmm_free(bio);
    }
    return 0;
}

int bio_validate(struct bio *bio, blkdev_t *blkdev) {
    if (bio == NULL || blkdev == NULL) {
        return -EINVAL;
    }
    if (bio->bdev != blkdev) {
        return -EINVAL;
    }
    if (bio->block_shift != blkdev->block_shift) {
        return -EINVAL;
    }
    if (bio->vec_length <= 0 || bio->vec_length > BIO_MAX_VECS) {
        return -EINVAL;
    }
    if (bio->size > BIO_MAX_SIZE) {
        return -EINVAL;
    }
    if (bio->ref_count <= 0) {
        return -EINVAL;
    }
    if (bio->error != 0) {
        return -EINVAL;
    }
    if (bio->valid || bio->done) {
        return -EINVAL;
    }

    uint32 total_size = 0;
    for (int i = 0; i < bio->vec_length; i++) {
        struct bio_vec *bvec = &bio->bvecs[i];
        if (bvec->bv_page == NULL) {
            return -EINVAL;
        }
        if ((uint32)bvec->offset + (uint32)bvec->len > PGSIZE) {
            return -EINVAL;
        }
        total_size += bvec->len;
        if (total_size > BIO_MAX_SIZE) {
            return -EINVAL;
        }
    }

    if (total_size != bio->size) {
        return -EINVAL;
    }

    return 0;
}