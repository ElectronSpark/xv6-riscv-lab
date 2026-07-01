#ifndef __KERNEL_X86_64_VDSO_H
#define __KERNEL_X86_64_VDSO_H

#include "types.h"

struct vm;

int x86_vdso_map(struct vm *vm, uint64 *ehdr);

#endif /* __KERNEL_X86_64_VDSO_H */
