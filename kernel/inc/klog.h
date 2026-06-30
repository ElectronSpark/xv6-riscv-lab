#ifndef __KERNEL_KLOG_H
#define __KERNEL_KLOG_H

#include "types.h"

#define KLOG_F_RING    (1U << 0)
#define KLOG_F_CONSOLE (1U << 1)

void kloginit(void);
void klog_write(const char *buf, int len, uint flags);
uint64 klog_dropped(void);

#endif /* __KERNEL_KLOG_H */
