/**
 * @file msg.c
 * @brief System V message-queue implementation.
 *
 * Provides msgget / msgsnd / msgrcv / msgctl.
 *
 * Messages are stored in a singly-linked list within each queue.
 * Memory for messages is dynamically allocated via kmm_alloc().
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

/* ── Internal message descriptor ── */

struct msg_msg {
    struct msg_msg *next;
    int64  mtype;
    uint64 msize;
    /* payload follows inline */
};

struct msg_queue {
    struct msqid_ds ds;
    struct msg_msg *head;
    struct msg_msg *tail;
    uint64 cur_bytes;     /* current bytes on queue (data only) */
};

static struct ipc_ids msg_ids;

void ipc_msg_init(void)
{
    ipc_ids_init(&msg_ids, "ipc_msg");
}

/* ================================================================== */
/*  msgget                                                            */
/* ================================================================== */

uint64 sys_msgget(void)
{
    int key, msgflg;
    argint(0, &key);
    argint(1, &msgflg);

    int irq = spin_lock_irqsave(&msg_ids.lock);

    if (key != IPC_PRIVATE) {
        int idx = ipc_findkey(&msg_ids, key);
        if (idx >= 0) {
            if ((msgflg & IPC_CREAT) && (msgflg & IPC_EXCL)) {
                spin_unlock_irqrestore(&msg_ids.lock, irq);
                return (uint64)-EEXIST;
            }
            int id = ipc_buildid(idx, msg_ids.entries[idx].seq);
            spin_unlock_irqrestore(&msg_ids.lock, irq);
            return (uint64)id;
        }
        if (!(msgflg & IPC_CREAT)) {
            spin_unlock_irqrestore(&msg_ids.lock, irq);
            return (uint64)-ENOENT;
        }
    }

    struct msg_queue *mq = (struct msg_queue *)kmm_alloc(sizeof(*mq));
    if (mq == NULL) {
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        return (uint64)-ENOMEM;
    }
    memset(mq, 0, sizeof(*mq));
    mq->ds.msg_perm.key  = key;
    mq->ds.msg_perm.uid  = current->thread_group->euid;
    mq->ds.msg_perm.gid  = current->thread_group->egid;
    mq->ds.msg_perm.cuid = mq->ds.msg_perm.uid;
    mq->ds.msg_perm.cgid = mq->ds.msg_perm.gid;
    mq->ds.msg_perm.mode = (uint32)(msgflg & 0777);
    mq->ds.msg_qbytes    = MSGMNB;
    mq->ds.msg_ctime     = goldfish_rtc_read_ns() / 1000000000ULL;

    int id = ipc_addid(&msg_ids, key, mq);
    spin_unlock_irqrestore(&msg_ids.lock, irq);

    if (id < 0) {
        kmm_free(mq);
        return (uint64)-ENOSPC;
    }
    return (uint64)id;
}

/* ================================================================== */
/*  msgsnd                                                            */
/* ================================================================== */

/*
 * User-space msgbuf layout:
 *   long mtype;    (8 bytes on 64-bit)
 *   char mtext[];  (variable)
 */

uint64 sys_msgsnd(void)
{
    int msqid, msgflg;
    uint64 umsgp;
    uint64 msgsz;
    argint(0, &msqid);
    argaddr(1, &umsgp);
    argaddr(2, &msgsz);
    argint(3, &msgflg);

    if (msgsz > MSGMAX)
        return (uint64)-EINVAL;

    /* Read mtype from user */
    int64 mtype;
    if (either_copyin(&mtype, 1, umsgp, sizeof(mtype)) < 0)
        return (uint64)-EFAULT;
    if (mtype < 1)
        return (uint64)-EINVAL;

    /* Allocate internal message */
    struct msg_msg *m = (struct msg_msg *)kmm_alloc(
        sizeof(struct msg_msg) + msgsz);
    if (m == NULL)
        return (uint64)-ENOMEM;
    m->next  = NULL;
    m->mtype = mtype;
    m->msize = msgsz;

    /* Copy message body from user (after the mtype field) */
    if (msgsz > 0) {
        if (either_copyin((char *)m + sizeof(struct msg_msg), 1,
                          umsgp + sizeof(int64), msgsz) < 0) {
            kmm_free(m);
            return (uint64)-EFAULT;
        }
    }

    /* Enqueue */
    int irq = spin_lock_irqsave(&msg_ids.lock);
    struct msg_queue *mq = (struct msg_queue *)ipc_getobj(&msg_ids, msqid);
    if (mq == NULL) {
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        kmm_free(m);
        return (uint64)-EINVAL;
    }

    /* Check queue byte limit */
    if (mq->cur_bytes + msgsz > mq->ds.msg_qbytes) {
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        kmm_free(m);
        if (msgflg & IPC_NOWAIT)
            return (uint64)-EAGAIN;
        return (uint64)-EAGAIN;  /* simplified: don't block */
    }

    if (mq->tail)
        mq->tail->next = m;
    else
        mq->head = m;
    mq->tail = m;
    mq->cur_bytes += msgsz;
    mq->ds.msg_qnum++;
    mq->ds.msg_lspid = current->pid;
    mq->ds.msg_stime = goldfish_rtc_read_ns() / 1000000000ULL;

    spin_unlock_irqrestore(&msg_ids.lock, irq);
    /* Wake any threads blocked in msgrcv on this queue */
    wakeup_on_chan(mq);
    return 0;
}

