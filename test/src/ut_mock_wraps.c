#include "ut_mock_wraps.h"

page_t mock_pages[8] = { 0 };

void __wrap_page_lock_aqcuire(page_t *page) {
    (void)page;
}

void __wrap_page_lock_release(page_t *page) {
    (void)page;
}

page_t *__wrap___pa_to_page(uint64 physical) {
    return &mock_pages[physical >> 3];
}

void __wrap_panic(char*str) {
    printf(str);
}

void __wrap_acquire(struct spinlock*lock) {
    (void)lock;
}

void __wrap_release(struct spinlock*lock) {
    (void)lock;
}

void __wrap_initlock(struct spinlock*lock, char*name) {
    (void)lock;
    (void)name;
}