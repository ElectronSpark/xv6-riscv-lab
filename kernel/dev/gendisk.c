/*
 * gendisk — generic disk layer implementation.
 *
 * Wraps a raw blkdev_t, probes for GPT/MBR partitions, and creates
 * child blkdev_t sub-devices for each partition.  Each partition's
 * submit_bio transparently adds the LBA start offset before
 * dispatching to the parent device.
 */

#include <types.h>
#include <param.h>
#include "riscv.h"
#include "defs.h"
#include "string.h"
#include "printf.h"
#include <mm/page.h>
#include <mm/slab.h>
#include <dev/bio.h>
#include <dev/blkdev.h>
#include <dev/dev.h>
#include <dev/gendisk.h>
#include <dev/gpt.h>
#include <dev/mbr.h>
#include <lock/spinlock.h>
#include <list.h>
#include <errno.h>
#include <vfs/stat.h>

/*****************************************************************************
 * Global state
 *****************************************************************************/
static list_node_t __gendisk_list = {&__gendisk_list, &__gendisk_list};
static spinlock_t  __gendisk_lock = SPINLOCK_INITIALIZED("gendisk_lock");
static slab_cache_t __gendisk_cache = {0};
static slab_cache_t __gendisk_part_cache = {0};
static int __gendisk_next_num = 0;
static int __gendisk_initialised = 0;

/*****************************************************************************
 * Partition blkdev_t ops  — thin shim that offsets the bio
 *****************************************************************************/

static int __part_open(blkdev_t *bdev) {
    (void)bdev;
    return 0;
}

static int __part_release(blkdev_t *bdev) {
    (void)bdev;
    return 0;
}

static int __part_submit_bio(blkdev_t *bdev, struct bio *bio) {
    struct gendisk_part *part =
        container_of(bdev, struct gendisk_part, blkdev);

    /* Bounds-check: bio must not extend past partition end */
    uint64 bio_sectors = ((uint64)bio->size + BLK_SIZE - 1) >> BLK_SIZE_SHIFT;
    if (bio->blkno + bio_sectors > part->num_sectors) {
        printf("gendisk: %s: bio out of bounds (blkno=%ld, sectors=%ld, "
               "part_sectors=%ld)\n",
               part->devname, bio->blkno, bio_sectors, part->num_sectors);
        bio->error = -EINVAL;
        bio_complete(bio);
        return -EINVAL;
    }

    /* Translate block number by partition start offset */
    bio->blkno += part->start_lba;
    bio->bdev = part->parent->raw;

    /* Dispatch to parent's raw device — bypass blkdev_submit_bio
     * validation because we've already translated blkno and bdev. */
    return part->parent->raw->ops.submit_bio(part->parent->raw, bio);
}

static blkdev_ops_t __part_ops = {
    .open       = __part_open,
    .release    = __part_release,
    .submit_bio = __part_submit_bio,
};

/*****************************************************************************
 * Helper: format partition device name  "disk0p1"
 *****************************************************************************/
static void __format_part_name(char *buf, size_t bufsz,
                               const char *parent_name, int part_num) {
    /* Manual string formatting — no snprintf in kernel core */
    size_t plen = 0;
    while (parent_name[plen] && plen < bufsz - 4)
        plen++;

    size_t pos = 0;
    for (size_t i = 0; i < plen && pos < bufsz - 1; i++)
        buf[pos++] = parent_name[i];

    if (pos < bufsz - 1)
        buf[pos++] = 'p';

    /* Convert part_num to decimal */
    char numstr[8];
    int nlen = 0;
    int n = part_num;
    if (n == 0) {
        numstr[nlen++] = '0';
    } else {
        while (n > 0 && nlen < 7) {
            numstr[nlen++] = '0' + (n % 10);
            n /= 10;
        }
    }
    /* Reverse */
    for (int i = nlen - 1; i >= 0 && pos < bufsz - 1; i--)
        buf[pos++] = numstr[i];

    buf[pos] = '\0';
}

/*****************************************************************************
 * BIO helper: synchronous sector read
 *****************************************************************************/
