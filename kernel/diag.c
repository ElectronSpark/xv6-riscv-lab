/**
 * diag.c — Diagnostic logging channel via COM2 (0x2F8)
 *
 * Provides dprintf(), a printf-like function whose output goes to the
 * second 16550 UART (COM2) instead of the console.  QEMU maps the
 * second -serial to COM2, so adding "-serial file:diag.log" sends all
 * dprintf() output to that file without cluttering the interactive
 * console.
 *
 * The driver is pure polled I/O (no interrupts, no buffers, no sleeping)
 * so it can be used from interrupt handlers and early boot code.
 */

#include <stdarg.h>
#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "diag.h"

/* ── COM2 port-I/O primitives (x86_64 only) ── */

#ifdef __x86_64__

#define COM2_BASE 0x2F8

/* Standard 16550 register offsets */
#define THR 0   /* Transmit Holding Register */
#define IER 1   /* Interrupt Enable Register */
#define FCR 2   /* FIFO Control Register */
#define LCR 3   /* Line Control Register */
#define MCR 4   /* Modem Control Register */
#define LSR 5   /* Line Status Register */

#define LSR_TX_IDLE (1 << 5)
#define LCR_8N1     0x03
#define LCR_DLAB    0x80
#define FCR_ENABLE  0x01
#define FCR_CLEAR   0x06
#define MCR_DTR     0x01
#define MCR_RTS     0x02
#define MCR_OUT2    0x08

static inline void diag_outb(uint16 port, uint8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8 diag_inb(uint16 port) {
    uint8 v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static spinlock_t diag_lock = SPINLOCK_INITIALIZED("diag");

void diaginit(void) {
    /* Standard 16550A initialisation for 115200 baud, 8N1, no interrupts */
    diag_outb(COM2_BASE + IER, 0x00);        /* disable all interrupts */
    diag_outb(COM2_BASE + LCR, LCR_DLAB);    /* enable DLAB (set baud) */
    diag_outb(COM2_BASE + 0,   0x01);        /* divisor low  (115200) */
    diag_outb(COM2_BASE + 1,   0x00);        /* divisor high */
    diag_outb(COM2_BASE + LCR, LCR_8N1);     /* 8 bits, no parity, 1 stop */
    diag_outb(COM2_BASE + FCR, FCR_ENABLE | FCR_CLEAR); /* enable & clear FIFO */
    diag_outb(COM2_BASE + MCR, MCR_DTR | MCR_RTS | MCR_OUT2);
}

/* Blocking single-char write — spins until TX holding register is empty */
static void diag_putc(int c) {
    while ((diag_inb(COM2_BASE + LSR) & LSR_TX_IDLE) == 0)
        ;
    diag_outb(COM2_BASE + THR, (uint8)c);
}

/* Write a buffer to COM2 with \n → \r\n conversion */
static void diag_puts(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i] == '\n')
            diag_putc('\r');
        diag_putc(s[i]);
    }
}

#else /* !__x86_64__ */

void diaginit(void) { /* no-op on non-x86 */ }

/* On non-x86 platforms, diag output is discarded */
static spinlock_t diag_lock = SPINLOCK_INITIALIZED("diag");
static void diag_puts(const char *s, int n) { (void)s; (void)n; }

#endif /* __x86_64__ */

/* ── Formatting engine (mirrors kernel printf.c) ── */

static char digits[] = "0123456789abcdef";

static void d_printint(long long xx, int base, int sign,
                       char *buf, int *len) {
    char tmp[24];
    int i = 0;
    unsigned long long x;

    if (sign && (sign = (xx < 0)))
        x = -xx;
    else
        x = xx;

    do {
        tmp[i++] = digits[x % base];
    } while ((x /= base) != 0);

    if (sign)
        tmp[i++] = '-';

    while (--i >= 0)
        buf[(*len)++] = tmp[i];
}

static void d_printptr(uint64 x, char *buf, int *len) {
    buf[(*len)++] = '0';
    buf[(*len)++] = 'x';
    for (int i = 0; i < (int)(sizeof(uint64) * 2); i++, x <<= 4)
        buf[(*len)++] = digits[x >> (sizeof(uint64) * 8 - 4)];
}

int dprintf(char *fmt, ...) {
    va_list ap;
    int i, cx, c0, c1, c2;
    char *s;
    char outbuf[512];
    int outlen = 0;

    spin_lock(&diag_lock);

    va_start(ap, fmt);
    for (i = 0; (cx = fmt[i] & 0xff) != 0; i++) {
        if (cx != '%') {
            outbuf[outlen++] = cx;
            if (outlen >= 500) {
                diag_puts(outbuf, outlen);
                outlen = 0;
            }
            continue;
        }
        i++;
        c0 = fmt[i] & 0xff;
        c1 = c2 = 0;
        if (c0) c1 = fmt[i + 1] & 0xff;
        if (c0 && c1) c2 = fmt[i + 2] & 0xff;

        if (c0 == 'd') {
            d_printint(va_arg(ap, int), 10, 1, outbuf, &outlen);
        } else if (c0 == 'l' && c1 == 'd') {
            d_printint(va_arg(ap, uint64), 10, 1, outbuf, &outlen);
            i++;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
            d_printint(va_arg(ap, uint64), 10, 1, outbuf, &outlen);
            i += 2;
        } else if (c0 == 'u') {
            d_printint(va_arg(ap, int), 10, 0, outbuf, &outlen);
        } else if (c0 == 'l' && c1 == 'u') {
            d_printint(va_arg(ap, uint64), 10, 0, outbuf, &outlen);
            i++;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
            d_printint(va_arg(ap, uint64), 10, 0, outbuf, &outlen);
            i += 2;
        } else if (c0 == 'x') {
            d_printint(va_arg(ap, int), 16, 0, outbuf, &outlen);
        } else if (c0 == 'l' && c1 == 'x') {
            d_printint(va_arg(ap, uint64), 16, 0, outbuf, &outlen);
            i++;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
            d_printint(va_arg(ap, uint64), 16, 0, outbuf, &outlen);
            i += 2;
        } else if (c0 == 'p') {
            d_printptr(va_arg(ap, uint64), outbuf, &outlen);
        } else if (c0 == 's') {
            s = va_arg(ap, char *);
            if (!s) s = "(null)";
            for (; *s; s++) {
                outbuf[outlen++] = *s;
                if (outlen >= 500) {
                    diag_puts(outbuf, outlen);
                    outlen = 0;
                }
            }
        } else if (c0 == '%') {
            outbuf[outlen++] = '%';
        } else if (c0 == 0) {
            break;
        } else {
            outbuf[outlen++] = '%';
            outbuf[outlen++] = c0;
        }
        if (outlen >= 500) {
            diag_puts(outbuf, outlen);
            outlen = 0;
        }
    }
    va_end(ap);

    if (outlen > 0)
        diag_puts(outbuf, outlen);

    spin_unlock(&diag_lock);
    return 0;
}
