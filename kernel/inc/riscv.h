#ifndef __KERNEL_RISCV_COMPAT_H
#define __KERNEL_RISCV_COMPAT_H

#if defined(CONFIG_ARCH_RISCV)
#include "../../arch/riscv/inc/riscv.h"
#elif defined(CONFIG_ARCH_X86_64)
#include "../../arch/x86_64/inc/x86.h"
#else
#if defined(__riscv)
#include "../../arch/riscv/inc/riscv.h"
#elif defined(__x86_64__) || defined(__i386__)
#include "../../arch/x86_64/inc/x86.h"
#else
#error "Unsupported architecture for riscv compatibility header"
#endif
#endif

#endif /* __KERNEL_RISCV_COMPAT_H */