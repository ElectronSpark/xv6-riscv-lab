#ifndef __BI_DIRECTIONAL_H
#define __BI_DIRECTIONAL_H

#include "types.h"
#include "list_type.h"
#include "atomic.h"     /* For memory barriers: smp_wmb, smp_rmb, smp_mb, etc. */
#include "lock/rcu.h"        /* For RCU primitives: rcu_dereference, rcu_assign_pointer, etc. */

// initialize a new node entry, making it an empty head or detached node
static inline void list_entry_init(list_node_t *entry) {
    entry->next = entry;
    entry->prev = entry;
}

/* ============================================================================
 * RCU (Read-Copy-Update) List Operations
 * 
 * These operations allow lock-free read access to lists while writers
 * still need to synchronize among themselves. Based on Linux kernel's
 * rculist.h implementation.
 * 
 * Key concepts:
 * - Readers use rcu_read_lock()/rcu_read_unlock() (from rcu.h)
 * - Writers must use appropriate locking (spinlocks, etc.)
 * - Memory barriers ensure proper ordering on weakly-ordered architectures
 * ============================================================================
 */

/* <--- RCU List Entry Accessors ---> */

/**
 * list_next_rcu - Get the next entry with RCU-safe dereference
 * @entry: current list entry
 * 
 * Returns the next pointer safely for RCU readers.
 */
#define list_next_rcu(entry)    rcu_dereference((entry)->next)

/**
 * list_prev_rcu - Get the prev entry with RCU-safe dereference
 * @entry: current list entry
 * 
 * Note: RCU traversal of prev pointers requires list_bidir_del_rcu()
 * instead of list_entry_del_rcu() for deletions.
 */
#define list_prev_rcu(entry)    rcu_dereference((entry)->prev)


/* <--- RCU List Initialization ---> */

/**
 * list_entry_init_rcu - Initialize a list head visible to RCU readers
 * @entry: list entry to initialize
 * 
 * Use this when the list being initialized may be visible to RCU readers.
 * For normal initialization when readers have no access, use list_entry_init().
 */
static inline void list_entry_init_rcu(list_node_t *entry) {
    WRITE_ONCE(entry->next, entry);
    WRITE_ONCE(entry->prev, entry);
}


/* <--- RCU List Add Operations ---> */

/**
 * __list_entry_add_rcu - Insert entry between two known consecutive entries
 * @new: new entry to insert
 * @prev: entry before the insertion point
 * @next: entry after the insertion point
 * 
 * Internal function. Use list_entry_add_rcu() or list_entry_add_tail_rcu().
 */
static inline void __list_entry_add_rcu(list_node_t *new,
                                        list_node_t *prev,
                                        list_node_t *next)
{
    new->next = next;
    new->prev = prev;
    rcu_assign_pointer(prev->next, new);
    next->prev = new;
}

/**
 * list_entry_add_rcu - Add entry to head of list with RCU safety
 * @head: list head to add after
 * @entry: new entry to add
 * 
 * Insert a new entry after the specified head. Good for implementing stacks.
 * 
 * The caller must take appropriate locks to avoid racing with other writers.
 * It is safe to run concurrently with RCU readers.
 */
static inline void list_entry_add_rcu(list_node_t *head, list_node_t *entry) {
    __list_entry_add_rcu(entry, head, head->next);
}

/**
 * list_entry_add_tail_rcu - Add entry to tail of list with RCU safety
 * @head: list head to add before
 * @entry: new entry to add
 * 
 * Insert a new entry before the specified head. Good for implementing queues.
 * 
 * The caller must take appropriate locks to avoid racing with other writers.
 * It is safe to run concurrently with RCU readers.
 */
static inline void list_entry_add_tail_rcu(list_node_t *head, list_node_t *entry) {
    __list_entry_add_rcu(entry, head->prev, head);
}


/* <--- RCU List Delete Operations ---> */

