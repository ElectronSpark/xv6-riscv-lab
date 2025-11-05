#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "list.h"
#include "spinlock.h"
#include "page.h"
#include "page_type.h"
#include "slab.h"
#include "workqueue.h"
#include "completion.h"
#include "timer.h"
#include "bio.h"

#define KERNEL_STACK_ORDER 2

struct queued_work_entry {
    struct workqueue *wq;
    struct work_struct *work;
};

static struct queued_work_entry g_pending_work = {0};
static bool g_fail_next_queue_work = false;

// Global flag to simulate allocation failure in tests
static bool g_test_fail_page_alloc = false;
static bool g_test_fail_slab_alloc = false;
static bool g_test_break_on_sleep = false;
static int g_test_sleep_call_count = 0;
static int g_test_max_sleep_calls = 1;

void pcache_test_fail_next_queue_work(void) {
    g_fail_next_queue_work = true;
}

static void run_pending_work(void) {
    if (g_pending_work.work == NULL) {
        return;
    }
    struct work_struct *work = g_pending_work.work;
    g_pending_work.work = NULL;
    work->func(work);
}

// -----------------------------------------------------------------------------
// Panic helpers
// -----------------------------------------------------------------------------
void __panic_start(void) {
    // no-op for host tests
}

void __panic_end(void) __attribute__((noreturn));
void __panic_end(void) {
    abort();
}

// -----------------------------------------------------------------------------
// Spinlock stubs
// -----------------------------------------------------------------------------
void spin_init(struct spinlock *lock, char *name) {
    if (lock == NULL) {
        return;
    }
    lock->locked = 0;
    lock->name = name;
    lock->cpu = NULL;
}

void spin_acquire(struct spinlock *lock) {
    if (lock == NULL) {
        return;
    }
    lock->locked = 1;
}

void spin_release(struct spinlock *lock) {
    if (lock == NULL) {
        return;
    }
    lock->locked = 0;
}

int spin_holding(struct spinlock *lock) {
    if (lock == NULL) {
        return 0;
    }
    return lock->locked != 0;
}

// -----------------------------------------------------------------------------
// Page helpers
// -----------------------------------------------------------------------------
void page_lock_acquire(page_t *page) {
    if (page == NULL) {
        return;
    }
    spin_acquire(&page->lock);
}

void page_lock_release(page_t *page) {
    if (page == NULL) {
        return;
    }
    spin_release(&page->lock);
}

void page_lock_assert_holding(page_t *page) {
    if (page == NULL) {
        return;
    }
    assert(spin_holding(&page->lock));
}

void page_lock_assert_unholding(page_t *page) {
    if (page == NULL) {
        return;
    }
    assert(!spin_holding(&page->lock));
}

int page_ref_count(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    return page->ref_count;
}

int page_ref_inc_unlocked(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    return ++page->ref_count;
}

int page_ref_dec_unlocked(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    return --page->ref_count;
}

int __page_ref_dec(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    page_lock_acquire(page);
    if (page->ref_count > 0) {
        page->ref_count--;
    }
    int count = page->ref_count;
    page_lock_release(page);
    if (count == 0) {
        if (page->pcache.pcache_node != NULL) {
            slab_free(page->pcache.pcache_node);
            page->pcache.pcache_node = NULL;
        }
        free(page);
    }
    return count;
}

int page_refcnt(void *physical) {
    return physical == NULL ? -1 : 1;
}

int page_ref_inc(void *ptr) {
    (void)ptr;
    return -1;
}

int page_ref_dec(void *ptr) {
    (void)ptr;
    return -1;
}

uint64 __page_to_pa(page_t *page) {
    if (page == NULL) {
        return 0;
    }
    return page->physical_address;
}

page_t *__pa_to_page(uint64 physical) {
    return (page_t *)(uintptr_t)physical;
}

void *page_alloc(uint64 order, uint64 flags) {
    (void)order;
    page_t *page = calloc(1, sizeof(page_t));
    if (page == NULL) {
        return NULL;
    }
    page->ref_count = 1;
    page->physical_address = (uint64)(uintptr_t)page;
    page->flags = 0;
    PAGE_FLAG_SET_TYPE(page->flags, flags);
    spin_init(&page->lock, "page_lock");
    return page;
}

void page_free(void *ptr, uint64 order) {
    (void)order;
    free(ptr);
}

// Page allocation for tests
page_t *ut_make_mock_page(uint64 order, uint64 flags) {
    (void)order;
    page_t *page = calloc(1, sizeof(page_t));
    if (page == NULL) {
        return NULL;
    }
    page->ref_count = 1;
    page->physical_address = (uint64)(uintptr_t)page;
    page->flags = 0;
    PAGE_FLAG_SET_TYPE(page->flags, flags);
    spin_init(&page->lock, "page_lock");
    return page;
}

page_t *__page_alloc(uint64 order, uint64 flags) {
    if (g_test_fail_page_alloc) {
        g_test_fail_page_alloc = false;
        return NULL;
    }
    return ut_make_mock_page(order, flags);
}

// -----------------------------------------------------------------------------
// Slab helpers
// -----------------------------------------------------------------------------
int slab_cache_init(slab_cache_t *cache, char *name, size_t obj_size, uint64 flags) {
    if (cache == NULL || obj_size == 0) {
        return -1;
    }
    memset(cache, 0, sizeof(*cache));
    cache->name = name;
    cache->flags = flags;
    cache->obj_size = obj_size;
    list_entry_init(&cache->free_list);
    list_entry_init(&cache->partial_list);
    list_entry_init(&cache->full_list);
    spin_init(&cache->lock, "slab_cache_lock");
    return 0;
}

