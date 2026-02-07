#ifndef KERNEL_PROC_QUEUE_H
#define KERNEL_PROC_QUEUE_H

#include "proc/proc_queue_type.h"
#include "proc/proc_types.h"
#include "list.h"
#include "errno.h"

/**
 * proc_list_foreach_unlocked - iterate over a proc_queue without locking
 * @q: pointer to the proc_queue_t
 * @pos: proc_node_t * used as the loop cursor
 * @tmp: proc_node_t * used as temporary storage for safe iteration
 *
 * The caller must ensure no concurrent modifications or hold an
 * appropriate lock externally.
 */
#define proc_list_foreach_unlocked(q, pos, tmp)   \
    list_foreach_node_safe(&(q)->head, pos, tmp, list.entry)

/* ======================== Initialization ======================== */

/**
 * proc_queue_init - initialize a list-based process queue
 * @q: queue to initialize
 * @name: human-readable name for debugging (may be NULL)
 * @lock: spinlock that protects this queue (may be NULL)
 */
void proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock);

/**
 * proc_queue_set_lock - (re)assign the protecting spinlock of a queue
 * @q: queue (ignored if NULL)
 * @lock: new spinlock pointer
 */
void proc_queue_set_lock(proc_queue_t *q, spinlock_t *lock);

/**
 * proc_tree_init - initialize a red-black-tree-based process queue
 * @q: tree to initialize
 * @name: human-readable name for debugging (may be NULL)
 * @lock: spinlock that protects this tree (may be NULL)
 */
void proc_tree_init(proc_tree_t *q, const char *name, spinlock_t *lock);

/**
 * proc_tree_set_lock - (re)assign the protecting spinlock of a tree
 * @q: tree (ignored if NULL)
 * @lock: new spinlock pointer
 */
void proc_tree_set_lock(proc_tree_t *q, spinlock_t *lock);

/**
 * proc_node_init - initialize a proc_node for the current process
 * @node: node to initialize
 *
 * Zeroes the node, sets type to NONE, error_no to 0 and proc to myproc().
 * Must be called in process context.
 */
void proc_node_init(proc_node_t *node);

/* ======================== Accessors ======================== */

/**
 * proc_queue_size - return the number of nodes in a list queue
 * @q: queue to query
 *
 * Returns the count (>= 0) or -EINVAL if @q is NULL.
 */
int proc_queue_size(proc_queue_t *q);

/**
 * proc_tree_size - return the number of nodes in a tree queue
 * @q: tree to query
 *
 * Returns the count (>= 0) or -EINVAL if @q is NULL.
 */
int proc_tree_size(proc_tree_t *q);

/**
 * proc_node_get_queue - get the list queue a node belongs to
 * @node: node to query
 *
 * Returns the owning proc_queue_t, or NULL if @node is NULL, not a
 * list-type node, or not currently enqueued.
 */
proc_queue_t *proc_node_get_queue(proc_node_t *node);

/**
 * proc_node_get_tree - get the tree queue a node belongs to
 * @node: node to query
 *
 * Returns the owning proc_tree_t, or NULL if @node is NULL, not a
 * tree-type node, or not currently enqueued.
 */
proc_tree_t *proc_node_get_tree(proc_node_t *node);

/**
 * proc_node_get_proc - get the process associated with a node
 * @node: node to query
 *
 * Returns the struct proc pointer, or NULL if @node is NULL.
 */
struct proc *proc_node_get_proc(proc_node_t *node);

/**
 * proc_node_get_errno - retrieve the error number stored in a node
 * @node: node to query
 * @error_no: output pointer for the stored errno
 *
 * Returns 0 on success, -EINVAL if either argument is NULL.
 */
int proc_node_get_errno(proc_node_t *node, int *error_no);

/* ======================== List Queue Operations ======================== */

/**
 * proc_queue_push - append a node to the tail of a list queue
 * @q: target queue
 * @node: node to enqueue (must not already be enqueued)
 *
 * The node is converted to list type and its queue back-pointer is set.
 *
 * Returns 0 on success, -EINVAL if arguments are invalid or the node
 * is already enqueued.
 */
int proc_queue_push(proc_queue_t *q, proc_node_t *node);

