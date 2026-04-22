/**
 * @file folio.c
 * @brief Linux-style folio implementation.
 *
 * Provides allocation, freeing, and page↔folio conversion for compound
 * page groups.  All reference-counting is delegated to the page_t layer
 * (head page only); this file adds the compound-order bookkeeping.
 */

#include <mm/folio.h>
#include <mm/page.h>
#include <mm/page_type.h>
#include <printf.h>

/* ======================================================================
 *  Conversion
 * ====================================================================== */

folio_t *page_folio(page_t *page)
{
    if (page == NULL)
        return NULL;
    if (PAGE_IS_TYPE(page, PAGE_TYPE_TAIL) && page->tail.head_page != NULL)
        return (folio_t *)page->tail.head_page;
    return (folio_t *)page;
}

/* ======================================================================
 *  Allocation / free
 * ====================================================================== */

folio_t *folio_alloc(unsigned int order, uint64 flags)
{
    page_t *head = __page_alloc((uint64)order, flags);
    if (head == NULL)
        return NULL;

    /* __page_alloc already sets compound_order and (for types that
     * support tails) sets up PAGE_TYPE_TAIL + head_page pointers. */
    return (folio_t *)head;
}

void folio_free(folio_t *folio)
{
    if (folio == NULL)
        return;
    unsigned int order = folio_order(folio);
    __page_free(&folio->page, (uint64)order);
}
