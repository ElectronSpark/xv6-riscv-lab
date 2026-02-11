#ifndef __HASH_LIST_TYPE_H__
#define __HASH_LIST_TYPE_H__

#include "types.h"
#include "list_type.h"

// to store hash value
typedef uint64 ht_hash_t;

// list head of each hash bucket
typedef struct list_node hlist_bucket_t;

typedef struct hlist_struct hlist_t;

// Hash list node entry.
//
// A node will include this structure to link to a hash list.
// It contains a linked list entry to link to a hash bucket, and a pointer to
// the bucket head it belongs to. If `bucket` is NULL, then this node is not
// inside of any hash list.
typedef struct hlist_entry {
    list_node_t list_entry;
    hlist_bucket_t *bucket;
} hlist_entry_t;

// Abstraction of hash function
//
// Get the hash value of a node as ht_hash_t.
//
// Args:
//   - (void *):
//       Pointer to a hash node.
//       The pointer is guaranteed not being NULL
// Returns (ht_hash_t):
//   - None Zero: The hash value of the node
//   - Zero: invalid hash value
typedef ht_hash_t (*hlist_hash_func_t)(void *);

// Compare a node with a key
//
// Args:
//   - (hlist_t *):
//       The hash list the node is in
//   - (void *):
//       Pointer to a hash node
//   - (void *):
//       Pointer to a dummy node with the same id as the target node
// Returns:
//   - node > key: a number > 0
//   - node == key: 0
//   - node < key: a number < 0
typedef int (*hlist_node_id_cmp_func_t)(hlist_t *, void *, void *);

// Abstraction function to get the node of a hash list entry
//
// Get the address of a node giving its hash list entry.
//
typedef void *(*hlist_get_node_func_t)(hlist_entry_t *);

// Abstraction function to get the Hash list entry of a node
//
// Get the address of a hash list entry giving the node it's in.
// This function needs to check if a node is invalid and return NULL if that's
// the case.
typedef hlist_entry_t *(*hlist_get_entry_func_t)(void *);

// abstract methods of a hash list
typedef struct hlist_func_struct {
    hlist_hash_func_t hash;
    hlist_get_node_func_t get_node;
    hlist_get_entry_func_t get_entry;
    hlist_node_id_cmp_func_t cmp_node;
} hlist_func_t;

// Hash List structure.
typedef struct hlist_struct {
    uint64 bucket_cnt;
    int64 elem_cnt;
    hlist_func_t func;
    hlist_bucket_t buckets[0];
} hlist_t;

#endif /* __HASH_LIST_TYPE_H__ */
