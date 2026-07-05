#include "types.h"
#include "defs.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "printf.h"
#include "klog.h"
#include "cmdline.h"
#include "dev/fdt.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/rq.h"

/* ════════════════════════════════════════════════════════════════════
 *  Asynchronous console output
 *
 *  printf() historically drained every byte to the UART synchronously
 *  (klog_write(KLOG_F_CONSOLE) → consputs → uartputc_sync busy-waiting
 *  on LSR) while holding pr.lock with interrupts disabled.  At 115200
 *  baud a long line stalls the calling CPU for milliseconds with IRQs
 *  off, and any other CPU that printf()s convoys behind pr.lock.
 *
 *  Instead, once the drain kthread is running, console bytes are staged
 *  in the ring below and emitted by the dedicated "consoled" kthread
 *  OUTSIDE any caller's critical section.
 *
 *  Invariants:
 *   - Ordering/wholeness: all printf console output is serialized by
 *     pr.lock, so each message's bytes are enqueued contiguously and
 *     drained strictly FIFO by the single drain thread — lines still
 *     reach the wire whole and in order.
 *   - Early boot: before the drain thread exists (cons_async.ready == 0)
 *     klog_write falls back to the synchronous consputs path.
 *   - Panic: panic_state() forces the synchronous path, and
 *     console_async_panic_flush() (called from __panic_start) first
 *     pushes any staged bytes out so panic output follows, not
 *     interleaves, earlier messages.
 *   - Overflow: if the ring is full the entire message is dropped (never
 *     split mid-line) and accounted; the drain thread reports the count.
 *
 *  console_async=0 on the kernel command line disables the whole
 *  mechanism (A/B isolation); default is enabled.
 * ════════════════════════════════════════════════════════════════════ */

#define CONS_ASYNC_RING_SIZE (128 * 1024) /* power of two */
#define CONS_ASYNC_CHUNK 512

static struct {
    spinlock_t lock;
    char buf[CONS_ASYNC_RING_SIZE];
    uint64 head;    /* absolute enqueue position (monotonic) */
    uint64 tail;    /* absolute drain position (monotonic)   */
    uint64 dropped; /* bytes dropped because the ring was full */
    int ready;      /* drain kthread is running               */
    int enabled;    /* console_async= cmdline (default on)    */
} cons_async = {
    .lock = SPINLOCK_INITIALIZED("consasync"),
    .enabled = 1,
};

uint64 console_async_dropped(void)
{
    return __atomic_load_n(&cons_async.dropped, __ATOMIC_RELAXED);
}

/*
 * Enqueue console bytes for the drain thread.  Returns 1 when the bytes
 * were consumed (staged or accounted as dropped), 0 when the caller must
 * fall back to the synchronous consputs path (drain thread not running,
 * or panic in progress).
 */
static int cons_async_enqueue(const char *buf, int len)
{
    if (!__atomic_load_n(&cons_async.ready, __ATOMIC_ACQUIRE) ||
        panic_state())
        return 0;

    spin_lock(&cons_async.lock);
    if (!cons_async.ready) {
        /* Lost a race with console_async_panic_flush(). */
        spin_unlock(&cons_async.lock);
        return 0;
    }
    uint64 used = cons_async.head - cons_async.tail;
    if ((uint64)len > CONS_ASYNC_RING_SIZE - used) {
        /* Ring full: drop the whole message rather than splitting a
         * line.  The drain thread reports the tally. */
        cons_async.dropped += (uint64)len;
        spin_unlock(&cons_async.lock);
        return 1;
    }
    for (int i = 0; i < len; i++)
        cons_async.buf[(cons_async.head + i) % CONS_ASYNC_RING_SIZE] = buf[i];
    /* Publish head after the bytes so a lockless panic flush on another
     * CPU never reads bytes that are not yet written. */
    __atomic_store_n(&cons_async.head, cons_async.head + (uint64)len,
                     __ATOMIC_RELEASE);
    spin_unlock(&cons_async.lock);
    return 1;
}

