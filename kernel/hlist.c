#include "hlist.h"

// call `hash` function
STATIC_INLINE ht_hash_t __hlist_hash(hlist_t *hlist, void *node) {
    return hlist->func.hash(node);
}

// call `get_node` function
STATIC_INLINE void *__hlist_get_node(hlist_t *hlist, hlist_entry_t *entry) {
    return hlist->func.get_node(entry);
}

// call `get_entry` function
STATIC_INLINE void *__hlist_get_entry(hlist_t *hlist, void *node) {
    return hlist->func.get_entry(node);
}

// call `cmp_node` function 
STATIC_INLINE int __hlist_cmp_node(hlist_t *hlist, void *node1, void *node2) {
    return hlist->func.cmp_node(hlist, node1, node2);
}

// to validate a hash list
STATIC_INLINE bool __hlist_validate(hlist_t *hlist) {
    if (hlist == NULL) {
        return false;
    }
    if (hlist->bucket_cnt == 0) {
        return false;
    }
    if (hlist->func.cmp_node == NULL || hlist->func.get_node == NULL
        || hlist->func.hash == NULL || hlist->func.get_entry == NULL) {
            return false;
        }
    return true;
}

// To check if a bucket belongs to a Hash List.
// The caller will guarantee the validity of the parameters 
STATIC_INLINE bool __hlist_is_bucket_of(hlist_t *hlist, hlist_bucket_t *bucket) {
    int offset = bucket - hlist->buckets;
    if (offset >= 0 || offset < hlist->bucket_cnt) {
        return true;
    }
    return false;
}

// To get the bucket where a node is in
STATIC_INLINE hlist_bucket_t *__hlist_get_node_bucket(hlist_t *hlist, void *node) {
    if (hlist == NULL || node == NULL) {
        return NULL;
    }
    hlist_entry_t *entry = __hlist_get_entry(hlist, node);
    if (entry == NULL) {
        return NULL;
    }
    return entry->bucket;
}

// To calculate the bucket a node with a giving hash value should be in
//
// The caller should ensure the validity of the parameters
STATIC_INLINE hlist_bucket_t *__hlist_calc_hash_bucket(hlist_t *hlist, ht_hash_t hash) {
    return hlist->buckets + (hash % hlist->bucket_cnt);
}

// To calculate the bucket the node should be in
// 
// The caller should ensure the validity of the parameters
STATIC_INLINE hlist_bucket_t *__hlist_calc_node_bucket(hlist_t *hlist, void *node) {
    ht_hash_t hash = __hlist_hash(hlist, node);
    return __hlist_calc_hash_bucket(hlist, hash);
}

// Initialize a bucket head
// The caller will guarantee the validity of the parameters
void __hlist_hash_bucket_init(hlist_t *hlist, hlist_bucket_t *bucket) {
    list_entry_init(bucket);
}

// replace node1 in hlist with node2
// The caller will guarantee the validity of the parameters
void __hlist_replace_node_entry(hlist_entry_t *old, hlist_entry_t *new) {
    list_entry_replace(&old->list_entry, &new->list_entry);
    new->bucket = old->bucket;
    old->bucket = NULL;
}

// Insert a hash node into a bucket and increase the item counter in `hlist`
// The caller will guarantee the validity of the parameters
void __hlist_insert_node_entry(hlist_t *hlist, hlist_bucket_t *bucket, hlist_entry_t *entry) {
    list_node_push_back(bucket, entry, list_entry);
    entry->bucket = bucket;
    hlist->elem_cnt += 1;
}

// remove a hash node from a bucket and decrease the item counter in `hlist`
// The caller will guarantee the validity of the parameters
void __hlist_remove_node_entry(hlist_t *hlist, hlist_entry_t *entry) {
    list_node_detach(entry, list_entry);
    entry->bucket = NULL;
    hlist->elem_cnt -= 1;
}

