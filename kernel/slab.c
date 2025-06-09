// SLAB allocator manages kernel objects smaller than a single page
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "page.h"
#include "list.h"
#include "slab.h"
#include "slab_private.h"

// create a detached SLAB and initialize its objects
// return the SLAB created if success
// return NULL if failed
STATIC_INLINE slab_t *__slab_make(uint64 flags, uint32 order, size_t offs, 
                                  size_t obj_size, uint32 obj_num) {
    page_t *page;
    int page_nums;
    void *page_base, **prev, **tmp;
    slab_t *slab;
    
    page = __page_alloc(order, PAGE_FLAG_SLAB);
    if (page == NULL) {
        return NULL;
    }
    page_base = (void *)__page_to_pa(page);
    if (page_base == NULL) {
        panic("__slab_make");
    }
    if (flags & SLAB_FLAG_EMBEDDED) {
        // Embedded SLAB puts its descriptor at the start of its cache page
        slab = page_base;
    } else if ((slab = kmm_alloc(sizeof(slab_t))) == NULL){
        __page_free(page, order);
        return NULL;
    }
    page_nums = 1 << order;
    for (int i = 0; i < page_nums; i++) {
        page->slab.slab = slab;
    }

    slab->cache = NULL;
    slab->slab_order = order;
    slab->in_use = 0;
    slab->page = page;
    list_entry_init(&slab->list_entry);

    prev = NULL;
    tmp = page_base + offs;
    for (int i = 0; i < obj_num; i++) {
        *tmp = prev;
        prev = tmp;
        tmp += obj_size;
    }
    slab->next = tmp;
    return slab;
}

// Destroy an empty and detached SLAB
STATIC_INLINE void __slab_destroy(slab_t *slab) {
    page_t *page;
    uint16 order;
    uint64 page_base;
    if (slab == NULL) {
        return;
    }
    if (__SLAB_ATTACHED(slab)) {
        panic("__slab_destroy(): destroy an attached SLAB");
    }
    if (!__SLAB_EMPTY(slab)) {
        panic("__slab_destroy(): destroy a non-empty SLAB");
    }
    page = slab->page;
    order = slab->slab_order;
    page_base = __page_to_pa(page);
    if (page_base == 0) {
        panic("__slab_destroy");
    }
    if ((uint64)slab != page_base) {
        kmm_free(slab);
    }
    __page_free(page, order);
}

// Attach an empty SLAB to a SLAB cache
// SLAB must be enqueued after attaching
STATIC_INLINE void __slab_attach(slab_cache_t *cache, slab_t *slab) {
    if (!LIST_NODE_IS_DETACHED(slab, list_entry)) {
        panic("__slab_attach(): SLAB cannot be attached when in a queue");
    }
    if (slab->slab_order != cache->slab_order) {
        panic("__slab_attach(): wrong order");
    }
    if (__SLAB_ATTACHED(slab)) {
        panic("__slab_attach(): attach an attached SLAB");
    }
    if (!__SLAB_EMPTY(slab)) {
        panic("__slab_attach(): attach a non-empty SLAB");
    }
    slab->cache = cache;
    cache->slab_total++;
    cache->obj_total += cache->slab_obj_num;
}

// Detach an empty SLAB from its SLAB cache
// SLAB must be dequeued before detaching
STATIC_INLINE void __slab_detach(slab_cache_t *cache, slab_t *slab) {
    if (!LIST_NODE_IS_DETACHED(slab, list_entry)) {
        panic("__slab_detach(): SLAB cannot be detached when in a queue");
    }
    if (slab->cache != cache) {
        panic("__slab_detach(): wrong SLAB cache");
    }
    if (!__SLAB_EMPTY(slab)) {
        panic("__slab_detach(): detach non-empty SLAB");
    }
    if (cache->slab_total == 0 || cache->obj_total < cache->slab_obj_num) {
        panic("__slab_detach(): counter error");
    }
    cache->obj_total -= cache->slab_obj_num;
    cache->slab_total--;
    slab->cache = NULL;
}

