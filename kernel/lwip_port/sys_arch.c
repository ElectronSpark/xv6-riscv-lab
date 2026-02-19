/**
 * @file sys_arch.c
 * @brief lwIP OS abstraction layer implementation for xv6 kernel
 *
 * Implements semaphores, mutexes, mailboxes, thread creation, time functions,
 * and critical section protection using xv6 kernel primitives.
 *
 * Uses xv6's sleep_on_chan/wakeup_on_chan for synchronisation as it provides
 * a simple and correct blocking primitive that works with spinlocks.
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "printf.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "timer/timer.h"
#include "timer/goldfish_rtc.h"
#include "string.h"

#include <stdarg.h>

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "arch/sys_arch.h"

/* ========================================================================== */
/* Minimal snprintf / vsnprintf for lwIP (kernel has no libc)                 */
/* ========================================================================== */

/**
 * vsnprintf: supports %s, %d, %u, %x, %X, %c, %p, %%, %ld, %lu, %lx, %lX,
 * %lld, %llu, %llx, %llX, %zd, %zu, %zx.
 * Also supports flags: '0', '-', '+', ' ',  field width, and precision.
 */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;
    if (size == 0)
        return 0;
    size_t max = size - 1;

    static const char hex_lower[] = "0123456789abcdef";
    static const char hex_upper[] = "0123456789ABCDEF";

#define PUTC(c) do { if (pos < max) buf[pos] = (c); pos++; } while (0)

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            PUTC(*fmt);
            continue;
        }
        fmt++;

        /* ── Parse flags ── */
        int flag_zero  = 0;   /* '0' — pad with zeros */
        int flag_minus = 0;   /* '-' — left-justify */
        int flag_plus  = 0;   /* '+' — show sign for positive */
        int flag_space = 0;   /* ' ' — space before positive */
        int flag_hash  = 0;   /* '#' — alternate form */
        for (;;) {
            if      (*fmt == '0') flag_zero  = 1;
            else if (*fmt == '-') flag_minus = 1;
            else if (*fmt == '+') flag_plus  = 1;
            else if (*fmt == ' ') flag_space = 1;
            else if (*fmt == '#') flag_hash  = 1;
            else break;
            fmt++;
        }
        if (flag_minus) flag_zero = 0; /* '-' overrides '0' */

        /* ── Parse width ── */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flag_minus = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        /* ── Parse precision ── */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                if (prec < 0) prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        /* ── Parse length modifier ── */
        enum { LEN_NONE, LEN_L, LEN_LL, LEN_Z } lenmod = LEN_NONE;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { lenmod = LEN_LL; fmt++; }
            else              lenmod = LEN_L;
        } else if (*fmt == 'z') {
            lenmod = LEN_Z;
            fmt++;
        }

        /* ── Conversion ── */
        switch (*fmt) {

        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen]) slen++;
            if (prec >= 0 && slen > prec) slen = prec;
            int pad = (width > slen) ? width - slen : 0;
            if (!flag_minus)
                for (int i = 0; i < pad; i++) PUTC(' ');
            for (int i = 0; i < slen; i++) PUTC(s[i]);
            if (flag_minus)
                for (int i = 0; i < pad; i++) PUTC(' ');
            break;
        }

        case 'd':
        case 'i': {
            int64 v;
            if      (lenmod == LEN_LL) v = va_arg(ap, long long);
            else if (lenmod == LEN_L)  v = va_arg(ap, long);
            else if (lenmod == LEN_Z)  v = (int64)va_arg(ap, size_t);
            else                       v = va_arg(ap, int);

            char tmp[24];
            int ti = 0;
            int negative = 0;
            uint64 uv;
            if (v < 0) { negative = 1; uv = (uint64)(-(v + 1)) + 1u; }
            else        uv = (uint64)v;

            do { tmp[ti++] = '0' + (int)(uv % 10); } while ((uv /= 10));
            /* Apply precision: minimum digits */
            if (prec >= 0) {
                while (ti < prec && ti < (int)sizeof(tmp)) tmp[ti++] = '0';
                flag_zero = 0;  /* precision overrides '0' flag */
            }

            char sign = 0;
            if (negative)       sign = '-';
            else if (flag_plus) sign = '+';
            else if (flag_space) sign = ' ';

            int numlen = ti + (sign ? 1 : 0);
            int pad = (width > numlen) ? width - numlen : 0;
            if (!flag_minus && !flag_zero)
                for (int i = 0; i < pad; i++) PUTC(' ');
            if (sign) PUTC(sign);
            if (!flag_minus && flag_zero)
                for (int i = 0; i < pad; i++) PUTC('0');
            while (ti > 0) PUTC(tmp[--ti]);
            if (flag_minus)
                for (int i = 0; i < pad; i++) PUTC(' ');
            break;
        }

        case 'u': {
            uint64 uv;
            if      (lenmod == LEN_LL) uv = va_arg(ap, unsigned long long);
            else if (lenmod == LEN_L)  uv = va_arg(ap, unsigned long);
            else if (lenmod == LEN_Z)  uv = va_arg(ap, size_t);
            else                       uv = va_arg(ap, unsigned);

            char tmp[24];
            int ti = 0;
            do { tmp[ti++] = '0' + (int)(uv % 10); } while ((uv /= 10));
            if (prec >= 0) {
                while (ti < prec && ti < (int)sizeof(tmp)) tmp[ti++] = '0';
                flag_zero = 0;
            }

            int pad = (width > ti) ? width - ti : 0;
            if (!flag_minus && !flag_zero)
                for (int i = 0; i < pad; i++) PUTC(' ');
            if (!flag_minus && flag_zero)
                for (int i = 0; i < pad; i++) PUTC('0');
            while (ti > 0) PUTC(tmp[--ti]);
            if (flag_minus)
                for (int i = 0; i < pad; i++) PUTC(' ');
            break;
        }

        case 'x':
        case 'X': {
            const char *hexd = (*fmt == 'X') ? hex_upper : hex_lower;
            uint64 uv;
            if      (lenmod == LEN_LL) uv = va_arg(ap, unsigned long long);
            else if (lenmod == LEN_L)  uv = va_arg(ap, unsigned long);
            else if (lenmod == LEN_Z)  uv = va_arg(ap, size_t);
            else                       uv = va_arg(ap, unsigned);

            char tmp[24];
            int ti = 0;
            do { tmp[ti++] = hexd[uv & 0xf]; } while ((uv >>= 4));
            if (prec >= 0) {
                while (ti < prec && ti < (int)sizeof(tmp)) tmp[ti++] = '0';
                flag_zero = 0;
            }

            int prefix = (flag_hash && uv != 0) ? 2 : 0;
            int numlen = ti + prefix;
            int pad = (width > numlen) ? width - numlen : 0;

            if (!flag_minus && !flag_zero)
                for (int i = 0; i < pad; i++) PUTC(' ');
            if (prefix) { PUTC('0'); PUTC(*fmt == 'X' ? 'X' : 'x'); }
            if (!flag_minus && flag_zero)
                for (int i = 0; i < pad; i++) PUTC('0');
            while (ti > 0) PUTC(tmp[--ti]);
            if (flag_minus)
                for (int i = 0; i < pad; i++) PUTC(' ');
            break;
        }

        case 'p': {
            uint64 pv = (uint64)va_arg(ap, void *);
            PUTC('0'); PUTC('x');
            char tmp[20];
            int ti = 0;
            do { tmp[ti++] = hex_lower[pv & 0xf]; } while ((pv >>= 4));
            int pad = (width > ti + 2) ? width - ti - 2 : 0;
            if (!flag_minus)
                for (int i = 0; i < pad; i++) PUTC('0');
            while (ti > 0) PUTC(tmp[--ti]);
            if (flag_minus)
                for (int i = 0; i < pad; i++) PUTC(' ');
            break;
        }

        case 'c':
            PUTC((char)va_arg(ap, int));
            break;

        case '%':
            PUTC('%');
            break;

        case '\0':
            goto out;

        default:
            /* Unknown specifier — output literal */
            PUTC('%');
            PUTC(*fmt);
            break;
        }
    }
