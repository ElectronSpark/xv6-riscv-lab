#include "types.h"
#include "string.h"
#include "errno.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "defs.h"
#include "printf.h"
#include "proc/tq.h"
#include "list.h"
#include "rbtree.h"

#define tq_enqueued(node)                                                      \
    (((node)->type == THREAD_QUEUE_TYPE_LIST && (node)->list.queue != NULL) || \
     ((node)->type == THREAD_QUEUE_TYPE_TREE && (node)->tree.queue != NULL))

void tq_init(tq_t *q, const char *name, spinlock_t *lock) {
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
    tnode_t *node1 = (tnode_t *)key1;
    tnode_t *node2 = (tnode_t *)key2;
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
    tnode_t *tnode = container_of(node, tnode_t, tree.entry);
    return (uint64)tnode;
}

static struct rb_root_opts __q_root_opts = {
    .keys_cmp_fun = __q_root_keys_cmp_fun,
    .get_key_fun = __q_root_get_key_fun,
};

void ttree_init(ttree_t *q, const char *name, spinlock_t *lock) {
    rb_root_init(&q->root, &__q_root_opts);
    q->counter = 0;
    if (name == NULL) {
        q->name = "NULL"; // Default name if none provided
    } else {
        q->name = name;
    }
    q->lock = lock;
}

void tq_set_lock(tq_t *q, spinlock_t *lock) {
    if (q != NULL) {
        q->lock = lock;
    }
}

void ttree_set_lock(ttree_t *q, spinlock_t *lock) {
    if (q != NULL) {
        q->lock = lock;
    }
}

// Initialize a thread node to None type
static void __tnode_to_none(tnode_t *node) {
    if (node == NULL) {
        return;
    }
    node->type = THREAD_QUEUE_TYPE_NONE;
}

// Initialize a thread node as a list node
static void __tnode_to_list(tnode_t *node) {
    if (node == NULL) {
        return;
    }
    node->type = THREAD_QUEUE_TYPE_LIST;
    list_entry_init(&node->list.entry);
    node->list.queue = NULL; // Initialize the queue pointer to NULL
}

// Initialize a thread node as a tree node
static void __tnode_to_tree(tnode_t *node) {
    if (node == NULL) {
        return;
    }
    node->type = THREAD_QUEUE_TYPE_TREE;
    rb_node_init(&node->tree.entry);
    node->tree.queue = NULL; // Initialize the queue pointer to NULL
}

void tnode_init(tnode_t *node) {
    memset(node, 0, sizeof(tnode_t));
    __tnode_to_none(node);
    node->error_no = 0; // Initialize error_no to 0
    node->thread =
        current; // Initialize the thread pointer to the current thread
}

int tq_size(tq_t *q) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }
    return q->counter;
}

int ttree_size(ttree_t *q) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }
    return q->counter;
}

tq_t *tnode_get_queue(tnode_t *node) {
    if (node == NULL) {
        return NULL; // Error: node is NULL
    }
    if (node->type != THREAD_QUEUE_TYPE_LIST) {
        return NULL; // Error: node is not in a list
    }
    return node->list.queue;
}

ttree_t *tnode_get_tree(tnode_t *node) {
    if (node == NULL) {
        return NULL; // Error: node is NULL
    }
    if (node->type != THREAD_QUEUE_TYPE_TREE) {
        return NULL; // Error: node is not in a tree
    }
    return node->tree.queue;
}

struct thread *tnode_get_thread(tnode_t *node) {
    if (node == NULL) {
        return NULL; // Error: node is NULL
    }
    return node->thread;
}

int tnode_get_errno(tnode_t *node, int *error_no) {
    if (node == NULL || error_no == NULL) {
        return -EINVAL; // Error: node or errno pointer is NULL
    }
    *error_no = node->error_no;
    return 0;
}

int tq_push(tq_t *q, tnode_t *node) {
    if (q == NULL || tnode_get_thread(node) == NULL) {
        return -EINVAL; // Error: queue or thread is NULL
    }

    if (tq_enqueued(node)) {
        return -EINVAL; // Error: thread is already in a queue
    }

    __tnode_to_list(node); // Initialize the node as a list node
    list_node_push(&q->head, node, list.entry);
    node->list.queue = q;
    q->counter++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return 0; // Success
}

