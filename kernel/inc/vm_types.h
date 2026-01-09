#ifndef __KERNEL_VM_TYPES_H
#define __KERNEL_VM_TYPES_H

#include "types.h"
#include "riscv.h"
#include "page_type.h"
#include "list_type.h" 
#include "bintree_type.h"

typedef struct vm vm_t;
struct file;

typedef struct vma {
    struct rb_node  rb_entry;   // Red-black tree node for managing VM areas
    list_node_t     list_entry;
    list_node_t     free_list_entry; // For managing free VM areas
    vm_t            *vm;        // Pointer to the VM structure this area belongs to
    uint64          start;
    uint64          end;
    uint64          flags;  // Flags for the memory area (e.g., read, write, execute)
    struct file     *file;  // File associated with this memory area, if any
    uint64          pgoff;  // Offset in the file for this memory area
} vma_t;

#define VM_FLAG_NONE        0x0     // not accessible
#define VM_FLAG_READ        0x1
#define VM_FLAG_WRITE       0x2
#define VM_FLAG_EXEC        0x4
#define VM_FLAG_USERMAP     0x8     // User-mapped page
#define VM_FLAG_FWRITE      0x20    // File-backed writable
#define VM_FLAG_GROWSDOWN   0x100   // Grow downwards
#define VM_FLAG_GROWSUP     0x200   // Grow upwards

#define VM_FLAG_PROT_MASK  (VM_FLAG_READ | VM_FLAG_WRITE |      \
                            VM_FLAG_EXEC | VM_FLAG_USERMAP |    \
                            VM_FLAG_FWRITE |      \
                            VM_FLAG_GROWSDOWN | VM_FLAG_GROWSUP)

// Virtual Memory Management structure
typedef struct vm {
    pagetable_t     pagetable;
    struct rb_root  vm_tree;
    bool            valid;
    uint64          trapframe; // Pointer to the trap frame for this VM
    vma_t           *stack;
    size_t          stack_size; // Size of the stack area
    vma_t           *heap;
    size_t          heap_size;  // Size of the heap area
    list_node_t     vm_list;  // List of VM areas
    list_node_t     vm_free_list;  // List of free VM areas
} vm_t;

#endif // __KERNEL_VM_TYPES_H
