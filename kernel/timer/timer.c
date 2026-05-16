#include "types.h"
#include "string.h"
#include "errno.h"
#include "param.h"
#include "printf.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "timer/timer.h"
#include "list.h"
#include "rbtree.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "trap.h"

uint64 __clint_timer_irqno = RISCV_S_TIMER_INTERRUPT;
uint64 __timebase_frequency = 10000000UL;
uint64 __jiff_ticks = 0; // Calculated in timerinit()
static uint64 ticks;

// The following functions are used to manage the red-black tree of thread nodes
// in insertion procedures.
static int __timer_root_keys_cmp_fun(uint64 key1, uint64 key2) {
    struct timer_node *node1 = (struct timer_node *)key1;
    struct timer_node *node2 = (struct timer_node *)key2;
    // First compare node->tree.key, it they are equal, use the address of
    // the nodes as distinguishing factors.
    if (node1->expires < node2->expires) {
        return -1;
    } else if (node1->expires > node2->expires) {
        return 1;
    } else if (key1 < key2) {
        return -1;
    } else if (key1 > key2) {
        return 1;
    } else {
        return 0; // Equal
    }
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

// Update the next tick number according to the node list of the timer
// This will not validate the timer, also will not try to acquire its locks
static void __timer_update_next_tick(struct timer_root *timer) {
    struct timer_node *next =
        LIST_FIRST_NODE(&timer->list_head, struct timer_node, list_entry);
    if (next != NULL) {
        timer->next_tick = next->expires;
    } else {
        timer->next_tick = 0;
    }
}

static bool __timer_node_before(struct timer_node *a, struct timer_node *b) {
    if (a->expires < b->expires)
        return true;
    if (a->expires > b->expires)
        return false;
    return (uint64)a < (uint64)b;
}

static void clockintr(int irq, void *data, device_t *dev) {
    // ask for the next timer interrupt. this also clears
    // the interrupt request. 1000000 is about a tenth
    // of a second.
    w_stimecmp(r_time() + JIFF_TICKS);
    if (IS_BOOT_HART()) {
        __atomic_fetch_add((uint64 *)data, 1, __ATOMIC_SEQ_CST);
        sched_timer_tick();
        hyperv_input_intr();
    }
    if (!sched_holding()) {
        SET_NEEDS_RESCHED();
    }
}

void timer_init(struct timer_root *timer) {
    if (timer == NULL) {
        return;
    }
    memset(timer, 0, sizeof(struct timer_root));
    rb_root_init(&timer->root, &__timer_root_opts);
    list_entry_init(&timer->list_head);
    timer->next_tick = 0;
    timer->current_tick = 0;
    timer->valid = 1;
    ticks = 0;
    spin_init(&timer->lock, "timer_lock");
    struct irq_desc timer_irq_desc = {
        .handler = clockintr,
        .data = &ticks,
        .dev = NULL,
    };
    int ret = register_irq_handler(CLINT_TIMER_IRQ, &timer_irq_desc);
    assert(ret == 0, "timer_init: Failed to register timer IRQ handler");
}

void timer_node_init(struct timer_node *node, uint64 expires,
                     void (*callback)(struct timer_node *), void *data,
                     int retry_limit) {
    if (node == NULL) {
        return;
    }
    memset(node, 0, sizeof(struct timer_node));
    rb_node_init(&node->rb);
    list_entry_init(&node->list_entry);
    node->expires = expires;
    node->callback = callback;
    node->data = data;
    node->retry_limit = retry_limit > 0 ? retry_limit : TIMER_DEFAULT_RETRY_LIMIT;
}

// Add a timer_node to a timer_root;
// After adding the timer_node, timer_remove needs to be called to
// remove the node from its root (e.g, in callback or the thread context after
// waking up). Otherwise, the timer will keep calling the callback function
// every time the timer receives a tick.
int timer_add(struct timer_root *timer, struct timer_node *node) {
    // Add timer to the timer_root
    if (timer == NULL || node == NULL) {
        return -EINVAL;
    }
    if (node->callback == NULL) {
        return -EINVAL;
    }
    if (node->timer != NULL || !rb_node_is_empty(&node->rb) ||
        !LIST_ENTRY_IS_DETACHED(&node->list_entry)) {
        return -EINVAL;
    }
    spin_lock(&timer->lock);
    if (!timer->valid) {
        spin_unlock(&timer->lock);
        return -EINVAL;
    }
    if (timer->current_tick >= node->expires) {
        // Timer already expired
        spin_unlock(&timer->lock);
        return -EINVAL;
    }
    bool inserted = false;
    struct timer_node *pos, *tmp;
    list_foreach_node_safe(&timer->list_head, pos, tmp, list_entry) {
        if (__timer_node_before(node, pos)) {
            list_entry_insert(LIST_PREV_ENTRY(&pos->list_entry),
                              &node->list_entry);
            inserted = true;
            break;
        }
    }
    if (!inserted)
        list_node_push(&timer->list_head, node, list_entry);
    node->timer = timer;
    __timer_update_next_tick(timer);
    spin_unlock(&timer->lock);
    return 0;
}

static void __timer_remove_unlocked(struct timer_root *timer,
                                    struct timer_node *node) {
    if (!LIST_ENTRY_IS_DETACHED(&node->list_entry))
        list_node_detach(node, list_entry);
    rb_node_init(&node->rb);
    node->timer = NULL;
    __timer_update_next_tick(timer);
}

void timer_remove(struct timer_node *node) {
    // Remove timer from the timer_root
    struct timer_root *timer = node->timer;
    if (timer == NULL) {
        return;
    }
    spin_lock(&timer->lock);
    __timer_remove_unlocked(timer, node);
    spin_unlock(&timer->lock);
}

// Handle timer tick
// Each time a timer tick occurs, we need to check if any timers have expired.
// It will try to execute the callback functions of expired timers.
// Either the callback function or the thread to be woken up should
// remove the timer from its timer_root.
// This function may call the callback functions of the expired timers
// for at most TIMER_DEFAULT_RETRY_LIMIT times if the timer nodes are still
// in the timer list the next time the timer_tick function is called.
// The callback function will be called with the timer lock locked
void timer_tick(struct timer_root *timer, uint64 ticks) {
    if (timer == NULL || ticks == 0) {
        return;
    }
    if (timer->valid == 0) {
        return;
    }
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
        // No timer expired
        spin_unlock(&timer->lock);
        return;
    }

    struct timer_node *node, *next;
    list_foreach_node_safe(&timer->list_head, node, next, list_entry) {
        if (node->expires > ticks) {
            break;
        }
        if (node->callback == NULL) {
            // If no callback is set, just remove the timer
            printf("Warning: Timer expired without callback\n");
            __timer_remove_unlocked(node->timer, node);
            continue;
        }
        node->retry++;
        if (node->retry >= node->retry_limit) {
            // Because many callback functions is to wake up a thread,
            // and the thread will remove the timer node from the timer_root,
            // we will try to call the callback function multiple times until
            // retry limit is reached or the timer node is removed by the
            // thread.
            __timer_remove_unlocked(node->timer, node);
        }
        node->callback(node);
    }

    spin_unlock(&timer->lock);
}

uint64 get_jiffs(void) {
    extern uint64 ticks;
    return __atomic_load_n(&ticks, __ATOMIC_SEQ_CST);
}
