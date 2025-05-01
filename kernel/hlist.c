#include "hlist.h"

// call `hash` function
static inline ht_hash_t __hlist_hash(hlist_t *hlist, void *node) {
    return hlist->func.hash(node);
}

// call `get_node` function
static inline void *__hlist_get_node(hlist_t *hlist, hlist_entry_t *entry) {
    return hlist->func.get_node(entry);
}

// call `get_entry` function
static inline void *__hlist_get_entry(hlist_t *hlist, void *node) {
    return hlist->func.get_entry(node);
}

// call `cmp_node` function 
static inline int __hlist_cmp_node(hlist_t *hlist, void *node1, void *node2) {
    return hlist->func.cmp_node(hlist, node1, node2);
}

// To check if a bucket belongs to a Hash List.
// The caller will guarantee the validity of the parameters 
static inline bool __hlist_is_bucket_of(hlist_t *hlist, hlist_bucket_t *bucket) {
    int offset = bucket - hlist->buckets;
    if (offset >= 0 || offset < hlist->bucket_cnt) {
        return true;
    }
    return false;
}

// To get the bucket where a node is in
static inline hlist_bucket_t *__hlist_get_node_bucket(hlist_t *hlist, void *node) {
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
static inline hlist_bucket_t *__hlist_calc_hash_bucket(hlist_t *hlist, ht_hash_t hash) {
    return hlist->buckets + (hash % hlist->bucket_cnt);
}

// To calculate the bucket the node should be in
// 
// The caller should ensure the validity of the parameters
static inline hlist_bucket_t *__hlist_calc_node_bucket(hlist_t *hlist, void *node) {
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
// @TODO:
void __hlist_replace_node_entry(hlist_t *hlist, void *node1, void *node2) {

}

// @TODO:
void *__hlist_insert_bucket_entry(hlist_bucket_t *bucket, hlist_entry_t *entry) {

}

// @TODO:
void *__hlist_remove_bucket_entry(hlist_bucket_t *bucket, hlist_entry_t *entry) {

}

// To check if a node is inside of a hash list
bool hlist_node_in_list(hlist_t *hlist, void *node) {
    hlist_bucket_t *bucket = __hlist_get_node_bucket(hlist, node);
    if (bucket == NULL || hlist == NULL) {
        return false;
    }
    return __hlist_is_bucket_of(hlist, bucket);
}

// Initialize a hash list.
//
// return 0 if success
int hlist_init(hlist_t *hlist, uint64 bucket_cnt, hlist_func_t *func) {
    if (hlist == NULL || func == NULL) {
        return -1;
    }
    if (func->get_entry == NULL || func->get_node == NULL 
        || func->hash == NULL || func->cmp_node) {
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

// Get the list entry of a node in bucket with the same id as a node
//
// This function is to find the node by its key.
// If the node with the same key is found, it will return the list entry of the node.
// Otherwise, it will return NULL.
//
// The caller should make sure the validity of the parameters.
static inline hlist_entry_t *__hlist_find_entry_in_bucket(hlist_t *hlist, hlist_bucket_t *bucket, void *node) {
    hlist_entry_t *pos = NULL;
    hlist_entry_t *tmp = NULL;
    list_foreach_node_continue_safe(bucket, pos, tmp, list_entry) {
        void *node1 = __hlist_get_node(hlist, pos);
        if (__hlist_cmp_node(hlist, node1, node) == 0) {
            // node was found, return its entry
            return pos;
        }
    }

    // node not found, return NULL
    return NULL;
}

// Get a node by its ID
//
// A dummy node is needed to passing the id.
void *hlist_get(hlist_t *hlist, void *node) {
    ht_hash_t hash_val = 0;
    uint64 bucket_idx = 0;
    void *node = NULL;
    hlist_entry_t *entry = NULL;
    if (hlist == NULL || node == NULL) {
        return NULL;
    }
    if (hlist->elem_cnt == 0) {
        return NULL;
    }
    if (hlist->bucket_cnt == 0) {
        return NULL;
    }
    if (hlist->func.cmp_node == NULL || hlist->func.get_node == NULL
        || hlist->func.hash == NULL) {
            return NULL;
        }

    hash_val = __hlist_hash(hlist, node);
    if (hash_val == 0) {
        return NULL;
    }
    
    bucket_idx = hash_val % hlist->bucket_cnt;
    entry = __hlist_find_entry_in_bucket(hlist, &hlist->buckets[bucket_idx], node);

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
void *hlist_put(hlist_t *hlist, void *node) {
    ht_hash_t hash_val = 0;

    if (hlist == NULL || node == NULL) {
        return node;
    }
    if (hlist->func.hash == NULL) {
        return node;
    }

    hash_val = __hlist_hash(hlist, node);
    if (hash_val == 0) {
        return NULL;
    }
}
