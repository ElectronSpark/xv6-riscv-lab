//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "vfs/xv6fs/ondisk.h"  // for BSIZE
#include "buf.h"
#include "virtio.h"
#include "blkdev.h"
#include "page.h"
#include "errno.h"
#include "proc/sched.h"
#include "completion.h"
#include "trap.h"
#include "freelist.h"
#include "fdt.h"

// the address of virtio mmio register r for disk n.
#define R(n, r) ((volatile uint32 *)(__virtio_mmio_base[n] + (r)))

// These are initialized from platform info at runtime
uint64 __virtio_mmio_base[N_VIRTIO] = { 0x10001000, 0x10002000, 0x10003000 };
uint64 __virtio_irqno[N_VIRTIO] = { 1, 2, 3 };

static void
virtio_disk_rw(int diskno, struct bio *bio, uint64 sector, void *buf, size_t size, int write);
static void virtio_disk_intr(int irq, void *data, device_t *dev);

STATIC struct disk {
  // a set (not a ring) of DMA descriptors, with which the
  // driver tells the device where to read and write individual
  // disk operations. there are NUM descriptors.
  // most commands consist of a "chain" (a linked list) of a couple of
  // these descriptors.
  struct virtq_desc *desc;

  // a ring in which the driver writes descriptor numbers
  // that the driver would like the device to process.  it only
  // includes the head descriptor of each chain. the ring has
  // NUM elements.
  struct virtq_avail *avail;

  // a ring in which the device writes descriptor numbers that
  // the device has finished processing (just the head of each chain).
  // there are NUM used ring entries.
  struct virtq_used *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].
  uint16 free_list[NUM]; // The index of the free descriptors
  struct freelist desc_freelist; // Freelist manager for descriptors

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  struct {
    struct bio *bio;
    completion_t *comp;
    bool done;
    char status;
  } info[NUM];

  // disk command headers.
  // one-for-one with descriptors, for convenience.
  struct virtio_blk_req ops[NUM];
  
  struct spinlock vdisk_lock;
  
} disks[N_VIRTIO_DISK];

static int __virtio_disk_open(blkdev_t *blkdev) {
  return 0;
}

static int __virtio_disk_release(blkdev_t *blkdev) {
  return 0;
}

static int __virtio_disk_submit_bio(blkdev_t *blkdev, struct bio *bio) {
  struct bio_vec bvec;
  struct bio_iter iter;
  int diskno = blkdev->dev.minor - 1; // minor 1 -> disk 0, minor 2 -> disk 1

  bio_start_io_acct(bio);
  bio_for_each_segment(&bvec, bio, &iter) {
    uint64 sector = iter.blkno;
    page_t *page = bvec.bv_page;
    assert(page != NULL, "virtio_disk_submit_bio: page is NULL");
    void *pa = (void *)__page_to_pa(page);
    assert(pa != NULL, "virtio_disk_submit_bio: page has no physical address");
    virtio_disk_rw(diskno, bio, sector, pa + bvec.offset, bvec.len, bio_dir_write(bio));
    iter.size_done += bvec.len;
  }

  bio_end_io_acct(bio);
  bio_endio(bio);
  return 0;
}

static blkdev_ops_t __virtio_disk_ops = {
    .open = __virtio_disk_open,
    .release = __virtio_disk_release,
    .submit_bio = __virtio_disk_submit_bio
};

static blkdev_t virtio_disk_devs[N_VIRTIO_DISK] = {
    {
        .dev = {
            .major = 2,
            .minor = 1,
        },
        .readable = 1,
        .writable = 1,
        .block_shift = 0, // 2^0 * 512 = 512 bytes per block
    },
    {
        .dev = {
            .major = 2,
            .minor = 2,
        },
        .readable = 1,
        .writable = 1,
        .block_shift = 0, // 2^0 * 512 = 512 bytes per block
    },
};

static void __virtio_blkdev_init(int diskno) {
  virtio_disk_devs[diskno].ops = __virtio_disk_ops;
  int errno = blkdev_register(&virtio_disk_devs[diskno]);
  assert(errno == 0, "virtio_blkdev_init: blkdev_register failed: %d", errno);
  struct irq_desc virtio_irq_desc = {
    .handler = virtio_disk_intr,
    .data = (void *)(uint64)diskno,
    .dev = &virtio_disk_devs[diskno].dev,
  };
  errno = register_irq_handler(PLIC_IRQ(VIRTIO0_IRQ + diskno), &virtio_irq_desc);
  assert(errno == 0, "virtio_blkdev_init: register_irq_handler failed: %d", errno);
}

