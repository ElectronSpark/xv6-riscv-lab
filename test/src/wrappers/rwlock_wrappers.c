/*
 * RWLock wrappers for unit tests
 * Provides no-op behavior for host testing
 */

#include <stddef.h>
#include "types.h"
#include "lock/rwlock_types.h"

void __wrap_rwlock_init(struct rwlock *rw, const char *name)
{
    if (rw == NULL) {
        return;
    }
    __atomic_store_n(&rw->state, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rw->w_holder, RWLOCK_NONE_HOLDER, __ATOMIC_SEQ_CST);
    rw->name = name;
}

void __wrap_rwlock_rlock(struct rwlock *rw)
{
    (void)rw;
}

void __wrap_rwlock_runlock(struct rwlock *rw)
{
    (void)rw;
}

void __wrap_rwlock_wlock(struct rwlock *rw)
{
    (void)rw;
}

void __wrap_rwlock_wunlock(struct rwlock *rw)
{
    (void)rw;
}

int __wrap_rwlock_r_sleep_cb(void *data)
{
    (void)data;
    return 0;
}

void __wrap_rwlock_r_wake_cb(void *data, int status)
{
    (void)data;
    (void)status;
}
