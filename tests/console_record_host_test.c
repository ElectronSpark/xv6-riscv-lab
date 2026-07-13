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

struct fake_batch_sink {
    char bytes[CONSOLE_RECORD_BATCH_MAX_PHYSICAL_BYTES + 16];
    unsigned int len;
    unsigned int record_calls;
    unsigned int lock_acquisitions;
    unsigned int lock_releases;
    unsigned int normal_writer_calls;
};

static struct fake_batch_sink batch_sink;
static char max_batch[CONSOLE_RECORD_BATCH_MAX_LOGICAL_BYTES];

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

static void fake_batch_emit_record(struct fake_batch_sink *sink,
                                   const char *record, console_u32 len)
{
    sink->record_calls++;
    for (console_u32 i = 0; i < len; i++) {
        if (record[i] == '\n')
            sink->bytes[sink->len++] = '\r';
        sink->bytes[sink->len++] = record[i];
    }
}

/* Models the production batch path, including one held wire mutex across all
 * rows. A pending normal writer is emitted only after the batch releases the
 * mutex, while an emergency bypass may dirty an already emitted envelope and
 * forces a no-credit result. */
static int fake_batch_ioctl(
    unsigned long cmd, const struct console_record_batch_write_v1 *request,
    size_t request_copy_len, const char *records, int root, int allocation_ok,
    int payload_copy_ok, int pre_lock_available, int lock_ret,
    int post_lock_available, int emergency_after_row,
    const char *pending_normal_writer, struct fake_batch_sink *sink)
{
    console_u32 start = 0;
    console_u32 emitted = 0;
    int ret;

    if (cmd != CONSOLE_IOC_WRITE_RECORD_BATCH)
        return -ENOTTY;
    if (!root)
        return -EPERM;
    if (request == NULL || request_copy_len != sizeof(*request))
        return -EFAULT;
    if (request->version != CONSOLE_RECORD_BATCH_ABI_VERSION ||
        request->flags != 0 || request->reserved0 != 0 ||
        request->reserved1 != 0 || request->data_ptr == 0 ||
        request->data_len == 0 ||
        request->data_len > CONSOLE_RECORD_BATCH_MAX_LOGICAL_BYTES ||
        request->record_count == 0 ||
        request->record_count > CONSOLE_RECORD_BATCH_MAX_RECORDS ||
        (console_u64)request->data_len + request->record_count >
            CONSOLE_RECORD_BATCH_MAX_PHYSICAL_BYTES)
        return -EINVAL;
    if (!allocation_ok)
        return -ENOMEM;
    if (!payload_copy_ok || records == NULL)
        return -EFAULT;
    if (!console_record_batch_wire_text_valid(records, request->data_len,
                                              request->record_count))
        return -EINVAL;
    if (!pre_lock_available)
        return -EAGAIN;
    if (lock_ret != 0)
        return lock_ret;

    sink->lock_acquisitions++;
    if (!post_lock_available) {
        ret = -EAGAIN;
        goto unlock;
    }

    while (start < request->data_len) {
        console_u32 end = start;
        while (records[end] != '\n')
            end++;
        end++;
        fake_batch_emit_record(sink, records + start, end - start);
        if ((int)emitted == emergency_after_row) {
            sink->bytes[sink->len++] = '!';
            ret = -EAGAIN;
            goto unlock;
        }
        emitted++;
        start = end;
    }
    ret = emitted == request->record_count ? (int)request->data_len : -EAGAIN;

unlock:
    sink->lock_releases++;
    if (pending_normal_writer != NULL) {
        size_t len = strlen(pending_normal_writer);
        memcpy(sink->bytes + sink->len, pending_normal_writer, len);
        sink->len += (unsigned int)len;
        sink->normal_writer_calls++;
    }
    return ret;
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

static struct console_record_batch_write_v1
batch_request_for(const char *records, console_u32 len, console_u32 count)
{
    return (struct console_record_batch_write_v1){
        .version = CONSOLE_RECORD_BATCH_ABI_VERSION,
        .flags = 0,
        .data_ptr = (console_u64)(uintptr_t)records,
        .data_len = len,
        .record_count = count,
        .reserved0 = 0,
        .reserved1 = 0,
    };
}

static void reset_batch_sink(void)
{
    memset(&batch_sink, 0, sizeof(batch_sink));
}

static int check_batch_pre_emission(
    const char *label, const struct console_record_batch_write_v1 *request,
    size_t request_copy_len, const char *records, int root, int allocation_ok,
    int payload_copy_ok, int pre_lock_available, int lock_ret,
    int post_lock_available, int expected)
{
    reset_batch_sink();
    return check(fake_batch_ioctl(
                     CONSOLE_IOC_WRITE_RECORD_BATCH, request,
                     request_copy_len, records, root, allocation_ok,
                     payload_copy_ok, pre_lock_available, lock_ret,
                     post_lock_available, -1, NULL, &batch_sink) == expected &&
                     batch_sink.len == 0 && batch_sink.record_calls == 0,
                 label);
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
    const char batch_valid[] = "A\nBB\n";
    const char batch_empty_record[] = "A\n\n";
    const char batch_missing_lf[] = "A\nBB";
    const char batch_cr[] = "A\r\n";
    const char batch_nul[] = {'A', '\0', '\n'};
    struct console_record_batch_write_v1 batch_request =
        batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);

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

    if (!check(sizeof(batch_request) == 32, "batch request size") ||
        !check(offsetof(struct console_record_batch_write_v1, version) == 0,
               "batch version offset") ||
        !check(offsetof(struct console_record_batch_write_v1, flags) == 4,
               "batch flags offset") ||
        !check(offsetof(struct console_record_batch_write_v1, data_ptr) == 8,
               "batch data pointer offset") ||
        !check(offsetof(struct console_record_batch_write_v1, data_len) == 16,
               "batch data length offset") ||
        !check(offsetof(struct console_record_batch_write_v1, record_count) ==
                   20,
               "batch record count offset") ||
        !check(offsetof(struct console_record_batch_write_v1, reserved0) == 24,
               "batch reserved0 offset") ||
        !check(offsetof(struct console_record_batch_write_v1, reserved1) == 28,
               "batch reserved1 offset") ||
        !check(_IOC_TYPE(CONSOLE_IOC_WRITE_RECORD_BATCH) ==
                   CONSOLE_RECORD_IOC_MAGIC,
               "batch ioctl magic") ||
        !check(_IOC_NR(CONSOLE_IOC_WRITE_RECORD_BATCH) ==
                   CONSOLE_RECORD_IOC_WRITE_BATCH_NR,
               "batch ioctl number") ||
        !check(_IOC_SIZE(CONSOLE_IOC_WRITE_RECORD_BATCH) ==
                   sizeof(batch_request),
               "batch ioctl request size") ||
        !check(_IOC_DIR(CONSOLE_IOC_WRITE_RECORD_BATCH) == _IOC_WRITE,
               "batch ioctl direction"))
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

    reset_batch_sink();
    if (!check(fake_batch_ioctl(CONSOLE_IOC_WRITE_RECORD_BATCH, &batch_request,
                                sizeof(batch_request), batch_valid, 1, 1, 1,
                                1, 0, 1, -1, "NORMAL", &batch_sink) ==
                   (int)batch_request.data_len,
               "batch valid logical-byte return") ||
        !check(batch_sink.lock_acquisitions == 1 &&
                   batch_sink.lock_releases == 1,
               "batch acquires and releases wire mutex exactly once") ||
        !check(batch_sink.record_calls == 2,
               "batch emits two complete records while locked") ||
        !check(batch_sink.normal_writer_calls == 1 &&
                   batch_sink.len == 7 + strlen("NORMAL") &&
                   memcmp(batch_sink.bytes, "A\r\nBB\r\nNORMAL",
                          batch_sink.len) == 0,
               "normal writer cannot interleave batch records"))
        return 1;

    {
        size_t offset = 0;
        for (unsigned int i = 0; i < 700; i++) {
            memset(max_batch + offset, 'x', 509);
            max_batch[offset + 509] = '\n';
            offset += 510;
        }
        memset(max_batch + offset, 'y', 79);
        max_batch[offset + 79] = '\n';
        offset += 80;
        batch_request = batch_request_for(max_batch, (console_u32)offset,
                                          CONSOLE_RECORD_BATCH_MAX_RECORDS);
        reset_batch_sink();
        if (!check(offset == CONSOLE_RECORD_BATCH_MAX_LOGICAL_BYTES,
                   "batch exact logical maximum arithmetic") ||
            !check(offset + CONSOLE_RECORD_BATCH_MAX_RECORDS ==
                       CONSOLE_RECORD_BATCH_MAX_PHYSICAL_BYTES,
                   "batch exact physical maximum arithmetic") ||
            !check(fake_batch_ioctl(
                       CONSOLE_IOC_WRITE_RECORD_BATCH, &batch_request,
                       sizeof(batch_request), max_batch, 1, 1, 1, 1, 0, 1,
                       -1, "N", &batch_sink) == (int)offset,
                   "max batch success") ||
            !check(batch_sink.lock_acquisitions == 1 &&
                       batch_sink.lock_releases == 1 &&
                       batch_sink.record_calls ==
                           CONSOLE_RECORD_BATCH_MAX_RECORDS,
                   "max batch is one mutex critical section") ||
            !check(batch_sink.len ==
                       CONSOLE_RECORD_BATCH_MAX_PHYSICAL_BYTES + 1 &&
                       batch_sink.bytes
                               [CONSOLE_RECORD_BATCH_MAX_PHYSICAL_BYTES] ==
                           'N',
                   "max batch physical bound precedes normal writer"))
            return 1;
    }

    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);
    if (!check_batch_pre_emission("batch short request copy", &batch_request,
                                  sizeof(batch_request) - 1, batch_valid, 1,
                                  1, 1, 1, 0, 1, -EFAULT) ||
        !check_batch_pre_emission("batch root-only", &batch_request,
                                  sizeof(batch_request), batch_valid, 0, 1, 1,
                                  1, 0, 1, -EPERM) ||
        !check_batch_pre_emission("batch allocation failure", &batch_request,
                                  sizeof(batch_request), batch_valid, 1, 0, 1,
                                  1, 0, 1, -ENOMEM) ||
        !check_batch_pre_emission("batch payload copy failure", &batch_request,
                                  sizeof(batch_request), batch_valid, 1, 1, 0,
                                  1, 0, 1, -EFAULT) ||
        !check_batch_pre_emission("batch unavailable before lock",
                                  &batch_request, sizeof(batch_request),
                                  batch_valid, 1, 1, 1, 0, 0, 1, -EAGAIN) ||
        !check_batch_pre_emission("batch timed lock failure", &batch_request,
                                  sizeof(batch_request), batch_valid, 1, 1, 1,
                                  1, -ETIMEDOUT, 1, -ETIMEDOUT) ||
        !check_batch_pre_emission("batch interrupted lock failure",
                                  &batch_request, sizeof(batch_request),
                                  batch_valid, 1, 1, 1, 1, -EINTR, 1,
                                  -EINTR) ||
        !check_batch_pre_emission("batch panic/unavailable after lock",
                                  &batch_request, sizeof(batch_request),
                                  batch_valid, 1, 1, 1, 1, 0, 0, -EAGAIN))
        return 1;

    batch_request = batch_request_for(batch_empty_record,
                                      sizeof(batch_empty_record) - 1, 2);
    if (!check_batch_pre_emission("batch empty record rejected", &batch_request,
                                  sizeof(batch_request), batch_empty_record, 1,
                                  1, 1, 1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_missing_lf,
                                      sizeof(batch_missing_lf) - 1, 2);
    if (!check_batch_pre_emission("batch missing terminal LF rejected",
                                  &batch_request, sizeof(batch_request),
                                  batch_missing_lf, 1, 1, 1, 1, 0, 1,
                                  -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_cr, sizeof(batch_cr) - 1, 1);
    if (!check_batch_pre_emission("batch CR rejected", &batch_request,
                                  sizeof(batch_request), batch_cr, 1, 1, 1, 1,
                                  0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_nul, sizeof(batch_nul), 1);
    if (!check_batch_pre_emission("batch NUL rejected", &batch_request,
                                  sizeof(batch_request), batch_nul, 1, 1, 1,
                                  1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 1);
    if (!check_batch_pre_emission("batch record-count drift rejected",
                                  &batch_request, sizeof(batch_request),
                                  batch_valid, 1, 1, 1, 1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);
    batch_request.flags = 1;
    if (!check_batch_pre_emission("batch flags rejected", &batch_request,
                                  sizeof(batch_request), batch_valid, 1, 1, 1,
                                  1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);
    batch_request.version++;
    if (!check_batch_pre_emission("batch version rejected", &batch_request,
                                  sizeof(batch_request), batch_valid, 1, 1, 1,
                                  1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);
    batch_request.data_ptr = 0;
    if (!check_batch_pre_emission("batch null data pointer rejected",
                                  &batch_request, sizeof(batch_request),
                                  batch_valid, 1, 1, 1, 1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 0);
    if (!check_batch_pre_emission("batch zero record count rejected",
                                  &batch_request, sizeof(batch_request),
                                  batch_valid, 1, 1, 1, 1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(
        batch_valid, sizeof(batch_valid) - 1,
        CONSOLE_RECORD_BATCH_MAX_RECORDS + 1);
    if (!check_batch_pre_emission("batch record maximum enforced",
                                  &batch_request, sizeof(batch_request),
                                  batch_valid, 1, 1, 1, 1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);
    batch_request.data_len = CONSOLE_RECORD_BATCH_MAX_LOGICAL_BYTES + 1;
    if (!check_batch_pre_emission("batch logical maximum enforced",
                                  &batch_request, sizeof(batch_request),
                                  batch_valid, 1, 1, 1, 1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);
    batch_request.reserved0 = 1;
    if (!check_batch_pre_emission("batch reserved0 rejected", &batch_request,
                                  sizeof(batch_request), batch_valid, 1, 1, 1,
                                  1, 0, 1, -EINVAL))
        return 1;
    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);
    batch_request.reserved1 = 1;
    if (!check_batch_pre_emission("batch reserved rejected", &batch_request,
                                  sizeof(batch_request), batch_valid, 1, 1, 1,
                                  1, 0, 1, -EINVAL))
        return 1;

    batch_request = batch_request_for(batch_valid, sizeof(batch_valid) - 1, 2);
    reset_batch_sink();
    if (!check(fake_batch_ioctl(CONSOLE_IOC_WRITE_RECORD_BATCH, &batch_request,
                                sizeof(batch_request), batch_valid, 1, 1, 1,
                                1, 0, 1, 0, "N", &batch_sink) == -EAGAIN,
               "batch emergency bypass returns no credit") ||
        !check(batch_sink.lock_acquisitions == 1 &&
                   batch_sink.lock_releases == 1 &&
                   batch_sink.record_calls == 1 && batch_sink.len != 0,
               "batch emergency failure honestly follows emission") ||
        !check(batch_sink.bytes[batch_sink.len - 1] == 'N',
               "normal writer remains outside failed batch lock"))
        return 1;

    return 0;
}
