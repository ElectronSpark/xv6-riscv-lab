#ifndef __KERNEL_TIMER_H
#define __KERNEL_TIMER_H

#include "timer/timer_types.h"

#define TIMER_DEFAULT_RETRY_LIMIT 3

extern uint64 __timebase_frequency;
extern uint64 __jiff_ticks;
#define TIMEBASE_FREQUENCY __timebase_frequency
#define HZ 1000UL
#define JIFF_TICKS __jiff_ticks
#define TICK_MS (TIMEBASE_FREQUENCY / HZ)
#define TICK_S TIMEBASE_FREQUENCY

extern uint64 __clint_timer_irqno;
#define CLINT_TIMER_IRQ __clint_timer_irqno

void timer_init(struct timer_root *timer);
void timer_tick(struct timer_root *timer, uint64 ticks);
void timer_node_init(struct timer_node *node, 
                     uint64 expires, 
                     void (*callback)(struct timer_node *), 
                     void *data,
                     int retry_limit);
int timer_add(struct timer_root *timer, struct timer_node *node);
void timer_remove(struct timer_node *node);

// @TODO: consider overflow
uint64 get_jiffs(void);

#endif // __KERNEL_TIMER_H