// Take a SLAB out from the free/partial/full list it's in
// No validity check
STATIC_INLINE void __slab_dequeue(slab_cache_t *cache, slab_t *slab) {
    uint64 *cache_counter;
    list_node_t *list_entry;
    if (LIST_NODE_IS_DETACHED(slab, list_entry)) {
        panic("__slab_dequeue(): SLAB is not in a queue");
    }
    if (slab->cache != cache) {
        panic("__slab_dequeue(): wrong SLAB cache");
    }
    if (__SLAB_EMPTY(slab)) {
        cache_counter = &cache->slab_free;
        list_entry = &cache->free_list;
    } else if (__SLAB_FULL(slab)) {
        cache_counter = &cache->slab_full;
        list_entry = &cache->full_list;
    } else {
        cache_counter = &cache->slab_partial;
        list_entry = &cache->partial_list;
    }
    if (*cache_counter == 0) {
        panic("__slab_dequeue(): list counter error");
    }
    if (LIST_IS_EMPTY(list_entry)) {
        panic("__slab_dequeue(): list head error");
    }
    list_node_detach(slab, list_entry);
    *cache_counter -= 1;
}

// put a SLAB into the free/partial/full list accordingly
// No validity check
STATIC_INLINE void __slab_enqueue(slab_cache_t *cache, slab_t *slab) {
    list_node_t *list_entry;
    if (!LIST_NODE_IS_DETACHED(slab, list_entry)) {
        panic("__slab_enqueue(): SLAB is already in a queue");
    }
    if (slab->cache != cache) {
        panic("__slab_enqueue(): wrong SLAB cache");
    }
    if (__SLAB_EMPTY(slab)) {
        list_entry = &cache->free_list;
        cache->slab_free++;
    } else if (__SLAB_FULL(slab)) {
        list_entry = &cache->full_list;
        cache->slab_full++;
    } else {
        list_entry = &cache->partial_list;
        cache->slab_partial++;
    }
    list_node_push_back(list_entry, slab, list_entry);
}

// Take out the first SLAB from the free list of a SLAB cache, and dequeue it
STATIC_INLINE slab_t *__slab_pop_free(slab_cache_t *cache) {
    slab_t *slab;
    if (cache == NULL || cache->slab_free == 0) {
        return NULL;
    }
    cache->slab_free--;
    slab = list_node_pop_back(&cache->free_list, slab_t, list_entry);
    if (slab == NULL) {
        panic("__slab_pop_free(): failed to pop empty SLAB");
    }
    if (!__SLAB_EMPTY(slab)) {
        panic("__slab_pop_free(): get a non-empty SLAB when trying to get an empty one");
    }
    return slab;
}

// Take out the first SLAB from the partial list of a SLAB cache, and dequeue it
STATIC_INLINE slab_t *__slab_pop_partial(slab_cache_t *cache) {
    slab_t *slab;
    if (cache == NULL || cache->slab_partial == 0) {
        return NULL;
    }
    cache->slab_partial--;
    slab = list_node_pop_back(&cache->partial_list, slab_t, list_entry);
    if (slab == NULL) {
        panic("__slab_pop_free(): failed to pop half-full SLAB");
    }
    if (__SLAB_EMPTY(slab) || __SLAB_FULL(slab)) {
        panic("__slab_pop_free(): get an empty or full SLAB when trying to get an half-full one");
    }
    return slab;
}

// Take a SLAB object out of its SLAB and increase the in_use counter of the
// SLAB
// No validity check
// Will not change the counter in its SLAB cache.
// Return the ptr to the object if success
// Return NULL if failed
STATIC_INLINE void *__slab_obj_get(slab_t *slab) {
    void *ret_ptr;
    ret_ptr = slab->next;
    if (ret_ptr != NULL) {
        slab->next = *(void **)ret_ptr;
        slab->in_use++;
    }
    return ret_ptr;
}

// Put a SLAB object back to its SLAB and decrese the in_use counter of the
// SLAB
// No validity check
// Will not change the counter in its SLAB cache.
STATIC_INLINE void __slab_obj_put(slab_t *slab, void *ptr) {
    *(void**)ptr = slab->next;
    slab->next = ptr;
    slab->in_use--;
}

// Get the base address of an object giving its SLAB and its index
STATIC_INLINE void *__slab_idx2obj(slab_t *slab, int idx) {
    void *ret_ptr;
    void *page_base;
    if (!__SLAB_ATTACHED(slab)) {
        // We need the SLAB cache the SLAB attached to to determine some 
        // parameters.
        return NULL;
    }
    if (idx < 0 || idx >= slab->cache->slab_obj_num) {
        // object not in the range of the SLAB
        return NULL;
    }
    page_base = __SLAB_PAGE_BASE(slab);
    ret_ptr = page_base + slab->cache->offset;
    ret_ptr += idx * slab->cache->obj_size;
    return ret_ptr;
}

