/**
 * @file pgtable.h
 * @brief Architecture-independent page-table abstraction.
 *
 * This header provides a uniform API for all page-table and PTE operations.
 * The shared VM code (kernel/mm/vm.c) includes this header instead of
 * architecture-specific headers and NEVER references raw PTE flag values
 * (PTE_V, PTE_W, PA2PTE, …) directly.
 *
 * Inline PTE helpers are provided by the architecture:
 *   kernel/arch/riscv/inc/pgtable_defs.h
 *   kernel/arch/x86_64/inc/pgtable_defs.h
 *
 * Non-inline functions (walk, mappages, …) are implemented in
 * kernel/mm/pgtable.c.
 */

#ifndef __MM_PGTABLE_H
#define __MM_PGTABLE_H

#include "types.h"

/* ========================================================================== */
/*  Architecture-specific inline PTE helpers */
/* ========================================================================== */
/*
 * Each arch header defines:
 *
 *   Query:
 *     pte_present(pte_t *p)       – is the PTE valid/present?
 *     pte_write(pte_t *p)         – does the PTE allow writes?
 *     pte_user(pte_t *p)          – is the PTE user-accessible?
 *     pte_nonleaf(pte_t *p)       – valid PTE with no R/W/X (directory entry)?
 *     pte_write_ready(pte_t *p)   – writable AND accessed AND dirty?
 *     pte_pa(pte_t *p)            – extract physical address from leaf PTE
 *
 *   Construction:
 *     mk_pte(uint64 pa, uint64 vma_flags)
 *       Build a new leaf PTE.  @vma_flags uses PROT_READ/PROT_WRITE/PROT_EXEC
 *       and VMA_FLAG_USER.  PTE_V, PTE_A, PTE_D are set automatically.
 *
 *   Modification:
 *     pte_wrprotect(pte_t *p)     – clear write permission (for COW)
 *     pte_clear(pte_t *p)         – zero the PTE
 *     pte_modify(pte_t *p, uint64 vma_flags)
 *       Rewrite the PTE keeping the same PA but applying new permissions.
 */

#if defined(CONFIG_ARCH_RISCV)
#include "../../../arch/riscv/inc/pgtable_defs.h"
#elif defined(CONFIG_ARCH_X86_64)
#include "../../../arch/x86_64/inc/pgtable_defs.h"
#else
/* Compiler-based fallback (same as kernel/inc/riscv.h) */
#if defined(__riscv)
#include "../../../arch/riscv/inc/pgtable_defs.h"
#elif defined(__x86_64__) || defined(__i386__)
#include "../../../arch/x86_64/inc/pgtable_defs.h"
#else
#error "Unsupported architecture for pgtable abstraction"
#endif
#endif

/* ========================================================================== */
/*  Page-table page allocator */
/* ========================================================================== */

pde_t *pgtab_alloc(void);
void pgtab_free(void *pa);

/* ========================================================================== */
/*  Page-table walking */
/* ========================================================================== */

/**
 * walk - Walk a multi-level page table and return a pointer to the leaf PTE
 *        for virtual address @va.  Allocates intermediate page-table pages
 *        when @alloc is set.
 *
 * @retl2, @retl1: optional out-pointers to intermediate PTE slots (used by
 *                 arch-specific code; shared code passes NULL).
 *
 * Returns NULL if the mapping does not exist and @alloc is 0.
 */
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc, pte_t **retl2,
            pte_t **retl1);

/**
 * walkaddr - Look up a user virtual address and return its physical address,
 *            or 0 if not mapped or not user-accessible.
 */
uint64 walkaddr(pagetable_t pagetable, uint64 va);

/* ========================================================================== */
/*  Page-table bulk mapping / unmapping */
/* ========================================================================== */

/**
 * kvmmap - Map a region in a kernel page table (panic on failure).
 *          Used only during boot; does not flush TLB.
 *
 * @perm: raw PTE permission flags (arch-specific; callers are arch code).
 */
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm);

/**
 * mappages - Create leaf PTEs mapping [@va, @va+@size) → [@pa, @pa+@size).
 *            Both @va and @size must be page-aligned.
 *
 * @perm: raw PTE permission flags (arch-specific; callers are arch code
 *        or vm_mmap_region_locked which obtains them via vma2pte_flags).
 *
 * Returns 0 on success, negative errno on failure.
 */
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
             int perm);

/**
 * uvmunmap - Remove @npages of mappings starting from @va.
 *            Panics if a mapping does not exist.
 *
 * @do_free: if set, the physical page is freed via pgtab_free.
 */
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free);

/* ========================================================================== */
/*  2MB huge-page support                                                     */
/* ========================================================================== */

/**
 * walk_pmd - Walk to page-directory (level 1) and return a pointer to the
 *            PMD entry for @va.  Allocates upper-level tables when @alloc
 *            is set, but never allocates a level-0 page table.
 */
pte_t *walk_pmd(pagetable_t pagetable, uint64 va, int alloc);

/**
 * map_hugepage - Create a single 2MB huge-page mapping.
 *                Both @va and @pa must be 2MB-aligned.
 *
 * @perm: raw PTE permission flags (arch-specific).
 * Returns 0 on success, negative errno on failure.
 */
int map_hugepage(pagetable_t pagetable, uint64 va, uint64 pa, int perm);

/**
 * unmap_hugepage - Remove a single 2MB huge-page mapping.
 *                  @va must be 2MB-aligned.
 *
 * @do_free: if set, the physical page group is freed.
 */
void unmap_hugepage(pagetable_t pagetable, uint64 va, int do_free);

/* ========================================================================== */
/*  PTE ↔ VMA flag conversion                                                */
/* ========================================================================== */

/**
 * vma2pte_flags - Convert VMA protection flags (PROT_READ, PROT_WRITE,
 *                 PROT_EXEC, VMA_FLAG_USER) to architecture-specific PTE
 *                 permission bits.
 */
uint64 vma2pte_flags(uint64 vma_flags);

/**
 * pte2vma_flags - Convert architecture-specific PTE permission bits back
 *                 to VMA protection flags.
 */
uint64 pte2vma_flags(pte_t *pte);

/* ========================================================================== */
/*  PTE zapping (used by rmap) */
/* ========================================================================== */

/**
 * vm_zap_pte - Clear all PTEs in @vma's range that map @target_pa.
 *
 * Acquires vm->spinlock internally; caller must NOT hold it.
 * Returns the number of PTEs cleared.
 */
struct vm;
struct vma;
int vm_zap_pte(struct vm *vm, struct vma *vma, uint64 target_pa);

#endif /* __MM_PGTABLE_H */
