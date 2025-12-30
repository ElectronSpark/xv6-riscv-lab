/*
 * Workqueue wrappers for unit tests
 * Provides mock workqueue behavior
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "workqueue.h"

struct queued_work_entry {
    struct workqueue *wq;
    struct work_struct *work;
};

static struct queued_work_entry g_pending_work = {0};
static bool g_fail_next_queue_work = false;

void pcache_test_fail_next_queue_work(void)
{
    g_fail_next_queue_work = true;
}

static void run_pending_work(void)
{
    if (g_pending_work.work == NULL) {
        return;
    }
    struct work_struct *work = g_pending_work.work;
    g_pending_work.work = NULL;
    work->func(work);
}

// Public function for tests and wrappers to trigger pending work execution
void pcache_test_run_pending_work(void)
{
    run_pending_work();
}

struct workqueue *__wrap_workqueue_create(const char *name, int max_active)
{
    struct workqueue *wq = calloc(1, sizeof(struct workqueue));
    if (wq == NULL) {
        return NULL;
    }
    
    if (name != NULL) {
        strncpy(wq->name, name, sizeof(wq->name) - 1);
    }
    wq->max_active = max_active;
    // Directly initialize lock for mock
    wq->lock.locked = 0;
    wq->lock.name = "workqueue_lock";
    
    return wq;
}

bool __wrap_queue_work(struct workqueue *wq, struct work_struct *work)
{
    if (wq == NULL || work == NULL) {
        return false;
    }
    
    if (g_fail_next_queue_work) {
        g_fail_next_queue_work = false;
        return false;
    }
    
    g_pending_work.wq = wq;
    g_pending_work.work = work;
    
    // Don't execute work synchronously - just mark as pending.
    // The caller will handle completion_reinit and then explicitly
    // wait for completion. This matches the async behavior where
    // work is queued and executed later.
    //run_pending_work();
    
    return true;
}

void __wrap_init_work_struct(struct work_struct *work, void (*func)(struct work_struct*), uint64 data)
{
    if (work == NULL) {
        return;
    }
    work->func = func;
    work->data = data;
}

struct work_struct *__wrap_create_work_struct(void (*func)(struct work_struct*), uint64 data)
{
    struct work_struct *work = calloc(1, sizeof(struct work_struct));
    if (work == NULL) {
        return NULL;
    }
    __wrap_init_work_struct(work, func, data);
    return work;
}

void __wrap_free_work_struct(struct work_struct *work)
{
    if (work != NULL) {
        free(work);
    }
}
