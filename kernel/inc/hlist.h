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
 * @param replace Whether to replace an existing node with the same key
 * @return 
 *   - NULL if the node was inserted successfully
 *   - The original node if insertion failed
 *   - The preexisting node
 */
void *hlist_put(hlist_t *hlist, void *node, bool replace);

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
 * @brief Get the next bucket in a hash list
 * @param hlist The hash list containing the buckets
 * @param bucket The current bucket
 * @return Pointer to the next bucket, or NULL if at the last bucket or invalid input
 * 
 * Iterates forward through the bucket array. Returns NULL when the end
 * of the bucket array is reached.
 */
static inline hlist_bucket_t *hlist_next_bucket(hlist_t *hlist, hlist_bucket_t *bucket) {
    if (hlist == NULL || bucket == NULL) {
        return NULL;
    }
    int offset = bucket - hlist->buckets;
    if (offset < 0 || offset >= hlist->bucket_cnt) {
        return NULL; // bucket is not part of this hlist
    }
    offset++;
    if (offset < hlist->bucket_cnt) {
        return hlist->buckets + offset;
    }
    return NULL;
}

/**
 * @brief Get the previous bucket in a hash list
 * @param hlist The hash list containing the buckets
 * @param bucket The current bucket
 * @return Pointer to the previous bucket, or NULL if at the first bucket or invalid input
 * 
 * Iterates backward through the bucket array. Returns NULL when the beginning
 * of the bucket array is reached.
 */
static inline hlist_bucket_t *hlist_prev_bucket(hlist_t *hlist, hlist_bucket_t *bucket) {
    if (hlist == NULL || bucket == NULL) {
        return NULL;
    }
    int offset = bucket - hlist->buckets;
    if (offset < 0 || offset >= hlist->bucket_cnt) {
        return NULL; // bucket is not part of this hlist
    }
    offset--;
    if (offset >= 0) {
        return hlist->buckets + offset;
    }
    return NULL;
}

/**
 * @brief Get the first entry in a bucket
 * @param bucket The bucket to get the first entry from
 * @return Pointer to the first entry, or NULL if the bucket is empty or NULL
 * 
 * Returns the first entry in the bucket's linked list.
 */
static inline hlist_entry_t *hlist_bucket_first_entry(hlist_bucket_t *bucket) {
    if (bucket == NULL) {
        return NULL;
    }
    return LIST_FIRST_NODE(bucket, hlist_entry_t, list_entry);
}

/**
 * @brief Get the last entry in a bucket
 * @param bucket The bucket to get the last entry from
 * @return Pointer to the last entry, or NULL if the bucket is empty or NULL
 * 
 * Returns the last entry in the bucket's linked list.
 */
static inline hlist_entry_t *hlist_bucket_last_entry(hlist_bucket_t *bucket) {
    if (bucket == NULL) {
        return NULL;
    }
    return LIST_LAST_NODE(bucket, hlist_entry_t, list_entry);
}

/**
 * @brief Get the next entry in a hash list
 * @param hlist The hash list to iterate
 * @param entry The current entry
 * @return Pointer to the next entry, or NULL if at the end of the hash list
 * 
 * Iterates forward through the hash list. If the current bucket is exhausted,
 * continues to the next non-empty bucket. Returns NULL when all entries
 * have been visited.
 */
static inline hlist_entry_t *hlist_next_entry(hlist_t *hlist, hlist_entry_t *entry) {
    if (hlist == NULL || entry == NULL || entry->bucket == NULL) {
        return NULL;
    }
    hlist_bucket_t *bucket = entry->bucket;
    hlist_entry_t *next = LIST_NEXT_NODE(bucket, entry, list_entry);
    while (next == NULL) {
        bucket = hlist_next_bucket(hlist, bucket);
        if (bucket == NULL) {
            return NULL;
        }
        next = LIST_FIRST_NODE(bucket, hlist_entry_t, list_entry);
    }
    return next;
}