/**
 * list_entry_del_rcu - Delete entry from list without re-initialization
 * @entry: the entry to delete
 * 
 * Note: LIST_ENTRY_IS_DETACHED() on entry does NOT return true after this.
 * The entry is in an undefined state suitable for RCU-based traversal.
 * 
 * In particular, we cannot poison the forward pointer that may still be
 * used for walking the list.
 * 
 * The caller must take appropriate locks to avoid racing with other writers.
 * It is safe to run concurrently with RCU readers.
 * 
 * The caller must defer freeing using synchronize_rcu() or call_rcu().
 */
static inline void list_entry_del_rcu(list_node_t *entry) {
    list_node_t *prev = entry->prev;
    list_node_t *next = entry->next;
    
    WRITE_ONCE(prev->next, next);
    next->prev = prev;
    /* Do NOT re-initialize entry->next - readers may still traverse it */
}

/**
 * list_entry_del_init_rcu - Delete entry and reinitialize it
 * @entry: the entry to delete
 * 
 * Same as list_entry_del_rcu() but also reinitializes the entry.
 * Use when you need LIST_ENTRY_IS_DETACHED() to return true after deletion.
 */
static inline void list_entry_del_init_rcu(list_node_t *entry) {
    list_entry_del_rcu(entry);
    list_entry_init_rcu(entry);
}

/**
 * list_entry_replace_rcu - Replace old entry with new entry atomically
 * @old: the entry being replaced
 * @new: the new entry to insert
 * 
 * The @old entry will be replaced with @new atomically from the perspective
 * of concurrent RCU readers.
 * 
 * The caller must take appropriate locks to avoid racing with other writers.
 * It is safe to run concurrently with RCU readers.
 */
static inline void list_entry_replace_rcu(list_node_t *old, list_node_t *new) {
    new->next = old->next;
    new->prev = old->prev;
    rcu_assign_pointer(new->prev->next, new);
    new->next->prev = new;
    /* Leave old->prev pointing to old for detection if needed */
}


/* <--- RCU List Node Operations (container-based) ---> */

/**
 * list_node_add_rcu - Add node to head of list with RCU safety
 * @head: list head
 * @node: node to add
 * @member: the list_node_t member within the node structure
 */
#define list_node_add_rcu(head, node, member)                   \
    list_entry_add_rcu(head, &((node)->member))

/**
 * list_node_add_tail_rcu - Add node to tail of list with RCU safety
 * @head: list head
 * @node: node to add  
 * @member: the list_node_t member within the node structure
 */
#define list_node_add_tail_rcu(head, node, member)              \
    list_entry_add_tail_rcu(head, &((node)->member))

/**
 * list_node_del_rcu - Delete node from list with RCU safety
 * @node: node to delete
 * @member: the list_node_t member within the node structure
 */
#define list_node_del_rcu(node, member)                         \
    list_entry_del_rcu(&((node)->member))


/* <--- RCU List Entry Accessors for Traversal ---> */

/**
 * list_entry_rcu - Get the container struct for this entry (RCU-safe)
 * @ptr: the list_node_t pointer
 * @type: the type of the container struct
 * @member: the name of the list_node_t within the struct
 * 
 * This primitive may safely run concurrently with _rcu list-mutation
 * primitives as long as it's guarded by rcu_read_lock().
 */
#define list_entry_rcu(ptr, type, member) \
    container_of(READ_ONCE(ptr), type, member)

/**
 * LIST_FIRST_ENTRY_RCU - Get the first element from a list (RCU-safe)
 * @head: the list head
 * 
 * Note: list must NOT be empty (use list_first_or_null_rcu for that).
 */
#define LIST_FIRST_ENTRY_RCU(head) list_next_rcu(head)

/**
 * LIST_FIRST_NODE_RCU - Get first node from list (RCU-safe)
 * @head: the list head
 * @type: the type of the container struct
 * @member: the name of the list_node_t within the struct
 * 
 * Returns NULL if the list is empty.
 */
