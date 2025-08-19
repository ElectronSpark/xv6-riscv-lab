#include "types.h"
#include "errno.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "sched.h"
#include "defs.h"
#include "proc_queue.h"
#include "list.h"
#include "rbtree.h"

#define proc_queue_enqueued(node)   \
    (((node)->type == PROC_QUEUE_TYPE_LIST && (node)->list.queue != NULL)   \
    || ((node)->type == PROC_QUEUE_TYPE_TREE && (node)->tree.queue != NULL))

void proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock) {
    list_entry_init(&q->head);
    q->counter = 0;
    if (name == NULL) {
        q->name = "NULL"; // Default name if none provided
    } else {
        q->name = name;
    }
    q->lock = lock;
}

static int __q_root_keys_cmp_fun(uint64 key1, uint64 key2) {
    proc_node_t *node1 = (proc_node_t *)key1;
    proc_node_t *node2 = (proc_node_t *)key2;
    // First compare node->tree.key, it they are equal, use the address of
    // the nodes as distinguishing factors.
    if (node1->tree.key < node2->tree.key) {
        return -1;
    } else if (node1->tree.key > node2->tree.key) {
        return 1;
    } else if (key1 < key2) {
        return -1;
    } else if (key1 > key2) {
        return 1;
    } else {
        return 0; // Equal
    }
}

static uint64 __q_root_get_key_fun(struct rb_node *node) {
    assert(node != NULL, "node is NULL");
    proc_node_t *proc_node = container_of(node, proc_node_t, tree.entry);
    return (uint64)proc_node;
}

static struct rb_root_opts __q_root_opts = {
    .keys_cmp_fun = __q_root_keys_cmp_fun,
    .get_key_fun = __q_root_get_key_fun,
};

void proc_tree_init(proc_tree_t *q, const char *name, spinlock_t *lock) {
    rb_root_init(&q->root, &__q_root_opts);
    q->counter = 0;
    if (name == NULL) {
        q->name = "NULL"; // Default name if none provided
    } else {
        q->name = name;
    }
    q->lock = lock;
}

void proc_queue_set_lock(proc_queue_t *q, spinlock_t *lock) {
    if (q != NULL) {
        q->lock = lock;
    }
}

void proc_tree_set_lock(proc_tree_t *q, spinlock_t *lock) {
    if (q != NULL) {
        q->lock = lock;
    }
}

// Initialize a proc node to None type
static void __proc_node_to_none(proc_node_t *node) {
    if (node == NULL) {
        return;
    }
    node->type = PROC_QUEUE_TYPE_NONE;
}

// Initialize a proc node as a list node
static void __proc_node_to_list(proc_node_t *node) {
    if (node == NULL) {
        return;
    }
    node->type = PROC_QUEUE_TYPE_LIST;
    list_entry_init(&node->list.entry);
    node->list.queue = NULL; // Initialize the queue pointer to NULL
}

// Initialize a proc node as a tree node
static void __proc_node_to_tree(proc_node_t *node) {
    if (node == NULL) {
        return;
    }
    node->type = PROC_QUEUE_TYPE_TREE;
    rb_node_init(&node->tree.entry);
    node->tree.queue = NULL; // Initialize the queue pointer to NULL
}

void proc_node_init(proc_node_t *node) {
    memset(node, 0, sizeof(proc_node_t));
    __proc_node_to_none(node);
    node->errno = 0; // Initialize errno to 0
    node->proc = myproc();  // Initialize the process pointer to the current process
}

int proc_queue_size(proc_queue_t *q) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }
    return q->counter;
}

int proc_tree_size(proc_tree_t *q) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }
    return q->counter;
}

proc_queue_t *proc_node_get_queue(proc_node_t *node) {
    if (node == NULL) {
        return NULL; // Error: node is NULL
    }
    if (node->type != PROC_QUEUE_TYPE_LIST) {
        return NULL; // Error: node is not in a list
    }
    return node->list.queue;
}

proc_tree_t *proc_node_get_tree(proc_node_t *node) {
    if (node == NULL) {
        return NULL; // Error: node is NULL
    }
    if (node->type != PROC_QUEUE_TYPE_TREE) {
        return NULL; // Error: node is not in a tree
    }
    return node->tree.queue;
}

struct proc *proc_node_get_proc(proc_node_t *node) {
    if (node == NULL) {
        return NULL; // Error: node is NULL
    }
    return node->proc;
}

