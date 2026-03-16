/**
 * @file folio_types.h
 * @brief Linux-style folio type definitions.
 *
 * A folio is a physically and virtually contiguous set of bytes, usually
 * mapped into the page cache.  It is a power-of-two in size and aligned
 * to that same power-of-two.  At its smallest, it consists of just one
 * page (order-0); at its largest, it contains 2^FOLIO_MAX_ORDER pages.
 *
 * In this implementation folio_t is defined as a struct whose first
 * member is a page_t, so (folio_t *)head_page is a zero-cost cast.
 */
#ifndef __KERNEL_FOLIO_TYPES_H
#define __KERNEL_FOLIO_TYPES_H

#include <mm/page_type.h>

/**
 * Maximum compound order for page-cache folios.
 * Order 4 = 16 pages = 64 KB (with 4 KB base page size).
 * NOTE: folio_alloc() itself accepts up to PAGE_BUDDY_MAX_ORDER;
 * this constant only constrains pcache / xv6fs stack arrays.
 */
#define FOLIO_MAX_ORDER 4
#define FOLIO_MAX_ORDER_NR_PAGES (1U << FOLIO_MAX_ORDER)

/**
 * struct folio - a contiguous power-of-two group of pages.
 * @page: the head (first) page of the group.
 *
 * The struct is intentionally layout-compatible with page_t so that
 *   &folio->page == (page_t *)folio
 * is always true.  folio_t may only point to a head page; it is never
 * used to refer to a tail page.
 */
typedef struct folio {
    page_t page; /* must be first */
} folio_t;

#endif /* __KERNEL_FOLIO_TYPES_H */
