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
int proctab_get_pid_proc(int pid, struct proc **pp);
void proctab_proc_add(struct proc *p);
void proctab_proc_remove(struct proc *p);


#endif // __KERNEL_PROC_PRIVATE_H__
