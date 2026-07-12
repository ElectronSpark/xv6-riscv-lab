#define HOST_LIBC_PROGRAM 1

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dev/console.h"

struct fake_sink {
    char bytes[CONSOLE_RECORD_MAX_PHYSICAL_BYTES + 8];
    unsigned int len;
    unsigned int calls;
};

static void fake_emit_record(struct fake_sink *sink, const char *record,
                             console_u32 len)
{
    sink->calls++;
    for (console_u32 i = 0; i < len; i++) {
        if (record[i] == '\n')
            sink->bytes[sink->len++] = '\r';
        sink->bytes[sink->len++] = record[i];
    }
}

/* Models the x86 handler's ordered contract: exact command and 24-byte
 * request copy, validation and payload copy before timed locking, a locked
 * state recheck, then one contiguous fake-UART emission. */
static int fake_record_ioctl(unsigned long cmd,
                             const struct console_record_write_v1 *request,
                             size_t request_copy_len, const char *record,
                             int root, int payload_copy_ok,
                             int pre_lock_available, int lock_ret,
                             int post_lock_available,
                             unsigned long emergency_before,
                             unsigned long emergency_after,
                             struct fake_sink *sink)
{
    if (cmd != CONSOLE_IOC_WRITE_RECORD)
        return -ENOTTY;
    if (!root)
        return -EPERM;
    if (request == NULL || request_copy_len != sizeof(*request))
        return -EFAULT;
    if (request->version != CONSOLE_RECORD_ABI_VERSION || request->flags != 0 ||
        request->reserved != 0 || request->data_ptr == 0 ||
        request->data_len == 0 ||
        request->data_len > CONSOLE_RECORD_MAX_INPUT_BYTES)
        return -EINVAL;
    if (!payload_copy_ok || record == NULL)
        return -EFAULT;
    if (!console_record_wire_text_valid(record, request->data_len))
        return -EINVAL;
    if (!pre_lock_available)
        return -EAGAIN;
    if (lock_ret != 0)
        return lock_ret;
    if (!post_lock_available)
        return -EAGAIN;

    fake_emit_record(sink, record, request->data_len);
    return emergency_before == emergency_after ? (int)request->data_len
                                               : -EAGAIN;
}

static int check(int condition, const char *label)
{
    if (condition)
        return 1;
    fprintf(stderr, "console-record-host-test: %s\n", label);
    return 0;
}

static int zero_output(const struct fake_sink *sink)
{
    return sink->calls == 0 && sink->len == 0;
}

static struct console_record_write_v1 request_for(const char *record,
                                                  console_u32 len)
{
    return (struct console_record_write_v1){
        .version = CONSOLE_RECORD_ABI_VERSION,
        .flags = 0,
        .data_ptr = (console_u64)(uintptr_t)record,
        .data_len = len,
        .reserved = 0,
    };
}

