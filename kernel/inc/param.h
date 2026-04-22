#ifndef __KERNEL_PARAM_H
#define __KERNEL_PARAM_H

#include "compiler.h"

// Console device major and minor numbers
#define CONSOLE_MAJOR 1
#define CONSOLE_MINOR 1
#define NULL_MAJOR 1
#define NULL_MINOR 2
#define RANDOM_MAJOR 1
#define RANDOM_MINOR 3
#define ZERO_MAJOR 1
#define ZERO_MINOR 4

// TTY / PTY device major and minor numbers
#define TTY_DEV_MAJOR 5
#define TTY_DEV_MINOR 1
#define PTMX_MAJOR    5
#define PTMX_MINOR    2
#define PTY_MAJOR 136
#define PTY_MINOR_BASE 0

#define MAXPID 0x7FFFFFF0 // maximum process ID
#define NR_THREAD 10000   // maximum number of threads
// #define NR_THREAD       0xFFFFFFFFFFFFFFUL
#define NCPU 8              // maximum number of CPUs
#define NOFILE 512          // open files per thread
#define NFILE 512           // open files per system
#define NINODE 50           // maximum number of active i-nodes
#define NDEV 10             // maximum major device number
#define ROOTDEV mkdev(2, 1) // device number of file system root disk (virtio)
#define XV6FS_TEST_DEV mkdev(2, 2) // device number of xv6fs test disk (second virtio)
#define RAMDISK_DEV mkdev(3, 1)   // device number of ramdisk
#define MAXARG 32                 // max exec arguments
#define MAXENV 64                 // max exec environment variables
#define MAXOPBLOCKS 80            // max # of blocks any FS op writes
#define LOGSIZE (MAXOPBLOCKS * 3) // max data blocks in on-disk log
#define NBUF (MAXOPBLOCKS * 300)  // size of disk block cache
#define FSSIZE 200000             // size of file system in blocks
#define MAXPATH 128               // maximum file path name

#define USERSTACK_MINSZ (PAGE_SIZE << 2) // minimum user stack size
#define USERSTACK 128                    // user stack pages (512 KiB)
#define USERSTACK_GROWTH 32              // user stack growth pages
#define MAXUSTACK (1UL << 11) // maximum number of pages in user stack (8 MiB)
#define MAXUHEAP (1UL << 24) // maximum number of pages in user heap

// Number of pages reserved above heap start for brk/sbrk growth.
// mmap(addr=0) will not allocate within this range.
// Each process gets its own reservation based on heap_start.
#define HEAP_RESERVE_PAGES (1UL << 16)  // 64K pages = 256 MiB

// The base(2) order of the size of a page in bytes
#ifndef PAGE_SHIFT
#ifdef PGSHIFT
#define PAGE_SHIFT PGSHIFT
#else
#define PAGE_SHIFT 12
#endif /* PGSHIFT */
#endif /* PAGE_SHIFT */

// The size of a page in bytes
#ifndef PAGE_SIZE
#ifdef PGSIZE
#define PAGE_SIZE PGSIZE
#else
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#endif /* PGSIZE */
#endif /* PAGE_SIZE */

// Mask to get the offset in bytes in a page
#ifndef PAGE_MASK
#define PAGE_MASK (PAGE_SIZE - 1)
#endif /* PAGE_MASK */

// Default kernel stack size
#define KERNEL_STACK_ORDER 2 // kernel stack size is 16KB
#define KERNEL_STACK_SIZE                                                      \
    (1UL << (PAGE_SHIFT + KERNEL_STACK_ORDER)) // kernel stack size in bytes
#define INTR_STACK_ORDER 2                     // interrupt stack size is 16KB
#define INTR_STACK_SIZE                                                        \
    (1UL << (PAGE_SHIFT + INTR_STACK_ORDER)) // interrupt stack size in bytes
#define TRAPFRAME_ORDER 0                    // trapframe size is 4KB
#define TRAPFRAME_MAPSIZE                                                      \
    (1UL << (PAGE_SHIFT + TRAPFRAME_ORDER)) // trapframe size in bytes

#define BACKTRACE_MAX_DEPTH 32 // maximum depth of backtrace

#endif /* __KERNEL_PARAM_H */
