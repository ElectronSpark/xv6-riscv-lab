/*
 * gendisk — generic disk layer public API
 */
#ifndef __KERNEL_GENDISK_H
#define __KERNEL_GENDISK_H

#include <dev/gendisk_types.h>

typedef struct page_struct page_t;

/*
 * Initialise the gendisk subsystem (slab caches, global list).
 * Called once at boot.
 */
void gendisk_init(void);

/*
 * Probe a raw block device for partitions (GPT then MBR).
 * For each discovered partition a child blkdev_t is registered
 * in devtmpfs as "<raw_devname>p<N>" (e.g. disk0p1).
 *
 * Returns the gendisk on success, ERR_PTR on failure.
 */
struct gendisk *gendisk_probe(blkdev_t *raw);

/*
 * Remove a gendisk and all its partition sub-devices.
 * Unregisters partition blkdevs and frees the gendisk.
 */
void gendisk_remove(struct gendisk *gd);

/*
 * Add a partition manually.
 * Called by GPT/MBR parsers or by the loop device layer.
 */
int gendisk_add_part(struct gendisk *gd, uint64 start_lba,
                     uint64 num_sectors, int part_num);

/*
 * Add a partition with GPT GUIDs (type + unique partition GUID).
 * Called by the GPT parser to store GUID information with each partition.
 */
int gendisk_add_part_guid(struct gendisk *gd, uint64 start_lba,
                          uint64 num_sectors, int part_num,
                          const struct gpt_guid *type_guid,
                          const struct gpt_guid *part_guid);

/*
 * Simple BIO helper: read a single page from a blkdev synchronously.
 * Caller provides a page_t whose physical memory will be filled.
 * blkno is in 512-byte sector units.  Returns 0 on success.
 */
int gendisk_read_sectors(blkdev_t *bdev, uint64 blkno, page_t *page,
                         uint16 len, uint16 offset);

/*
 * Find a block device by its unique partition GUID.
 * Walks all known gendisks and their partitions.
 * Returns blkdev_t pointer on match, or NULL if not found.
 */
blkdev_t *gendisk_find_by_guid(const struct gpt_guid *guid);

/*
 * Parse a GUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into
 * a gpt_guid struct.  Returns 0 on success, -1 on parse error.
 */
int gendisk_guid_parse(const char *str, struct gpt_guid *out);

/*
 * Format a gpt_guid as "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into buf.
 * buf must be at least 37 bytes.
 */
void gendisk_guid_format(const struct gpt_guid *guid, char *buf, size_t bufsz);

#endif /* __KERNEL_GENDISK_H */
