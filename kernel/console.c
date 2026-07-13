//
// Console input and output to UART.
// Routes through the TTY layer for line discipline, signal generation
// (Ctrl+C → SIGINT), and termios support.
// Uses SBI for early boot output before UART init.
// Converts \n to \r\n for proper terminal display.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "lock/mutex.h"
#include "lock/mutex_types.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "errno.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "proc/proc_private.h"
#include "mm/vm.h"
#include "dev/cdev.h"
#include "dev/console.h"
#include "trap.h"
#include "dev/uart.h"
#include "sbi.h"
#include "signal.h"
#include "tty/tty.h"
#include "tty/session.h"
#include "vfs/pipe.h"
#include <smp/percpu.h>

#ifndef CONSOLE_MAJOR
#define CONSOLE_MAJOR 1
#endif
#ifndef CONSOLE_MINOR
#define CONSOLE_MINOR 1
#endif

#define BACKSPACE 0x100
#define C(x) ((x) - '@') // Control-x

// Flag to track if UART has been initialized
// Before UART init, we use SBI (riscv) or direct COM1 (x86) for console output
static volatile int uart_initialized = 0;

#ifdef __x86_64__
/* Direct COM1 output for early x86 boot (before UART driver init) */
static inline void early_console_putchar(int c) {
    while (!(({uint8 v; asm volatile("inb %1,%0":"=a"(v):"Nd"((uint16)(0x3F8+5))); v;}) & 0x20))
        ;
    asm volatile("outb %0,%1"::"a"((uint8)c),"Nd"((uint16)0x3F8));
}
#else
static inline void early_console_putchar(int c) { sbi_console_putchar(c); }
#endif

// The console TTY — allocated during consoledevinit()
// Before this is set, consoleintr() falls back to the raw buffer.
struct tty *console_tty = NULL;

#ifdef __x86_64__
/*
 * Normal x86 serial producers share this sleepable mutex.  The UART's own
 * spinlock remains intentionally per character: holding it across a record
 * would leave interrupts disabled for tens of milliseconds.  Emergency
 * contexts cannot sleep, so they bypass this lock and advance the generation;
 * a concurrent record writer returns no-credit rather than claiming a clean
 * row it could not protect.
 */
#define CONSOLE_RECORD_LOCK_TIMEOUT_MS 50

static mutex_t console_wire_lock;
static volatile uint64 console_wire_emergency_generation;

static void console_wire_note_emergency(void)
{
    __atomic_fetch_add(&console_wire_emergency_generation, 1,
                       __ATOMIC_ACQ_REL);
}

static void console_uart_raw_putc(unsigned char c)
{
    if (!uart_initialized)
        early_console_putchar(c);
    else
        uartputc_sync(c);
}

static void console_wire_emit_raw_locked(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        console_uart_raw_putc((unsigned char)s[i]);
}

static void console_wire_emit_text_locked(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i] == BACKSPACE) {
            console_uart_raw_putc('\b');
            console_uart_raw_putc(' ');
            console_uart_raw_putc('\b');
        } else {
            if (s[i] == '\n')
                console_uart_raw_putc('\r');
            console_uart_raw_putc((unsigned char)s[i]);
        }
    }
}

static int console_wire_can_sleep(void)
{
    return uart_initialized && !panic_state() && current != NULL &&
           !CPU_IN_ITR() && spin_depth_snapshot() == 0;
}

static void console_wire_emit_raw(const char *s, int n)
{
    if (!console_wire_can_sleep()) {
        console_wire_note_emergency();
        console_wire_emit_raw_locked(s, n);
        return;
    }

    mutex_lock(&console_wire_lock);
    console_wire_emit_raw_locked(s, n);
    mutex_unlock(&console_wire_lock);
}

static void console_wire_emit_text(const char *s, int n)
{
    if (!console_wire_can_sleep()) {
        console_wire_note_emergency();
        console_wire_emit_text_locked(s, n);
        return;
    }

    mutex_lock(&console_wire_lock);
    console_wire_emit_text_locked(s, n);
    mutex_unlock(&console_wire_lock);
}