#define LIST_FIRST_NODE_RCU(head, type, member) ({                      \
    list_node_t *__first_entry = list_next_rcu(head);                   \
    type *__result = NULL;                                              \
    if (!LIST_ENTRY_IS_HEAD(head, __first_entry)) {                     \
        __result = container_of(__first_entry, type, member);           \
    }                                                                   \
    __result;                                                           \
})

/**
 * list_next_or_null_rcu - Get next element or NULL if at end (RCU-safe)
 * @head: the list head
 * @ptr: the current list entry
 * @type: the type of the container struct
 * @member: the name of the list_node_t within the struct
 */
#define list_next_or_null_rcu(head, ptr, type, member) ({               \
    list_node_t *__head = (head);                                       \
    list_node_t *__ptr = (ptr);                                         \
    list_node_t *__next = READ_ONCE(__ptr->next);                       \
    __next != __head ? list_entry_rcu(__next, type, member) : NULL;     \
})


/* <--- RCU List Traversal Macros ---> */

/**
 * list_foreach_entry_rcu - Iterate over list entries (RCU-safe)
 * @head: the list head
 * @pos: the loop cursor (list_node_t *)
 * 
 * This primitive may safely run concurrently with _rcu list-mutation
 * primitives as long as it's guarded by rcu_read_lock().
 */
#define list_foreach_entry_rcu(head, pos)                               \
    for ((pos) = list_next_rcu(head);                                   \
         !LIST_ENTRY_IS_HEAD((head), (pos));                            \
         (pos) = list_next_rcu(pos))

/**
 * list_foreach_entry_continue_rcu - Continue iteration (RCU-safe)
 * @head: the list head
 * @pos: the current position (list_node_t *)
 */
#define list_foreach_entry_continue_rcu(head, pos)                      \
    for (;                                                              \
         !LIST_ENTRY_IS_HEAD((head), (pos));                            \
         (pos) = list_next_rcu(pos))

/**
 * list_foreach_node_rcu - Iterate over list nodes (RCU-safe)
 * @head: the list head
 * @pos: the loop cursor (pointer to container type)
 * @member: the name of the list_node_t within the container
 * 
 * This primitive may safely run concurrently with _rcu list-mutation
 * primitives as long as it's guarded by rcu_read_lock().
 */
#define list_foreach_node_rcu(head, pos, member)                        \
    for ((pos) = list_entry_rcu(list_next_rcu(head),                    \
                                typeof(*(pos)), member);                \
         &(pos)->member != (head);                                      \
         (pos) = list_entry_rcu(list_next_rcu(&(pos)->member),          \
                                typeof(*(pos)), member))

/**
 * list_foreach_node_continue_rcu - Continue node iteration (RCU-safe)
 * @head: the list head
 * @pos: the current position (pointer to container type)
 * @member: the name of the list_node_t within the container
 */
#define list_foreach_node_continue_rcu(head, pos, member)               \
    for ((pos) = list_entry_rcu(list_next_rcu(&(pos)->member),          \
                                typeof(*(pos)), member);                \
         &(pos)->member != (head);                                      \
         (pos) = list_entry_rcu(list_next_rcu(&(pos)->member),          \
                                typeof(*(pos)), member))

/**
 * list_foreach_node_from_rcu - Iterate from current position (RCU-safe)
 * @head: the list head
 * @pos: the starting position (pointer to container type)
 * @member: the name of the list_node_t within the container
 * 
 * Iterate over the tail of a list starting from the given position,
 * which must have been in the list when the RCU read lock was taken.
 */
#define list_foreach_node_from_rcu(head, pos, member)                   \
    for (;                                                              \
         &(pos)->member != (head);                                      \
         (pos) = list_entry_rcu(list_next_rcu(&(pos)->member),          \
                                typeof(*(pos)), member))

/* <--- macros manipulating entries ---> */
#define LIST_NEXT_ENTRY(entry) ((entry)->next)
#define LIST_PREV_ENTRY(entry) ((entry)->prev)
#define LIST_FIRST_ENTRY(head) LIST_NEXT_ENTRY(head)
#define LIST_LAST_ENTRY(head) LIST_PREV_ENTRY(head)
#define LIST_ENTRY_INITIALIZED(entry) { .prev = &(entry), .next = &(entry) }


