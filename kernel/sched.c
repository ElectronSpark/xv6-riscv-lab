#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "list.h"
#include "hlist.h"
#include "proc_queue.h"
#include "sched.h"
#include "slab.h"
#include "rbtree.h"


struct chan_queue_node {
    struct rb_node rb_entry; // Red-black tree node
    uint64 chan; // Key for the red-black tree
    proc_queue_t wait_queue; // Process queue of channel
};

slab_cache_t chan_queue_slab;
static struct rb_root __chan_queue_root;

static proc_queue_t ready_queue;
static proc_queue_t sleep_queue;
static spinlock_t __sched_lock;   // ready_queue and sleep_queue share this lock


/* chan queue related functions */
static int __chan_keys_cmp_fun(uint64 chan1, uint64 chan2) {
    return chan1 - chan2;
}

static uint64 __chan_get_key_fun(struct rb_node *node) {
    struct chan_queue_node *chan_node = container_of(node, struct chan_queue_node, rb_entry);
    return chan_node->chan;
}

static struct rb_root_opts __chan_queue_opts = {
    .keys_cmp_fun = __chan_keys_cmp_fun,
    .get_key_fun = __chan_get_key_fun,
};

static void chan_queue_init(void) {
    rb_root_init(&__chan_queue_root, &__chan_queue_opts);
    int slab_ret = slab_cache_init(&chan_queue_slab, 
                                   "chan_queue_slab", 
                                   sizeof(struct chan_queue_node), 
                                   0);
    assert(slab_ret == 0, "Failed to initialize chan queue slab cache");
}

static struct chan_queue_node *chan_queue_alloc(uint64 chan) {
    struct chan_queue_node *node = slab_alloc(&chan_queue_slab);
    if (node) {
        node->chan = chan;
        rb_node_init(&node->rb_entry);
        proc_queue_init(&node->wait_queue, "chan_wait_queue");
    }
    if (node != NULL) {
        printf("+");
    }
    return node;
}

static void chan_queue_free(struct chan_queue_node *node) {
    printf("-");
    slab_free(node);
}

proc_queue_t *chan_queue_get(uint64 chan, bool create) {
    struct rb_node *node = rb_find_key(&__chan_queue_root, chan);
    struct chan_queue_node *chan_node = NULL;
    if (node == NULL) {
        if (!create) {
            return NULL; // Not found
        }
        chan_node = chan_queue_alloc(chan);
        assert(chan_node != NULL, "Failed to allocate channel queue node");
        struct rb_node *ret_node = rb_insert_color(&__chan_queue_root, &chan_node->rb_entry);
        assert(ret_node == &chan_node->rb_entry, "Failed to insert channel queue node");
        if (ret_node != &chan_node->rb_entry) {
            // If the node was already in the tree, free the allocated node
            chan_queue_free(chan_node);
            return NULL; // Node already exists
        }
    } else {
        chan_node = container_of(node, struct chan_queue_node, rb_entry);
    }
    return &chan_node->wait_queue;
}

int chan_queue_pop(uint64 chan, proc_queue_t *ret_queue) {
    struct rb_node *node = rb_delete_color(&__chan_queue_root, chan);
    if (node == NULL) {
        return -1; // Not found
    }

    struct chan_queue_node *chan_node = container_of(node, struct chan_queue_node, rb_entry);
    proc_queue_bulk_move(ret_queue, &chan_node->wait_queue);

    // Free the chan queue node
    chan_queue_free(chan_node);
    return 0; // Success
}

/* Scheduler lock functions */
// These functions are used to acquire and release the scheduler lock.
// To avoid deadlocks, locks must be held in the following order:
// - locks of each process -- proc.c
// - locks related to the process queue -- proc_queue.c
// - sched_lock -- sched.c
// - proc table lock -- proc.c
//
// The locks of task queues and the scheduler lock should not be held simultaneously,
int sched_holding(void) {
    // Check if the scheduler lock is currently held
    return spin_holding(&__sched_lock);
}

/* Scheduler lock assertions */
static inline void __sched_assert_holding(void) {
    assert(spin_holding(&__sched_lock), "sched_lock must be held");
}

static inline void __sched_assert_unholding(void) {
    assert(!spin_holding(&__sched_lock), "sched_lock must not be held");
}

void sched_lock(void) {
    spin_acquire(&__sched_lock);
}

void sched_unlock(void) {
    spin_release(&__sched_lock);
}

