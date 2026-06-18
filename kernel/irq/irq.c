#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "proc/thread.h"
#include "defs.h"
#include "printf.h"
#include "proc/sched.h"
#include <mm/page.h>
#include "trap.h"
#include <mm/slab.h>
#include "errno.h"
#include "trap.h"
#include "dev/plic.h"

static struct irq_desc *irq_descs[IRQCNT] = {0};
static slab_cache_t __irq_desc_slab = {0};
static spinlock_t irq_write_lock = SPINLOCK_INITIALIZED(
    "irq_write_lock"); // Protects write operations to irq_descs

static struct irq_desc *__alloc_irq_desc(struct irq_desc *in_desc) {
    struct irq_desc *desc = slab_alloc(&__irq_desc_slab);
    if (desc == NULL) {
        return NULL;
    }
    if (in_desc != NULL) {
        *desc = *in_desc;
    } else {
        memset(desc, 0, sizeof(struct irq_desc));
    }
    // Always zero the RCU head to ensure clean state
    memset(&desc->rcu_head, 0, sizeof(desc->rcu_head));
    return desc;
}

static void __free_irq_desc(struct irq_desc *desc) {
    if (desc == NULL) {
        return;
    }
    slab_free(desc);
}

static void __rcu_free_irq_desc_chain(void *data)
{
    struct irq_desc *desc = (struct irq_desc *)data;
    while (desc != NULL) {
        struct irq_desc *next = desc->next;
        __free_irq_desc(desc);
        desc = next;
    }
}

void irq_desc_init(void) {
    int ret = slab_cache_init(&__irq_desc_slab, "irq_desc",
                              sizeof(struct irq_desc), SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "irq_desc_init: Failed to initialize irq_desc slab cache");
}

int register_irq_handler(int irq_num, struct irq_desc *desc) {
    if (irq_num < 0 || irq_num >= IRQCNT) {
        return -EINVAL;
    }
    if (desc == NULL) {
        return -EINVAL;
    }

    // Allocate and initialize new descriptor
    struct irq_desc *new_desc = __alloc_irq_desc(desc);
    if (new_desc == NULL) {
        return -ENOMEM;
    }
    new_desc->irq = irq_num;
    new_desc->count = 0;
    new_desc->next = NULL;

    // Acquire write lock to serialize registration
    spin_lock(&irq_write_lock);

    struct irq_desc *old_desc = rcu_dereference(irq_descs[irq_num]);
    if (old_desc == NULL) {
        // Use RCU to safely publish the new descriptor
        rcu_assign_pointer(irq_descs[irq_num], new_desc);
    } else {
        struct irq_desc *tail = old_desc;
        while (tail->next != NULL)
            tail = tail->next;
        /*
         * Legacy PCI INTx lines are commonly shared.  The descriptor list is
         * append-only while readers run under RCU; the write lock serializes
         * registration so publishing the next pointer is enough here.
         */
        rcu_assign_pointer(tail->next, new_desc);
    }

    spin_unlock(&irq_write_lock);

    // Enable PLIC interrupt after handler is registered
    // plic_enable_irq sets priority=1 and enables the IRQ on all harts
    if (irq_num >= PLIC_IRQ_OFFSET &&
        irq_num < PLIC_IRQ_OFFSET + PLIC_IRQ_CNT) {
        plic_enable_irq(irq_num - PLIC_IRQ_OFFSET);
    }

    return 0;
}

int unregister_irq_handler(int irq_num) {
    if (irq_num < 0 || irq_num >= IRQCNT) {
        return -EINVAL;
    }

    // Acquire write lock to serialize unregistration
    spin_lock(&irq_write_lock);

    // Get the old descriptor
    struct irq_desc *old_desc = irq_descs[irq_num];
    if (old_desc == NULL) {
        spin_unlock(&irq_write_lock);
        return -ENOENT; // No handler registered
    }

    // Clear the descriptor pointer using RCU
    rcu_assign_pointer(irq_descs[irq_num], NULL);

    spin_unlock(&irq_write_lock);

    // Use call_rcu() for non-blocking deferred freeing.
    call_rcu(&old_desc->rcu_head, __rcu_free_irq_desc_chain, old_desc);

    return 0;
}

static void irq_dispatch_chain(int irq, struct irq_desc *desc)
{
    for (struct irq_desc *cur = desc; cur != NULL;
         cur = rcu_dereference(cur->next)) {
        __atomic_add_fetch(&cur->count, 1, __ATOMIC_SEQ_CST);
        if (cur->handler != NULL)
            cur->handler(irq, cur->data, cur->dev);
    }
}

static int __do_plic_irq(void) {
    int irq = plic_claim();
    if (!irq) {
        // Assume hart may receive spurious interrupts
        return 0;
    }
    if (irq >= PLIC_IRQ_CNT) {
        printf("do_irq: invalid PLIC irq %d\n", irq);
        return -ENODEV;
    }

    irq += PLIC_IRQ_OFFSET;

    // Enter RCU read-side critical section
    rcu_read_lock();

    // Safely dereference the IRQ descriptor
    struct irq_desc *desc = rcu_dereference(irq_descs[irq]);
    if (desc == NULL) {
        rcu_read_unlock();
        printf("do_irq: no handler for irq_num %d\n", irq);
        plic_complete(irq - PLIC_IRQ_OFFSET);
        return -ENODEV;
    }

    irq_dispatch_chain(irq, desc);

    // Exit RCU read-side critical section
    rcu_read_unlock();
    plic_complete(irq - PLIC_IRQ_OFFSET);
    return irq;
}

int do_irq(int irq_num) {
    if (irq_num < 0 || irq_num >= CLINT_IRQ_CNT) {
        printf("do_irq: invalid irq_num %d\n", irq_num);
        return -ENODEV;
    }

    if (irq_num == RISCV_S_EXTERNAL_INTERRUPT) {
        // PLIC IRQ
        // Treat separately
        return __do_plic_irq();
    }

    // Enter RCU read-side critical section
    rcu_read_lock();

    // Safely dereference the IRQ descriptor
    struct irq_desc *desc = rcu_dereference(irq_descs[irq_num]);
    if (desc == NULL) {
        rcu_read_unlock();
        printf("do_irq: no handler for irq_num %d\n", irq_num);
        return -ENODEV;
    }

    irq_dispatch_chain(irq_num, desc);

    // Exit RCU read-side critical section
    rcu_read_unlock();
    return irq_num;
}

/**
 * do_device_irq - Dispatch a hardware device IRQ by number.
 *
 * Called from architecture-specific trap handlers (x86 IOAPIC path)
 * when a device interrupt arrives.  The hw_irq is the ISA/device IRQ
 * number (e.g. 4 for COM1).  This looks up the handler registered via
 * register_irq_handler(PLIC_IRQ(hw_irq), ...) and invokes it.
 *
 * @param hw_irq  Hardware IRQ number (0-based, before PLIC_IRQ_OFFSET).
 * @return        The internal irq index on success, 0 if no handler.
 */
int do_device_irq(int hw_irq) {
    if (hw_irq < 0 || hw_irq >= PLIC_IRQ_CNT) {
        printf("do_device_irq: invalid hw_irq %d\n", hw_irq);
        return 0;
    }

    int irq = hw_irq + PLIC_IRQ_OFFSET;

    rcu_read_lock();

    struct irq_desc *desc = rcu_dereference(irq_descs[irq]);
    if (desc == NULL) {
        rcu_read_unlock();
        return 0;
    }

    irq_dispatch_chain(irq, desc);

    rcu_read_unlock();
    return irq;
}
