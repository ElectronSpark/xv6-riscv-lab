/**
 * @file cmdline.c
 * @brief Kernel command line parser.
 *
 * Parses boot command line parameters from platform.cmdline.
 * The command line is populated by architecture-specific code:
 *   - RISC-V: FDT /chosen/bootargs  (set by QEMU -append or U-Boot)
 *   - x86_64: PVH cmdline_paddr or multiboot cmdline
 */

#include "types.h"
#include "defs.h"
#include "param.h"
#include "string.h"
#include "printf.h"
#include "dev/fdt.h"
#include "cmdline.h"

/**
 * Look up a single key=value parameter from the kernel command line.
 * Parameters are space-separated.  Value ends at next space or end-of-string.
 *
 * @param key    Parameter name (e.g. "root", "rootfstype")
 * @param buf    Output buffer for the value
 * @param bufsz  Size of buf
 * @return  0 on success, -1 if key not found
 */
int cmdline_get_param(const char *key, char *buf, size_t bufsz)
{
    if (!platform.has_cmdline || key == NULL || buf == NULL || bufsz == 0)
        return -1;

    size_t keylen = strlen(key);
    const char *p = platform.cmdline;

    while (*p) {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        /* check if this token starts with key= */
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            const char *val = p + keylen + 1;
            const char *end = val;
            while (*end && *end != ' ' && *end != '\t')
                end++;
            size_t vlen = (size_t)(end - val);
            if (vlen >= bufsz)
                vlen = bufsz - 1;
            memcpy(buf, val, vlen);
            buf[vlen] = '\0';
            return 0;
        }

        /* skip to next whitespace */
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return -1;
}

/*
 * VirtIO disk naming convention (Linux-compatible):
 *   vda  = disk0 (major=2, minor=1)
 *   vda1 = disk0p1 (major=2, minor=2)
 *   ...
 *   vda15 = disk0p15 (major=2, minor=16)
 *   vdb  = disk1 (major=2, minor=17)
 *   vdb1 = disk1p1 (major=2, minor=18)
 *   ...
 *
 * GENDISK_MINOR_STRIDE = 16 per disk.
 */
#define VIRTIO_MAJOR    2
#define RAMDISK_MAJOR   3
#define SDHCI_MAJOR     4
#define MINOR_STRIDE    16  /* must match GENDISK_MINOR_STRIDE in virtio_disk.c */

/**
 * Parse a VirtIO device name: vda, vdb, ..., vdz, optionally followed by
 * a partition number (1-15).
 *
 * @param name   Points to the character after "/dev/" (e.g. "vda1")
 * @return  dev_t on success, 0 on failure
 */
static dev_t __parse_virtio(const char *name)
{
    if (name[0] != 'v' || name[1] != 'd')
        return 0;
    if (name[2] < 'a' || name[2] > 'z')
        return 0;

    int disk_idx = name[2] - 'a';   /* 0=vda, 1=vdb, ... */
    int base_minor = 1 + disk_idx * MINOR_STRIDE;

    const char *rest = name + 3;
    if (*rest == '\0') {
        /* whole disk: vda, vdb, ... */
        return mkdev(VIRTIO_MAJOR, base_minor);
    }

    /* partition number */
    int part = 0;
    while (*rest >= '0' && *rest <= '9') {
        part = part * 10 + (*rest - '0');
        rest++;
    }
    if (*rest != '\0' || part < 1 || part > 15)
        return 0;

    return mkdev(VIRTIO_MAJOR, base_minor + part);
}

/**
 * Parse an eMMC/SD device name:
 *   mmcblk0      → major=4, minor=1
 *   mmcblk0p1    → major=4, minor=2
 *   mmcblk1      → major=4, minor=1+MINOR_STRIDE
 *   sd0          → major=4, minor=1
 *   sd0p1        → major=4, minor=2
 *
 * @param name   Points to the character after "/dev/"
 * @return  dev_t on success, 0 on failure
 */
static dev_t __parse_mmc(const char *name)
{
    int disk_idx = -1;

    if (str_startswith(name, "mmcblk")) {
        const char *p = name + 6;
        disk_idx = 0;
        while (*p >= '0' && *p <= '9') {
            disk_idx = disk_idx * 10 + (*p - '0');
            p++;
        }
        int base_minor = 1 + disk_idx * MINOR_STRIDE;
        if (*p == '\0')
            return mkdev(SDHCI_MAJOR, base_minor);
        if (*p == 'p') {
            p++;
            int part = 0;
            while (*p >= '0' && *p <= '9') {
                part = part * 10 + (*p - '0');
                p++;
            }
            if (*p != '\0' || part < 1 || part > 15)
                return 0;
            return mkdev(SDHCI_MAJOR, base_minor + part);
        }
        return 0;
    }

    /* sd0, sd1, etc. (xv6 naming) */
    if (name[0] == 's' && name[1] == 'd' && name[2] >= '0' && name[2] <= '9') {
        const char *p = name + 2;
        disk_idx = 0;
        while (*p >= '0' && *p <= '9') {
            disk_idx = disk_idx * 10 + (*p - '0');
            p++;
        }
        int base_minor = disk_idx + 1;
        if (*p == '\0')
            return mkdev(SDHCI_MAJOR, base_minor);
        if (*p == 'p') {
            p++;
            int part = 0;
            while (*p >= '0' && *p <= '9') {
                part = part * 10 + (*p - '0');
                p++;
            }
            if (*p != '\0' || part < 1 || part > 15)
                return 0;
            return mkdev(SDHCI_MAJOR, base_minor + part);
        }
        return 0;
    }

    return 0;
}