/**
 * @brief Get the previous entry in a hash list
 * @param hlist The hash list to iterate
 * @param entry The current entry
 * @return Pointer to the previous entry, or NULL if at the beginning of the hash list
 * 
 * Iterates backward through the hash list. If the current bucket is exhausted,
 * continues to the previous non-empty bucket. Returns NULL when all entries
 * have been visited in reverse.
 */
static inline hlist_entry_t *hlist_prev_entry(hlist_t *hlist, hlist_entry_t *entry) {
    if (hlist == NULL || entry == NULL || entry->bucket == NULL) {
        return NULL;
    }
    hlist_bucket_t *bucket = entry->bucket;
    hlist_entry_t *prev = LIST_PREV_NODE(bucket, entry, list_entry);
    while (prev == NULL) {
        bucket = hlist_prev_bucket(hlist, bucket);
        if (bucket == NULL) {
            return NULL;
        }
        prev = LIST_LAST_NODE(bucket, hlist_entry_t, list_entry);
    }
    return prev;
}

/**
 * @brief Get the first entry in a hash list
 * @param hlist The hash list to search
 * @return Pointer to the first entry, or NULL if the hash list is empty
 * 
 * Finds the first non-empty bucket and returns its first entry.
 */
static inline hlist_entry_t *hlist_first_entry(hlist_t *hlist) {
    if (hlist == NULL) {
        return NULL;
    }
    for (int i = 0; i < hlist->bucket_cnt; i++) {
        hlist_entry_t *entry = LIST_FIRST_NODE(&hlist->buckets[i], hlist_entry_t, list_entry);
        if (entry != NULL) {
            return entry;
        }
    }
    return NULL;
}

/**
 * @brief Get the last entry in a hash list
 * @param hlist The hash list to search
 * @return Pointer to the last entry, or NULL if the hash list is empty
 * 
 * Finds the last non-empty bucket and returns its last entry.
 */
static inline hlist_entry_t *hlist_last_entry(hlist_t *hlist) {
    if (hlist == NULL) {
        return NULL;
    }
    for (int i = hlist->bucket_cnt - 1; i >= 0; i--) {
        hlist_entry_t *entry = LIST_LAST_NODE(&hlist->buckets[i], hlist_entry_t, list_entry);
        if (entry != NULL) {
            return entry;
        }
    }
    return NULL;
}

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

/**
 * @brief Get the first node in a hash list
 * @param hlist The hash list to search
 * @param type The type of the container struct
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the first node, or NULL if the hash list is empty
 */
#define HLIST_FIRST_NODE(hlist, type, member) ({                                \
    hlist_entry_t *__e = hlist_first_entry(hlist);                              \
    __e ? container_of(__e, type, member) : NULL;                               \
})

/**
 * @brief Get the last node in a hash list
 * @param hlist The hash list to search
 * @param type The type of the container struct
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the last node, or NULL if the hash list is empty
 */
#define HLIST_LAST_NODE(hlist, type, member) ({                                 \
    hlist_entry_t *__e = hlist_last_entry(hlist);                               \
    __e ? container_of(__e, type, member) : NULL;                               \
})

/**
 * @brief Get the next node in a hash list
 * @param hlist The hash list to iterate
 * @param node The current node (container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the next node, or NULL if at the end
 */
#define HLIST_NEXT_NODE(hlist, node, member) ({                                 \
    hlist_entry_t *__e = hlist_next_entry(hlist, &(node)->member);              \
    __e ? container_of(__e, typeof(*(node)), member) : NULL;                    \
})

/**
 * @brief Get the previous node in a hash list
 * @param hlist The hash list to iterate
 * @param node The current node (container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the previous node, or NULL if at the beginning
 */
#define HLIST_PREV_NODE(hlist, node, member) ({                                 \
    hlist_entry_t *__e = hlist_prev_entry(hlist, &(node)->member);              \
    __e ? container_of(__e, typeof(*(node)), member) : NULL;                    \
})