tnode_t *tq_first(tq_t *q) {
    if (q == NULL) {
        return ERR_PTR(-EINVAL); // Error: queue is NULL
    }

    if (q->counter == 0) {
        return NULL; // Queue is empty or invalid
    } else if (q->counter < 0) {
        return ERR_PTR(-EINVAL); // Error: queue has invalid counter
    }

    tnode_t *first_node = LIST_FIRST_NODE(&q->head, tnode_t, list.entry);
    assert(first_node != NULL,
           "tq_first: queue is not empty but failed to get the first node");
    return first_node;
}

int tq_remove(tq_t *q, tnode_t *node) {
    if (q == NULL || tnode_get_thread(node) == NULL) {
        return -EINVAL; // Error: queue or thread is NULL
    }

    if (tnode_get_queue(node) != q) {
        return -EINVAL; // Error: thread is not in the specified queue
    }

    if (q->counter <= 0) {
        panic("tq_remove: queue is empty");
    }

    list_node_detach(node, list.entry);
    __tnode_to_none(node);
    q->counter--;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return 0; // Success
}

tnode_t *tq_pop(tq_t *q) {
    if (q == NULL) {
        return ERR_PTR(-EINVAL); // Error: queue is NULL
    }
    tnode_t *dequeued_node = tq_first(q);
    if (IS_ERR_OR_NULL(dequeued_node)) {
        return dequeued_node;
    }
    assert(tnode_get_queue(dequeued_node) == q,
           "Dequeued node is not in the expected queue");
    int ret = tq_remove(q, dequeued_node);
    if (ret == 0) {
        return dequeued_node; // Return the dequeued node
    }
    return ERR_PTR(ret); // Return the error code if failed to remove the node
}

// Move all thread from one queue to another.
// This is to enconvinience walking up all thread in a queue.
// This will not change the pointer of the threads to their queues.
// 'to' and 'from' must be different queues.
int tq_bulk_move(tq_t *to, tq_t *from) {
    if (to == NULL || from == NULL) {
        return -EINVAL; // Error: one of the queues is NULL
    }
    if (to == from) {
        return -EINVAL; // Error: cannot move to the same queue
    }
    if (to->counter > 0) {
        return -ENOTEMPTY; // Error: destination queue is not empty
    }
    if (from->counter == 0) {
        return 0;
    } else if (from->counter < 0) {
        return -EINVAL; // Error: source queue has invalid counter
    }

    to->counter += from->counter;
    from->counter = 0;
    list_entry_insert_bulk(LIST_LAST_ENTRY(&to->head), &from->head);
    tnode_t *proc = NULL;
    tnode_t *tmp = NULL;
    list_foreach_node_safe(&to->head, proc, tmp, list.entry) {
        assert(tnode_get_queue(proc) == from,
               "Thread is not in the expected queue");
        proc->list.queue = to; // Update the queue pointer for each thread
    }

    return 0; // Success
}

/**
 * Core list-queue wait with custom sleep/wakeup callbacks.
 *
 * Protocol:
 *   1. Disable interrupts (prevent timer/signal races during enqueue)
 *   2. Set thread state to @state
 *   3. Enqueue waiter onto @q
 *   4. Invoke sleep_callback (typically releases the caller's lock);
 *      its return value is forwarded as @c status to wakeup_callback
 *   5. scheduler_yield() — thread is descheduled
 *   6. On resume: invoke wakeup_callback with the sleep_callback status
 *   7. Self-detach from @q if still enqueued (async wakeup by signal)
 *   8. Restore interrupt state
 */
