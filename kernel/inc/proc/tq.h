#ifndef KERNEL_THREAD_QUEUE_H
#define KERNEL_THREAD_QUEUE_H

#include "proc/tq_type.h"
#include "proc/proc_types.h"
#include "list.h"
#include "errno.h"

/**
 * proc_list_foreach_unlocked - iterate over a tq without locking
 * @q: pointer to the tq_t
 * @pos: tnode_t * used as the loop cursor
 * @tmp: tnode_t * used as temporary storage for safe iteration
 *
 * The caller must ensure no concurrent modifications or hold an
 * appropriate lock externally.
 */
#define proc_list_foreach_unlocked(q, pos, tmp)   \
    list_foreach_node_safe(&(q)->head, pos, tmp, list.entry)

/* ======================== Initialization ======================== */

/**
 * tq_init - initialize a list-based process queue
 * @q: queue to initialize
 * @name: human-readable name for debugging (may be NULL)
 * @lock: spinlock that protects this queue (may be NULL)
 */
void tq_init(tq_t *q, const char *name, spinlock_t *lock);

/**
 * tq_set_lock - (re)assign the protecting spinlock of a queue
 * @q: queue (ignored if NULL)
 * @lock: new spinlock pointer
 */
void tq_set_lock(tq_t *q, spinlock_t *lock);

/**
 * ttree_init - initialize a red-black-tree-based process queue
 * @q: tree to initialize
 * @name: human-readable name for debugging (may be NULL)
 * @lock: spinlock that protects this tree (may be NULL)
 */
void ttree_init(ttree_t *q, const char *name, spinlock_t *lock);

/**
 * ttree_set_lock - (re)assign the protecting spinlock of a tree
 * @q: tree (ignored if NULL)
 * @lock: new spinlock pointer
 */
void ttree_set_lock(ttree_t *q, spinlock_t *lock);

/**
 * tnode_init - initialize a tnode for the current process
 * @node: node to initialize
 *
 * Zeroes the node, sets type to NONE, error_no to 0 and proc to myproc().
 * Must be called in process context.
 */
void tnode_init(tnode_t *node);

/* ======================== Accessors ======================== */

/**
 * tq_size - return the number of nodes in a list queue
 * @q: queue to query
 *
 * Returns the count (>= 0) or -EINVAL if @q is NULL.
 */
int tq_size(tq_t *q);

/**
 * ttree_size - return the number of nodes in a tree queue
 * @q: tree to query
 *
 * Returns the count (>= 0) or -EINVAL if @q is NULL.
 */
int ttree_size(ttree_t *q);

/**
 * tnode_get_queue - get the list queue a node belongs to
 * @node: node to query
 *
 * Returns the owning tq_t, or NULL if @node is NULL, not a
 * list-type node, or not currently enqueued.
 */
tq_t *tnode_get_queue(tnode_t *node);

/**
 * tnode_get_tree - get the tree queue a node belongs to
 * @node: node to query
 *
 * Returns the owning ttree_t, or NULL if @node is NULL, not a
 * tree-type node, or not currently enqueued.
 */
ttree_t *tnode_get_tree(tnode_t *node);

/**
 * tnode_get_proc - get the process associated with a node
 * @node: node to query
 *
 * Returns the struct proc pointer, or NULL if @node is NULL.
 */
struct proc *tnode_get_proc(tnode_t *node);

/**
 * tnode_get_errno - retrieve the error number stored in a node
 * @node: node to query
 * @error_no: output pointer for the stored errno
 *
 * Returns 0 on success, -EINVAL if either argument is NULL.
 */
int tnode_get_errno(tnode_t *node, int *error_no);

/* ======================== List Queue Operations ======================== */

/**
 * tq_push - append a node to the tail of a list queue
 * @q: target queue
 * @node: node to enqueue (must not already be enqueued)
 *
 * The node is converted to list type and its queue back-pointer is set.
 *
 * Returns 0 on success, -EINVAL if arguments are invalid or the node
 * is already enqueued.
 */
int tq_push(tq_t *q, tnode_t *node);

/**
 * tq_first - peek at the head of a list queue
 * @q: queue to inspect
 *
 * Returns the first tnode_t without removing it.
 * Returns NULL if the queue is empty, ERR_PTR(-EINVAL) if @q is NULL
 * or has a corrupt counter.
 */
tnode_t *tq_first(tq_t *q);

/**
 * tq_pop - remove and return the head of a list queue
 * @q: queue to pop from
 *
 * Returns the dequeued tnode_t (type reset to NONE).
 * Returns NULL if the queue is empty, or ERR_PTR on error.
 */
tnode_t *tq_pop(tq_t *q);

/**
 * tq_remove - remove a specific node from a list queue
 * @q: queue the node should belong to
 * @node: node to remove
 *
 * Panics if the queue counter is <= 0 (internal invariant violation).
 *
 * Returns 0 on success, -EINVAL if the node is not in @q.
 */
int tq_remove(tq_t *q, tnode_t *node);

/**
 * tq_bulk_move - transfer all nodes from one queue to another
 * @to: destination queue (must be empty)
 * @from: source queue (drained after the call)
 *
 * Moves the entire linked list in O(1) and updates each node's queue
 * back-pointer in O(n). @to and @from must be distinct.
 *
 * Returns 0 on success, -EINVAL on bad arguments, -ENOTEMPTY if @to
 * is not empty.
 */
