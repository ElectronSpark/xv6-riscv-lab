#include "types.h"
#include "string.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc/proc.h"
#include "defs.h"
#include "printf.h"
#include "proc/sched.h"
#include "page.h"
#include "trap.h"
#include "slab.h"
#include "rcu.h"
#include "errno.h"
#include "trap.h"

static struct irq_desc *irq_descs[IRQCNT] = { 0 };
static slab_cache_t __irq_desc_slab = { 0 };
static spinlock_t irq_write_lock; // Protects write operations to irq_descs

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

void irq_desc_init(void) {
    spin_init(&irq_write_lock, "irq_write");
    int ret = slab_cache_init(&__irq_desc_slab, "irq_desc", 
                              sizeof(struct irq_desc), 
                              SLAB_FLAG_EMBEDDED);
    assert(ret == 0, "irq_desc_init: Failed to initialize irq_desc slab cache");
}

// Callback for freeing IRQ descriptor after grace period
static void __rcu_free_irq_desc(void *data) {
    struct irq_desc *desc = (struct irq_desc *)data;
    __free_irq_desc(desc);
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

    // Acquire write lock to serialize registration
    spin_acquire(&irq_write_lock);

    // Check if handler already exists - fail to prevent double registration
    struct irq_desc *old_desc = rcu_dereference(irq_descs[irq_num]);
    if (old_desc != NULL) {
        spin_release(&irq_write_lock);
        __free_irq_desc(new_desc);
        return -EEXIST; // Handler already registered
    }

    // Use RCU to safely publish the new descriptor
    rcu_assign_pointer(irq_descs[irq_num], new_desc);

    spin_release(&irq_write_lock);

    return 0;
}

int unregister_irq_handler(int irq_num) {
    if (irq_num < 0 || irq_num >= IRQCNT) {
        return -EINVAL;
    }

    // Acquire write lock to serialize unregistration
    spin_acquire(&irq_write_lock);

    // Get the old descriptor
    struct irq_desc *old_desc = irq_descs[irq_num];
    if (old_desc == NULL) {
        spin_release(&irq_write_lock);
        return -ENOENT; // No handler registered
    }

    // Clear the descriptor pointer using RCU
    rcu_assign_pointer(irq_descs[irq_num], NULL);

    spin_release(&irq_write_lock);

    // Use call_rcu() for non-blocking deferred freeing
    // The descriptor will be freed after all readers complete
    call_rcu(&old_desc->rcu_head, __rcu_free_irq_desc, old_desc);

    return 0;
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

    // When an IRQ exists, increase its counter no matter whether handler is NULL
    __atomic_add_fetch(&desc->count, 1, __ATOMIC_SEQ_CST);
    
    // Call the handler if it exists
    if (desc->handler != NULL) {
        desc->handler(irq, desc->data, desc->dev);
    }
    
    // Exit RCU read-side critical section
    rcu_read_unlock();
    plic_complete(irq - PLIC_IRQ_OFFSET);
    return irq;
}

int do_irq(struct trapframe *tf) {
    assert(tf->scause >> 63, "do_irq: not an interrupt");
    int irq_num = tf->scause & ((1UL << 63) - 1);
    if (irq_num >= CLINT_IRQ_CNT) {
        printf("do_irq: invalid irq_num %d\n", irq_num);
        return -ENODEV;
    }

    if (irq_num == 9) {
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

    // When an IRQ exists, increase its counter no matter whether handler is NULL
    __atomic_add_fetch(&desc->count, 1, __ATOMIC_SEQ_CST);
    
    // Call the handler if it exists
    if (desc->handler != NULL) {
        desc->handler(irq_num, desc->data, desc->dev);
    }
    
    // Exit RCU read-side critical section
    rcu_read_unlock();
    return irq_num;
}
