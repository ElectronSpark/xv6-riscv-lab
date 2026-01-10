#ifndef __KERNEL_PROC_RQ_TYPES_H
#define __KERNEL_PROC_RQ_TYPES_H

#include "compiler.h"
#include "riscv.h"
#include "types.h"
#include "spinlock.h"
#include "list_type.h"

struct rq;
struct sched_entity;

struct rq {
    struct spinlock lock;
    list_node_t proc_list;  // List of processes in this run queue
    int proc_count;         // Number of processes in the run queue
} __ALIGNED_CACHELINE;

struct sched_entity {
    struct rq *rq;          // Pointer to the run queue
    list_node_t run_list;   // List node for the run queue
    int priority;           // Priority of the scheduling entity
    
};

#endif  // __KERNEL_PROC_RQ_TYPES_H