/* <--- macros do test on entries ---> */
#define LIST_IS_EMPTY(head) (LIST_NEXT_ENTRY(head) == (head))
#define LIST_ENTRY_IS_HEAD(head, entry) ((head) == (entry))
#define LIST_ENTRY_IS_DETACHED(entry) (LIST_NEXT_ENTRY(entry) == (entry))
#define LIST_ENTRY_IS_FIRST(head, entry) (LIST_PREV_ENTRY(entry) == (head))
#define LIST_ENTRY_IS_LAST(head, entry) (LIST_NEXT_ENTRY(entry) == (head))


/* <--- macros manipulating nodes ---> */

// get the next node of a node
//
// Args:
//   - head (list_node_t *):
//       the head entry of a list
//   - node (node_type *):
//       the current node
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - the address of the next node
//   - NULL if the given node is the last node of a list
#define LIST_NEXT_NODE(head, node, member) ({                           \
    list_node_t *__current_entry = NULL;                                \
    list_node_t *__next_entry = NULL;                                   \
    typeof(*node) *__result = NULL;                                     \
    if ((node) != NULL) {                                               \
        __current_entry = &((node)->member);                            \
        __next_entry = LIST_NEXT_ENTRY(__current_entry);                \
        if ( !LIST_ENTRY_IS_HEAD(head, __next_entry) ) {                \
            __result =                                                  \
                container_of(__next_entry, typeof(*node), member);      \
        }                                                               \
    }                                                                   \
    __result;                                                           \
})

// get the previous node of a node
//
// Args:
//   - head (list_node_t *):
//       the head entry of a list
//   - node (node_type *):
//       the current node
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - the address of the previous node
//   - NULL if the given node is the first node of a list
#define LIST_PREV_NODE(head, node, member) ({                           \
    list_node_t *__current_entry = NULL;                                \
    list_node_t *__prev_entry = NULL;                                   \
    typeof(*node) *__result = NULL;                                     \
    if ((node) != NULL) {                                               \
        __current_entry = &((node)->member);                            \
        __prev_entry = LIST_PREV_ENTRY(__current_entry);                \
        if ( !LIST_ENTRY_IS_HEAD(head, __prev_entry) ) {                \
            __result =                                                  \
                container_of(__prev_entry, typeof(*node), member);      \
        }                                                               \
    }                                                                   \
    __result;                                                           \
})

// get the first node of a list
//
// Args:
//   - head (list_node_t *):
//       the head entry of a list
//   - type:
//       the type of the nodes in the list
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - the address of the first node in the given list
//   - NULL if the given list is empty
#define LIST_FIRST_NODE(head, type, member) ({                          \
    list_node_t *__first_entry = LIST_FIRST_ENTRY(head);                \
    type *__result = NULL;                                              \
    if ( !LIST_ENTRY_IS_HEAD(head, __first_entry) ) {                   \
        __result = container_of(__first_entry, type, member);           \
    }                                                                   \
    __result;                                                           \
})

// get the last node of a list
//
// Args:
//   - head (list_node_t *):
//       the head entry of a list
//   - type:
//       the type of the nodes in the list
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - the address of the last node in the given list
//   - NULL if the given list is empty
#define LIST_LAST_NODE(head, type, member) ({                           \
    list_node_t *__last_entry = LIST_LAST_ENTRY(head);                  \
    type *__result = NULL;                                              \
    if ( !LIST_ENTRY_IS_HEAD(head, __last_entry) ) {                    \
        __result = container_of(__last_entry, type, member);            \
    }                                                                   \
    __result;                                                           \
})

// to check if a node is detached
//
// Args:
//   - node (node_type *):
//       the node to check
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - true: the node is detached
//   - false: the node is in a list
#define LIST_NODE_IS_DETACHED(node, member)         \
    LIST_ENTRY_IS_DETACHED(&((node)->member))