/**
 * @brief Get the first node in a bucket
 * @param bucket The bucket to get the first node from
 * @param type The type of the container struct
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the first node, or NULL if the bucket is empty
 */
#define HLIST_BUCKET_FIRST_NODE(bucket, type, member) ({                        \
    hlist_entry_t *__e = hlist_bucket_first_entry(bucket);                      \
    __e ? container_of(__e, type, member) : NULL;                               \
})

/**
 * @brief Get the last node in a bucket
 * @param bucket The bucket to get the last node from
 * @param type The type of the container struct
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the last node, or NULL if the bucket is empty
 */
#define HLIST_BUCKET_LAST_NODE(bucket, type, member) ({                         \
    hlist_entry_t *__e = hlist_bucket_last_entry(bucket);                       \
    __e ? container_of(__e, type, member) : NULL;                               \
})

/**
 * @brief Iterate over hash list nodes using container_of
 * @param hlist The hash list to iterate
 * @param pos Variable for the loop cursor (container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 *
 * Example:
 *   my_node_t *node;
 *   hlist_foreach_node(hlist, node, entry) {
 *       // use node
 *   }
 */
#define hlist_foreach_node(hlist, pos, member)                                  \
    for (hlist_entry_t *__entry = hlist_first_entry(hlist);                     \
         __entry && (pos = container_of(__entry, typeof(*(pos)), member), 1);  \
         __entry = hlist_next_entry(hlist, __entry))

/**
 * @brief Iterate over hash list nodes in reverse using container_of
 * @param hlist The hash list to iterate
 * @param pos Variable for the loop cursor (container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 *
 * Example:
 *   my_node_t *node;
 *   hlist_foreach_node_reverse(hlist, node, entry) {
 *       // use node in reverse order
 *   }
 */
#define hlist_foreach_node_reverse(hlist, pos, member)                          \
    for (hlist_entry_t *__entry = hlist_last_entry(hlist);                      \
         __entry && (pos = container_of(__entry, typeof(*(pos)), member), 1);  \
         __entry = hlist_prev_entry(hlist, __entry))

/**
 * @brief Safely iterate over hash list nodes, allowing removal during iteration
 * @param hlist The hash list to iterate
 * @param pos Variable for the loop cursor (container type pointer)
 * @param tmp Temporary variable for safe iteration (same container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 *
 * Example:
 *   my_node_t *node, *tmp;
 *   hlist_foreach_node_safe(hlist, node, tmp, entry) {
 *       hlist_pop(hlist, node);  // safe to remove
 *   }
 */
#define hlist_foreach_node_safe(hlist, pos, tmp, member)                        \
    for ((pos) = HLIST_FIRST_NODE(hlist, typeof(*(pos)), member),               \
         (tmp) = (pos) ? HLIST_NEXT_NODE(hlist, pos, member) : NULL;            \
         (pos) != NULL;                                                         \
         (pos) = (tmp),                                                         \
         (tmp) = (pos) ? HLIST_NEXT_NODE(hlist, pos, member) : NULL)

/**
 * @brief Safely iterate over hash list nodes in reverse, allowing removal
 * @param hlist The hash list to iterate
 * @param pos Variable for the loop cursor (container type pointer)
 * @param tmp Temporary variable for safe iteration (same container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 *
 * Example:
 *   my_node_t *node, *tmp;
 *   hlist_foreach_node_reverse_safe(hlist, node, tmp, entry) {
 *       hlist_pop(hlist, node);  // safe to remove
 *   }
 */
#define hlist_foreach_node_reverse_safe(hlist, pos, tmp, member)                \
    for ((pos) = HLIST_LAST_NODE(hlist, typeof(*(pos)), member),                \
         (tmp) = (pos) ? HLIST_PREV_NODE(hlist, pos, member) : NULL;            \
         (pos) != NULL;                                                         \
         (pos) = (tmp),                                                         \
         (tmp) = (pos) ? HLIST_PREV_NODE(hlist, pos, member) : NULL)


