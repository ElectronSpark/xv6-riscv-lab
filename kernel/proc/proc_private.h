#ifndef __KERNEL_THREAD_PRIVATE_H__
#define __KERNEL_THREAD_PRIVATE_H__

#include "types.h"
#include "hlist.h"
#include "list.h"
#include "lock/spinlock.h"

#define NR_THREAD_HASH_BUCKETS 31

void __proctab_init(void);
void __proctab_set_initproc(struct thread *p);
struct thread *__proctab_get_initproc(void);
int __alloc_pid(void);
void __free_pid(void);

void proctab_proc_add(struct thread *p);
void proctab_proc_remove(struct thread *p);

// Register the given thread as the idle process for the current CPU.
void register_idle_threadess(struct thread *p);

void init_idle_rq(void);
void init_fifo_rq(void);
void init_fifo_rq_range(int start_cls_id, int end_cls_id);

// Run queue priority tests
void rq_test_run(void);

#endif // __KERNEL_THREAD_PRIVATE_H__
