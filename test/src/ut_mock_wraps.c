#include "ut_mock_wraps.h"

page_t mock_pages[8] = { 0 };

void __wrap_page_lock_aqcuire(page_t *page) {
    (void)page;
}

void __wrap_page_lock_release(page_t *page) {
    (void)page;
}

page_t *__wrap___pa_to_page(uint64 physical) {
    print_message("physical: %lx\n", physical);
    return &mock_pages[(physical - KERNBASE) >> PAGE_SHIFT]; // Using PAGE_SHIFT=12
}

uint64 __wrap___page_to_pa(page_t *page) {
    if (page == NULL) return 0;
    return ((page - mock_pages) << PAGE_SHIFT) + KERNBASE; // Using PAGE_SHIFT=12
}

int __wrap_page_ref_count(page_t *page) {
    if (page == NULL) return 0;
    return page->ref_count;
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