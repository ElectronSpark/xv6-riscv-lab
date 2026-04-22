/*
 * GPT (GUID Partition Table) on-disk structures and parser API.
 */
#ifndef __KERNEL_GPT_H
#define __KERNEL_GPT_H

#include <types.h>

#define GPT_SIGNATURE       0x5452415020494645ULL   /* "EFI PART" */
#define GPT_HEADER_LBA      1
#define GPT_ENTRY_SIZE      128
#define GPT_MAX_ENTRIES     128

/* A 128-bit GUID stored as raw bytes (little-endian mixed encoding). */
struct __attribute__((packed)) gpt_guid {
    uint32 data1;
    uint16 data2;
    uint16 data3;
    uint8  data4[8];
};

/* GPT header (LBA 1), 92 bytes minimum. */
struct __attribute__((packed)) gpt_header {
    uint64 signature;           /* must be GPT_SIGNATURE */
    uint32 revision;            /* typically 0x00010000 */
    uint32 header_size;         /* size of this header in bytes (>= 92) */
    uint32 header_crc32;        /* CRC32 of header (with this field zeroed) */
    uint32 reserved;            /* must be zero */
    uint64 my_lba;              /* LBA of this header (1 for primary) */
    uint64 alternate_lba;       /* LBA of backup header */
    uint64 first_usable_lba;    /* first usable LBA for partitions */
    uint64 last_usable_lba;     /* last usable LBA for partitions */
    struct gpt_guid disk_guid;  /* disk GUID */
    uint64 part_entry_lba;      /* LBA of partition entry array (usually 2) */
    uint32 num_part_entries;    /* number of partition entries */
    uint32 part_entry_size;     /* size of each entry (usually 128) */
    uint32 part_array_crc32;    /* CRC32 of entire partition entry array */
};

_Static_assert(sizeof(struct gpt_header) == 92, "GPT header must be 92 bytes");

/* A single GPT partition entry (128 bytes). */
struct __attribute__((packed)) gpt_entry {
    struct gpt_guid type_guid;  /* partition type GUID (all zero = unused) */
    struct gpt_guid part_guid;  /* unique partition GUID */
    uint64 start_lba;           /* first LBA */
    uint64 end_lba;             /* last LBA (inclusive) */
    uint64 attributes;          /* attribute flags */
    uint16 name[36];            /* UTF-16LE partition name */
};

_Static_assert(sizeof(struct gpt_entry) == 128, "GPT entry must be 128 bytes");

struct gendisk;

/*
 * CRC32 (ISO 3309 / ITU-T V.42) for GPT validation.
 * Standard Ethernet CRC32 polynomial 0xEDB88320.
 */
uint32 crc32(uint32 crc, const void *buf, size_t len);

/*
 * Parse GPT partition table from a block device.
 * Reads LBA 1+ from the raw device attached to the gendisk.
 * Returns the number of partitions found (>= 0), or negative errno.
 */
int gpt_parse(struct gendisk *gd);

/*
 * Check if a gpt_guid is all-zeros (empty partition type).
 */
static inline int gpt_guid_is_zero(const struct gpt_guid *guid) {
    return guid->data1 == 0 && guid->data2 == 0 &&
           guid->data3 == 0 &&
           guid->data4[0] == 0 && guid->data4[1] == 0 &&
           guid->data4[2] == 0 && guid->data4[3] == 0 &&
           guid->data4[4] == 0 && guid->data4[5] == 0 &&
           guid->data4[6] == 0 && guid->data4[7] == 0;
}

#endif /* __KERNEL_GPT_H */
