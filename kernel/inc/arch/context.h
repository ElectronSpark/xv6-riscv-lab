#ifndef _ARCH_CONTEXT_H_
#define _ARCH_CONTEXT_H_

// Architecture-specific context structure
struct context;

// Perform an architecture-specific context switch
void arch_context_switch(struct context **old, struct context *new);

#endif // _ARCH_CONTEXT_H_