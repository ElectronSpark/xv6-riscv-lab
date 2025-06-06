#include "list.h" 
#include "ut_page_wraps.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

void __wrap_page_lock_aqcuire(page_t *page) {
    (void)page;
}

void __wrap_page_lock_release(page_t *page) {
    (void)page;
}

int __wrap_page_ref_count(page_t *page) {
    if (page == NULL) return -1;
    return page->ref_count;
}

int __wrap_page_ref_inc(void *ptr) {
    page_t *page = __pa_to_page((uint64)ptr);
    if (page == NULL) return -1;
    return __page_ref_inc(page);
}

int __wrap_page_ref_dec(void *ptr) {
    page_t *page = __pa_to_page((uint64)ptr);
    if (page == NULL) return -1;
    return __page_ref_dec(page);
}

int __wrap_page_refcnt(void *physical) {
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
    page_t *page = __page_alloc(order, flags);
    if (page == NULL) return NULL;
    return (void *)__page_to_pa(page);
}

void __wrap_page_free(void *ptr, uint64 order) {
    page_t *page = __pa_to_page((uint64)ptr);
    __page_free(page, order);
}
