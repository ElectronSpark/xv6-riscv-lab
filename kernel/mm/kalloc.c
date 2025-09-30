// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "page.h"
#include "slab.h"

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

STATIC slab_cache_t __kmm_slab_cache[SLAB_CACHE_NUMS][1];
STATIC char __kmm_slab_names[SLAB_CACHE_NUMS][32] = { 0 };

STATIC_INLINE void __init_kmm_slab_name(int idx, size_t bytes) {
  char num[32] = { '\0' };
  int num_idx = 29;
  while (bytes != 0) {
    num_idx--;
    int rem = bytes % 10;
    num[num_idx] = '0' + rem;
    bytes /= 10;
  }

  strcat(__kmm_slab_names[idx], &num[num_idx]);
}

void
kinit()
{
  size_t obj_size = SLAB_OBJ_MIN_SIZE;
  page_buddy_init(PGROUNDUP((uint64)end), PHYSTOP);
  for (int i = 0; i < SLAB_CACHE_NUMS; i++) {
    __init_kmm_slab_name(i, obj_size);
    if (slab_cache_init(__kmm_slab_cache[i], __kmm_slab_names[i], obj_size,
        SLAB_FLAG_EMBEDDED | SLAB_FLAG_STATIC ) != 0) {
          printf("failed to initialize kmm slab: %s\n", __kmm_slab_names[i]);
          panic("kinit");
    }
    obj_size *= 2;
  }
}

// allocate memory of size bytes from the pre-defined SLABs
// return the base address of the object if success
// return NULL if failed
void *kmm_alloc(size_t size) {
    int slab_idx = 0;
    size_t obj_size = SLAB_OBJ_MIN_SIZE;

    while (slab_idx < SLAB_CACHE_NUMS) {
      if (obj_size >= size) {
        break;
      }
      obj_size <<= 1;
      slab_idx++;
    }
    if (obj_size > SLAB_OBJ_MAX_SIZE || slab_idx >= SLAB_CACHE_NUMS) {
      return NULL;
    }
    push_off();
    void *ret = slab_alloc(__kmm_slab_cache[slab_idx]);
    pop_off();
    return ret;
}

// free the memory allocated from kmm_alloc
void kmm_free(void *ptr) {
  push_off();
    slab_free(ptr);
    pop_off();
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// TODO: deprecated
void
kfree(void *pa)
{
  page_t *page = __pa_to_page((uint64)pa);

  if (__page_ref_dec(page) == -1) {
    panic("kfree");
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// TODO: deprecated
void *
kalloc(void)
{
  void *pa;
  page_t *page = __page_alloc(0, PAGE_FLAG_ANON);
  if (page == NULL) {
    return NULL;
  }

  pa = (void *)__page_to_pa(page);

  if(pa)
    memset((char*)pa, 5, PGSIZE); // fill with junk
  else
    panic("kalloc");
  return (void*)pa;
}
