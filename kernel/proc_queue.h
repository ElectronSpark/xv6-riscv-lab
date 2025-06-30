#ifndef KERNEL_PROC_QUEUE_H
#define KERNEL_PROC_QUEUE_H

#include "proc_queue_type.h"

void proc_queue_init(proc_queue_t *q, uint64 flags, const char *name);
void proc_queue_entry_init(proc_queue_entry_t *entry, proc_queue_t *q);

void proc_queue_push(proc_queue_t *q, struct proc *p);
struct proc *proc_queue_pop(proc_queue_t *q);

bool proc_queue_is_empty(proc_queue_t *q);
int proc_queue_size(proc_queue_t *q);


#endif // KERNEL_PROC_QUEUE_H
