#include "param.h"
#include "list.h"
#include "ut_page_wraps.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

// Wrapped page utility functions
page_t *__wrap___pa_to_page(uint64 physical) {
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
}

uint64 __wrap___page_to_pa(page_t *page) {
    if (page == NULL)
        return 0;
    return page->physical_address;
}

int __wrap___page_ref_inc(page_t *page) {
    if (page == NULL)
        return -1;
    return ++page->ref_count;
}

int __wrap___page_ref_dec(page_t *page) {
    if (page == NULL)
        return -1;
    if (page->ref_count > 0) {
        page->ref_count--;
    }
    return page->ref_count;
}

void __wrap___page_init(page_t *page, uint64 physical, int ref_count,
                        uint64 flags) {
    if (page == NULL)
        return;
    page->physical_address = physical;
    page->ref_count = ref_count;
    page->flags = flags;
    page->lock.locked = 0;
    page->lock.cpu = NULL;
}

void __wrap_page_lock_acquire(page_t *page) { (void)page; }

void __wrap_page_lock_spin_unlock(page_t *page) { (void)page; }

int __wrap_page_ref_count(page_t *page) {
    if (page == NULL)
        return -1;
    return page->ref_count;
}

int __wrap_page_ref_inc(void *ptr) {
    page_t *page = __wrap___pa_to_page((uint64)ptr);
    if (page == NULL)
        return -1;
    return __wrap___page_ref_inc(page);
}

int __wrap_page_ref_dec(void *ptr) {
    page_t *page = __wrap___pa_to_page((uint64)ptr);
    if (page == NULL)
        return -1;
    return __wrap___page_ref_dec(page);
}

int __wrap_page_refcnt(void *physical) {
    if (physical == NULL)
        return -1; // Handle NULL pointer case explicitly
    page_t *page = __wrap___pa_to_page((uint64)physical);
    if (page == NULL)
        return -1;
    return __wrap_page_ref_count(page);
}

void __wrap_panic(char *str) {
    print_message("%s\n", str);
    fail_msg("Panic encountered: %s", str);
}

void __wrap_spin_lock(spinlock_t *lock) {
    if (lock == NULL) {
        return;
    }
    lock->locked = 1;
}

void __wrap_spin_unlock(spinlock_t *lock) {
    if (lock == NULL) {
        return;
    }
    lock->locked = 0;
}

int __wrap_spin_holding(spinlock_t *lock) {
    if (lock == NULL) {
        return 0;
    }
    return lock->locked != 0;
}

void __wrap_spin_init(spinlock_t *lock, char *name) {
    (void)lock;
    (void)name;
}

void __wrap_spin_lock(spinlock_t *lock) {
    if (lock == NULL) {
        return;
    }
    lock->locked = 1;
}

void __wrap_spin_unlock(spinlock_t *lock) {
    if (lock == NULL) {
        return;
    }
    lock->locked = 0;
}

int __wrap_cpuid(void) {
    return 0; // Always return CPU 0 for tests
}

void __wrap_push_off(void) {
    // No-op for tests
}

void __wrap_pop_off(void) {
    // No-op for tests
}

void *__wrap_kmm_alloc(size_t size) { return test_malloc(size); }

void __wrap_kmm_free(void *ptr) { test_free(ptr); }

void *__wrap_page_alloc(uint64 order, uint64 flags) {
    page_t *page = ut_make_mock_page(order, flags);
    if (page == NULL)
        return NULL;
    return (void *)__wrap___page_to_pa(page);
}

void __wrap_page_free(void *ptr, uint64 order) {
    (void)order;
    ut_destroy_mock_page(ptr);
}

typedef struct ut_mock_page_range {
    union {
        page_t *page; // Pointer to the mock page descriptor
        void *mman_base;
    };
    void *mock_phy_start;
    uint64 order; // Order of the mock page
    uint64 size;  // Size of the mock page: 1UL << (order + PAGE_SHIFT + 1)
} ut_mock_page_range_t;

page_t *ut_make_mock_page(uint64 order, uint64 flags) {
    // page_t *page = malloc(sizeof(page_t));
    size_t mock_size = (1UL << (order + PAGE_SHIFT + 1));
    void *page_base = mmap(NULL, mock_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (page_base == NULL)
        return NULL;
    memset(page_base, 0, sizeof(page_t));

    ut_mock_page_range_t *mock_range =
        page_base + (mock_size >> 1) - sizeof(ut_mock_page_range_t);
    mock_range->page = (page_t *)page_base;
    mock_range->order = order;
    mock_range->size = mock_size;
    mock_range->mock_phy_start = page_base + (mock_size >> 1);

    __wrap___page_init(mock_range->page, (uint64)mock_range->mock_phy_start, 0,
                       flags);

    return page_base;
}

void ut_destroy_mock_page(void *physical) {
    if (physical == NULL)
        return;
    ut_mock_page_range_t *mock_range =
        (ut_mock_page_range_t *)((void *)physical -
                                 sizeof(ut_mock_page_range_t));

    if (munmap(mock_range->mman_base, mock_range->size) != 0) {
        print_message("Failed to unmap page memory\n");
    }
}

void ut_destroy_mock_page_t(page_t *page) {
    if (page == NULL)
        return;

    ut_destroy_mock_page((void *)page->physical_address);
}

int __real_page_refcnt(void *physical) {
    if (physical == NULL)
        return -1;
    page_t *page = __wrap___pa_to_page((uint64)physical);
    if (page == NULL)
        return -1;
    return page->ref_count;
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
