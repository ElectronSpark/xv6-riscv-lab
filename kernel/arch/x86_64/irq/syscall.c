#include "types.h"
#include "proc/thread.h"
#include "mm/vm.h"

int fetchaddr(uint64 addr, uint64 *ip) {
    struct thread *p = current;
    if (!p || !p->vm) {
        return -1;
    }
    return vm_copyin(p->vm, (char *)ip, addr, sizeof(*ip));
}

int fetchstr(uint64 addr, char *buf, int max) {
    struct thread *p = current;
    if (!p || !p->vm) {
        return -1;
    }
    return vm_copyinstr(p->vm, buf, addr, max) < 0 ? -1 : 0;
}

void argaddr(int n, uint64 *ip) {
    (void)n;
    if (ip) {
        *ip = 0;
    }
}

void argint(int n, int *ip) {
    (void)n;
    if (ip) {
        *ip = 0;
    }
}

void argint64(int n, int64 *ip) {
    (void)n;
    if (ip) {
        *ip = 0;
    }
}

int argstr(int n, char *buf, int max) {
    (void)n;
    if (!buf || max <= 0) {
        return -1;
    }
    buf[0] = '\0';
    return -1;
}
