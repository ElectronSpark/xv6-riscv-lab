/*
 * GPT (GUID Partition Table) parser.
 *
 * Reads and validates the GPT header at LBA 1, then iterates
 * partition entries to register sub-devices via gendisk_add_part().
 */

#include <types.h>
#include <param.h>
#include "riscv.h"
#include "string.h"
#include "printf.h"
#include <mm/page.h>
#include <dev/bio.h>
#include <dev/blkdev.h>
#include <dev/gpt.h>
#include <dev/gendisk.h>
#include <errno.h>

int gpt_parse(struct gendisk *gd) {
    if (gd == NULL || gd->raw == NULL)
        return -EINVAL;

    blkdev_t *bdev = gd->raw;
    int ret = 0;
    int found = 0;

    /* Allocate a temp page for reading GPT header (LBA 1) */
    page_t *hdr_page = __page_alloc(0, PAGE_TYPE_ANON);
    if (hdr_page == NULL)
        return -ENOMEM;
    void *hdr_va = (void *)__page_to_pa(hdr_page);
    memset(hdr_va, 0, PGSIZE);

    /* Read LBA 1 (512 bytes at offset 512 within the page is fine,
     * but we read from sector 1 into the page start) */
    ret = gendisk_read_sectors(bdev, GPT_HEADER_LBA, hdr_page, BLK_SIZE, 0);
    if (ret != 0) {
        printf("gpt: disk%d: failed to read GPT header: %d\n",
               gd->disk_num, ret);
        goto out_free_hdr;
    }

    struct gpt_header *ghdr = (struct gpt_header *)hdr_va;

    /* Validate signature */
    if (ghdr->signature != GPT_SIGNATURE) {
        printf("gpt: disk%d: invalid GPT signature\n", gd->disk_num);
        ret = 0; /* not an error — just no GPT */
        goto out_free_hdr;
    }

    /* Validate header CRC32 */
    uint32 saved_crc = ghdr->header_crc32;
    ghdr->header_crc32 = 0;
    uint32 computed_crc = crc32(0, ghdr, ghdr->header_size);
    ghdr->header_crc32 = saved_crc;

    if (computed_crc != saved_crc) {
        printf("gpt: disk%d: header CRC mismatch (got 0x%x, expected 0x%x)\n",
               gd->disk_num, computed_crc, saved_crc);
        ret = -EIO;
        goto out_free_hdr;
    }

    uint32 num_entries = ghdr->num_part_entries;
    uint32 entry_size = ghdr->part_entry_size;
    uint64 entry_lba = ghdr->part_entry_lba;
    uint32 expected_array_crc = ghdr->part_array_crc32;

    if (num_entries > GPT_MAX_ENTRIES)
        num_entries = GPT_MAX_ENTRIES;
    if (entry_size < sizeof(struct gpt_entry))
        entry_size = sizeof(struct gpt_entry);

    printf("gpt: disk%d: %d partition entries at LBA %ld (entry size %d)\n",
           gd->disk_num, num_entries, entry_lba, entry_size);

    /* Calculate how many pages we need to read for the partition array */
    uint64 array_bytes = (uint64)num_entries * entry_size;
    uint64 array_pages = (array_bytes + PGSIZE - 1) / PGSIZE;

    /* Use a simple static allocation approach — read page by page */
    page_t *epage = __page_alloc(0, PAGE_TYPE_ANON);
    if (epage == NULL) {
        ret = -ENOMEM;
        goto out_free_hdr;
    }
    void *entry_va = (void *)__page_to_pa(epage);

    /* CRC32 over the entire partition array */
    uint32 array_crc = 0;
    int part_num = 0;
    uint64 sectors_per_page = PGSIZE / BLK_SIZE;

    for (uint64 page_idx = 0; page_idx < array_pages; page_idx++) {
        uint64 read_lba = entry_lba + page_idx * sectors_per_page;
        uint16 read_len = PGSIZE;
        uint64 remaining = array_bytes - page_idx * PGSIZE;
        if (remaining < PGSIZE)
            read_len = (uint16)remaining;

        memset(entry_va, 0, PGSIZE);
        ret = gendisk_read_sectors(bdev, read_lba, epage, read_len, 0);
        if (ret != 0) {
            printf("gpt: disk%d: failed to read partition entries at LBA %ld\n",
                   gd->disk_num, read_lba);
            goto out_free_entries;
        }

        /* Accumulate CRC over the raw bytes (including padding) */
        uint64 crc_bytes = remaining < PGSIZE ? remaining : PGSIZE;
        array_crc = crc32(array_crc, entry_va, crc_bytes);

        /* Parse entries in this page */
        uint64 entries_in_page = PGSIZE / entry_size;
        uint64 first_entry = page_idx * entries_in_page;

        for (uint64 i = 0; i < entries_in_page && (first_entry + i) < num_entries; i++) {
            struct gpt_entry *ge =
                (struct gpt_entry *)((uint8 *)entry_va + i * entry_size);

            /* Skip empty (type GUID all zeros) */
            if (gpt_guid_is_zero(&ge->type_guid))
                continue;

            part_num++;

            if (ge->start_lba == 0 || ge->end_lba < ge->start_lba)
                continue;

            uint64 nsectors = ge->end_lba - ge->start_lba + 1;
            printf("gpt: disk%d: partition %d: start=%ld end=%ld sectors=%ld\n",
                   gd->disk_num, part_num, ge->start_lba, ge->end_lba, nsectors);

            int add_ret = gendisk_add_part_guid(gd, ge->start_lba, nsectors,
                                                part_num, &ge->type_guid,
                                                &ge->part_guid);
            if (add_ret != 0) {
                printf("gpt: failed to add partition %d: %d\n", part_num, add_ret);
                continue;
            }
            found++;
        }
    }

    /* Validate partition array CRC */
    if (array_crc != expected_array_crc) {
        printf("gpt: disk%d: WARNING: partition array CRC mismatch "
               "(got 0x%x, expected 0x%x) — proceeding anyway\n",
               gd->disk_num, array_crc, expected_array_crc);
    }

    ret = found;

out_free_entries:
    __page_free(epage, 0);
out_free_hdr:
    __page_free(hdr_page, 0);
    return ret;
}