/* ============================================================================
 * RCU (Read-Copy-Update) Hash List Operations
 * 
 * These operations allow lock-free read access to hash lists while writers
 * still need to synchronize among themselves. Based on Linux kernel's
 * rculist.h implementation.
 * 
 * Key concepts:
 * - Readers use rcu_read_lock()/rcu_read_unlock() (from rcu.h)
 * - Writers must use appropriate locking (spinlocks, etc.)
 * - Memory barriers ensure proper ordering on weakly-ordered architectures
 * ============================================================================
 */

/* <--- RCU Hash List Entry Operations ---> */

/**
 * @brief Initialize a hash list entry visible to RCU readers
 * @param entry The hash list entry to initialize
 * 
 * Use this when the entry being initialized may be visible to RCU readers.
 */
static inline void hlist_entry_init_rcu(hlist_entry_t *entry) {
    if (entry) {
        WRITE_ONCE(entry->bucket, NULL);
        list_entry_init_rcu(&entry->list_entry);
    }
}

/**
 * @brief Add a hash list entry with RCU safety
 * @param hlist The hash list
 * @param bucket The bucket to add to
 * @param entry The entry to add
 * 
 * The caller must take appropriate locks to avoid racing with other writers.
 * It is safe to run concurrently with RCU readers.
 */
static inline void hlist_entry_add_rcu(hlist_t *hlist, hlist_bucket_t *bucket,
                                       hlist_entry_t *entry) {
    list_entry_add_rcu(bucket, &entry->list_entry);
    WRITE_ONCE(entry->bucket, bucket);
    hlist->elem_cnt += 1;
}

/**
 * @brief Delete a hash list entry with RCU safety
 * @param hlist The hash list
 * @param entry The entry to delete
 * 
 * Note: The entry is NOT reinitialized after deletion - readers may still
 * traverse it. The caller must defer freeing using synchronize_rcu() or call_rcu().
 * 
 * The caller must take appropriate locks to avoid racing with other writers.
 * It is safe to run concurrently with RCU readers.
 */
static inline void hlist_entry_del_rcu(hlist_t *hlist, hlist_entry_t *entry) {
    list_entry_del_rcu(&entry->list_entry);
    /* Do NOT clear entry->bucket - readers may still check it */
    hlist->elem_cnt -= 1;
}

/**
 * @brief Delete and reinitialize a hash list entry with RCU safety
 * @param hlist The hash list
 * @param entry The entry to delete
 * 
 * Same as hlist_entry_del_rcu() but also reinitializes the entry.
 */
static inline void hlist_entry_del_init_rcu(hlist_t *hlist, hlist_entry_t *entry) {
    list_entry_del_rcu(&entry->list_entry);
    hlist_entry_init_rcu(entry);
    hlist->elem_cnt -= 1;
}

/**
 * @brief Replace a hash list entry with RCU safety
 * @param hlist The hash list
 * @param old The entry to replace
 * @param new The new entry to insert
 * 
 * The caller must take appropriate locks to avoid racing with other writers.
 * It is safe to run concurrently with RCU readers.
 */
static inline void hlist_entry_replace_rcu(hlist_t *hlist,
                                           hlist_entry_t *old,
                                           hlist_entry_t *new) {
    list_entry_replace_rcu(&old->list_entry, &new->list_entry);
    WRITE_ONCE(new->bucket, old->bucket);
    /* Do NOT clear old->bucket - readers may still check it */
}


/* <--- RCU Hash List Entry Accessors ---> */

/**
 * @brief Get the first entry in a bucket (RCU-safe)
 * @param bucket The bucket to get the first entry from
 * @return Pointer to the first entry, or NULL if empty
 */
static inline hlist_entry_t *hlist_bucket_first_entry_rcu(hlist_bucket_t *bucket) {
    if (bucket == NULL) {
        return NULL;
    }
    return LIST_FIRST_NODE_RCU(bucket, hlist_entry_t, list_entry);
}

