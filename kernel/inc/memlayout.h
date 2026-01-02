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
#define NVIRTIO 2  // number of virtio disks

#define PCIE_ECAM 0x30000000L

// Intel E1000 Ethernet Controller interface
#define E1000_PCI_ADDR 0x40000000L
#define E1000_IRQ 33

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#ifdef HOST_TEST
// Make sure the whole memory area is in the user space when testing
#define KERNBASE 0x40000000L
#else
#define KERNBASE 0x80000000L
#endif
#define PHYSTOP (KERNBASE + 128*1024*1024)
#define TOTALPAGES  ((PHYSTOP - KERNBASE) >> 12)

// We keep the actual highest 1MB of physical memory to store symbols
#define KERNEL_SYMBOLS_START    PHYSTOP
#define KERNEL_SYMBOLS_SIZE     0x100000
#define KERNEL_SYMBOLS_END      (KERNEL_SYMBOLS_START + KERNEL_SYMBOLS_SIZE)
#define KERNEL_SYMBOLS_IDX_START (KERNEL_SYMBOLS_END + 0x1000)
#define KERNEL_SYMBOLS_IDX_SIZE (KERNEL_SYMBOLS_SIZE - 0x1000)
#define KERNEL_SYMBOLS_IDX_END (KERNEL_SYMBOLS_IDX_START + KERNEL_SYMBOLS_IDX_SIZE)
// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)
// Trampoline page for signal handling.
// SIG_TRAMPOLINE would be mapped to the user space
#define SIG_TRAMPOLINE (TRAMPOLINE - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (TRAMPOLINE - (PGSIZE << 1) - ((p)+1)* 2*PGSIZE)

#define UVMBOTTOM 0x1000L
#define UVMTOP (MAXVA - (1UL << 32))
#define USTACKTOP UVMTOP
#if UVMBOTTOM + (MAXUSTACK << PAGE_SHIFT) > USTACKTOP
#error "User stack too large"
#endif
// The lowest address of the user stack.
#define USTACK_MAX_BOTTOM   (USTACKTOP - (MAXUSTACK << PAGE_SHIFT))
#define UHEAP_MAX_TOP       (UVMBOTTOM + (MAXUHEAP << PAGE_SHIFT))

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   user stack
//   128MB padding
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   SIG_TRAMPOLINE (used by the signal handling code)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - (PGSIZE << 1))

#endif          /* __KERNEL_MEMORY_LAYOUT_H */
