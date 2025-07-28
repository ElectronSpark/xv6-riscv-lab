#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "proc_queue.h"
#include "list.h"

void proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock) {
    list_entry_init(&q->head);
    q->counter = 0;
    if (name == NULL) {
        q->name = "NULL"; // Default name if none provided
    } else {
        q->name = name;
    }
    q->lock = lock;
}

void proc_queue_set_lock(proc_queue_t *q, spinlock_t *lock) {
    if (q != NULL) {
        q->lock = lock;
    }
}

void proc_node_init(proc_node_t *node) {
    list_entry_init(&node->list_entry);
    node->queue = NULL; // Initialize the queue pointer to NULL
    node->proc = myproc();  // Initialize the process pointer to the current process
}

int proc_queue_size(proc_queue_t *q) {
    if (q == NULL) {
        return -1; // Error: queue is NULL
    }
    return q->counter;
}

proc_queue_t *proc_node_get_queue(proc_node_t *node) {
    if (node == NULL) {
        return NULL; // Error: node is NULL
    }
    return node->queue;
}

struct proc *proc_node_get_proc(proc_node_t *node) {
    if (node == NULL) {
        return NULL; // Error: node is NULL
    }
    return node->proc;
}

int proc_queue_push(proc_queue_t *q, proc_node_t *node) {
    if (q == NULL || proc_node_get_proc(node) == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (proc_node_get_queue(node) != NULL) {
        return -1; // Error: process is already in a queue
    }

    list_node_push(&q->head, node, list_entry);
    node->queue = q;
    q->counter++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // printf("pushing process %d to queue %s\n", p->pid, q->name);

    return 0; // Success
}

int proc_queue_first(proc_queue_t *q, proc_node_t **ret_node) {
    if (q == NULL || ret_node == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (q->counter <= 0) {
        *ret_node = NULL; // Set ret_node to NULL if queue is empty
        if (q->counter == 0) {
            return 0;
        } else {
            return -1;
        }
    }

    proc_node_t *first_node = LIST_FIRST_NODE(&q->head, proc_node_t, list_entry);
    assert(first_node != NULL, "proc_queue_first: queue is not empty but failed to get the first node");
    *ret_node = first_node; // Copy the process data
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // printf("popping process %d from queue %s\n", proc_node_get_proc(first_node)->pid, q->name);

    return 0; // Success
}

int proc_queue_remove(proc_queue_t *q, proc_node_t *node) {
    if (q == NULL || proc_node_get_proc(node) == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (proc_node_get_queue(node) != q) {
        return -1; // Error: process is not in the specified queue
    }

    if (q->counter <= 0) {
        panic("proc_queue_remove: queue is empty");
    }

    list_node_detach(node, list_entry);
    node->queue = NULL;
    q->counter--;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // printf("removing process %d from queue %s\n", proc_node_get_proc(node)->pid, q->name);

    return 0; // Success
}

int proc_queue_pop(proc_queue_t *q, proc_node_t **ret_node) {
    if (q == NULL || ret_node == NULL) {
        return -1; // Error: queue or ret_node is NULL
    }
    proc_node_t *dequeued_node = NULL;
    int ret = proc_queue_first(q, &dequeued_node);
    if (ret != 0) {
        return ret;
    }
    if (dequeued_node == NULL) {
        *ret_node = NULL; // Queue is empty
        return 0; // Success, but no node to return
    }

    ret = proc_queue_remove(q, dequeued_node);
    if (ret == 0) {
        *ret_node = dequeued_node; // Return the dequeued node
    } else {
        *ret_node = NULL; // Error occurred, set ret_node to NULL
    }
    return ret;
}

// Move all process from one queue to another.
// This is to enconvinience walking up all process in a queue.
// This will not change the pointer of the processes to their queues.
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
    proc_node_t *proc = NULL;
    proc_node_t *tmp = NULL;
    list_foreach_node_safe(&to->head, proc, tmp, list_entry) {
        proc->queue = to; // Update the queue pointer for each process
    }

    return 0; // Success
}