out:
    if (size > 0)
        buf[pos < max ? pos : max] = '\0';
    return (int)pos;

#undef PUTC
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

/* ========================================================================== */
/* Initialisation                                                             */
/* ========================================================================== */

static uint64 sys_start_ns; /* timestamp at sys_init() */

void sys_init(void)
{
    sys_start_ns = goldfish_rtc_read_ns();
}

/* ========================================================================== */
/* Time                                                                       */
/* ========================================================================== */

u32_t sys_now(void)
{
    uint64 now_ns = goldfish_rtc_read_ns();
    /* Return ms since sys_init(). Wrap at 32 bits is fine for lwIP. */
    return (u32_t)((now_ns - sys_start_ns) / NS_PER_MS);
}

u32_t sys_jiffies(void)
{
    return (u32_t)get_jiffs();
}

/* ========================================================================== */
/* Random                                                                     */
/* ========================================================================== */

static uint32 lwip_rand_state = 0x12345678;

uint32 lwip_xv6_rand(void)
{
    /* xorshift32 PRNG seeded with RTC + cycle counter */
    if (lwip_rand_state == 0x12345678) {
        lwip_rand_state = (uint32)(goldfish_rtc_read_ns() ^ r_time());
        if (lwip_rand_state == 0)
            lwip_rand_state = 0xDEADBEEF;
    }
    uint32 x = lwip_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    lwip_rand_state = x;
    return x;
}

