#ifndef __KERNEL_DEV_CONSOLE_H
#define __KERNEL_DEV_CONSOLE_H

/*
 * Shared /dev/console record-write UAPI.
 *
 * A caller passes one ASCII record ending in LF.  The x86 console expands
 * that LF to CRLF while holding its wire mutex, so a successful request is
 * one serial-atomic record.  This header deliberately has no RISC-V command
 * implementation; the command remains an x86-only console extension.
 */

#ifdef HOST_LIBC_PROGRAM
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t console_u32;
typedef uint64_t console_u64;
#else
#include "types.h"
typedef uint32 console_u32;
typedef uint64 console_u64;
#endif

#define CONSOLE_RECORD_ABI_VERSION 1U
#define CONSOLE_RECORD_MAX_INPUT_BYTES 511U
#define CONSOLE_RECORD_MAX_PHYSICAL_BYTES 512U

struct console_record_write_v1 {
    console_u32 version;
    console_u32 flags;
    console_u64 data_ptr;
    console_u32 data_len;
    console_u32 reserved;
};

/* Keep this ABI fixed-width and naturally aligned on both x86 kernel and
 * host-glibc recorder builds. */
typedef char console_record_write_v1_size_must_be_24[
    sizeof(struct console_record_write_v1) == 24 ? 1 : -1];
typedef char console_record_write_v1_version_offset_must_be_0[
    offsetof(struct console_record_write_v1, version) == 0 ? 1 : -1];
typedef char console_record_write_v1_flags_offset_must_be_4[
    offsetof(struct console_record_write_v1, flags) == 4 ? 1 : -1];
typedef char console_record_write_v1_data_ptr_offset_must_be_8[
    offsetof(struct console_record_write_v1, data_ptr) == 8 ? 1 : -1];
typedef char console_record_write_v1_data_len_offset_must_be_16[
    offsetof(struct console_record_write_v1, data_len) == 16 ? 1 : -1];
typedef char console_record_write_v1_reserved_offset_must_be_20[
    offsetof(struct console_record_write_v1, reserved) == 20 ? 1 : -1];

/* Linux-compatible ioctl encoding, provided locally for the freestanding
 * kernel while using the host definition for HOST_LIBC_PROGRAM builds. */
#ifndef _IOC
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_WRITE 1U
#define _IOC(dir, type, nr, size)                                             \
    (((unsigned long)(dir) << _IOC_DIRSHIFT) |                                \
     ((unsigned long)(type) << _IOC_TYPESHIFT) |                              \
     ((unsigned long)(nr) << _IOC_NRSHIFT) |                                  \
     ((unsigned long)(size) << _IOC_SIZESHIFT))
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#endif

#define CONSOLE_RECORD_IOC_MAGIC 0x59U /* 'Y' */
#define CONSOLE_RECORD_IOC_WRITE_NR 0x01U
#define CONSOLE_IOC_WRITE_RECORD                                           \
    _IOW(CONSOLE_RECORD_IOC_MAGIC, CONSOLE_RECORD_IOC_WRITE_NR,             \
         struct console_record_write_v1)

static inline int console_record_wire_text_valid(const char *data,
                                                  console_u32 data_len)
{
    if (data == 0 || data_len == 0 ||
        data_len > CONSOLE_RECORD_MAX_INPUT_BYTES ||
        data_len + 1 > CONSOLE_RECORD_MAX_PHYSICAL_BYTES ||
        data[data_len - 1] != '\n')
        return 0;

    for (console_u32 i = 0; i + 1 < data_len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x20 || c > 0x7e)
            return 0;
    }
    return 1;
}

#endif /* __KERNEL_DEV_CONSOLE_H */