// Get the index of an object.
STATIC_INLINE int __slab_obj2idx(slab_t *slab, void *ptr) {
    size_t base_offs;
    void *page_base;
    int idx;
    if (ptr == NULL) {
        return -1;
    }
    if ((uint64)ptr & 7UL) {
        // all objects are aligned with 8 bytes
        return -1;
    }
    if (!(__SLAB_ATTACHED(slab))) {
        // We need the SLAB cache the SLAB attached to to determine some 
        // parameters.
        return -1;
    }
    page_base = __SLAB_PAGE_BASE(slab) - slab->cache->offset;
    if (ptr < page_base) {
        // object not in the range of the SLAB
        return -1;
    }
    base_offs = ptr - page_base;
    idx = base_offs / slab->cache->obj_size;
    if (idx >= slab->cache->slab_obj_num) {
        // object not in the range of the SLAB
        return -1;
    }
    return idx;
}

// find the SLAB of a object giving its address
STATIC_INLINE slab_t *__find_obj_slab(void *ptr) {
    uint64 page_base;
    page_t *page = NULL;

    if (ptr == NULL) {
        return NULL;
    }
    
    page_base = PGROUNDDOWN((uint64)ptr);
    page = __pa_to_page(page_base);
    if (page == NULL) {
        return NULL;
    }
    if (!(page->flags & PAGE_FLAG_SLAB)) {
        return NULL;
    }
    if (page->slab.slab == NULL) {
        return NULL;
    }
    return page->slab.slab;
}

// aqcuire the lock of a SLAB cache
// no checking here
STATIC_INLINE void __slab_cache_lock(slab_cache_t *cache) {
    acquire(&cache->lock);
}

// release the lock of a SLAB cache
// no checking here
STATIC_INLINE void __slab_cache_unlock(slab_cache_t *cache) {
    release(&cache->lock);
}

// Initialize a existing SLAB cache without checking
STATIC_INLINE void __slab_cache_init(slab_cache_t *cache, char *name, 
                                     size_t obj_size, uint64 flags) {
    size_t offset = 0;
    uint32 limits;
    uint16 slab_obj_num;

    if (flags & SLAB_FLAG_EMBEDDED) {
        offset = __SLAB_OBJ_OFFSET(obj_size);
    }
    slab_obj_num = (uint16)__SLAB_ORDER_OBJS(obj_size, offset, obj_size);
    limits = slab_obj_num * 4;
    // The size of each object must aligned to 8 bytes
    obj_size = ((obj_size + 7) >> 3) << 3;

    // memset(cache, 0, sizeof(slab_cache_t));
    cache->name = name;
    cache->flags = flags;
    cache->obj_size = obj_size;
    cache->offset = offset;
    cache->slab_order = SLAB_DEFAULT_ORDER;
    cache->slab_obj_num = slab_obj_num;
    cache->limits = limits;
    cache->slab_free = 0;
    cache->slab_partial = 0;
    cache->slab_full = 0;
    cache->slab_total = 0;
    cache->obj_active = 0;
    cache->obj_total = 0;

    list_entry_init(&cache->free_list);
    list_entry_init(&cache->partial_list);
    list_entry_init(&cache->full_list);
    initlock(&cache->lock, name);
}


// Initialize a existing SLAB cache
int slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, 
                    uint64 flags) {
    if (cache == NULL) {
        return -1;
    }
    if (flags & (~(SLAB_FLAG_STATIC | SLAB_FLAG_EMBEDDED))) {
        // invalid flags
        return -1;
    }
    if (obj_size > SLAB_OBJ_MAX_SIZE) {
        // the size of objects is too large
        return -1;
    }
    if (obj_size < SLAB_OBJ_MIN_SIZE) {
        // round up the size of the object to 
        obj_size = SLAB_OBJ_MIN_SIZE;
    }
    __slab_cache_init(cache, name, obj_size, flags);
    return 0;
}

// create and initialize a SLAB cache
slab_cache_t *slab_cache_create(char *name, size_t obj_size, uint64 flags) {
    slab_cache_t *slab_cache = kmm_alloc(sizeof(slab_cache_t));
    if (slab_cache == NULL) {
        return NULL;
    }
    if (slab_cache_init(slab_cache, name, obj_size, flags) != 0) {
        kmm_free(slab_cache);
        slab_cache = NULL;
    }
    return slab_cache;
}

