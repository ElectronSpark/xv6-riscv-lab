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

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
static uint16 __page_refcnt[TOTALPAGES] = { 0 };

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
  page_buddy_init(PGROUNDUP((uint64)end), PHYSTOP);
}

// return the pointer pointing to the reference count
// of a page.
// return 0 if failed.
// this function is unlocked
static inline uint16 *__refcnt_addr(void *physical)
{
  if ((uint64)physical < KERNBASE || (uint64)physical >= PHYSTOP) {
    return 0;
  }
  return &__page_refcnt[((uint64)physical - KERNBASE) >> 12];
}

// increase the reference count of a page by 1
// return 0 if successful. return -1 if failed. 
int
page_refinc(void *physical)
{
  uint16 *refcnt = __refcnt_addr(physical);
  if (refcnt == 0) {
    return -1;
  }
  acquire(&kmem.lock);
  (*refcnt)++;
  release(&kmem.lock);
  return 0;
}

// return the reference count of a page
// return -1 if failed
int
page_refcnt(void *physical)
{
  uint16 *refcnt, ret;
  refcnt = __refcnt_addr(physical);
  if (refcnt == 0) {
    return -1;
  }
  acquire(&kmem.lock);
  ret = *refcnt;
  release(&kmem.lock);
  return ret;
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  uint16 *refcnt;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    refcnt = __refcnt_addr(p);
    if (refcnt == 0) {
      panic("freerange");
    }
    (*refcnt)++;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  uint16 *refcnt;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  refcnt = __refcnt_addr(pa);
  if (refcnt == 0) {
    panic("kfree(): invalid refcnt");
  }
  acquire(&kmem.lock);
  if (*refcnt == 0) {
    panic("kfree(): trying to free a freed page");
  }
  (*refcnt)--;
  if (*refcnt > 0) {
    release(&kmem.lock);
    return;
  }
  release(&kmem.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  uint16 *refcnt;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  refcnt = __refcnt_addr(r);
  if (refcnt == 0 || *refcnt > 0) {
    panic("kalloc");
  }
  (*refcnt)++;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
