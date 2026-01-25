#ifndef __KERNEL_PAGE_H
#define __KERNEL_PAGE_H

#include "compiler.h"
#include <mm/page_type.h>

uint64 managed_page_base();
page_t *__pa_to_page(uint64 physical);
uint64 __page_to_pa(page_t *page);
void page_lock_acquire(page_t *page);
void page_lock_release(page_t *page);
void page_lock_assert_holding(page_t *page);
void page_lock_assert_unholding(page_t *page);
int page_buddy_init(void);
page_t *__page_alloc(uint64 order, uint64 flags);
void __page_free(page_t *page, uint64 order);
void *page_alloc(uint64 order, uint64 flags);
void page_free(void *ptr, uint64 order);
int __page_ref_inc(page_t *page);
int __page_ref_dec(page_t *page);
int page_ref_inc_unlocked(page_t *page);
int page_ref_dec_unlocked(page_t *page);
int page_refcnt(void *physical);
int page_ref_inc(void *ptr);
int page_ref_dec(void *ptr);
int page_ref_count(page_t *page);

void page_buddy_stat(uint64 *ret_arr, bool *empty_arr, size_t size);
void print_buddy_system_stat(int detailed);


#endif              /* __KERNEL_PAGE_H */
