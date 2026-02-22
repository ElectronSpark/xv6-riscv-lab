#ifndef __KERNEL_SBI_COMPAT_H
#define __KERNEL_SBI_COMPAT_H

#if defined(CONFIG_ARCH_RISCV) || (!defined(CONFIG_ARCH_X86_64) && defined(__riscv))
#include "../arch/riscv/inc/sbi.h"
#elif defined(CONFIG_ARCH_X86_64) || defined(__x86_64__) || defined(__i386__)
#include "../arch/x86_64/inc/sbi.h"
#else
#error "Unsupported architecture for SBI compatibility"
#endif

#endif /* __KERNEL_SBI_COMPAT_H */