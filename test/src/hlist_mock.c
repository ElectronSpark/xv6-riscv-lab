#include "hlist_mock.h"
#include <stdlib.h>
#include <string.h>

// Allocate a hash list with the given bucket count
hlist_t *mock_hlist_create(uint64 bucket_cnt) {
    size_t size = sizeof(hlist_t) + bucket_cnt * sizeof(hlist_bucket_t);
    hlist_t *hlist = (hlist_t *)malloc(size);
    if (hlist) {
        memset(hlist, 0, size);
    }
    return hlist;
}

// Mock implementation of hlist_init
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
        list_entry_init(&hlist->buckets[i]);
    }

    hlist->bucket_cnt = bucket_cnt;
    hlist->func = *func;
    hlist->elem_cnt = 0;

    return 0;
}

// Helper functions
static inline ht_hash_t __hlist_hash(hlist_t *hlist, void *node) {
    return hlist->func.hash(node);
}

static inline void *__hlist_get_node(hlist_t *hlist, hlist_entry_t *entry) {
    return hlist->func.get_node(entry);
}

static inline void *__hlist_get_entry(hlist_t *hlist, void *node) {
    return hlist->func.get_entry(node);
}

static inline int __hlist_cmp_node(hlist_t *hlist, void *node1, void *node2) {
    return hlist->func.cmp_node(hlist, node1, node2);
}

static inline bool __hlist_validate(hlist_t *hlist) {
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

static inline hlist_bucket_t *__hlist_calc_hash_bucket(hlist_t *hlist, ht_hash_t hash) {
    return &hlist->buckets[hash % hlist->bucket_cnt];
}

static inline hlist_entry_t *__hlist_find_entry_in_bucket(hlist_t *hlist, hlist_bucket_t *bucket, void *node) {
    list_node_t *pos = bucket->next;
    while (pos != bucket) {
        hlist_entry_t *entry = container_of(pos, hlist_entry_t, list_entry);
        void *node1 = __hlist_get_node(hlist, entry);
        if (__hlist_cmp_node(hlist, node1, node) == 0) {
            // node was found, return its entry
            return entry;
        }
        pos = pos->next;
    }
    // node not found, return NULL
    return NULL;
}

static void __hlist_get(hlist_t *hlist, void *node, hlist_bucket_t **ret_bucket, hlist_entry_t **ret_entry) {
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

// Helper functions for inserting and removing
static void __hlist_insert_node_entry(hlist_t *hlist, hlist_bucket_t *bucket, hlist_entry_t *entry) {
    list_node_push_back(bucket, entry, offsetof(hlist_entry_t, list_entry));
    entry->bucket = bucket;
    hlist->elem_cnt += 1;
}

static void __hlist_remove_node_entry(hlist_t *hlist, hlist_entry_t *entry) {
    list_node_detach(entry, offsetof(hlist_entry_t, list_entry));
    entry->bucket = NULL;
    hlist->elem_cnt -= 1;
}

static void __hlist_replace_node_entry(hlist_entry_t *old, hlist_entry_t *new) {
    list_entry_replace(&old->list_entry, &new->list_entry);
    new->bucket = old->bucket;
    old->bucket = NULL;
}

// Implementation of hlist_get_node_hash
ht_hash_t hlist_get_node_hash(hlist_t *hlist, void *node) {
    if (hlist == NULL || node == NULL) {
        return 0;
    }
    if (hlist->func.hash == NULL) {
        return 0;
    }
    return __hlist_hash(hlist, node);
}

// Mock implementation of hlist_get
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

// Mock implementation of hlist_put
void *hlist_put(hlist_t *hlist, void *node) {
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
        }
        // replace the existing node and return the old node
        __hlist_replace_node_entry(entry, new_entry);
        return old_node;
    }
}

// Mock implementation of hlist_pop
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

// Mock implementation of hlist_node_in_list
bool hlist_node_in_list(hlist_t *hlist, void *node) {
    if (hlist == NULL || node == NULL) {
        return false;
    }
    
    hlist_entry_t *entry = __hlist_get_entry(hlist, node);
    if (entry == NULL) {
        return false;
    }
    
    return entry->bucket != NULL;
}
