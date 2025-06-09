#ifndef __KERNEL_SLAB_PRIVATE_H__
#define __KERNEL_SLAB_PRIVATE_H__

#include "compiler.h"
#include "slab_type.h"

// If the SLAB is attached to a SLAB CACHE is determined by its 'cache' pointer
#define __SLAB_ATTACHED(slab)       ((slab)->cache != NULL)
// The number of free objects in the slab. If detached then its 0
#define __SLAB_OBJ_FREE(slab) ({                                            \
    int __ret = 0;                                                          \
    if (__SLAB_ATTACHED(slab)                                               \
        && (slab)->in_use > (slab)->cache->slab_obj_num) {                  \
        __ret = (slab)->cache->slab_obj_num - (slab)->in_use;               \
    }                                                                       \
    __ret;                                                                  \
})
// If the SLAB is full then weather its detached or its number of free objects
// is 0.
#define __SLAB_FULL(slab)           (__SLAB_OBJ_FREE(slab) == 0)
// No object allocated
#define __SLAB_EMPTY(slab)          ((slab)->in_use == 0)

// Calculate the offset of the first object in bytes for embedded SLABs
#define __SLAB_OBJ_OFFSET(obj_size)                                         \
    (((sizeof(slab_t) + (obj_size) - 1) / (obj_size)) * obj_size)
// Calculate how many objects in each SLAB
#define __SLAB_ORDER_OBJS(order, offs, obj_size)                            \
    (((PAGE_SIZE << (order)) - (offs)) / obj_size)
// Get the base address of the pages where the SLAB stores its objects
#define __SLAB_PAGE_BASE(slab) ({                                           \
    void *__base_addr = NULL;                                               \
    if ((slab)->page != NULL) {                                             \
        __base_addr = (void *)__page_to_pa((slab)->page);                   \
    }                                                                       \
    __base_addr;                                                            \
})


STATIC_INLINE slab_t *__slab_make(uint64 flags, uint32 order, size_t offs, 
                                  size_t obj_size, uint32 obj_num);
STATIC_INLINE void __slab_destroy(slab_t *slab);
STATIC_INLINE void __slab_attach(slab_cache_t *cache, slab_t *slab);
STATIC_INLINE void __slab_detach(slab_cache_t *cache, slab_t *slab);
STATIC_INLINE void __slab_dequeue(slab_cache_t *cache, slab_t *slab);
STATIC_INLINE void __slab_enqueue(slab_cache_t *cache, slab_t *slab);
STATIC_INLINE slab_t *__slab_pop_free(slab_cache_t *cache);
STATIC_INLINE slab_t *__slab_pop_partial(slab_cache_t *cache);
STATIC_INLINE void *__slab_obj_get(slab_t *slab);
STATIC_INLINE void __slab_obj_put(slab_t *slab, void *ptr);
STATIC_INLINE void *__slab_idx2obj(slab_t *slab, int idx);
STATIC_INLINE int __slab_obj2idx(slab_t *slab, void *ptr);
STATIC_INLINE slab_t *__find_obj_slab(void *ptr);

STATIC_INLINE void __slab_cache_lock(slab_cache_t *cache);
STATIC_INLINE void __slab_cache_unlock(slab_cache_t *cache);
STATIC_INLINE void __slab_cache_init(slab_cache_t *cache, char *name, 
                                     size_t obj_size, uint64 flags);
STATIC_INLINE int __slab_cache_shrink_unlocked(slab_cache_t *cache, int nums);

#endif // __KERNEL_SLAB_PRIVATE_H__
