/**
 * @file sys_arch.h
 * @brief lwIP OS abstraction types for xv6 kernel
 *
 * Defines the types for semaphores, mutexes, mailboxes, and threads
 * that lwIP uses for its OS abstraction layer.
 *
 * We use xv6's sleep_on_chan/wakeup_on_chan for blocking, which takes
 * any pointer as a "channel" key. Each struct contains a spinlock and
 * dedicated channel addresses (struct fields whose addresses serve as
 * unique channel identifiers).
 */

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include "types.h"
#include "lock/spinlock.h"

/* --------------------------------------------------------------------------
 * Protection level (for SYS_LIGHTWEIGHT_PROT)
 * We use interrupt enable/disable.
 * -------------------------------------------------------------------------- */
typedef int sys_prot_t;

/* --------------------------------------------------------------------------
 * Semaphore: spinlock + count; sleep on &count
 * -------------------------------------------------------------------------- */
typedef struct sys_sem {
    spinlock_t lock;
    int count;
    int valid;
} sys_sem_t;

/* --------------------------------------------------------------------------
 * Mutex: spinlock + held flag; sleep on &held
 * -------------------------------------------------------------------------- */
typedef struct sys_mutex {
    spinlock_t lock;
    int held;
    int valid;
} sys_mutex_t;

/* --------------------------------------------------------------------------
 * Mailbox: fixed-size circular buffer with spinlock
 * -------------------------------------------------------------------------- */
#define SYS_MBOX_SIZE 128

typedef struct sys_mbox {
    spinlock_t lock;
    int not_empty_chan;  /* address used as sleep channel for readers */
    int not_full_chan;   /* address used as sleep channel for writers */
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

#endif /* LWIP_ARCH_SYS_ARCH_H */