/**
 * proc_queue_first - peek at the head of a list queue
 * @q: queue to inspect
 *
 * Returns the first proc_node_t without removing it.
 * Returns NULL if the queue is empty, ERR_PTR(-EINVAL) if @q is NULL
 * or has a corrupt counter.
 */
proc_node_t *proc_queue_first(proc_queue_t *q);

/**
 * proc_queue_pop - remove and return the head of a list queue
 * @q: queue to pop from
 *
 * Returns the dequeued proc_node_t (type reset to NONE).
 * Returns NULL if the queue is empty, or ERR_PTR on error.
 */
proc_node_t *proc_queue_pop(proc_queue_t *q);

/**
 * proc_queue_remove - remove a specific node from a list queue
 * @q: queue the node should belong to
 * @node: node to remove
 *
 * Panics if the queue counter is <= 0 (internal invariant violation).
 *
 * Returns 0 on success, -EINVAL if the node is not in @q.
 */
int proc_queue_remove(proc_queue_t *q, proc_node_t *node);

/**
 * proc_queue_bulk_move - transfer all nodes from one queue to another
 * @to: destination queue (must be empty)
 * @from: source queue (drained after the call)
 *
 * Moves the entire linked list in O(1) and updates each node's queue
 * back-pointer in O(n). @to and @from must be distinct.
 *
 * Returns 0 on success, -EINVAL on bad arguments, -ENOTEMPTY if @to
 * is not empty.
 */
int proc_queue_bulk_move(proc_queue_t *to, proc_queue_t *from);

/* ======================== Tree Queue Operations ======================== */

/**
 * proc_tree_add - insert a node into a tree queue
 * @q: target tree
 * @node: node to insert (must not already be enqueued)
 *
 * The node is converted to tree type, its tree.queue back-pointer is
 * set, and it is inserted into the red-black tree keyed by
 * node->tree.key (with the node address as a tiebreaker).
 *
 * Returns 0 on success, -EINVAL if arguments are invalid or the node
 * is already enqueued.
 */
int proc_tree_add(proc_tree_t *q, proc_node_t *node);

/**
 * proc_tree_first - peek at the minimum-key node in a tree queue
 * @q: tree to inspect
 *
 * Returns the proc_node_t with the smallest key, NULL if the tree is
 * empty, or ERR_PTR on error.
 */
proc_node_t *proc_tree_first(proc_tree_t *q);

/**
 * proc_tree_key_min - get the minimum key value in a tree queue
 * @q: tree to query
 * @key: output pointer for the minimum key
 *
 * Returns 0 on success, -ENOENT if the tree is empty, or a negative
 * errno on other errors.
 */
int proc_tree_key_min(proc_tree_t *q, uint64 *key);

/**
 * proc_tree_remove - remove a specific node from a tree queue
 * @q: tree the node should belong to
 * @node: node to remove
 *
 * Validates that @node is in @q before removal.
 *
 * Returns 0 on success, -EINVAL if the node is not in @q, -ENOENT if
 * the rb-tree deletion fails.
 */
int proc_tree_remove(proc_tree_t *q, proc_node_t *node);

/* ======================== Wait / Wakeup (List) ======================== */

/**
 * proc_queue_wait_in_state - sleep on a list queue in a given state
 * @q: queue to wait on
 * @lock: spinlock to release before sleeping (re-acquired on wakeup)
 * @rdata: optional output for data passed by the waker (may be NULL)
 * @state: process sleeping state (must satisfy PSTATE_IS_SLEEPING)
 *
 * Pushes the current process onto @q, calls scheduler_sleep(), and on
 * return self-removes from @q if still enqueued (e.g. async wakeup by
 * signal).
 *
 * Returns the waiter's error_no: 0 on normal wakeup, -EINTR if woken
 * asynchronously, or the error_no set by the waker.
 */
int proc_queue_wait_in_state(proc_queue_t *q, struct spinlock *lock, 
                             uint64 *rdata, enum procstate state);

/**
 * proc_queue_wait - sleep on a list queue (PSTATE_UNINTERRUPTIBLE)
 * @q: queue to wait on
 * @lock: spinlock to release before sleeping
 * @rdata: optional output for data passed by the waker (may be NULL)
 *
 * Convenience wrapper around proc_queue_wait_in_state().
 */
