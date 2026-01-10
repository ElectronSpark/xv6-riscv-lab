#ifndef __KERNEL_TIMER_TYPES_H
#define __KERNEL_TIMER_TYPES_H

#include "types.h"
#include "list_type.h"
#include "spinlock.h"
#include "bintree_type.h"

struct timer_root {
    struct rb_root root;
    list_node_t list_head;
    uint64 current_tick;
    uint64 next_tick;
    struct {
        uint64 valid: 1;
    };
    spinlock_t lock;
};

struct timer_node {
    struct rb_node rb;
    list_node_t list_entry;
    uint64 expires;
    int retry;
    int retry_limit;
    struct timer_root *timer;
    void (*callback)(struct timer_node*);
    void *data;
};

#endif // __KERNEL_TIMER_TYPES_H
