#ifndef __KERNEL_KLOG_H
#define __KERNEL_KLOG_H

#include "types.h"

#define KLOG_RING_SIZE 16384

#define KLOG_F_RING    (1U << 0)
#define KLOG_F_CONSOLE (1U << 1)

void kloginit(void);
int klog_ring_enabled(void);
void klog_write(const char *buf, int len, uint flags);
size_t klog_capacity(void);
size_t klog_size(void);
size_t klog_snapshot(char *dst, size_t max, uint64 *dropped);
void klog_clear(void);
uint64 klog_dropped(void);

/* Asynchronous console output (klog.c).  console_async_init spawns the
 * "consoled" drain kthread (no-op with console_async=0 on the cmdline);
 * console_async_panic_flush synchronously drains staged bytes and
 * disables the async path — called from __panic_start(). */
void console_async_init(void);
void console_async_panic_flush(void);
uint64 console_async_dropped(void);

#endif /* __KERNEL_KLOG_H */
