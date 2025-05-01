#ifndef __HASH_LIST_H__
#define __HASH_LIST_H__

#include "hlist_type.h"
#include "list.h"

// Get from the Linux kernel
#define GOLDEN_RATIO_PRIME_32 0x9e370001UL
#define GOLDEN_RATIO_PRIME_64 0x9e37fffffffc0001UL

// The maximun number of buckets in a hash list
#define HLIST_BUCKET_CNT_MAX 0xffffUL


#define HLIST_EMPTY(hlist)  ({  \
    (hlist) == NULL             \
    || (hlist)->elem_cnt == 0;  \
})


bool hlist_node_in_list(hlist_t *hlist, void *node);

int hlist_init(hlist_t *hlist, uint64 bucket_cnt, size_t id_length, hlist_func_t *func);

ht_hash_t hlist_get_node_hash(hlist_t *hlist, void *node);

void *hlist_get(hlist_t *hlist, void *node);

void *hlist_put(hlist_t *hlist, void *node);

void hlist_remove(hlist_t *hlist, void *node);

void *hlist_pop(hlist_t *hlist);

//
#define hlist_foreach_bucket_continue(hlist, idx, bucket)           \
    if (hlist)                                                      \
    for (   ;                                                       \
            idx < (hlist)->bucket_cnt;                              \
            idx++, bucket = &(hlist)->buckets[idx])

// 
#define hlist_foreach_bucket(hlist, idx, bucket)                    \
    if (hlist)                                                      \
    for (   idx = 0, bucket = &(hlist)->buckets[idx];               \
            idx < (hlist)->bucket_cnt;                              \
            idx++, bucket = &(hlist)->buckets[idx])

//
#define hlist_foreach_entry(hlist, idx, bucket, pos, tmp)           \
    hlist_foreach_bucket(hlist, idx, bucket)                        \
    list_foreach_node_safe(bucket, pos, tmp, list_entry)

//
#define hlist_foreach_entry_continue(hlist, idx, bucket, pos, tmp)  \
    hlist_foreach_bucket_continue(hlist, idx, bucket)               \
    list_foreach_node_continue_safe(bucket, pos, tmp, list_entry)

#endif      /* __HASH_LIST_H__ */
