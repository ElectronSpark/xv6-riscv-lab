// Physical memory allocator, for user threads,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "lock/spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include <mm/page.h>
#include <mm/slab.h>
#include "bits.h"
#include <smp/percpu.h>

#ifdef __x86_64__
static inline void x86_kinit_dbg_outb(uint16 port, uint8 value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void x86_kinit_dbg_puts(const char *s) {
    while (*s) {
        x86_kinit_dbg_outb(0xE9, (uint8)*s++);
    }
}

#define X86_KINIT_MARK(msg) x86_kinit_dbg_puts(msg)
#else
#define X86_KINIT_MARK(msg) ((void)0)
#endif

STATIC slab_cache_t __kmm_slab_cache[SLAB_CACHE_NUMS][1];
STATIC char __kmm_slab_names[SLAB_CACHE_NUMS][32] = {0};
static slab_cache_t __slab_t_pool = {
    0}; // Special cache for slab descriptors themselves
static slab_cache_t __slab_cache_t_pool = {
    0}; // Special cache for slab_cache_t descriptors themselves

STATIC_INLINE void __init_kmm_slab_name(int idx, size_t bytes) {
    char num[32] = {'\0'};
    int num_idx = 29;
    while (bytes != 0) {
        num_idx--;
        int rem = bytes % 10;
        num[num_idx] = '0' + rem;
        bytes /= 10;
    }

    strcat(__kmm_slab_names[idx], &num[num_idx]);
}

void kinit() {
    size_t obj_size = SLAB_OBJ_MIN_SIZE;

    X86_KINIT_MARK("[xv6 x86_64] kinit: begin\n");
    page_buddy_init();
    X86_KINIT_MARK("[xv6 x86_64] kinit: after page_buddy_init\n");

    int ret = slab_cache_init(&__slab_t_pool, "slab_t_pool", sizeof(slab_t),
                              SLAB_FLAG_STATIC | SLAB_FLAG_EMBEDDED);
    X86_KINIT_MARK("[xv6 x86_64] kinit: after slab_t_pool\n");
    assert(ret == 0,
           "__slab_t_pool_init: failed to initialize slab_t pool, errno=%d",
           ret);
    ret = slab_cache_init(&__slab_cache_t_pool, "slab_cache_t_pool",
                          sizeof(slab_cache_t),
                          SLAB_FLAG_STATIC | SLAB_FLAG_EMBEDDED);
    X86_KINIT_MARK("[xv6 x86_64] kinit: after slab_cache_t_pool\n");
    assert(ret == 0,
           "__slab_cache_t_pool_init: failed to initialize slab_cache_t pool, "
           "errno=%d",
           ret);

    for (int i = 0; i < SLAB_CACHE_NUMS; i++) {
        X86_KINIT_MARK("[xv6 x86_64] kinit: slab loop iter\n");
        __init_kmm_slab_name(i, obj_size);
        if (slab_cache_init(__kmm_slab_cache[i], __kmm_slab_names[i], obj_size,
                            SLAB_FLAG_EMBEDDED | SLAB_FLAG_STATIC) != 0) {
            printf("failed to initialize kmm slab: %s\n", __kmm_slab_names[i]);
            panic("kinit");
        }
        X86_KINIT_MARK("[xv6 x86_64] kinit: slab loop iter done\n");
        obj_size *= 2;
    }

    X86_KINIT_MARK("[xv6 x86_64] kinit: done\n");
}

slab_t *slab_t_desc_alloc(void) {
    slab_t *slab_desc = slab_alloc(&__slab_t_pool);
    return slab_desc;
}

void slab_t_desc_free(slab_t *slab_desc) {
    if (slab_desc == NULL) {
        return;
    }
    slab_free(slab_desc);
}

slab_cache_t *slab_cache_t_alloc(void) {
    slab_cache_t *cache_desc = slab_alloc(&__slab_cache_t_pool);
    return cache_desc;
}

void slab_cache_t_free(slab_cache_t *cache_desc) {
    if (cache_desc == NULL) {
        return;
    }
    slab_free(cache_desc);
}

// allocate memory of size bytes from the pre-defined SLABs
// return the base address of the object if success
// return NULL if failed
void *kmm_alloc(size_t size) {
    int slab_idx = 0;
    if (size > SLAB_OBJ_MAX_SIZE) {
        return NULL;
    }
    if (size < SLAB_OBJ_MIN_SIZE) {
        size = SLAB_OBJ_MIN_SIZE;
    }
    size_t obj_shift = sizeof(size_t) * 8 - bits_clzg(size - 1);
    size_t obj_size = SLAB_OBJ_MIN_SIZE;

    while (slab_idx < SLAB_CACHE_NUMS) {
        if (obj_size >= size) {
            break;
        }
        obj_size <<= 1;
        slab_idx++;
    }
    assert(slab_idx >= 0 && slab_idx < SLAB_CACHE_NUMS,
           "kmm_alloc: invalid slab index");
    assert((1UL << obj_shift) >= size,
           "kmm_alloc: invalid object size calculation");
    if (obj_shift > SLAB_OBJ_MAX_SHIFT || slab_idx >= SLAB_CACHE_NUMS) {
        return NULL;
    }
    void *ret = slab_alloc(__kmm_slab_cache[slab_idx]);
    return ret;
}

// free the memory allocated from kmm_alloc
void kmm_free(void *ptr) { slab_free(ptr); }

// Shrink all kmm slab caches, releasing unused slabs back to buddy system.
// Called as emergency memory reclaim when slab allocation fails due to OOM.
// This allows the system to recover during stress tests (e.g., forkforkfork)
// where many threads exit and their slabs are freed but not yet shrunk.
void kmm_shrink_all(void) {
    for (int i = 0; i < SLAB_CACHE_NUMS; i++) {
        slab_cache_shrink(__kmm_slab_cache[i], 0x7fffffff);
    }
}

// Get total free pages from buddy system
uint64 get_total_free_pages(void) {
    uint64 total = 0;
    uint64 ret_arr[PAGE_BUDDY_MAX_ORDER + 1] = {0};
    bool empty_arr[PAGE_BUDDY_MAX_ORDER + 1] = {false};
    page_buddy_stat(ret_arr, empty_arr, PAGE_BUDDY_MAX_ORDER + 1);
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        total += (1UL << i) * ret_arr[i];
    }
    return total;
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// TODO: deprecated
void kfree(void *pa) {
    page_t *page = __pa_to_page((uint64)pa);
    if (__page_ref_dec(page) == -1) {
        panic("kfree");
    }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// TODO: deprecated
void *kalloc(void) {
    void *pa;
    page_t *page = __page_alloc(0, PAGE_TYPE_ANON);
    if (page == NULL) {
        return NULL;
    }

    pa = (void *)__page_to_pa(page);

    if (pa)
        memset((char *)pa, 5, PGSIZE); // fill with junk
    else
        panic("kalloc");
    return (void *)pa;
}