int tq_wait_in_state_cb(tq_t *q, sleep_callback_t sleep_callback,
                        wakeup_callback_t wakeup_callback, void *callback_data,
                        uint64 *rdata, enum thread_state state) {
    if (q == NULL) {
        return -EINVAL;
    }

    if (!THREAD_IS_SLEEPING(state)) {
        return -EINVAL; // Invalid state for sleeping
    }

    int intr = intr_off_save();
    struct thread *cur = current;
    tnode_t waiter = {0};
    __thread_state_set(cur, state);
    tnode_init(&waiter);
    // Will be cleared when waking up a thread with thread queue APIs
    waiter.error_no = -EINTR;
    if (tq_push(q, &waiter) != 0) {
        panic("Failed to push thread to sleep queue");
    }

    int cb_status = 0;
    if (sleep_callback != NULL) {
        cb_status = sleep_callback(callback_data);
    }
    scheduler_yield();
    if (wakeup_callback != NULL) {
        wakeup_callback(callback_data, cb_status);
    }

    if (tq_enqueued(&waiter)) {
        // When the thread is waken up by the queue leader, the waiter is
        // already detached from the queue. If it's waken up asynchronously(e.g
        // by signals), we need to remove it from the queue.
        assert(tq_remove(q, &waiter) == 0,
               "Failed to remove interrupted waiter from queue");
    }
    intr_restore(intr);

    if (rdata != NULL) {
        *rdata = waiter.data;
    }
    return waiter.error_no;
}

int tq_wait_in_state(tq_t *q, spinlock_t *lock, uint64 *rdata,
                     enum thread_state state) {
    return tq_wait_in_state_cb(q, spin_sleep_cb, spin_wake_cb, lock, rdata,
                               state);
}

int tq_wait_cb(tq_t *q, sleep_callback_t sleep_callback,
               wakeup_callback_t wakeup_callback, void *callback_data,
               uint64 *rdata) {
    return tq_wait_in_state_cb(q, sleep_callback, wakeup_callback,
                               callback_data, rdata, THREAD_UNINTERRUPTIBLE);
}

int tq_wait(tq_t *q, spinlock_t *lock, uint64 *rdata) {
    return tq_wait_in_state_cb(q, spin_sleep_cb, spin_wake_cb, lock, rdata,
                               THREAD_UNINTERRUPTIBLE);
}

static struct thread *__do_wakeup(tnode_t *woken, int error_no, uint64 rdata) {
    if (woken == NULL) {
        return ERR_PTR(-EINVAL); // Nothing to wake up
    }
    if (woken->thread == NULL) {
        printf("woken thread is NULL\n");
        return ERR_PTR(-EINVAL);
    }
    woken->error_no = error_no; // Set the error number for the woken thread
    woken->data = rdata;        // Set the data for the woken thread
    struct thread *p = woken->thread;
    // Note: pi_lock is acquired internally by scheduler_wakeup
    scheduler_wakeup(p);
    return p;
}

static struct thread *__tq_wakeup_one(tq_t *q, int error_no, uint64 rdata) {
    if (q == NULL) {
        return ERR_PTR(-EINVAL);
    }

    tnode_t *woken = tq_pop(q);
    if (IS_ERR_OR_NULL(woken)) {
        return ERR_CAST(woken); // No thread to wake up
    }

    return __do_wakeup(woken, error_no, rdata);
}

struct thread *tq_wakeup(tq_t *q, int error_no, uint64 rdata) {
    return __tq_wakeup_one(q, error_no, rdata);
}

int tq_wakeup_all(tq_t *q, int error_no, uint64 rdata) {
    if (q == NULL) {
        return -EINVAL;
    }

    int counter = 0;
    for (;;) {
        struct thread *p = __tq_wakeup_one(q, error_no, rdata);
        if (p == NULL) {
            assert(q->counter == 0,
                   "Queue counter is not zero when queue is empty");
            break; // Queue is empty
        }
        if (IS_ERR(p)) {
            return PTR_ERR(p);
        }
        counter++;
    }

    return counter; // Return the number of threads woken up
}

