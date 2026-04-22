/**
 * @file sem.c
 * @brief System V semaphore implementation.
 *
 * Provides semget / semop / semtimedop / semctl.
 *
 * Each semaphore set is an array of uint16 values protected by a
 * spinlock.  semop blocks the calling thread (via yield) when a
 * decrement cannot be satisfied immediately.
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
#include "proc/sched.h"
#include "mm/vm.h"
#include "timer/goldfish_rtc.h"
#include "signal.h"

/* ── Internal semaphore-set descriptor ── */

struct sem_array {
    struct semid_ds ds;
    uint16 vals[SEMMSL];           /* semaphore values */
};

static struct ipc_ids sem_ids;

void ipc_sem_init(void)
{
    ipc_ids_init(&sem_ids, "ipc_sem");
}

/* ================================================================== */
/*  semget                                                            */
/* ================================================================== */

uint64 sys_semget(void)
{
    int key, nsems, semflg;
    argint(0, &key);
    argint(1, &nsems);
    argint(2, &semflg);

    if (nsems < 0 || nsems > SEMMSL)
        return (uint64)-EINVAL;

    int irq = spin_lock_irqsave(&sem_ids.lock);

    if (key != IPC_PRIVATE) {
        int idx = ipc_findkey(&sem_ids, key);
        if (idx >= 0) {
            if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
                spin_unlock_irqrestore(&sem_ids.lock, irq);
                return (uint64)-EEXIST;
            }
            int id = ipc_buildid(idx, sem_ids.entries[idx].seq);
            spin_unlock_irqrestore(&sem_ids.lock, irq);
            return (uint64)id;
        }
        if (!(semflg & IPC_CREAT)) {
            spin_unlock_irqrestore(&sem_ids.lock, irq);
            return (uint64)-ENOENT;
        }
    }

    if (nsems == 0)
        nsems = 1;

    struct sem_array *sa = (struct sem_array *)kmm_alloc(sizeof(*sa));
    if (sa == NULL) {
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        return (uint64)-ENOMEM;
    }
    memset(sa, 0, sizeof(*sa));
    sa->ds.sem_perm.key  = key;
    sa->ds.sem_perm.uid  = current->thread_group->euid;
    sa->ds.sem_perm.gid  = current->thread_group->egid;
    sa->ds.sem_perm.cuid = sa->ds.sem_perm.uid;
    sa->ds.sem_perm.cgid = sa->ds.sem_perm.gid;
    sa->ds.sem_perm.mode = (uint32)(semflg & 0777);
    sa->ds.sem_nsems     = (uint32)nsems;
    sa->ds.sem_ctime     = goldfish_rtc_read_ns() / 1000000000ULL;

    int id = ipc_addid(&sem_ids, key, sa);
    spin_unlock_irqrestore(&sem_ids.lock, irq);

    if (id < 0) {
        kmm_free(sa);
        return (uint64)-ENOSPC;
    }
    return (uint64)id;
}

/* ================================================================== */
/*  semop / semtimedop                                                */
/* ================================================================== */

/* Maximum number of operations per semop call */
#define SEMOPM 32

static uint64 do_semop(int semid, uint64 usops, unsigned nsops,
                       int has_timeout)
{
    if (nsops == 0 || nsops > SEMOPM)
        return (uint64)-EINVAL;

    struct sembuf ops[SEMOPM];
    if (either_copyin(ops, 1, usops, nsops * sizeof(struct sembuf)) < 0)
        return (uint64)-EFAULT;

    spin_lock(&sem_ids.lock);

    for (;;) {
        struct sem_array *sa =
            (struct sem_array *)ipc_getobj(&sem_ids, semid);
        if (sa == NULL) {
            spin_unlock(&sem_ids.lock);
            return (uint64)-EIDRM;
        }

        /* Try to apply all operations atomically */
        int would_block = 0;
        uint16 tmp[SEMMSL];
        memmove(tmp, sa->vals, sa->ds.sem_nsems * sizeof(uint16));

        for (unsigned i = 0; i < nsops; i++) {
            unsigned num = ops[i].sem_num;
            int16   sop = ops[i].sem_op;

            if (num >= sa->ds.sem_nsems) {
                spin_unlock(&sem_ids.lock);
                return (uint64)-EFBIG;
            }

            if (sop > 0) {
                tmp[num] += (uint16)sop;
            } else if (sop < 0) {
                if ((int)tmp[num] + sop < 0) {
                    if (ops[i].sem_flg & IPC_NOWAIT) {
                        spin_unlock(&sem_ids.lock);
                        return (uint64)-EAGAIN;
                    }
                    would_block = 1;
                    break;
                }
                tmp[num] = (uint16)((int)tmp[num] + sop);
            } else {
                /* sem_op == 0: wait for value to become 0 */
                if (tmp[num] != 0) {
                    if (ops[i].sem_flg & IPC_NOWAIT) {
                        spin_unlock(&sem_ids.lock);
                        return (uint64)-EAGAIN;
                    }
                    would_block = 1;
                    break;
                }
            }
        }

        if (!would_block) {
            /* Commit */
            memmove(sa->vals, tmp, sa->ds.sem_nsems * sizeof(uint16));
            sa->ds.sem_otime = goldfish_rtc_read_ns() / 1000000000ULL;
            spin_unlock(&sem_ids.lock);
            /* Wake up any threads blocked on this semaphore set */
            wakeup_on_chan(sa);
            return 0;
        }

        /* Block: atomically release sem_ids.lock and sleep.
         * On wakeup the lock is reacquired by sleep_on_chan. */
        int ret = sleep_on_chan_interruptible(sa, &sem_ids.lock);
        if (ret != 0) {
            /* Interrupted by signal — lock is reacquired */
            spin_unlock(&sem_ids.lock);
            return (uint64)-EINTR;
        }
        /* Lock is reacquired, loop to re-check */
    }
}

