#include "types.h"
#include "defs.h"
#include "lock/spinlock.h"
#include "printf.h"
#include "klog.h"
#include "cmdline.h"
#include "dev/fdt.h"

#ifdef XV6_KLOG
static struct {
    spinlock_t lock;
    char ring[KLOG_RING_SIZE];
    uint64 head;
    uint64 count;
    uint64 dropped;
    int ready;
    int enabled;
    int configured;
} klog = {
    .lock = SPINLOCK_INITIALIZED("klog"),
    .enabled = 1,
};

static void klog_configure_from_cmdline(void)
{
    char value[8];

    if (__atomic_load_n(&klog.configured, __ATOMIC_ACQUIRE) ||
        !platform.has_cmdline)
        return;

    if (cmdline_get_param("klog", value, sizeof(value)) == 0 &&
        cmdline_value_is_false(value))
        __atomic_store_n(&klog.enabled, 0, __ATOMIC_RELEASE);

    __atomic_store_n(&klog.configured, 1, __ATOMIC_RELEASE);
}

void kloginit(void)
{
    spin_init(&klog.lock, "klog");
    klog_configure_from_cmdline();
    __atomic_store_n(&klog.ready, 1, __ATOMIC_RELEASE);
}

int klog_ring_enabled(void)
{
    if (!__atomic_load_n(&klog.ready, __ATOMIC_ACQUIRE))
        return 0;
    klog_configure_from_cmdline();
    return __atomic_load_n(&klog.enabled, __ATOMIC_ACQUIRE) != 0;
}

static void klog_ring_write_locked(const char *buf, int len)
{
    for (int i = 0; i < len; i++) {
        if (klog.count == KLOG_RING_SIZE) {
            klog.dropped++;
        } else {
            klog.count++;
        }
        klog.ring[klog.head] = buf[i];
        klog.head = (klog.head + 1) % KLOG_RING_SIZE;
    }
}

static void klog_ring_write(const char *buf, int len)
{
    if (!klog_ring_enabled())
        return;

    if (panic_state()) {
#ifdef __CHECKER__
        spin_lock(&klog.lock);
#else
        if (!spin_trylock(&klog.lock))
            return;
#endif
        klog_ring_write_locked(buf, len);
        spin_unlock(&klog.lock);
        return;
    }

    spin_lock(&klog.lock);
    klog_ring_write_locked(buf, len);
    spin_unlock(&klog.lock);
}

void klog_write(const char *buf, int len, uint flags)
{
    if (buf == NULL || len <= 0)
        return;

    if (flags & KLOG_F_RING)
        klog_ring_write(buf, len);
    if (flags & KLOG_F_CONSOLE)
        consputs(buf, len);
}

size_t klog_capacity(void)
{
    if (!klog_ring_enabled())
        return 0;
    return KLOG_RING_SIZE;
}

size_t klog_size(void)
{
    size_t count;

    if (!klog_ring_enabled())
        return 0;

    spin_lock(&klog.lock);
    count = (size_t)klog.count;
    spin_unlock(&klog.lock);
    return count;
}

size_t klog_snapshot(char *dst, size_t max, uint64 *dropped)
{
    size_t n;
    uint64 start;

    if (dropped != NULL)
        *dropped = 0;
    if (dst == NULL || max == 0 ||
        !klog_ring_enabled())
        return 0;

    spin_lock(&klog.lock);
    n = (size_t)klog.count;
    if (n > max)
        n = max;
    start = (klog.head + KLOG_RING_SIZE - n) % KLOG_RING_SIZE;
    for (size_t i = 0; i < n; i++)
        dst[i] = klog.ring[(start + i) % KLOG_RING_SIZE];
    if (dropped != NULL)
        *dropped = klog.dropped;
    spin_unlock(&klog.lock);

    return n;
}

void klog_clear(void)
{
    if (!klog_ring_enabled())
        return;

    spin_lock(&klog.lock);
    klog.head = 0;
    klog.count = 0;
    klog.dropped = 0;
    spin_unlock(&klog.lock);
}

uint64 klog_dropped(void)
{
    uint64 dropped;

    if (!klog_ring_enabled())
        return 0;

    spin_lock(&klog.lock);
    dropped = klog.dropped;
    spin_unlock(&klog.lock);
    return dropped;
}
#else
void kloginit(void)
{
}

int klog_ring_enabled(void)
{
    return 0;
}

void klog_write(const char *buf, int len, uint flags)
{
    if (buf == NULL || len <= 0)
        return;
    if (flags & KLOG_F_CONSOLE)
        consputs(buf, len);
}

size_t klog_capacity(void)
{
    return 0;
}

size_t klog_size(void)
{
    return 0;
}

size_t klog_snapshot(char *dst, size_t max, uint64 *dropped)
{
    (void)dst;
    (void)max;
    if (dropped != NULL)
        *dropped = 0;
    return 0;
}

void klog_clear(void)
{
}

uint64 klog_dropped(void)
{
    return 0;
}
#endif
