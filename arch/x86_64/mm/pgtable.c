/**
 * @file pgtable.c
 * @brief x86-64 page-table walking, mapping, and flag-conversion.
 *
 * Architecture-specific implementation of the pgtable abstraction
 * declared in <mm/pgtable.h>.  Uses raw x86-64 PTE macros from x86.h.
 */

#include "types.h"
#include "param.h"
#include "x86.h" /* PTE_*, PA2PTE, PTE2PA, PX, PGSIZE, PAGETABLE_LEVELS */
#include "defs.h"
#include <mm/pgtable.h>
#include <mm/vm.h>
#include <mm/vm_types.h>
#include <mm/page.h>
#include <mm/rmap.h>
#include "string.h"
#include "printf.h"
#include "errno.h"

/* ========================================================================== */
/*  Page-table page allocator */
/* ========================================================================== */

/*
 * pgtab_alloc — allocate a zeroed page-table page.
 *
 * Returns a PHYSICAL ADDRESS (from page_alloc).  The returned pointer
 * is usable directly because the PML4[0] identity map keeps PA == VA
 * for the low address range.  walk() stores this PA into parent PTEs
 * via PA2PTE() — no VA2PA() conversion needed.
 */
pde_t *pgtab_alloc(void) {
    void *pa = page_alloc(0, PAGE_TYPE_PGTABLE);
    if (pa) {
        memset(pa, 0, PGSIZE);
    }
    return (pde_t *)pa;
}

void pgtab_free(void *pa) { page_free(pa, 0); }

/* ========================================================================== */
/*  x86-64 4-level page-table walking */
/* ========================================================================== */

/*
 * Walk the 4-level x86-64 page table and return a pointer to the leaf PTE
 * for virtual address @va.  If @alloc is set, intermediate page-table
 * pages are allocated as needed.
 */
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc, pte_t **retl2,
            pte_t **retl1) {
    assert(VA_IS_VALID(va), "walk: va not canonical");
    assert(pagetable != NULL, "walk: pagetable is null");

    pte_t *ret_pte[PAGETABLE_LEVELS];
    for (int i = 0; i < PAGETABLE_LEVELS; i++)
        ret_pte[i] = NULL;

    for (int level = PAGETABLE_LEVELS - 1; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        ret_pte[level] = pte;
        assert(pte != NULL, "walk: pte is null");
        if (*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc || (pagetable = (pde_t *)pgtab_alloc()) == 0)
                return NULL;
            memset(pagetable, 0, PGSIZE);
            /*
             * On x86-64 intermediate PTEs (PML4E, PDPTE, PDE) must carry
             * U and W bits so user pages beneath them are accessible.
             * WALK_INTERMEDIATE_FLAGS = PTE_U | PTE_W.
             */
            *pte = PA2PTE(pagetable) | PTE_V | WALK_INTERMEDIATE_FLAGS;
        }
    }

    if (retl2 && PAGETABLE_LEVELS > 2)
        *retl2 = ret_pte[PAGETABLE_LEVELS > 3 ? 3 : 2];
    if (retl1 && PAGETABLE_LEVELS > 1)
        *retl1 = ret_pte[PAGETABLE_LEVELS > 3 ? 2 : 1];

    return &pagetable[PX(0, va)];
}

/*
 * Look up a user virtual address and return its physical address,
 * or 0 if not mapped or not user-accessible.
 */
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
    pte_t *pte;
    uint64 pa;

    if (!VA_IS_VALID(va))
        return 0;

    pte = walk(pagetable, va, 0, NULL, NULL);
    if (pte == 0)
        return 0;
    if ((*pte & PTE_V) == 0)
        return 0;
    if ((*pte & PTE_U) == 0)
        return 0;
    pa = PTE2PA(*pte);
    return pa;
}

/* ========================================================================== */
/*  Bulk mapping / unmapping */
/* ========================================================================== */

void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm) {
    if (mappages(kpgtbl, va, sz, pa, perm) != 0)
        panic("kvmmap");
}

int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
             int perm) {
    uint64 a, last;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("mappages: va not aligned, va %p", (void *)va);
    if ((size % PGSIZE) != 0)
        panic("mappages: size not aligned, va %p, size %p", (void *)va,
              (void *)size);
    if (size == 0)
        panic("mappages: size zero, va %p", (void *)va);

    a = va;
    last = va + size - PGSIZE;
    for (;;) {
        if ((pte = walk(pagetable, a, 1, NULL, NULL)) == 0)
            return -ENOMEM;
        if (*pte & PTE_V)
            panic(
                "mappages: remap, va=%p pa=%p existing_pte=%p (pa=%p flags=%p)",
                (void *)a, (void *)pa, (void *)*pte, (void *)PTE2PA(*pte),
                (void *)PTE_FLAGS(*pte));
        *pte = PA2PTE(pa) | perm | PTE_V | PTE_A | PTE_D;
        if (a == last)
            break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
    uint64 a;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
        if ((pte = walk(pagetable, a, 0, NULL, NULL)) == 0)
            panic("uvmunmap: walk");
        if ((*pte & PTE_V) == 0)
            panic("uvmunmap: not mapped, va=%p, pa=%p, flags: %llx", (void *)a,
                  (void *)PTE2PA(*pte), (unsigned long long)PTE_FLAGS(*pte));
        if (PTE_FLAGS(*pte) == PTE_V)
            panic("uvmunmap: not a leaf");
        uint64 pa = PTE2PA(*pte);
        *pte = 0;
        if (do_free) {
            pgtab_free((void *)pa);
        }
    }
}

/* ========================================================================== */
/*  PTE ↔ VMA flag conversion                                                */
/* ========================================================================== */

uint64 vma2pte_flags(uint64 flags) {
    uint64 pte_flags = 0;
    if (flags & PROT_READ)
        pte_flags |= PTE_R;
    if (flags & PROT_WRITE)
        pte_flags |= PTE_W;
    if (flags & PROT_EXEC)
        pte_flags |= PTE_X;
    if (flags & VMA_FLAG_USER)
        pte_flags |= PTE_U;
    else
        pte_flags |= PTE_G;  /* Kernel pages are Global (not flushed on CR3 switch) */
    return pte_flags;
}

uint64 pte2vma_flags(pte_t *pte) {
    uint64 flags = 0;
    uint64 pte_val = *pte;
    if (pte_val & PTE_R)
        flags |= PROT_READ;
    if (pte_val & PTE_W)
        flags |= PROT_WRITE;
    if (pte_val & PTE_X)
        flags |= PROT_EXEC;
    if (pte_val & PTE_U)
        flags |= VMA_FLAG_USER;
    return flags;
}

/* ========================================================================== */
/*  PTE zapping (used by rmap) */
/* ========================================================================== */

int vm_zap_pte(vm_t *vm, vma_t *vma, uint64 target_pa) {
    if (vm == NULL || vma == NULL || vm->pagetable == NULL)
        return 0;

    int zapped = 0;
    pagetable_t pgtable = vm->pagetable;

    for (uint64 va = vma->start; va < vma->end; va += PGSIZE) {
        pte_t *pte = walk(pgtable, va, 0, NULL, NULL);
        if (pte == NULL || (*pte & PTE_V) == 0)
            continue;
        if (PTE2PA(*pte) != target_pa)
            continue;

        vm_pgtable_lock(vm);
        if ((*pte & PTE_V) && PTE2PA(*pte) == target_pa) {
            *pte = 0;
            sfence_vma_page(va);
            zapped++;
        }
        vm_pgtable_unlock(vm);
        vm_remote_sfence(vm);
    }

    return zapped;
}