int gendisk_read_sectors(blkdev_t *bdev, uint64 blkno, page_t *page,
                         uint16 len, uint16 offset) {
    struct bio *bio = bio_alloc(bdev, 1, false, NULL, NULL);
    if (IS_ERR_OR_NULL(bio))
        return -ENOMEM;

    bio->blkno = blkno;
    int ret = bio_add_seg(bio, page, 0, len, offset);
    if (ret != 0) {
        bio_release(bio);
        return ret;
    }

    ret = blkdev_submit_bio(bdev, bio);
    if (ret != 0) {
        bio_release(bio);
        return ret;
    }

    ret = bio_await(bio);
    bio_release(bio);
    return ret;
}

/*****************************************************************************
 * Subsystem init
 *****************************************************************************/
void gendisk_init(void) {
    if (__gendisk_initialised)
        return;

    int ret;
    ret = slab_cache_init(&__gendisk_cache, "gendisk",
                          sizeof(struct gendisk), 0);
    assert(ret == 0, "gendisk_init: gendisk slab init failed");

    ret = slab_cache_init(&__gendisk_part_cache, "gendisk_part",
                          sizeof(struct gendisk_part), 0);
    assert(ret == 0, "gendisk_init: gendisk_part slab init failed");

    __gendisk_initialised = 1;
}

/*****************************************************************************
 * Add a partition to an existing gendisk
 *****************************************************************************/
int gendisk_add_part(struct gendisk *gd, uint64 start_lba,
                     uint64 num_sectors, int part_num) {
    if (gd == NULL || part_num < 1 || part_num > GENDISK_MAX_PARTS)
        return -EINVAL;

    if (gd->nr_parts >= GENDISK_MAX_PARTS)
        return -ENOSPC;

    struct gendisk_part *part = slab_alloc(&__gendisk_part_cache);
    if (part == NULL)
        return -ENOMEM;

    memset(part, 0, sizeof(*part));
    part->parent = gd;
    part->start_lba = start_lba;
    part->num_sectors = num_sectors;
    part->part_num = part_num;

    /* Format device name */
    __format_part_name(part->devname, sizeof(part->devname),
                       gd->raw->dev.devname ? gd->raw->dev.devname : "disk",
                       part_num);

    /* Set up the partition blkdev */
    memset(&part->blkdev, 0, sizeof(part->blkdev));
    part->blkdev.dev.major = gd->raw->dev.major;
    /* Minor = parent disk minor + partition number (Linux convention) */
    part->blkdev.dev.minor = gd->raw->dev.minor + part_num;
    part->blkdev.dev.devname = part->devname;
    part->blkdev.dev.devmode = S_IFBLK | 0600;
    part->blkdev.readable = gd->raw->readable;
    part->blkdev.writable = gd->raw->writable;
    part->blkdev.block_shift = gd->raw->block_shift;
    part->blkdev.ops = __part_ops;

    int ret = blkdev_register(&part->blkdev);
    if (ret != 0) {
        printf("gendisk: failed to register %s: %d\n", part->devname, ret);
        slab_free(part);
        return ret;
    }

    /* Store in gendisk's partition array */
    spin_lock(&gd->lock);
    if (part_num - 1 < GENDISK_MAX_PARTS) {
        gd->parts[part_num - 1] = part;
    }
    gd->nr_parts++;
    spin_unlock(&gd->lock);

    printf("gendisk: registered /dev/%s (start=%ld, sectors=%ld, "
           "major=%d, minor=%d)\n",
           part->devname, start_lba, num_sectors,
           part->blkdev.dev.major, part->blkdev.dev.minor);

    return 0;
}

/*****************************************************************************
 * Add a partition with GPT GUID information
 *****************************************************************************/
int gendisk_add_part_guid(struct gendisk *gd, uint64 start_lba,
                          uint64 num_sectors, int part_num,
                          const struct gpt_guid *type_guid,
                          const struct gpt_guid *part_guid) {
    int ret = gendisk_add_part(gd, start_lba, num_sectors, part_num);
    if (ret != 0)
        return ret;

    /* Store GUIDs in the just-added partition */
    spin_lock(&gd->lock);
    if (part_num - 1 < GENDISK_MAX_PARTS && gd->parts[part_num - 1] != NULL) {
        struct gendisk_part *part = gd->parts[part_num - 1];
        if (type_guid != NULL)
            memmove(&part->type_guid, type_guid, sizeof(struct gpt_guid));
        if (part_guid != NULL)
            memmove(&part->part_guid, part_guid, sizeof(struct gpt_guid));
        part->has_guid = 1;
    }
    spin_unlock(&gd->lock);
    return 0;
}

