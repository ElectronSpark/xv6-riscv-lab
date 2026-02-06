/*
 * Spinlock wrappers for unit tests
 * Provides simple mutex-like behavior for host testing
 */

#include <stddef.h>
#include <assert.h>
#include "types.h"
#include "spinlock.h"
#include "wrapper_tracking.h"
#include "concurrency_harness.h"

// Global tracking pointer (NULL if tracking disabled)
spinlock_tracking_t *g_spinlock_tracking = NULL;

void wrapper_tracking_enable_spinlock(spinlock_tracking_t *tracking)
{
    g_spinlock_tracking = tracking;
}

void wrapper_tracking_disable_spinlock(void)
{
    g_spinlock_tracking = NULL;
}

void __wrap_spin_init(struct spinlock *lock, char *name)
{
    if (g_spinlock_tracking) {
        g_spinlock_tracking->spin_init_count++;
        g_spinlock_tracking->last_spin_init = lock;
        g_spinlock_tracking->last_spin_name = name;
    }
    
    if (lock == NULL) {
        return;
    }
    lock->locked = 0;
    lock->name = name;
    lock->cpu = NULL;
}

void __wrap_spin_lock(struct spinlock *lock)
{
    if (g_spinlock_tracking) {
        g_spinlock_tracking->spin_lock_count++;
        g_spinlock_tracking->last_spin_lock = lock;
    }
    
    if (lock == NULL) {
        return;
    }
    if (g_concurrency_mode) {
        conc_spin_lock(lock);
    }
    __atomic_store_n(&lock->locked, 1, __ATOMIC_SEQ_CST);
}

void __wrap_spin_unlock(struct spinlock *lock)
{
    if (g_spinlock_tracking) {
        g_spinlock_tracking->spin_unlock_count++;
        g_spinlock_tracking->last_spin_unlock = lock;
    }
    
    if (lock == NULL) {
        return;
    }
    __atomic_store_n(&lock->locked, 0, __ATOMIC_SEQ_CST);
    if (g_concurrency_mode) {
        conc_spin_unlock(lock);
    }
}

int __wrap_spin_holding(struct spinlock *lock)
{
    if (lock == NULL) {
        return 0;
    }
    return lock->locked != 0;
}

void __wrap_push_off(void)
{
    /* Interrupt disabling not needed in host tests */
}

void __wrap_pop_off(void)
{
    /* Interrupt enabling not needed in host tests */
}