static int console_record_write_ioctl(void *arg)
{
    struct console_record_write_v1 request;
    char record[CONSOLE_RECORD_MAX_INPUT_BYTES];
    uint64 emergency_before;
    int ret;

    if (current == NULL || current->thread_group == NULL ||
        current->thread_group->euid != 0)
        return -EPERM;
    if (arg == NULL ||
        either_copyin(&request, 1, (uint64)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.version != CONSOLE_RECORD_ABI_VERSION || request.flags != 0 ||
        request.reserved != 0 || request.data_ptr == 0 ||
        request.data_len == 0 ||
        request.data_len > CONSOLE_RECORD_MAX_INPUT_BYTES)
        return -EINVAL;
    if (either_copyin(record, 1, request.data_ptr, request.data_len) < 0)
        return -EFAULT;
    if (!console_record_wire_text_valid(record, request.data_len))
        return -EINVAL;

    /* Every user byte is copied and checked before this sleepable lock. */
    if (!uart_initialized || panic_state())
        return -EAGAIN;
    ret = mutex_lock_timed(&console_wire_lock, CONSOLE_RECORD_LOCK_TIMEOUT_MS);
    if (ret != 0)
        return ret;

    emergency_before = __atomic_load_n(&console_wire_emergency_generation,
                                       __ATOMIC_ACQUIRE);
    if (!uart_initialized || panic_state()) {
        ret = -EAGAIN;
    } else {
        console_wire_emit_text_locked(record, (int)request.data_len);
        if (emergency_before !=
            __atomic_load_n(&console_wire_emergency_generation,
                            __ATOMIC_ACQUIRE))
            ret = -EAGAIN; /* row may have been dirtied by an emergency bypass */
        else
            ret = (int)request.data_len;
    }
    /* The timed acquisition above has exactly this one release path. */
    mutex_unlock(&console_wire_lock);
    return ret;
}

static int console_record_batch_write_ioctl(void *arg)
{
    struct console_record_batch_write_v1 request;
    char *records = NULL;
    uint64 batch_generation;
    console_u32 terminal_rc_record_len = 0;
    console_u32 record_start;
    console_u32 records_emitted;
    int terminal_commit;
    int ret;

    if (current == NULL || current->thread_group == NULL ||
        current->thread_group->euid != 0)
        return -EPERM;
    if (arg == NULL ||
        either_copyin(&request, 1, (uint64)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.version != CONSOLE_RECORD_BATCH_ABI_VERSION ||
        (request.flags != 0 &&
         request.flags != CONSOLE_RECORD_BATCH_F_TERMINAL_COMMIT) ||
        request.reserved0 != 0 ||
        request.reserved1 != 0 || request.data_ptr == 0 ||
        request.data_len == 0 ||
        request.data_len > CONSOLE_RECORD_BATCH_MAX_LOGICAL_BYTES ||
        request.record_count == 0 ||
        request.record_count > CONSOLE_RECORD_BATCH_MAX_RECORDS ||
        (uint64)request.data_len + (uint64)request.record_count >
            CONSOLE_RECORD_BATCH_MAX_PHYSICAL_BYTES)
        return -EINVAL;
    terminal_commit =
        request.flags == CONSOLE_RECORD_BATCH_F_TERMINAL_COMMIT;

    records = kvmalloc(request.data_len);
    if (records == NULL)
        return -ENOMEM;
    if (either_copyin(records, 1, request.data_ptr, request.data_len) < 0) {
        ret = -EFAULT;
        goto out_free;
    }
    if (terminal_commit ?
            !console_record_terminal_commit_wire_text_valid(
                records, request.data_len, request.record_count, NULL, 0,
                &terminal_rc_record_len) :
            !console_record_batch_wire_text_valid(
                records, request.data_len, request.record_count)) {
        ret = -EINVAL;
        goto out_free;
    }

    /* Metadata, payload, and every record boundary are fixed before locking. */
    if (!uart_initialized || panic_state()) {
        ret = -EAGAIN;
        goto out_free;
    }
    ret = mutex_lock_timed(&console_wire_lock,
                           CONSOLE_RECORD_LOCK_TIMEOUT_MS);
    if (ret != 0)
        goto out_free;

    batch_generation = __atomic_load_n(&console_wire_emergency_generation,
                                        __ATOMIC_ACQUIRE);
    if (!uart_initialized || panic_state()) {
        ret = -EAGAIN;
        goto out_unlock;
    }

    if (terminal_commit) {
        /*
         * RC is still provisional.  An emergency bypass before or during it
         * prevents FENCE.  Immediately before FENCE, recheck both the
         * generation and UART availability.  Once FENCE emission starts the
         * ioctl is committed: a concurrent emergency may contaminate that
         * final row (which the host rejects), but no later check may turn a
         * complete clean terminal into an error.
         */
        console_wire_emit_text_locked(records, (int)terminal_rc_record_len);
        if (__atomic_load_n(&console_wire_emergency_generation,
                            __ATOMIC_ACQUIRE) != batch_generation) {
            ret = -EAGAIN;
            goto out_unlock;
        }
        if (!uart_initialized || panic_state() ||
            __atomic_load_n(&console_wire_emergency_generation,
                            __ATOMIC_ACQUIRE) != batch_generation) {
            ret = -EAGAIN;
            goto out_unlock;
        }
        console_wire_emit_text_locked(
            records + terminal_rc_record_len,
            (int)(request.data_len - terminal_rc_record_len));
        ret = (int)request.data_len;
        goto out_unlock;
    }

    record_start = 0;
    records_emitted = 0;
    while (record_start < request.data_len) {
        console_u32 record_end = record_start;
        uint64 emergency_before;
        uint64 emergency_after;

        while (record_end < request.data_len && records[record_end] != '\n')
            record_end++;
        if (record_end >= request.data_len) {
            ret = -EINVAL; /* defensive: validation above already found every LF */
            goto out_unlock;
        }
        record_end++;

        emergency_before =
            __atomic_load_n(&console_wire_emergency_generation,
                            __ATOMIC_ACQUIRE);
        if (emergency_before != batch_generation) {
            ret = -EAGAIN;
            goto out_unlock;
        }
        console_wire_emit_text_locked(records + record_start,
                                      (int)(record_end - record_start));
        emergency_after =
            __atomic_load_n(&console_wire_emergency_generation,
                            __ATOMIC_ACQUIRE);
        if (emergency_after != emergency_before) {
            ret = -EAGAIN; /* emitted envelope may have emergency wire dirt */
            goto out_unlock;
        }

        records_emitted++;
        record_start = record_end;
    }

    if (records_emitted != request.record_count ||
        __atomic_load_n(&console_wire_emergency_generation,
                        __ATOMIC_ACQUIRE) != batch_generation)
        ret = -EAGAIN;
    else
        ret = (int)request.data_len;

out_unlock:
    mutex_unlock(&console_wire_lock);
out_free:
    kvfree(records);
    return ret;
}
#endif /* __x86_64__ */

//
// send one character to the uart.
// called by printf(), and to echo input characters,
// but not from write().
//
void consputc(int c) {
#ifdef __x86_64__
    if (c == BACKSPACE) {
        const char erase[] = {'\b', ' ', '\b'};
        console_wire_emit_raw(erase, sizeof(erase));
        return;
    }
    char text = (char)c;
    console_wire_emit_text(&text, 1);
    return;
#else
    if (!uart_initialized) {
        // Use early console output before UART is ready
        if (c == BACKSPACE) {
            early_console_putchar('\b');
            early_console_putchar(' ');
            early_console_putchar('\b');
        } else {
            // Convert \n to \r\n for proper terminal output
            if (c == '\n')
                early_console_putchar('\r');
            early_console_putchar(c);
        }
        return;
    }

    // Use synchronous UART output (safe for interrupt context, like
    // xv6-OrangePi_RV2)
    if (c == BACKSPACE) {
        // if the user typed backspace, overwrite with a space.
        uartputc_sync('\b');
        uartputc_sync(' ');
        uartputc_sync('\b');
    } else {
        // Convert \n to \r\n for proper terminal output
        if (c == '\n')
            uartputc_sync('\r');
        uartputc_sync(c);
    }
#endif
}

//
// send a string to the console.
// for use by puts() and optimized printing.
//
void consputs(const char *s, int n) {
#ifdef __x86_64__
    console_wire_emit_text(s, n);
    return;
#else
    if (!uart_initialized) {
        // Use early console output before UART is ready
        for (int i = 0; i < n; i++) {
            if (s[i] == BACKSPACE) {
                early_console_putchar('\b');
                early_console_putchar(' ');
                early_console_putchar('\b');
            } else {
                // Convert \n to \r\n for proper terminal output
                if (s[i] == '\n')
                    early_console_putchar('\r');
                early_console_putchar(s[i]);
            }
        }
        return;
    }

    for (int i = 0; i < n; i++) {
        if (s[i] == BACKSPACE) {
            uartputc_sync('\b');
            uartputc_sync(' ');
            uartputc_sync('\b');
        } else {
            // Convert \n to \r\n for proper terminal output
            if (s[i] == '\n')
                uartputc_sync('\r');
            uartputc_sync(s[i]);
        }
    }
#endif
}

struct {
    spinlock_t lock;

    // input
#define INPUT_BUF_SIZE 128
    char buf[INPUT_BUF_SIZE];
    uint r; // Read index
    uint w; // Write index
    uint e; // Edit index
} cons;

//
// user write()s to the console go here.
//
// When the TTY is active, output post-processing (OPOST/ONLCR) is
// controlled by the TTY's termios flags.  On x86, keep user writes on the
// synchronous COM path used by kernel printf(): Hyper-V named-pipe serial is
// reliable for polling output but may not deliver TX-empty interrupts, and a
// blocked stderr write can stop init/desktop before the framebuffer repaints.
//
int consolewrite(cdev_t *cdev, bool user_src, const void *buffer, size_t n) {
    int i;
    uint64 src = (uint64)buffer;
    char kbuf[64];
    char outbuf[128]; // room for ONLCR expansion (worst case 2x)
    int written = 0;

    /* Check OPOST flags from the TTY (if active) */
    int do_onlcr = 1; /* default: convert \n → \r\n */
    if (console_tty != NULL) {
        spin_lock(&console_tty->lock);
        do_onlcr = (console_tty->termios.c_oflag & OPOST) &&
                    (console_tty->termios.c_oflag & ONLCR);
        spin_unlock(&console_tty->lock);
    }

    while (written < (int)n) {
        int batch_size = n - written;
        if (batch_size > 64)
            batch_size = 64;

        for (i = 0; i < batch_size; i++) {
            if (either_copyin(&kbuf[i], user_src, src + written + i, 1) < 0)
                return written > 0 ? written : -EFAULT;
        }

        // Expand into output buffer
        int olen = 0;
        for (i = 0; i < batch_size; i++) {
            if (do_onlcr && kbuf[i] == '\n')
                outbuf[olen++] = '\r';
            outbuf[olen++] = kbuf[i];
        }

#ifdef __x86_64__
        /* TTY post-processing already produced exact bytes in outbuf. */
        console_wire_emit_raw(outbuf, olen);
        written += batch_size;
#else
        // Submit output, waiting interruptibly when the TX buffer is full
        int sent = 0;
        while (sent < olen) {
            int accepted = uartputs_nb(outbuf + sent, olen - sent);
            sent += accepted;
            if (sent < olen) {
                int ret = uart_tx_wait();
                if (ret != 0) {
                    // Interrupted by signal — return partial write count.
                    // We consumed this batch from userspace, so count it.
                    written += batch_size;
                    return written > 0 ? written : -EINTR;
                }
            }
        }
        written += batch_size;
#endif
    }

    return written;
}

//
// user read()s from the console go here.
// When the TTY is active, reads go through the TTY line discipline.
// Before TTY init, falls back to the raw console buffer.
//
int consoleread(cdev_t *cdev, bool user_dst, void *buffer, size_t n) {
    if (console_tty != NULL)
        return tty_read(console_tty, (char *)buffer, n, user_dst);

    /* --- Early boot fallback (before TTY is allocated) --- */
    uint target;
    int c;
    char kbuf[64];
    int batch_count;
    uint64 dst = (uint64)buffer;
    int got_newline = 0;
    int got_eof = 0;

    target = n;
    spin_lock(&cons.lock);
    while (n > 0 && !got_newline && !got_eof) {
        batch_count = 0;

        // Collect up to 64 chars (or until newline/EOF) while holding lock
        while (n > 0 && batch_count < 64 && !got_newline && !got_eof) {
            // wait until interrupt handler has put some input into cons.buffer.
            while (cons.r == cons.w) {
                if (killed(current)) {
                    spin_unlock(&cons.lock);
                    // Copy any pending data before returning
                    if (batch_count > 0) {
                        if (either_copyout(user_dst, dst, kbuf, batch_count) <
                            0)
                            return target - n; // return bytes read so far
                        dst += batch_count;
                        n -= batch_count;
                    }
                    return target - n;
                }
                sleep_on_chan(&cons.r, &cons.lock);
            }

            c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

            if (c == C('D')) { // end-of-file
                if (n < target || batch_count > 0) {
                    // Save ^D for next time, to make sure
                    // caller gets a 0-byte result.
                    cons.r--;
                }
                got_eof = 1;
                break;
            }

            kbuf[batch_count++] = c;
            n--;

            if (c == '\n') {
                // a whole line has arrived
                got_newline = 1;
                break;
            }
        }

        // Release lock before copying to userspace
        spin_unlock(&cons.lock);

        // Copy batch to userspace (without holding spinlock)
        if (batch_count > 0) {
            if (either_copyout(user_dst, dst, kbuf, batch_count) < 0) {
                // Copy failed, return what we've read so far
                return target - n - batch_count;
            }
            dst += batch_count;
        }

        // Re-acquire lock if we need to continue
        if (n > 0 && !got_newline && !got_eof) {
            spin_lock(&cons.lock);
        }
    }

    // If we exited the loop while holding the lock, release it
    // (This happens if we hit EOF or newline in the first iteration)
    // Actually, we always release the lock inside the loop before copying,
    // so we should not be holding it here.

    return target - n;
}

static int consoleopen(cdev_t *cdev) { return 0; }

static int consoleclose(cdev_t *cdev) { return 0; }

//
// Console ioctl - delegates to the TTY layer for termios/TIOCSPGRP/etc.
//
static int console_ioctl_common(uint64 cmd, void *arg) {
#ifdef __x86_64__
    if (cmd == CONSOLE_IOC_WRITE_RECORD)
        return console_record_write_ioctl(arg);
    if (cmd == CONSOLE_IOC_WRITE_RECORD_BATCH)
        return console_record_batch_write_ioctl(arg);
#endif
    if (console_tty == NULL)
        return -ENOTTY;
    return tty_ioctl(console_tty, cmd, arg);
}

static int consoleioctl(cdev_t *cdev, uint64 cmd, void *arg) {
    (void)cdev;
    return console_ioctl_common(cmd, arg);
}

//
// Console device_t ioctl - same, but takes device_t* (called from
// dev_ioctl via vfs_ioctl).
//
static int console_dev_ioctl(device_t *dev, uint64 cmd, void *arg) {
    (void)dev;
    return console_ioctl_common(cmd, arg);
}

//
// Console poll - check whether console has data ready for reading / writing.
// Delegates to tty_poll which inspects raw_buf (raw mode) or input_pipe
// (canonical mode).
//
static int consolepoll(cdev_t *cdev, short events) {
    if (console_tty == NULL)
        return 0;
    return tty_poll(console_tty, events);
}

static cdev_ops_t console_cdev_ops = {
    .read = consoleread,
    .write = consolewrite,
    .open = consoleopen,
    .release = consoleclose,
    .ioctl = consoleioctl,
    .poll = consolepoll,
};

static cdev_t console_cdev = {
    .dev =
        {
            .major = CONSOLE_MAJOR,
            .minor = CONSOLE_MINOR,
            .devname = "console",
            .devmode = S_IFCHR | 0666,
        },
    .readable = 1,
    .writable = 1,
};

extern void uartintr(int irq, void *data, device_t *dev);

/*
 * TTY input staging buffer.
 *
 * consoleintr() runs in interrupt context and cannot call tty_input()
 * directly because the TTY line discipline may sleep (pipe_write).
 * Instead, we put raw characters into this lock-free ring buffer and
 * a kernel thread (console_tty_input_thread) drains it into tty_input().
 */
#define TTY_INBUF_SIZE 256
static volatile char tty_inbuf[TTY_INBUF_SIZE];
static volatile uint tty_inbuf_w = 0; /* write index (interrupt) */
static volatile uint tty_inbuf_r = 0; /* read index  (thread)   */

//
// the console input interrupt handler.
// uartintr() calls this for input character.
//
// When the TTY layer is active, characters are staged in tty_inbuf
// and a kernel thread feeds them to tty_input().
//
// Before TTY init, falls back to the raw console buffer with
// basic editing.
//
void consoleintr(int c) {
    /* ---- TTY path: stage in ring buffer for deferred processing ---- */
    if (console_tty != NULL) {
        /* ^P still handled here for kernel debugging */
        if (c == C('P')) {
            procdump();
            return;
        }
        if (c == C('B')) {
            procdump_bt();
            return;
        }
        if (c == C('O')) {
            int irq_state = sleep_lock_irqsave();
            scheduler_dump_chan_queue();
            sleep_unlock_irqrestore(irq_state);
            return;
        }
        uint w = tty_inbuf_w;
        uint next = (w + 1) % TTY_INBUF_SIZE;
        if (next != tty_inbuf_r) { /* drop if full */
            tty_inbuf[w] = (char)c;
            __atomic_store_n(&tty_inbuf_w, next, __ATOMIC_RELEASE);
        }
        return;
    }

    /* ---- Early boot fallback (raw buffer) ---- */
    spin_lock(&cons.lock);

    switch (c) {
    case C('P'): // Print process list.
        procdump();
        break;
    case C('B'):
        procdump_bt();
        break;
    case C('O'): {
        int irq_state = sleep_lock_irqsave();
        scheduler_dump_chan_queue();
        sleep_unlock_irqrestore(irq_state);
        break;
    }
    case C('U'): // Kill line.
        while (cons.e != cons.w &&
               cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] != '\n') {
            cons.e--;
            consputc(BACKSPACE);
        }
        break;
    case C('H'): // Backspace
    case '\x7f': // Delete key
        if (cons.e != cons.w) {
            cons.e--;
            consputc(BACKSPACE);
        }
        break;
    default:
        if (c != 0 && cons.e - cons.r < INPUT_BUF_SIZE) {
            c = (c == '\r') ? '\n' : c;

            // echo back to the user.
            if (c == '\x1b')
                consputc('['); // Escape sequence start
            else if (c == '\t')
                consputc(' '); // Convert tab to space for simplicity
            else if ((c < 32 || c > 126) &&
                     c != '\n') // Non-printable characters
                consputc('?');  // Replace with '?'
            else
                consputc(c);

            // store for consumption by consoleread().
            cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

            if (c == '\n' || c == '\t' || c == C('D') ||
                cons.e - cons.r == INPUT_BUF_SIZE) {
                // wake up consoleread() if a whole line (or end-of-file)
                // has arrived.
                cons.w = cons.e;
                wakeup_on_chan(&cons.r);
            }
        }
        break;
    }

    spin_unlock(&cons.lock);
}

void consoleinit(void) {
    spin_init(&cons.lock, "cons");
#ifdef __x86_64__
    mutex_init(&console_wire_lock, "console_wire");
#endif

    // Try to initialize UART hardware
    // Returns 1 if successful (QEMU), 0 if deferred (real hardware uses SBI)
    if (uartinit()) {
        // Mark UART as initialized - switch from SBI to UART output
        uart_initialized = 1;
    }
    // If uartinit returned 0, keep uart_initialized = 0 to continue using SBI
}

// SBI console input polling thread
// On non-QEMU platforms where UART hardware isn't used, we poll SBI for input
static void sbi_console_poll_thread(uint64 arg1, uint64 arg2) {
    (void)arg1;
    (void)arg2;
    for (;;) {
        // Read all available characters in a batch
        int got_input = 0;
        for (int i = 0; i < 32; i++) { // Read up to 32 chars per cycle
            int c = sbi_console_getchar();
            if (c >= 0) {
                consoleintr(c);
                got_input = 1;
            } else {
                break; // No more input available
            }
        }

        if (!got_input) {
            // No input available, sleep briefly to avoid busy-waiting
            // Use 1ms for responsive interactive typing
            sleep_ms(1);
        }
        // If we got input, immediately check for more without sleeping
    }
}

/*
 * TTY input feeder thread.
 *
 * Drains the tty_inbuf ring buffer (filled by consoleintr in interrupt
 * context) and feeds characters to tty_input() which can safely sleep.
 */
static void console_tty_input_thread(uint64 arg1, uint64 arg2) {
    (void)arg1;
    (void)arg2;

    for (;;) {
        uint r = __atomic_load_n(&tty_inbuf_r, __ATOMIC_ACQUIRE);
        uint w = __atomic_load_n(&tty_inbuf_w, __ATOMIC_ACQUIRE);

        if (r == w) {
            /* Nothing to process — poll every 1 ms */
            sleep_ms(1);
            continue;
        }

        /* Drain available characters into the TTY line discipline */
        while (r != w) {
            char ch = tty_inbuf[r];
            r = (r + 1) % TTY_INBUF_SIZE;
            __atomic_store_n(&tty_inbuf_r, r, __ATOMIC_RELEASE);

            tty_input(console_tty, &ch, 1);

            /* Re-read w in case more arrived */
            w = __atomic_load_n(&tty_inbuf_w, __ATOMIC_ACQUIRE);
        }
    }
}

/*
 * TTY output drain thread.
 *
 * Drains the TTY output pipe and sends bytes to the UART/SBI console.
 * Only echo output (from tty_echo_char in the line discipline) flows
 * through this pipe.  User writes go directly to UART via consolewrite.
 *
 * Data arriving from the pipe has already been post-processed by
 * tty_echo_char (OPOST/ONLCR), so bytes are output verbatim — no
 * additional \n → \r\n conversion is performed.
 */
static void console_tty_drain_thread(uint64 arg1, uint64 arg2) {
    (void)arg1;
    (void)arg2;
    char buf[64];

    for (;;) {
        /* tty_output blocks if no data is available */
        ssize_t n = tty_output(console_tty, buf, sizeof(buf));
        if (n <= 0) {
            sleep_ms(1);
            continue;
        }
#ifdef __x86_64__
        /* TTY has already performed ONLCR; send its bytes verbatim. */
        console_wire_emit_raw(buf, (int)n);
#else
        for (ssize_t i = 0; i < n; i++) {
            if (!uart_initialized)
                early_console_putchar((unsigned char)buf[i]);
            else
                uartputc_sync((unsigned char)buf[i]);
        }
#endif
    }
}

void consoledevinit(void) {
    console_cdev.ops = console_cdev_ops;
    int errno = cdev_register(&console_cdev);
    assert(errno == 0, "consoleinit: cdev_register failed: %d\n", errno);

    /* Install ioctl on the device_t level (used by vfs_ioctl → dev_ioctl) */
    console_cdev.dev.ops.ioctl = console_dev_ioctl;

    struct irq_desc uart_irq_desc = {
        .handler = uartintr,
        .data = NULL,
        .dev = &console_cdev.dev,
    };
    errno = register_irq_handler(PLIC_IRQ(UART0_IRQ), &uart_irq_desc);
    assert(errno == 0,
           "consoledevinit: register_irq_handler failed, error code: %d\n",
           errno);

    /* ---- Allocate the console TTY ---- */
    console_tty = tty_alloc("console", NULL);

    /*
     * Make both TTY pipes non-blocking for writes so that tty_input()
     * (called from the feeder thread) and tty_echo_char() never sleep
     * when the pipe is full — characters are silently discarded instead.
     */
    pipe_set_flags(console_tty->input_pipe, (1 << PIPE_FLAGS_NONBLOCK_WR));
    pipe_set_flags(console_tty->output_pipe, (1 << PIPE_FLAGS_NONBLOCK_WR));

    /* Attach the console TTY to init's session as the controlling terminal */
    struct thread *initproc = __proctab_get_initproc();
    if (initproc != NULL && initproc->session != NULL) {
        session_set_ctrl_tty(initproc->session, console_tty);
    }

    /* Start the input feeder thread (intr ring buf → tty_input) */
    struct thread *feeder =
        kthread_create("tty_input", console_tty_input_thread, 0, 0, 0);
    if (!IS_ERR_OR_NULL(feeder)) {
        feeder->sched_entity->priority = MAKE_PRIORITY(16, 0);
        wakeup(feeder);
    }

    /* Start the output drain thread (echo → UART) */
    struct thread *drain =
        kthread_create("tty_drain", console_tty_drain_thread, 0, 0, 0);
    if (!IS_ERR_OR_NULL(drain)) {
        drain->sched_entity->priority = MAKE_PRIORITY(16, 0);
        wakeup(drain);
    }

    // Start SBI polling thread if UART hardware not available
    if (!uart_initialized) {
        struct thread *p =
            kthread_create("sbi_console", sbi_console_poll_thread, 0, 0, 0);
        if (!IS_ERR_OR_NULL(p))
            wakeup(p);
    }
}
