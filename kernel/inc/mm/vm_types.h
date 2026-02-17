#ifndef __KERNEL_VM_TYPES_H
#define __KERNEL_VM_TYPES_H

#include "types.h"
#include "riscv.h"
#include <mm/page_type.h>
#include "list_type.h"
#include "bintree_type.h"
#include "lock/rwsem_types.h"
#include <vfs/vfs_types.h>
#include <uabi/mman.h>

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
 * VMA protection flags
 * Base POSIX-compatible PROT_* and MAP_* flags are defined in uabi/mman.h.
 */

// Kernel-only protection extensions
#define PROT_GROWSUP 0x40   // Heap-like region (grows up)
#define PROT_GROWSDOWN 0x80 // Stack-like region (grows down)
#define PROT_MASK                                                              \
    (PROT_NONE | PROT_READ | PROT_WRITE | PROT_EXEC | PROT_GROWSUP |           \
     PROT_GROWSDOWN)

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
