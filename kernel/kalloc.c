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

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

void
kinit()
{
  page_buddy_init(PGROUNDUP((uint64)end), PHYSTOP);
}

// increase the reference count of a page by 1
// return 0 if successful. return -1 if failed. 
int
page_refinc(void *physical)
{
  page_t *page = __pa_to_page((uint64)physical);
  if (__page_ref_inc(page) < 0) {
    return -1;
  }
  return 0;
}

// return the reference count of a page
// return -1 if failed
int
page_refcnt(void *physical)
{
  page_t *page = __pa_to_page((uint64)physical);
  return page_ref_count(page);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
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