// Get the list entry of a node in bucket with the same id as a node
//
// This function is to find the node by its key.
// If the node with the same key is found, it will return the list entry of the node.
// Otherwise, it will return NULL.
//
// The caller should make sure the validity of the parameters.
STATIC_INLINE hlist_entry_t *__hlist_find_entry_in_bucket(hlist_t *hlist, hlist_bucket_t *bucket, void *node) {
    hlist_entry_t *pos = NULL;
    hlist_entry_t *tmp = NULL;
    list_foreach_node_safe(bucket, pos, tmp, list_entry) {
        void *node1 = __hlist_get_node(hlist, pos);
        if (__hlist_cmp_node(hlist, node1, node) == 0) {
            // node was found, return its entry
            return pos;
        }
    }

    // node not found, return NULL
    return NULL;
}

// To check if a node is inside of a hash list
bool hlist_node_in_list(hlist_t *hlist, void *node) {
    hlist_bucket_t *bucket = __hlist_get_node_bucket(hlist, node);
    if (bucket == NULL || hlist == NULL) {
        return false;
    }
    return __hlist_is_bucket_of(hlist, bucket);
}

// Get a hash list entry by the ID of its node
//
// Try to find a node with the same id as a dummy node, and return the entry and the hash
// bucket it's in through two pointers.
//
// This is a private function that should not be called by the user.
// The caller will ensure the validity of the parameters
void __hlist_get(hlist_t *hlist, void *node, hlist_bucket_t **ret_bucket, hlist_entry_t **ret_entry) {
    ht_hash_t hash_val = 0;
    hlist_bucket_t *bucket = NULL;
    hlist_entry_t *entry = NULL;

    hash_val = __hlist_hash(hlist, node);
    if (hash_val == 0) {
        goto ret;
    }
    
    bucket = __hlist_calc_hash_bucket(hlist, hash_val);
    if (bucket == NULL) {
        goto ret;
    }

    entry = __hlist_find_entry_in_bucket(hlist, bucket, node);

ret:
    if (ret_bucket != NULL) {
        *ret_bucket = bucket;
    }
    if (ret_entry != NULL) {
        *ret_entry = entry;
    }
}

// Initialize a hash list.
//
// return 0 if success
int hlist_init(hlist_t *hlist, uint64 bucket_cnt, hlist_func_t *func) {
    if (hlist == NULL || func == NULL) {
        return -1;
    }
    if (func->get_entry == NULL || func->get_node == NULL 
        || func->hash == NULL || func->cmp_node == NULL) {
            return -1;
        }
    if (bucket_cnt == 0 || bucket_cnt > HLIST_BUCKET_CNT_MAX) {
        return -1;
    }

    for (int i = 0; i < bucket_cnt; i++) {
        __hlist_hash_bucket_init(hlist, hlist->buckets + i);
    }

    hlist->bucket_cnt = bucket_cnt;
    hlist->func = *func;
    hlist->elem_cnt = 0;

    return 0;
}

// Get the hash value of a node
// Return 0 if failed to get the hash value
ht_hash_t hlist_get_node_hash(hlist_t *hlist, void *node) {
    if (hlist == NULL || node == NULL) {
        return 0;
    }
    if (hlist->func.hash == NULL) {
        return 0;
    }
    return __hlist_hash(hlist, node);
}

// Get a node by its ID
//
// A dummy node is needed to passing the id.
void *hlist_get(hlist_t *hlist, void *node) {
    hlist_entry_t *entry = NULL;

    if (node == NULL) {
        return NULL;
    }
    if (!__hlist_validate(hlist)) {
        return NULL;
    }

    __hlist_get(hlist, node, NULL, &entry);

    if (entry == NULL) {
        return NULL;
    }

    return __hlist_get_node(hlist, entry);
}