int proc_node_get_errno(proc_node_t *node, int *errno) {
    if (node == NULL || errno == NULL) {
        return -EINVAL; // Error: node or errno pointer is NULL
    }
    *errno = node->errno;
    return 0;
}

int proc_queue_push(proc_queue_t *q, proc_node_t *node) {
    if (q == NULL || proc_node_get_proc(node) == NULL) {
        return -EINVAL; // Error: queue or process is NULL
    }

    if (proc_queue_enqueued(node)) {
        return -EINVAL; // Error: process is already in a queue
    }

    __proc_node_to_list(node); // Initialize the node as a list node
    list_node_push(&q->head, node, list.entry);
    node->list.queue = q;
    q->counter++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // printf("pushing process %d to queue %s\n", p->pid, q->name);

    return 0; // Success
}

int proc_queue_first(proc_queue_t *q, proc_node_t **ret_node) {
    if (q == NULL || ret_node == NULL) {
        return -1; // Error: queue or process is NULL
    }

    if (q->counter <= 0) {
        *ret_node = NULL; // Set ret_node to NULL if queue is empty
        if (q->counter == 0) {
            return 0;
        } else {
            return -EINVAL;
        }
    }

    proc_node_t *first_node = LIST_FIRST_NODE(&q->head, proc_node_t, list.entry);
    assert(first_node != NULL, "proc_queue_first: queue is not empty but failed to get the first node");
    *ret_node = first_node; // Copy the process data

    return 0; // Success
}

int proc_queue_remove(proc_queue_t *q, proc_node_t *node) {
    if (q == NULL || proc_node_get_proc(node) == NULL) {
        return -EINVAL; // Error: queue or process is NULL
    }

    if (proc_node_get_queue(node) != q) {
        return -EINVAL; // Error: process is not in the specified queue
    }

    if (q->counter <= 0) {
        panic("proc_queue_remove: queue is empty");
    }

    list_node_detach(node, list.entry);
    __proc_node_to_none(node);
    q->counter--;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // printf("removing process %d from queue %s\n", proc_node_get_proc(node)->pid, q->name);

    return 0; // Success
}

int proc_queue_pop(proc_queue_t *q, proc_node_t **ret_node) {
    if (q == NULL || ret_node == NULL) {
        return -EINVAL; // Error: queue or ret_node is NULL
    }
    proc_node_t *dequeued_node = NULL;
    int ret = proc_queue_first(q, &dequeued_node);
    if (ret != 0) {
        return ret;
    }
    if (dequeued_node == NULL) {
        *ret_node = NULL; // Queue is empty
        return 0; // Success, but no node to return
    }
    assert(proc_node_get_queue(dequeued_node) == q, "Dequeued node is not in the expected queue");
    ret = proc_queue_remove(q, dequeued_node);
    if (ret == 0) {
        *ret_node = dequeued_node; // Return the dequeued node
    } else {
        *ret_node = NULL; // Error occurred, set ret_node to NULL
    }
    return ret;
}

// Move all process from one queue to another.
// This is to enconvinience walking up all process in a queue.
// This will not change the pointer of the processes to their queues.
int proc_queue_bulk_move(proc_queue_t *to, proc_queue_t *from) {
    if (to == NULL || from == NULL) {
        return -EINVAL; // Error: one of the queues is NULL
    }

    if (from->counter <= 0) {
        return 0;
    }

    to->counter += from->counter;
    from->counter = 0;
    list_entry_insert_bulk(LIST_LAST_ENTRY(&to->head), &from->head);
    proc_node_t *proc = NULL;
    proc_node_t *tmp = NULL;
    list_foreach_node_safe(&to->head, proc, tmp, list.entry) {
        assert(proc_node_get_queue(proc) == to, "Process is not in the expected queue");
        proc->list.queue = to; // Update the queue pointer for each process
    }

    return 0; // Success
}

int proc_queue_wait(proc_queue_t *q, struct spinlock *lock) {
    if (q == NULL) {
        return -EINVAL;
    }

    proc_node_t waiter = { 0 };
    proc_node_init(&waiter);
    // Will be cleared when waking up a process with proc queue APIs
    waiter.errno = -EINTR;
    proc_lock(myproc());
    if (proc_queue_push(q, &waiter) != 0) {
        panic("Failed to push process to sleep queue");
    }

    __proc_set_pstate(myproc(), PSTATE_UNINTERRUPTIBLE);
    scheduler_sleep(lock);
    if (proc_queue_enqueued(&waiter)) {
        // When the process is waken up by the queue leader, the waiter is already detached from the queue.
        // If it's waken up asynchronously(e.g by signals), we need to remove it from the queue.
        assert(proc_queue_remove(q, &waiter) == 0, "Failed to remove interrupted waiter from queue");
    }
    proc_unlock(myproc());

    return waiter.errno;
}

