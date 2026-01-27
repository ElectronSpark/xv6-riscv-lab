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
 * @param head    Pointer to the list head structure
 * @param new_node Pointer to the node to push
 * @param member  Name of the next-pointer field within the node structure
 *
 * @note This is a LIFO (stack) operation - most recently pushed nodes are at
 * the front
 * @note The CAS loop ensures linearizability even under contention
 */
#define LLIST_PUSH(head, new_node, member)                                     \
    do {                                                                       \
        typeof((head)->member) old_head;                                       \
        do {                                                                   \
            old_head = smp_load_acquire(&(head)->member);                      \
            (new_node)->member = old_head;                                     \
        } while (!atomic_cas_ptr(&(head)->member, old_head, (new_node)));      \
    } while (0)

/**
 * @brief Check if a lockless list is empty
 *
 * @param head   Pointer to the list head structure
 * @param member Name of the next-pointer field within the node structure
 * @return Non-zero if empty, zero if not empty
 *
 * @note This is a point-in-time check; the list may change immediately after
 */
#define LLIST_IS_EMPTY(head, member) (smp_load_acquire(&(head)->member) == NULL)

/**
 * @brief Initialize a lockless list head to empty
 *
 * @param head   Pointer to the list head structure
 * @param member Name of the next-pointer field within the node structure
 */
#define LLIST_INIT(head, member)                                               \
    do {                                                                       \
        smp_store_release(&(head)->member, NULL);                              \
    } while (0)

/**
 * @brief Atomically migrate all nodes from source list to destination list
 *
 * This operation atomically detaches all nodes from the source list and
 * attaches them to the destination list. After this operation, the source
 * list will be empty.
 *
 * Algorithm:
 * 1. CAS loop to atomically replace src head with NULL (steal all nodes)
 * 2. Store the stolen chain to dest with release semantics
 *
 * @param dest   Pointer to the destination list head (must be thread-local)
 * @param src    Pointer to the source list head (may be shared)
 * @param member Name of the next-pointer field within the node structure
 *
 * @warning dest MUST be local to the caller (not accessed by other threads).
 *          This is a single-consumer pattern for the destination.
 *
 * @note Useful for batch processing: multiple producers push to a shared list,
 *       then a single consumer migrates all nodes to a local list for
 * processing
 */
#define LLIST_MIGRATE(dest, src, member)                                       \
    do {                                                                       \
        typeof((src)->member) old_head;                                        \
        do {                                                                   \
            old_head = smp_load_acquire(&(src)->member);                       \
        } while (!atomic_cas_ptr(&(src)->member, old_head, NULL));             \
        smp_store_release(&(dest)->member, old_head);                          \
    } while (0)

#endif /* __KERNEL_LOCKLESS_LIST_H */
