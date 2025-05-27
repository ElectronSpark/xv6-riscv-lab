#ifndef __HASH_LIST_H__
#define __HASH_LIST_H__

/**
 * @file hlist.h
 * @brief Hash list implementation for xv6 kernel
 *
 * This file provides a hash list data structure implementation which supports
 * efficient key-based lookup, insertion, and removal operations.
 */

#include "hlist_type.h"
#include "list.h"

/**
 * @brief Prime constants used for hash calculations
 * These values are borrowed from the Linux kernel to provide good 
 * hash distribution properties.
 */
#define GOLDEN_RATIO_PRIME_32 ((ht_hash_t)0x9e370001UL)
#define GOLDEN_RATIO_PRIME_64 ((ht_hash_t)0x9e37fffffffc0001UL)

/**
 * @brief Maximum number of buckets allowed in a hash list
 */
#define HLIST_BUCKET_CNT_MAX 0xffffUL

/**
 * @brief Macro to check if a hash list is empty
 * @param hlist The hash list to check
 * @return true if the hash list is NULL or has no elements, false otherwise
 */
#define HLIST_EMPTY(hlist)  ({  \
    (hlist) == NULL             \
    || (hlist)->elem_cnt == 0;  \
})

/**
 * @brief Macro to check if a hash list entry is already in a bucket
 * @param entry The hash list entry to check
 * @return true if the entry is attached to a bucket, false otherwise
 */
#define HLIST_ENTRY_ATTACHED(entry) (!!((entry)->bucket))

/**
 * @brief Initialize a hash list entry
 * 
 * Sets the bucket pointer to NULL and initializes the list entry.
 * 
 * @param entry The hash list entry to initialize
 */
static inline void hlist_entry_init(hlist_entry_t *entry) {
    if (entry) {
        entry->bucket = NULL;
        list_entry_init(&entry->list_entry);
    }
}

/**
 * @brief Check if a node is in a hash list
 * 
 * @param hlist The hash list to check
 * @param node The node to check for
 * @return true if the node is in the hash list, false otherwise
 */
bool hlist_node_in_list(hlist_t *hlist, void *node);

/**
 * @brief Initialize a hash list
 * 
 * @param hlist The hash list to initialize
 * @param bucket_cnt The number of buckets to create
 * @param func Pointer to hash list function structure containing callback functions
 * @return 0 on success, -1 on error
 */
int hlist_init(hlist_t *hlist, uint64 bucket_cnt, hlist_func_t *func);

/**
 * @brief Get the hash value for a node
 * 
 * @param hlist The hash list
 * @param node The node to calculate hash for
 * @return The hash value for the given node
 */
ht_hash_t hlist_get_node_hash(hlist_t *hlist, void *node);

/**
 * @brief Get a node from a hash list by key
 * 
 * @param hlist The hash list to search in
 * @param node A node containing the key to search for
 * @return The found node or NULL if not found
 */
void *hlist_get(hlist_t *hlist, void *node);

/**
 * @brief Insert a node into a hash list
 * 
 * If a node with the same key already exists, it will be replaced.
 * 
 * @param hlist The hash list to insert into
 * @param node The node to insert
 * @return The replaced node if one existed, NULL otherwise
 */
void *hlist_put(hlist_t *hlist, void *node);

/**
 * @brief Remove a node from a hash list
 * 
 * If node is NULL, the first node found in any bucket will be removed.
 * If node is not NULL, the node with matching key will be removed.
 * 
 * @param hlist The hash list to remove from
 * @param node The node containing the key to remove, or NULL to remove any node
 * @return The removed node, or NULL if no node was removed
 */
void *hlist_pop(hlist_t *hlist, void *node);

/**
 * @brief Continue iterating through hash list buckets
 * 
 * This macro continues a bucket iteration from the current index.
 * 
 * @param hlist The hash list to iterate
 * @param idx The current bucket index
 * @param bucket Pointer to the current bucket
 */
#define hlist_foreach_bucket_continue(hlist, idx, bucket)           \
    if (hlist)                                                      \
    for (   ;                                                       \
            idx < (hlist)->bucket_cnt;                              \
            idx++, bucket = &(hlist)->buckets[idx])

/**
 * @brief Iterate through all buckets in a hash list
 * 
 * This macro provides iteration through all buckets from the beginning.
 * 
 * @param hlist The hash list to iterate
 * @param idx Variable to store the current bucket index
 * @param bucket Pointer to the current bucket
 */
#define hlist_foreach_bucket(hlist, idx, bucket)                    \
    if (hlist)                                                      \
    for (   idx = 0, bucket = &(hlist)->buckets[idx];               \
            idx < (hlist)->bucket_cnt;                              \
            idx++, bucket = &(hlist)->buckets[idx])

/**
 * @brief Iterate through all entries in a hash list
 * 
 * This macro combines bucket iteration with node iteration within each bucket.
 * 
 * @param hlist The hash list to iterate
 * @param idx Variable to store the current bucket index
 * @param bucket Pointer to the current bucket
 * @param pos Current position in the list
 * @param tmp Temporary variable for safe iteration
 */
#define hlist_foreach_entry(hlist, idx, bucket, pos, tmp)           \
    hlist_foreach_bucket(hlist, idx, bucket)                        \
    list_foreach_node_safe(bucket, pos, tmp, list_entry)

/**
 * @brief Continue iterating through entries from current position
 * 
 * This macro continues entry iteration from the current position.
 * 
 * @param hlist The hash list to iterate
 * @param idx Variable storing the current bucket index
 * @param bucket Pointer to the current bucket
 * @param pos Current position in the list
 * @param tmp Temporary variable for safe iteration
 */
#define hlist_foreach_entry_continue(hlist, idx, bucket, pos, tmp)  \
    hlist_foreach_bucket_continue(hlist, idx, bucket)               \
    list_foreach_node_continue_safe(bucket, pos, tmp, list_entry)

#endif      /* __HASH_LIST_H__ */
