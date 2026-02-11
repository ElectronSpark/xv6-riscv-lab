// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio disk
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel
#include "param.h"
#include "riscv.h"

#ifndef __KERNEL_MEMORY_LAYOUT_H
#define __KERNEL_MEMORY_LAYOUT_H

// ============================================================================
// Embedded kernel symbols
// ============================================================================
// The symbol table is embedded directly in the kernel image (.ksymbols
// section). These linker symbols mark the boundaries:
//   _ksymbols_start / _ksymbols_end   - Raw symbol data (text format)
//   _ksymbols_idx_start / _ksymbols_idx_end - Parsed index (rb-tree nodes)
//   _kernel_image_end - End of loaded kernel image (before BSS)

extern char _ksymbols_start[];
extern char _ksymbols_end[];
extern char _ksymbols_idx_start[];
extern char _ksymbols_idx_end[];
extern char _kernel_image_end[];

// Raw symbol data embedded in kernel
#define KERNEL_SYMBOLS_START ((uint64)_ksymbols_start)
#define KERNEL_SYMBOLS_END ((uint64)_ksymbols_end)
#define KERNEL_SYMBOLS_SIZE (KERNEL_SYMBOLS_END - KERNEL_SYMBOLS_START)

// Parsed symbol index (for rb-tree nodes)
#define KERNEL_SYMBOLS_IDX_START ((uint64)_ksymbols_idx_start)
#define KERNEL_SYMBOLS_IDX_END ((uint64)_ksymbols_idx_end)
#define KERNEL_SYMBOLS_IDX_SIZE                                                \
    (KERNEL_SYMBOLS_IDX_END - KERNEL_SYMBOLS_IDX_START)

// End of kernel image (before BSS) - used for memory calculations
#define KERNEL_IMAGE_END ((uint64)_kernel_image_end)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80200000 to PHYSTOP.
extern uint64 __physical_memory_start;
extern uint64 __physical_memory_end;
extern uint64 __physical_total_pages;
#ifdef HOST_TEST
// Make sure the whole memory area is in the user space when testing
#define KERNBASE 0x40000000L
#else
// KERNBASE is a runtime variable set from FDT at boot time
// For user-space, use kernbase() syscall to get the actual value
#define KERNBASE __physical_memory_start
#endif
#define PHYSTOP __physical_memory_end
#define TOTALPAGES __physical_total_pages

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)
#define TRAMPOLINE_DATA (TRAMPOLINE - PGSIZE)
// Trampoline page for signal handling.
// percpu data for trampoline and kernel.
#define TRAMPOLINE_CPULOCAL (TRAMPOLINE - (PGSIZE * 2))
// SIG_TRAMPOLINE would be mapped to the user space
#define SIG_TRAMPOLINE (TRAMPOLINE - (PGSIZE * 3))

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KIRQSTACKTOP (MAXVA - (PGSIZE << 6))
#define KIRQSTACK(hartid)                                                      \
    (KIRQSTACKTOP -                                                            \
     ((hartid) + 1) *                                                          \
         (INTR_STACK_SIZE << 1)) // each stack has guard pages above and below

#if NCPU > 64
#error "NCPU too large"
#endif

#define UVMBOTTOM 0x1000L
// Leave the top-level PTE containing TRAMPOLINE identical to kernel page table
// Index 255 covers 0x3FC0000000 to 0x3FFFFFFFFF (last 1 GiB of 256 GiB address
// space)
#define UVMTOP                                                                 \
    (TRAMPOLINE & ~((1UL << 30) - 1)) // Start of the shared 1 GiB region

// TRAPFRAME must be below UVMTOP (outside the shared region)
// so it can be mapped per-thread.
#define TRAPFRAME                                                              \
    (UVMTOP - (PGSIZE << 6)) // Leave space for 64 trapframes (one per CPU)
#define TRAPFRAME_POFFSET                                                      \
    ((PAGE_SIZE - sizeof(struct thread) - sizeof(struct utrapframe) - 16) &    \
     ~0x7UL)
#define USTACKTOP (TRAPFRAME - PGSIZE) // Guard page between stack and trapframe

#if UVMBOTTOM + MAXUSTACK > USTACKTOP
#error "User stack too large"
#endif
// The lowest address of the user stack.
#define USTACK_MAX_BOTTOM (USTACKTOP - (MAXUSTACK << PAGE_SHIFT))
#define UHEAP_MAX_TOP (UVMBOTTOM + (MAXUHEAP << PAGE_SHIFT))

#if KIRQSTACK(64) < UVMTOP
#error "Not enough space for kernel stacks"
#endif

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   user stack
//   guard page
//   TRAPFRAME * 64 (per CPU, mapped the last page of kernel stack, used by the
//   trampoline)
// --- UVMTOP boundary (last PTE shared with kernel) ---
//   SIG_TRAMPOLINE (used by the signal handling code)
//   CPU_LOCAL (per-cpu data, used by trampoline and kernel)
//   TRAMPOLINE_DATA (global data for trampoline code)
//   TRAMPOLINE (the same page as in the kernel)

#endif /* __KERNEL_MEMORY_LAYOUT_H */