static void
__virtio_disk_init_one(int diskno)
{
  struct disk *disk = &disks[diskno];
  uint32 status = 0;

  spin_init(&disk->vdisk_lock, "virtio_disk");

  if(*R(diskno, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(diskno, VIRTIO_MMIO_VERSION) != 2 ||
     *R(diskno, VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(diskno, VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk %d", diskno);
  }
  
  // reset device
  *R(diskno, VIRTIO_MMIO_STATUS) = status;

  // set ACKNOWLEDGE status bit
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(diskno, VIRTIO_MMIO_STATUS) = status;

  // set DRIVER status bit
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(diskno, VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R(diskno, VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(diskno, VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(diskno, VIRTIO_MMIO_STATUS) = status;

  // re-read status to ensure FEATURES_OK is set.
  status = *R(diskno, VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk %d FEATURES_OK unset", diskno);

  // initialize queue 0.
  *R(diskno, VIRTIO_MMIO_QUEUE_SEL) = 0;

  // ensure queue 0 is not in use.
  if(*R(diskno, VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk %d should not be ready", diskno);

  // check maximum queue size.
  uint32 max = *R(diskno, VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk %d has no queue 0", diskno);
  if(max < NUM)
    panic("virtio disk %d max queue too short", diskno);

  // allocate and zero queue memory.
  disk->desc = kalloc();
  disk->avail = kalloc();
  disk->used = kalloc();
  if(!disk->desc || !disk->avail || !disk->used)
    panic("virtio disk %d kalloc", diskno);
  memset(disk->desc, 0, PGSIZE);
  memset(disk->avail, 0, PGSIZE);
  memset(disk->used, 0, PGSIZE);

  // set queue size.
  *R(diskno, VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // write physical addresses.
  *R(diskno, VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk->desc;
  *R(diskno, VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk->desc >> 32;
  *R(diskno, VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk->avail;
  *R(diskno, VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk->avail >> 32;
  *R(diskno, VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk->used;
  *R(diskno, VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk->used >> 32;

  // queue is ready.
  *R(diskno, VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // all NUM descriptors start out unused.
  freelist_init(&disk->desc_freelist, disk->free, disk->free_list, NUM);

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(diskno, VIRTIO_MMIO_STATUS) = status;

  __virtio_blkdev_init(diskno);
  // plic.c and trap.c arrange for interrupts from VIRTIO IRQs.
}

void
virtio_disk_init(void)
{
  if (!platform.has_virtio || platform.virtio_count == 0)
    return;
  
  int num_disks = platform.virtio_count;
  if (num_disks > N_VIRTIO_DISK)
    num_disks = N_VIRTIO_DISK;
  
  for(int i = 0; i < num_disks; i++) {
    __virtio_disk_init_one(i);
  }
}

// find a free descriptor, mark it non-free, return its index.
STATIC int
alloc_desc(struct disk *disk)
{
  return freelist_alloc(&disk->desc_freelist);
}

// mark a descriptor as free.
STATIC void
free_desc(struct disk *disk, int i)
{
  if(freelist_free(&disk->desc_freelist, i) != 0)
    panic("free_desc: invalid free");
  
  disk->desc[i].addr = 0;
  disk->desc[i].len = 0;
  disk->desc[i].flags = 0;
  disk->desc[i].next = 0;
  
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (freelist_available(&disk->desc_freelist) >= 3) {
    wakeup_on_chan(&disk->free[0]);
  }
}

// free a chain of descriptors.
STATIC void
free_chain(struct disk *disk, int i)
{
  while(1){
    int flag = disk->desc[i].flags;
    int nxt = disk->desc[i].next;
    free_desc(disk, i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
STATIC int
alloc3_desc(struct disk *disk, int *idx)
{
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc(disk);
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(disk, idx[j]);
      return -1;
    }
  }
  return 0;
}

static void
virtio_disk_rw(int diskno, struct bio *bio, uint64 sector, void *buf, size_t size, int write)
{
  struct disk *disk = &disks[diskno];
  assert(size == BSIZE, "virtio_disk_rw: size must be BSIZE");
  assert(buf != NULL, "virtio_disk_rw: buf is NULL");
  // assert((uint64)buf % PGSIZE == 0, "virtio_disk_rw: buf must be page-aligned");
  completion_t comp;
  completion_init(&comp);

  spin_lock(&disk->vdisk_lock);

  // the spec's Section 5.2 says that legacy block operations use
  // three descriptors: one for type/reserved/sector, one for the
  // data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc(disk, idx) == 0) {
      break;
    }
    // printf("virtio_disk_rw: no free descriptors, sleeping\n");
    sleep_on_chan(&disk->free[0], &disk->vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_req *buf0 = &disk->ops[idx[0]];

  if(write)
    buf0->type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0->type = VIRTIO_BLK_T_IN; // read the disk
  buf0->reserved = 0;
  buf0->sector = sector;

  disk->desc[idx[0]].addr = (uint64) buf0;
  disk->desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk->desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk->desc[idx[0]].next = idx[1];

  disk->desc[idx[1]].addr = (uint64) buf;
  disk->desc[idx[1]].len = size;
  if(write)
    disk->desc[idx[1]].flags = 0; // device reads b->data
  else
    disk->desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk->desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk->desc[idx[1]].next = idx[2];

  disk->info[idx[0]].status = 0xff; // device writes 0 on success
  disk->desc[idx[2]].addr = (uint64) &disk->info[idx[0]].status;
  disk->desc[idx[2]].len = 1;
  disk->desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk->desc[idx[2]].next = 0;

  // record struct buf for virtio_disk_intr().
  bio->private_data = NULL;
  disk->info[idx[0]].bio = bio;

  // tell the device the first index in our chain of descriptors.
  disk->avail->ring[disk->avail->idx % NUM] = idx[0];

  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  // tell the device another avail ring entry is available.
  disk->avail->idx += 1; // not % NUM ...

  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  assert(!intr_get(), "virtio_disk_rw: interrupts enabled");
  *R(diskno, VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  // @TODO: not allow interrupt here
  disk->info[idx[0]].done = false;
  disk->info[idx[0]].comp = &comp;
  spin_unlock(&disk->vdisk_lock);

  wait_for_completion(&comp);

  spin_lock(&disk->vdisk_lock);
  assert(disk->info[idx[0]].done == true, "virtio_disk_rw: not done");
  disk->info[idx[0]].comp = NULL;
  disk->info[idx[0]].bio = NULL;
  free_chain(disk, idx[0]);

  spin_unlock(&disk->vdisk_lock);
}

static void virtio_disk_intr(int irq, void *data, device_t *dev)
{
  uint64 diskno = (uint64)data;
  struct disk *disk = &disks[diskno];
  spin_lock(&disk->vdisk_lock);

  // the device won't raise another interrupt until we tell it
  // we've seen this interrupt, which the following line does.
  // this may race with the device writing new entries to
  // the "used" ring, in which case we may process the new
  // completion entries in this interrupt, and have nothing to do
  // in the next interrupt, which is harmless.
  *R(diskno, VIRTIO_MMIO_INTERRUPT_ACK) = *R(diskno, VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  // the device increments disk->used->idx when it
  // adds an entry to the used ring.

  while(disk->used_idx != disk->used->idx){
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int id = disk->used->ring[disk->used_idx % NUM].id;

    char status = disk->info[id].status;
    
    if(status != 0) {
      struct bio *bio = disk->info[id].bio;
      printf("ERROR: id=%d status=%d buf=%p blockno=0x%lx\n",
             id, status, bio, bio ? bio->blkno : 0);
      panic("virtio_disk_intr status: %d", status);
    }

    completion_t *comp = disk->info[id].comp;
    assert(comp != NULL, "virtio_disk_intr: comp is NULL");
    assert(disk->info[id].done == false, "virtio_disk_intr: already done");
    // Mark the bio as done
    disk->info[id].done = true;
    // Wake up the waiting thread
    complete_all(disk->info[id].comp);

    disk->used_idx += 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
  }

  spin_unlock(&disk->vdisk_lock);
}
