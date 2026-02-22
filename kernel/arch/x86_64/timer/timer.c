/**
 * @file timer.c
 * @brief x86_64 PIT timer and timer subsystem.
 *
 * Provides a basic 100 Hz tick using the 8254 PIT (channel 0, mode 3).
 * Implements the red-black tree based software timer infrastructure
 * (timer_init, timer_add, timer_remove, timer_tick, timer_node_init)
 * shared with the scheduler timer subsystem (sched_timer.c).
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
#include "string.h"
#include "rbtree.h"
#include "list.h"
#include "lock/spinlock.h"
#include "proc/sched.h"
#include "defs.h"

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

uint64 get_jiffs(void) { return __atomic_load_n(&jiffies_count, __ATOMIC_RELAXED); }

/* ── debugcon output (port 0xE9) ── */
static inline void dbg_putc(char c) {
    asm volatile("outb %0, %1" : : "a"((uint8)c), "Nd"((uint16)0xE9));
}

void timer_tick_advance(void) {
    uint64 j = __atomic_fetch_add(&jiffies_count, 1, __ATOMIC_RELAXED);
    /* Print '.' every 1 second (100 ticks at 100 Hz) */
    if ((j % PIT_HZ) == 0)
        dbg_putc('.');
    /* Notify scheduler timer subsystem that a tick has occurred */
    sched_timer_tick();
}

/* ── Red-black-tree based software timer infrastructure ── */

static int __timer_root_keys_cmp_fun(uint64 key1, uint64 key2) {
    struct timer_node *node1 = (struct timer_node *)key1;
    struct timer_node *node2 = (struct timer_node *)key2;
    if (node1->expires < node2->expires)
        return -1;
    else if (node1->expires > node2->expires)
        return 1;
    else if (key1 < key2)
        return -1;
    else if (key1 > key2)
        return 1;
    else
        return 0;
}

static uint64 __timer_root_get_key_fun(struct rb_node *node) {
    assert(node != NULL, "node is NULL");
    struct timer_node *timer_node = container_of(node, struct timer_node, rb);
    return (uint64)timer_node;
}

static struct rb_root_opts __timer_root_opts = {
    .keys_cmp_fun = __timer_root_keys_cmp_fun,
    .get_key_fun = __timer_root_get_key_fun,
};

static void __timer_update_next_tick(struct timer_root *timer) {
    struct timer_node *next =
        LIST_FIRST_NODE(&timer->list_head, struct timer_node, list_entry);
    if (next != NULL)
        timer->next_tick = next->expires;
    else
        timer->next_tick = 0;
}

void timer_init(struct timer_root *timer) {
    if (timer == NULL)
        return;
    memset(timer, 0, sizeof(struct timer_root));
    rb_root_init(&timer->root, &__timer_root_opts);
    list_entry_init(&timer->list_head);
    timer->next_tick = 0;
    timer->current_tick = 0;
    timer->valid = 1;
    spin_init(&timer->lock, "timer_lock");
    /* No IRQ registration here — PIT IRQ is handled by the x86 trap path */
}

void timer_node_init(struct timer_node *node, uint64 expires,
                     void (*callback)(struct timer_node *), void *data,
                     int retry_limit) {
    if (node == NULL)
        return;
    memset(node, 0, sizeof(struct timer_node));
    rb_node_init(&node->rb);
    list_entry_init(&node->list_entry);
    node->expires = expires;
    node->callback = callback;
    node->data = data;
    node->retry_limit = retry_limit;
}

int timer_add(struct timer_root *timer, struct timer_node *node) {
    if (timer == NULL || node == NULL)
        return -1;
    if (node->callback == NULL)
        return -1;
    spin_lock(&timer->lock);
    if (!timer->valid) {
        spin_unlock(&timer->lock);
        return -1;
    }
    if (timer->current_tick >= node->expires) {
        spin_unlock(&timer->lock);
        return -1;
    }
    struct rb_node *inserted = rb_insert_color(&timer->root, &node->rb);
    if (inserted == NULL) {
        spin_unlock(&timer->lock);
        return -1;
    }
    if (inserted != &node->rb) {
        spin_unlock(&timer->lock);
        return -1;
    }
    struct rb_node *prev = rb_prev_node(&node->rb);
    if (prev == NULL) {
        list_node_push_back(&timer->list_head, node, list_entry);
        timer->next_tick = node->expires;
    } else {
        struct timer_node *prev_node =
            container_of(prev, struct timer_node, rb);
        list_node_insert(prev_node, node, list_entry);
    }
    node->timer = timer;
    spin_unlock(&timer->lock);
    return 0;
}

static void __timer_remove_unlocked(struct timer_root *timer,
                                    struct timer_node *node) {
    rb_delete_node_color(&timer->root, &node->rb);
    list_node_detach(node, list_entry);
    node->timer = NULL;
    __timer_update_next_tick(timer);
}

void timer_remove(struct timer_node *node) {
    struct timer_root *timer = node->timer;
    if (timer == NULL)
        return;
    spin_lock(&timer->lock);
    __timer_remove_unlocked(timer, node);
    spin_unlock(&timer->lock);
}

void timer_tick(struct timer_root *timer, uint64 ticks) {
    if (timer == NULL || ticks == 0)
        return;
    if (timer->valid == 0)
        return;
    spin_lock(&timer->lock);
    if (timer->next_tick == 0) {
        spin_unlock(&timer->lock);
        return;
    }
    if (timer->current_tick >= ticks) {
        spin_unlock(&timer->lock);
        return;
    }
    timer->current_tick = ticks;
    if (timer->next_tick > ticks) {
        spin_unlock(&timer->lock);
        return;
    }

    struct timer_node *node, *next;
    list_foreach_node_safe(&timer->list_head, node, next, list_entry) {
        if (node->expires > ticks)
            break;
        if (node->callback == NULL) {
            printf("Warning: Timer expired without callback\n");
            __timer_remove_unlocked(node->timer, node);
            continue;
        }
        node->retry++;
        if (node->retry >= node->retry_limit) {
            __timer_remove_unlocked(node->timer, node);
        }
        node->callback(node);
    }

    spin_unlock(&timer->lock);
}

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