/*****************************************************************************
 * GUID string parsing: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> gpt_guid
 *****************************************************************************/
static int __hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int __parse_hex_bytes(const char *s, uint8 *out, int nbytes) {
    for (int i = 0; i < nbytes; i++) {
        int hi = __hex_digit(s[i * 2]);
        int lo = __hex_digit(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8)((hi << 4) | lo);
    }
    return 0;
}

int gendisk_guid_parse(const char *str, struct gpt_guid *out) {
    /* Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars) */
    if (str == NULL || out == NULL) return -1;

    /* Validate length and dashes */
    int len = 0;
    while (str[len] != '\0') len++;
    if (len != 36) return -1;
    if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-')
        return -1;

    uint8 bytes[16];

    /* data1: 8 hex chars (4 bytes), stored little-endian */
    if (__parse_hex_bytes(str, bytes, 4) < 0) return -1;
    out->data1 = ((uint32)bytes[0] << 24) | ((uint32)bytes[1] << 16) |
                 ((uint32)bytes[2] << 8) | bytes[3];

    /* data2: 4 hex chars (2 bytes) */
    if (__parse_hex_bytes(str + 9, bytes, 2) < 0) return -1;
    out->data2 = ((uint16)bytes[0] << 8) | bytes[1];

    /* data3: 4 hex chars (2 bytes) */
    if (__parse_hex_bytes(str + 14, bytes, 2) < 0) return -1;
    out->data3 = ((uint16)bytes[0] << 8) | bytes[1];

    /* data4[0..1]: 4 hex chars (2 bytes) */
    if (__parse_hex_bytes(str + 19, bytes, 2) < 0) return -1;
    out->data4[0] = bytes[0];
    out->data4[1] = bytes[1];

    /* data4[2..7]: 12 hex chars (6 bytes) */
    if (__parse_hex_bytes(str + 24, bytes, 6) < 0) return -1;
    for (int i = 0; i < 6; i++)
        out->data4[2 + i] = bytes[i];

    return 0;
}

/*****************************************************************************
 * GUID formatting: gpt_guid -> "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 *****************************************************************************/
static const char __hex_chars[] = "0123456789abcdef";

void gendisk_guid_format(const struct gpt_guid *guid, char *buf, size_t bufsz) {
    if (bufsz < 37) {
        if (bufsz > 0) buf[0] = '\0';
        return;
    }
    int pos = 0;

    /* data1: 4 bytes -> 8 hex */
    buf[pos++] = __hex_chars[(guid->data1 >> 28) & 0xf];
    buf[pos++] = __hex_chars[(guid->data1 >> 24) & 0xf];
    buf[pos++] = __hex_chars[(guid->data1 >> 20) & 0xf];
    buf[pos++] = __hex_chars[(guid->data1 >> 16) & 0xf];
    buf[pos++] = __hex_chars[(guid->data1 >> 12) & 0xf];
    buf[pos++] = __hex_chars[(guid->data1 >> 8) & 0xf];
    buf[pos++] = __hex_chars[(guid->data1 >> 4) & 0xf];
    buf[pos++] = __hex_chars[guid->data1 & 0xf];
    buf[pos++] = '-';

    /* data2: 2 bytes -> 4 hex */
    buf[pos++] = __hex_chars[(guid->data2 >> 12) & 0xf];
    buf[pos++] = __hex_chars[(guid->data2 >> 8) & 0xf];
    buf[pos++] = __hex_chars[(guid->data2 >> 4) & 0xf];
    buf[pos++] = __hex_chars[guid->data2 & 0xf];
    buf[pos++] = '-';

    /* data3: 2 bytes -> 4 hex */
    buf[pos++] = __hex_chars[(guid->data3 >> 12) & 0xf];
    buf[pos++] = __hex_chars[(guid->data3 >> 8) & 0xf];
    buf[pos++] = __hex_chars[(guid->data3 >> 4) & 0xf];
    buf[pos++] = __hex_chars[guid->data3 & 0xf];
    buf[pos++] = '-';

    /* data4[0..1]: 2 bytes -> 4 hex */
    buf[pos++] = __hex_chars[(guid->data4[0] >> 4) & 0xf];
    buf[pos++] = __hex_chars[guid->data4[0] & 0xf];
    buf[pos++] = __hex_chars[(guid->data4[1] >> 4) & 0xf];
    buf[pos++] = __hex_chars[guid->data4[1] & 0xf];
    buf[pos++] = '-';

    /* data4[2..7]: 6 bytes -> 12 hex */
    for (int i = 2; i < 8; i++) {
        buf[pos++] = __hex_chars[(guid->data4[i] >> 4) & 0xf];
        buf[pos++] = __hex_chars[guid->data4[i] & 0xf];
    }
    buf[pos] = '\0';
}

