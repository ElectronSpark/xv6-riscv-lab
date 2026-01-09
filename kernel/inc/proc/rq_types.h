#ifndef __KERNEL_PROC_RQ_TYPES_H
#define __KERNEL_PROC_RQ_TYPES_H

#include "compiler.h"
#include "riscv.h"
#include "types.h"
#include "spinlock.h"
#include "list_type.h"

struct rq {
    struct spinlock lock;
    list_node_t proc_list;  // List of processes in this run queue
    int proc_count;         // Number of processes in the run queue
} __ALIGNED_CACHELINE;

#endif  // __KERNEL_PROC_RQ_TYPES_H
