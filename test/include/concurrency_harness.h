/*
 * Concurrency harness for pcache host tests
 *
 * Provides real mutual exclusion for xv6 spinlocks using pthread mutexes,
 * and real blocking/wakeup for proc_queue using pthread condvars.
 *
 * This header deliberately does NOT include <pthread.h> because the test
 * include paths contain kernel/inc/proc which causes <pthread.h> ->
 * <sched.h> -> kernel/proc/sched.h -> kernel/printf.h, conflicting with
 * the host <stdio.h>.  All pthread usage lives in concurrency_harness.c.
 *
 * Usage:
 *   1. Call concurrency_mode_enable() before concurrent tests
 *   2. Call concurrency_mode_disable() after
 *
 * When enabled:
 *   - spin_lock/spin_unlock use a global hash table of pthread mutexes
 *   - proc_queue_wait blocks on a condvar and releases the associated spinlock
 *   - proc_queue_wakeup_all broadcasts the condvar
 */
#ifndef CONCURRENCY_HARNESS_H
#define CONCURRENCY_HARNESS_H

/* Global flag — checked by wrappers via atomic load.
 * We use _Bool directly to avoid pulling in <stdbool.h> which
 * would redefine the kernel's bool enum to _Bool, causing type
 * mismatches with function declarations in pcache.h.          */
extern _Bool g_concurrency_mode;

/* Enable / disable concurrency mode (init / destroy internal tables) */
void concurrency_mode_enable(void);
void concurrency_mode_disable(void);

/* Spinlock -> pthread_mutex operations (called from spinlock wrappers) */
void conc_spin_lock(void *lock_ptr);
void conc_spin_unlock(void *lock_ptr);

/* proc_queue -> pthread_cond operations (called from proc wrappers) */
void conc_proc_queue_wait(void *queue_ptr, void *lock_ptr);
void conc_proc_queue_wakeup_all(void *queue_ptr);

/* ---- Thread management (wraps pthread_create/join) ---- */
#define CONC_MAX_THREADS 16

typedef void *(*conc_thread_fn_t)(void *);

int  conc_thread_create(int slot, conc_thread_fn_t fn, void *arg);
int  conc_thread_join(int slot, void **retval);

/* ---- Barrier (wraps pthread_barrier) ---- */
int  conc_barrier_init(int count);
void conc_barrier_wait(void);
void conc_barrier_destroy(void);

/* ---- Sleep helper ---- */
void conc_sleep_ms(int ms);

#endif /* CONCURRENCY_HARNESS_H */