/**
 * @brief Get the next entry in a hash list (RCU-safe)
 * @param hlist The hash list to iterate
 * @param entry The current entry
 * @return Pointer to the next entry, or NULL if at the end
 */
static inline hlist_entry_t *hlist_next_entry_rcu(hlist_t *hlist, hlist_entry_t *entry) {
    if (hlist == NULL || entry == NULL) {
        return NULL;
    }
    hlist_bucket_t *bucket = READ_ONCE(entry->bucket);
    if (bucket == NULL) {
        return NULL;
    }
    hlist_entry_t *next = LIST_NEXT_NODE_RCU(bucket, entry, list_entry);
    while (next == NULL) {
        bucket = hlist_next_bucket(hlist, bucket);
        if (bucket == NULL) {
            return NULL;
        }
        next = LIST_FIRST_NODE_RCU(bucket, hlist_entry_t, list_entry);
    }
    return next;
}

/**
 * @brief Get the first entry in a hash list (RCU-safe)
 * @param hlist The hash list to search
 * @return Pointer to the first entry, or NULL if empty
 */
static inline hlist_entry_t *hlist_first_entry_rcu(hlist_t *hlist) {
    if (hlist == NULL) {
        return NULL;
    }
    for (int i = 0; i < hlist->bucket_cnt; i++) {
        hlist_entry_t *entry = LIST_FIRST_NODE_RCU(&hlist->buckets[i],
                                                    hlist_entry_t, list_entry);
        if (entry != NULL) {
            return entry;
        }
    }
    return NULL;
}


/* <--- RCU Hash List Node Macros ---> */

/**
 * @brief Get the first node in a hash list (RCU-safe)
 * @param hlist The hash list to search
 * @param type The type of the container struct
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the first node, or NULL if empty
 */
#define HLIST_FIRST_NODE_RCU(hlist, type, member) ({                            \
    hlist_entry_t *__e = hlist_first_entry_rcu(hlist);                          \
    __e ? container_of(__e, type, member) : NULL;                               \
})

/**
 * @brief Get the next node in a hash list (RCU-safe)
 * @param hlist The hash list to iterate
 * @param node The current node (container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the next node, or NULL if at the end
 */
#define HLIST_NEXT_NODE_RCU(hlist, node, member) ({                             \
    hlist_entry_t *__e = hlist_next_entry_rcu(hlist, &(node)->member);          \
    __e ? container_of(__e, typeof(*(node)), member) : NULL;                    \
})

/**
 * @brief Get the first node in a bucket (RCU-safe)
 * @param bucket The bucket to get the first node from
 * @param type The type of the container struct
 * @param member The name of the hlist_entry_t member within the container
 * @return Pointer to the first node, or NULL if empty
 */
#define HLIST_BUCKET_FIRST_NODE_RCU(bucket, type, member) ({                    \
    hlist_entry_t *__e = hlist_bucket_first_entry_rcu(bucket);                  \
    __e ? container_of(__e, type, member) : NULL;                               \
})


/* <--- RCU Hash List Traversal Macros ---> */

/**
 * @brief Iterate over hash list nodes (RCU-safe)
 * @param hlist The hash list to iterate
 * @param pos Variable for the loop cursor (container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 *
 * This primitive may safely run concurrently with _rcu list-mutation
 * primitives as long as it's guarded by rcu_read_lock().
 *
 * Example:
 *   rcu_read_lock();
 *   my_node_t *node;
 *   hlist_foreach_node_rcu(hlist, node, entry) {
 *       // use node
 *   }
 *   rcu_read_unlock();
 */
#define hlist_foreach_node_rcu(hlist, pos, member)                              \
    for (hlist_entry_t *__entry = hlist_first_entry_rcu(hlist);                 \
         __entry && (pos = container_of(__entry, typeof(*(pos)), member), 1);   \
         __entry = hlist_next_entry_rcu(hlist, __entry))