slab_cache_t *slab_cache_create(char *name, size_t obj_size, uint64 flags) {
    slab_cache_t *cache = calloc(1, sizeof(slab_cache_t));
    if (cache == NULL) {
        return NULL;
    }
    if (slab_cache_init(cache, name, obj_size, flags) != 0) {
        free(cache);
        return NULL;
    }
    return cache;
}

int slab_cache_destroy(slab_cache_t *cache) {
    free(cache);
    return 0;
}

int slab_cache_shrink(slab_cache_t *cache, int nums) {
    (void)cache;
    (void)nums;
    return 0;
}

void *slab_alloc(slab_cache_t *cache) {
    if (cache == NULL || cache->obj_size == 0) {
        return NULL;
    }
    if (g_test_fail_slab_alloc) {
        g_test_fail_slab_alloc = false;
        return NULL;
    }
    return calloc(1, cache->obj_size);
}

void slab_free(void *obj) {
    free(obj);
}

// -----------------------------------------------------------------------------
// Workqueue helpers
// -----------------------------------------------------------------------------
struct workqueue *workqueue_create(const char *name, int max_active) {
    (void)max_active;
    struct workqueue *wq = calloc(1, sizeof(struct workqueue));
    if (wq == NULL) {
        return NULL;
    }
    spin_init(&wq->lock, "workqueue_lock");
    list_entry_init(&wq->worker_list);
    list_entry_init(&wq->work_list);
    list_entry_init(&wq->idle_queue.head);
    wq->idle_queue.counter = 0;
    wq->idle_queue.name = name;
    wq->idle_queue.lock = &wq->lock;
    wq->active = 1;
    wq->max_active = max_active;
    wq->min_active = 0;
    if (name != NULL) {
        strncpy(wq->name, name, WORKQUEUE_NAME_MAX);
        wq->name[WORKQUEUE_NAME_MAX] = '\0';
    }
    return wq;
}

bool queue_work(struct workqueue *wq, struct work_struct *work) {
    if (wq == NULL || work == NULL || work->func == NULL) {
        return false;
    }
    if (g_fail_next_queue_work) {
        g_fail_next_queue_work = false;
        return false;
    }
    run_pending_work();
    g_pending_work.wq = wq;
    g_pending_work.work = work;
    return true;
}

void init_work_struct(struct work_struct *work, void (*func)(struct work_struct*), uint64 data) {
    if (work == NULL) {
        return;
    }
    list_entry_init(&work->entry);
    work->func = func;
    work->data = data;
}

struct work_struct *create_work_struct(void (*func)(struct work_struct*), uint64 data) {
    struct work_struct *work = calloc(1, sizeof(struct work_struct));
    if (work == NULL) {
        return NULL;
    }
    init_work_struct(work, func, data);
    return work;
}

void free_work_struct(struct work_struct *work) {
    free(work);
}

// -----------------------------------------------------------------------------
// Completion helpers
// -----------------------------------------------------------------------------
void completion_init(completion_t *c) {
    if (c == NULL) {
        return;
    }
    c->done = 0;
}

void completion_reinit(completion_t *c) {
    if (c == NULL) {
        return;
    }
    c->done = 0;
}

bool try_wait_for_completion(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    run_pending_work();
    if (c->done > 0) {
        c->done--;
        return true;
    }
    return false;
}

void wait_for_completion(completion_t *c) {
    if (c == NULL) {
        return;
    }
    run_pending_work();
    if (c->done == 0) {
        c->done = 1;
    }
}

void complete(completion_t *c) {
    if (c == NULL) {
        return;
    }
    if (c->done < INT_MAX) {
        c->done++;
    }
}

void complete_all(completion_t *c) {
    if (c == NULL) {
        return;
    }
    c->done = INT_MAX;
}

bool completion_done(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    return c->done == 0;
}

// -----------------------------------------------------------------------------
// Timer helpers
// -----------------------------------------------------------------------------
uint64 get_jiffs(void) {
    static uint64 counter = 1;
    return counter++;
}

void sleep_ms(uint64 ms) {
    (void)ms;
    run_pending_work();
}

// -----------------------------------------------------------------------------
// Process stubs
// -----------------------------------------------------------------------------
struct proc;

int kernel_proc_create(const char *name, struct proc **retp, void *entry,
                       uint64 arg1, uint64 arg2, int stack_order) {
    (void)name;
    (void)entry;
    (void)arg1;
    (void)arg2;
    (void)stack_order;
    if (retp != NULL) {
        *retp = (struct proc *)0x1;
    }
    return 1;
}

void wakeup_proc(struct proc *p) {
    (void)p;
}

struct proc *myproc(void) {
    return (struct proc *)0x2;
}

void wakeup_on_chan(void *chan) {
    (void)chan;
}

void pcache_test_set_break_on_sleep(bool enable) {
    g_test_break_on_sleep = enable;
    g_test_sleep_call_count = 0;
}

void pcache_test_set_max_sleep_calls(int max_calls) {
    g_test_max_sleep_calls = max_calls;
}

void pcache_test_fail_next_page_alloc(void) {
    g_test_fail_page_alloc = true;
}

void pcache_test_fail_next_slab_alloc(void) {
    g_test_fail_slab_alloc = true;
}

void sleep_on_chan(void *chan, struct spinlock *lk) {
    (void)chan;
    (void)lk;
    
    if (g_test_break_on_sleep) {
        g_test_sleep_call_count++;
        if (g_test_sleep_call_count >= g_test_max_sleep_calls) {
            // Set flag to make next allocation fail and exit the function
            g_test_fail_page_alloc = true;
            return;
        }
    }
}

// -----------------------------------------------------------------------------
// Misc helpers
// -----------------------------------------------------------------------------
void panic_disable_bt(void) {
}

int panic_state(void) {
    return 0;
}
