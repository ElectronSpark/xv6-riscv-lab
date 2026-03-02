/*
 * MBR partition table parser.
 *
 * Parses the 4 primary partition entries from an MBR sector (LBA 0).
 * Extended (EBR) partitions are not yet supported.
 */

#include <types.h>
#include <dev/mbr.h>
#include <dev/gendisk.h>
#include "printf.h"

int mbr_is_gpt_protective(const struct mbr_sector *mbr) {
    if (mbr == NULL)
        return 0;
    if (mbr->signature != MBR_SIGNATURE)
        return 0;
    /* A protective MBR has at least one entry with type 0xEE. */
    for (int i = 0; i < MBR_PART_ENTRIES; i++) {
        if (mbr->parts[i].type == MBR_TYPE_GPT_PROT)
            return 1;
    }
    return 0;
}

int mbr_parse(struct gendisk *gd, void *lba0_data) {
    if (gd == NULL || lba0_data == NULL)
        return -1;

    struct mbr_sector *mbr = (struct mbr_sector *)lba0_data;

    /* Validate MBR signature */
    if (mbr->signature != MBR_SIGNATURE) {
        printf("mbr: no valid MBR signature on disk%d\n", gd->disk_num);
        return 0;
    }

    int found = 0;
    for (int i = 0; i < MBR_PART_ENTRIES; i++) {
        struct mbr_part_entry *pe = &mbr->parts[i];

        /* Skip empty entries */
        if (pe->type == MBR_TYPE_EMPTY)
            continue;

        /* Skip extended partition entries (not yet supported) */
        if (pe->type == MBR_TYPE_EXTENDED1 || pe->type == MBR_TYPE_EXTENDED2) {
            printf("mbr: disk%d: skipping extended partition %d (type 0x%x)\n",
                   gd->disk_num, i + 1, pe->type);
            continue;
        }

        if (pe->lba_start == 0 || pe->lba_count == 0)
            continue;

        printf("mbr: disk%d: partition %d: type=0x%x start=%u count=%u\n",
               gd->disk_num, found + 1, pe->type,
               pe->lba_start, pe->lba_count);

        int ret = gendisk_add_part(gd, (uint64)pe->lba_start,
                                   (uint64)pe->lba_count, found + 1);
        if (ret != 0) {
            printf("mbr: failed to add partition %d: %d\n", found + 1, ret);
            continue;
        }
        found++;
    }

    return found;
}
