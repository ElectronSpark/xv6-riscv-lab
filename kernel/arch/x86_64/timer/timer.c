#include "types.h"
#include "arch/timer.h"
#include "timer/timer.h"

uint64 __clint_timer_irqno = 0;
uint64 __timebase_frequency = 10000000UL;
uint64 __jiff_ticks = 10000UL;
uint64 __goldfish_rtc_mmio_base = 0;
uint64 __goldfish_rtc_irqno = 0;

void timer_init(struct timer_root *timer) { (void)timer; }

void timer_tick(struct timer_root *timer, uint64 ticks) {
    (void)timer;
    (void)ticks;
}

void timer_node_init(struct timer_node *node, uint64 expires,
                     void (*callback)(struct timer_node *), void *data,
                     int retry_limit) {
    (void)node;
    (void)expires;
    (void)callback;
    (void)data;
    (void)retry_limit;
}

int timer_add(struct timer_root *timer, struct timer_node *node) {
    (void)timer;
    (void)node;
    return 0;
}

void timer_remove(struct timer_node *node) { (void)node; }

uint64 get_jiffs(void) { return 0; }

void goldfish_rtc_init(void) {}

uint64 goldfish_rtc_read_ns(void) { return 0; }

uint64 goldfish_rtc_read_sec(void) { return 0; }

void goldfish_rtc_set_alarm_ns(uint64 ns) { (void)ns; }

void goldfish_rtc_set_alarm_sec(uint64 sec) { (void)sec; }

void goldfish_rtc_clear_alarm(void) {}

void goldfish_rtc_irq_enable(int enable) { (void)enable; }

uint64 goldfish_rtc_get_alarm_count(void) { return 0; }

uint64 __plic_mmio_base = 0;

void arch_timer_init(void) {
    // Stub timer init for x86_64
}

void arch_timer_init_hart(void) {
    // Stub timer init hart for x86_64
}