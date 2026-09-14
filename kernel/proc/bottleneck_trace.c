#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "cmdline.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/bottleneck_trace.h"
#include "timer/timer.h"

_Static_assert(sizeof(struct bt_record) == 80, "bottleneck record ABI");
_Static_assert(sizeof(struct bt_trace_header) == 96, "bottleneck header ABI");

static struct {
    struct bt_trace_header header;
    struct bt_record records[BT_TRACE_CAPACITY];
} bt_storage;

/* Leaf lock: never acquire a subsystem lock, allocate, schedule, format, or
 * copy to userspace while held. Existing caller locks may precede this lock.
 */
static spinlock_t bt_lock = SPINLOCK_INITIALIZED("bottleneck_trace");
static int bt_capable;
static int bt_active;
static uint64 bt_active_capture;
static int bt_snapshot_leased;
static int bt_needs_reset;
static uint64 bt_last_id;

void bt_init(void)
{
    char value[8];
    int enabled = cmdline_get_param("bottleneck_trace", value, sizeof(value)) == 0
                  && strcmp(value, "1") == 0;
    bt_storage.header.magic = BT_TRACE_MAGIC;
    bt_storage.header.version = BT_TRACE_VERSION;
    bt_storage.header.header_size = sizeof(struct bt_trace_header);
    bt_storage.header.record_size = sizeof(struct bt_record);
    bt_storage.header.capacity = BT_TRACE_CAPACITY;
    bt_storage.header.timebase_frequency = __timebase_frequency;
    __atomic_store_n(&bt_capable, enabled, __ATOMIC_RELEASE);
}

int bt_enabled(void)
{
    return __atomic_load_n(&bt_capable, __ATOMIC_ACQUIRE);
}

uint64 bt_new_id(void)
{
    if (!bt_enabled())
        return 0;
    /* IDs are never reused by reset/start. Exhaustion cannot occur in a
     * bounded capture boot; zero remains the unavailable sentinel. */
    return __atomic_add_fetch(&bt_last_id, 1, __ATOMIC_RELAXED);
}

void bt_record(uint32 event, uint64 id, uint64 generation,
               uint64 a, uint64 b, uint64 c, uint64 d)
{
    if (!bt_enabled())
        return;
    uint64 capture = __atomic_load_n(&bt_active_capture, __ATOMIC_ACQUIRE);
    if (capture == 0)
        return;
    /* Capture the observation before waiting for the ring lock. push_off
     * keeps its CPU/current-task identity stable across that wait. */
    push_off();
    struct thread *p = mycpu()->proc;
    struct bt_record rec = {
        .ticks = r_time(), .event = event, .cpu = cpuid(),
        .pid = p != NULL ? p->pid : 0,
        .task_seq = p != NULL ? p->pid_seq : 0,
        .id = id, .generation = generation, .a = a, .b = b, .c = c, .d = d,
    };
    spin_lock(&bt_lock);
    if (bt_active && capture == bt_storage.header.capture_number) {
        bt_storage.header.attempted++;
        if (bt_storage.header.count < BT_TRACE_CAPACITY)
            bt_storage.records[bt_storage.header.count++] = rec;
        else
            bt_storage.header.dropped++;
    }
    spin_unlock(&bt_lock);
    pop_off();
}

int bt_control(enum bt_control command)
{
    int ret = 0;
    if (!bt_enabled())
        return -ENODEV;
    spin_lock(&bt_lock);
    switch (command) {
    case BT_CONTROL_RESET:
        if (bt_active || bt_snapshot_leased) {
            ret = -EBUSY;
            break;
        }
        /* Old bytes are unreachable: snapshot length uses count. Avoid
         * clearing the entire 5 MiB ring with interrupts disabled. */
        bt_storage.header.count = 0;
        bt_storage.header.attempted = 0;
        bt_storage.header.dropped = 0;
        bt_storage.header.start_ticks = 0;
        bt_storage.header.stop_ticks = 0;
        bt_needs_reset = 0;
        break;
    case BT_CONTROL_START:
        if (bt_active || bt_snapshot_leased || bt_needs_reset) {
            ret = -EBUSY;
            break;
        }
        bt_storage.header.capture_number++;
        bt_storage.header.start_ticks = r_time();
        bt_storage.header.stop_ticks = 0;
        bt_needs_reset = 1;
        __atomic_store_n(&bt_active, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&bt_active_capture, bt_storage.header.capture_number,
                         __ATOMIC_RELEASE);
        break;
    case BT_CONTROL_STOP:
        if (bt_active) {
            __atomic_store_n(&bt_active_capture, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&bt_active, 0, __ATOMIC_RELEASE);
            bt_storage.header.stop_ticks = r_time();
        }
        break;
    default:
        ret = -EINVAL;
        break;
    }
    spin_unlock(&bt_lock);
    return ret;
}

int bt_snapshot_acquire(const void **data, size_t *bytes)
{
    if (data == NULL || bytes == NULL)
        return -EINVAL;
    if (!bt_enabled())
        return -ENODEV;
    spin_lock(&bt_lock);
    if (bt_active || bt_snapshot_leased) {
        spin_unlock(&bt_lock);
        return -EBUSY;
    }
    bt_snapshot_leased = 1;
    *data = &bt_storage;
    *bytes = sizeof(bt_storage.header) +
             bt_storage.header.count * sizeof(struct bt_record);
    spin_unlock(&bt_lock);
    return 0;
}

void bt_snapshot_release(void)
{
    spin_lock(&bt_lock);
    bt_snapshot_leased = 0;
    spin_unlock(&bt_lock);
}