/* Scheduler functions */
void scheduler_init(void) {
    spin_init(&__sched_lock, "sched_lock");
    proc_queue_init(&ready_queue, "ready_queue");
    proc_queue_init(&sleep_queue, "sleep_queue");
    chan_queue_init();
}

// Pick the next process to run from the ready queue.
// Returns a RUNABLE state process or NULL if no process is ready.
// The process returned will be locked
static struct proc *__sched_pick_next(void) {
    struct proc *p = NULL;
    sched_lock();
    assert(proc_queue_pop(&ready_queue, &p) == 0, "Failed to pop process from ready queue");
    sched_unlock();
    
    if (p) {
        spin_acquire(&p->lock);
        assert(p->state != RUNNING, "found and running process in ready queue");
        assert(p->state != SLEEPING, "try to schedule a sleeping process");
        assert(p->state != UNUSED, "try to schedule an uninitialized process");
        assert(p->state != ZOMBIE, "found and zombie process in ready queue");
        assert(p->state == RUNNABLE, "try to schedule unknown process state");
        return p;
    }

    return NULL;
}

static void __idle(void) {
    // This function is called when no process is ready to run.
    // It can be used to put the CPU into a low-power state or
    // perform other idle tasks.
    // For now, we just yield the CPU.
    __sched_assert_unholding();
    asm volatile("wfi");  // Wait for interrupt
}

// Switch to the given process.
// The process will change its state to RUNNING afer the switching.
// Before calling this function, the caller must hold both the process's lock
//    and the scheduler lock.
// Returns the previous process that was running on this CPU.
static struct spinlock *__switch_to(struct proc *p) {
    __sched_assert_holding();
    assert(!intr_get(), "Interrupts must be disabled before switching to a process");
    assert(p != NULL, "Cannot switch to a NULL process");
    assert(p->state == RUNNABLE, "Cannot switch to a non-RUNNABLE process");
    assert(spin_holding(&p->lock), "Process lock must be held before switch");

    // Switch to the process's context
    mycpu()->proc = p; // Set the current process for this CPU
    p->state = RUNNING; // Set the process state to RUNNING

    struct spinlock * lk = (struct spinlock *)__swtch_context(&mycpu()->context, &p->context, 0);
    assert(spin_holding(&p->lock), "Process lock must be held after switch");
    assert(!intr_get(), "Interrupts must be disabled before switching to a process");

    mycpu()->proc = NULL; // Clear the current process for this CPU

    return lk;
}

void scheduler_run(void) {
    intr_off(); // Disable interrupts to keep the atomicity of the scheduling operation
    while (1) {
        struct proc *p = __sched_pick_next();
        if (!p) {
            intr_on(); // allow interrupts to be handled
            __idle();
            intr_off();
            continue;
        }
        // printf("CPU: %d -> %s (pid: %d)\n", cpuid(), p->name, p->pid);
        sched_lock();
        struct spinlock *lk = __switch_to(p);
        assert(!intr_get(), "Interrupts must be disabled after switching to a process");
        if (p->state == RUNNING) {
            p->state = RUNNABLE; // If we returned to a RUNNING process, set it to RUNNABLE
            if (proc_queue_push(&ready_queue, p) != 0) {
                panic("Failed to push process back to ready queue");
            }
        }
        sched_unlock();
        // printf("CPU: %d <- %s: %s (pid: %d)\n", cpuid(), p->name, procstate_to_str(p->state), p->pid);
        spin_release(&p->lock);
        if (lk != NULL) {
            spin_release(lk); // Release the lock returned by __swtch_context
        }
    }
}

// Yield the CPU to allow other processes to run.
// lk will not be re-acquired after yielding.
int scheduler_yield(uint64 *ret_arg, struct spinlock *lk) {
    push_off();
    struct proc *proc = myproc();
    assert(spin_holding(&proc->lock), "Process lock must be held before yielding");
    __sched_assert_holding();
    int lk_holding = (lk != NULL && spin_holding(lk));
    pop_off();

    assert(!intr_get(), "Interrupts must be disabled after switching to a process");
    int intena = mycpu()->intena; // Save interrupt state
    assert(mycpu()->noff == 2 || (mycpu()->noff == 3 && lk_holding), 
           "Process must hold and only hold the proc lock and sched lock when yielding. Current noff: %d", mycpu()->noff);
    uint64 ret_val = __swtch_context(&proc->context, &mycpu()->context, (uint64)lk);

    assert(!intr_get(), "Interrupts must be disabled after switching to a process");
    assert(myproc() == proc, "Yield returned to a different process");
    assert(spin_holding(&proc->lock), "Process lock must be held after yield");
    assert(proc->state == RUNNING, "Process state must be RUNNING after yield");

    mycpu()->intena = intena; // Restore interrupt state
    
    if (ret_arg) {
        *ret_arg = ret_val; // Return the value from the context switch
    }

    return 0;
}

