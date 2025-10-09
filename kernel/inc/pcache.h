#ifndef __KERNEL_PAGE_CACHE_H__
#define __KERNEL_PAGE_CACHE_H__

#include "types.h"
#include "list_type.h"
#include "bintree_type.h"
#include "spinlock.h"
#include "kobject.h"
#include "dev_types.h"


typedef struct page_struct page_t;
struct pcache;
struct pcache_ops;

struct pcache_ops {
    int (*read_page)(struct pcache *pcache, page_t *page);
    int (*write_page)(struct pcache *pcache, page_t *page);
    int (*write_begin)(struct pcache *pcache);
    int (*write_end)(struct pcache *pcache);
    void (*invalidate_page)(struct pcache *pcache, page_t *page);
    void (*set_dirty_page)(struct pcache *pcache, page_t *page);
    int (*sync)(struct pcache *pcache);
    void (*destroy)(struct pcache *pcache);
};

struct pcache {
    struct kobject kobj;
    list_node_t lru;
    struct spinlock lock;
    struct rb_root rb;
    size_t size;
    size_t max_size;
    struct pcache_ops *ops;
};

#endif /* __KERNEL_PAGE_CACHE_H__ */
