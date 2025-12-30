/*
 * Completion wrappers for unit tests
 * Provides mock completion synchronization
 */

#include <stddef.h>
#include <stdbool.h>
#include "types.h"
#include "completion.h"

void __wrap_completion_init(completion_t *c)
{
    if (c == NULL) {
        return;
    }
    c->done = 0;
    // Directly initialize lock for mock
    c->lock.locked = 0;
    c->lock.name = "completion_lock";
}

void __wrap_completion_reinit(completion_t *c)
{
    if (c == NULL) {
        return;
    }
    c->lock.locked = 1;  // Simulate acquire
    c->done = 0;
    c->lock.locked = 0;  // Simulate release
}

bool __wrap_try_wait_for_completion(completion_t *c)
{
    if (c == NULL) {
        return false;
    }
    c->lock.locked = 1;  // Simulate acquire
    bool ret = c->done > 0;
    if (ret) {
        c->done--;
    }
    c->lock.locked = 0;  // Simulate release
    return ret;
}

void __wrap_wait_for_completion(completion_t *c)
{
    if (c == NULL) {
        return;
    }
    
    // In unit tests with synchronous workqueue execution, we need to
    // run any pending work before waiting for completion
    extern void pcache_test_run_pending_work(void);
    pcache_test_run_pending_work();
    
    // Now check if completion was signaled
    c->lock.locked = 1;
    if (c->done > 0) {
        c->done--;
    }
    c->lock.locked = 0;
}

void __wrap_complete(completion_t *c)
{
    if (c == NULL) {
        return;
    }
    c->lock.locked = 1;  // Simulate acquire
    c->done++;
    c->lock.locked = 0;  // Simulate release
}

void __wrap_complete_all(completion_t *c)
{
    if (c == NULL) {
        return;
    }
    c->lock.locked = 1;  // Simulate acquire
    c->done = 0x7fffffff; // Max int
    c->lock.locked = 0;  // Simulate release
}

bool __wrap_completion_done(completion_t *c)
{
    if (c == NULL) {
        return false;
    }
    c->lock.locked = 1;  // Simulate acquire
    bool ret = c->done > 0;
    c->lock.locked = 0;  // Simulate release
    return ret;
}
