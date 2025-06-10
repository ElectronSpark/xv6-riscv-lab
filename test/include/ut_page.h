#ifndef __UT_PAGE_H__
#define __UT_PAGE_H__

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include "cmocka.h"

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "page.h"
#include "page_private.h"

// Structure to store buddy system state for page tests
typedef struct {
    uint64 counts[PAGE_BUDDY_MAX_ORDER + 1];
    bool empty[PAGE_BUDDY_MAX_ORDER + 1];
    uint64 total_free_pages;
    bool skip;
} buddy_system_state_t;

// Mock function declarations to prevent implicit function declaration warnings
void *__wrap_page_alloc(uint64 order, uint64 flags);
void __wrap_page_free(void *ptr, uint64 order);
int __wrap_page_ref_count(page_t *page);
int __wrap_page_ref_inc(void *ptr);
int __wrap_page_ref_dec(void *ptr);
int __wrap_page_refcnt(void *physical);
page_t *__wrap___page_alloc(uint64 order, uint64 flags);
void __wrap___page_free(page_t *page, uint64 order);

// Function declarations for real functions that are aliases to the wrappers
void *__real_page_alloc(uint64 order, uint64 flags);
void __real_page_free(void *ptr, uint64 order);
int __real_page_ref_count(page_t *page);
int __real_page_ref_inc(void *ptr);
int __real_page_ref_dec(void *ptr);
int __real_page_refcnt(void *ptr);
page_t *__real___page_alloc(uint64 order, uint64 flags);
void __real___page_free(page_t *page, uint64 order);

// Passthrough flags
extern bool __wrap___page_alloc_passthrough;
extern bool __wrap___page_free_passthrough;
extern bool __wrap_page_alloc_passthrough;
extern bool __wrap_page_free_passthrough;
extern bool __wrap_page_ref_inc_passthrough;
extern bool __wrap_page_ref_dec_passthrough;
extern bool __wrap_page_refcnt_passthrough;
extern bool __wrap_page_ref_count_passthrough;

// Mock allocating pages with mmap
page_t *ut_make_mock_page(uint64 order, uint64 flags);
void ut_destroy_mock_page(void *physical);
void ut_destroy_mock_page_t(page_t *page);

// Functions to enable/disable page wrapper passthroughs
void ut_page_wrappers_enable_passthrough(void);
void ut_page_wrappers_disable_passthrough(void);

// Functions for more granular control of page wrapper passthroughs
void ut_page_core_alloc_enable_passthrough(void);
void ut_page_core_alloc_disable_passthrough(void);
void ut_page_public_alloc_enable_passthrough(void);
void ut_page_public_alloc_disable_passthrough(void);
void ut_page_ref_enable_passthrough(void);
void ut_page_ref_disable_passthrough(void);

#endif // __UT_PAGE_H__
