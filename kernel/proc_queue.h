#ifndef KERNEL_PROC_QUEUE_H
#define KERNEL_PROC_QUEUE_H

#include "proc_queue_type.h"
#include "list.h"

// Traverse the process queue without locking
#define proc_queue_foreach_unlocked(q, pos, tmp)   \
    list_foreach_node_safe(&(q)->head, pos, tmp, queue_entry.list_entry)

#define proc_in_queue(__proc, __queue) \
    (((__proc)->queue_entry.queue == (__queue) &&   \
      (__queue) != NULL) ||                         \
     ((__proc)->queue_entry.queue != NULL &&        \
      (__queue) == NULL))

#define proc_queue_of(proc) \
    ((__proc)->queue_entry.queue)

void proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock);
void proc_queue_set_lock(proc_queue_t *q, spinlock_t *lock);
void proc_queue_entry_init(proc_queue_entry_t *entry);

int proc_queue_size(proc_queue_t *q);
proc_queue_t *proc_queue_get_queue(struct proc *p);

int proc_queue_push(proc_queue_t *q, struct proc *p);
int proc_queue_pop(proc_queue_t *q, struct proc **ret_proc);
int proc_queue_remove(proc_queue_t *q, struct proc *p);
int proc_queue_bulk_move(proc_queue_t *to, proc_queue_t *from);


#endif // KERNEL_PROC_QUEUE_H
