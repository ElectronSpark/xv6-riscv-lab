#ifndef __HOST_TEST_HLIST_TYPE_H__
#define __HOST_TEST_HLIST_TYPE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Define kernel types for host testing
typedef uint64_t uint64;
typedef uint64_t ht_hash_t;

// Forward declarations
struct hlist_struct;
typedef struct hlist_struct hlist_t;

// List node type from list_type.h for testing
typedef struct list_node {
    struct list_node *next;
    struct list_node *prev;
} list_node_t;

// Hash list node entry.
typedef struct hlist_entry {
    list_node_t list_entry;
    struct list_node *bucket;
} hlist_entry_t;

// Function types for hash list
typedef ht_hash_t (*hlist_hash_func_t)(void *);
typedef int (*hlist_cmp_func_t)(hlist_t *, void *, void *);
typedef void *(*hlist_get_node_func_t)(void *);
typedef void *(*hlist_get_entry_func_t)(void *);

// Hash list functions
typedef struct hlist_func_struct {
    hlist_hash_func_t hash;
    hlist_cmp_func_t cmp_node;
    hlist_get_node_func_t get_node;
    hlist_get_entry_func_t get_entry;
} hlist_func_t;

// Hash list bucket type
typedef struct list_node hlist_bucket_t;

// Hash list structure
struct hlist_struct {
    uint64 bucket_cnt;
    uint64 elem_cnt;
    hlist_func_t func;
    hlist_bucket_t buckets[0];
};

// The container_of macro for host testing
#define container_of(ptr, type, member) ({          \
    void *__mptr = (void *)(ptr);                   \
    ((type *)(__mptr - offsetof(type, member))); })

#endif /* __HOST_TEST_HLIST_TYPE_H__ */
