#ifndef __KERNEL_SOFT_INTERRUPT_TYPES_H
#define __KERNEL_SOFT_INTERRUPT_TYPES_H

#include "compiler.h"
#include "types.h"
#include "list_type.h"
#include "lock/spinlock.h"

// Softirqs are a mechanism for deferring work in the kernel to be executed
// The types and orders are copied from Linux
enum softirq_type {
    SOFTIRQ_TYPE_HI_SOFTIRQ = 0,
    SOFTIRQ_TYPE_TIMER,
    SOFTIRQ_TYPE_NET_TX,
    SOFTIRQ_TYPE_NET_RX,
    SOFTIRQ_TYPE_BLOCK,
    SOFTIRQ_TYPE_IRQ_POLL,
    SOFTIRQ_TYPE_TASKLET,
    SOFTIRQ_TYPE_SCHED,
    SOFTIRQ_TYPE_HRTIMER,
    SOFTIRQ_TYPE_RCU,
    SOFTIRQ_TYPE_MAX,
};

// I will use 16 bits ffs
BUILD_BUG_ON(SOFTIRQ_TYPE_MAX > 16);

struct softirq_action {
    void (*handler)(void);
};

struct softirq {
    uint64 pending; // Bitmap of pending softirqs
    struct softirq_action actions[SOFTIRQ_TYPE_MAX]; // Array of softirq actions
} __ALIGNED_CACHELINE;

#endif       /* __KERNEL_SOFT_INTERRUPT_TYPES_H */
