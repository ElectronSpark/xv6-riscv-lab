#ifndef __KERNEL_ARCH_TRAPFRAME_H
#define __KERNEL_ARCH_TRAPFRAME_H

#if defined(CONFIG_ARCH_RISCV) || (!defined(CONFIG_ARCH_X86_64) && defined(__riscv))
#include "../../../arch/riscv/inc/trapframe.h"
#elif defined(CONFIG_ARCH_X86_64) || defined(__x86_64__) || defined(__i386__)
#include "../../../arch/x86_64/inc/trapframe.h"
#else
#error "Unsupported architecture: missing CONFIG_ARCH_* define"
#endif

#endif /* __KERNEL_ARCH_TRAPFRAME_H */