// RB Tree based thread queue
// Because there may be more than one thread with the same key,
// we need to round down the key to find the minimum key node.
// Since key2 is the node to compare with, and we want to find the first node
// with key >= key1, we can just return 1 when key1 == key2
static int __q_root_keys_cmp_rdown(uint64 key1, uint64 key2) {
    tnode_t *node1 = (tnode_t *)key1;
    tnode_t *node2 = (tnode_t *)key2;
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

// Check if a thread node is in the tree.
// It will only check the node. Will not try to search in the tree.
static bool __tnode_in_tree(ttree_t *q, tnode_t *node) {
    if (q == NULL || node == NULL) {
        return false;
    }

    if (node->type != THREAD_QUEUE_TYPE_TREE) {
        return false; // Node is not a tree node
    }
    if (node->tree.queue != q) {
        return false; // Node is not in the specified tree
    }
    return true;
}

static tnode_t *__ttree_find_key_min(ttree_t *q, uint64 key) {
    if (q == NULL) {
        return NULL;
    }

    struct rb_root dummy_root = q->root;
    dummy_root.opts = &__q_root_rdown_opts;

    tnode_t dummy = {.tree.key = key, 0};

    struct rb_node *node = rb_find_key_rup(&dummy_root, (uint64)&dummy);
    if (node == NULL) {
        return NULL; // No node found
    }
    tnode_t *target = container_of(node, tnode_t, tree.entry);
    if (target->tree.key != key) {
        return NULL; // No matching key found
    }
    return target;
}

int ttree_add(ttree_t *q, tnode_t *node) {
    if (q == NULL || node == NULL || tnode_get_thread(node) == NULL) {
        return -EINVAL; // Error: queue or thread is NULL
    }

    if (tq_enqueued(node)) {
        return -EINVAL; // Error: thread is already in a queue
    }

    __tnode_to_tree(node); // Initialize the node as a tree node
    node->tree.queue = q;  // Set the queue pointer
    struct rb_node *inserted_node =
        rb_insert_color(&q->root, &node->tree.entry);
    assert(inserted_node == &node->tree.entry,
           "Failed to insert node into tree");
    q->counter++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return 0; // Success
}

tnode_t *ttree_first(ttree_t *q) {
    if (q == NULL) {
        return ERR_PTR(-EINVAL); // Error: queue or return node pointer is NULL
    }

    struct rb_node *first_node = rb_first_node(&q->root);
    if (IS_ERR_OR_NULL(first_node)) {
        return ERR_CAST(first_node); // No first node found
    }

    return container_of(first_node, tnode_t, tree.entry);
}

int ttree_key_min(ttree_t *q, uint64 *key) {
    tnode_t *min_node = ttree_first(q);
    if (min_node == NULL) {
        return -ENOENT; // Error: tree is empty
    } else if (IS_ERR(min_node)) {
        return PTR_ERR(min_node); // Error: failed to get the first node
    }
    *key = min_node->tree.key;
    return 0; // Success
}

static int __ttree_do_remove(ttree_t *q, tnode_t *node) {
    struct rb_node *removed_node =
        rb_delete_node_color(&q->root, &node->tree.entry);
    if (removed_node == NULL) {
        return -ENOENT; // Error: node not found
    }

    __tnode_to_none(node);
    q->counter--;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return 0; // Success
}

// Remove a thread node from a thread tree.
// Will not check if the node is in the tree.
int ttree_remove(ttree_t *q, tnode_t *node) {
    if (q == NULL || node == NULL) {
        return -EINVAL; // Error: queue or node is NULL
    }
    if (!__tnode_in_tree(q, node)) {
        return -EINVAL; // Error: node is not in the tree
    }
    return __ttree_do_remove(q, node);
}

/**
 * Core tree-queue wait with custom sleep/wakeup callbacks.
 * Same protocol as tq_wait_in_state_cb(), but inserts the waiter
 * into the red-black tree keyed by @key.
 */
int ttree_wait_in_state_cb(ttree_t *q, uint64 key,
                           sleep_callback_t sleep_callback,
                           wakeup_callback_t wakeup_callback,
                           void *callback_data, uint64 *rdata,
                           enum thread_state state) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }

    if (!THREAD_IS_SLEEPING(state)) {
        return -EINVAL; // Invalid state for sleeping
    }

    struct thread *cur = current;
    int intr = intr_off_save();
    __thread_state_set(cur, state);
    tnode_t waiter = {0};
    tnode_init(&waiter);
    // Will be cleared when waking up a thread with thread queue APIs
    waiter.error_no = -EINTR;
    waiter.tree.key = key;

    if (ttree_add(q, &waiter) != 0) {
        panic("Failed to push thread to sleep tree");
    }

    int cb_status = 0;
    if (sleep_callback != NULL) {
        cb_status = sleep_callback(callback_data);
    }
    scheduler_yield();
    if (wakeup_callback != NULL) {
        wakeup_callback(callback_data, cb_status);
    }

    if (tq_enqueued(&waiter)) {
        // When the thread is waken up by the queue leader, the waiter is
        // already detached from the queue. If it's waken up asynchronously(e.g
        // by signals), we need to remove it from the queue.
        assert(ttree_remove(q, &waiter) == 0,
               "Failed to remove interrupted waiter from tree");
    }
    intr_restore(intr);

    if (rdata != NULL) {
        *rdata = waiter.data;
    }
    return waiter.error_no;
}

