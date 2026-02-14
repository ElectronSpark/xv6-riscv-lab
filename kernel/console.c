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
#include "lock/mutex_types.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "proc/proc_private.h"
#include "mm/vm.h"
#include "dev/cdev.h"
#include "trap.h"
#include "dev/uart.h"
#include "sbi.h"
#include "signal.h"
#include "tty/tty.h"
#include "tty/session.h"
#include "vfs/pipe.h"

#ifndef CONSOLE_MAJOR
#define CONSOLE_MAJOR 1
#endif
#ifndef CONSOLE_MINOR
#define CONSOLE_MINOR 1
#endif

#define BACKSPACE 0x100
#define C(x) ((x) - '@') // Control-x

// Flag to track if UART has been initialized
// Before UART init, we use SBI for console output
static volatile int uart_initialized = 0;

// The console TTY — allocated during consoledevinit()
// Before this is set, consoleintr() falls back to the raw buffer.
struct tty *console_tty = NULL;

//
// send one character to the uart.
// called by printf(), and to echo input characters,
// but not from write().
//
void consputc(int c) {
    if (!uart_initialized) {
        // Use SBI for early console output before UART is ready
        if (c == BACKSPACE) {
            sbi_console_putchar('\b');
            sbi_console_putchar(' ');
            sbi_console_putchar('\b');
        } else {
            // Convert \n to \r\n for proper terminal output
            if (c == '\n')
                sbi_console_putchar('\r');
            sbi_console_putchar(c);
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
}

//
// send a string to the console.
// for use by puts() and optimized printing.
//
void consputs(const char *s, int n) {
    if (!uart_initialized) {
        // Use SBI for early console output before UART is ready
        for (int i = 0; i < n; i++) {
            if (s[i] == BACKSPACE) {
                sbi_console_putchar('\b');
                sbi_console_putchar(' ');
                sbi_console_putchar('\b');
            } else {
                // Convert \n to \r\n for proper terminal output
                if (s[i] == '\n')
                    sbi_console_putchar('\r');
                sbi_console_putchar(s[i]);
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
int consolewrite(cdev_t *cdev, bool user_src, const void *buffer, size_t n) {
    int i;
    uint64 src = (uint64)buffer;
    char kbuf[64];
    int written = 0;

    while (written < n) {
        int batch_size = n - written;
        if (batch_size > 64)
            batch_size = 64;

        for (i = 0; i < batch_size; i++) {
            if (either_copyin(&kbuf[i], user_src, src + written + i, 1) < 0)
                return written + i;
        }

        // Output with \n -> \r\n conversion for proper terminal display
        // Use interrupt-driven uartputc for efficiency
        for (i = 0; i < batch_size; i++) {
            if (kbuf[i] == '\n')
                uartputc('\r');
            uartputc(kbuf[i]);
        }
        written += batch_size;
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
static int consoleioctl(cdev_t *cdev, uint64 cmd, uint64 arg) {
    if (console_tty == NULL)
        return -ENOTTY;
    return tty_ioctl(console_tty, cmd, arg);
}

//
// Console device_t ioctl - same, but takes device_t* (called from
// dev_ioctl via vfs_ioctl).
//
static int console_dev_ioctl(device_t *dev, uint64 cmd, uint64 arg) {
    if (console_tty == NULL)
        return -ENOTTY;
    return tty_ioctl(console_tty, cmd, arg);
}

static cdev_ops_t console_cdev_ops = {
    .read = consoleread,
    .write = consolewrite,
    .open = consoleopen,
    .release = consoleclose,
    .ioctl = consoleioctl,
};

static cdev_t console_cdev = {
    .dev =
        {
            .major = CONSOLE_MAJOR,
            .minor = CONSOLE_MINOR,
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
 * The TTY line discipline writes echo output (^C, backspace sequences,
 * etc.) into the TTY output pipe. This thread drains that pipe and
 * sends bytes to the actual UART/SBI console.
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
        for (ssize_t i = 0; i < n; i++) {
            consputc((unsigned char)buf[i]);
        }
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
    assert(!IS_ERR_OR_NULL(console_tty), "consoledevinit: tty_alloc failed");

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
    if (!IS_ERR_OR_NULL(feeder))
        wakeup(feeder);

    /* Start the output drain thread (echo → UART) */
    struct thread *drain =
        kthread_create("tty_drain", console_tty_drain_thread, 0, 0, 0);
    if (!IS_ERR_OR_NULL(drain))
        wakeup(drain);

    // Start SBI polling thread if UART hardware not available
    if (!uart_initialized) {
        struct thread *p =
            kthread_create("sbi_console", sbi_console_poll_thread, 0, 0, 0);
        if (!IS_ERR_OR_NULL(p))
            wakeup(p);
    }
}
