#ifndef __KERNEL_BOTTLENECK_TRACE_H
#define __KERNEL_BOTTLENECK_TRACE_H

#include "types.h"

/* Native-endian fixed-width ABI (little endian on the x86_64 capture target).
 * Numeric magic is 0x3152435454425658; little-endian bytes are "XVBTTCR1".
 * Timestamps are r_time() ticks; timebase_frequency is ticks per second.
 */
#define BT_TRACE_MAGIC 0x3152435454425658ULL
#define BT_TRACE_VERSION 1ULL
#define BT_TRACE_CAPACITY 65536ULL

enum bt_event {
    BT_CREATE = 1, BT_SETTIME = 2, BT_EXPIRE = 3, BT_READ = 4,
    BT_RELEASE = 5, BT_WORK_ENQUEUE = 6, BT_WORK_START = 7,
    BT_WORK_END = 8, BT_MANAGER_WAKE = 9, BT_MANAGER_DISPATCH = 10,
    BT_NOTIFY_BEGIN = 11, BT_NOTIFY_END = 12, BT_REARM = 13,
    BT_WAKE = 14, BT_DISPATCH = 15, BT_MARK = 16, BT_WAKE_COMMIT = 17,
    BT_ARM = 18,
};

struct bt_record {
    uint64 ticks;
    uint32 event;
    uint32 cpu;
    int32 pid;
    uint32 reserved;
    uint64 task_seq; /* current->pid_seq; zero if there is no current task */
    uint64 id;
    uint64 generation;
    uint64 a;
    uint64 b;
    uint64 c;
    uint64 d;
};

struct bt_trace_header {
    uint64 magic;
    uint64 version;
    uint64 header_size;
    uint64 record_size;
    uint64 capacity;
    uint64 count; /* accepted records, stored in append order */
    uint64 attempted; /* calls admitted under lock, including saturation */
    uint64 dropped; /* saturation only: attempted == count + dropped */
    uint64 capture_number; /* increases on start; never reset */
    uint64 start_ticks;
    uint64 stop_ticks;
    uint64 timebase_frequency;
};

enum bt_control { BT_CONTROL_RESET, BT_CONTROL_START, BT_CONTROL_STOP };

/* Called once during boot, before the instrumented subsystem is published.
 * Only bottleneck_trace=1 enables the capability. Capture initially stopped.
 */
void bt_init(void);
int bt_enabled(void); /* capability, not active-capture state */
uint64 bt_new_id(void); /* nonzero per-boot ID, even between captures; 0 if off */
void bt_record(uint32 event, uint64 id, uint64 generation,
               uint64 a, uint64 b, uint64 c, uint64 d);
int bt_control(enum bt_control command);

/* One stopped snapshot reader at a time. The pointer remains immutable until
 * release; start/reset return EBUSY while leased. No copyout under trace lock.
 * Caller must release after successful acquire, including open/read failure.
 */
int bt_snapshot_acquire(const void **data, size_t *bytes);
void bt_snapshot_release(void);

#endif