// to check if a node is the first node of its list
//
// Args:
//   - head (list_node_t *):
//       the head entry of the list
//   - node (node_type *):
//       the node to check
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - true: the node is the first node of its list
//   - false: the node is not the first node of its list
#define LIST_NODE_IS_FIRST(head, node, member)      \
    LIST_ENTRY_IS_FIRST(head, &((node)->member))

// to check if a node is the last node of its list
//
// Args:
//   - head (list_node_t *):
//       the head entry of the list
//   - node (node_type *):
//       the node to check
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - true: the node is the last node of its list
//   - false: the node is not the last node of its list
#define LIST_NODE_IS_LAST(head, node, member)       \
    LIST_ENTRY_IS_LAST(head, &((node)->member))


/* <--- add and remove list entries ---> */

// take a node entry out of a list, and initialize it
static inline void list_entry_detach(list_node_t *entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    list_entry_init(entry);
}

// insert a node entry to the next position of the previous node entry
static inline void list_entry_insert(list_node_t *prev, list_node_t *entry) {
    list_node_t *next = LIST_NEXT_ENTRY(prev);
    entry->prev = prev;
    entry->next = next;
    prev->next = entry;
    next->prev = entry;
}

// replace a list entry with another entry, and initialize the old entry.
static inline void list_entry_replace(list_node_t *old, list_node_t *new) {
    if (old == NULL || new == NULL) {
        return;
    }
    list_entry_init(new);
    if (!LIST_ENTRY_IS_DETACHED(old)) {
        list_node_t *prev = LIST_PREV_ENTRY(old);
        list_entry_detach(old);
        list_entry_insert(prev, new);
    }
}

// push a node into the start of a list
static inline void list_entry_push_back(list_node_t *head, list_node_t *entry)
{
    list_entry_insert(head, entry);
}

// push a node into the end of a list
static inline void list_entry_push(list_node_t *head, list_node_t *entry)
{
    list_entry_insert(LIST_PREV_ENTRY(head), entry);
}

// Insert all entries in a source list into after a previous entry.
// The head of the source list will be initialized to an empty list.
static inline void list_entry_insert_bulk(list_node_t *prev,
                                          list_node_t *source_head)
{
    if (LIST_IS_EMPTY(source_head)) {
        return; // nothing to do
    }
    list_node_t *source_first = LIST_FIRST_ENTRY(source_head);
    list_node_t *source_last = LIST_LAST_ENTRY(source_head);
    source_first->prev = prev;
    source_last->next = prev->next;
    prev->next->prev = source_last;
    prev->next = source_first;
    list_entry_init(source_head);
}

// take the fisrt node out from a list
// return NULL if the list is empty
static inline list_node_t *list_entry_pop_back(list_node_t *head)
{
    list_node_t *first_entry = LIST_FIRST_ENTRY(head);
    if (LIST_ENTRY_IS_HEAD(head, first_entry)) {
        return NULL;
    }
    list_entry_detach(first_entry);
    return first_entry;
}

// take the last node out from a list
// return NULL if the list is empty
static inline list_node_t *list_entry_pop(list_node_t *head)
{
    list_node_t *last_entry = LIST_LAST_ENTRY(head);
    if (LIST_ENTRY_IS_HEAD(head, last_entry)) {
        return NULL;
    }
    list_entry_detach(last_entry);
    return last_entry;
}


/* <--- add and remove list nodes ---> */

// remove a node from its list
//
// Args:
//   - node (node_type *):
//       the node to detach
//   - member:
//       the member of the list entry in the structure of the node
#define list_node_detach(node, member)                          \
    list_entry_detach(&((node)->member))

// insert a node to the next position of another node
//
// Args:
//   - prev (node_type *):
//       a node in a list
//   - node (node_type *):
//       the node to be inserted
//   - member:
//       the member of the list entry in the structure of the node
#define list_node_insert(prev, node, member)                    \
    list_entry_insert(&((prev)->member), &((node)->member))

