/**
 * @file timer.c
 * @brief x86_64 PIT timer and timer subsystem.
 *
 * Provides a basic 100 Hz tick using the 8254 PIT (channel 0, mode 3).
 * Also provides the timer tree stubs needed by the shared kernel code.
 *
 * The goldfish_rtc and CLINT symbols are provided as no-op stubs to
 * satisfy linker references from shared RISC-V code that hasn't been
 * fully abstracted yet.
 */

#include "types.h"
#include "arch/timer.h"
#include "timer/timer.h"
#include "x86.h"
#include "printf.h"

/* ── Linker-compat globals (RISC-V legacy) ── */
uint64 __clint_timer_irqno = 0;
uint64 __timebase_frequency = 10000000UL;
uint64 __jiff_ticks = 10000UL;
uint64 __goldfish_rtc_mmio_base = 0;
uint64 __goldfish_rtc_irqno = 0;
uint64 __plic_mmio_base = 0;

/* ── PIT constants ── */
#define PIT_FREQ       1193182UL
#define PIT_HZ         100        /* 100 Hz = 10 ms per tick */
#define PIT_DIVISOR    (PIT_FREQ / PIT_HZ)

#define PIT_CH0_DATA   0x40
#define PIT_CMD        0x43

/* I/O port helpers */
static inline void outb(uint16 port, uint8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8 inb(uint16 port) {
    uint8 val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ── Jiffies counter ── */
static volatile uint64 jiffies_count = 0;

uint64 get_jiffs(void) { return jiffies_count; }

void timer_tick_advance(void) {
    __atomic_fetch_add(&jiffies_count, 1, __ATOMIC_RELAXED);
}

/* ── Timer tree (per-CPU software timers) ── */

void timer_init(struct timer_root *timer) { (void)timer; }

void timer_tick(struct timer_root *timer, uint64 ticks) {
    (void)timer;
    (void)ticks;
}

void timer_node_init(struct timer_node *node, uint64 expires,
                     void (*callback)(struct timer_node *), void *data,
                     int retry_limit) {
    (void)node; (void)expires; (void)callback; (void)data; (void)retry_limit;
}

int timer_add(struct timer_root *timer, struct timer_node *node) {
    (void)timer; (void)node;
    return 0;
}

void timer_remove(struct timer_node *node) { (void)node; }

/* ── Goldfish RTC stubs ── */
void   goldfish_rtc_init(void) {}
uint64 goldfish_rtc_read_ns(void)  { return 0; }
uint64 goldfish_rtc_read_sec(void) { return 0; }
void   goldfish_rtc_set_alarm_ns(uint64 ns)   { (void)ns; }
void   goldfish_rtc_set_alarm_sec(uint64 sec)  { (void)sec; }
void   goldfish_rtc_clear_alarm(void) {}
void   goldfish_rtc_irq_enable(int enable)    { (void)enable; }
uint64 goldfish_rtc_get_alarm_count(void) { return 0; }

/* ── PIT setup ── */

static void pit_init(void) {
    /* Channel 0, Access lo/hi, Mode 3 (square wave), binary */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0_DATA, (uint8)(PIT_DIVISOR & 0xFF));
    outb(PIT_CH0_DATA, (uint8)((PIT_DIVISOR >> 8) & 0xFF));
}

void arch_timer_init(void) {
    pit_init();
    printf("[x86] PIT timer initialized at %d Hz (divisor %ld)\n",
           PIT_HZ, (uint64)PIT_DIVISOR);
}

void arch_timer_init_hart(void) {
    /* No per-CPU timer setup needed for PIT (it's global) */
}