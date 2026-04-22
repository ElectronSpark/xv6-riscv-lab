/*
 * MBR partition table on-disk structures and parser API.
 */
#ifndef __KERNEL_MBR_H
#define __KERNEL_MBR_H

#include <types.h>

#define MBR_SIGNATURE       0xAA55
#define MBR_PART_ENTRIES    4
#define MBR_TYPE_EMPTY      0x00
#define MBR_TYPE_GPT_PROT   0xEE   /* GPT protective MBR */
#define MBR_TYPE_EXTENDED1  0x05
#define MBR_TYPE_EXTENDED2  0x0F
#define MBR_TYPE_LINUX      0x83

/* A single MBR partition entry (16 bytes) */
struct __attribute__((packed)) mbr_part_entry {
    uint8  status;          /* 0x80 = bootable, 0x00 = inactive */
    uint8  chs_first[3];   /* CHS of first sector */
    uint8  type;            /* partition type */
    uint8  chs_last[3];    /* CHS of last sector */
    uint32 lba_start;       /* LBA of first sector */
    uint32 lba_count;       /* number of sectors */
};

/* On-disk MBR sector (512 bytes) */
struct __attribute__((packed)) mbr_sector {
    uint8                bootstrap[446];
    struct mbr_part_entry parts[MBR_PART_ENTRIES];
    uint16               signature;   /* must be MBR_SIGNATURE (0xAA55) */
};

_Static_assert(sizeof(struct mbr_sector) == 512, "MBR sector must be 512 bytes");

struct gendisk;

/*
 * Parse MBR partition table from a page containing LBA 0.
 * Returns the number of partitions found (>= 0), or negative errno.
 */
int mbr_parse(struct gendisk *gd, void *lba0_data);

/*
 * Check whether an MBR sector contains a GPT protective MBR.
 * Returns 1 if GPT protective, 0 otherwise.
 */
int mbr_is_gpt_protective(const struct mbr_sector *mbr);

#endif /* __KERNEL_MBR_H */
