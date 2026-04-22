/**
 * @file shm.c
 * @brief System V shared-memory implementation.
 *
 * Provides shmget / shmat / shmdt / shmctl.
 *
 * Each shared-memory segment is backed by a set of kalloc()'d physical
 * pages.  shmat() maps them into the calling process's VM via
 * vm_mmap_region(), shmdt() unmaps via vm_munmap_region().
 */

#include "types.h"
#include "string.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include "ipc.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "mm/vm.h"
#include <mm/pgtable.h>
#include <mm/page.h>
#include "timer/goldfish_rtc.h"

/* ── Internal segment descriptor ── */

struct shm_segment {
    struct shmid_ds ds;
    void **pages;    /* dynamically-allocated array of kalloc'd PA pointers */
    int    npages;
};

static struct ipc_ids shm_ids;

void ipc_shm_init(void)
{
    ipc_ids_init(&shm_ids, "ipc_shm");
}

/* ================================================================== */
/*  shmget                                                            */
/* ================================================================== */

uint64 sys_shmget(void)
{
    int key, shmflg;
    uint64 size;
    argint(0, &key);
    argaddr(1, &size);
    argint(2, &shmflg);

    if (size == 0 || size > SHMMAX)
        return (uint64)-EINVAL;

    int irq = spin_lock_irqsave(&shm_ids.lock);

    /* If key != IPC_PRIVATE, check for existing segment */
    if (key != IPC_PRIVATE) {
        int idx = ipc_findkey(&shm_ids, key);
        if (idx >= 0) {
            if (shmflg & IPC_CREAT && shmflg & IPC_EXCL) {
                spin_unlock_irqrestore(&shm_ids.lock, irq);
                return (uint64)-EEXIST;
            }
            /* Return existing id */
            int id = ipc_buildid(idx, shm_ids.entries[idx].seq);
            spin_unlock_irqrestore(&shm_ids.lock, irq);
            return (uint64)id;
        }
        /* Not found — must have IPC_CREAT to create */
        if (!(shmflg & IPC_CREAT)) {
            spin_unlock_irqrestore(&shm_ids.lock, irq);
            return (uint64)-ENOENT;
        }
    }

    /* Allocate a new segment */
    int npages = (int)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    struct shm_segment *seg = (struct shm_segment *)kvmalloc(sizeof(*seg));
    if (seg == NULL) {
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return (uint64)-ENOMEM;
    }
    memset(seg, 0, sizeof(*seg));
    seg->npages = npages;

    /* Allocate page-pointer array */
    seg->pages = (void **)kvmalloc((size_t)npages * sizeof(void *));
    if (seg->pages == NULL) {
        kvfree(seg);
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return (uint64)-ENOMEM;
    }
    memset(seg->pages, 0, (size_t)npages * sizeof(void *));

    /* Allocate physical pages */
    for (int i = 0; i < npages; i++) {
        seg->pages[i] = kalloc();
        if (seg->pages[i] == NULL) {
            /* Free already-allocated pages */
            for (int j = 0; j < i; j++)
                kfree(seg->pages[j]);
            kvfree(seg->pages);
            kvfree(seg);
            spin_unlock_irqrestore(&shm_ids.lock, irq);
            return (uint64)-ENOMEM;
        }
        memset(seg->pages[i], 0, PAGE_SIZE);
    }

    /* Fill in shmid_ds */
    seg->ds.shm_perm.key  = key;
    seg->ds.shm_perm.uid  = current->thread_group->euid;
    seg->ds.shm_perm.gid  = current->thread_group->egid;
    seg->ds.shm_perm.cuid = seg->ds.shm_perm.uid;
    seg->ds.shm_perm.cgid = seg->ds.shm_perm.gid;
    seg->ds.shm_perm.mode = (uint32)(shmflg & 0777);
    seg->ds.shm_segsz     = size;
    seg->ds.shm_cpid      = current->pid;
    seg->ds.shm_ctime     = goldfish_rtc_read_ns() / 1000000000ULL;

    int id = ipc_addid(&shm_ids, key, seg);
    spin_unlock_irqrestore(&shm_ids.lock, irq);

    if (id < 0) {
        for (int i = 0; i < npages; i++)
            kfree(seg->pages[i]);
        kvfree(seg->pages);
        kvfree(seg);
        return (uint64)-ENOSPC;
    }

    return (uint64)id;
}

