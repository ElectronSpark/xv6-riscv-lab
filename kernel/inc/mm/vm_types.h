#ifndef __KERNEL_VM_TYPES_H
#define __KERNEL_VM_TYPES_H

#include "types.h"
#include "riscv.h"
#include <mm/page_type.h>
#include "list_type.h"
#include "bintree_type.h"
#include "lock/rwsem_types.h"
#include <vfs/vfs_types.h>

typedef struct vm vm_t;

typedef struct vma {
    struct rb_node rb_entry; // Red-black tree node for managing VM areas
    list_node_t list_entry;
    list_node_t free_list_entry; // For managing free VM areas
    vm_t *vm; // Pointer to the VM structure this area belongs to
    uint64 start;
    uint64 end;
    uint64 flags; // Flags for the memory area (e.g., read, write, execute)
    struct vfs_file *file; // File associated with this memory area
    uint64 pgoff;          // Offset in the file for this memory area
} vma_t;

/*
 * VMA protection flags (POSIX-compatible)
 * These match the POSIX mmap PROT_* and MAP_* flags for compatibility.
 */

// Protection flags (POSIX PROT_*)
#define PROT_NONE 0x0       // Page cannot be accessed
#define PROT_READ 0x1       // Page can be read
#define PROT_WRITE 0x2      // Page can be written
#define PROT_EXEC 0x4       // Page can be executed
#define PROT_GROWSUP 0x40   // Heap-like region (grows up)
#define PROT_GROWSDOWN 0x80 // Stack-like region (grows down)
#define PROT_MASK                                                              \
    (PROT_NONE | PROT_READ | PROT_WRITE | PROT_EXEC | PROT_GROWSUP |           \
     PROT_GROWSDOWN)

// Mapping flags (POSIX MAP_*)
#define MAP_SHARED 0x01    // Share changes
#define MAP_PRIVATE 0x02   // Changes are private
#define MAP_FIXED 0x10     // Interpret addr exactly
#define MAP_ANONYMOUS 0x20 // Don't use a file
#define MAP_ANON MAP_ANONYMOUS

// VMA flags (stored in vma->flags, xv6-specific, high bits avoid PROT_*
// conflict)
#define VMA_FLAG_USER 0x08       // User-accessible mapping
#define VMA_FLAG_GROWSDOWN 0x100 // Stack-like region (grows down)
#define VMA_FLAG_GROWSUP 0x200   // Heap-like region (grows up)
#define VMA_FLAG_FILE 0x400      // File-backed mapping

// Combined mask of all bits that may appear in vma->flags
#define VMA_FLAG_PROT_MASK                                                     \
    (PROT_READ | PROT_WRITE | PROT_EXEC | VMA_FLAG_USER | VMA_FLAG_GROWSDOWN | \
     VMA_FLAG_GROWSUP | VMA_FLAG_FILE)

// mmap failure return value
#define MAP_FAILED ((void *)(uint64) - 1)

// mremap flags (POSIX-compatible)
#define MREMAP_MAYMOVE 1 // May move the mapping to a new address
#define MREMAP_FIXED 2 // Use specified new address (must also specify MAYMOVE)

// msync flags (POSIX-compatible)
#define MS_ASYNC 1      // Schedule sync, return immediately
#define MS_SYNC 4       // Synchronous sync
#define MS_INVALIDATE 2 // Invalidate cached data

// madvise advice flags (POSIX-compatible)
#define MADV_NORMAL 0     // No special treatment
#define MADV_RANDOM 1     // Expect random page references
#define MADV_SEQUENTIAL 2 // Expect sequential page references
#define MADV_WILLNEED 3   // Will need these pages soon
#define MADV_DONTNEED 4   // Don't need these pages anymore
#define MADV_FREE 8       // Pages can be freed (if not dirty)

// Virtual Memory Management structure
typedef struct vm {
    rwsem_t rw_lock; // protect the vm tree and vma list
    struct rb_root vm_tree;
    pte_t *trapframe_pte; // Pointer to the leaf page table for trapframes
    vma_t *stack;
    size_t stack_size; // Size of the stack area
    vma_t *heap;
    size_t heap_size;         // Size of the heap area
    list_node_t vm_list;      // List of VM areas
    list_node_t vm_free_list; // List of free VM areas
    cpumask_t cpumask;        // CPUs using this VM

    spinlock_t spinlock; // Spinlock for protecting the pagetable
    pagetable_t pagetable;
    int refcount; // Reference count
} vm_t;

#endif // __KERNEL_VM_TYPES_H