/**
 * @brief Iterate over entries in a bucket (RCU-safe)
 * @param bucket The bucket to iterate
 * @param pos Current position (hlist_entry_t *)
 *
 * Example:
 *   rcu_read_lock();
 *   hlist_entry_t *entry;
 *   hlist_foreach_bucket_entry_rcu(bucket, entry) {
 *       // use entry
 *   }
 *   rcu_read_unlock();
 */
#define hlist_foreach_bucket_entry_rcu(bucket, pos)                             \
    for ((pos) = hlist_bucket_first_entry_rcu(bucket);                          \
         (pos) != NULL;                                                         \
         (pos) = LIST_NEXT_NODE_RCU(bucket, pos, list_entry))

/**
 * @brief Iterate over nodes in a bucket (RCU-safe)
 * @param bucket The bucket to iterate
 * @param pos Variable for the loop cursor (container type pointer)
 * @param member The name of the hlist_entry_t member within the container
 *
 * Example:
 *   rcu_read_lock();
 *   my_node_t *node;
 *   hlist_foreach_bucket_node_rcu(bucket, node, entry) {
 *       // use node
 *   }
 *   rcu_read_unlock();
 */
#define hlist_foreach_bucket_node_rcu(bucket, pos, member)                      \
    for (hlist_entry_t *__entry = hlist_bucket_first_entry_rcu(bucket);         \
         __entry && (pos = container_of(__entry, typeof(*(pos)), member), 1);   \
         __entry = LIST_NEXT_NODE_RCU(bucket, __entry, list_entry))


/* <--- RCU Hash List Lookup Functions ---> */

/**
 * @brief Check if an entry is attached to a bucket (RCU-safe)
 * @param entry The hash list entry to check
 * @return true if the entry is attached to a bucket, false otherwise
 */
#define HLIST_ENTRY_ATTACHED_RCU(entry) (!!(READ_ONCE((entry)->bucket)))

/**
 * @brief Check if a hash list is empty (RCU-safe)
 * @param hlist The hash list to check
 * @return true if the hash list is NULL or has no elements, false otherwise
 * 
 * Note: Due to the nature of RCU, this check may be stale immediately
 * after returning.
 */
#define HLIST_EMPTY_RCU(hlist)  ({                                              \
    (hlist) == NULL || READ_ONCE((hlist)->elem_cnt) == 0;                       \
})


/* <--- RCU Hash List Functions ---> */

/**
 * @brief Get a node by its key with RCU safety
 * @param hlist The hash list to search
 * @param node A node containing the key to search for
 * @return The found node, or NULL if not found
 * 
 * This is an RCU read-side operation. Must be called within
 * rcu_read_lock()/rcu_read_unlock(). The returned pointer is only
 * valid within the RCU read-side critical section.
 */
void *hlist_get_rcu(hlist_t *hlist, void *node);

/**
 * @brief Put a node into a hash list with RCU safety
 * @param hlist The hash list to insert into
 * @param node The node to insert
 * @param replace Whether to replace an existing node with the same key
 * @return 
 *   - NULL if the node was inserted successfully
 *   - The original node if insertion failed
 *   - The preexisting node if found (and optionally replaced)
 * 
 * This is an RCU write-side operation. The caller must hold appropriate
 * locks to synchronize with other writers. After replacing a node, the
 * caller must defer freeing the old node using synchronize_rcu() or call_rcu().
 */
void *hlist_put_rcu(hlist_t *hlist, void *node, bool replace);

/**
 * @brief Pop a node from a hash list with RCU safety
 * @param hlist The hash list to remove from
 * @param node The node containing the key to remove, or NULL to remove any
 * @return The removed node, or NULL if no node was removed
 * 
 * This is an RCU write-side operation. The caller must hold appropriate
 * locks to synchronize with other writers. The caller must defer freeing
 * the returned node using synchronize_rcu() or call_rcu().
 */
void *hlist_pop_rcu(hlist_t *hlist, void *node);

#endif      /* __HASH_LIST_H__ */
