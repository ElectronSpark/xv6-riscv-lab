#ifndef __KERNEL_TIMER_H
#define __KERNEL_TIMER_H

#include "timer_types.h"

#define TIMER_DEFAULT_RETRY_LIMIT 3

void timer_init(struct timer_root *timer);
void timer_tick(struct timer_root *timer, uint64 ticks);
void timer_node_init(struct timer_node *node, 
                     uint64 expires, 
                     void (*callback)(void *), 
                     void *data);
int timer_add(struct timer_root *timer, struct timer_node *node);
void timer_remove(struct timer_node *node);

uint64 get_jiffs(void);

#endif // __KERNEL_TIMER_H