int proc_queue_wait(proc_queue_t *q, struct spinlock *lock, uint64 *rdata);

/**
 * proc_queue_wakeup - wake and dequeue the first waiter
 * @q: queue to wake from
 * @error_no: error code delivered to the waiter's proc_node.error_no
 * @rdata: data delivered to the waiter's proc_node.data
 *
 * Pops the head of @q, sets its error_no and data fields, then calls
 * scheduler_wakeup on the associated process.
 *
 * Returns:
 *   - pointer to the woken struct proc on success
 *   - NULL if the queue was empty
 *   - ERR_PTR(-EINVAL) if @q is NULL or internal error
 */
struct proc *proc_queue_wakeup(proc_queue_t *q, int error_no, uint64 rdata);

/**
 * proc_queue_wakeup_all - wake every waiter in a list queue
 * @q: queue to drain
 * @error_no: error code delivered to each waiter
 * @rdata: data delivered to each waiter
 *
 * Repeatedly pops and wakes waiters until the queue is empty.
 *
 * Returns the number of processes woken (>= 0), -EINVAL if @q is NULL,
 * or a negative errno if an internal wakeup fails.
 */
int proc_queue_wakeup_all(proc_queue_t *q, int error_no, uint64 rdata);

/* ======================== Wait / Wakeup (Tree) ======================== */

/**
 * proc_tree_wait_in_state - sleep on a tree queue with a given key
 * @q: tree to wait on
 * @key: sort key for the waiter (determines wakeup priority)
 * @lock: spinlock to release before sleeping
 * @rdata: optional output for data passed by the waker (may be NULL)
 * @state: process sleeping state (must satisfy PSTATE_IS_SLEEPING)
 *
 * Inserts the current process into @q keyed by @key, sleeps, and on
 * return self-removes if still enqueued.
 *
 * Returns the waiter's error_no (see proc_queue_wait_in_state).
 */
int proc_tree_wait_in_state(proc_tree_t *q, uint64 key, struct spinlock *lock, 
                            uint64 *rdata, enum procstate state);

/**
 * proc_tree_wait - sleep on a tree queue (PSTATE_UNINTERRUPTIBLE)
 * @q: tree to wait on
 * @key: sort key for the waiter
 * @lock: spinlock to release before sleeping
 * @rdata: optional output for data passed by the waker (may be NULL)
 *
 * Convenience wrapper around proc_tree_wait_in_state().
 */
int proc_tree_wait(proc_tree_t *q, uint64 key, struct spinlock *lock, uint64 *rdata);

/**
 * proc_tree_wakeup_one - wake the first waiter with a matching key
 * @q: tree to wake from
 * @key: key to match
 * @error_no: error code delivered to the waiter
 * @rdata: data delivered to the waiter
 *
 * Finds the minimum-address node whose tree.key == @key, removes it,
 * and wakes its process.
 *
 * Returns:
 *   - pointer to the woken struct proc on success
 *   - ERR_PTR(-ENOENT) if no node with @key exists
 *   - ERR_PTR(-EINVAL) if @q is NULL
 */
struct proc *proc_tree_wakeup_one(proc_tree_t *q, uint64 key, int error_no, uint64 rdata);

/**
 * proc_tree_wakeup_key - wake all waiters with a matching key
 * @q: tree to wake from
 * @key: key to match
 * @error_no: error code delivered to each waiter
 * @rdata: data delivered to each waiter
 *
 * Returns 0 if at least one waiter was woken, -ENOENT if none matched,
 * -EINVAL if @q is NULL.
 */
int proc_tree_wakeup_key(proc_tree_t *q, uint64 key, int error_no, uint64 rdata);

/**
 * proc_tree_wakeup_all - wake every waiter in a tree queue
 * @q: tree to drain
 * @error_no: error code delivered to each waiter
 * @rdata: data delivered to each waiter
 *
 * Iterates the tree in-order, removes and wakes each node.
 *
 * Returns 0 on success, -ENOENT if the tree was empty, -EINVAL if @q
 * is NULL.
 */
int proc_tree_wakeup_all(proc_tree_t *q, int error_no, uint64 rdata);

#endif // KERNEL_PROC_QUEUE_H
