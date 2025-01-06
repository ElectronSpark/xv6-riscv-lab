#ifndef __KERNEL_PAGE_H
#define __KERNEL_PAGE_H

#include "page_type.h"

uint64 managed_page_base();
page_t *__pa_to_page(uint64 physical);
uint64 __page_to_pa(page_t *page);
void page_lock_aqcuire(page_t *page);
void page_lock_release(page_t *page);
int page_buddy_init(uint64 pa_start, uint64 pa_end);
page_t *page_alloc(uint64 order, uint64 flags);
void page_free(page_t *page, uint64 order);
int page_ref_inc(page_t *page);
int page_ref_dec(page_t *page);
int page_ref_count(page_t *page);

void print_buddy_system_stat(void);


#endif              /* __KERNEL_PAGE_H */
