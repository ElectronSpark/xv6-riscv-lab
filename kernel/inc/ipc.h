/**
 * @file ipc.h
 * @brief System V IPC (shared memory, semaphores, message queues)
 *
 * Provides the kernel-internal definitions for System V IPC objects
 * that are shared across shm.c, sem.c, msg.c, and ipc_util.c.
 */

#ifndef __KERNEL_IPC_H
#define __KERNEL_IPC_H

#include "types.h"
#include "lock/spinlock.h"
#include "list_type.h"

/* ── IPC key/flag constants (match Linux ABI) ── */
#define IPC_PRIVATE  0
#define IPC_CREAT    01000
#define IPC_EXCL     02000
#define IPC_NOWAIT   04000
#define IPC_RMID     0
#define IPC_SET      1
#define IPC_STAT     2
#define IPC_INFO     3

/* ── Permission bits ── */
struct ipc_perm {
    int32  key;
    uint32 uid;
    uint32 gid;
    uint32 cuid;
    uint32 cgid;
    uint32 mode;
    uint16 seq;
    uint16 __pad;
};

/* ── Limits ── */
#define IPC_MAX_IDS     128    /* max IDs per IPC type */
#define SHMMAX          (64UL * 1024 * 1024)  /* 64 MiB max segment size */
#define SHMMNI          IPC_MAX_IDS
#define SEMMNI          IPC_MAX_IDS
#define SEMMSL          250    /* max semaphores per set */
#define MSGMNI          IPC_MAX_IDS
#define MSGMAX          8192   /* max message size */
#define MSGMNB          16384  /* default max bytes on queue */

/* ── Generic IPC ID table ── */
struct ipc_id_entry {
    int    in_use;        /* 0 = free slot */
    int    key;           /* IPC_PRIVATE (0) or user-chosen key */
    uint16 seq;           /* sequence number for ID generation */
    void  *kern_obj;      /* pointer to shm_segment / sem_array / msg_queue */
};

struct ipc_ids {
    spinlock_t lock;
    int        max_id;    /* highest in-use slot + 1 */
    struct ipc_id_entry entries[IPC_MAX_IDS];
};

/* ── IPC utility functions (ipc_util.c) ── */
void ipc_ids_init(struct ipc_ids *ids, const char *name);
int  ipc_findkey(struct ipc_ids *ids, int key);
int  ipc_addid(struct ipc_ids *ids, int key, void *obj);
void ipc_rmid(struct ipc_ids *ids, int id);
void *ipc_getobj(struct ipc_ids *ids, int id);

/* Generate a Linux-compatible IPC identifier from slot index + sequence */
static inline int ipc_buildid(int idx, int seq) {
    return (seq * IPC_MAX_IDS) + idx;
}
static inline int ipc_id_to_idx(int id) {
    return id % IPC_MAX_IDS;
}

/* ── Shared memory ── */
struct shmid_ds {
    struct ipc_perm shm_perm;
    uint64 shm_segsz;
    uint64 shm_atime;
    uint64 shm_dtime;
    uint64 shm_ctime;
    int32  shm_cpid;
    int32  shm_lpid;
    uint32 shm_nattch;
    uint32 __pad;
};

/* ── Semaphores ── */
struct semid_ds {
    struct ipc_perm sem_perm;
    uint64 sem_otime;
    uint64 sem_ctime;
    uint32 sem_nsems;
    uint32 __pad;
};

struct sembuf {
    uint16 sem_num;
    int16  sem_op;
    int16  sem_flg;
    int16  __pad;
};

/* ── Message queues ── */
struct msqid_ds {
    struct ipc_perm msg_perm;
    uint64 msg_stime;
    uint64 msg_rtime;
    uint64 msg_ctime;
    uint64 msg_qnum;
    uint64 msg_qbytes;
    int32  msg_lspid;
    int32  msg_lrpid;
};

/* ── Syscall prototypes ── */
uint64 sys_shmget(void);
uint64 sys_shmat(void);
uint64 sys_shmdt(void);
uint64 sys_shmctl(void);

uint64 sys_semget(void);
uint64 sys_semop(void);
uint64 sys_semtimedop(void);
uint64 sys_semctl(void);

uint64 sys_msgget(void);
uint64 sys_msgsnd(void);
uint64 sys_msgrcv(void);
uint64 sys_msgctl(void);

/* ── Initialization ── */
void ipc_shm_init(void);
void ipc_sem_init(void);
void ipc_msg_init(void);

#endif /* __KERNEL_IPC_H */
