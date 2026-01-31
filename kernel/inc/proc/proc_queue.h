#ifndef KERNEL_PROC_QUEUE_H
#define KERNEL_PROC_QUEUE_H

#include "proc/proc_queue_type.h"
#include "proc/proc.h"
#include "list.h"

// Traverse the process queue without locking
#define proc_list_foreach_unlocked(q, pos, tmp)   \
    list_foreach_node_safe(&(q)->head, pos, tmp, list.entry)

void proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock);
void proc_queue_set_lock(proc_queue_t *q, spinlock_t *lock);
void proc_tree_init(proc_tree_t *q, const char *name, spinlock_t *lock);
void proc_tree_set_lock(proc_tree_t *q, spinlock_t *lock);
void proc_node_init(proc_node_t *node);

int proc_queue_size(proc_queue_t *q);
int proc_tree_size(proc_tree_t *q);
proc_queue_t *proc_node_get_queue(proc_node_t *node);
proc_tree_t *proc_node_get_tree(proc_node_t *node);
struct proc *proc_node_get_proc(proc_node_t *node);
int proc_node_get_errno(proc_node_t *node, int *error_no);

int proc_queue_push(proc_queue_t *q, proc_node_t *node);
int proc_queue_first(proc_queue_t *q, proc_node_t **ret_node);
int proc_queue_pop(proc_queue_t *q, proc_node_t **ret_node);
int proc_queue_remove(proc_queue_t *q, proc_node_t *node);
int proc_queue_bulk_move(proc_queue_t *to, proc_queue_t *from);

int proc_tree_add(proc_tree_t *q, proc_node_t *node);
int proc_tree_first(proc_tree_t *q, proc_node_t **ret_node);
int proc_tree_key_min(proc_tree_t *q, uint64 *key);
int proc_tree_remove(proc_tree_t *q, proc_node_t *node);

int proc_queue_wait_in_state(proc_queue_t *q, struct spinlock *lock, 
                             uint64 *rdata, enum procstate state);
int proc_queue_wait(proc_queue_t *q, struct spinlock *lock, uint64 *rdata);
int proc_queue_wakeup(proc_queue_t *q, int error_no, uint64 rdata, struct proc **retp);
int proc_queue_wakeup_all(proc_queue_t *q, int error_no, uint64 rdata);

int proc_tree_wait_in_state(proc_tree_t *q, uint64 key, struct spinlock *lock, 
                            uint64 *rdata, enum procstate state);
int proc_tree_wait(proc_tree_t *q, uint64 key, struct spinlock *lock, uint64 *rdata);
int proc_tree_wakeup_one(proc_tree_t *q, uint64 key, int error_no, uint64 rdata, struct proc **retp);
int proc_tree_wakeup_key(proc_tree_t *q, uint64 key, int error_no, uint64 rdata);
int proc_tree_wakeup_all(proc_tree_t *q, int error_no, uint64 rdata);

#endif // KERNEL_PROC_QUEUE_H