/* ========================================================================== */
/* Critical section (SYS_LIGHTWEIGHT_PROT)                                    */
/* ========================================================================== */

sys_prot_t sys_arch_protect(void)
{
    return intr_off_save();
}

void sys_arch_unprotect(sys_prot_t pval)
{
    intr_restore(pval);
}

/* ========================================================================== */
/* Semaphores                                                                 */
/* ========================================================================== */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    if (sem == NULL)
        return ERR_ARG;
    spin_init(&sem->lock, "lwip_sem");
    sem->count = count;
    sem->valid = 1;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem)
{
    if (sem == NULL)
        return;
    sem->valid = 0;
}

void sys_sem_signal(sys_sem_t *sem)
{
    spin_lock(&sem->lock);
    sem->count++;
    wakeup_on_chan(&sem->count);
    spin_unlock(&sem->lock);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    uint64 start_ms = sys_now();

    spin_lock(&sem->lock);
    while (sem->count <= 0) {
        if (timeout != 0) {
            uint64 elapsed = sys_now() - start_ms;
            if (elapsed >= timeout) {
                spin_unlock(&sem->lock);
                return SYS_ARCH_TIMEOUT;
            }
        }
        /* sleep_on_chan releases sem->lock and reacquires it on wakeup */
        sleep_on_chan(&sem->count, &sem->lock);
    }
    sem->count--;
    spin_unlock(&sem->lock);

    uint64 elapsed = sys_now() - start_ms;
    return (u32_t)elapsed;
}

/* ========================================================================== */
/* Mutexes                                                                    */
/* ========================================================================== */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    if (mutex == NULL)
        return ERR_ARG;
    spin_init(&mutex->lock, "lwip_mtx");
    mutex->held = 0;
    mutex->valid = 1;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    spin_lock(&mutex->lock);
    while (mutex->held) {
        sleep_on_chan(&mutex->held, &mutex->lock);
    }
    mutex->held = 1;
    spin_unlock(&mutex->lock);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    spin_lock(&mutex->lock);
    mutex->held = 0;
    wakeup_on_chan(&mutex->held);
    spin_unlock(&mutex->lock);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if (mutex == NULL)
        return;
    mutex->valid = 0;
}