/**
 * Parse "root=" value, stripping optional "/dev/" prefix.
 *
 * Supported values:
 *   /dev/vdaN[p]   - VirtIO disk
 *   /dev/ram[0]    - Ramdisk
 *   /dev/ramdisk   - Ramdisk (xv6 native name)
 *   /dev/mmcblkNpM - eMMC/SD
 *   /dev/sdN[pM]   - SD card (xv6 naming)
 *   M:N            - Explicit major:minor
 *
 * Also supports xv6 native names:
 *   /dev/disk0     - VirtIO disk0
 *   /dev/disk0p1   - VirtIO disk0 partition 1
 *   /dev/mmc0      - eMMC (xv6 naming)
 *   /dev/mmc0p1    - eMMC partition 1
 */
dev_t cmdline_get_root_dev(void)
{
    char root_val[64];
    if (cmdline_get_param("root", root_val, sizeof(root_val)) != 0)
        return 0;

    const char *name = root_val;

    /* Try explicit major:minor format first (e.g. "2:1") */
    {
        const char *p = name;
        uint64 maj = 0;
        while (*p >= '0' && *p <= '9') {
            maj = maj * 10 + (*p - '0');
            p++;
        }
        if (*p == ':' && p != name) {
            p++;
            uint64 min = 0;
            const char *q = p;
            while (*p >= '0' && *p <= '9') {
                min = min * 10 + (*p - '0');
                p++;
            }
            if (*p == '\0' && p != q) {
                dev_t dev = mkdev((int)maj, (int)min);
                printf("cmdline: root=%s -> dev(%ld,%ld)\n", root_val, maj, min);
                return dev;
            }
        }
    }

    /* Strip optional "/dev/" prefix */
    if (str_startswith(name, "/dev/"))
        name += 5;

    dev_t dev = 0;

    /* Ramdisk: ram, ram0, ramdisk */
    if (strcmp(name, "ram") == 0 || strcmp(name, "ram0") == 0 ||
        strcmp(name, "ramdisk") == 0) {
        dev = RAMDISK_DEV;
    }
    /* VirtIO: vda, vda1, vdb, vdb1, ... */
    else if (name[0] == 'v' && name[1] == 'd') {
        dev = __parse_virtio(name);
    }
    /* eMMC/SD: mmcblk0, mmcblk0p1, sd0, sd0p1, ... */
    else if (str_startswith(name, "mmcblk") ||
             (name[0] == 's' && name[1] == 'd')) {
        dev = __parse_mmc(name);
    }
    /* xv6 native names: disk0, disk0p1, disk1, ... */
    else if (str_startswith(name, "disk")) {
        const char *p = name + 4;
        int disk_idx = 0;
        while (*p >= '0' && *p <= '9') {
            disk_idx = disk_idx * 10 + (*p - '0');
            p++;
        }
        int base_minor = 1 + disk_idx * MINOR_STRIDE;
        if (*p == '\0') {
            dev = mkdev(VIRTIO_MAJOR, base_minor);
        } else if (*p == 'p') {
            p++;
            int part = 0;
            while (*p >= '0' && *p <= '9') {
                part = part * 10 + (*p - '0');
                p++;
            }
            if (*p == '\0' && part >= 1 && part <= 15)
                dev = mkdev(VIRTIO_MAJOR, base_minor + part);
        }
    }
    /* xv6 native eMMC names: mmc0, mmc0p1, ... */
    else if (str_startswith(name, "mmc")) {
        const char *p = name + 3;
        int disk_idx = 0;
        while (*p >= '0' && *p <= '9') {
            disk_idx = disk_idx * 10 + (*p - '0');
            p++;
        }
        int base_minor = disk_idx + 1;
        if (*p == '\0') {
            dev = mkdev(SDHCI_MAJOR, base_minor);
        } else if (*p == 'p') {
            p++;
            int part = 0;
            while (*p >= '0' && *p <= '9') {
                part = part * 10 + (*p - '0');
                p++;
            }
            if (*p == '\0' && part >= 1 && part <= 15)
                dev = mkdev(SDHCI_MAJOR, base_minor + part);
        }
    }

    if (dev != 0) {
        printf("cmdline: root=%s -> dev(%d,%d)\n",
               root_val, major(dev), minor(dev));
    } else {
        printf("cmdline: unrecognized root=%s\n", root_val);
    }

    return dev;
}