/*****************************************************************************
 * GUID comparison helper
 *****************************************************************************/
static int __guid_equal(const struct gpt_guid *a, const struct gpt_guid *b) {
    return a->data1 == b->data1 && a->data2 == b->data2 &&
           a->data3 == b->data3 &&
           a->data4[0] == b->data4[0] && a->data4[1] == b->data4[1] &&
           a->data4[2] == b->data4[2] && a->data4[3] == b->data4[3] &&
           a->data4[4] == b->data4[4] && a->data4[5] == b->data4[5] &&
           a->data4[6] == b->data4[6] && a->data4[7] == b->data4[7];
}

/*****************************************************************************
 * Find a partition blkdev by its unique partition GUID
 *****************************************************************************/
blkdev_t *gendisk_find_by_guid(const struct gpt_guid *guid) {
    if (guid == NULL || gpt_guid_is_zero(guid))
        return NULL;

    blkdev_t *found = NULL;

    spin_lock(&__gendisk_lock);
    list_node_t *node;
    list_foreach_entry(&__gendisk_list, node) {
        struct gendisk *gd = container_of(node, struct gendisk, entry);
        spin_lock(&gd->lock);
        for (int i = 0; i < GENDISK_MAX_PARTS; i++) {
            struct gendisk_part *part = gd->parts[i];
            if (part == NULL || !part->has_guid)
                continue;
            if (__guid_equal(&part->part_guid, guid)) {
                found = &part->blkdev;
                spin_unlock(&gd->lock);
                goto out;
            }
        }
        spin_unlock(&gd->lock);
    }
out:
    spin_unlock(&__gendisk_lock);
    return found;
}


/*****************************************************************************
 * Probe a raw blkdev for partitions
 *****************************************************************************/
struct gendisk *gendisk_probe(blkdev_t *raw) {
    if (raw == NULL)
        return ERR_PTR(-EINVAL);

    if (!__gendisk_initialised)
        gendisk_init();

    /* Allocate gendisk */
    struct gendisk *gd = slab_alloc(&__gendisk_cache);
    if (gd == NULL)
        return ERR_PTR(-ENOMEM);

    memset(gd, 0, sizeof(*gd));
    gd->raw = raw;
    gd->disk_num = __gendisk_next_num++;
    spin_init(&gd->lock, "gendisk");
    list_entry_init(&gd->entry);

    /* Add to global disk list */
    spin_lock(&__gendisk_lock);
    list_entry_push(&__gendisk_list, &gd->entry);
    spin_unlock(&__gendisk_lock);

    printf("gendisk: probing disk%d (/dev/%s) for partitions\n",
           gd->disk_num, raw->dev.devname ? raw->dev.devname : "?");

    /* Read LBA 0 to check for partition table */
    page_t *lba0_page = __page_alloc(0, PAGE_TYPE_ANON);
    if (lba0_page == NULL) {
        printf("gendisk: failed to allocate page for LBA 0 read\n");
        return gd; /* return gendisk without partitions */
    }

    void *lba0_va = (void *)__page_to_pa(lba0_page);
    memset(lba0_va, 0, PGSIZE);

    int ret = gendisk_read_sectors(raw, 0, lba0_page, BLK_SIZE, 0);
    if (ret != 0) {
        printf("gendisk: failed to read LBA 0: %d\n", ret);
        __page_free(lba0_page, 0);
        return gd;
    }