// Put a node into a hash list
//
// This function will insert a node into a hash list. It returns NULL if there's
// no node with the same id as the node passing to it. Otherwise, it will replace
// the existing node and return the pointer to it.
// It will return the node passing to it if something goes wrong.
void *hlist_put(hlist_t *hlist, void *node, bool replace) {
    hlist_bucket_t *bucket = NULL;
    hlist_entry_t *entry = NULL;
    void *old_node = NULL;
    hlist_entry_t *new_entry = NULL;

    if (!__hlist_validate(hlist)) {
        return node;
    }

    // To check the validity of the node given
    new_entry = __hlist_get_entry(hlist, node);
    if (new_entry == NULL) {
        return node;
    }

    // To check is the given node is detached
    if (HLIST_ENTRY_ATTACHED(new_entry)) {
        // Cannot insert a attached node into a hash list
        return node;
    }

    __hlist_get(hlist, node, &bucket, &entry);
    if (bucket == NULL) {
        return node;
    }

    if (entry == NULL) {
        // No entry with found, means the node with the same id doesn't exist.
        // Thus just insert it and return NULL.
        __hlist_insert_node_entry(hlist, bucket, new_entry);
        return NULL;
    } else {
        // the old entry is not valid!
        old_node = __hlist_get_node(hlist, entry);
        if (old_node == NULL) {
            return node;
        } else if (node == old_node) {
            // The node is already in the hash list, no need to replace it.
            return node;
        }
        // replace the existing node and return the old node
        if (replace) {
            __hlist_replace_node_entry(entry, new_entry);
        }
        return old_node;
    }
}

// Pop out a hash node from a hash list
//
// When `node` is not NULL, it will try to find the node with the same key in 
// `hlist` as `node`, then remove and return it.
// When `node` is NULL, it will remove and return a node from `hlist`.
// Return NULL if `hlist` is empty or no node with the same key as `node` is
// found in `hlist`.
void *hlist_pop(hlist_t *hlist, void *node) {
    if (!__hlist_validate(hlist)) {
        return NULL;
    }

    // If the hash list is empty, then it cannot pop out any node.
    if (hlist->elem_cnt == 0) {
        return NULL;
    }
    
    // If the given node is NULL, then pop out the first node in the list
    if (node == NULL) {
        // Get the first non-empty bucket
        uint64 idx = 0;
        hlist_bucket_t *bucket = NULL;
        hlist_foreach_bucket(hlist, idx, bucket) {
            if (!LIST_IS_EMPTY(bucket)) {
                // Get the first entry in the bucket
                list_node_t *first_entry = LIST_FIRST_ENTRY(bucket);
                // Convert list node to hlist entry
                hlist_entry_t *entry = container_of(first_entry, hlist_entry_t, list_entry);
                // Get the node from the entry
                void *ret_node = __hlist_get_node(hlist, entry);
                // Remove the entry from the bucket
                __hlist_remove_node_entry(hlist, entry);
                // Return the node
                return ret_node;
            }
        }
        // No node found in any bucket
        return NULL;
    }

    // If the given node is not NULL, try to find the node with the same key
    hlist_bucket_t *bucket = NULL;
    hlist_entry_t *entry = NULL;
    
    __hlist_get(hlist, node, &bucket, &entry);
    
    if (entry != NULL) {
        // found
        void *ret_node = __hlist_get_node(hlist, entry);
        if (ret_node != NULL) {
            __hlist_remove_node_entry(hlist, entry);
        }
        return ret_node;
    } else {
        // not found
        return NULL;
    }
}

size_t hlist_len(hlist_t *hlist) {
    if (!__hlist_validate(hlist)) {
        return 0;
    }
    return hlist->elem_cnt;
}


/* ============================================================================
 * RCU (Read-Copy-Update) Hash List Operations
 * ============================================================================
 *
 * Note: Basic entry operations (insert, remove, replace) are provided as
 * inline functions in hlist.h:
 *   - hlist_entry_add_rcu()
 *   - hlist_entry_del_rcu()
 *   - hlist_entry_replace_rcu()
 *
 * The functions below provide higher-level RCU-safe hash list operations.
 */

/**
 * @brief Find an entry in a bucket with RCU safety (for readers)
 * @param hlist The hash list
 * @param bucket The bucket to search
 * @param node The node containing the key to search for
 * @return The found entry, or NULL if not found
 * 
 * Must be called within rcu_read_lock()/rcu_read_unlock().
 */
STATIC_INLINE hlist_entry_t *__hlist_find_entry_in_bucket_rcu(hlist_t *hlist,
                                                              hlist_bucket_t *bucket,
                                                              void *node) {
    hlist_entry_t *pos = NULL;
    hlist_foreach_bucket_entry_rcu(bucket, pos) {
        void *node1 = __hlist_get_node(hlist, pos);
        if (__hlist_cmp_node(hlist, node1, node) == 0) {
            return pos;
        }
    }
    return NULL;
}

