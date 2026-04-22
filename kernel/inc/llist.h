// Simple lockless single linked list implementation

#ifndef __KERNEL_LOCKLESS_LIST_H
#define __KERNEL_LOCKLESS_LIST_H
#include "types.h"
#include "compiler.h"
#include "smp/atomic.h"

/**
 * @brief Atomically push a node onto a lockless singly-linked list
 *
 * This macro implements a lock-free stack push using compare-and-swap (CAS).
 * Multiple threads can safely push nodes concurrently without locks.
 *
 * Algorithm:
 * 1. Load current head with acquire semantics (ensures visibility of prior
 * writes)
 * 2. Point new node's next to current head
 * 3. CAS to atomically replace head with new node
 * 4. Retry if another thread modified head between load and CAS
 *
 * @param head     The list head pointer (T*, must be an lvalue)
 * @param new_node Pointer to the node to push
 * @param member   Name of the next-pointer field within the node structure
 *
 * @note This is a LIFO (stack) operation - most recently pushed nodes are at
 * the front
 * @note The CAS loop ensures linearizability even under contention
 */
#define LLIST_PUSH(head, new_node, member)                                     \
    do {                                                                       \
        typeof(head) old_head;                                                 \
        do {                                                                   \
            old_head = smp_load_acquire(&(head));                              \
            (new_node)->member = old_head;                                     \
        } while (!atomic_cas_ptr(&(head), &old_head, (new_node)));             \
    } while (0)

/**
 * @brief Check if a lockless list is empty
 *
 * @param head The list head pointer (T*, must be an lvalue)
 * @return Non-zero if empty, zero if not empty
 *
 * @note This is a point-in-time check; the list may change immediately after
 */
#define LLIST_IS_EMPTY(head) (smp_load_acquire(&(head)) == NULL)

/**
 * @brief Initialize a lockless list head to empty
 *
 * @param head The list head pointer (T*, must be an lvalue)
 */
#define LLIST_INIT(head)                                                       \
    do {                                                                       \
        smp_store_release(&(head), NULL);                                      \
    } while (0)

/**
 * @brief Atomically migrate all nodes from source list to destination pointer
 *
 * This operation atomically detaches all nodes from the source list and
 * stores them to a destination pointer. After this operation, the source
 * list will be empty.
 *
 * Algorithm:
 * 1. CAS loop to atomically replace src head with NULL (steal all nodes)
 * 2. Store the stolen chain to dest with release semantics
 *
 * @param dest The destination head pointer (T*, must be an lvalue),
 *             should be thread-local
 * @param src  The source head pointer (T*, must be an lvalue)
 *
 * @warning dest SHOULD be a local variable (not accessed by other threads).
 * This is a single-consumer pattern for the destination.
 *
 * @note Useful for batch processing: multiple producers push to a shared list,
 *       then a single consumer migrates all nodes to a local pointer for
 * processing
 */
#define LLIST_MIGRATE(dest, src)                                               \
    do {                                                                       \
        typeof(src) old_head;                                                  \
        do {                                                                   \
            old_head = smp_load_acquire(&(src));                               \
        } while (!atomic_cas_ptr(&(src), &old_head, NULL));                    \
        smp_store_release(&(dest), old_head);                                  \
    } while (0)

/**
 * @brief Pop one node from a thread-local list (non-atomic)
 *
 * This macro pops a single node from the front of a list and stores it
 * in the destination pointer. After this operation, head advances to
 * the next node, and the popped node's next pointer is cleared.
 *
 * @param dest   The destination pointer (T*, must be an lvalue) to store
 *               the popped node, or NULL if list was empty
 * @param head   The list head pointer (T*, must be an lvalue)
 * @param member Name of the next-pointer field within the node structure
 *
 * @warning This is NOT thread-safe. The list must be thread-local or
 * protected by external synchronization (e.g., after LLIST_MIGRATE to
 * a local list, or while holding a lock).
 *
 * @note Typical usage pattern:
 *       1. LLIST_MIGRATE to steal entire list to local pointer
 *       2. Loop with LLIST_POP to process nodes one by one
 */
#define LLIST_POP(dest, head, member)                                          \
    do {                                                                       \
        (dest) = (head);                                                       \
        if ((dest) != NULL) {                                                  \
            (head) = (dest)->member;                                           \
            (dest)->member = NULL;                                             \
        }                                                                      \
    } while (0)

#endif /* __KERNEL_LOCKLESS_LIST_H */