    /* Check for GPT (protective MBR + GPT header) */
    struct mbr_sector *mbr = (struct mbr_sector *)lba0_va;
    if (mbr->signature == MBR_SIGNATURE && mbr_is_gpt_protective(mbr)) {
        printf("gendisk: disk%d: GPT protective MBR detected, trying GPT\n",
               gd->disk_num);
        int gpt_ret = gpt_parse(gd);
        if (gpt_ret > 0) {
            printf("gendisk: disk%d: found %d GPT partitions\n",
                   gd->disk_num, gpt_ret);
            __page_free(lba0_page, 0);
            return gd;
        }
        if (gpt_ret < 0)
            printf("gendisk: disk%d: GPT parse error %d, falling back to MBR\n",
                   gd->disk_num, gpt_ret);
    }

    /* Try MBR */
    if (mbr->signature == MBR_SIGNATURE) {
        int mbr_ret = mbr_parse(gd, lba0_va);
        if (mbr_ret > 0)
            printf("gendisk: disk%d: found %d MBR partitions\n",
                   gd->disk_num, mbr_ret);
        else if (mbr_ret == 0)
            printf("gendisk: disk%d: no partitions found in MBR\n",
                   gd->disk_num);
    } else {
        printf("gendisk: disk%d: no partition table found\n", gd->disk_num);
    }

    __page_free(lba0_page, 0);
    return gd;
}

/*****************************************************************************
 * Remove a gendisk and all partitions
 *****************************************************************************/
void gendisk_remove(struct gendisk *gd) {
    if (gd == NULL)
        return;

    /* Remove from global list */
    spin_lock(&__gendisk_lock);
    list_entry_detach(&gd->entry);
    spin_unlock(&__gendisk_lock);

    /* Unregister all partition blkdevs */
    spin_lock(&gd->lock);
    for (int i = 0; i < GENDISK_MAX_PARTS; i++) {
        struct gendisk_part *part = gd->parts[i];
        if (part == NULL)
            continue;
        gd->parts[i] = NULL;
        spin_unlock(&gd->lock);

        printf("gendisk: unregistering /dev/%s\n", part->devname);
        blkdev_unregister(&part->blkdev);
        slab_free(part);

        spin_lock(&gd->lock);
    }
    gd->nr_parts = 0;
    spin_unlock(&gd->lock);

    slab_free(gd);
}

/*****************************************************************************
 * sys_dumpblk — print block device / partition information to console
 *
 * mode 0: compact listing (lsblk-style)
 * mode 1: detailed listing with partition table info
 *****************************************************************************/

static void __print_size_human(uint64 bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        uint64 gb = bytes / (1024ULL * 1024 * 1024);
        uint64 mb_frac = (bytes % (1024ULL * 1024 * 1024)) / (1024ULL * 1024);
        printf("%ld.%ldG", gb, mb_frac * 10 / 1024);
    } else if (bytes >= 1024ULL * 1024) {
        uint64 mb = bytes / (1024ULL * 1024);
        uint64 kb_frac = (bytes % (1024ULL * 1024)) / 1024;
        printf("%ld.%ldM", mb, kb_frac * 10 / 1024);
    } else if (bytes >= 1024) {
        printf("%ldK", bytes / 1024);
    } else {
        printf("%ldB", bytes);
    }
}

static void __dump_gendisk_compact(void) {
    printf("NAME          MAJ:MIN  SIZE       TYPE  PARTITIONS\n");

    spin_lock(&__gendisk_lock);
    list_node_t *node;
    list_foreach_entry(&__gendisk_list, node) {
        struct gendisk *gd = container_of(node, struct gendisk, entry);
        spin_lock(&gd->lock);

        /* Print disk line */
        printf("%-13s %3d:%-3d  ",
               gd->raw->dev.devname ? gd->raw->dev.devname : "?",
               gd->raw->dev.major, gd->raw->dev.minor);

        /* Disk size: we don't store total sectors, so show "?" */
        printf("%-10s ", "-");
        printf("disk  %d\n", gd->nr_parts);

        /* Print partition lines */
        for (int i = 0; i < GENDISK_MAX_PARTS; i++) {
            struct gendisk_part *part = gd->parts[i];
            if (part == NULL)
                continue;
            uint64 part_bytes = part->num_sectors * BLK_SIZE;
            printf("  %-11s %3d:%-3d  ", part->devname,
                   part->blkdev.dev.major, part->blkdev.dev.minor);
            __print_size_human(part_bytes);
            /* Pad to 10 chars */
            printf("%-5s", "");
            printf("part\n");
        }

        spin_unlock(&gd->lock);
    }
    spin_unlock(&__gendisk_lock);
}