/**
 * @brief Get a hash list entry by node key with RCU safety (for readers)
 * @param hlist The hash list
 * @param node The node containing the key to search for
 * @param ret_bucket Output parameter for the bucket (may be NULL)
 * @param ret_entry Output parameter for the entry (may be NULL)
 * 
 * Must be called within rcu_read_lock()/rcu_read_unlock().
 */
void __hlist_get_rcu(hlist_t *hlist, void *node,
                     hlist_bucket_t **ret_bucket, hlist_entry_t **ret_entry) {
    ht_hash_t hash_val = 0;
    hlist_bucket_t *bucket = NULL;
    hlist_entry_t *entry = NULL;

    hash_val = __hlist_hash(hlist, node);
    if (hash_val == 0) {
        goto ret;
    }
    
    bucket = __hlist_calc_hash_bucket(hlist, hash_val);
    if (bucket == NULL) {
        goto ret;
    }

    entry = __hlist_find_entry_in_bucket_rcu(hlist, bucket, node);

ret:
    if (ret_bucket != NULL) {
        *ret_bucket = bucket;
    }
    if (ret_entry != NULL) {
        *ret_entry = entry;
    }
}

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
void *hlist_get_rcu(hlist_t *hlist, void *node) {
    hlist_entry_t *entry = NULL;

    if (node == NULL) {
        return NULL;
    }
    if (!__hlist_validate(hlist)) {
        return NULL;
    }

    __hlist_get_rcu(hlist, node, NULL, &entry);

    if (entry == NULL) {
        return NULL;
    }

    return __hlist_get_node(hlist, entry);
}

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
void *hlist_put_rcu(hlist_t *hlist, void *node, bool replace) {
    hlist_bucket_t *bucket = NULL;
    hlist_entry_t *entry = NULL;
    void *old_node = NULL;
    hlist_entry_t *new_entry = NULL;

    if (!__hlist_validate(hlist)) {
        return node;
    }

    new_entry = __hlist_get_entry(hlist, node);
    if (new_entry == NULL) {
        return node;
    }

    if (HLIST_ENTRY_ATTACHED(new_entry)) {
        return node;
    }

    /* Use non-RCU lookup since we're the writer with locks held */
    __hlist_get(hlist, node, &bucket, &entry);
    if (bucket == NULL) {
        return node;
    }

    if (entry == NULL) {
        /* Node doesn't exist, insert with RCU safety */
        hlist_entry_add_rcu(hlist, bucket, new_entry);
        return NULL;
    } else {
        old_node = __hlist_get_node(hlist, entry);
        if (old_node == NULL) {
            return node;
        } else if (node == old_node) {
            return node;
        }
        if (replace) {
            hlist_entry_replace_rcu(hlist, entry, new_entry);
        }
        return old_node;
    }
}

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
void *hlist_pop_rcu(hlist_t *hlist, void *node) {
    if (!__hlist_validate(hlist)) {
        return NULL;
    }

    if (hlist->elem_cnt == 0) {
        return NULL;
    }
    
    if (node == NULL) {
        /* Pop the first node */
        uint64 idx = 0;
        hlist_bucket_t *bucket = NULL;
        hlist_foreach_bucket(hlist, idx, bucket) {
            if (!LIST_IS_EMPTY(bucket)) {
                list_node_t *first_entry = LIST_FIRST_ENTRY(bucket);
                hlist_entry_t *entry = container_of(first_entry,
                                                    hlist_entry_t, list_entry);
                void *ret_node = __hlist_get_node(hlist, entry);
                hlist_entry_del_rcu(hlist, entry);
                return ret_node;
            }
        }
        return NULL;
    }

    /* Find and remove the specific node */
    hlist_bucket_t *bucket = NULL;
    hlist_entry_t *entry = NULL;
    
    __hlist_get(hlist, node, &bucket, &entry);
    
    if (entry != NULL) {
        void *ret_node = __hlist_get_node(hlist, entry);
        if (ret_node != NULL) {
            hlist_entry_del_rcu(hlist, entry);
        }
        return ret_node;
    } else {
        return NULL;
    }
}
