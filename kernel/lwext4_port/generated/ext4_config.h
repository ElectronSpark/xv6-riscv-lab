/**
 * @file ext4_config.h (generated)
 * @brief lwext4 configuration overrides for xv6 kernel port.
 *
 * Included by lwext4's ext4_config.h when CONFIG_USE_DEFAULT_CFG=0.
 * Only macros that differ from defaults are defined here; all others
 * fall through to the #ifndef guards in the upstream header.
 */
#ifndef EXT4_CONFIG_GENERATED_H
#define EXT4_CONFIG_GENERATED_H

/* Disable journaling for initial port (ext2/ext3-compat).
 * ext4 journal recovery is tightly coupled with lwext4's mountpoint
 * system which we bypass. */
#define CONFIG_JOURNALING_ENABLE  0

/* Disable extended attributes to simplify the initial port. */
#define CONFIG_XATTR_ENABLE       0

/* Use lwext4's own errno definitions — avoids pulling in a system
 * errno.h (which doesn't exist in freestanding kernel mode). */
#define CONFIG_HAVE_OWN_ERRNO     1

/* Use lwext4's own open-flag definitions. */
#define CONFIG_HAVE_OWN_OFLAGS    1

/* Use lwext4's own assert implementation. */
#define CONFIG_HAVE_OWN_ASSERT    1

/* Enable debug printf (uses kernel's printf via compat shim). */
#define CONFIG_DEBUG_PRINTF       1
#define CONFIG_DEBUG_ASSERT       1

/* Block-device cache: 32 entries for better performance. */
#define CONFIG_BLOCK_DEV_CACHE_SIZE  32

/* Disable block-device statistics counters (saves a few cycles). */
#define CONFIG_BLOCK_DEV_ENABLE_STATS 0

/* We provide malloc/free via compat/stdlib.h shim (kmm_alloc/kmm_free). */
#define CONFIG_USE_USER_MALLOC    0

#endif /* EXT4_CONFIG_GENERATED_H */
