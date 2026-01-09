#ifndef __KERNEL_PROC_PRIVATE_H__
#define __KERNEL_PROC_PRIVATE_H__

#include "types.h"
#include "hlist.h"
#include "list.h"
#include "spinlock.h"

#define NPROC_HASH_BUCKETS 31

void __proctab_init(void);
void __proctab_set_initproc(struct proc *p);
struct proc *__proctab_get_initproc(void);
struct proc *__proctab_get_pid_proc(int pid);
int __alloc_pid(void);

/**
 * @brief Get a process by PID using RCU (lock-free)
 * @param pid The PID to look up
 * @param pp Output pointer to the found process
 * @return 0 on success, -1 on invalid arguments
 *
 * This function uses RCU for lock-free lookup. The caller MUST be
 * within an rcu_read_lock()/rcu_read_unlock() critical section.
 * The returned pointer is only valid within the RCU critical section.
 */
int proctab_get_pid_proc(int pid, struct proc **pp);

void proctab_proc_add(struct proc *p);
void proctab_proc_remove(struct proc *p);


#endif // __KERNEL_PROC_PRIVATE_H__
