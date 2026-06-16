#ifndef __KERNEL_RMAP_H
#define __KERNEL_RMAP_H

#include "types.h"
#include "list_type.h"
#include "bintree_type.h"
#include "lock/rwsem_types.h"

/*
 * Reverse Mapping (rmap) for anonymous pages.
 *
 * Modelled after Linux's anon_vma / anon_vma_chain system:
 *
 *  - Each anonymous VMA that has mapped pages owns an anon_vma.
 *  - Each anon_vma holds an rb-tree of anon_vma_chain nodes (AVCs) linking
 *    it to all VMAs whose pages it may contain.
 *  - Each VMA maintains a list of AVCs (same_vma chain) linking it to all
 *    anon_vmas that may contain pages originally allocated under that VMA
 *    or its fork ancestors.
 *
 * On fork, the child VMA gets its own anon_vma and an AVC linking the
 * child VMA to the parent's anon_vma.  This way, try_to_unmap() can
 * locate every PTE mapping a given physical page by walking the page's
 * anon_vma rb-tree.
 *
 * Locking
 * -------
 *   anon_vma->rwsem
 *     - read-locked  for rmap traversal  (try_to_unmap, page_referenced, ...)
 *     - write-locked for structural mods (adding/removing AVCs)
 *
 *   Locking order:
 *     vm->rw_lock  →  anon_vma->rwsem  →  vm->spinlock (pgtable)  →  page->lock
 */

/* Forward declarations */
typedef struct vma vma_t;
typedef struct vm vm_t;
typedef struct page_struct page_t;

/**
 * struct anon_vma - shared object that groups VMAs whose pages may overlap.
 * @rwsem:    protects the vma_tree; read for traversal, write for modification.
 * @refcount: number of AVCs pointing here.  Freed when it drops to 0.
 * @vma_tree: rb-tree of anon_vma_chain nodes, keyed by a stable AVC key.
 */
struct anon_vma {
    rwsem_t rwsem;
    int refcount;
    struct rb_root vma_tree;
};

/**
 * struct anon_vma_chain - link between a VMA and an anon_vma.
 * @rb_entry: node in anon_vma->vma_tree.
 * @tree_key: stable unique key for anon_vma->vma_tree.
 * @vma:      the VMA this AVC belongs to.
 * @anon_vma: the anon_vma this AVC points to.
 * @same_vma: list node chaining all AVCs that belong to the same VMA.
 */
struct anon_vma_chain {
    struct rb_node rb_entry;
    uint64 tree_key;
    vma_t *vma;
    struct anon_vma *anon_vma;
    list_node_t same_vma;
};

/* ========================================================================== */
/*  Lifecycle                                                                 */
/* ========================================================================== */

/** Initialise slab caches — called once from mm init. */
void rmap_init(void);

/** Allocate a fresh anon_vma (refcount = 1). */
struct anon_vma *anon_vma_alloc(void);

/** Increment refcount. */
void anon_vma_ref(struct anon_vma *av);

/** Decrement refcount; free when it reaches 0. */
void anon_vma_put(struct anon_vma *av);

/** Allocate an anon_vma_chain from slab. */
struct anon_vma_chain *avc_alloc(void);

/** Free an anon_vma_chain back to slab. */
void avc_free(struct anon_vma_chain *avc);

/* ========================================================================== */
/*  VMA ↔ anon_vma wiring                                                    */
/* ========================================================================== */

/**
 * anon_vma_prepare - ensure @vma has an anon_vma.
 *
 * If @vma->anon_vma is already set, this is a no-op.
 * Otherwise allocates an anon_vma + AVC and links them.
 *
 * Returns 0 on success, -ENOMEM on failure.
 */
int anon_vma_prepare(vma_t *vma);

/**
 * anon_vma_chain_link - insert @avc into @vma's same_vma list and
 *                       @anon_vma's vma_tree.
 *
 * Caller must hold anon_vma->rwsem for write.
 */
void anon_vma_chain_link(vma_t *vma, struct anon_vma_chain *avc,
                         struct anon_vma *anon_vma);

/**
 * anon_vma_fork - set up rmap linkage for a child VMA after fork.
 *
 * Creates a new anon_vma for @child and links @child to @parent's
 * anon_vma chain so that pages shared via COW can be located by
 * try_to_unmap().
 *
 * Returns 0 on success, -ENOMEM on failure.
 */
int anon_vma_fork(vma_t *child, vma_t *parent);

/**
 * anon_vma_unlink - tear down all AVC links for @vma.
 *
 * Called when a VMA is being freed.  For each AVC in @vma->anon_vma_chain,
 * removes it from the corresponding anon_vma's rb-tree and puts the refcount.
 */
void anon_vma_unlink(vma_t *vma);

/* ========================================================================== */
/*  Page rmap operations                                                      */
/* ========================================================================== */

/**
 * page_add_anon_rmap - record that @page is now mapped in @vma at @va.
 *
 * Atomically increments page->anon.mapcount and, if this is the first
 * mapping, sets page->anon.anon_vma.
 */
void page_add_anon_rmap(page_t *page, vma_t *vma, uint64 va);

/**
 * page_remove_rmap - record that one PTE mapping of @page has been removed.
 *
 * Atomically decrements page->anon.mapcount.
 */
void page_remove_rmap(page_t *page);

/** Returns true if page->anon.mapcount > 0. */
int page_mapped(page_t *page);

/** Returns current mapcount. */
int page_mapcount(page_t *page);

/* ========================================================================== */
/*  Folio rmap wrappers                                                       */
/* ========================================================================== */

typedef struct folio folio_t;

/**
 * folio_add_anon_rmap - record that @folio's head page is mapped at @va.
 *
 * Thin wrapper around page_add_anon_rmap operating on the head page.
 */
static inline void folio_add_anon_rmap(folio_t *folio, vma_t *vma, uint64 va)
{
    page_add_anon_rmap((page_t *)folio, vma, va);
}

/**
 * folio_remove_rmap - remove one PTE mapping from @folio's head page.
 */
static inline void folio_remove_rmap(folio_t *folio)
{
    page_remove_rmap((page_t *)folio);
}

/** Returns true if folio head page mapcount > 0. */
static inline int folio_mapped(folio_t *folio)
{
    return page_mapped((page_t *)folio);
}

/** Returns current mapcount of folio head page. */
static inline int folio_rmap_mapcount(folio_t *folio)
{
    return page_mapcount((page_t *)folio);
}

/**
 * try_to_unmap - remove ALL PTE mappings of @page.
 *
 * Walks the page's anon_vma rb-tree (under read-lock), locates every
 * PTE that maps this page, clears it, flushes TLB, and decrements
 * mapcount.
 *
 * Returns 0 if all mappings were successfully removed, -1 otherwise.
 */
int try_to_unmap(page_t *page);

#endif /* __KERNEL_RMAP_H */
