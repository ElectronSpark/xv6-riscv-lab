/*
 * Minimal test environment for tmpfs truncate.c
 * 
 * This file provides all the types and stubs needed to compile truncate.c
 * for unit testing. It uses guards to prevent the kernel headers from
 * being included.
 */
#ifndef TMPFS_TEST_ENV_H
#define TMPFS_TEST_ENV_H

/* Block all kernel headers that would conflict - use EXACT guard names from kernel */
#define __KERNEL_TYPES_H
#define __KERNEL_RISCV_H
#define __KERNEL_DEFS_H
#define __KERNEL_PARAM_H
#define __ERRNO_H_
#define KERNEL_INC_BITS_H
#define __KERNEL_STAT_H
#define __KERNEL_SPINLOCK_H
#define __KERNEL_PROC_H
#define __KERNEL_VM_H
#define __KERNEL_MUTEX_TYPES_H
#define KERNEL_RWLOCK_H
#define __KERNEL_COMPLETION_H
#define __KERNEL_VIRTUAL_FILE_SYSTEM_FS_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_PRIVATE_H
#define __BI_DIRECTIONAL_H
#define __HASH_LIST_H__
#define __KERNEL_SLAB_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_TMPFS_PRIVATE_H
#define __HASH_LIST_TYPE_H__
#define KERNEL_VIRTUAL_FILE_SYSTEM_TYPES_H
#define __KERNEL_SIGNAL_TYPES_H
#define __KERNEL_COMPLETION_TYPES_H
#define __KERNEL_RWLOCK_TYPES_H
#define KERNEL_INC_ATOMIC_H
#define __KERNEL_COMPILER_H
#define KERNEL_OBJECT_H
#define __KERNEL_PCACHE_TYPES_H
#define __KERNEL_VFS_STAT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Basic types (matching kernel types.h)
 * ============================================================================ */
typedef unsigned char uint8;
typedef unsigned long uint64;
typedef long int64;
typedef int64 loff_t;

/* ============================================================================
 * Page constants (matching riscv.h/param.h)
 * ============================================================================ */
#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (PAGE_SIZE - 1)
#endif

/* ============================================================================
 * Error codes (matching errno.h)
 * ============================================================================ */
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EFBIG
#define EFBIG 27
#endif

/* ============================================================================
 * TMPFS constants (matching tmpfs_private.h)
 * ============================================================================ */
#define TMPFS_INODE_DBLOCKS 32UL
#define TMPFS_INODE_INDRECT_START TMPFS_INODE_DBLOCKS
#define TMPFS_INODE_INDRECT_ITEMS (PAGE_SIZE / sizeof(void *))
#define TMPFS_INODE_DINDRECT_START (TMPFS_INODE_INDRECT_START + TMPFS_INODE_INDRECT_ITEMS)
#define TMPFS_INODE_DINDRECT_ITEMS (TMPFS_INODE_INDRECT_ITEMS * TMPFS_INODE_INDRECT_ITEMS)
#define TMPFS_INODE_TINDRECT_START (TMPFS_INODE_DINDRECT_START + TMPFS_INODE_DINDRECT_ITEMS)
#define TMPFS_INODE_TINDRECT_ITEMS (TMPFS_INODE_DINDRECT_ITEMS * TMPFS_INODE_INDRECT_ITEMS)
#define TMPFS_MAX_FILE_SIZE ((TMPFS_INODE_TINDRECT_START + TMPFS_INODE_TINDRECT_ITEMS) * PAGE_SIZE)
#define TMPFS_IBLOCK(pos) ((pos) >> PAGE_SHIFT)
#define TMPFS_IBLOCK_OFFSET(pos) ((pos) & PAGE_MASK)

/* ============================================================================
 * Minimal structures for testing (matching vfs_types.h and tmpfs_private.h)
 * ============================================================================ */

/* Minimal vfs_inode - only fields used by truncate.c */
struct vfs_inode {
    loff_t size;
    int n_blocks;
    /* Padding to match kernel structure size if needed */
};

/* Minimal tmpfs_inode - matches kernel tmpfs_private.h */
struct tmpfs_inode {
    struct vfs_inode vfs_inode;
    bool embedded;
    union {
        union {
            struct {
                void *direct[TMPFS_INODE_DBLOCKS];
                void **indirect;
                void ***double_indirect;
                void ****triple_indirect;
            };
            uint8 data[0];
        } file;
    };
};

#define TMPFS_INODE_EMBEDDED_DATA_LEN   \
    (sizeof(struct tmpfs_inode) - offsetof(struct tmpfs_inode, file.data))

/* container_of macro */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* ============================================================================
 * Function stubs - to be implemented in test file or linked via --wrap
 * ============================================================================ */

/* Memory allocation - declare as extern, implement in test file */
extern void *kalloc(void);
extern void kfree(void *);

/* Panic handling */
#define ASSERTION_FAILURE "Assertion failure"
#define PANIC "Panic"

/* These will be defined in the test file */
extern void __panic_impl(const char *type, const char *fmt, ...);

#define __panic(type, fmt, ...) __panic_impl(type, fmt, ##__VA_ARGS__)
#define panic(fmt, ...) __panic(PANIC, fmt, ##__VA_ARGS__)
#define assert(expr, fmt, ...) \
    do { \
        if (!(expr)) { \
            __panic(ASSERTION_FAILURE, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#endif /* TMPFS_TEST_ENV_H */