// add a node at the start of a list
//
// Args:
//   - head (list_node_t *):
//       the head entry of the list
//   - node (node_type *):
//       the node to insert
//   - member:
//       the member of the list entry in the structure of the node
#define list_node_push_back(head, node, member)                 \
    list_entry_push_back(head, &((node)->member))

// add a node at the end of a list
//
// Args:
//   - head (list_node_t *):
//       the head entry of the list
//   - node (node_type *):
//       the node to insert
//   - member:
//       the member of the list entry in the structure of the node
#define list_node_push(head, node, member)                      \
    list_entry_push(head, &((node)->member))

// get remove the first node of a list
//
// Args:
//   - head (list_node_t *):
//       the head entry of a list
//   - type:
//       the type of the nodes in the list
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - the address of the node popped
//   - NULL if the given list is empty
#define list_node_pop_back(head, type, member) ({               \
    list_node_t *__last_entry = list_entry_pop_back(head);      \
    type *__result = NULL;                                      \
    if (__last_entry != NULL) {                                 \
        __result = container_of(__last_entry, type, member);    \
    }                                                           \
    __result;                                                   \
})

// get remove the last node of a list
//
// Args:
//   - head (list_node_t *):
//       the head entry of a list
//   - type:
//       the type of the nodes in the list
//   - member:
//       the member of the list entry in the structure of the node
//
// Returns (node_type *):
//   - the address of the node popped
//   - NULL if the given list is empty
#define list_node_pop(head, type, member) ({                    \
    list_node_t *__last_entry = list_entry_pop(head);           \
    type *__result = NULL;                                      \
    if (__last_entry != NULL) {                                 \
        __result = container_of(__last_entry, type, member);    \
    }                                                           \
    __result;                                                   \
})

/* <- traverse lists -> */

// 
#define list_foreach_entry(head, pos)                                       \
    for (   (pos) = LIST_FIRST_ENTRY(head);                                 \
            !LIST_ENTRY_IS_HEAD((head), (pos));                             \
            (pos) = LIST_NEXT_ENTRY(pos))

#define list_foreach_entry_continue(head, pos)                              \
    for (   ;                                                               \
            !LIST_ENTRY_IS_HEAD((head), (pos));                             \
            (pos) = LIST_NEXT_ENTRY(pos))

#define list_for_each_entry_safe(head, pos, tmp)                            \
    for (   (pos) = LIST_FIRST_ENTRY(head), (tmp) = LIST_NEXT_ENTRY(pos);   \
            !LIST_ENTRY_IS_HEAD((head), (pos));                             \
            (pos) = (tmp), (tmp) = LIST_NEXT_ENTRY(tmp))

#define list_for_each_entry_continue_safe(head, pos, tmp)                   \
    for (   (tmp) = LIST_NEXT_ENTRY(pos);                                   \
            !LIST_ENTRY_IS_HEAD((head), (pos));                             \
            (pos) = (tmp), (tmp) = LIST_NEXT_ENTRY(tmp))

#define list_foreach_node_safe(head, pos, tmp, member)                      \
    for (   (pos) = LIST_FIRST_NODE(head, typeof(*pos), member),            \
                (tmp) = LIST_NEXT_NODE(head, pos, member);                  \
            pos != NULL;                                                    \
            (pos) = (tmp), (tmp) = LIST_NEXT_NODE(head, pos, member))

#define list_foreach_node_continue_safe(head, pos, tmp, member)             \
    for (   (tmp) = LIST_NEXT_NODE(head, pos, member);                      \
            pos != NULL;                                                    \
            (pos) = (tmp), (tmp) = LIST_NEXT_NODE(head, pos, member))

#define list_foreach_entry_inv(head, pos)                                   \
    for (   (pos) = LIST_LAST_ENTRY(head);                                  \
            !LIST_ENTRY_IS_HEAD((head), (pos));                             \
            (pos) = LIST_PREV_ENTRY(pos))

#define list_foreach_entry_inv_continue(head, pos)                          \
    for (   ;                                                               \
            !LIST_ENTRY_IS_HEAD((head), (pos));                             \
            (pos) = LIST_PREV_ENTRY(pos))