int tq_bulk_move(tq_t *to, tq_t *from);

/* ======================== Tree Queue Operations ======================== */

/**
 * ttree_add - insert a node into a tree queue
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
int ttree_add(ttree_t *q, tnode_t *node);

/**
 * ttree_first - peek at the minimum-key node in a tree queue
 * @q: tree to inspect
 *
 * Returns the tnode_t with the smallest key, NULL if the tree is
 * empty, or ERR_PTR on error.
 */
tnode_t *ttree_first(ttree_t *q);

/**
 * ttree_key_min - get the minimum key value in a tree queue
 * @q: tree to query
 * @key: output pointer for the minimum key
 *
 * Returns 0 on success, -ENOENT if the tree is empty, or a negative
 * errno on other errors.
 */
int ttree_key_min(ttree_t *q, uint64 *key);

/**
 * ttree_remove - remove a specific node from a tree queue
 * @q: tree the node should belong to
 * @node: node to remove
 *
 * Validates that @node is in @q before removal.
 *
 * Returns 0 on success, -EINVAL if the node is not in @q, -ENOENT if
 * the rb-tree deletion fails.
 */
int ttree_remove(ttree_t *q, tnode_t *node);

/* ======================== Wait / Wakeup (List) ======================== */

/**
 * tq_wait_in_state - sleep on a list queue in a given state
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
int tq_wait_in_state(tq_t *q, struct spinlock *lock, 
                             uint64 *rdata, enum procstate state);

/**
 * tq_wait - sleep on a list queue (PSTATE_UNINTERRUPTIBLE)
 * @q: queue to wait on
 * @lock: spinlock to release before sleeping
 * @rdata: optional output for data passed by the waker (may be NULL)
 *
 * Convenience wrapper around tq_wait_in_state().
 */
int tq_wait(tq_t *q, struct spinlock *lock, uint64 *rdata);

/**
 * tq_wakeup - wake and dequeue the first waiter
 * @q: queue to wake from
 * @error_no: error code delivered to the waiter's tnode.error_no
 * @rdata: data delivered to the waiter's tnode.data
 *
 * Pops the head of @q, sets its error_no and data fields, then calls
 * scheduler_wakeup on the associated process.
 *
 * Returns:
 *   - pointer to the woken struct proc on success
 *   - NULL if the queue was empty
 *   - ERR_PTR(-EINVAL) if @q is NULL or internal error
 */
struct proc *tq_wakeup(tq_t *q, int error_no, uint64 rdata);

/**
 * tq_wakeup_all - wake every waiter in a list queue
 * @q: queue to drain
 * @error_no: error code delivered to each waiter
 * @rdata: data delivered to each waiter
 *
 * Repeatedly pops and wakes waiters until the queue is empty.
 *
 * Returns the number of processes woken (>= 0), -EINVAL if @q is NULL,
 * or a negative errno if an internal wakeup fails.
 */
int tq_wakeup_all(tq_t *q, int error_no, uint64 rdata);

/* ======================== Wait / Wakeup (Tree) ======================== */

/**
 * ttree_wait_in_state - sleep on a tree queue with a given key
 * @q: tree to wait on
 * @key: sort key for the waiter (determines wakeup priority)
 * @lock: spinlock to release before sleeping
 * @rdata: optional output for data passed by the waker (may be NULL)
 * @state: process sleeping state (must satisfy PSTATE_IS_SLEEPING)
 *
 * Inserts the current process into @q keyed by @key, sleeps, and on
 * return self-removes if still enqueued.
 *
 * Returns the waiter's error_no (see tq_wait_in_state).
 */
int ttree_wait_in_state(ttree_t *q, uint64 key, struct spinlock *lock, 
                            uint64 *rdata, enum procstate state);

/**
 * ttree_wait - sleep on a tree queue (PSTATE_UNINTERRUPTIBLE)
 * @q: tree to wait on
 * @key: sort key for the waiter
 * @lock: spinlock to release before sleeping
 * @rdata: optional output for data passed by the waker (may be NULL)
 *
 * Convenience wrapper around ttree_wait_in_state().
 */
int ttree_wait(ttree_t *q, uint64 key, struct spinlock *lock, uint64 *rdata);

/**
 * ttree_wakeup_one - wake the first waiter with a matching key
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
struct proc *ttree_wakeup_one(ttree_t *q, uint64 key, int error_no, uint64 rdata);

/**
 * ttree_wakeup_key - wake all waiters with a matching key
 * @q: tree to wake from
 * @key: key to match
 * @error_no: error code delivered to each waiter
 * @rdata: data delivered to each waiter
 *
 * Returns 0 if at least one waiter was woken, -ENOENT if none matched,
 * -EINVAL if @q is NULL.
 */
int ttree_wakeup_key(ttree_t *q, uint64 key, int error_no, uint64 rdata);

/**
 * ttree_wakeup_all - wake every waiter in a tree queue
 * @q: tree to drain
 * @error_no: error code delivered to each waiter
 * @rdata: data delivered to each waiter
 *
 * Iterates the tree in-order, removes and wakes each node.
 *
 * Returns 0 on success, -ENOENT if the tree was empty, -EINVAL if @q
 * is NULL.
 */
int ttree_wakeup_all(ttree_t *q, int error_no, uint64 rdata);

#endif // KERNEL_THREAD_QUEUE_H
