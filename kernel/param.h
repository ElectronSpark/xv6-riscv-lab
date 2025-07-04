#ifndef __KERNEL_PARAM_H
#define __KERNEL_PARAM_H

#include "compiler.h"

#ifdef LAB_FS
#define NPROC        10  // maximum number of processes
#else
#define NPROC        64  // maximum number of processes (speedsup bigfile)
#endif
#define NCPU          8  // maximum number of CPUs
#define NOFILE       64  // open files per process
#define NFILE       256  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*30)  // size of disk block cache
#ifdef LAB_FS
#define FSSIZE       200000  // size of file system in blocks
#else
#ifdef LAB_LOCK
#define FSSIZE       10000  // size of file system in blocks
#else
#define FSSIZE       2000   // size of file system in blocks
#endif
#endif
#define MAXPATH      128   // maximum file path name

#ifdef LAB_UTIL
#define USERSTACK    2     // user stack pages
#else
#define USERSTACK    1     // user stack pages
#endif

// The base(2) order of the size of a page in bytes
#ifndef PAGE_SHIFT
#ifdef PGSHIFT
#define PAGE_SHIFT                  PGSHIFT
#else
#define PAGE_SHIFT                  12
#endif          /* PGSHIFT */
#endif          /* PAGE_SHIFT */

// The size of a page in bytes
#ifndef PAGE_SIZE
#ifdef PGSIZE
#define PAGE_SIZE                   PGSIZE
#else
#define PAGE_SIZE                   (1UL << PAGE_SHIFT)
#endif          /* PGSIZE */
#endif          /* PAGE_SIZE */

// Mask to get the offset in bytes in a page
#ifndef PAGE_MASK
#define PAGE_MASK                   (PAGE_SIZE - 1)
#endif          /* PAGE_MASK */

#define KERNEL_STACK_ORDER 1 // kernel stack size is 8KB
#define KERNEL_STACK_SIZE (1UL << (PAGE_SHIFT + KERNEL_STACK_ORDER)) // kernel stack size in bytes
#define TRAPFRAME_ORDER 0 // trapframe size is 4KB
#define TRAPFRAME_SIZE (1UL << (PAGE_SHIFT + TRAPFRAME_ORDER)) // trapframe size in bytes

#endif              /* __KERNEL_PARAM_H */
