/**
 * sysmm.c - Memory management syscall handlers
 *
 * System call implementations for mmap, munmap, mprotect, mremap,
 * msync, mincore, and madvise.
 * These are thin wrappers that extract arguments from the trapframe
 * and dispatch to the corresponding vm.c functions.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "proc/thread.h"
#include <mm/vm.h>

// mmap(addr, length, prot, flags, fd, offset)
uint64 sys_mmap(void) {
    uint64 addr, offset;
    int length, prot, flags, fd;

    argaddr(0, &addr);
    argint(1, &length);
    argint(2, &prot);
    argint(3, &flags);
    argint(4, &fd);
    argaddr(5, &offset);

    return vm_mmap(current->vm, addr, (size_t)length, prot, flags, fd, offset);
}

// munmap(addr, length)
uint64 sys_munmap(void) {
    uint64 addr;
    int length;

    argaddr(0, &addr);
    argint(1, &length);

    if (length <= 0)
        return -EINVAL;

    return (uint64)vm_munmap(current->vm, addr, (size_t)length);
}

// mprotect(addr, length, prot)
uint64 sys_mprotect(void) {
    uint64 addr;
    int length, prot;

    argaddr(0, &addr);
    argint(1, &length);
    argint(2, &prot);

    if (length <= 0)
        return -EINVAL;

    return (uint64)vm_mprotect(current->vm, addr, (size_t)length, prot);
}

// mremap(old_addr, old_size, new_size, flags, new_addr)
uint64 sys_mremap(void) {
    uint64 old_addr, new_addr;
    int old_size, new_size, flags;

    argaddr(0, &old_addr);
    argint(1, &old_size);
    argint(2, &new_size);
    argint(3, &flags);
    argaddr(4, &new_addr);

    if (old_size < 0 || new_size <= 0)
        return -EINVAL;

    return vm_mremap(current->vm, old_addr, (size_t)old_size,
                     (size_t)new_size, flags, new_addr);
}

// msync(addr, length, flags)
uint64 sys_msync(void) {
    uint64 addr;
    int length, flags;

    argaddr(0, &addr);
    argint(1, &length);
    argint(2, &flags);

    if (length <= 0)
        return -EINVAL;

    return (uint64)vm_msync(current->vm, addr, (size_t)length, flags);
}

// mincore(addr, length, vec)
uint64 sys_mincore(void) {
    uint64 addr, vec_uaddr;
    int length;

    argaddr(0, &addr);
    argint(1, &length);
    argaddr(2, &vec_uaddr);

    if (length <= 0)
        return -EINVAL;

    size_t sz = (size_t)length;
    size_t num_pages = (PGROUNDUP(sz)) / PGSIZE;

    // Use a stack buffer, process in chunks of 256 pages
    unsigned char kbuf[256];
    size_t done = 0;
    while (done < num_pages) {
        size_t chunk = num_pages - done;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);

        int ret = vm_mincore(current->vm, addr + done * PGSIZE,
                             chunk * PGSIZE, kbuf);
        if (ret < 0)
            return (uint64)ret;

        if (vm_copyout(current->vm, vec_uaddr + done,
                       (void *)kbuf, chunk) < 0)
            return (uint64)(-EFAULT);

        done += chunk;
    }
    return 0;
}

// madvise(addr, length, advice)
uint64 sys_madvise(void) {
    uint64 addr;
    int length, advice;

    argaddr(0, &addr);
    argint(1, &length);
    argint(2, &advice);

    if (length <= 0)
        return -EINVAL;

    return (uint64)vm_madvise(current->vm, addr, (size_t)length, advice);
}
