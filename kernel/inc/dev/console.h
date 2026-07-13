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

/*
 * C1 V2 emits one complete BEGIN/META/CHUNK/END envelope as a batch so
 * ordinary console writers cannot enter between its records.  These bounds
 * are the exact reviewed maximum envelope: 701 LF-terminated logical records
 * consume at most 357080 input bytes and 357781 bytes after LF-to-CRLF
 * expansion.
 */
#define CONSOLE_RECORD_BATCH_ABI_VERSION 1U
#define CONSOLE_RECORD_BATCH_F_TERMINAL_COMMIT 1U
#define CONSOLE_RECORD_BATCH_MAX_RECORDS 701U
#define CONSOLE_RECORD_BATCH_MAX_LOGICAL_BYTES 357080U
#define CONSOLE_RECORD_BATCH_MAX_PHYSICAL_BYTES 357781U
#define CONSOLE_RECORD_TERMINAL_MARKER_MAX_BYTES 64U
#define CONSOLE_RECORD_TERMINAL_MAX_LOGICAL_BYTES 141U

struct console_record_write_v1 {
    console_u32 version;
    console_u32 flags;
    console_u64 data_ptr;
    console_u32 data_len;
    console_u32 reserved;
};

struct console_record_batch_write_v1 {
    console_u32 version;
    console_u32 flags;
    console_u64 data_ptr;
    console_u32 data_len;
    console_u32 record_count;
    console_u32 reserved0;
    console_u32 reserved1;
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

typedef char console_record_batch_write_v1_size_must_be_32[
    sizeof(struct console_record_batch_write_v1) == 32 ? 1 : -1];
typedef char console_record_batch_write_v1_version_offset_must_be_0[
    offsetof(struct console_record_batch_write_v1, version) == 0 ? 1 : -1];
typedef char console_record_batch_write_v1_flags_offset_must_be_4[
    offsetof(struct console_record_batch_write_v1, flags) == 4 ? 1 : -1];
typedef char console_record_batch_write_v1_data_ptr_offset_must_be_8[
    offsetof(struct console_record_batch_write_v1, data_ptr) == 8 ? 1 : -1];
typedef char console_record_batch_write_v1_data_len_offset_must_be_16[
    offsetof(struct console_record_batch_write_v1, data_len) == 16 ? 1 : -1];
typedef char console_record_batch_write_v1_record_count_offset_must_be_20[
    offsetof(struct console_record_batch_write_v1, record_count) == 20 ? 1 :
                                                                         -1];
typedef char console_record_batch_write_v1_reserved0_offset_must_be_24[
    offsetof(struct console_record_batch_write_v1, reserved0) == 24 ? 1 : -1];
typedef char console_record_batch_write_v1_reserved1_offset_must_be_28[
    offsetof(struct console_record_batch_write_v1, reserved1) == 28 ? 1 : -1];

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
#define CONSOLE_RECORD_IOC_WRITE_BATCH_NR 0x02U
#define CONSOLE_IOC_WRITE_RECORD                                           \
    _IOW(CONSOLE_RECORD_IOC_MAGIC, CONSOLE_RECORD_IOC_WRITE_NR,             \
         struct console_record_write_v1)
#define CONSOLE_IOC_WRITE_RECORD_BATCH                                     \
    _IOW(CONSOLE_RECORD_IOC_MAGIC, CONSOLE_RECORD_IOC_WRITE_BATCH_NR,       \
         struct console_record_batch_write_v1)

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

static inline int
console_record_batch_wire_text_valid(const char *data, console_u32 data_len,
                                     console_u32 record_count)
{
    console_u32 record_start = 0;
    console_u32 records = 0;

    if (data == 0 || data_len == 0 ||
        data_len > CONSOLE_RECORD_BATCH_MAX_LOGICAL_BYTES ||
        record_count == 0 ||
        record_count > CONSOLE_RECORD_BATCH_MAX_RECORDS ||
        (console_u64)data_len + (console_u64)record_count >
            CONSOLE_RECORD_BATCH_MAX_PHYSICAL_BYTES)
        return 0;

    for (console_u32 i = 0; i < data_len; i++) {
        unsigned char c = (unsigned char)data[i];

        if (c == '\n') {
            console_u32 record_len = i - record_start + 1;
            if (record_len == 1 ||
                record_len > CONSOLE_RECORD_MAX_INPUT_BYTES ||
                !console_record_wire_text_valid(data + record_start,
                                                record_len))
                return 0;
            records++;
            if (records > record_count)
                return 0;
            record_start = i + 1;
        } else if (c == '\r' || c == '\0' || c < 0x20 || c > 0x7e) {
            return 0;
        }
    }

    return record_start == data_len && records == record_count;
}

/*
 * The terminal-commit flag deliberately narrows the generic batch grammar to
 * two exact records.  The optional expected marker is used by the recorder;
 * the kernel passes no expected marker and instead proves that both rows use
 * the same bounded alphanumeric marker.
 */
static inline int console_record_terminal_commit_wire_text_valid(
    const char *data, console_u32 data_len, console_u32 record_count,
    const char *expected_marker, console_u32 expected_marker_len,
    console_u32 *rc_record_len_out)
{
    console_u32 marker_len;
    console_u32 rc_record_len;

    if (data == 0 || record_count != 2 || data_len < 15 ||
        data_len > CONSOLE_RECORD_TERMINAL_MAX_LOGICAL_BYTES)
        return 0;

    rc_record_len = 0;
    while (rc_record_len < data_len && data[rc_record_len] != '\n')
        rc_record_len++;
    if (rc_record_len == data_len)
        return 0;
    rc_record_len++;
    if (rc_record_len < 7)
        return 0;
    marker_len = rc_record_len - 6;
    if (marker_len == 0 ||
        marker_len > CONSOLE_RECORD_TERMINAL_MARKER_MAX_BYTES ||
        data_len != marker_len * 2 + 13)
        return 0;

    for (console_u32 i = 0; i < marker_len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z')))
            return 0;
    }
    if (data[marker_len] != ':' || data[marker_len + 1] != 'R' ||
        data[marker_len + 2] != 'C' || data[marker_len + 3] != ':' ||
        data[marker_len + 4] != '0' || data[marker_len + 5] != '\n')
        return 0;
    for (console_u32 i = 0; i < marker_len; i++) {
        if (data[rc_record_len + i] != data[i])
            return 0;
    }
    if (data[rc_record_len + marker_len] != ':' ||
        data[rc_record_len + marker_len + 1] != 'F' ||
        data[rc_record_len + marker_len + 2] != 'E' ||
        data[rc_record_len + marker_len + 3] != 'N' ||
        data[rc_record_len + marker_len + 4] != 'C' ||
        data[rc_record_len + marker_len + 5] != 'E' ||
        data[rc_record_len + marker_len + 6] != '\n')
        return 0;

    if (expected_marker != 0) {
        if (expected_marker_len != marker_len)
            return 0;
        for (console_u32 i = 0; i < marker_len; i++) {
            if (expected_marker[i] != data[i])
                return 0;
        }
    } else if (expected_marker_len != 0) {
        return 0;
    }

    if (rc_record_len_out != 0)
        *rc_record_len_out = rc_record_len;
    return 1;
}

#endif /* __KERNEL_DEV_CONSOLE_H */
