#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "proc_queue.h"
#include "list.h"

void proc_queue_init(proc_queue_t *q, const char *name) {
    list_entry_init(&q->head);
    q->counter = 0;
    if (name == NULL) {
        q->name = "NULL"; // Default name if none provided
    } else {
        q->name = name;
    }
}

void proc_queue_entry_init(proc_queue_entry_t *entry) {
    list_entry_init(&entry->list_entry);
    entry->queue = NULL; // Initialize the queue pointer to NULL
}

int proc_queue_size(proc_queue_t *q) {
    if (q == NULL) {
        return -1; // Error: queue is NULL
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

    if (proc_queue_get_queue(p) != NULL) {
        return -1; // Error: process is already in a queue
    }

    list_node_push(&q->head, p, queue_entry.list_entry);
    p->queue_entry.queue = q;
    q->counter++;

    return 0; // Success
}

int proc_queue_pop(proc_queue_t *q, struct proc **ret_proc) {
    if (q == NULL || ret_proc == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (q->counter <= 0) {
        *ret_proc = NULL; // Set ret_proc to NULL if queue is empty
        return 0;
    }

    struct proc *dequeued_proc = 
        list_node_pop_back(&q->head, struct proc, queue_entry.list_entry);
    if (dequeued_proc == NULL) {
        panic("proc_queue_pop: failed to pop from queue");
    }
    dequeued_proc->queue_entry.queue = NULL;
    q->counter--;
    *ret_proc = dequeued_proc; // Copy the process data

    return 0; // Success
}

int proc_queue_remove(proc_queue_t *q, struct proc *p) {
    if (q == NULL || p == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (proc_queue_get_queue(p) != q) {
        return -1; // Error: process is not in the specified queue
    }

    if (q->counter <= 0) {
        panic("proc_queue_remove: queue is empty");
    }

    list_node_detach(p, queue_entry.list_entry);
    p->queue_entry.queue = NULL;
    q->counter--;

    return 0; // Success
}

// Move all process from one queue to another.
int proc_queue_bulk_move(proc_queue_t *to, proc_queue_t *from) {
    if (to == NULL || from == NULL) {
        return -1; // Error: one of the queues is NULL
    }

    if (from->counter <= 0) {
        return -1; // Error: source queue is empty
    }

    to->counter += from->counter;
    from->counter = 0;
    list_entry_insert_bulk(LIST_LAST_ENTRY(&to->head), &from->head);

    return 0; // Success
}