static void __do_wakeup(proc_node_t *woken, int errno, struct proc **retp) {
    if (woken == NULL) {
        return; // Nothing to wake up
    }
    if (woken->proc == NULL) {
        printf("woken process is NULL\n");
        return;
    }
    woken->errno = errno; // Set the error number for the woken process
    struct proc *p = woken->proc;
    proc_lock(p);
    sched_lock();
    if (retp != NULL) {
        __atomic_store_n(retp, p, __ATOMIC_SEQ_CST);
    }
    scheduler_wakeup(p);
    sched_unlock();
    proc_unlock(p);
}

static int __proc_queue_wakeup_one(proc_queue_t *q, int errno, struct proc **retp) {
    if (q == NULL) {
        return -EINVAL;
    }

    proc_node_t *woken = NULL;
    int ret = proc_queue_pop(q, &woken);
    if (ret != 0) {
        if (woken != NULL) {
            __do_wakeup(woken, ret, retp);
        }
        return ret; // Error: failed to pop process from queue
    }

    __do_wakeup(woken, errno, retp);
    return 0; // Success
}

int proc_queue_wakeup(proc_queue_t *q, int errno, struct proc **retp) {
    return __proc_queue_wakeup_one(q, errno, retp);
}

int proc_queue_wakeup_all(proc_queue_t *q, int errno) {
    if (q == NULL) {
        return -EINVAL;
    }

    for(;;) {
        int ret = __proc_queue_wakeup_one(q, errno, NULL);
        if (ret != 0) {
            return ret;
        }
        if (q->counter == 0) {
            break;
        }
    }

    return 0;
}

// RB Tree based proc queue
static int __q_root_keys_cmp_rdown(uint64 key1, uint64 key2) {
    proc_node_t *node1 = (proc_node_t *)key1;
    proc_node_t *node2 = (proc_node_t *)key2;
    // First compare node->tree.key, it they are equal, use the address of
    // the nodes as distinguishing factors.
    if (node1->tree.key < node2->tree.key) {
        return -1;
    } else if (node1->tree.key > node2->tree.key) {
        return 1;
    } else if (key1 == 0) {
        return 0;
    } else {
        return 1;
    }
}

static struct rb_root_opts __q_root_rdown_opts = {
    .keys_cmp_fun = __q_root_keys_cmp_rdown,
    .get_key_fun = __q_root_get_key_fun,
};

// Check if a process node is in the tree.
// It will only check the node. Will not try to search in the tree.
static bool __proc_node_in_tree(proc_tree_t *q, proc_node_t *node) {
    if (q == NULL || node == NULL) {
        return false;
    }

    if (node->type != PROC_QUEUE_TYPE_TREE) {
        return false; // Node is not a tree node
    }
    if (node->tree.queue != q) {
        return false; // Node is not in the specified tree
    }
    return true;
}

static proc_node_t *__proc_tree_find_key_min(proc_tree_t *q, uint64 key) {
    if (q == NULL) {
        return NULL;
    }
    
    struct rb_root dummy_root = q->root;
    dummy_root.opts = &__q_root_rdown_opts;

    proc_node_t dummy = {
        .tree.key = key,
        0
    };

    struct rb_node *node = rb_find_key_rup(&dummy_root, (uint64)&dummy);   
    if (node == NULL) {
        return NULL; // No node found
    }
    proc_node_t *target = container_of(node, proc_node_t, tree.entry);
    if (target->tree.key != key) {
        return NULL; // No matching key found
    }
    return target;
}

int proc_tree_add(proc_tree_t *q, proc_node_t *node) {
    if (q == NULL || node == NULL || proc_node_get_proc(node) == NULL) {
        return -EINVAL; // Error: queue or process is NULL
    }

    if (proc_queue_enqueued(node)) {
        return -EINVAL; // Error: process is already in a queue
    }

    __proc_node_to_tree(node); // Initialize the node as a tree node
    node->tree.queue = q; // Set the queue pointer
    struct rb_node *inserted_node = rb_insert_color(&q->root, &node->tree.entry);
    assert(inserted_node == &node->tree.entry, "Failed to insert node into tree");
    q->counter++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return 0; // Success
}