/* ================================================================== */
/*  shmat                                                             */
/* ================================================================== */

/* VMA flags for SHM pages: readable, writable, user-accessible, shared.
 * VMA_FLAG_SHARED is critical: without it, fork() would COW the pages
 * instead of sharing the physical frames, breaking SHM semantics. */
#define SHM_VMA_FLAGS  (PROT_READ | PROT_WRITE | VMA_FLAG_USER | VMA_FLAG_SHARED)

uint64 sys_shmat(void)
{
    int shmid;
    uint64 shmaddr;
    int shmflg;
    argint(0, &shmid);
    argaddr(1, &shmaddr);
    argint(2, &shmflg);

    (void)shmflg;  /* SHM_RDONLY etc. — we map RW for simplicity */

    int irq = spin_lock_irqsave(&shm_ids.lock);
    struct shm_segment *seg = (struct shm_segment *)ipc_getobj(&shm_ids, shmid);
    if (seg == NULL) {
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return (uint64)-EINVAL;
    }

    int npages = seg->npages;
    size_t size = (size_t)npages * PAGE_SIZE;
    void **pages = seg->pages;  /* safe: pages array freed only on IPC_RMID */

    /* Pre-increment attach count so IPC_RMID won't free the backing pages
     * while we map them outside the lock. */
    seg->ds.shm_nattch++;
    seg->ds.shm_lpid  = current->pid;
    seg->ds.shm_atime = goldfish_rtc_read_ns() / 1000000000ULL;

    spin_unlock_irqrestore(&shm_ids.lock, irq);

    /* Find a free range in the user VM if shmaddr == 0 */
    if (shmaddr == 0)
        shmaddr = vm_find_free_range(current->vm, size, 0);
    if (shmaddr == 0) {
        /* Undo the pre-increment */
        irq = spin_lock_irqsave(&shm_ids.lock);
        seg->ds.shm_nattch--;
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return (uint64)-ENOMEM;
    }

    /* Create a single VMA covering the entire SHM region, then install
     * per-page PTEs.  Using one VMA instead of npages individual VMAs
     * avoids massive maple-tree fragmentation that can cause partial
     * mtree_store_range failures (leading to PTE leaks and fork bugs). */
    vm_wlock(current->vm);
    int ret = vm_mmap_region_locked(current->vm, shmaddr, size,
                                    SHM_VMA_FLAGS, NULL, 0, NULL);
    if (ret < 0) {
        vm_wunlock(current->vm);
        irq = spin_lock_irqsave(&shm_ids.lock);
        seg->ds.shm_nattch--;
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return (uint64)-ENOMEM;
    }

    pte_t pte_flags = vma2pte_flags(SHM_VMA_FLAGS);
    for (int i = 0; i < npages; i++) {
        if (mappages(current->vm->pagetable,
                     shmaddr + (uint64)i * PAGE_SIZE,
                     PAGE_SIZE, (uint64)pages[i], pte_flags) != 0) {
            /* Tear down the VMA.  __vma_set_free walks the PTEs and
             * calls page_ref_dec for each already-mapped page, which
             * undoes the page_ref_inc we did for pages 0..i-1. */
            vma_t *vma = vm_find_area(current->vm, shmaddr);
            if (vma)
                vma_free(current->vm, vma);
            vm_wunlock(current->vm);
            irq = spin_lock_irqsave(&shm_ids.lock);
            seg->ds.shm_nattch--;
            spin_unlock_irqrestore(&shm_ids.lock, irq);
            return (uint64)-ENOMEM;
        }
        /* Take a mapping reference.  The segment already holds one from
         * kalloc().  page_ref_dec in __vma_set_free (on shmdt / exit)
         * will drop this reference, leaving the segment's ref intact. */
        page_ref_inc(pages[i]);
    }
    vm_wunlock(current->vm);

    return shmaddr;
}

