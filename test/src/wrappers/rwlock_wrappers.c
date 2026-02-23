/*
 * RWLock wrappers/stubs for host unit tests
 */

#include "lock/rwlock_types.h"
#include "types.h"

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
    if (rw == NULL) {
        return;
    }
    __atomic_add_fetch(&rw->state, 1, __ATOMIC_SEQ_CST);
}

void __wrap_rwlock_runlock(struct rwlock *rw)
{
    if (rw == NULL) {
        return;
    }
    (void)__atomic_sub_fetch(&rw->state, 1, __ATOMIC_SEQ_CST);
}

void __wrap_rwlock_wlock(struct rwlock *rw)
{
    if (rw == NULL) {
        return;
    }
    __atomic_store_n(&rw->w_holder, 0, __ATOMIC_SEQ_CST);
}

void __wrap_rwlock_wunlock(struct rwlock *rw)
{
    if (rw == NULL) {
        return;
    }
    __atomic_store_n(&rw->w_holder, RWLOCK_NONE_HOLDER, __ATOMIC_SEQ_CST);
}

int __wrap_rwlock_r_sleep_cb(void *data)
{
    struct rwlock *rw = (struct rwlock *)data;
    __wrap_rwlock_runlock(rw);
    return 1;
}

void __wrap_rwlock_r_wake_cb(void *data, int status)
{
    if (status) {
        __wrap_rwlock_rlock((struct rwlock *)data);
    }
}
