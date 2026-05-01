/**
 * @file sys_arch.h
 * @brief lwIP OS abstraction types for xv6 kernel
 *
 * Defines the types for semaphores, mutexes, mailboxes, and threads
 * that lwIP uses for its OS abstraction layer.
 *
 * - Semaphores wrap the kernel's native sem_t (tq-based).  Timed waits
 *   combine sched_timer_set() with tq_wait() so lwIP TCP timers fire.
 * - Mutexes wrap the kernel's native mutex_t (tq-based, owner-tracked).
 * - Mailboxes use a circular buffer with two sem_t (not-empty / not-full).
 */

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include "types.h"
#include "lock/spinlock.h"
#include "lock/semaphore_types.h"
#include "lock/mutex_types.h"

/* --------------------------------------------------------------------------
 * Protection level (for SYS_LIGHTWEIGHT_PROT)
 * We use interrupt enable/disable.
 * -------------------------------------------------------------------------- */
typedef int sys_prot_t;

/* --------------------------------------------------------------------------
 * Semaphore: wraps kernel sem_t
 * -------------------------------------------------------------------------- */
typedef struct sys_sem {
    sem_t    sem;
    int      valid;
} sys_sem_t;

/* --------------------------------------------------------------------------
 * Mutex: wraps kernel mutex_t
 * -------------------------------------------------------------------------- */
typedef struct sys_mutex {
    mutex_t  mutex;
    int      valid;
} sys_mutex_t;

/* --------------------------------------------------------------------------
 * Mailbox: fixed-size circular buffer guarded by a spinlock,
 *          with two kernel semaphores for blocking.
 * -------------------------------------------------------------------------- */
/*
 * Browser workloads can receive large HTTPS responses as many MSS-sized TCP
 * pbufs before the WebKit network thread gets scheduled again.  Keep the
 * mailbox comfortably above TCP_WND / MSS so lwIP does not stall the stream
 * merely because the netconn recv queue filled.
 */
#define SYS_MBOX_SIZE 512

typedef struct sys_mbox {
    spinlock_t lock;       /* protects the circular buffer fields */
    sem_t not_empty;       /* readers block here when count == 0  */
    sem_t not_full;        /* writers block here when count == max */
    void *msgs[SYS_MBOX_SIZE];
    int head;
    int tail;
    int count;
    int valid;
} sys_mbox_t;

/* --------------------------------------------------------------------------
 * Thread: just a thread pointer
 * -------------------------------------------------------------------------- */
typedef struct thread *sys_thread_t;

/* --------------------------------------------------------------------------
 * Per-thread semaphore for LWIP_NETCONN_SEM_PER_THREAD
 * -------------------------------------------------------------------------- */
#if LWIP_NETCONN_SEM_PER_THREAD
sys_sem_t *sys_arch_netconn_sem_get(void);
void sys_arch_netconn_sem_alloc(void);
void sys_arch_netconn_sem_free(void);
#define LWIP_NETCONN_THREAD_SEM_GET()   sys_arch_netconn_sem_get()
#define LWIP_NETCONN_THREAD_SEM_ALLOC() sys_arch_netconn_sem_alloc()
#define LWIP_NETCONN_THREAD_SEM_FREE()  sys_arch_netconn_sem_free()
#endif /* LWIP_NETCONN_SEM_PER_THREAD */

/* --------------------------------------------------------------------------
 * Validity macros — lwIP uses these to check if a primitive is initialised
 * -------------------------------------------------------------------------- */
#define sys_sem_valid(sem)          ((sem) != NULL && (sem)->valid)
#define sys_sem_set_invalid(sem)    do { if ((sem) != NULL) (sem)->valid = 0; } while(0)
#define sys_mutex_valid(mu)         ((mu) != NULL && (mu)->valid)
#define sys_mutex_set_invalid(mu)   do { if ((mu) != NULL) (mu)->valid = 0; } while(0)
#define sys_mbox_valid(mbox)        ((mbox) != NULL && (mbox)->valid)
#define sys_mbox_set_invalid(mbox)  do { if ((mbox) != NULL) (mbox)->valid = 0; } while(0)

#define sys_sem_valid_val(sem)          ((sem).valid)
#define sys_sem_set_invalid_val(sem)    do { (sem).valid = 0; } while(0)
#define sys_mbox_valid_val(mbox)        ((mbox).valid)
#define sys_mbox_set_invalid_val(mbox)  do { (mbox).valid = 0; } while(0)

/* xv6: yield current CPU timeslice without depending on the timer subsystem */
void sys_arch_yield(void);

#endif /* LWIP_ARCH_SYS_ARCH_H */