/* ================================================================== */
/*  shmdt                                                             */
/* ================================================================== */

uint64 sys_shmdt(void)
{
    uint64 shmaddr;
    argaddr(0, &shmaddr);

    /*
     * We don't track per-process attach info, so we iterate all segments
     * to find which one was mapped at shmaddr, then unmap.
     */
    int irq = spin_lock_irqsave(&shm_ids.lock);

    struct shm_segment *found = NULL;
    for (int i = 0; i < shm_ids.max_id; i++) {
        if (!shm_ids.entries[i].in_use)
            continue;
        struct shm_segment *seg =
            (struct shm_segment *)shm_ids.entries[i].kern_obj;
        if (seg == NULL)
            continue;

        uint64 pa = walkaddr(current->vm->pagetable, shmaddr);
        if (pa == 0)
            continue;
        if (pa != (uint64)seg->pages[0])
            continue;

        found = seg;
        break;
    }

    if (found == NULL) {
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return (uint64)-EINVAL;
    }

    int npages = found->npages;
    if (found->ds.shm_nattch > 0)
        found->ds.shm_nattch--;
    found->ds.shm_lpid  = current->pid;
    found->ds.shm_dtime = goldfish_rtc_read_ns() / 1000000000ULL;

    spin_unlock_irqrestore(&shm_ids.lock, irq);

    /* Unmap the single VMA covering the entire SHM region.
     * __vma_set_free walks the PTEs and calls page_ref_dec for each
     * mapped page; the segment's own reference (from kalloc) keeps
     * the physical pages alive until IPC_RMID. */
    vm_munmap_region(current->vm, shmaddr, (size_t)npages * PAGE_SIZE);

    return 0;
}

/* ================================================================== */
/*  shmctl                                                            */
/* ================================================================== */

uint64 sys_shmctl(void)
{
    int shmid, cmd;
    uint64 ubuf;
    argint(0, &shmid);
    argint(1, &cmd);
    argaddr(2, &ubuf);

    /* For IPC_SET, copy user data before acquiring the spinlock
     * (either_copyin may sleep for VM lock). */
    struct shmid_ds set_buf;
    if (cmd == IPC_SET) {
        if (either_copyin(&set_buf, 1, ubuf, sizeof(set_buf)) < 0)
            return (uint64)-EFAULT;
    }

    int irq = spin_lock_irqsave(&shm_ids.lock);
    struct shm_segment *seg = (struct shm_segment *)ipc_getobj(&shm_ids, shmid);
    if (seg == NULL) {
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return (uint64)-EINVAL;
    }

    switch (cmd) {
    case IPC_STAT: {
        struct shmid_ds buf;
        memmove(&buf, &seg->ds, sizeof(buf));
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        if (either_copyout(1, ubuf, &buf, sizeof(buf)) < 0)
            return (uint64)-EFAULT;
        return 0;
    }
    case IPC_SET: {
        seg->ds.shm_perm.uid  = set_buf.shm_perm.uid;
        seg->ds.shm_perm.gid  = set_buf.shm_perm.gid;
        seg->ds.shm_perm.mode = set_buf.shm_perm.mode & 0777;
        seg->ds.shm_ctime     = goldfish_rtc_read_ns() / 1000000000ULL;
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return 0;
    }
    case IPC_RMID: {
        if (seg->ds.shm_nattch > 0) {
            /* Can't destroy while attached — mark key invalid and let
             * last shmdt clean up.  Simplified: just refuse. */
            spin_unlock_irqrestore(&shm_ids.lock, irq);
            return (uint64)-EBUSY;
        }
        /* Free backing pages */
        for (int i = 0; i < seg->npages; i++)
            kfree(seg->pages[i]);
        kvfree(seg->pages);
        ipc_rmid(&shm_ids, shmid);
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        kvfree(seg);
        return 0;
    }
    default:
        spin_unlock_irqrestore(&shm_ids.lock, irq);
        return (uint64)-EINVAL;
    }
}
