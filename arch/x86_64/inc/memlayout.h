#ifndef __KERNEL_X86_64_MEMORY_LAYOUT_H
#define __KERNEL_X86_64_MEMORY_LAYOUT_H

#include "param.h"
#include "types.h"

extern char _ksymbols_start[];
extern char _ksymbols_end[];
extern char _ksymbols_idx_start[];
extern char _ksymbols_idx_end[];
extern char _kernel_image_end[];

#define KERNEL_SYMBOLS_START ((uint64)_ksymbols_start)
#define KERNEL_SYMBOLS_END ((uint64)_ksymbols_end)
#define KERNEL_SYMBOLS_SIZE (KERNEL_SYMBOLS_END - KERNEL_SYMBOLS_START)

#define KERNEL_SYMBOLS_IDX_START ((uint64)_ksymbols_idx_start)
#define KERNEL_SYMBOLS_IDX_END ((uint64)_ksymbols_idx_end)
#define KERNEL_SYMBOLS_IDX_SIZE                                                \
	(KERNEL_SYMBOLS_IDX_END - KERNEL_SYMBOLS_IDX_START)

#define KERNEL_IMAGE_END ((uint64)_kernel_image_end)

extern uint64 __physical_memory_start;
extern uint64 __physical_memory_end;
extern uint64 __physical_total_pages;

#ifdef HOST_TEST
#define KERNBASE 0x40000000L
#else
#define KERNBASE __physical_memory_start
#endif
#define PHYSTOP __physical_memory_end
#define TOTALPAGES __physical_total_pages

#define UVMBOTTOM 0x1000L

#define TRAMPOLINE (MAXVA & ~(PGSIZE - 1))
#define TRAMPOLINE_DATA (TRAMPOLINE - PGSIZE)
#define TRAMPOLINE_CPULOCAL (TRAMPOLINE - (PGSIZE * 2))

/*
 * Signal trampoline: mapped at PML4[510] so it has its own shared
 * PML4 entry (separate from PML4[511] which holds TRAMPOLINE etc.).
 * PML4[510] covers VA 0xFFFFFF0000000000 .. 0xFFFFFF7FFFFFFFFF.
 */
#define SIG_TRAMPOLINE 0xFFFFFF0000000000UL

/*
 * CPU entry area: GDT+TSS (page 0) and IDT (page 1) mapped at
 * high-canonical addresses accessible under user CR3 (via PML4[511]).
 * This is needed because iretq must read the GDT to validate CS/SS,
 * and the IDT/TSS must be reachable for fault delivery.
 */
#define TRAPVEC_ALIAS_BASE (TRAMPOLINE_CPULOCAL - (4 * PGSIZE))
#define CPU_ENTRY_AREA     (TRAPVEC_ALIAS_BASE - (2 * PGSIZE))
#define CPU_ENTRY_GDT      CPU_ENTRY_AREA
#define CPU_ENTRY_IDT      (CPU_ENTRY_AREA + PGSIZE)

#define KIRQSTACKTOP (CPU_ENTRY_AREA - ((PGSIZE << 6) - PGSIZE))
#define KIRQSTACK(hartid)                                                      \
	(KIRQSTACKTOP - ((hartid) + 1) * (INTR_STACK_SIZE << 1))

#if MAX_CPUS > 64
#error "MAX_CPUS too large"
#endif

/*
 * x86_64 user virtual address space ends at the top of low canonical range.
 * With 4-level paging (48-bit VA): user space is 0 .. 0x00007FFFFFFFFFFF.
 * TRAMPOLINE etc. live in the high canonical range (shared via PML4[511]).
 * SIG_TRAMPOLINE lives in PML4[510].  Neither is part of user VA.
 */
#define UVMTOP 0x0000800000000000UL
#define TRAPFRAME (UVMTOP - (PGSIZE << 6))
#define TRAPFRAME_POFFSET                                                      \
	((PAGE_SIZE - sizeof(struct thread) - sizeof(struct utrapframe) - 16) &    \
	 ~0x7UL)
#define USTACKTOP (TRAPFRAME - PGSIZE)

#define USTACK_MAX_BOTTOM (USTACKTOP - (MAXUSTACK << PAGE_SHIFT))
#define UHEAP_MAX_TOP (UVMBOTTOM + (MAXUHEAP << PAGE_SHIFT))

// Kernel VM address range.
// On x86_64 the kernel identity-maps all physical memory starting at PA 0.
// The kernel VM covers from PGSIZE (skipping the NULL guard page) up to the
// end of the low canonical half. The trampoline / CPU-entry areas in the
// high canonical half remain managed separately — they are shared with
// every user page table via the top-level PML4 entry.
#define KVMBASE PGSIZE
#define KVMTOP  UVMTOP

#endif /* __KERNEL_X86_64_MEMORY_LAYOUT_H */
