/*
 * Minimal test environment for tmpfs truncate.c
 * 
 * This file provides all the types and stubs needed to compile truncate.c
 * for unit testing. It uses guards to prevent the kernel headers from
 * being included.
 *
 * Since the bmap has been replaced with pcache, this header provides mock
 * pcache types and function declarations.
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
#define __KERNEL_VFS_STAT_H
#define KERNEL_VIRTUAL_FILE_SYSTEM_STAT_H
/* Block pcache headers — we provide mock types below */
#define __KERNEL_PAGE_CACHE_H__
#define KERNEL_PAGE_CACHE_TYPES_H

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
/* Kernel uses PGSIZE; alias it to PAGE_SIZE for the test environment */
#ifndef PGSIZE
#define PGSIZE PAGE_SIZE
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
 * Mock pcache types — just enough for truncate.c to compile
 * ============================================================================ */

/* Forward declarations */
struct pcache;
struct pcache_node;

/* Minimal page_t with pcache extension */
typedef struct page_struct {
    struct {
        struct pcache_node *pcache_node;
    } pcache;
} page_t;

/* Minimal pcache_node — only the data pointer is used by truncate.c */
struct pcache_node {
    void *data;
};

/* Minimal pcache structure — only 'active' flag is checked by truncate.c */
struct pcache {
    int active;
};

/* pcache function declarations (implementations in the test .c file) */
page_t *pcache_get_page(struct pcache *pcache, uint64 blkno);
void pcache_put_page(struct pcache *pcache, page_t *page);
int pcache_read_page(struct pcache *pcache, page_t *page);
int pcache_mark_page_dirty(struct pcache *pcache, page_t *page);
int pcache_discard_blk(struct pcache *pcache, uint64 blkno);
void pcache_teardown(struct pcache *pcache);

/* ============================================================================
 * TMPFS constants (matching kernel tmpfs_private.h — pcache model)
 * ============================================================================ */
#define TMPFS_MAX_FILE_SIZE ((uint64)1 * 1024 * 1024 * 1024)
#define TMPFS_IBLOCK(pos)        ((pos) >> PAGE_SHIFT)
#define TMPFS_IBLOCK_OFFSET(pos) ((pos) & PAGE_MASK)

/* ============================================================================
 * Minimal structures for testing (matching vfs_types.h and tmpfs_private.h)
 * ============================================================================ */

/* Minimal vfs_inode — includes i_data pcache used by truncate.c */
struct vfs_inode {
    loff_t size;
    int n_blocks;
    struct pcache i_data;
};

/* Minimal tmpfs_inode — matches the simplified kernel tmpfs_private.h
 * (no bmap; non-embedded data lives entirely in i_data pcache).
 * The dir variant is the largest union member in the kernel and determines
 * the amount of space available for embedded file data.  We replicate the
 * size here with a padding array so TMPFS_INODE_EMBEDDED_DATA_LEN is the
 * same as in the kernel (~288 bytes). */
struct tmpfs_inode {
    struct vfs_inode vfs_inode;
    bool embedded;
    union {
        char _dir_padding[288]; /* same size as kernel dir { hlist_t + buckets[15] } */
        union {
            char *symlink_target;
            char data[0];
        } sym;
        union {
            uint8 data[0]; /* embedded data for small files */
        } file;
    };
};

#define TMPFS_INODE_EMBEDDED_DATA_LEN   \
    (sizeof(struct tmpfs_inode) - offsetof(struct tmpfs_inode, sym.data))

/* container_of macro */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* ============================================================================
 * Function stubs — to be implemented in the test .c file
 * ============================================================================ */

/* Panic handling */
#define ASSERTION_FAILURE "Assertion failure"
#define PANIC "Panic"

extern void __panic_impl(const char *type, const char *fmt, ...);

#define __panic(type, fmt, ...) __panic_impl(type, fmt, ##__VA_ARGS__)
#define panic(fmt, ...) __panic(PANIC, fmt, ##__VA_ARGS__)
#define assert(expr, fmt, ...) \
    do { \
        if (!(expr)) { \
            __panic(ASSERTION_FAILURE, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

/* tmpfs pcache lifecycle — declared in tmpfs_private.h, mocked in test */
extern void tmpfs_inode_pcache_init(struct vfs_inode *inode);
extern void tmpfs_inode_pcache_teardown(struct vfs_inode *inode);

#endif /* TMPFS_TEST_ENV_H */
