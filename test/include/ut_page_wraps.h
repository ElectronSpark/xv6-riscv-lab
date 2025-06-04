#ifndef __UT_MOCK_WRAPS_H__
#define __UT_MOCK_WRAPS_H__

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "cmocka.h"

#include "types.h"
#include "riscv.h"
#include "page.h"
#include "memlayout.h"

extern page_t mock_pages[8];

// External functions we need for testing
extern uint64 __get_buddy_addr(uint64 physical, uint32 order);

// Mock function declarations to prevent implicit function declaration warnings
page_t *__wrap___page_alloc(uint64 order, uint64 flags);
void __wrap___page_free(page_t *page, uint64 order);
page_t *__wrap___pa_to_page(uint64 physical);
uint64 __wrap___page_to_pa(page_t *page);
void *__wrap_page_alloc(uint64 order, uint64 flags);
void __wrap_page_free(void *ptr, uint64 order);

// Define buddy_pool_t structure for testing
typedef struct {
    list_node_t     lru_head;
    spinlock_t      lock;
    uint64          count;
} buddy_pool_t;

// Define necessary buddy system macros needed for testing
// The size of a buddy group in bytes
#define PAGE_BUDDY_BYTES(order) \
    (1UL << ((order) + PAGE_SHIFT))

// The address mask to get the offset address of a buddy group
#define PAGE_BUDDY_OFFSET_MASK(order) \
    (PAGE_BUDDY_BYTES(order) - 1)

// The address mask to get the base address of a buddy group
#define PAGE_BUDDY_BASE_MASK(order) \
    (~PAGE_BUDDY_OFFSET_MASK(order))

#endif // __UT_MOCK_WRAPS_H__