// destroy a slab cache
// only non-STATIC , empty SLAB cache can be freed
// return 0 if success
// return -1 if failed
int slab_cache_destroy(slab_cache_t *cache) {
    int tmp;
    if (cache == NULL) {
        return -1;
    }
    // This lock will not release if SLAB cache is successfully destroyed.
    __slab_cache_lock(cache);
    if (cache->flags & SLAB_FLAG_STATIC ) {
        // cannot destroy a STATIC SLAB
        __slab_cache_unlock(cache);
        return -1;
    }
    if (cache->slab_partial != 0 || cache->slab_full != 0) {
        // will not allow to destroy a SLAB cache with allocated objects
        __slab_cache_unlock(cache);
        return -1;
    }
    tmp = cache->slab_free;
    if (__slab_cache_shrink_unlocked(cache, tmp) != tmp) {
        // failed to destroy all the SLABs
        __slab_cache_unlock(cache);
        return -1;
    }
    kmm_free(cache);
    // no releasing lock here, because the SLAB has been destroyed
    return 0;
}

// try to delete empty SLABs without locking the SLAB cache
// return the actual number of SLABs deleted
// return -1 if failed.
STATIC_INLINE int __slab_cache_shrink_unlocked(slab_cache_t *cache, int nums) {
    int slab_free_after, tmp, counter;
    slab_t *slab = NULL;
    if (cache == NULL) {
        return -1;
    }
    if (nums == 0 || nums >= cache->slab_free) {
        slab_free_after = 0;
    } else {
        slab_free_after = cache->slab_free - nums;
    }
    counter = 0;
    while (cache->slab_free > slab_free_after) {
        tmp = cache->slab_free;
        slab = __slab_pop_free(cache);
        if (slab == NULL) {
            panic("__slab_cache_shrink_unlocked: slab == NULL");
        }
        if (tmp == cache->slab_free) {
            panic("__slab_cache_shrink_unlocked: tmp == cache->slab_free");
        }
        tmp = cache->slab_total;
        __slab_detach(cache, slab);
        if (tmp == cache->slab_total) {
            panic("__slab_cache_shrink_unlocked: tmp == cache->slab_total");
        }
        __slab_destroy(slab);
        counter++;
    }
    return counter;
}

// try to delete empty SLABs
// return the actual number of SLABs deleted
// return -1 if failed.
int slab_cache_shrink(slab_cache_t *cache, int nums) {
    int ret;
    if (cache == NULL) {
        return -1;
    }
    __slab_cache_lock(cache);
    ret = __slab_cache_shrink_unlocked(cache, nums);
    __slab_cache_unlock(cache);
    return ret;
}

// allocate an object from a SLAB cache
// return the base address of the object if success
// return NULL if failed
void *slab_alloc(slab_cache_t *cache) {
    void *obj = NULL;
    slab_t *slab;
    if (cache == NULL) {
        return NULL;
    }
    __slab_cache_lock(cache);
    if (cache->slab_partial > 0) {
        // Try to get object from a half-full SLAB
        slab = __slab_pop_partial(cache);
        if (slab == NULL) {
            panic("slab_alloc(): Failed to get a half-full SLAB when the partial list is not empty");
        }
    } else if (cache->slab_free > 0) {
        // Try to get object from an empty SLAB
        slab = __slab_pop_free(cache);
        panic("slab_alloc(): Failed to get an empty SLAB when the free list is not empty");
    } else {
        // Try to make an empty SLAB
        slab = __slab_make( cache->flags, cache->slab_order, cache->offset, 
                            cache->obj_size, cache->slab_obj_num);
        if (slab == NULL) {
            // failed to create new SLAB, just return NULL
            obj = NULL;
            goto done;
        }
    }
    // Find an empty or half-full SLAB
    obj = __slab_obj_get(slab);
    if (obj != NULL) {
        cache->obj_active++;
    }
    __slab_enqueue(cache, slab);
done:
    __slab_cache_lock(cache);
    return obj;
}

// try to free an object
// the function will find the slab of the object from the page descriptor 
void slab_free(void *obj) {
    slab_t *slab;
    slab_cache_t *cache;
    int tmp;
    slab = __find_obj_slab(obj);
    if (obj == NULL) {
        return;
    }
    cache = slab->cache;
    if (cache == NULL) {
        panic("slab_free");
    }
    __slab_cache_lock(cache);
    __slab_dequeue(cache, slab);
    __slab_obj_put(slab, obj);
    cache->obj_active--;
    __slab_enqueue(cache, slab);
    tmp = cache->obj_total - cache->obj_active;
    if(tmp >= cache->limits) {
        tmp = tmp / (cache->slab_obj_num * 2);
        if (__slab_cache_shrink_unlocked(cache, tmp) < 0) {
            panic("slab_free(): shrink");
        }
    }
    __slab_cache_unlock(cache);
}