/* ================================================================== */
/*  msgrcv                                                            */
/* ================================================================== */

uint64 sys_msgrcv(void)
{
    int msqid, msgflg;
    uint64 umsgp, msgsz;
    int64 msgtyp;
    argint(0, &msqid);
    argaddr(1, &umsgp);
    argaddr(2, &msgsz);
    argint64(3, &msgtyp);
    argint(4, &msgflg);

    if (msgflg & IPC_NOWAIT) {
        /* Non-blocking: single check */
        int irq = spin_lock_irqsave(&msg_ids.lock);
        struct msg_queue *mq =
            (struct msg_queue *)ipc_getobj(&msg_ids, msqid);
        if (mq == NULL) {
            spin_unlock_irqrestore(&msg_ids.lock, irq);
            return (uint64)-EINVAL;
        }

        struct msg_msg *prev = NULL;
        struct msg_msg *cur  = mq->head;
        struct msg_msg *found = NULL;
        struct msg_msg *found_prev = NULL;

        while (cur) {
            int match = 0;
            if (msgtyp == 0) {
                match = 1;
            } else if (msgtyp > 0) {
                match = (cur->mtype == msgtyp);
            } else {
                match = (cur->mtype <= -msgtyp);
                if (match && found && cur->mtype >= found->mtype)
                    match = 0;
            }
            if (match) {
                found = cur;
                found_prev = prev;
                if (msgtyp >= 0)
                    break;
            }
            prev = cur;
            cur = cur->next;
        }

        if (!found) {
            spin_unlock_irqrestore(&msg_ids.lock, irq);
            return (uint64)-ENOMSG;
        }

        uint64 copy_sz = found->msize;
        if (copy_sz > msgsz) {
            if (!(msgflg & 0x20000 /* MSG_NOERROR */)) {
                spin_unlock_irqrestore(&msg_ids.lock, irq);
                return (uint64)-E2BIG;
            }
            copy_sz = msgsz;
        }

        if (found_prev)
            found_prev->next = found->next;
        else
            mq->head = found->next;
        if (mq->tail == found)
            mq->tail = found_prev;

        mq->cur_bytes -= found->msize;
        mq->ds.msg_qnum--;
        mq->ds.msg_lrpid = current->pid;
        mq->ds.msg_rtime = goldfish_rtc_read_ns() / 1000000000ULL;
        spin_unlock_irqrestore(&msg_ids.lock, irq);

        if (either_copyout(1, umsgp, &found->mtype,
                           sizeof(found->mtype)) < 0) {
            kmm_free(found);
            return (uint64)-EFAULT;
        }
        if (copy_sz > 0) {
            if (either_copyout(1, umsgp + sizeof(int64),
                               (char *)found + sizeof(struct msg_msg),
                               copy_sz) < 0) {
                kmm_free(found);
                return (uint64)-EFAULT;
            }
        }
        uint64 ret = copy_sz;
        kmm_free(found);
        return ret;
    }

    /* Blocking receive: sleep until a message arrives */
    spin_lock(&msg_ids.lock);

    for (;;) {
        struct msg_queue *mq =
            (struct msg_queue *)ipc_getobj(&msg_ids, msqid);
        if (mq == NULL) {
            spin_unlock(&msg_ids.lock);
            return (uint64)-EIDRM;
        }

        /* Search for matching message */
        struct msg_msg *prev = NULL;
        struct msg_msg *cur  = mq->head;
        struct msg_msg *found = NULL;
        struct msg_msg *found_prev = NULL;

        while (cur) {
            int match = 0;
            if (msgtyp == 0) {
                match = 1;  /* any type */
            } else if (msgtyp > 0) {
                match = (cur->mtype == msgtyp);
            } else {
                /* msgtyp < 0: lowest type ≤ abs(msgtyp) */
                match = (cur->mtype <= -msgtyp);
                if (match && found && cur->mtype >= found->mtype)
                    match = 0;  /* already have a better candidate */
            }
            if (match) {
                found = cur;
                found_prev = prev;
                if (msgtyp >= 0)
                    break;  /* first match suffices for typ==0 or typ>0 */
            }
            prev = cur;
            cur = cur->next;
        }

        if (found) {
            /* Check size BEFORE dequeuing so the message stays on the
             * queue when we return -E2BIG. */
            uint64 copy_sz = found->msize;
            if (copy_sz > msgsz) {
                if (!(msgflg & 0x20000 /* MSG_NOERROR */)) {
                    spin_unlock(&msg_ids.lock);
                    return (uint64)-E2BIG;
                }
                copy_sz = msgsz;
            }

            /* Dequeue */
            if (found_prev)
                found_prev->next = found->next;
            else
                mq->head = found->next;
            if (mq->tail == found)
                mq->tail = found_prev;

            mq->cur_bytes -= found->msize;
            mq->ds.msg_qnum--;
            mq->ds.msg_lrpid = current->pid;
            mq->ds.msg_rtime = goldfish_rtc_read_ns() / 1000000000ULL;

            spin_unlock(&msg_ids.lock);

            /* Copy mtype */
            if (either_copyout(1, umsgp, &found->mtype,
                               sizeof(found->mtype)) < 0) {
                kmm_free(found);
                return (uint64)-EFAULT;
            }
            /* Copy body */
            if (copy_sz > 0) {
                if (either_copyout(1, umsgp + sizeof(int64),
                                   (char *)found + sizeof(struct msg_msg),
                                   copy_sz) < 0) {
                    kmm_free(found);
                    return (uint64)-EFAULT;
                }
            }

            uint64 ret = copy_sz;
            kmm_free(found);
            return ret;
        }

        /* No matching message — sleep until msgsnd wakes us */
        int ret = sleep_on_chan_interruptible(mq, &msg_ids.lock);
        if (ret != 0) {
            /* Interrupted by signal — lock is reacquired */
            spin_unlock(&msg_ids.lock);
            return (uint64)-EINTR;
        }
        /* Lock is reacquired, loop to re-check */
    }
}

