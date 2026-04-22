#ifndef __KERNEL_ARCH_CACHE_H
#define __KERNEL_ARCH_CACHE_H

#if defined(CONFIG_ARCH_RISCV) || (!defined(CONFIG_ARCH_X86_64) && defined(__riscv))
#include "../../../arch/riscv/inc/cache.h"
#elif defined(CONFIG_ARCH_X86_64) || defined(__x86_64__) || defined(__i386__)
#include "../../../arch/x86_64/inc/cache.h"
#else
#error "Unsupported architecture for cache operations"
#endif

#endif /* __KERNEL_ARCH_CACHE_H */
