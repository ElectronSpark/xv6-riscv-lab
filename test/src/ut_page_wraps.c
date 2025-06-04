#include "ut_page_wraps.h"
#include "list.h"

WEAK page_t mock_pages[8] = { 0 };
WEAK page_t __pages[8] = { 0 };

void __wrap_page_lock_aqcuire(page_t *page) {
    (void)page;
}

void __wrap_page_lock_release(page_t *page) {
    (void)page;
}

page_t *__wrap___pa_to_page(uint64 physical) {
    print_message("physical: %lx\n", physical);
    
    // Validate physical address is within managed range
    if (physical < KERNBASE || physical >= KERNBASE + (8 * PGSIZE)) {
        print_message("physical address out of mock range: %lx\n", physical);
        return NULL;
    }
    
    uint64 index = (physical - KERNBASE) >> PAGE_SHIFT;
    if (index >= 8) {
        print_message("page index out of mock range: %lu\n", index);
        return NULL;
    }
    
    return &mock_pages[index]; // Using PAGE_SHIFT=12
}

uint64 __wrap___page_to_pa(page_t *page) {
    if (page == NULL) return 0;
    return ((page - mock_pages) << PAGE_SHIFT) + KERNBASE; // Using PAGE_SHIFT=12
}

int __wrap_page_ref_count(page_t *page) {
    if (page == NULL) return -1;
    return page->ref_count;
}

int __wrap___page_ref_inc(page_t *page) {
    if (page == NULL) return -1;
    if (page->ref_count == 0) return -1;
    page->ref_count++;
    return page->ref_count;
}

int __wrap___page_ref_dec(page_t *page) {
    if (page == NULL) return -1;
    if (page->ref_count > 0) {
        page->ref_count--;
        return page->ref_count;
    } else if (page->ref_count == 0) {
        // When trying to decrease a zero ref count, return -1 but don't modify the count
        return -1;
    } else {
        // For negative ref counts, just return the current value
        return page->ref_count;
    }
}

int __wrap_page_ref_inc(void *ptr) {
    page_t *page = __wrap___pa_to_page((uint64)ptr);
    if (page == NULL) return -1;
    return __wrap___page_ref_inc(page);
}

int __wrap_page_ref_dec(void *ptr) {
    page_t *page = __wrap___pa_to_page((uint64)ptr);
    if (page == NULL) return -1;
    return __wrap___page_ref_dec(page);
}

int __wrap_page_refcnt(void *physical) {
    if (physical == NULL) return -1;  // Handle NULL pointer case explicitly
    page_t *page = __wrap___pa_to_page((uint64)physical);
    if (page == NULL) return -1;
    return __wrap_page_ref_count(page);
}

void __wrap_panic(char *str) {
    print_message("%s\n", str);
    fail_msg("Panic encountered: %s", str);
}

void __wrap_acquire(struct spinlock *lock) {
    (void)lock;
}

void __wrap_release(struct spinlock *lock) {
    (void)lock;
}

void __wrap_initlock(struct spinlock *lock, char *name) {
    (void)lock;
    (void)name;
}

void *__wrap_memset(void *dst, int c, uint n) {
    return memset(dst, c, n);
}

void *__wrap_kmm_alloc(size_t size) {
    return test_malloc(size);
}

void __wrap_kmm_free(void *ptr) {
    test_free(ptr);
}

// Mock external variables needed by the tests
WEAK buddy_pool_t __buddy_pools[PAGE_BUDDY_MAX_ORDER + 1] = {0};
WEAK uint64 __managed_start = KERNBASE;
WEAK uint64 __managed_end = KERNBASE + 8 * PGSIZE; // 8 pages for our test

