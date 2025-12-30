/*
 * Page allocation wrappers for unit tests
 * Provides mock page allocation/deallocation
 * 
 * This file consolidates all page-related wrappers including:
 * - Page allocation/deallocation (page_alloc, page_free, __page_alloc, __page_free)
 * - Address conversion (__pa_to_page, __page_to_pa)
 * - Reference counting (page_ref_inc, page_ref_dec, page_refcnt)
 * - Page locking (page_lock_acquire, page_lock_release)
 * - Page initialization (__page_init)
 * - Spinlock operations (spin_acquire, spin_release, etc.)
 * - CPU and memory management utilities (cpuid, kmm_alloc, kmm_free)
 * - Mock page creation utilities for tests (ut_make_mock_page, ut_destroy_mock_page)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <cmocka.h>
#include "types.h"
#include "page.h"
#include "page_type.h"
#include "spinlock.h"
#include "param.h"
#include "list.h"

// Forward declarations for functions defined later in this file
page_t *__wrap___pa_to_page(uint64 physical);
uint64 __wrap___page_to_pa(page_t *page);
void __wrap___page_init(page_t *page, uint64 physical, int ref_count, uint64 flags);
int __wrap___page_ref_inc(page_t *page);
int __wrap___page_ref_dec(page_t *page);
int __wrap_page_ref_count(page_t *page);

// Forward declarations for real functions (for passthrough mode)
page_t *__real___page_alloc(uint64 order, uint64 flags);
void __real___page_free(page_t *page, uint64 order);

// Global flags for simulating failures
static bool g_test_fail_page_alloc = false;

void pcache_test_fail_next_page_alloc(void)
{
    g_test_fail_page_alloc = true;
}

void __wrap_page_lock_acquire(page_t *page)
{
    if (page == NULL) {
        return;
    }
    // Directly manipulate lock for mock - no dependency on spinlock wrappers
    page->lock.locked = 1;
}

void __wrap_page_lock_release(page_t *page)
{
    if (page == NULL) {
        return;
    }
    page->lock.locked = 0;
}

void __wrap_page_lock_spin_release(page_t *page)
{
    (void)page;
}

void __wrap_page_lock_assert_holding(page_t *page)
{
    if (page == NULL) {
        return;
    }
    assert(page->lock.locked);
}

void __wrap_page_lock_assert_unholding(page_t *page)
{
    if (page == NULL) {
        return;
    }
    assert(!page->lock.locked);
}

void __wrap_panic(char *str)
{
    print_message("%s\n", str);
    fail_msg("Panic encountered: %s", str);
}

int __wrap_page_ref_count(page_t *page)
{
    if (page == NULL) {
        return 0;
    }
    return page->ref_count;
}

int __wrap___page_ref_inc(page_t *page)
{
    if (page == NULL) return -1;
    return ++page->ref_count;
}

int __wrap___page_ref_dec(page_t *page)
{
    if (page == NULL) return -1;
    if (page->ref_count > 0) {
        page->ref_count--;
    }
    return page->ref_count;
}

int __wrap_page_ref_inc(void *ptr)
{
    page_t *page = __wrap___pa_to_page((uint64)ptr);
    if (page == NULL) return -1;
    return __wrap___page_ref_inc(page);
}

int __wrap_page_ref_dec(void *ptr)
{
    page_t *page = __wrap___pa_to_page((uint64)ptr);
    if (page == NULL) return -1;
    return __wrap___page_ref_dec(page);
}

int __wrap_page_ref_inc_unlocked(page_t *page)
{
    if (page == NULL) {
        return 0;
    }
    return ++page->ref_count;
}

int __wrap_page_ref_dec_unlocked(page_t *page)
{
    if (page == NULL) {
        return 0;
    }
    return --page->ref_count;
}

int __wrap_page_refcnt(void *physical)
{
    if (physical == NULL) return -1;
    page_t *page = __wrap___pa_to_page((uint64)physical);
    if (page == NULL) return -1;
    return __wrap_page_ref_count(page);
}

int __real_page_refcnt(void *physical)
{
    if (physical == NULL) return -1;
    page_t *page = __wrap___pa_to_page((uint64)physical);
    if (page == NULL) return -1;
    return page->ref_count;
}

uint64 __wrap___page_to_pa(page_t *page)
{
    if (page == NULL) {
        return 0;
    }
    return page->physical_address;
}

page_t *__wrap___pa_to_page(uint64 physical)
{
#ifdef UT_PAGE_TEST_BUILD
    // For ut_page test: use managed memory range checking
    extern uint64 __managed_start;
    extern uint64 __managed_end;
    
    // Check if address is within managed range
    if (physical < __managed_start || physical >= __managed_end) {
        return NULL;
    }
    
    // For unit tests, calculate page pointer from physical address
    extern page_t __pages[];
    uint64 page_index = (physical - __managed_start) / PAGE_SIZE;
    return &__pages[page_index];
#else
    // For other tests: simple cast (no range checking)
    return (page_t *)(uintptr_t)physical;
#endif
}

void __wrap___page_init(page_t *page, uint64 physical, int ref_count, uint64 flags)
{
    if (page == NULL) return;
    page->physical_address = physical;
    page->ref_count = ref_count;
    page->flags = flags;
    page->lock.locked = 0;
    page->lock.cpu = NULL;
}

// Mock page creation utilities for advanced testing
typedef struct ut_mock_page_range {
    union {
        page_t *page;
        void *mman_base;
    };
    void *mock_phy_start;
    uint64 order;
    uint64 size;
} ut_mock_page_range_t;

page_t *ut_make_mock_page(uint64 order, uint64 flags)
{
    size_t mock_size = (1UL << (order + PAGE_SHIFT + 1));
    void *page_base = mmap(NULL, mock_size,
                            PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (page_base == NULL) return NULL;
    memset(page_base, 0, sizeof(page_t));

    ut_mock_page_range_t *mock_range = 
        page_base + (mock_size >> 1) - sizeof(ut_mock_page_range_t);
    mock_range->page = (page_t *)page_base;
    mock_range->order = order;
    mock_range->size = mock_size;
    mock_range->mock_phy_start = page_base + (mock_size >> 1);

    __wrap___page_init(mock_range->page,
                (uint64)mock_range->mock_phy_start, 
                1, flags);  // ref_count starts at 1 (allocated pages have 1 reference)

    return page_base;
}

void ut_destroy_mock_page(void *physical)
{
    if (physical == NULL) return;
    ut_mock_page_range_t *mock_range = 
        (ut_mock_page_range_t *)((void *)physical - sizeof(ut_mock_page_range_t));

    if (munmap(mock_range->mman_base, mock_range->size) != 0) {
        print_message("Failed to unmap page memory\n");
    }
}

void ut_destroy_mock_page_t(page_t *page)
{
    if (page == NULL) return;
    ut_destroy_mock_page((void *)page->physical_address);
}

void *__wrap_page_alloc(uint64 order, uint64 flags)
{
    if (g_test_fail_page_alloc) {
        g_test_fail_page_alloc = false;
        return NULL;
    }
    page_t *page = ut_make_mock_page(order, flags);
    if (page == NULL) return NULL;
    return (void *)__wrap___page_to_pa(page);
}

void __wrap_page_free(void *ptr, uint64 order)
{
    (void)order;
    ut_destroy_mock_page(ptr);
}

// Passthrough control for advanced testing scenarios
#ifdef UT_PAGE_TEST_BUILD
// For ut_page: use real kernel allocator by default (compiled from page.c)
bool __wrap___page_alloc_passthrough = true;
page_t *__wrap___page_alloc(uint64 order, uint64 flags)
{
    if (__wrap___page_alloc_passthrough) {
        return __real___page_alloc(order, flags);
    }
    return mock_ptr_type(page_t *);
}

bool __wrap___page_free_passthrough = true;
void __wrap___page_free(page_t *page, uint64 order)
{
    if (__wrap___page_free_passthrough) {
        __real___page_free(page, order);
    } else {
        mock();
    }
}
#else
// For other tests: allocate page_t and set up mapping
bool __wrap___page_alloc_passthrough = false;
page_t *__wrap___page_alloc(uint64 order, uint64 flags)
{
    if (g_test_fail_page_alloc) {
        g_test_fail_page_alloc = false;
        return NULL;
    }
    // Create a page_t structure
    page_t *page = ut_make_mock_page(order, flags);
    return page;  // Return the page_t pointer, not the physical address
}

bool __wrap___page_free_passthrough = false;
void __wrap___page_free(page_t *page, uint64 order)
{
    (void)order;
    if (page == NULL) return;
    ut_destroy_mock_page_t(page);
}
#endif
