#ifndef __KERNEL_VM_TYPES_H
#define __KERNEL_VM_TYPES_H

#include "types.h"
#include "riscv.h"
#include <mm/page_type.h>
#include "list_type.h"
#include "bintree_type.h"
#include "maple_tree_type.h"
#include "lock/rwsem_types.h"
#include <vfs/vfs_types.h>

struct anon_vma;

typedef struct vm vm_t;

typedef struct vma {
    vm_t *vm; // Pointer to the VM structure this area belongs to
    uint64 start;
    uint64 end;
    uint64 flags; // Flags for the memory area (e.g., read, write, execute)
    struct vfs_file *file; // File associated with this memory area
    uint64 pgoff;          // Offset in the file for this memory area
    struct anon_vma *anon_vma;   // rmap: this VMA's own anon_vma
    list_node_t anon_vma_chain;  // rmap: list of anon_vma_chain nodes
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
#define MAP_SHARED_VALIDATE 0x03
#define MAP_TYPE 0x0f
#define MAP_FIXED 0x10     // Interpret addr exactly
#define MAP_ANONYMOUS 0x20 // Don't use a file
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_NONBLOCK 0x10000
#define MAP_STACK 0x20000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FILE 0

// VMA flags (stored in vma->flags, xv6-specific, high bits avoid PROT_*
// conflict)
#define VMA_FLAG_USER 0x08       // User-accessible mapping
#define VMA_FLAG_GROWSDOWN 0x100 // Stack-like region (grows down)
#define VMA_FLAG_GROWSUP 0x200   // Heap-like region (grows up)
#define VMA_FLAG_FILE 0x400      // File-backed mapping
#define VMA_FLAG_SHARED 0x800    // Shared mapping (MAP_SHARED)
#define VMA_FLAG_KERNEL 0x1000   // Kernel-space mapping (no lazy alloc, no COW)
#define VMA_FLAG_DONTFORK 0x2000 // Exclude mapping from forked children
#define VMA_FLAG_DONTDUMP 0x4000 // Exclude mapping from core dumps
#define VMA_FLAG_WIPEONFORK 0x8000 // Zero child pages after fork
#define VMA_FLAG_MERGEABLE 0x10000 // KSM-compatible advisory
#define VMA_FLAG_HUGEPAGE 0x20000  // Transparent hugepage advisory
#define VMA_FLAG_NOHUGEPAGE 0x40000 // Disable transparent hugepage advisory

#define VMA_FLAG_ADVICE_MASK                                                  \
    (VMA_FLAG_DONTFORK | VMA_FLAG_DONTDUMP | VMA_FLAG_WIPEONFORK |            \
     VMA_FLAG_MERGEABLE | VMA_FLAG_HUGEPAGE | VMA_FLAG_NOHUGEPAGE)

// Combined mask of all bits that may appear in vma->flags
#define VMA_FLAG_PROT_MASK                                                     \
    (PROT_READ | PROT_WRITE | PROT_EXEC | VMA_FLAG_USER | VMA_FLAG_GROWSDOWN | \
     VMA_FLAG_GROWSUP | VMA_FLAG_FILE | VMA_FLAG_SHARED | VMA_FLAG_KERNEL)

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
#define MADV_REMOVE 9
#define MADV_DONTFORK 10
#define MADV_DOFORK 11
#define MADV_MERGEABLE 12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE 14
#define MADV_NOHUGEPAGE 15
#define MADV_DONTDUMP 16
#define MADV_DODUMP 17
#define MADV_WIPEONFORK 18
#define MADV_KEEPONFORK 19
#define MADV_COLD 20
#define MADV_PAGEOUT 21
#define MADV_POPULATE_READ 22
#define MADV_POPULATE_WRITE 23
#define MADV_DONTNEED_LOCKED 24
#define MADV_COLLAPSE 25

// Virtual Memory Management structure
typedef struct vm {
    rwsem_t rw_lock; // protect the vm maple tree
    struct maple_tree vm_mt; // Maple tree of VM areas (keyed by [start, end-1])
    pte_t *trapframe_pte; // Pointer to the leaf page table for trapframes
    vma_t *stack;
    size_t stack_size; // Size of the stack area
    vma_t *heap;
    size_t heap_size;         // Size of the heap area
    uint64 heap_reserve_end;  // Upper bound of heap reservation (mmap avoids)
    cpumask_t cpumask;        // CPUs using this VM

    spinlock_t spinlock; // Spinlock for protecting the pagetable
    pagetable_t pagetable;
    int refcount; // Reference count
    uint64 vm_bottom;    // Lowest VA managed by this VM
    uint64 vm_top;       // Highest VA (exclusive) managed by this VM
    int is_kernel;       // Non-zero if this is the kernel VM singleton
    uint16 asid;       // ASID (RISC-V) / PCID (x86_64), 0 = kernel
    uint16 asid_gen;   // Generation when ASID was assigned
} vm_t;

#endif // __KERNEL_VM_TYPES_H