int proc_tree_first(proc_tree_t *q, proc_node_t **ret_node) {
    if (q == NULL || ret_node == NULL) {
        return -EINVAL; // Error: queue or return node pointer is NULL
    }

    struct rb_node *first_node = rb_first_node(&q->root);
    if (first_node == NULL) {
        return -ENODATA; // Error: no first node found
    }

    *ret_node = container_of(first_node, proc_node_t, tree.entry);
    return 0; // Success
}

int proc_tree_key_min(proc_tree_t *q, uint64 *key) {
    proc_node_t *min_node = NULL;
    int ret = proc_tree_first(q, &min_node);
    if (ret != 0) {
        return ret; // Error: failed to get the first node
    }
    *key = min_node->tree.key;
    return 0; // Success
}

static int __proc_tree_do_remove(proc_tree_t *q, proc_node_t *node) {
    struct rb_node *removed_node = rb_delete_node_color(&q->root, &node->tree.entry);
    if (removed_node == NULL) {
        return -ENOENT; // Error: node not found
    }

    __proc_node_to_none(node);
    q->counter--;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return 0; // Success
}

// Remove a proc node from a proc tree.
// Will not check if the node is in the tree.
int proc_tree_remove(proc_tree_t *q, proc_node_t *node) {
    if (q == NULL || node == NULL) {
        return -EINVAL; // Error: queue or node is NULL
    }
    if (!__proc_node_in_tree(q, node)) {
        return -EINVAL; // Error: node is not in the tree
    }
    return __proc_tree_do_remove(q, node);
}

int proc_tree_wait(proc_tree_t *q, uint64 key, struct spinlock *lock) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }

    proc_node_t waiter = { 0 };
    proc_node_init(&waiter);
    // Will be cleared when waking up a process with proc queue APIs
    waiter.errno = -EINTR;
    waiter.tree.key = key;

    proc_lock(myproc());
    if (proc_tree_add(q, &waiter) != 0) {
        panic("Failed to push process to sleep tree");
    }

    __proc_set_pstate(myproc(), PSTATE_UNINTERRUPTIBLE);
    scheduler_sleep(lock);
    if (proc_queue_enqueued(&waiter)) {
        // When the process is waken up by the queue leader, the waiter is already detached from the queue.
        // If it's waken up asynchronously(e.g by signals), we need to remove it from the queue.
        assert(proc_tree_remove(q, &waiter) == 0, "Failed to remove interrupted waiter from tree");
    }
    proc_unlock(myproc());

    return waiter.errno;
}

int proc_tree_wakeup_one(proc_tree_t *q, uint64 key, int errno, struct proc **retp) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }

    proc_node_t *target = __proc_tree_find_key_min(q, key);
    if (target == NULL) {
        return -ENOENT; // Error: no matching node found
    }

    if (__proc_tree_do_remove(q, target) != 0) {
        return -ENOENT; // Error: failed to remove node from tree
    }

    struct proc *p = NULL;
    __do_wakeup(target, errno, &p);
    if (p == NULL) {
        return -ENOENT; // Error: no process to wake up
    }

    return 0; // Success
}

// Wake up all nodes with a given key
int proc_tree_wakeup_key(proc_tree_t *q, uint64 key, int errno) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }

    int count = 0;

    while (proc_tree_wakeup_one(q, key, errno, NULL) == 0) {
        count++;
    }

    if (count == 0) {
        return -ENOENT; // Error: no nodes with the given key found
    }
    return 0;
}

int proc_tree_wakeup_all(proc_tree_t *q, int errno) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }
    if (q->counter <= 0) {
        return -ENOENT; // Error: no nodes in the tree
    }

    int count = 0;
    proc_node_t *pos = NULL;
    proc_node_t *n = NULL;

    rb_foreach_entry_safe(&q->root, pos, n, tree.entry) {
        assert(__proc_node_in_tree(q, pos), "Process node is not in the tree");
        // The whole tree will be abandoned, so don't need to adjust its structure.
        __proc_node_to_none(pos);
        __do_wakeup(pos, errno, NULL);
    }

    if (count == 0) {
        return -ENOENT; // Error: no nodes with the given key found
    }
    q->root.node = NULL; // Clear the root node after waking up all nodes
    return 0;
}
