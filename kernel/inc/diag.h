/**
 * @file diag.h
 * @brief Diagnostic logging via secondary UART (COM2 on x86_64)
 *
 * dprintf() works like printf() but outputs to a dedicated diagnostic
 * serial port (COM2 / 0x2F8) so that diagnostic messages don't
 * interleave with normal console output.  In QEMU the second serial
 * is redirected to a file (diag.log) via -serial file:diag.log.
 *
 * On non-x86_64 builds, dprintf() silently discards output.
 */

#ifndef __KERNEL_DIAG_H
#define __KERNEL_DIAG_H

/**
 * @brief Initialize the diagnostic UART (COM2).
 * Call once during early boot, after printfinit().
 */
void diaginit(void);

/**
 * @brief Diagnostic printf — output goes to COM2 (second serial port).
 * @param fmt  printf-style format string
 * @return 0
 *
 * Safe to call from interrupt context (polled I/O, no sleeping).
 * Uses its own spinlock to serialise output.
 */
int dprintf(char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* __KERNEL_DIAG_H */
