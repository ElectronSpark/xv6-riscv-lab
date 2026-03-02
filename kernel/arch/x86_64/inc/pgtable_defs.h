/**
 * @file pgtable_defs.h
 * @brief x86-64 inline PTE helpers for the pgtable abstraction.
 *
 * Included by kernel/inc/mm/pgtable.h — never include this directly.
 *
 * x86-64 PTE layout (4-level page table: PML4 → PDPT → PD → PT):
 *   Bit  0     – Present (P)            ← PTE_V / PTE_R
 *   Bit  1     – Read/Write (R/W)       ← PTE_W
 *   Bit  2     – User/Supervisor (U/S)  ← PTE_U
 *   Bit  5     – Accessed (A)
 *   Bit  6     – Dirty (D)
 *   Bit  8     – Global (G)
 *   Bit 63     – No Execute (NX)
 *   Bits [51:12] – Physical address
 *
 * On x86 all present pages are implicitly readable (no separate R bit).
 * Execute permission is inverted: set NX to deny execution.
 */

#ifndef __ARCH_X86_PGTABLE_DEFS_H
#define __ARCH_X86_PGTABLE_DEFS_H

#include "types.h"
#include <mm/vm_types.h> /* PROT_READ, PROT_WRITE, PROT_EXEC, VMA_FLAG_USER */

/* ── Raw PTE bit definitions (private to this header + pgtable.c) ───────── */

#define __PTE_V (1ULL << 0)   /* Present */
#define __PTE_R (1ULL << 0)   /* Readable = Present on x86 */
#define __PTE_W (1ULL << 1)   /* Writable (R/W bit) */
#define __PTE_X 0             /* No explicit exec bit; use NX to deny */
#define __PTE_U (1ULL << 2)   /* User accessible (U/S bit) */
#define __PTE_A (1ULL << 5)   /* Accessed */
#define __PTE_D (1ULL << 6)   /* Dirty */
#define __PTE_NX (1ULL << 63) /* No Execute */

#define __PA2PTE(pa) ((uint64)(pa) & 0x000FFFFFFFFFF000ULL)
#define __PTE2PA(pte) ((uint64)(pte) & 0x000FFFFFFFFFF000ULL)
#define __PTE_FLAGS(pte) ((uint64)(pte) & ~0x000FFFFFFFFFF000ULL)

/* ── PTE queries ────────────────────────────────────────────────────────── */

/** Is the PTE valid/present? */
static inline int pte_present(pte_t *pte) { return (*pte & __PTE_V) != 0; }

/** Does the PTE grant write permission? */
static inline int pte_write(pte_t *pte) { return (*pte & __PTE_W) != 0; }

/** Is the PTE user-accessible? */
static inline int pte_user(pte_t *pte) { return (*pte & __PTE_U) != 0; }

/** Is the PTE dirty (has been written to)? */
static inline int pte_dirty(pte_t *pte) { return (*pte & __PTE_D) != 0; }

/** Set the dirty bit on a PTE. */
static inline void pte_mkdirty(pte_t *pte) { *pte |= __PTE_D; }

/** Clear the dirty bit on a PTE (keep all other bits). */
static inline void pte_mkclean(pte_t *pte) { *pte &= ~__PTE_D; }

/**
 * Is the PTE a non-leaf (directory/intermediate) entry?
 * On x86-64 intermediate entries have Present=1 but all permission bits
 * are propagated differently (U, W on intermediates enable leaf access).
 * For consistency with RISC-V, treat a PTE whose only flag-area bit is
 * Present as non-leaf.  In practice user PTEs in this codebase always
 * have at least U set on leaves.
 */
static inline int pte_nonleaf(pte_t *pte) {
    return __PTE_FLAGS(*pte) == __PTE_V;
}

/**
 * Is the PTE fully write-ready (writable, accessed, dirty)?
 */
static inline int pte_write_ready(pte_t *pte) {
    return (*pte & (__PTE_W | __PTE_A | __PTE_D)) ==
           (__PTE_W | __PTE_A | __PTE_D);
}

/** Extract the physical address stored in a PTE. */
static inline uint64 pte_pa(pte_t *pte) { return __PTE2PA(*pte); }

/* ── PTE construction ───────────────────────────────────────────────────── */

/**
 * mk_pte - Build a leaf PTE from a physical address and VMA-level flags.
 *
 * @pa:        physical address (page-aligned)
 * @vma_flags: combination of PROT_READ, PROT_WRITE, PROT_EXEC, VMA_FLAG_USER
 *
 * On x86, PROT_READ maps to Present (always set for a valid PTE),
 * PROT_WRITE maps to R/W, and PROT_EXEC is a no-op (NX is not set
 * because this codebase doesn't use NX for user pages).
 */
static inline pte_t mk_pte(uint64 pa, uint64 vma_flags) {
    pte_t pte = __PA2PTE(pa) | __PTE_V | __PTE_A | __PTE_D;
    if (vma_flags & PROT_READ)
        pte |= __PTE_R; /* __PTE_R == __PTE_V, but harmless to OR again */
    if (vma_flags & PROT_WRITE)
        pte |= __PTE_W;
    /* PROT_EXEC: on x86 __PTE_X == 0, so this is a no-op. */
    if (vma_flags & PROT_EXEC)
        pte |= __PTE_X;
    if (vma_flags & VMA_FLAG_USER)
        pte |= __PTE_U;
    return pte;
}

/* ── PTE modification ──────────────────────────────────────────────────── */

/** Clear the write-permission bit (COW write-protect). */
static inline void pte_wrprotect(pte_t *pte) { *pte &= ~__PTE_W; }

/** Zero the PTE entirely. */
static inline void pte_clear(pte_t *pte) { *pte = 0; }

/**
 * pte_modify - Rewrite a PTE keeping the same PA but applying new
 *              VMA-level permission flags.
 */
static inline void pte_modify(pte_t *pte, uint64 vma_flags) {
    uint64 pa = __PTE2PA(*pte);
    *pte = mk_pte(pa, vma_flags);
}

#endif /* __ARCH_X86_PGTABLE_DEFS_H */
