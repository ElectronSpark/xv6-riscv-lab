//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc/thread.h"
#include "smp/ipi.h"
#include <dev/fb.h>

static spinlock_t __panic_bt_lock = SPINLOCK_INITIALIZED("panic_bt_lock");

// Global panic state - set when any core panics
static volatile int __global_panic_state = 0;

/* Panic text capture buffer — filled during panic for BSOD display */
#define PANIC_TEXT_SIZE 4096
static char __panic_textbuf[PANIC_TEXT_SIZE];
static int  __panic_textpos = 0;

static void panic_capture(const char *s, int n)
{
    for (int i = 0; i < n && __panic_textpos < PANIC_TEXT_SIZE - 1; i++)
        __panic_textbuf[__panic_textpos++] = s[i];
    __panic_textbuf[__panic_textpos] = '\0';
}

int panic_state(void) {
    return __atomic_load_n(&__global_panic_state, __ATOMIC_ACQUIRE);
}

// lock to avoid interleaving concurrent printf's.
STATIC struct {
    spinlock_t lock;
    int locking;
} pr;

/* Wrapper: sends to console and also captures during panic */
static inline void printf_emit(const char *buf, int len)
{
    consputs(buf, len);
    if (__atomic_load_n(&__global_panic_state, __ATOMIC_RELAXED))
        panic_capture(buf, len);
}

#define PRINTF_OUTBUF_SIZE 512
#define PRINTF_FLUSH_MARGIN 32

static inline void printf_putc_buf(char *outbuf, int *outlen, char c)
{
    if (*outlen >= PRINTF_OUTBUF_SIZE) {
        printf_emit(outbuf, *outlen);
        *outlen = 0;
    }
    outbuf[(*outlen)++] = c;
}

static inline void printf_maybe_flush(char *outbuf, int *outlen)
{
    if (*outlen >= PRINTF_OUTBUF_SIZE - PRINTF_FLUSH_MARGIN) {
        printf_emit(outbuf, *outlen);
        *outlen = 0;
    }
}

STATIC char digits[] = "0123456789abcdef";

STATIC void printint(long long xx, int base, int sign, char *outbuf,
	                     int *outlen) {
    char buf[32];
    int i;
    unsigned long long x;

    if (sign && (sign = (xx < 0)))
        x = -xx;
    else
        x = xx;

    i = 0;
    do {
        buf[i++] = digits[x % base];
    } while ((x /= base) != 0);

    if (sign)
        buf[i++] = '-';

    while (--i >= 0)
        printf_putc_buf(outbuf, outlen, buf[i]);
}

STATIC void printptr(uint64 x, char *outbuf, int *outlen) {
    int i;
    printf_putc_buf(outbuf, outlen, '0');
    printf_putc_buf(outbuf, outlen, 'x');
    for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        printf_putc_buf(outbuf, outlen,
                        digits[x >> (sizeof(uint64) * 8 - 4)]);
}

STATIC void print_padding(int len, char *outbuf, int *outlen) {
    for (int i = 0; i < len; i++) {
        printf_putc_buf(outbuf, outlen, ' ');
    }
}

STATIC void print_timestamp(char *outbuf, int *outlen) {
    uint64 stime = r_time();
    printf_putc_buf(outbuf, outlen, '[');
    printint(stime, 10, 0, outbuf, outlen);
    printf_putc_buf(outbuf, outlen, ']');
    printf_putc_buf(outbuf, outlen, ' ');
}

