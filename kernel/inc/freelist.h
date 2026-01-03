//
// Generic freelist management for fixed-size resource pools.
// Used by virtio disk descriptors, UART buffers, etc.
//

#ifndef __KERNEL_FREELIST_H
#define __KERNEL_FREELIST_H

#include "types.h"

// Generic freelist structure for managing a pool of resources
// Supports O(1) allocation and deallocation
struct freelist {
  char *free;           // is resource[i] free? (array of size max_items)
  uint16 *list;         // indices of free resources (array of size max_items)
  uint16 idx;           // current position in free list
  uint16 max_items;     // maximum number of items in the pool
};

// Initialize a freelist with the given arrays and capacity
// free and list arrays must be allocated by caller with size max_items
static inline void
freelist_init(struct freelist *fl, char *free, uint16 *list, uint16 max_items)
{
  fl->free = free;
  fl->list = list;
  fl->max_items = max_items;
  fl->idx = 0;
  
  // Initialize all items as free
  for(int i = 0; i < max_items; i++) {
    fl->free[i] = 1;
    fl->list[i] = i;
    fl->idx++;
  }
}

// Allocate an item from the freelist
// Returns item index on success, -1 if no free items available
static inline int
freelist_alloc(struct freelist *fl)
{
  if (fl->idx <= 0) {
    return -1;
  }

  fl->idx--;
  int idx = fl->list[fl->idx];
  fl->free[idx] = 0; // Mark as used
  return idx;
}

// Free an item back to the freelist
// Returns 0 on success, -1 on error
static inline int
freelist_free(struct freelist *fl, int i)
{
  if(i < 0 || i >= fl->max_items)
    return -1; // Invalid index
  if(fl->free[i])
    return -1; // Already free
  
  fl->free[i] = 1;
  fl->list[fl->idx] = i; // Add to free list
  fl->idx++;
  
  if(fl->idx > fl->max_items)
    return -1; // Corruption detected
  
  return 0;
}

// Check how many items are currently free
static inline int
freelist_available(struct freelist *fl)
{
  return fl->idx;
}

// Check if a specific item is free
static inline int
freelist_is_free(struct freelist *fl, int i)
{
  if(i < 0 || i >= fl->max_items)
    return 0;
  return fl->free[i];
}

#endif // __KERNEL_FREELIST_H
