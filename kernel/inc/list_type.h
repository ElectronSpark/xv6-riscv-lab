/* definition of list structure */
#ifndef __BI_DIRECTIONAL_LIST_TYPE
#define __BI_DIRECTIONAL_LIST_TYPE

// definition of a bi-directional list node entry
// a list entry can be the head or a node of a list.
//
// when it's the head of a list:
//   - prev:
//       points to the last node entry of the list
//       points to itself when in an empty list
//   - next:
//       points to the first node entry of the list
//       points to itself when in an empty list
//
// when it's the node of a list:
//   - prev:
//       points to the next node entry of the list, or to the head entry of
//       the list if it's the last node.
//       points to itself when not in a list
//   - next:
//       points to the previous node entry of the list, or to the head entry of
//       the list if it's the first node.
//       points to itself when not in a list
typedef struct list_node {
    struct list_node *prev;
    struct list_node *next;
} list_node_t;

#endif