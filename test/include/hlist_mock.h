#ifndef __HOST_TEST_HLIST_H__
#define __HOST_TEST_HLIST_H__

#include "hlist_type_mock.h"
#include <stddef.h> /* For offsetof */

// The maximum number of buckets in a hash list
#define HLIST_BUCKET_CNT_MAX 0xffffUL

// To check if a hash bucket is empty
#define HLIST_EMPTY(hlist)  ({  \
    (hlist) == NULL             \
    || (hlist)->elem_cnt == 0;  \
})

// To check is a hash list entry is in a bucket
#define HLIST_ENTRY_ATTACHED(entry) (!!((entry)->bucket))

// List macros for host testing
#define LIST_NEXT_ENTRY(entry) ((entry)->next)
#define LIST_PREV_ENTRY(entry) ((entry)->prev)
#define LIST_FIRST_ENTRY(head) LIST_NEXT_ENTRY(head)
#define LIST_LAST_ENTRY(head) LIST_PREV_ENTRY(head)
#define LIST_IS_EMPTY(head) (LIST_NEXT_ENTRY(head) == (head))
#define LIST_ENTRY_IS_HEAD(head, entry) ((head) == (entry))

// Mock implementations for list functions needed for testing
static inline void list_entry_init(list_node_t *entry) {
    if (entry) {
        entry->next = entry;
        entry->prev = entry;
    }
}

static inline void list_entry_replace(list_node_t *old, list_node_t *new) {
    if (old && new) {
        new->next = old->next;
        new->prev = old->prev;
        old->next->prev = new;
        old->prev->next = new;
        // Reset old entry
        old->next = old;
        old->prev = old;
    }
}

static inline void list_node_push_back(list_node_t *head, void *node, int offset) {
    if (head && node) {
        list_node_t *entry = (list_node_t *)((char *)node + offset);
        list_node_t *tail = head->prev;
        
        // Insert between tail and head
        entry->next = head;
        entry->prev = tail;
        tail->next = entry;
        head->prev = entry;
    }
}

static inline void list_node_detach(void *node, int offset) {
    if (node) {
        list_node_t *entry = (list_node_t *)((char *)node + offset);
        list_node_t *prev = entry->prev;
        list_node_t *next = entry->next;
        
        // Remove from list
        prev->next = next;
        next->prev = prev;
        
        // Reset entry
        entry->next = entry;
        entry->prev = entry;
    }
}

// Hash list iterator macro for host testing
#define hlist_foreach_bucket(hlist, idx, bucket)                    \
    if (hlist)                                                      \
    for (   idx = 0, bucket = &(hlist)->buckets[idx];               \
            idx < (hlist)->bucket_cnt;                              \
            idx++, bucket = &(hlist)->buckets[idx])

// Initialize hash list entry
static inline void hlist_entry_init(hlist_entry_t *entry) {
    if (entry) {
        entry->bucket = NULL;
        list_entry_init(&entry->list_entry);
    }
}

// Function declarations from hlist.c
bool hlist_node_in_list(hlist_t *hlist, void *node);
int hlist_init(hlist_t *hlist, uint64 bucket_cnt, hlist_func_t *func);
ht_hash_t hlist_get_node_hash(hlist_t *hlist, void *node);
void *hlist_get(hlist_t *hlist, void *node);
void *hlist_put(hlist_t *hlist, void *node);
void *hlist_pop(hlist_t *hlist, void *node);

// Mock helper function to create a hash list with dynamic memory allocation
hlist_t *mock_hlist_create(uint64 bucket_cnt);

#endif /* __HOST_TEST_HLIST_H__ */
