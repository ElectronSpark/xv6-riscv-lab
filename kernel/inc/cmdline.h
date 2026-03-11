/**
 * @file cmdline.h
 * @brief Kernel command line parsing.
 *
 * Parses the boot command line (from FDT bootargs, QEMU -append,
 * PVH/multiboot cmdline, or U-Boot bootargs) to extract kernel
 * parameters such as root=, rootfstype=, etc.
 *
 * The command line is stored in platform.cmdline by the platform
 * initialization code.
 */

#ifndef __KERNEL_CMDLINE_H
#define __KERNEL_CMDLINE_H

#include "types.h"

/**
 * Parse the "root=" parameter from the kernel command line and return
 * the corresponding block device number.
 *
 * Supported formats:
 *   root=/dev/vdaN    - VirtIO disk (major=2, minor=1+N*16 for disk, +part)
 *   root=/dev/ram     - Ramdisk (major=3, minor=1)
 *   root=/dev/ram0    - Ramdisk (major=3, minor=1)
 *   root=/dev/mmcblkNpM - eMMC/SD (major=4)
 *   root=/dev/sdN     - eMMC/SD (major=4)
 *   root=M:N          - Explicit major:minor
 *
 * @return  the parsed dev_t, or 0 if no root= was specified or could not
 *          be parsed.
 */
dev_t cmdline_get_root_dev(void);

/**
 * Look up a single key=value parameter from the kernel command line.
 *
 * @param key    Parameter name (e.g. "rootfstype")
 * @param buf    Output buffer for the value
 * @param bufsz  Size of buf
 * @return  0 on success, -1 if key not found
 */
int cmdline_get_param(const char *key, char *buf, size_t bufsz);

#endif /* __KERNEL_CMDLINE_H */