// Print to the console.
int printf(char *fmt, ...) {
    va_list ap;
    int i, cx, c0, c1, c2, locking;
    char *s;
    static int nnewline = false;
    char outbuf[PRINTF_OUTBUF_SIZE]; // Buffer for batched output
    int outlen = 0;

    // Use atomic load to see if panic disabled locking
    locking = __atomic_load_n(&pr.locking, __ATOMIC_ACQUIRE);
    if (locking)
        spin_lock(&pr.lock);

    if (!__atomic_test_and_set(&nnewline, __ATOMIC_ACQUIRE)) {
        print_timestamp(outbuf, &outlen);
    }

    va_start(ap, fmt);
    for (i = 0; (cx = fmt[i] & 0xff) != 0; i++) {
        if (cx != '%') {
            printf_putc_buf(outbuf, &outlen, cx);
            if (cx == '\n') {
                __atomic_clear(&nnewline, __ATOMIC_RELEASE);
            }
            printf_maybe_flush(outbuf, &outlen);
            continue;
        }
        i++;

        // Parse optional '-' flag (left-align) and field width
        int left_align = 0;
        int field_width = 0;
        if ((fmt[i] & 0xff) == '-') {
            left_align = 1;
            i++;
        }
        while ((fmt[i] & 0xff) >= '0' && (fmt[i] & 0xff) <= '9') {
            field_width = field_width * 10 + ((fmt[i] & 0xff) - '0');
            i++;
        }
        int start_pos = outlen;

        c0 = fmt[i + 0] & 0xff;
        c1 = c2 = 0;
        if (c0)
            c1 = fmt[i + 1] & 0xff;
        if (c0 && c1 && c0 == '*' && c1 == 's') {
            int len = va_arg(ap, int);
            print_padding(len, outbuf, &outlen);
            i++;
            c0 = c1;
            c1 = fmt[i + 1] & 0xff;
        }
        if (c1)
            c2 = fmt[i + 2] & 0xff;
        if (c0 == 'd') {
            printint(va_arg(ap, int), 10, 1, outbuf, &outlen);
        } else if (c0 == 'l' && c1 == 'd') {
            printint(va_arg(ap, uint64), 10, 1, outbuf, &outlen);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
            printint(va_arg(ap, uint64), 10, 1, outbuf, &outlen);
            i += 2;
        } else if (c0 == 'u') {
            printint(va_arg(ap, int), 10, 0, outbuf, &outlen);
        } else if (c0 == 'l' && c1 == 'u') {
            printint(va_arg(ap, uint64), 10, 0, outbuf, &outlen);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
            printint(va_arg(ap, uint64), 10, 0, outbuf, &outlen);
            i += 2;
        } else if (c0 == 'x') {
            printint(va_arg(ap, int), 16, 0, outbuf, &outlen);
        } else if (c0 == 'l' && c1 == 'x') {
            printint(va_arg(ap, uint64), 16, 0, outbuf, &outlen);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
            printint(va_arg(ap, uint64), 16, 0, outbuf, &outlen);
            i += 2;
        } else if (c0 == 'p') {
            printptr(va_arg(ap, uint64), outbuf, &outlen);
        } else if (c0 == 'c') {
            consputc(va_arg(ap, int));
        } else if (c0 == 's') {
            if ((s = va_arg(ap, char *)) == 0)
                s = "(null)";
            for (; *s; s++) {
                printf_putc_buf(outbuf, &outlen, *s);
                printf_maybe_flush(outbuf, &outlen);
            }
        } else if (c0 == '%') {
            printf_putc_buf(outbuf, &outlen, '%');
        } else if (c0 == 0) {
            break;
        } else {
            // Print unknown % sequence to draw attention.
            printf_putc_buf(outbuf, &outlen, '%');
            printf_putc_buf(outbuf, &outlen, c0);
        }

        // Apply left-aligned field width padding
        if (left_align && field_width > 0) {
            int written = outlen - start_pos;
            while (written < field_width) {
                printf_putc_buf(outbuf, &outlen, ' ');
                written++;
            }
        }
        printf_maybe_flush(outbuf, &outlen);
    }
    va_end(ap);

    // Flush remaining buffer
    if (outlen > 0) {
        printf_emit(outbuf, outlen);
    }

    if (locking)
        spin_unlock(&pr.lock);

    return 0;
}

static int __bt_enabled = 1;

void panic_disable_bt(void) { __bt_enabled = 0; }

void __panic_start() __acquires(__panic_msg_context)
{
    // Set global panic state FIRST so other cores can see it
    __atomic_store_n(&__global_panic_state, 1, __ATOMIC_RELEASE);

    // Disable printf locking FIRST, before anything else.
    // This prevents recursive deadlock when panic is called due to
    // a deadlock on pr.lock itself (spin_acquire timeout on "pr" lock).
    // Use atomic store with release semantics so other cores see it.
    __atomic_store_n(&pr.locking, 0, __ATOMIC_RELEASE);

    SET_CPU_CRASHED();
    panic_msg_lock();
    uint64 fp = r_fp();
    struct thread *p = current;
    if (p == NULL || p->kstack == 0) {
        // No thread context - use kernel memory bounds for backtrace
        printf("[Core: %d] No thread context, fp=%p\n", cpuid(), (void *)fp);
        if (__bt_enabled) {
            // Use virtual addresses for stack bounds (fp is a higher-half VA)
            printf("backtrace:\n");
            print_backtrace(fp, (uint64)PA2VA(KERNBASE), (uint64)-1ULL);
        }
        return;
    }
    printf("[Core: %d] In thread %d (%s) at %p\n", cpuid(), p->pid, p->name,
           (void *)fp);
    if (__bt_enabled) {
        size_t kstack_size = (1UL << (PAGE_SHIFT + p->kstack_order));
        printf("backtrace (kstack=%p size=%ld):\n",
               (void *)p->kstack, (long)kstack_size);
        if (fp >= p->kstack && fp < p->kstack + kstack_size) {
            print_backtrace(fp, p->kstack, p->kstack + kstack_size);
        } else {
            /* fp outside kstack — try wider range for useful backtrace */
            printf("  (fp outside kstack, using wider range)\n");
            print_backtrace(fp, (uint64)PA2VA(KERNBASE), (uint64)-1ULL);
        }
    }
}

void trigger_panic(void) {
    SET_CPU_CRASHED();
    // Mask all interrupts except software interrupt (IPI)
    w_sie(SIE_SSIE);

    // Send IPI to all harts(including self) to crash
    ipi_send_all(IPI_REASON_CRASH);

    // Enable interrupts so IPI can be received
    intr_on();

    for (;;)
        arch_wait_for_interrupt();
}

void __panic_end() __releases(__panic_msg_context)
{
    panic_msg_unlock();
    fb_panic_screen(__panic_textbuf);
    trigger_panic();
}

void printfinit(void) {
    spin_init(&pr.lock, "pr");
    pr.locking = 1;
}

void panic_msg_lock(void) __acquires(__panic_msg_context)
{
#ifdef __CHECKER__
    __acquire_context(__panic_msg_context);
#else
    spin_lock(&__panic_bt_lock);
#endif
}

void panic_msg_unlock(void) __releases(__panic_msg_context)
{
#ifdef __CHECKER__
    __release_context(__panic_msg_context);
#else
    spin_unlock(&__panic_bt_lock);
#endif
}
