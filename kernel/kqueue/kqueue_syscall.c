/*
 * kqueue_syscall.c - System call wrappers for kqueue
 *
 * SYS_kqueue          → sys_kqueue()
 * SYS_kevent_register → sys_kevent_register()
 * SYS_kevent_wait     → sys_kevent_wait()
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "vfs/file.h"
#include "vfs/vfs_types.h"
#include "kqueue.h"
#include "kqueue_types.h"
#include <mm/vm.h>

/* Max events per syscall to bound kernel-side allocation */
#define KEVENT_MAX_BATCH 256

/*
 * sys_kqueue - create a new kqueue file descriptor
 *
 * Takes no arguments.
 * Returns: fd on success, negative errno on failure.
 */
uint64 sys_kqueue(void) {
    return (uint64)kqueue_create();
}

/*
 * sys_kevent_register - register/modify/delete events on a kqueue
 *
 * a0: int kqfd          - kqueue file descriptor
 * a1: struct kevent *changelist - array of changes
 * a2: int nchanges      - number of changes
 *
 * Returns: 0 on success, negative errno on failure.
 */
uint64 sys_kevent_register(void) {
    int kqfd;
    uint64 changelist_addr;
    int nchanges;

    argint(0, &kqfd);
    argaddr(1, &changelist_addr);
    argint(2, &nchanges);

    if (nchanges < 0 || nchanges > KEVENT_MAX_BATCH)
        return (uint64)-EINVAL;
    if (nchanges == 0)
        return 0;

    /* Resolve kqueue fd → kqueue struct */
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, kqfd);
    if (f == NULL)
        return (uint64)-EBADF;
    struct kqueue *kq = (struct kqueue *)f->private_data;
    if (kq == NULL) {
        vfs_fput(f);
        return (uint64)-EBADF;
    }

    /* Copy changelist from user space */
    size_t bytes = (size_t)nchanges * sizeof(struct kevent);
    struct kevent *changelist = kmm_alloc(bytes);
    if (changelist == NULL) {
        vfs_fput(f);
        return (uint64)-ENOMEM;
    }

    if (vm_copyin(current->vm, (char *)changelist, changelist_addr, bytes) !=
        0) {
        kmm_free(changelist);
        vfs_fput(f);
        return (uint64)-EFAULT;
    }

    int ret = kqueue_register(kq, changelist, nchanges);

    /* Copy back any EV_ERROR entries */
    if (ret == 0) {
        vm_copyout(current->vm, changelist_addr, (char *)changelist, bytes);
    }

    kmm_free(changelist);
    vfs_fput(f);
    return (uint64)ret;
}

/*
 * sys_kevent_wait - wait for events on a kqueue
 *
 * a0: int kqfd          - kqueue file descriptor
 * a1: struct kevent *eventlist  - output event array
 * a2: int nevents       - max events to return
 * a3: int timeout_ms    - timeout: -1=block, 0=poll, >0=ms
 *
 * Returns: number of events, or negative errno.
 */
uint64 sys_kevent_wait(void) {
    int kqfd;
    uint64 eventlist_addr;
    int nevents;
    int timeout_ms;

    argint(0, &kqfd);
    argaddr(1, &eventlist_addr);
    argint(2, &nevents);
    argint(3, &timeout_ms);

    if (nevents <= 0 || nevents > KEVENT_MAX_BATCH)
        return (uint64)-EINVAL;

    /* Resolve kqueue fd → kqueue struct */
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, kqfd);
    if (f == NULL)
        return (uint64)-EBADF;
    struct kqueue *kq = (struct kqueue *)f->private_data;
    if (kq == NULL) {
        vfs_fput(f);
        return (uint64)-EBADF;
    }

    /* Allocate kernel buffer for events */
    size_t bytes = (size_t)nevents * sizeof(struct kevent);
    struct kevent *eventlist = kmm_alloc(bytes);
    if (eventlist == NULL) {
        vfs_fput(f);
        return (uint64)-ENOMEM;
    }

    int ret = kqueue_wait(kq, eventlist, nevents, timeout_ms);

    /* Copy events out to user space */
    if (ret > 0) {
        size_t out_bytes = (size_t)ret * sizeof(struct kevent);
        if (vm_copyout(current->vm, eventlist_addr, (char *)eventlist,
                       out_bytes) != 0) {
            ret = -EFAULT;
        }
    }

    kmm_free(eventlist);
    vfs_fput(f);
    return (uint64)ret;
}
