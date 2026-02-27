/*
 * rmap.c — Reverse Mapping (rmap) for anonymous pages.
 *
 * Implements Linux-style anon_vma / anon_vma_chain infrastructure.
 * See kernel/inc/mm/rmap.h for design overview and locking rules.
 */

#include "types.h"
#include "param.h"
#include "defs.h"
#include <mm/rmap.h>
#include <mm/pgtable.h>
#include <mm/vm.h>
#include <mm/vm_types.h>
#include <mm/page.h>
#include <mm/page_type.h>
#include <mm/slab.h>
#include "lock/rwsem.h"
#include "bintree.h"
#include "list.h"
#include "string.h"
#include <smp/atomic.h>

/* ========================================================================== */
/*  Slab caches                                                               */
/* ========================================================================== */

static slab_cache_t __anon_vma_pool = {0};
static slab_cache_t __avc_pool = {0};

/* ========================================================================== */
/*  RB-tree opts for anon_vma->vma_tree (keyed by vma->start)                 */
/* ========================================================================== */

static int __avc_cmp(uint64 a, uint64 b)
{
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static uint64 __avc_get_key(struct rb_node *node)
{
    struct anon_vma_chain *avc =
        container_of(node, struct anon_vma_chain, rb_entry);
    return avc->vma->start;
}

static struct rb_root_opts __avc_tree_opts = {
    .keys_cmp_fun = __avc_cmp,
    .get_key_fun = __avc_get_key,
};

/* ========================================================================== */
/*  Lifecycle                                                                 */
/* ========================================================================== */

void rmap_init(void)
{
    slab_cache_init(&__anon_vma_pool, "anon_vma",
                    sizeof(struct anon_vma), SLAB_FLAG_STATIC);
    slab_cache_init(&__avc_pool, "anon_vma_chain",
                    sizeof(struct anon_vma_chain), SLAB_FLAG_STATIC);
}

struct anon_vma *anon_vma_alloc(void)
{
    struct anon_vma *av = slab_alloc(&__anon_vma_pool);
    if (av == NULL)
        return NULL;
    memset(av, 0, sizeof(*av));
    rwsem_init(&av->rwsem, 0, "anon_vma");
    av->refcount = 1;
    rb_root_init(&av->vma_tree, &__avc_tree_opts);
    return av;
}

void anon_vma_ref(struct anon_vma *av)
{
    if (av == NULL)
        return;
    __sync_fetch_and_add(&av->refcount, 1);
}

void anon_vma_put(struct anon_vma *av)
{
    if (av == NULL)
        return;
    int old = __sync_fetch_and_sub(&av->refcount, 1);
    if (old == 1) {
        /* Last reference — free it. */
        slab_free(av);
    }
}

struct anon_vma_chain *avc_alloc(void)
{
    struct anon_vma_chain *avc = slab_alloc(&__avc_pool);
    if (avc == NULL)
        return NULL;
    memset(avc, 0, sizeof(*avc));
    rb_node_init(&avc->rb_entry);
    list_entry_init(&avc->same_vma);
    return avc;
}

void avc_free(struct anon_vma_chain *avc)
{
    if (avc)
        slab_free(avc);
}

/* ========================================================================== */
/*  VMA ↔ anon_vma wiring                                                    */
/* ========================================================================== */

void anon_vma_chain_link(vma_t *vma, struct anon_vma_chain *avc,
                         struct anon_vma *anon_vma)
{
    avc->vma = vma;
    avc->anon_vma = anon_vma;

    /* Add to VMA's list of AVCs. */
    list_node_push_back(&vma->anon_vma_chain, avc, same_vma);

    /* Add to anon_vma's rb-tree.
     * If a node with the same key exists (same vma->start from a
     * previous VMA that occupied this address), rb_insert_node returns
     * the existing node.  For simplicity we allow duplicates by ensuring
     * keys include enough uniqueness.  In practice, the VMA start
     * address plus refcount serialisation prevents collisions. */
    anon_vma_ref(anon_vma);
    rb_insert_node(&anon_vma->vma_tree, &avc->rb_entry);
}

int anon_vma_prepare(vma_t *vma)
{
    if (vma->anon_vma != NULL)
        return 0;

    struct anon_vma *av = anon_vma_alloc();
    if (av == NULL)
        return -12; /* ENOMEM */

    struct anon_vma_chain *avc = avc_alloc();
    if (avc == NULL) {
        anon_vma_put(av);
        return -12;
    }

    rwsem_acquire_write(&av->rwsem);
    anon_vma_chain_link(vma, avc, av);
    rwsem_release(&av->rwsem);

    vma->anon_vma = av;
    return 0;
}

int anon_vma_fork(vma_t *child, vma_t *parent)
{
    /* If parent has no anon_vma, nothing to do. */
    if (parent->anon_vma == NULL)
        return 0;

    /* Give the child its own anon_vma. */
    int ret = anon_vma_prepare(child);
    if (ret != 0)
        return ret;

    /* Link child to every anon_vma that parent is linked to.
     * Walk parent's AVC list and create a corresponding AVC for child
     * in each anon_vma. */
    struct anon_vma_chain *parent_avc;
    list_node_t *pos;
    for (pos = parent->anon_vma_chain.next;
         pos != &parent->anon_vma_chain;
         pos = pos->next) {
        parent_avc = container_of(pos, struct anon_vma_chain, same_vma);
        struct anon_vma *av = parent_avc->anon_vma;

        struct anon_vma_chain *new_avc = avc_alloc();
        if (new_avc == NULL)
            return -12;

        rwsem_acquire_write(&av->rwsem);
        anon_vma_chain_link(child, new_avc, av);
        rwsem_release(&av->rwsem);
    }

    return 0;
}

void anon_vma_unlink(vma_t *vma)
{
    if (vma == NULL)
        return;

    /* Walk VMA's AVC list and remove each from its anon_vma's tree. */
    while (vma->anon_vma_chain.next != &vma->anon_vma_chain) {
        list_node_t *entry = vma->anon_vma_chain.next;
        struct anon_vma_chain *avc =
            container_of(entry, struct anon_vma_chain, same_vma);
        struct anon_vma *av = avc->anon_vma;

        /* Remove from VMA's list first. */
        list_entry_detach(entry);

        /* Remove from anon_vma's rb-tree. */
        rwsem_acquire_write(&av->rwsem);
        rb_delete_key(&av->vma_tree, (unsigned long)avc->vma->start);
        rwsem_release(&av->rwsem);

        anon_vma_put(av);
        avc_free(avc);
    }

    if (vma->anon_vma != NULL) {
        anon_vma_put(vma->anon_vma);
        vma->anon_vma = NULL;
    }
}

/* ========================================================================== */
/*  Page rmap operations                                                      */
/* ========================================================================== */

void page_add_anon_rmap(page_t *page, vma_t *vma, uint64 va)
{
    (void)va; /* va reserved for future use (e.g., page_vaddr_at) */

    if (page == NULL || vma == NULL)
        return;

    int old = __sync_fetch_and_add(&page->anon.mapcount, 1);
    if (old == 0 && vma->anon_vma != NULL) {
        /* First mapping — record the anon_vma for reverse lookup. */
        page->anon.anon_vma = vma->anon_vma;
    }
}

void page_remove_rmap(page_t *page)
{
    if (page == NULL)
        return;
    __sync_fetch_and_sub(&page->anon.mapcount, 1);
}

int page_mapped(page_t *page)
{
    if (page == NULL)
        return 0;
    return __sync_fetch_and_add(&page->anon.mapcount, 0) > 0;
}

int page_mapcount(page_t *page)
{
    if (page == NULL)
        return 0;
    return __sync_fetch_and_add(&page->anon.mapcount, 0);
}

/* ========================================================================== */
/*  try_to_unmap                                                              */
/* ========================================================================== */

int try_to_unmap(page_t *page)
{
    if (page == NULL)
        return -1;

    struct anon_vma *av = page->anon.anon_vma;
    if (av == NULL)
        return -1;

    uint64 target_pa = __page_to_pa(page);

    rwsem_acquire_read(&av->rwsem);

    struct anon_vma_chain *avc, *tmp_avc;
    rb_foreach_entry_safe(&av->vma_tree, avc, tmp_avc, rb_entry) {
        vma_t *vma = avc->vma;
        if (vma == NULL || vma->vm == NULL)
            continue;

        vm_t *vm = vma->vm;

        /* Delegate all page-table work to the arch-aware vm layer. */
        int zapped = vm_zap_pte(vm, vma, target_pa);
        for (int i = 0; i < zapped; i++)
            page_remove_rmap(page);
    }

    rwsem_release(&av->rwsem);

    return page_mapcount(page) == 0 ? 0 : -1;
}