#define list_for_each_entry_inv_safe(head, pos, tmp)                        \
    for (   (pos) = LIST_LAST_ENTRY(head), (tmp) = LIST_PREV_ENTRY(pos);    \
            !LIST_ENTRY_IS_HEAD((head), (pos));                             \
            (pos) = (tmp), (tmp) = LIST_PREV_ENTRY(tmp))

#define list_for_each_entry_inv_continue_safe(head, pos, tmp)               \
    for (   (tmp) = LIST_PREV_ENTRY(pos);                                   \
            !LIST_ENTRY_IS_HEAD((head), (pos));                             \
            (pos) = (tmp), (tmp) = LIST_PREV_ENTRY(tmp))

#define list_foreach_node_inv_safe(head, pos, tmp, member)                  \
    for (   (pos) = LIST_LAST_NODE(head, typeof(*pos), member),             \
                (tmp) = LIST_PREV_NODE(head, pos, member);                  \
            pos != NULL;                                                    \
            (pos) = (tmp), (tmp) = LIST_PREV_NODE(head, pos, member))

#define list_foreach_node_inv_continue_safe(head, pos, tmp, member)         \
    for (   (tmp) = LIST_PREV_NODE(head, pos, member);                      \
            pos != NULL;                                                    \
            (pos) = (tmp), (tmp) = LIST_PREV_NODE(head, pos, member))

/* <- find node in a list  -> */

#define list_find_next(head, last, member, ret, __match_cond) ({                \
    typeof(*(last)) *__list_tmp_ptr = NULL;                                     \
    if ((last) == NULL) {                                                       \
        ret = LIST_FIRST_NODE(head, typeof(*(last)), member);                   \
    } else {                                                                    \
        ret = LIST_NEXT_NODE(head, (last), member);                             \
    }                                                                           \
    if ((ret) != NULL) {                                                        \
        list_foreach_node_continue_safe(head, ret, __list_tmp_ptr, member) {    \
            if (__match_cond)                                                   \
                break;                                                          \
        }                                                                       \
    }                                                                           \
    ret; })

#define list_find_first(head, type, member, ret, __match_cond)              \
    list_find_next(head, (type *)NULL, member, ret, __match_cond)

#define list_find_prev(head, last, member, ret, __match_cond) ({                \
    typeof(*(last)) *__list_tmp_ptr = NULL;                                     \
    if ((last) == NULL) {                                                       \
        ret = LIST_LAST_NODE(head, typeof(*(last)), member);                    \
    } else {                                                                    \
        ret = LIST_PREV_NODE(head, (last), member);                             \
    }                                                                           \
    if ((ret) != NULL) {                                                        \
        list_foreach_node_inv_continue_safe(head, ret, __list_tmp_ptr, member) {\
            if (__match_cond)                                                   \
                break;                                                          \
        }                                                                       \
    }                                                                           \
    ret; })

#define list_find_last(head, type, member, ret, __match_cond)               \
    list_find_prev(head, (type *)NULL, member, ret, __match_cond)


/* ============================================================================
 * RCU Find Operations (RCU-safe versions of list_find)
 * ============================================================================
 */

/**
 * list_find_next_rcu - Find next matching node (RCU-safe)
 * @head: the list head
 * @last: start search after this node (NULL to start from beginning)
 * @member: the name of the list_node_t within the container
 * @ret: variable to store the result
 * @__match_cond: condition expression that uses 'ret' to test for a match
 */
#define list_find_next_rcu(head, last, member, ret, __match_cond) ({           \
    if ((last) == NULL) {                                                      \
        ret = LIST_FIRST_NODE_RCU(head, typeof(*(ret)), member);               \
    } else {                                                                   \
        list_node_t *__next = list_next_rcu(&((last)->member));                \
        if (!LIST_ENTRY_IS_HEAD(head, __next)) {                               \
            ret = container_of(__next, typeof(*(ret)), member);                \
        } else {                                                               \
            ret = NULL;                                                        \
        }                                                                      \
    }                                                                          \
    if ((ret) != NULL) {                                                       \
        list_foreach_node_from_rcu(head, ret, member) {                        \
            if (__match_cond)                                                  \
                break;                                                         \
        }                                                                      \
        if (&(ret)->member == (head))                                          \
            ret = NULL;                                                        \
    }                                                                          \
    ret; })

