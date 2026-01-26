#ifndef __KERNEL_SOFT_INTERRUPT_H
#define __KERNEL_SOFT_INTERRUPT_H

#include "softirq_types.h"

void softirq_init(void);
void raise_softirq(enum softirq_type type);
void raise_ip_softirq(enum softirq_type type, int cpu_id);
void do_softirqs(void);
void open_softirq(enum softirq_type type, void (*handler)(void));

#endif      /* __KERNEL_SOFT_INTERRUPT_H */
