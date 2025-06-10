#include "list.h" 
#include "ut_page.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

// Passthrough flags for page wrapper functions
bool __wrap_page_alloc_passthrough = false;
bool __wrap_page_free_passthrough = false;
bool __wrap_page_ref_inc_passthrough = false;
bool __wrap_page_ref_dec_passthrough = false;
bool __wrap_page_refcnt_passthrough = false;
bool __wrap_page_ref_count_passthrough = false;

void __wrap_page_lock_aqcuire(page_t *page) {
    (void)page;
}

void __wrap_page_lock_release(page_t *page) {
    (void)page;
}

int __wrap_page_ref_count(page_t *page) {
    if (__wrap_page_ref_count_passthrough) {
        return __real_page_ref_count(page);
    }
    if (page == NULL) return -1;
    return page->ref_count;
}

int __wrap_page_ref_inc(void *ptr) {
    if (__wrap_page_ref_inc_passthrough) {
        return __real_page_ref_inc(ptr);
    }
    page_t *page = __pa_to_page((uint64)ptr);
    if (page == NULL) return -1;
    return __page_ref_inc(page);
}

int __wrap_page_ref_dec(void *ptr) {
    if (__wrap_page_ref_dec_passthrough) {
        return __real_page_ref_dec(ptr);
    }
    page_t *page = __pa_to_page((uint64)ptr);
    if (page == NULL) return -1;
    return __page_ref_dec(page);
}

int __wrap_page_refcnt(void *physical) {
    if (__wrap_page_refcnt_passthrough) {
        return __real_page_refcnt(physical);
    }
    if (physical == NULL) return -1;  // Handle NULL pointer case explicitly
    page_t *page = __pa_to_page((uint64)physical);
    if (page == NULL) return -1;
    return page_ref_count(page);
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

void *__wrap_kmm_alloc(size_t size) {
    return test_malloc(size);
}

void __wrap_kmm_free(void *ptr) {
    test_free(ptr);
}

void *__wrap_page_alloc(uint64 order, uint64 flags) {
    if (__wrap_page_alloc_passthrough) {
        return __real_page_alloc(order, flags);
    }
    page_t *page = __page_alloc(order, flags);
    if (page == NULL) return NULL;
    return (void *)__page_to_pa(page);
}

void __wrap_page_free(void *ptr, uint64 order) {
    if (__wrap_page_free_passthrough) {
        __real_page_free(ptr, order);
        return;
    }
    page_t *page = __pa_to_page((uint64)ptr);
    __page_free(page, order);
}

typedef struct ut_mock_page_range {
    union {
        page_t *page; // Pointer to the mock page descriptor
        void *mman_base;
    };
    void *mock_phy_start;
    uint64 order;   // Order of the mock page
    uint64 size;    // Size of the mock page: 1UL << (order + PAGE_SHIFT + 1)
} ut_mock_page_range_t;

page_t *ut_make_mock_page(uint64 order, uint64 flags) {
    // page_t *page = malloc(sizeof(page_t));
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

    __page_init(mock_range->page,
                (uint64)mock_range->mock_phy_start, 
                0, flags);

    return page_base;
}

void ut_destroy_mock_page(void *physical) {
    if (physical == NULL) return;
    ut_mock_page_range_t *mock_range = (ut_mock_page_range_t *)((void *)physical - sizeof(ut_mock_page_range_t));

    if (munmap(mock_range->mman_base, mock_range->size) != 0) {
        print_message("Failed to unmap page memory\n");
    }
}

void ut_destroy_mock_page_t(page_t *page) {
    if (page == NULL) return;

    ut_destroy_mock_page((void *)page->physical_address);
}

bool __wrap___page_alloc_passthrough = false;
page_t *__wrap___page_alloc(uint64 order, uint64 flags) {
    if (__wrap___page_alloc_passthrough) {
        return __real___page_alloc(order, flags);
    }
    return mock_ptr_type(page_t *);
}

bool __wrap___page_free_passthrough = false;
void __wrap___page_free(page_t *page, uint64 order) {
    if (__wrap___page_free_passthrough) {
        __real___page_free(page, order);
    } else {
        mock();
    }
}

/**
 * Enable passthrough for all page wrapper functions
 * This makes all page wrapper functions call their real counterparts
 */
void ut_page_wrappers_enable_passthrough(void) {
    __wrap_page_alloc_passthrough = true;
    __wrap_page_free_passthrough = true;
    __wrap_page_ref_inc_passthrough = true;
    __wrap_page_ref_dec_passthrough = true;
    __wrap_page_refcnt_passthrough = true;
    __wrap_page_ref_count_passthrough = true;
    __wrap___page_alloc_passthrough = true;
    __wrap___page_free_passthrough = true;
}

/**
 * Disable passthrough for all page wrapper functions
 * This makes all page wrapper functions use mocks
 */
void ut_page_wrappers_disable_passthrough(void) {
    __wrap_page_alloc_passthrough = false;
    __wrap_page_free_passthrough = false;
    __wrap_page_ref_inc_passthrough = false;
    __wrap_page_ref_dec_passthrough = false;
    __wrap_page_refcnt_passthrough = false;
    __wrap_page_ref_count_passthrough = false;
    __wrap___page_alloc_passthrough = false;
    __wrap___page_free_passthrough = false;
}

/**
 * Enable passthrough for core page allocation functions
 * This enables __page_alloc and __page_free
 */
void ut_page_core_alloc_enable_passthrough(void) {
    __wrap___page_alloc_passthrough = true;
    __wrap___page_free_passthrough = true;
}

/**
 * Disable passthrough for core page allocation functions
 */
void ut_page_core_alloc_disable_passthrough(void) {
    __wrap___page_alloc_passthrough = false;
    __wrap___page_free_passthrough = false;
}

/**
 * Enable passthrough for public page allocation functions
 * This enables page_alloc and page_free
 */
void ut_page_public_alloc_enable_passthrough(void) {
    __wrap_page_alloc_passthrough = true;
    __wrap_page_free_passthrough = true;
}

/**
 * Disable passthrough for public page allocation functions
 */
void ut_page_public_alloc_disable_passthrough(void) {
    __wrap_page_alloc_passthrough = false;
    __wrap_page_free_passthrough = false;
}

/**
 * Enable passthrough for page reference counting functions
 * This enables page_ref_inc, page_ref_dec, page_ref_count, and page_refcnt
 */
void ut_page_ref_enable_passthrough(void) {
    __wrap_page_ref_inc_passthrough = true;
    __wrap_page_ref_dec_passthrough = true;
    __wrap_page_refcnt_passthrough = true;
    __wrap_page_ref_count_passthrough = true;
}

/**
 * Disable passthrough for page reference counting functions
 */
void ut_page_ref_disable_passthrough(void) {
    __wrap_page_ref_inc_passthrough = false;
    __wrap_page_ref_dec_passthrough = false;
    __wrap_page_refcnt_passthrough = false;
    __wrap_page_ref_count_passthrough = false;
}