/**
 * list_find_first_rcu - Find first matching node (RCU-safe)
 * @head: the list head
 * @type: the container type
 * @member: the name of the list_node_t within the container
 * @ret: variable to store the result
 * @__match_cond: condition expression that uses 'ret' to test for a match
 */
#define list_find_first_rcu(head, type, member, ret, __match_cond)             \
    list_find_next_rcu(head, (type *)NULL, member, ret, __match_cond)


/* ============================================================================
 * RCU List Splice Operations
 * ============================================================================
 */

/**
 * list_entry_splice_init_rcu - Splice an RCU-protected list into another list
 * @list: the list to splice (will be reinitialized)
 * @prev: entry to splice after
 * @next: entry to splice before
 * 
 * Note: The list pointed to by @prev and @next can be RCU-read traversed
 * concurrently with this function.
 * 
 * Important: The caller must ensure no other updates to @list occur.
 * After calling this function, @list will be empty.
 */
static inline void __list_entry_splice_rcu(list_node_t *list,
                                           list_node_t *prev,
                                           list_node_t *next)
{
    list_node_t *first = list->next;
    list_node_t *last = list->prev;
    
    if (first == list)
        return;  /* Empty list, nothing to do */
    
    /* Initialize list head first - readers will see empty list */
    list_entry_init_rcu(list);
    
    /* Memory barrier to ensure list is seen as empty before splicing */
    smp_wmb();
    
    /* Now splice the entries */
    first->prev = prev;
    last->next = next;
    rcu_assign_pointer(prev->next, first);
    next->prev = last;
}

/**
 * list_entry_splice_head_rcu - Splice list at head with RCU safety
 * @list: the list to splice (will be reinitialized)
 * @head: the destination list head
 */
static inline void list_entry_splice_head_rcu(list_node_t *list,
                                              list_node_t *head)
{
    if (!LIST_IS_EMPTY(list))
        __list_entry_splice_rcu(list, head, head->next);
}

/**
 * list_entry_splice_tail_rcu - Splice list at tail with RCU safety
 * @list: the list to splice (will be reinitialized)
 * @head: the destination list head
 */
static inline void list_entry_splice_tail_rcu(list_node_t *list,
                                              list_node_t *head)
{
    if (!LIST_IS_EMPTY(list))
        __list_entry_splice_rcu(list, head->prev, head);
}


/* ============================================================================
 * RCU Utility Macros
 * ============================================================================
 */

/**
 * LIST_IS_EMPTY_RCU - Check if list is empty (RCU-safe)
 * @head: the list head
 * 
 * Note: Due to the nature of RCU, this check may be stale immediately
 * after returning. Use list_first_or_null_rcu() when you need to both
 * check and access the first element atomically.
 */
#define LIST_IS_EMPTY_RCU(head) (list_next_rcu(head) == (head))

/**
 * LIST_NEXT_NODE_RCU - Get the next node (RCU-safe)
 * @head: the list head
 * @node: the current node
 * @member: the name of the list_node_t within the container
 * 
 * Returns the next node, or NULL if at the end of the list.
 */
#define LIST_NEXT_NODE_RCU(head, node, member) ({                              \
    list_node_t *__next_entry = NULL;                                          \
    typeof(*(node)) *__result = NULL;                                          \
    if ((node) != NULL) {                                                      \
        __next_entry = list_next_rcu(&((node)->member));                       \
        if (!LIST_ENTRY_IS_HEAD(head, __next_entry)) {                         \
            __result = container_of(__next_entry, typeof(*(node)), member);    \
        }                                                                      \
    }                                                                          \
    __result;                                                                  \
})

#endif          /* __BI_DIRECTIONAL_H */
