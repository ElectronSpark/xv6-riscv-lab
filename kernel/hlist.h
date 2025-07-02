#ifndef __HASH_LIST_H__
#define __HASH_LIST_H__

/**
 * @file hlist.h
 * @brief Hash list implementation
 */

#include "compiler.h"
#include "hlist_type.h"
#include "list.h"

/**
 * @brief Prime constants used for hash calculations
 */
#define GOLDEN_RATIO_PRIME_32 ((ht_hash_t)0x9e370001UL)
#define GOLDEN_RATIO_PRIME_64 ((ht_hash_t)0x9e37fffffffc0001UL)

/**
 * @brief Default prime constant used for hashing
 */
#define GOLDEN_RATIO_PRIME GOLDEN_RATIO_PRIME_64

/**
 * @brief Generates a hash value from an integer
 * @param key The integer key to hash
 * @return A non-zero hash value
 */
static inline ht_hash_t hlist_hash_int(int key) {
    ht_hash_t ret = key * (0x100000001 - GOLDEN_RATIO_PRIME);
    if (ret == 0) {
        ret = GOLDEN_RATIO_PRIME; // Ensure non-zero hash
    }
    return ret;
}

/**
 * @brief Generates a hash value from a 64-bit unsigned integer
 * @param key The 64-bit unsigned integer to hash
 * @return A non-zero hash value
 */
static inline ht_hash_t hlist_hash_uint64(uint64 key) {
    ht_hash_t ret = key * GOLDEN_RATIO_PRIME;
    if (ret == 0) {
        ret = GOLDEN_RATIO_PRIME; // Ensure non-zero hash
    }
    return ret;
}

/**
 * @brief Generates a hash value from a string
 * @param str Pointer to the string to hash
 * @param len Length of the string in bytes
 * @return A non-zero hash value
 */
static inline ht_hash_t hlist_hash_str(char *str, size_t len) {
    ht_hash_t ret = GOLDEN_RATIO_PRIME * len;
    int tail_size = len % sizeof(ht_hash_t);
    ht_hash_t tail = 0;
    for (size_t i = 0; i < len - tail_size; i += sizeof(ht_hash_t)) {
        ht_hash_t *p = (ht_hash_t *)(str + i);
        ret ^= (*p) * GOLDEN_RATIO_PRIME;
    }
    if (tail_size > 0) {
        for (size_t i = len - tail_size; i < len; i++) {
            tail <<= 8;
            tail |= (unsigned char)str[i];
        }
        ret ^= tail * GOLDEN_RATIO_PRIME;
    }
    if (ret == 0) {
        ret = GOLDEN_RATIO_PRIME; // Ensure non-zero hash
    }
    return ret;
}

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
 * @param hlist The hash list to check
 * @param node The node to check for
 * @return true if the node is in the hash list, false otherwise
 */
bool hlist_node_in_list(hlist_t *hlist, void *node);

/**
 * @brief Initialize a hash list
 * @param hlist The hash list to initialize
 * @param bucket_cnt The number of buckets to create
 * @param func Pointer to hash list function structure
 * @return 0 on success, -1 on error
 */
int hlist_init(hlist_t *hlist, uint64 bucket_cnt, hlist_func_t *func);

/**
 * @brief Get the hash value for a node
 * @param hlist The hash list
 * @param node The node to calculate hash for
 * @return The hash value for the given node
 */
ht_hash_t hlist_get_node_hash(hlist_t *hlist, void *node);

/**
 * @brief Get a node from a hash list by key
 * @param hlist The hash list to search in
 * @param node A node containing the key to search for
 * @return The found node or NULL if not found
 */
void *hlist_get(hlist_t *hlist, void *node);

/**
 * @brief Insert a node into a hash list
 * @param hlist The hash list to insert into
 * @param node The node to insert
 * @return The replaced node if one existed, NULL otherwise
 */
void *hlist_put(hlist_t *hlist, void *node);

/**
 * @brief Remove a node from a hash list
 * @param hlist The hash list to remove from
 * @param node The node containing the key to remove, or NULL to remove any node
 * @return The removed node, or NULL if no node was removed
 */
void *hlist_pop(hlist_t *hlist, void *node);

/**
 * @brief Get the number of elements in a hash list
 * @param hlist The hash list to check
 * @return The number of elements in the hash list, or 0 if hlist is NULL
 */
size_t hlist_len(hlist_t *hlist);

/**
 * @brief Continue iterating through hash list buckets
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
 * @param hlist The hash list to iterate
 * @param idx Variable to store the current bucket index
 * @param bucket Pointer to the current bucket
 * @param pos Current position in the list
 * @param tmp Temporary variable for safe iteration
 */
#define hlist_foreach_entry(hlist, idx, bucket, pos, tmp)           \
    hlist_foreach_bucket(hlist, idx, bucket)                        \
    if (!LIST_IS_EMPTY(bucket))                                     \
    list_foreach_node_safe(bucket, pos, tmp, list_entry)

/**
 * @brief Continue iterating through entries from current position
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
