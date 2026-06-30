#include "types.h"
#include "defs.h"
#include "lock/spinlock.h"
#include "printf.h"
#include "klog.h"

#define KLOG_RING_SIZE 16384

static struct {
    spinlock_t lock;
    char ring[KLOG_RING_SIZE];
    uint64 head;
    uint64 count;
    uint64 dropped;
    int ready;
} klog = {
    .lock = SPINLOCK_INITIALIZED("klog"),
};

void kloginit(void)
{
    spin_init(&klog.lock, "klog");
    __atomic_store_n(&klog.ready, 1, __ATOMIC_RELEASE);
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
    if (!__atomic_load_n(&klog.ready, __ATOMIC_ACQUIRE))
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

uint64 klog_dropped(void)
{
    uint64 dropped;

    if (!__atomic_load_n(&klog.ready, __ATOMIC_ACQUIRE))
        return 0;

    spin_lock(&klog.lock);
    dropped = klog.dropped;
    spin_unlock(&klog.lock);
    return dropped;
}
