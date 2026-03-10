#ifndef __KERNEL_BLOCK_IO_TYPES_H
#define __KERNEL_BLOCK_IO_TYPES_H

#include <compiler.h>
#include <param.h>
#include <types.h>
#include <list_type.h>
#include <kobject.h>
#include <lock/completion_types.h>

typedef struct blkdev blkdev_t;
typedef struct page_struct page_t;
typedef struct folio folio_t;
struct bio_vec;
struct bio;
struct bio_iter;
// Pointer to a buffer page,
struct bio_vec {
    page_t *bv_page; // buffer page
    uint16 len;      // length of this buffer in Bytes
    uint16 offset;   // offset in the page where the buffer starts
};

// A series of bio_vec to transfer data to/from a continuous area in blkdev
struct bio {
    struct kobject
        kobj; // kobject for reference counting and lifecycle management
    list_node_t list_entry; // Link a series of bio in a single request
    blkdev_t *bdev;         // Block device to do I/O
    uint16 block_shift; // Copy from blkdev, block size shift relative to 512
                        // bytes, typically 1(512) or 3(4096)
    int16 vec_length;   // Number of bio_vec in this bio
    uint16 size;        // Total number of Bytes to transfer
    uint16 done_size;   // Number of blocks already transferred
    uint16 inflight_segs;  // Number of device requests issued for this bio
    uint16 completed_segs; // Number of device requests completed
    uint64 blkno;       // Starting block number in the blkdev
    struct {
        uint64 valid
            : 1; // 1 if this bio is valid. Will be set to 1 after submission
        uint64 rw : 1;   // 0 for read, 1 for write
        uint64 done : 1; // 1 if the bio is done
    };
    void (*end_io)(struct bio *bio); // Completion callback when I/O is done
    void *private_data; // Private data for the bio, used by the completion
                        // callback
    int error;          // Error code if any error happens during I/O
    completion_t io_completion; // For callers to await I/O completion
    struct bio_vec bvecs[0];
};

// Iterator to iterate through a bio
struct bio_iter {
    uint64 blkno; // Current block number in the blkdev
    uint16
        size; // size of the un-transmitted buffer in the current bio in Bytes
    uint16 size_done; // size of the already transmitted buffer in the current
                      // bio in Bytes
    int16 bvec_idx;   // index of this bvec in the bio
};

/*
 * IO scheduler request queue: see <dev/iosched_types.h> and <dev/iosched.h>.
 * The scheduler uses bio->list_entry for queue linkage and bio->blkno for
 * sorted (elevator) dispatch ordering.
 */

#endif // __KERNEL_BLOCK_IO_TYPES_H
