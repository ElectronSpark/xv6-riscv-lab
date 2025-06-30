#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "proc_queue.h"
#include "list.h"

void proc_queue_init(proc_queue_t *q, uint64 flags, const char *name) {
    list_entry_init(&q->head);
    spin_init(&q->lock, "proc_queue_lock");
    q->counter = 0;
    if (name == NULL) {
        q->name = "NULL"; // Default name if none provided
    } else {
        q->name = name;
    }
    q->flags = flags | PROC_QUEUE_FLAG_VALID;
}

void proc_queue_entry_init(proc_queue_entry_t *entry) {
    list_entry_init(&entry->list_entry);
    entry->queue = NULL; // Initialize the queue pointer to NULL
}

int proc_queue_size(proc_queue_t *q) {
    if (q == NULL) {
        return -1; // Error: queue is NULL
    }
    if (!(q->flags & PROC_QUEUE_FLAG_VALID)) {
        return -1; // Error: queue is not valid
    }
    return q->counter;
}

proc_queue_t *proc_queue_get_queue(struct proc *p) {
    if (p == NULL) {
        return NULL; // Error: process is NULL
    }
    return p->queue_entry.queue;
}

int proc_queue_push(proc_queue_t *q, struct proc *p) {
    if (q == NULL || p == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (!(q->flags & PROC_QUEUE_FLAG_VALID)) {
        return -1; // Error: queue is not valid
    }

    if (proc_queue_get_queue(p) != NULL) {
        return -1; // Error: process is already in a queue
    }

    if (q->flags & PROC_QUEUE_FLAG_LOCK) {
        push_off();
        spin_acquire(&q->lock);
    }

    list_node_push_back(&q->head, p, queue_entry.list_entry);
    p->queue_entry.queue = q;
    q->counter++;

    if (q->flags & PROC_QUEUE_FLAG_LOCK) {
        spin_release(&q->lock);
        pop_off();
    }
    
    return 0; // Success
}

int proc_queue_pop(proc_queue_t *q, struct proc **ret_proc) {
    int ret_value = 0;

    if (q == NULL || ret_proc == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (!(q->flags & PROC_QUEUE_FLAG_VALID)) {
        return -1; // Error: queue is not valid
    }

    if (q->flags & PROC_QUEUE_FLAG_LOCK) {
        push_off();
        spin_acquire(&q->lock);
    }

    if (q->counter <= 0) {
        ret_value = -1; // Error: queue is empty
        goto ret;
    }

    struct proc *dequeued_proc = 
        list_node_pop(&q->head, struct proc, queue_entry.list_entry);
    if (dequeued_proc == NULL) {
        panic("proc_queue_pop: failed to pop from queue");
    }
    dequeued_proc->queue_entry.queue = NULL;
    q->counter--;
    *ret_proc = dequeued_proc; // Copy the process data

ret:
    if (q->flags & PROC_QUEUE_FLAG_LOCK) {
        spin_release(&q->lock);
        pop_off();
    }

    return ret_value;
}

int proc_queue_remove(proc_queue_t *q, struct proc *p) {
    int ret_value = 0;

    if (q == NULL || p == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (!(q->flags & PROC_QUEUE_FLAG_VALID)) {
        return -1; // Error: queue is not valid
    }

    if (proc_queue_get_queue(p) != q) {
        return -1; // Error: process is not in the specified queue
    }

    if (q->flags & PROC_QUEUE_FLAG_LOCK) {
        push_off();
        spin_acquire(&q->lock);
    }

    if (q->counter <= 0) {
        panic("proc_queue_remove: queue is empty");
    }

    list_node_detach(p, queue_entry.list_entry);
    p->queue_entry.queue = NULL;
    q->counter--;

    if (q->flags & PROC_QUEUE_FLAG_LOCK) {
        spin_release(&q->lock);
        pop_off();
    }

    return ret_value;
}