static void __dump_gendisk_detailed(void) {
    spin_lock(&__gendisk_lock);
    list_node_t *node;
    int disk_idx = 0;
    list_foreach_entry(&__gendisk_list, node) {
        struct gendisk *gd = container_of(node, struct gendisk, entry);
        spin_lock(&gd->lock);

        if (disk_idx > 0)
            printf("\n");

        printf("Disk /dev/%s (disk%d):\n",
               gd->raw->dev.devname ? gd->raw->dev.devname : "?",
               gd->disk_num);
        printf("  Major: %d, Minor: %d\n",
               gd->raw->dev.major, gd->raw->dev.minor);
        printf("  Block size: %ld bytes (shift=%d)\n",
               (uint64)BLK_SIZE << gd->raw->block_shift,
               gd->raw->block_shift);
        printf("  Readable: %s, Writable: %s\n",
               gd->raw->readable ? "yes" : "no",
               gd->raw->writable ? "yes" : "no");
        printf("  Partitions: %d\n", gd->nr_parts);

        for (int i = 0; i < GENDISK_MAX_PARTS; i++) {
            struct gendisk_part *part = gd->parts[i];
            if (part == NULL)
                continue;
            uint64 part_bytes = part->num_sectors * BLK_SIZE;
            printf("  Partition %d: /dev/%s\n", part->part_num, part->devname);
            printf("    Major: %d, Minor: %d\n",
                   part->blkdev.dev.major, part->blkdev.dev.minor);
            printf("    Start LBA: %ld, Sectors: %ld, Size: ",
                   part->start_lba, part->num_sectors);
            __print_size_human(part_bytes);
            printf("\n");
            if (part->has_guid) {
                char guid_str[37];
                gendisk_guid_format(&part->part_guid, guid_str, sizeof(guid_str));
                printf("    UUID: %s\n", guid_str);
                gendisk_guid_format(&part->type_guid, guid_str, sizeof(guid_str));
                printf("    Type: %s\n", guid_str);
            }
        }

        spin_unlock(&gd->lock);
        disk_idx++;
    }
    spin_unlock(&__gendisk_lock);

    if (disk_idx == 0)
        printf("No gendisk devices registered.\n");
}

/* Block device enumeration callback for mode 2 */
struct __blkdev_dump_ctx { int count; };

static int __dump_blkdev_cb(device_t *dev, void *ctx) {
    struct __blkdev_dump_ctx *dctx = (struct __blkdev_dump_ctx *)ctx;
    if (dev->type != DEV_TYPE_BLOCK)
        return 0;

    blkdev_t *bdev = (blkdev_t *)dev;
    if (dctx->count == 0)
        printf("NAME          MAJ:MIN  BLK_SIZE  RW\n");

    printf("%-13s %3d:%-3d  %-8ld  %s%s\n",
           dev->devname ? dev->devname : "?",
           dev->major, dev->minor,
           (uint64)BLK_SIZE << bdev->block_shift,
           bdev->readable ? "r" : "-",
           bdev->writable ? "w" : "-");
    dctx->count++;
    return 0;
}

uint64 sys_dumpblk(void) {
    int mode;
    argint(0, &mode);

    switch (mode) {
    case 0:
        __dump_gendisk_compact();
        break;
    case 1:
        __dump_gendisk_detailed();
        break;
    case 2: {
        /* Enumerate all registered block devices */
        struct __blkdev_dump_ctx ctx = {0};
        dev_for_each_device(__dump_blkdev_cb, &ctx);
        if (ctx.count == 0)
            printf("No block devices registered.\n");
        break;
    }
    default:
        __dump_gendisk_compact();
        break;
    }
    return 0;
}