/* ========================================================================== */
/* Mailboxes                                                                  */
/* ========================================================================== */

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    (void)size; /* We use a fixed-size circular buffer */
    if (mbox == NULL)
        return ERR_ARG;
    spin_init(&mbox->lock, "lwip_mbox");
    mbox->not_empty_chan = 0;
    mbox->not_full_chan = 0;
    mbox->head = 0;
    mbox->tail = 0;
    mbox->count = 0;
    mbox->valid = 1;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if (mbox == NULL)
        return;
    mbox->valid = 0;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    spin_lock(&mbox->lock);
    while (mbox->count >= SYS_MBOX_SIZE) {
        sleep_on_chan(&mbox->not_full_chan, &mbox->lock);
    }
    mbox->msgs[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % SYS_MBOX_SIZE;
    mbox->count++;
    wakeup_on_chan(&mbox->not_empty_chan);
    spin_unlock(&mbox->lock);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    spin_lock(&mbox->lock);
    if (mbox->count >= SYS_MBOX_SIZE) {
        spin_unlock(&mbox->lock);
        return ERR_MEM;
    }
    mbox->msgs[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % SYS_MBOX_SIZE;
    mbox->count++;
    wakeup_on_chan(&mbox->not_empty_chan);
    spin_unlock(&mbox->lock);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    /* In xv6, ISRs can use the same spinlock path */
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    uint64 start_ms = sys_now();

    spin_lock(&mbox->lock);
    while (mbox->count == 0) {
        if (timeout != 0) {
            uint64 elapsed = sys_now() - start_ms;
            if (elapsed >= timeout) {
                spin_unlock(&mbox->lock);
                return SYS_ARCH_TIMEOUT;
            }
        }
        sleep_on_chan(&mbox->not_empty_chan, &mbox->lock);
    }
    if (msg != NULL) {
        *msg = mbox->msgs[mbox->head];
    }
    mbox->head = (mbox->head + 1) % SYS_MBOX_SIZE;
    mbox->count--;
    wakeup_on_chan(&mbox->not_full_chan);
    spin_unlock(&mbox->lock);

    uint64 elapsed = sys_now() - start_ms;
    return (u32_t)elapsed;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    spin_lock(&mbox->lock);
    if (mbox->count == 0) {
        spin_unlock(&mbox->lock);
        return SYS_MBOX_EMPTY;
    }
    if (msg != NULL) {
        *msg = mbox->msgs[mbox->head];
    }
    mbox->head = (mbox->head + 1) % SYS_MBOX_SIZE;
    mbox->count--;
    wakeup_on_chan(&mbox->not_full_chan);
    spin_unlock(&mbox->lock);
    return 0;
}

/* ========================================================================== */
/* Threads                                                                    */
/* ========================================================================== */

struct lwip_thread_arg {
    lwip_thread_fn func;
    void *arg;
};

static int __lwip_thread_entry(uint64 arg1, uint64 arg2)
{
    (void)arg2;
    struct lwip_thread_arg *ta = (struct lwip_thread_arg *)arg1;
    lwip_thread_fn func = ta->func;
    void *arg = ta->arg;
    /* Free the argument struct (allocated via kmm_alloc) */
    kmm_free(ta);
    /* Run the lwIP thread function — it never returns */
    func(arg);
    return 0;
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                            void *arg, int stacksize, int prio)
{
    (void)stacksize;
    (void)prio;

    struct lwip_thread_arg *ta = kmm_alloc(sizeof(*ta));
    if (ta == NULL) {
        panic("lwip: failed to allocate thread arg");
    }
    ta->func = thread;
    ta->arg = arg;

    struct thread *t = kthread_create(name, __lwip_thread_entry,
                                      (uint64)ta, 0, KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        kmm_free(ta);
        panic("lwip: failed to create thread");
    }

    /* kthread_create leaves thread in THREAD_UNINTERRUPTIBLE — wake it */
    wakeup(t);

    return t;
}

/* ========================================================================== */
/* Per-thread semaphore (LWIP_NETCONN_SEM_PER_THREAD)                         */
/*                                                                            */
/* LWIP_NETCONN_FULLDUPLEX requires each thread that uses the netconn API     */
/* to have its own op_completed semaphore.  We keep a small static table      */
/* keyed by the thread pointer.  Entries are allocated on first use (lazy)    */
/* and freed when the thread shuts down its lwIP usage.                       */
/* ========================================================================== */

#define MAX_LWIP_THREAD_SEMS 32

static struct {
    struct thread *owner;
    sys_sem_t      sem;
    int            valid;
} lwip_thread_sems[MAX_LWIP_THREAD_SEMS];

static spinlock_t lwip_thread_sem_lock;
static int lwip_thread_sem_init_done;

static void lwip_thread_sem_ensure_init(void)
{
    if (!lwip_thread_sem_init_done) {
        spin_init(&lwip_thread_sem_lock, "lwip_tsem");
        lwip_thread_sem_init_done = 1;
    }
}

void sys_arch_netconn_sem_alloc(void)
{
    struct thread *t = current;
    lwip_thread_sem_ensure_init();
    spin_lock(&lwip_thread_sem_lock);
    /* Already allocated? */
    for (int i = 0; i < MAX_LWIP_THREAD_SEMS; i++) {
        if (lwip_thread_sems[i].valid && lwip_thread_sems[i].owner == t) {
            spin_unlock(&lwip_thread_sem_lock);
            return;
        }
    }
    /* Find a free slot */
    for (int i = 0; i < MAX_LWIP_THREAD_SEMS; i++) {
        if (!lwip_thread_sems[i].valid) {
            lwip_thread_sems[i].owner = t;
            sys_sem_new(&lwip_thread_sems[i].sem, 0);
            lwip_thread_sems[i].valid = 1;
            spin_unlock(&lwip_thread_sem_lock);
            return;
        }
    }
    spin_unlock(&lwip_thread_sem_lock);
    panic("lwip: too many threads using netconn API");
}

void sys_arch_netconn_sem_free(void)
{
    struct thread *t = current;
    spin_lock(&lwip_thread_sem_lock);
    for (int i = 0; i < MAX_LWIP_THREAD_SEMS; i++) {
        if (lwip_thread_sems[i].valid && lwip_thread_sems[i].owner == t) {
            sys_sem_free(&lwip_thread_sems[i].sem);
            lwip_thread_sems[i].valid = 0;
            lwip_thread_sems[i].owner = NULL;
            break;
        }
    }
    spin_unlock(&lwip_thread_sem_lock);
}

sys_sem_t *sys_arch_netconn_sem_get(void)
{
    struct thread *t = current;
    /* Fast path: scan without lock (valid+owner are stable once set) */
    for (int i = 0; i < MAX_LWIP_THREAD_SEMS; i++) {
        if (lwip_thread_sems[i].valid && lwip_thread_sems[i].owner == t)
            return &lwip_thread_sems[i].sem;
    }
    /* Not found — alloc lazily */
    sys_arch_netconn_sem_alloc();
    for (int i = 0; i < MAX_LWIP_THREAD_SEMS; i++) {
        if (lwip_thread_sems[i].valid && lwip_thread_sems[i].owner == t)
            return &lwip_thread_sems[i].sem;
    }
    panic("lwip: netconn_sem_get failed after alloc");
    return NULL; /* unreachable */
}

