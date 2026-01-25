//
// Ramdisk driver - block device using pre-loaded memory (FDT initrd region).
//

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "memlayout.h"
#include "lock/spinlock.h"
#include "vfs/xv6fs/ondisk.h"
#include "buf.h"
#include "blkdev.h"
#include "page.h"
#include "errno.h"
#include "fdt.h"

static struct {
  struct spinlock lock;
  uint64 base;
  uint64 size_bytes;
  uint64 size_blocks;
} ramdisk;

static int ramdisk_open(blkdev_t *blkdev) {
  return 0;
}

static int ramdisk_release(blkdev_t *blkdev) {
  return 0;
}

static int ramdisk_submit_bio(blkdev_t *blkdev, struct bio *bio) {
  struct bio_vec bvec;
  struct bio_iter iter;

  spin_lock(&ramdisk.lock);

  bio_start_io_acct(bio);
  bio_for_each_segment(&bvec, bio, &iter) {
    uint64 sector = iter.blkno;
    page_t *page = bvec.bv_page;
    
    if (page == NULL) {
      spin_unlock(&ramdisk.lock);
      bio_end_io_acct(bio);
      bio_endio(bio);
      return -EINVAL;
    }

    // Calculate offset in ramdisk
    uint64 offset = sector * 512;
    
    // Check bounds
    if (offset + bvec.len > ramdisk.size_bytes) {
      printf("ramdisk: access beyond end of device (offset=%lx, len=%d, size=%lx)\n",
             offset, bvec.len, ramdisk.size_bytes);
      spin_unlock(&ramdisk.lock);
      bio_end_io_acct(bio);
      bio_endio(bio);
      return -EINVAL;
    }

    void *pa = (void *)__page_to_pa(page);
    if (pa == NULL) {
      spin_unlock(&ramdisk.lock);
      bio_end_io_acct(bio);
      bio_endio(bio);
      return -EINVAL;
    }

    // Direct access to contiguous physical memory
    void *ramdisk_addr = (void *)(ramdisk.base + offset);
    
    if (bio_dir_write(bio)) {
      // Write to ramdisk
      memmove(ramdisk_addr, pa + bvec.offset, bvec.len);
    } else {
      // Read from ramdisk
      memmove(pa + bvec.offset, ramdisk_addr, bvec.len);
    }

    iter.size_done += bvec.len;
  }

  spin_unlock(&ramdisk.lock);
  
  bio_end_io_acct(bio);
  bio_endio(bio);
  return 0;
}

static blkdev_ops_t ramdisk_ops = {
    .open = ramdisk_open,
    .release = ramdisk_release,
    .submit_bio = ramdisk_submit_bio
};

static blkdev_t ramdisk_dev = {
    .dev = {
        .major = 3,
        .minor = 1,
    },
    .readable = 1,
    .writable = 1,
    .block_shift = 0, // 2^0 * 512 = 512 bytes per block
};

void
ramdisk_init(void)
{
  spin_init(&ramdisk.lock, "ramdisk");

  if (!platform.has_ramdisk || platform.ramdisk_base == 0 || platform.ramdisk_size == 0)
    return;

  ramdisk.base = platform.ramdisk_base;
  ramdisk.size_bytes = platform.ramdisk_size;
  ramdisk.size_blocks = platform.ramdisk_size / 512;

  printf("ramdisk: initialized %ld KB ramdisk (%ld sectors) at 0x%lx\n",
         ramdisk.size_bytes / 1024, ramdisk.size_blocks, ramdisk.base);

  // Register the ramdisk as a block device
  ramdisk_dev.ops = ramdisk_ops;
  int errno = blkdev_register(&ramdisk_dev);
  if (errno != 0) {
    panic("ramdisk_init: blkdev_register failed: %d", errno);
  }
}
