/**
 * @file folio.h
 * @brief Linux-style folio public API.
 *
 * A folio is a head page of a (possibly compound) page group.  All
 * reference counting, locking, and type queries operate on the head
 * page, making it impossible to accidentally manipulate a tail page.
 */
#ifndef __KERNEL_FOLIO_H
#define __KERNEL_FOLIO_H

#include <mm/folio_types.h>
#include <mm/page.h>
#include <param.h>

/* ======================================================================
 *  Conversion helpers
 * ====================================================================== */

/**
 * page_folio - obtain the folio that contains @page.
 *
 * If @page is a tail page, the head page is returned.  If @page is
 * already a head (or order-0) page, it is returned directly.
 */
folio_t *page_folio(page_t *page);

/**
 * folio_page - return the @n-th page inside @folio.
 *
 * @n must be < folio_nr_pages(folio).
 */
static inline page_t *folio_page(folio_t *folio, unsigned long n)
{
    return &folio->page + n;
}

/* ======================================================================
 *  Property accessors
 * ====================================================================== */

/** Return the compound order of the folio (0 for a single page). */
static inline unsigned int folio_order(folio_t *folio)
{
    return folio->page.compound_order;
}

/** Return the number of base pages in the folio. */
static inline unsigned long folio_nr_pages(folio_t *folio)
{
    return 1UL << folio_order(folio);
}

/** Return the size of the folio in bytes. */
static inline unsigned long folio_size(folio_t *folio)
{
    return folio_nr_pages(folio) << PAGE_SHIFT;
}

/** Return the physical address of the first byte of the folio. */
static inline uint64 folio_address(folio_t *folio)
{
    return folio->page.physical_address;
}

/** Return the page type stored in the head page flags. */
static inline unsigned int folio_type(folio_t *folio)
{
    return PAGE_FLAG_GET_TYPE(folio->page.flags);
}

static inline int folio_is_type(folio_t *folio, enum page_type type)
{
    return folio_type(folio) == (unsigned int)type;
}

#define folio_test_pcache(f) folio_is_type((f), PAGE_TYPE_PCACHE)
#define folio_test_anon(f)   folio_is_type((f), PAGE_TYPE_ANON)

/**
 * page_is_pcache - check whether @page belongs to the page cache.
 *
 * For compound pcache folios the PTE may map a tail page whose own type
 * is PAGE_TYPE_TAIL, not PAGE_TYPE_PCACHE.  This helper resolves the
 * head page and checks the head's type so that callers don't have to
 * handle the two cases separately.
 */
static inline int page_is_pcache(page_t *page)
{
    if (page == NULL)
        return 0;
    if (PAGE_IS_TYPE(page, PAGE_TYPE_PCACHE))
        return 1;
    if (PAGE_IS_TYPE(page, PAGE_TYPE_TAIL) && page->tail.head_page != NULL)
        return PAGE_IS_TYPE(page->tail.head_page, PAGE_TYPE_PCACHE);
    return 0;
}

/**
 * page_pcache_head - return the head page of a pcache folio.
 *
 * @page may be a tail page; this returns the head (which holds the
 * pcache.pcache and pcache.pcache_node pointers).
 * Returns NULL if @page is not a pcache page.
 */
static inline page_t *page_pcache_head(page_t *page)
{
    if (page == NULL)
        return NULL;
    if (PAGE_IS_TYPE(page, PAGE_TYPE_PCACHE))
        return page;
    if (PAGE_IS_TYPE(page, PAGE_TYPE_TAIL) && page->tail.head_page != NULL &&
        PAGE_IS_TYPE(page->tail.head_page, PAGE_TYPE_PCACHE))
        return page->tail.head_page;
    return NULL;
}

/* ======================================================================
 *  Lifecycle
 * ====================================================================== */

/**
 * folio_alloc - allocate a folio of the given compound order.
 * @order:  compound order (0 .. PAGE_BUDDY_MAX_ORDER).
 * @flags:  page type (PAGE_TYPE_*) OR'd with optional GFP flags.
 *
 * Returns NULL on failure.  The returned folio has ref_count == 1.
 * For order > 0, tail pages are set up with PAGE_TYPE_TAIL and a
 * back-pointer to the head page.
 */
folio_t *folio_alloc(unsigned int order, uint64 flags);

/**
 * folio_free - return a folio to the buddy allocator.
 *
 * The folio must have ref_count == 1 (the caller's last reference).
 */
void folio_free(folio_t *folio);

/* ======================================================================
 *  Reference counting  (operates on head page)
 * ====================================================================== */

/** Increment the folio's reference count. */
static inline int folio_get(folio_t *folio)
{
    return __page_ref_inc(&folio->page);
}

/**
 * folio_put - drop one reference to @folio.
 *
 * If the reference count reaches zero the folio is returned to the
 * allocator automatically (via __page_ref_dec → __buddy_put).
 */
static inline int folio_put(folio_t *folio)
{
    return __page_ref_dec(&folio->page);
}

/** Increment ref without holding the page lock (caller holds ≥2 refs). */
static inline int folio_ref_inc_unlocked(folio_t *folio)
{
    return page_ref_inc_unlocked(&folio->page);
}

/** Decrement ref without holding the page lock (result will be ≥1). */
static inline int folio_ref_dec_unlocked(folio_t *folio)
{
    return page_ref_dec_unlocked(&folio->page);
}

/** Return the current reference count. */
static inline int folio_ref_count(folio_t *folio)
{
    return page_ref_count(&folio->page);
}

/* ======================================================================
 *  Locking  (operates on head page's spinlock)
 * ====================================================================== */

static inline void folio_lock(folio_t *folio)
{
    page_lock_acquire(&folio->page);
}

static inline void folio_unlock(folio_t *folio)
{
    page_lock_release(&folio->page);
}

/* ======================================================================
 *  Anonymous-page helpers
 * ====================================================================== */

static inline int folio_mapcount(folio_t *folio)
{
    return folio->page.anon.mapcount;
}

/* ======================================================================
 *  Address helpers
 * ====================================================================== */

/** Convert a physical address to the folio that contains it. */
static inline folio_t *pa_to_folio(uint64 pa)
{
    page_t *pg = __pa_to_page(pa);
    if (pg == NULL)
        return NULL;
    return page_folio(pg);
}

#endif /* __KERNEL_FOLIO_H */