int main(void)
{
    const char valid[] = "YT_RECORD ok=1\n";
    const char multiple_lf[] = "first\nsecond\n";
    const char missing_final_lf[] = "missing-final-lf";
    const char contains_cr[] = "bad\rrecord\n";
    const char contains_nul[] = {'b', 'a', 'd', '\0', 'x', '\n'};
    char max_record[CONSOLE_RECORD_MAX_INPUT_BYTES];
    struct fake_sink sink = {0};
    struct console_record_write_v1 request =
        request_for(valid, (console_u32)(sizeof(valid) - 1));

    memset(max_record, 'x', sizeof(max_record));
    max_record[sizeof(max_record) - 1] = '\n';

    if (!check(sizeof(request) == 24, "request size") ||
        !check(offsetof(struct console_record_write_v1, version) == 0,
               "version offset") ||
        !check(offsetof(struct console_record_write_v1, flags) == 4,
               "flags offset") ||
        !check(offsetof(struct console_record_write_v1, data_ptr) == 8,
               "data pointer offset") ||
        !check(offsetof(struct console_record_write_v1, data_len) == 16,
               "data length offset") ||
        !check(offsetof(struct console_record_write_v1, reserved) == 20,
               "reserved offset") ||
        !check(_IOC_TYPE(CONSOLE_IOC_WRITE_RECORD) == CONSOLE_RECORD_IOC_MAGIC,
               "ioctl magic") ||
        !check(_IOC_NR(CONSOLE_IOC_WRITE_RECORD) == CONSOLE_RECORD_IOC_WRITE_NR,
               "ioctl number") ||
        !check(_IOC_SIZE(CONSOLE_IOC_WRITE_RECORD) == sizeof(request),
               "ioctl request size") ||
        !check(_IOC_DIR(CONSOLE_IOC_WRITE_RECORD) == _IOC_WRITE,
               "ioctl direction"))
        return 1;

    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 1, 9,
                                 9, &sink) == (int)request.data_len,
               "valid return") ||
        !check(sink.calls == 1, "valid one contiguous fake-sink call") ||
        !check(sink.len == request.data_len + 1,
               "valid LF to CRLF physical length") ||
        !check(memcmp(sink.bytes, "YT_RECORD ok=1\r\n", sink.len) == 0,
               "valid exact bytes"))
        return 1;

    memset(&sink, 0, sizeof(sink));
    request = request_for(max_record, sizeof(max_record));
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), max_record, 1, 1, 1, 0, 1,
                                 0, 0, &sink) == (int)sizeof(max_record),
               "max record return") ||
        !check(sink.calls == 1 && sink.len == CONSOLE_RECORD_MAX_PHYSICAL_BYTES,
               "511 input becomes one contiguous 512-byte CRLF record") ||
        !check(sink.bytes[510] == '\r' && sink.bytes[511] == '\n',
               "max record terminal CRLF"))
        return 1;

    memset(&sink, 0, sizeof(sink));
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request, 23,
                                 max_record, 1, 1, 1, 0, 1, 0, 0, &sink) ==
                   -EFAULT && zero_output(&sink),
               "short request copy is pre-emission fault") ||
        !check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request, 25,
                                 max_record, 1, 1, 1, 0, 1, 0, 0, &sink) ==
                   -EFAULT && zero_output(&sink),
               "oversize request copy is pre-emission fault") ||
        !check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD + 1, &request,
                                 sizeof(request), max_record, 1, 1, 1, 0, 1,
                                 0, 0, &sink) == -ENOTTY && zero_output(&sink),
               "wrong ioctl command has no record output") ||
        !check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, NULL,
                                 sizeof(request), max_record, 1, 1, 1, 0, 1,
                                 0, 0, &sink) == -EFAULT && zero_output(&sink),
               "null request is pre-emission fault") ||
        !check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), max_record, 0, 1, 1, 0, 1,
                                 0, 0, &sink) == -EPERM && zero_output(&sink),
               "privilege is pre-emission failure") ||
        !check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), max_record, 1, 0, 1, 0, 1,
                                 0, 0, &sink) == -EFAULT && zero_output(&sink),
               "payload copy fault is pre-emission failure"))
        return 1;

    request = request_for(valid, (console_u32)(sizeof(valid) - 1));
    request.version++;
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 1, 0, 0,
                                 &sink) == -EINVAL && zero_output(&sink),
               "version failure"))
        return 1;
    request = request_for(valid, (console_u32)(sizeof(valid) - 1));
    request.flags = 1;
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 1, 0, 0,
                                 &sink) == -EINVAL && zero_output(&sink),
               "flags failure"))
        return 1;
    request = request_for(valid, (console_u32)(sizeof(valid) - 1));
    request.reserved = 1;
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 1, 0, 0,
                                 &sink) == -EINVAL && zero_output(&sink),
               "reserved failure"))
        return 1;
    request = request_for(valid, (console_u32)(sizeof(valid) - 1));
    request.data_ptr = 0;
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 1, 0, 0,
                                 &sink) == -EINVAL && zero_output(&sink),
               "null payload pointer failure"))
        return 1;
    request = request_for(valid, 0);
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 1, 0, 0,
                                 &sink) == -EINVAL && zero_output(&sink),
               "zero length failure"))
        return 1;
    request = request_for(valid, CONSOLE_RECORD_MAX_INPUT_BYTES + 1);
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 1, 0, 0,
                                 &sink) == -EINVAL && zero_output(&sink),
               "oversize length failure"))
        return 1;

    request = request_for(multiple_lf, (console_u32)(sizeof(multiple_lf) - 1));
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), multiple_lf, 1, 1, 1, 0, 1,
                                 0, 0, &sink) == -EINVAL && zero_output(&sink),
               "embedded LF failure"))
        return 1;
    request = request_for(missing_final_lf,
                          (console_u32)(sizeof(missing_final_lf) - 1));
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), missing_final_lf, 1, 1, 1,
                                 0, 1, 0, 0, &sink) == -EINVAL &&
                   zero_output(&sink),
               "missing final LF failure"))
        return 1;
    request = request_for(contains_cr, (console_u32)(sizeof(contains_cr) - 1));
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), contains_cr, 1, 1, 1, 0, 1,
                                 0, 0, &sink) == -EINVAL && zero_output(&sink),
               "CR failure"))
        return 1;
    request = request_for(contains_nul, sizeof(contains_nul));
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), contains_nul, 1, 1, 1, 0, 1,
                                 0, 0, &sink) == -EINVAL && zero_output(&sink),
               "NUL failure"))
        return 1;

    request = request_for(valid, (console_u32)(sizeof(valid) - 1));
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 0, 0, 1, 0, 0,
                                 &sink) == -EAGAIN && zero_output(&sink),
               "pre-lock unavailable failure") ||
        !check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, -ETIMEDOUT,
                                 1, 0, 0, &sink) == -ETIMEDOUT &&
                   zero_output(&sink), "timed lock failure") ||
        !check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, -EINTR, 1,
                                 0, 0, &sink) == -EINTR && zero_output(&sink),
               "interrupted lock failure") ||
        !check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 0, 0, 0,
                                 &sink) == -EAGAIN && zero_output(&sink),
               "post-lock unavailable recheck failure"))
        return 1;

    memset(&sink, 0, sizeof(sink));
    if (!check(fake_record_ioctl(CONSOLE_IOC_WRITE_RECORD, &request,
                                 sizeof(request), valid, 1, 1, 1, 0, 1, 4, 5,
                                 &sink) == -EAGAIN && sink.calls == 1 &&
                   sink.len != 0,
               "emergency generation is no-credit after emission"))
        return 1;

    return 0;
}