int ttree_wait_in_state(ttree_t *q, uint64 key, spinlock_t *lock,
                        uint64 *rdata, enum thread_state state) {
    return ttree_wait_in_state_cb(q, key, spin_sleep_cb, spin_wake_cb, lock,
                                  rdata, state);
}

int ttree_wait_cb(ttree_t *q, uint64 key, sleep_callback_t sleep_callback,
                  wakeup_callback_t wakeup_callback, void *callback_data,
                  uint64 *rdata) {
    return ttree_wait_in_state_cb(q, key, sleep_callback, wakeup_callback,
                                  callback_data, rdata, THREAD_UNINTERRUPTIBLE);
}

int ttree_wait(ttree_t *q, uint64 key, spinlock_t *lock, uint64 *rdata) {
    return ttree_wait_in_state_cb(q, key, spin_sleep_cb, spin_wake_cb, lock,
                                  rdata, THREAD_UNINTERRUPTIBLE);
}

// Wake up one node with a given key
// Thread tree will always expect the waiter to detach itself from the tree when
// woken up.
struct thread *ttree_wakeup_one(ttree_t *q, uint64 key, int error_no,
                                uint64 rdata) {
    if (q == NULL) {
        return ERR_PTR(-EINVAL); // Error: queue is NULL
    }

    tnode_t *target = __ttree_find_key_min(q, key);
    if (target == NULL) {
        return ERR_PTR(-ENOENT); // Error: no matching node found
    }

    int ret = __ttree_do_remove(q, target);
    if (ret != 0) {
        return ERR_PTR(ret); // Error: failed to remove node from tree
    }

    return __do_wakeup(target, error_no, rdata);
}

// Wake up all nodes with a given key
int ttree_wakeup_key(ttree_t *q, uint64 key, int error_no, uint64 rdata) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }

    int count = 0;

    while (!IS_ERR_OR_NULL(ttree_wakeup_one(q, key, error_no, rdata))) {
        count++;
    }

    if (count == 0) {
        return -ENOENT; // Error: no nodes with the given key found
    }
    return 0;
}

int ttree_wakeup_all(ttree_t *q, int error_no, uint64 rdata) {
    if (q == NULL) {
        return -EINVAL; // Error: queue is NULL
    }
    if (q->counter <= 0) {
        return -ENOENT; // Error: no nodes in the tree
    }

    int count = 0;
    tnode_t *pos = NULL;
    tnode_t *n = NULL;

    rb_foreach_entry_safe(&q->root, pos, n, tree.entry) {
        assert(__tnode_in_tree(q, pos), "Thread node is not in the tree");
        // @TODO: The whole tree will be abandoned, so don't need to adjust its
        // structure.
        if (__ttree_do_remove(q, pos) != 0) {
            printf(
                "warning: Failed to remove node from tree during wakeup all\n");
        }
        __do_wakeup(pos, error_no, rdata);
        count++;
    }

    if (count == 0) {
        return -ENOENT; // Error: no nodes with the given key found
    }
    q->root.node = NULL; // Clear the root node after waking up all nodes
    return 0;
}
