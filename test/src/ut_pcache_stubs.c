#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <assert.h>

#include "types.h"
#include "completion.h"
#include "completion_types.h"
#include "list.h"
#include "bintree.h"
#include "mutex_types.h"
#include "page.h"
#include "workqueue.h"
#include "workqueue_types.h"

bool __ut_queue_work_execute_immediately = false;

static struct work_struct *__ut_last_work = NULL;

typedef struct stub_mutex_entry {
    mutex_t *mutex;
    atomic_flag flag;
    struct stub_mutex_entry *next;
} stub_mutex_entry_t;

static atomic_flag __stub_mutex_lock = ATOMIC_FLAG_INIT;
static stub_mutex_entry_t *__stub_mutex_entries = NULL;

static stub_mutex_entry_t *__stub_find_mutex_entry(mutex_t *lk) {
    stub_mutex_entry_t *entry = __stub_mutex_entries;
    while (entry != NULL) {
        if (entry->mutex == lk) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static void __stub_lock_registry(void) {
    while (atomic_flag_test_and_set_explicit(&__stub_mutex_lock, memory_order_acquire)) {
    }
}

static void __stub_unlock_registry(void) {
    atomic_flag_clear_explicit(&__stub_mutex_lock, memory_order_release);
}

void __ut_reset_workqueue_stub(void) {
    __ut_last_work = NULL;
    __ut_queue_work_execute_immediately = false;
}

void __ut_run_queued_work(void) {
    if (__ut_last_work != NULL && __ut_last_work->func != NULL) {
        struct work_struct *work = __ut_last_work;
        __ut_last_work = NULL;
        work->func(work);
    }
}

int mutex_lock(mutex_t *lk) {
    if (lk == NULL) {
        return -1;
    }
    __stub_lock_registry();
    stub_mutex_entry_t *entry = __stub_find_mutex_entry(lk);
    __stub_unlock_registry();
    if (entry == NULL) {
        return -1;
    }
    while (atomic_flag_test_and_set_explicit(&entry->flag, memory_order_acquire)) {
    }
    return 0;
}

void mutex_unlock(mutex_t *lk) {
    if (lk == NULL) {
        return;
    }
    __stub_lock_registry();
    stub_mutex_entry_t *entry = __stub_find_mutex_entry(lk);
    __stub_unlock_registry();
    if (entry == NULL) {
        return;
    }
    atomic_flag_clear_explicit(&entry->flag, memory_order_release);
}

void mutex_init(mutex_t *lk, char *name) {
    if (lk == NULL) {
        return;
    }
    memset(lk, 0, sizeof(*lk));
    lk->name = name;
    stub_mutex_entry_t *entry = malloc(sizeof(*entry));
    assert(entry != NULL);
    entry->mutex = lk;
    entry->flag = (atomic_flag)ATOMIC_FLAG_INIT;
    __stub_lock_registry();
    entry->next = __stub_mutex_entries;
    __stub_mutex_entries = entry;
    __stub_unlock_registry();
}

void completion_init(completion_t *c) {
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
}

void completion_reinit(completion_t *c) {
    if (c == NULL) {
        return;
    }
    c->done = 0;
}

void complete_all(completion_t *c) {
    if (c == NULL) {
        return;
    }
    c->done = 1;
}

void complete(completion_t *c) {
    complete_all(c);
}

void wait_for_completion(completion_t *c) {
    if (c == NULL) {
        return;
    }

    while (!c->done) {
        __ut_run_queued_work();
    }
}

bool try_wait_for_completion(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    bool was_done = c->done != 0;
    c->done = 0;
    return was_done;
}

bool completion_done(completion_t *c) {
    if (c == NULL) {
        return false;
    }
    return c->done != 0;
}

struct workqueue *workqueue_create(const char *name, int max_active) {
    (void)name;
    (void)max_active;
    struct workqueue *wq = malloc(sizeof(struct workqueue));
    if (wq == NULL) {
        return NULL;
    }
    memset(wq, 0, sizeof(*wq));
    wq->active = 1;
    return wq;
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
    struct work_struct *work = malloc(sizeof(struct work_struct));
    if (work == NULL) {
        return NULL;
    }
    init_work_struct(work, func, data);
    return work;
}

void free_work_struct(struct work_struct *work) {
    free(work);
}

bool queue_work(struct workqueue *wq, struct work_struct *work) {
    (void)wq;
    if (work == NULL || work->func == NULL) {
        return false;
    }
    __ut_last_work = work;
    return true;
}

void page_lock_acquire(page_t *page) {
    (void)page;
}

void page_lock_release(page_t *page) {
    (void)page;
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
    page->ref_count += 1;
    return page->ref_count;
}

int page_ref_dec_unlocked(page_t *page) {
    if (page == NULL) {
        return -1;
    }
    page->ref_count -= 1;
    return page->ref_count;
}

page_t *__page_alloc(uint64 order, uint64 flags) {
    (void)order;
    page_t *page = calloc(1, sizeof(page_t));
    if (page == NULL) {
        return NULL;
    }
    page->flags = flags;
    page->ref_count = 1;
    rb_node_init(&page->pcache.node);
    list_entry_init(&page->pcache.lru_entry);
    list_entry_init(&page->pcache.dirty_entry);
    return page;
}

void __page_free(page_t *page, uint64 order) {
    (void)order;
    free(page);
}
