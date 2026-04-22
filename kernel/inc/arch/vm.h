#ifndef _ARCH_VM_H_
#define _ARCH_VM_H_

#include "types.h"

// Generic PTE flags (to be mapped to arch-specific flags)
#define ARCH_PTE_V    (1L << 0) // Valid
#define ARCH_PTE_R    (1L << 1) // Read
#define ARCH_PTE_W    (1L << 2) // Write
#define ARCH_PTE_X    (1L << 3) // Execute
#define ARCH_PTE_U    (1L << 4) // User

// Forward declarations
struct vm;

// Initialize architecture-specific virtual memory
void arch_vm_init(void);

// Initialize virtual memory for the current CPU/hart
void arch_vm_init_hart(void);

// Map a virtual address to a physical address
int arch_vm_map(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm);

// Flush the TLB
void arch_tlb_flush(void);

// Create an empty user page table (may include arch-specific shared mappings
// such as the RISC-V trampoline PTE).
pagetable_t uvmcreate(void);

// Recursively free page-table pages (all leaf mappings must already be removed).
void freewalk(pagetable_t pagetable);

// Free user memory pages then page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz);

// Set up trampoline / trapframe page table entries for a new process VM.
// Returns 0 on success, negative errno on failure.
int arch_vm_setup_trampoline(struct vm *vm);

// Tear down trampoline / trapframe page table entries during VM destruction.
void arch_vm_teardown_trampoline(struct vm *vm);

// Dump page table entries (arch-specific formatting).
void dump_pagetable(pagetable_t pagetable, int level, int indent,
                    uint64 va_base, uint64 va_end, bool omit_pa);

#endif // _ARCH_VM_H_