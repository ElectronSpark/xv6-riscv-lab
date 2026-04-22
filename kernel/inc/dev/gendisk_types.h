/*
 * gendisk — generic disk layer types
 *
 * Wraps a raw blkdev_t (physical disk) and exposes partition sub-devices.
 * Each partition is itself a blkdev_t whose submit_bio transparently
 * adds the LBA start offset before dispatching to the parent device.
 */
#ifndef __KERNEL_GENDISK_TYPES_H
#define __KERNEL_GENDISK_TYPES_H

#include <types.h>
#include <list_type.h>
#include <lock/spinlock.h>
#include <dev/dev_types.h>
#include <dev/gpt.h>

#define GENDISK_MAX_PARTS 128   /* maximum partitions per disk */
#define GENDISK_NAME_LEN  32    /* max devtmpfs name length    */

struct gendisk;

/* A single partition — first field is blkdev_t so container_of works. */
struct gendisk_part {
    blkdev_t          blkdev;                 /* partition block device   */
    struct gendisk   *parent;                 /* back-pointer to disk     */
    uint64            start_lba;              /* offset in 512B sectors   */
    uint64            num_sectors;            /* size in 512B sectors     */
    int               part_num;               /* 1-based partition number */
    char              devname[GENDISK_NAME_LEN]; /* e.g. "disk0p1"       */
    struct gpt_guid   type_guid;              /* partition type GUID      */
    struct gpt_guid   part_guid;              /* unique partition GUID    */
    int               has_guid;               /* 1 if GUIDs are valid     */
};

/* A generic disk — wraps a raw hw blkdev. */
struct gendisk {
    blkdev_t         *raw;                    /* underlying hw blkdev     */
    int               disk_num;               /* 0-based disk index       */
    int               nr_parts;               /* number of valid parts    */
    struct gendisk_part *parts[GENDISK_MAX_PARTS]; /* partition array     */
    spinlock_t        lock;
    list_node_t       entry;                  /* global disk list         */
};

#endif /* __KERNEL_GENDISK_TYPES_H */