static void cons_async_drain_thread(uint64 arg1, uint64 arg2)
{
    /* Single drain thread — static chunk buffer keeps kstack usage low. */
    static char chunk[CONS_ASYNC_CHUNK];
    (void)arg1;
    (void)arg2;

    for (;;) {
        if (panic_state()) {
            /* The panic path owns the console now (synchronous). */
            sleep_ms(1000);
            continue;
        }

        int n = 0;
        uint64 drops = 0;

        spin_lock(&cons_async.lock);
        uint64 avail = cons_async.head - cons_async.tail;
        if (avail > 0) {
            n = (avail > CONS_ASYNC_CHUNK) ? CONS_ASYNC_CHUNK : (int)avail;
            for (int i = 0; i < n; i++)
                chunk[i] = cons_async.buf[(cons_async.tail + i) %
                                          CONS_ASYNC_RING_SIZE];
            cons_async.tail += (uint64)n;
        }
        if (cons_async.dropped != 0 && avail == (uint64)n) {
            /* Report drops once the backlog is cleared. */
            drops = cons_async.dropped;
            cons_async.dropped = 0;
        }
        spin_unlock(&cons_async.lock);

        if (n > 0) {
            /* UART I/O outside the ring lock and outside any printf
             * caller's critical section.  Emit in small steps with a
             * panic check between: a full chunk is ~45ms of char-wise
             * UART output, and a panic on another CPU must not have
             * its banner interleaved with our stale bytes. */
            int off = 0;
            while (off < n && !panic_state()) {
                int step = (n - off > 32) ? 32 : (n - off);
                consputs(chunk + off, step);
                off += step;
            }
            if (panic_state())
                continue; /* panic path owns the console now */
            if (drops != 0)
                printf("klog: console_async dropped %lu bytes\n", drops);
            continue; /* keep draining while there is a backlog */
        }

        /* Ring empty: poll.  printf callers may hold arbitrary locks or
         * run in IRQ context, so they cannot safely wake us; a short
         * timed sleep bounds drain latency at a few ms (well under the
         * 100 ms budget) without wakeup-path entanglement. */
        sleep_ms(2);
    }
}

void console_async_init(void)
{
    char value[8];

    if (platform.has_cmdline &&
        cmdline_get_param("console_async", value, sizeof(value)) == 0 &&
        cmdline_value_is_false(value))
        cons_async.enabled = 0;

    if (!cons_async.enabled)
        return;

    struct thread *t = kthread_create("consoled",
                                      (void *)cons_async_drain_thread,
                                      0, 0, 0);
    if (IS_ERR_OR_NULL(t)) {
        printf("WARNING: console_async: drain thread creation failed\n");
        return;
    }
    /* Same elevated priority as the tty drain thread so console output
     * stays prompt under load. */
    t->sched_entity->priority = MAKE_PRIORITY(16, 0);
    /* Publish before waking: from here on klog_write stages bytes. */
    __atomic_store_n(&cons_async.ready, 1, __ATOMIC_RELEASE);
    wakeup(t);
    printf("console_async: drain thread started (tid=%d)\n", t->pid);
}

/*
 * Called from __panic_start() (after printf locking is disabled, before
 * the first panic message) to push any staged-but-undrained bytes to the
 * UART synchronously and turn the async path off, so panic output comes
 * out after — not interleaved with — earlier messages.
 */
void console_async_panic_flush(void)
{
    if (!__atomic_load_n(&cons_async.ready, __ATOMIC_ACQUIRE))
        return;

    /* Stop further enqueues; panic-time klog_write goes synchronous. */
    __atomic_store_n(&cons_async.ready, 0, __ATOMIC_RELEASE);

    /* Best effort: the drain thread (or an enqueuer on another CPU) may
     * hold the lock and never release it (it will be halted by the crash
     * IPI).  Try once, then proceed locklessly — losing the race merely
     * risks duplicated bytes on an already-dying system. */
    int locked = spin_trylock(&cons_async.lock);

    uint64 tail = cons_async.tail;
    uint64 head = __atomic_load_n(&cons_async.head, __ATOMIC_ACQUIRE);
    while (tail != head) {
        char c = cons_async.buf[tail % CONS_ASYNC_RING_SIZE];
        consputs(&c, 1);
        tail++;
    }

    if (locked) {
        cons_async.tail = tail;
        spin_unlock(&cons_async.lock);
    }
    /* !locked: skip the tail store -- a concurrent drain holding the
     * lock may have advanced tail past our snapshot; rewinding it
     * would make the drain replay already-emitted bytes.  Worst case
     * without the store is duplicated output on a dying system. */
}

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
    if (flags & KLOG_F_CONSOLE) {
        /* Stage for the consoled drain kthread; fall back to the
         * synchronous UART path before the thread exists or in panic. */
        if (!cons_async_enqueue(buf, len))
            consputs(buf, len);
    }
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
    if (flags & KLOG_F_CONSOLE) {
        if (!cons_async_enqueue(buf, len))
            consputs(buf, len);
    }
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
