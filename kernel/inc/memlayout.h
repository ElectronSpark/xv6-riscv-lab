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

#define CLINT_TIMER_IRQ 5

// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio interface
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1
#define VIRTIO1 0x10002000
#define VIRTIO1_IRQ 2
#define VIRTIO2 0x10003000  // virtio console
#define VIRTIO2_IRQ 3
#define NVIRTIO 2  // number of virtio disks

#define PCIE_ECAM 0x30000000L

// Intel E1000 Ethernet Controller interface
#define E1000_PCI_ADDR 0x40000000L
#define E1000_IRQ 33

// Goldfish RTC (Real Time Clock)
#define GOLDFISH_RTC 0x101000L
#define GOLDFISH_RTC_IRQ 11

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80200000 to PHYSTOP.
#ifdef HOST_TEST
// Make sure the whole memory area is in the user space when testing
#define KERNBASE 0x40000000L
#else
#define KERNBASE 0x80000000L
#endif
#define PHYSTOP (KERNBASE + 128*1024*1024)
#define TOTALPAGES  ((PHYSTOP - KERNBASE) >> 12)

// We keep the actual highest 1MB of physical memory to store symbols
#define KERNEL_SYMBOLS_START    0x88200000LL
#define KERNEL_SYMBOLS_SIZE     0x100000
#define KERNEL_SYMBOLS_END      (KERNEL_SYMBOLS_START + KERNEL_SYMBOLS_SIZE)
#define KERNEL_SYMBOLS_IDX_START KERNEL_SYMBOLS_END
#define KERNEL_SYMBOLS_IDX_SIZE 0x80000
#define KERNEL_SYMBOLS_IDX_END (KERNEL_SYMBOLS_IDX_START + KERNEL_SYMBOLS_IDX_SIZE)
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
#define KIRQSTACK(hartid) (KIRQSTACKTOP - ((hartid)+1)*(INTR_STACK_SIZE << 1)) // each stack has guard pages above and below

#if NCPU > 64
#error "NCPU too large"
#endif

#define UVMBOTTOM 0x1000L
// Leave the top-level PTE containing TRAMPOLINE identical to kernel page table
// Index 255 covers 0x3FC0000000 to 0x3FFFFFFFFF (last 1 GiB of 256 GiB address space)
#define UVMTOP (TRAMPOLINE & ~((1UL << 30) - 1))   // Start of the shared 1 GiB region

// TRAPFRAME must be below UVMTOP (outside the shared region)
// so it can be mapped per-process.
#define TRAPFRAME (UVMTOP - PGSIZE)
#define USTACKTOP (TRAPFRAME - PGSIZE)  // Guard page between stack and trapframe

#if UVMBOTTOM + MAXUSTACK > USTACKTOP
#error "User stack too large"
#endif
// The lowest address of the user stack.
#define USTACK_MAX_BOTTOM   (USTACKTOP - (MAXUSTACK << PAGE_SHIFT))
#define UHEAP_MAX_TOP       (UVMBOTTOM + (MAXUHEAP << PAGE_SHIFT))

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
//   TRAPFRAME (p->trapframe, used by the trampoline)
// --- UVMTOP boundary (last PTE shared with kernel) ---
//   SIG_TRAMPOLINE (used by the signal handling code)
//   CPU_LOCAL (per-cpu data, used by trampoline and kernel)
//   TRAMPOLINE_DATA (global data for trampoline code)
//   TRAMPOLINE (the same page as in the kernel)

#endif          /* __KERNEL_MEMORY_LAYOUT_H */
