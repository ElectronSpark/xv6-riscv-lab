#ifndef KERNEL_PROC_QUEUE_H
#define KERNEL_PROC_QUEUE_H

#include "proc_queue_type.h"

void proc_queue_init(proc_queue_t *q, uint64 flags, const char *name);
void proc_queue_entry_init(proc_queue_entry_t *entry);

int proc_queue_size(proc_queue_t *q);
proc_queue_t *proc_queue_get_queue(struct proc *p);

int proc_queue_push(proc_queue_t *q, struct proc *p);
int proc_queue_pop(proc_queue_t *q, struct proc **ret_proc);
int proc_queue_remove(proc_queue_t *q, struct proc *p);


#endif // KERNEL_PROC_QUEUE_H
