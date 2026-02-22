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

#define TRAMPOLINE (MAXVA - PGSIZE)
#define TRAMPOLINE_DATA (TRAMPOLINE - PGSIZE)
#define TRAMPOLINE_CPULOCAL (TRAMPOLINE - (PGSIZE * 2))
#define SIG_TRAMPOLINE (TRAMPOLINE - (PGSIZE * 3))

#define KIRQSTACKTOP (MAXVA - (PGSIZE << 6))
#define KIRQSTACK(hartid)                                                      \
	(KIRQSTACKTOP - ((hartid) + 1) * (INTR_STACK_SIZE << 1))

#if NCPU > 64
#error "NCPU too large"
#endif

#define UVMTOP (TRAMPOLINE & ~((1UL << 30) - 1))
#define TRAPFRAME (UVMTOP - (PGSIZE << 6))
#define TRAPFRAME_POFFSET                                                      \
	((PAGE_SIZE - sizeof(struct thread) - sizeof(struct utrapframe) - 16) &    \
	 ~0x7UL)
#define USTACKTOP (TRAPFRAME - PGSIZE)

#define USTACK_MAX_BOTTOM (USTACKTOP - (MAXUSTACK << PAGE_SHIFT))
#define UHEAP_MAX_TOP (UVMBOTTOM + (MAXUHEAP << PAGE_SHIFT))

#endif /* __KERNEL_X86_64_MEMORY_LAYOUT_H */