#ifndef __BI_DIRECTIONAL_H
#define __BI_DIRECTIONAL_H

#include "types.h"
#include "list_type.h"

// initialize a new node entry, making it an empty head or detached node
static inline void list_entry_init(list_node_t *entry) {
    entry->next = entry;
    entry->prev = entry;
}

/* <--- macros manipulating entries ---> */
#define LIST_NEXT_ENTRY(entry) ((entry)->next)
#define LIST_PREV_ENTRY(entry) ((entry)->prev)
#define LIST_FIRST_ENTRY(head) LIST_NEXT_ENTRY(head)
#define LIST_LAST_ENTRY(head) LIST_PREV_ENTRY(head)
#define LIST_ENTRY_INITIALIZED(entry) { .prev = (entry), .next = (entry) }


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
    *new = *old;
    old->prev->next = new;
    old->next->prev = new;
    list_entry_init(old);
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

#endif          /* __BI_DIRECTIONAL_H */
