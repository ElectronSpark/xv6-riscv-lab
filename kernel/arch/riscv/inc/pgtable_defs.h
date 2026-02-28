/**
 * @file pgtable_defs.h
 * @brief RISC-V Sv39 inline PTE helpers for the pgtable abstraction.
 *
 * Included by kernel/inc/mm/pgtable.h — never include this directly.
 *
 * RISC-V Sv39 PTE layout (64-bit, 3-level page table):
 *   Bits [63:54]  – reserved
 *   Bits [53:10]  – PPN (44 bits)
 *   Bits  [9:8]   – RSW (software, available)
 *   Bit    7      – Dirty (D)
 *   Bit    6      – Accessed (A)
 *   Bit    5      – Global (G)
 *   Bit    4      – User (U)
 *   Bit    3      – eXecute (X)
 *   Bit    2      – Write (W)
 *   Bit    1      – Read (R)
 *   Bit    0      – Valid (V)
 *
 * A non-leaf PTE has R=W=X=0 and points to the next level table.
 */

#ifndef __ARCH_RISCV_PGTABLE_DEFS_H
#define __ARCH_RISCV_PGTABLE_DEFS_H

#include "types.h"
#include <mm/vm_types.h> /* PROT_READ, PROT_WRITE, PROT_EXEC, VMA_FLAG_USER */

/* ── Raw PTE bit definitions (private to this header + pgtable.c) ───────── */

#define __PTE_V (1L << 0)
#define __PTE_R (1L << 1)
#define __PTE_W (1L << 2)
#define __PTE_X (1L << 3)
#define __PTE_U (1L << 4)
#define __PTE_A (1L << 6)
#define __PTE_D (1L << 7)

#define __PA2PTE(pa) ((((uint64)(pa)) >> 12) << 10)
#define __PTE2PA(pte) (((pte) >> 10) << 12)
#define __PTE_FLAGS(pte) ((pte) & 0x3FF & (~(__PTE_A | __PTE_D)))

/* ── PTE queries ────────────────────────────────────────────────────────── */

/** Is the PTE valid/present? */
static inline int pte_present(pte_t *pte) { return (*pte & __PTE_V) != 0; }

/** Does the PTE grant write permission? */
static inline int pte_write(pte_t *pte) { return (*pte & __PTE_W) != 0; }

/** Is the PTE user-accessible? */
static inline int pte_user(pte_t *pte) { return (*pte & __PTE_U) != 0; }

/**
 * Is the PTE a non-leaf (directory/intermediate) entry?
 * On RISC-V Sv39 a non-leaf PTE has V=1 but R=W=X=0.
 */
static inline int pte_nonleaf(pte_t *pte) {
    return __PTE_FLAGS(*pte) == __PTE_V;
}

/**
 * Is the PTE fully write-ready (writable, accessed, dirty)?
 * If all three are set the page can be written without a fault.
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
 * Sets V, A, D automatically so the page is immediately usable.
 */
static inline pte_t mk_pte(uint64 pa, uint64 vma_flags) {
    pte_t pte = __PA2PTE(pa) | __PTE_V | __PTE_A | __PTE_D;
    if (vma_flags & PROT_READ)
        pte |= __PTE_R;
    if (vma_flags & PROT_WRITE)
        pte |= __PTE_W;
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

#endif /* __ARCH_RISCV_PGTABLE_DEFS_H */
