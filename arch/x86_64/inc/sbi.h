#ifndef __KERNEL_ARCH_X86_SBI_H
#define __KERNEL_ARCH_X86_SBI_H

#include "types.h"

#define SBI_SUCCESS 0

static inline void sbi_probe_extensions(void) {}
static inline void sbi_start_secondary_harts(unsigned long start_addr) {
    (void)start_addr;
}

static inline void sbi_console_putchar(int c) { (void)c; }
static inline void sbi_console_puts(const char *s) { (void)s; }
static inline int sbi_console_getchar(void) { return -1; }

static inline void sbi_remote_hfence_i(unsigned long hart_mask,
                                       unsigned long hart_mask_base) {
    (void)hart_mask;
    (void)hart_mask_base;
}

static inline long sbi_remote_hfence_vma(unsigned long hart_mask,
                                         unsigned long hart_mask_base,
                                         unsigned long start_addr,
                                         unsigned long size) {
    (void)hart_mask;
    (void)hart_mask_base;
    (void)start_addr;
    (void)size;
    return SBI_SUCCESS;
}

#endif /* __KERNEL_ARCH_X86_SBI_H */