// Put the current process to sleep on the specified queue.
void scheduler_sleep(proc_queue_t *queue, struct spinlock *lk) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    struct proc *proc = myproc();
    assert(proc != NULL, "PCB is NULL");
    assert(queue != NULL, "Cannot sleep on a NULL queue");
    assert(spin_holding(&proc->lock), "Process lock must be held before sleeping");
    int lk_holding = (lk != NULL && spin_holding(lk));
    pop_off();  // Safe to decrease noff counter after acquiring the process lock

    sched_lock();
    proc->state = SLEEPING;
    if (proc_queue_push(queue, proc) != 0) {
        panic("Failed to push process to sleep queue");
    }
    scheduler_yield(NULL, lk); // Switch to the scheduler
    sched_unlock();

    // Because the process lock is acquired after lk, we need to release it before acquiring lk.
    // The process lock will be held after acquiring lk, if the process lock was held 
    // before the yield.
    spin_release(&proc->lock);
    if (lk_holding) {
        spin_acquire(lk);
    }
    spin_acquire(&proc->lock); // Reacquire the process lock after yielding
}

// Wake up a process from the sleep queue.
void scheduler_wakeup(struct proc *p) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    __sched_assert_holding();
    pop_off(); // Safe to decrease noff counter after acquiring the process lock
    // __sched_assert_unholding();
    assert(p != NULL, "Cannot wake up a NULL process");
    assert(p->state == SLEEPING, "Process must be SLEEPING to wake up");
    assert(!proc_in_queue(p, NULL), "Process must not be in any queue before waking up");
    assert(p != myproc(), "Cannot wake up the current process");

    p->state = RUNNABLE; // Change state to RUNNABLE
    p->chan = NULL; // Clear the channel

    if (proc_queue_push(&ready_queue, p) != 0) {
        panic("Failed to push process to ready queue");
    }
}

void scheduler_sleep_on_chan(void *chan, struct spinlock *lk) {
    push_off(); // Increase noff counter to ensure interruptions are disabled
    struct proc *proc = myproc();
    assert(proc != NULL, "PCB is NULL");
    assert(chan != NULL, "Cannot sleep on a NULL channel");
    assert(spin_holding(&proc->lock), "Process lock must be held before sleeping");
    int lk_holding = (lk != NULL && spin_holding(lk));
    pop_off();  // Safe to decrease noff counter after acquiring the process lock

    sched_lock();
    struct proc_queue *queue = chan_queue_get((uint64)chan, true);
    assert(queue != NULL, "Failed to get channel queue");

    proc->state = SLEEPING;
    if (proc_queue_push(queue, proc) != 0) {
        panic("Failed to push process to sleep queue");
    }
    scheduler_yield(NULL, lk); // Switch to the scheduler
    sched_unlock();

    // Because the process lock is acquired after lk, we need to release it before acquiring lk.
    // The process lock will be held after acquiring lk, if the process lock was held 
    // before the yield.
    spin_release(&proc->lock);
    if (lk_holding) {
        spin_acquire(lk);
    }
    spin_acquire(&proc->lock); // Reacquire the process lock after yielding
}

void scheduler_wakeup_on_chan(void *chan) {
    proc_queue_t tmp_queue = { 0 };
    proc_queue_init(&tmp_queue, "tmp_queue");
    sched_lock();
    if (chan_queue_pop((uint64)chan, &tmp_queue) != 0) {
        sched_unlock();
        return; // No processes waiting on this channel
    }
    sched_unlock();

    struct proc *p = NULL;
    struct proc *tmp = NULL;
    proc_queue_foreach_unlocked(&tmp_queue, p, tmp) {
        spin_acquire(&p->lock);
        assert(proc_queue_remove(&tmp_queue, p) == 0, "Failed to remove process from temporary queue");
        sched_lock();
        scheduler_wakeup(p);
        sched_unlock();
        spin_release(&p->lock);
    }
}