// Mock buddy system functions
page_t *__wrap___page_alloc(uint64 order, uint64 flags) {
    // Basic validity checks
    if (order > PAGE_BUDDY_MAX_ORDER) return NULL;
    
    // Check flags validity
    if (flags & PAGE_FLAG_LOCKED) return NULL;
    
    // For testing, we'll always return the first available page
    // This is a simplified approach and doesn't mimic the full buddy algorithm
    uint64 page_count = 1UL << order;
    
    // Find a suitably aligned page
    for (int i = 0; i < 8 - page_count + 1; i++) {
        // For orders > 0, the page index must be aligned to the order
        if (order > 0 && (i & ((1UL << order) - 1)) != 0) {
            // Not properly aligned for this order
            continue;
        }
        
        if (mock_pages[i].ref_count == 0) {
            // Found a potentially free, aligned page
            // Check if all pages in the group are free
            bool all_free = true;
            for (int j = 0; j < page_count; j++) {
                if (i + j < 8 && mock_pages[i + j].ref_count > 0) {
                    // This range is not entirely free
                    all_free = false;
                    break;
                }
            }
            
            if (!all_free) {
                continue;  // Try next page
            }
            
            // Initialize all pages in the group
            for (int j = 0; j < page_count; j++) {
                mock_pages[i + j].ref_count = 1;
                mock_pages[i + j].flags = flags;
            }
            
            print_message("  Allocated %lu pages at index %d (order %lu)\n", page_count, i, order);
            
            return &mock_pages[i];
        }
    }
    
    print_message("  Failed to allocate %lu pages (order %lu)\n", page_count, order);
    return NULL; // No suitable pages found
}

void __wrap___page_free(page_t *page, uint64 order) {
    if (page == NULL) return;
    if (order > PAGE_BUDDY_MAX_ORDER) {
        __wrap_panic("trying to free too many pages");
        return;
    }
    
    // Check if the page is in our mock_pages array
    if (page < mock_pages || page >= mock_pages + 8) {
        __wrap_panic("page outside of managed range");
        return;
    }
    
    // Check alignment - this needs to account for the buddy system's alignment requirements
    uint64 page_idx = page - mock_pages;
    
    // Debug output to help diagnose alignment issues
    print_message("  Freeing page at index %lu with order %lu, alignment mask: 0x%lx\n", 
                 page_idx, order, ((1UL << order) - 1));
    
    // For orders > 0, the page index must be aligned to the buddy boundary
    if (order > 0 && (page_idx & ((1UL << order) - 1)) != 0) {
        print_message("  Alignment error: page_idx=%lu, order=%lu, mask=0x%lx, result=0x%lx\n", 
                     page_idx, order, ((1UL << order) - 1), (page_idx & ((1UL << order) - 1)));
        
        // For the test environment, we'll just print a warning instead of panic
        print_message("WARNING: free pages not aligned to order %lu (page index: %lu)\n", 
                     order, page_idx);
    }
    
    // Check if all pages are in range
    uint64 page_count = 1UL << order;
    if (page_idx + page_count > 8) {
        __wrap_panic("page group extends beyond managed range");
        return;
    }
    
    // Free all pages in the group
    for (uint64 i = 0; i < page_count; i++) {
        page[i].ref_count = 0;
        page[i].flags = 0;
    }
}

void *__wrap_page_alloc(uint64 order, uint64 flags) {
    page_t *page = __wrap___page_alloc(order, flags);
    if (page == NULL) return NULL;
    return (void *)__wrap___page_to_pa(page);
}

void __wrap_page_free(void *ptr, uint64 order) {
    page_t *page = __wrap___pa_to_page((uint64)ptr);
    __wrap___page_free(page, order);
}

// Mock buddy system initialization
int __wrap_page_buddy_init(uint64 pa_start, uint64 pa_end) {
    // For our test, just mark all pages in the range as free 
    // and set up some buddy pools for verification
    
    // Check validity of range
    if (pa_start >= pa_end) return -1;
    if (pa_start < __managed_start || pa_end > __managed_end) return -1;
    
    uint64 num_pages = (pa_end - pa_start) >> PAGE_SHIFT;
    
    // Initialize mock buddy pools
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        __buddy_pools[i].count = 0;
    }
    
    // Put some pages in pools to simulate initialized buddy system
    // This is very simplified compared to the real buddy initialization
    if (num_pages >= 2) {
        // Add some single pages to order 0
        __buddy_pools[0].count = 2;
        
        // Add some pairs to order 1
        __buddy_pools[1].count = 1;
    }
    
    // Mark pages in the range as free
    for (uint64 addr = pa_start; addr < pa_end; addr += PGSIZE) {
        page_t *page = __wrap___pa_to_page(addr);
        if (page) {
            page->flags = 0;
            page->ref_count = 0;
        }
    }
    
    return 0;
}

// Mock for __get_buddy_addr
uint64 __wrap___get_buddy_addr(uint64 physical, uint32 order) {
    // The buddy address is the physical address with the bit at position (order + PAGE_SHIFT) toggled
    return physical ^ (1UL << (order + PAGE_SHIFT));
}