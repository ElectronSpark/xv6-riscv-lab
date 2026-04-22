#ifndef __KERNEL_ARCH_MEMLAYOUT_H
#define __KERNEL_ARCH_MEMLAYOUT_H

#if defined(CONFIG_ARCH_RISCV) || (!defined(CONFIG_ARCH_X86_64) && defined(__riscv))
#include "../../../arch/riscv/inc/memlayout.h"
#elif defined(CONFIG_ARCH_X86_64) || defined(__x86_64__) || defined(__i386__)
#include "../../../arch/x86_64/inc/memlayout.h"
#else
#error "Unsupported architecture: missing CONFIG_ARCH_* define"
#endif

#endif /* __KERNEL_ARCH_MEMLAYOUT_H */