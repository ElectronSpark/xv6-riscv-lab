#ifndef _ARCH_TRAP_H_
#define _ARCH_TRAP_H_

// Architecture-specific trapframe structure
struct trapframe;

// Initialize architecture-specific trap handling
void arch_trap_init(void);

// Initialize trap handling for the current CPU/hart
void arch_trap_init_hart(void);

#endif // _ARCH_TRAP_H_