/* ================================================================== */
/*  msgctl                                                            */
/* ================================================================== */

uint64 sys_msgctl(void)
{
    int msqid, cmd;
    uint64 ubuf;
    argint(0, &msqid);
    argint(1, &cmd);
    argaddr(2, &ubuf);

    int irq = spin_lock_irqsave(&msg_ids.lock);
    struct msg_queue *mq = (struct msg_queue *)ipc_getobj(&msg_ids, msqid);
    if (mq == NULL) {
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        return (uint64)-EINVAL;
    }

    switch (cmd) {
    case IPC_STAT: {
        struct msqid_ds buf;
        memmove(&buf, &mq->ds, sizeof(buf));
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        if (either_copyout(1, ubuf, &buf, sizeof(buf)) < 0)
            return (uint64)-EFAULT;
        return 0;
    }
    case IPC_SET: {
        struct msqid_ds buf;
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        if (either_copyin(&buf, 1, ubuf, sizeof(buf)) < 0)
            return (uint64)-EFAULT;
        irq = spin_lock_irqsave(&msg_ids.lock);
        mq = (struct msg_queue *)ipc_getobj(&msg_ids, msqid);
        if (mq == NULL) {
            spin_unlock_irqrestore(&msg_ids.lock, irq);
            return (uint64)-EINVAL;
        }
        mq->ds.msg_perm.uid  = buf.msg_perm.uid;
        mq->ds.msg_perm.gid  = buf.msg_perm.gid;
        mq->ds.msg_perm.mode = buf.msg_perm.mode & 0777;
        mq->ds.msg_qbytes    = buf.msg_qbytes;
        mq->ds.msg_ctime     = goldfish_rtc_read_ns() / 1000000000ULL;
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        return 0;
    }
    case IPC_RMID: {
        /* Free all queued messages */
        struct msg_msg *m = mq->head;
        while (m) {
            struct msg_msg *next = m->next;
            kmm_free(m);
            m = next;
        }
        ipc_rmid(&msg_ids, msqid);
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        /* Wake any threads blocked in msgrcv so they observe removal */
        wakeup_on_chan(mq);
        kmm_free(mq);
        return 0;
    }
    default:
        spin_unlock_irqrestore(&msg_ids.lock, irq);
        return (uint64)-EINVAL;
    }
}
