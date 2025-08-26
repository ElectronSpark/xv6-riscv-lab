#ifndef KERNEL_PROC_QUEUE_TYPE_H
#define KERNEL_PROC_QUEUE_TYPE_H

#include "types.h"
#include "list_type.h"
#include "spinlock.h"
#include "bintree_type.h"

typedef struct proc_queue proc_queue_t;
typedef struct proc_tree proc_tree_t;

typedef enum {
    PROC_QUEUE_TYPE_NONE = 0,
    PROC_QUEUE_TYPE_LIST,
    PROC_QUEUE_TYPE_TREE,
} proc_queue_type_t;

typedef struct proc_node {
    proc_queue_type_t type;
    union {
        struct {
            list_node_t entry;
            proc_queue_t *queue; // Pointer to the queue this entry belongs to
        } list;
        struct {
            struct rb_node entry;
            proc_tree_t *queue; // Pointer to the queue this entry belongs to
            uint64 key;
        } tree;
    };
    int error_no;  // 0: Wake up by queue leader, -EINTR: Wake up by signal
    uint64 data; // data passed to the process when wakening it up
    struct proc *proc;   // Pointer to the process this node represents
} proc_node_t;

typedef struct proc_queue {
    list_node_t head; // List of processes in the queue
    int counter; // Number of processes in the queue
    const char *name; // Name of the queue
    spinlock_t *lock; // Spinlock for the queue, it's optional
    uint64 flags;
} proc_queue_t;

typedef struct proc_tree {
    struct rb_root root;
    int counter; // Number of processes in the queue
    const char *name; // Name of the queue
    spinlock_t *lock; // Spinlock for the queue, it's optional
    uint64 flags;
} proc_tree_t;

#define PROC_QUEUE_FLAG_VALID   (1UL << 0) // indicates that the queue is valid
#define PROC_QUEUE_FLAG_LOCK    (1UL << 1) // will try to acquire spinlock before using the queue

#endif // KERNEL_PROC_QUEUE_TYPE_H