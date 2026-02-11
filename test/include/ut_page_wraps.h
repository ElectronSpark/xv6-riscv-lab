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
#include <mm/memlayout.h>
#include <mm/page.h>
#include "mm/page_private.h"
#include "spinlock.h"

// Mock function declarations to prevent implicit function declaration warnings
void *__wrap_page_alloc(uint64 order, uint64 flags);
void __wrap_page_free(void *ptr, uint64 order);
int __wrap_page_ref_count(page_t *page);
int __wrap_page_ref_inc(void *ptr);
int __wrap_page_ref_dec(void *ptr);
int __wrap_page_refcnt(void *physical);
int __wrap_spin_holding(spinlock_t *lock);
page_t *__wrap___page_alloc(uint64 order, uint64 flags);
void __wrap___page_free(page_t *page, uint64 order);

// Function declarations for real functions that are aliases to the wrappers
void *__real_page_alloc(uint64 order, uint64 flags);
void __real_page_free(void *ptr, uint64 order);
int __real_page_ref_inc(void *ptr);
int __real_page_ref_dec(void *ptr);
int __real_page_refcnt(void *ptr);
page_t *__real___page_alloc(uint64 order, uint64 flags);
void __real___page_free(page_t *page, uint64 order);

extern bool __wrap___page_alloc_passthrough;
extern bool __wrap___page_free_passthrough;

// mock allocating pages with mmap
page_t *ut_make_mock_page(uint64 order, uint64 flags);
void ut_destroy_mock_page(void *physical);
void ut_destroy_mock_page_t(page_t *page);

#endif // __UT_MOCK_WRAPS_H__