uint64 sys_semop(void)
{
    int semid;
    uint64 usops;
    int nsops;
    argint(0, &semid);
    argaddr(1, &usops);
    argint(2, &nsops);

    return do_semop(semid, usops, (unsigned)nsops, 0);
}

uint64 sys_semtimedop(void)
{
    int semid;
    uint64 usops;
    int nsops;
    /* uint64 utimespec; */  /* ignored for now */
    argint(0, &semid);
    argaddr(1, &usops);
    argint(2, &nsops);
    /* argaddr(3, &utimespec); */

    return do_semop(semid, usops, (unsigned)nsops, 1);
}

/* ================================================================== */
/*  semctl                                                            */
/* ================================================================== */

/* semctl sub-commands */
#define GETVAL  12
#define SETVAL  16
#define GETALL  13
#define SETALL  17

uint64 sys_semctl(void)
{
    int semid, semnum, cmd;
    uint64 arg;
    argint(0, &semid);
    argint(1, &semnum);
    argint(2, &cmd);
    argaddr(3, &arg);

    int irq = spin_lock_irqsave(&sem_ids.lock);
    struct sem_array *sa = (struct sem_array *)ipc_getobj(&sem_ids, semid);
    if (sa == NULL) {
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        return (uint64)-EINVAL;
    }

    switch (cmd) {
    case GETVAL: {
        if ((unsigned)semnum >= sa->ds.sem_nsems) {
            spin_unlock_irqrestore(&sem_ids.lock, irq);
            return (uint64)-EINVAL;
        }
        int val = sa->vals[semnum];
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        return (uint64)val;
    }
    case SETVAL: {
        if ((unsigned)semnum >= sa->ds.sem_nsems) {
            spin_unlock_irqrestore(&sem_ids.lock, irq);
            return (uint64)-EINVAL;
        }
        sa->vals[semnum] = (uint16)arg;
        sa->ds.sem_ctime = goldfish_rtc_read_ns() / 1000000000ULL;
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        /* Value changed — wake any threads blocked on this set */
        wakeup_on_chan(sa);
        return 0;
    }
    case GETALL: {
        uint32 nsems = sa->ds.sem_nsems;
        uint16 tmp[SEMMSL];
        memmove(tmp, sa->vals, nsems * sizeof(uint16));
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        if (either_copyout(1, arg, tmp, nsems * sizeof(uint16)) < 0)
            return (uint64)-EFAULT;
        return 0;
    }
    case SETALL: {
        uint32 nsems = sa->ds.sem_nsems;
        uint16 setall_tmp[SEMMSL];
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        if (either_copyin(setall_tmp, 1, arg, nsems * sizeof(uint16)) < 0)
            return (uint64)-EFAULT;
        irq = spin_lock_irqsave(&sem_ids.lock);
        sa = (struct sem_array *)ipc_getobj(&sem_ids, semid);
        if (sa == NULL) {
            spin_unlock_irqrestore(&sem_ids.lock, irq);
            return (uint64)-EINVAL;
        }
        memmove(sa->vals, setall_tmp, nsems * sizeof(uint16));
        sa->ds.sem_ctime = goldfish_rtc_read_ns() / 1000000000ULL;
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        return 0;
    }
    case IPC_STAT: {
        struct semid_ds buf;
        memmove(&buf, &sa->ds, sizeof(buf));
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        if (either_copyout(1, arg, &buf, sizeof(buf)) < 0)
            return (uint64)-EFAULT;
        return 0;
    }
    case IPC_SET: {
        struct semid_ds buf;
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        if (either_copyin(&buf, 1, arg, sizeof(buf)) < 0)
            return (uint64)-EFAULT;
        irq = spin_lock_irqsave(&sem_ids.lock);
        sa = (struct sem_array *)ipc_getobj(&sem_ids, semid);
        if (sa == NULL) {
            spin_unlock_irqrestore(&sem_ids.lock, irq);
            return (uint64)-EINVAL;
        }
        sa->ds.sem_perm.uid  = buf.sem_perm.uid;
        sa->ds.sem_perm.gid  = buf.sem_perm.gid;
        sa->ds.sem_perm.mode = buf.sem_perm.mode & 0777;
        sa->ds.sem_ctime     = goldfish_rtc_read_ns() / 1000000000ULL;
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        return 0;
    }
    case IPC_RMID: {
        ipc_rmid(&sem_ids, semid);
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        /* Wake any threads blocked on this set so they observe removal */
        wakeup_on_chan(sa);
        kmm_free(sa);
        return 0;
    }
    default:
        spin_unlock_irqrestore(&sem_ids.lock, irq);
        return (uint64)-EINVAL;
    }
}
