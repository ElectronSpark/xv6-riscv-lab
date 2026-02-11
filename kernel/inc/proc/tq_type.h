#ifndef KERNEL_THREAD_QUEUE_TYPE_H
#define KERNEL_THREAD_QUEUE_TYPE_H

#include "types.h"
#include "list_type.h"
#include "lock/spinlock.h"
#include "bintree_type.h"

typedef struct tq tq_t;
typedef struct ttree ttree_t;

typedef enum {
    THREAD_QUEUE_TYPE_NONE = 0,
    THREAD_QUEUE_TYPE_LIST,
    THREAD_QUEUE_TYPE_TREE,
} tq_type_t;

typedef struct tnode {
    tq_type_t type;
    union {
        struct {
            list_node_t entry;
            tq_t *queue; // Pointer to the queue this entry belongs to
        } list;
        struct {
            struct rb_node entry;
            ttree_t *queue; // Pointer to the queue this entry belongs to
            uint64 key;
        } tree;
    };
    int error_no; // 0: Wake up by queue leader, -EINTR: Wake up by signal
    uint64 data;  // data passed to the thread when wakening it up
    struct thread *thread; // Pointer to the thread this node represents
} tnode_t;

typedef struct tq {
    list_node_t head; // List of threads in the queue
    int counter;      // Number of threads in the queue
    const char *name; // Name of the queue
    spinlock_t *lock; // Spinlock for the queue, it's optional
    uint64 flags;
} tq_t;

typedef struct ttree {
    struct rb_root root;
    int counter;      // Number of threads in the queue
    const char *name; // Name of the queue
    spinlock_t *lock; // Spinlock for the queue, it's optional
    uint64 flags;
} ttree_t;

#define THREAD_QUEUE_FLAG_VALID (1UL << 0) // indicates that the queue is valid
#define THREAD_QUEUE_FLAG_LOCK                                                 \
    (1UL << 1) // will try to acquire spinlock before using the queue

#endif // KERNEL_THREAD_QUEUE_TYPE_H