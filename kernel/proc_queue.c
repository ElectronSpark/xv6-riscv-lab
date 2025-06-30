#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "proc_queue.h"
#include "list.h"

// void proc_queue_init(proc_queue_t *q, uint64 flags, const char *name) {
//     list_node_init(&q->head);
//     spinlock_init(&q->lock, "proc_queue_lock");
//     q->counter = 0;
//     q->name = name;
//     q->flags = flags;

//     if (flags & PROC_QUEUE_FLAG_LOCK) {
//         // If the queue is supposed to be locked, we can initialize the lock here.
//         spin_acquire(&q->lock);
//     }
// }

// void proc_queue_entry_init(proc_queue_entry_t *entry, proc_queue_t *q);

