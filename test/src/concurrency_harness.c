/*
 * Concurrency harness implementation
 *
 * This file includes <pthread.h> FIRST, before any kernel headers, to avoid
 * the include-path conflict (kernel/inc/proc/sched.h vs /usr/include/sched.h).
 * All pthread-dependent logic lives here.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#include "concurrency_harness.h"

/* =========================================================================
 * Spinlock -> pthread_mutex hash table
 * ========================================================================= */

#define CONC_LOCK_TABLE_SIZE 64

typedef struct {
    void *key;              /* spinlock_t pointer (NULL = empty slot) */
    pthread_mutex_t mutex;
} conc_lock_entry_t;

typedef struct {
    conc_lock_entry_t table[CONC_LOCK_TABLE_SIZE];
    pthread_mutex_t table_lock;
} conc_lock_table_t;

static conc_lock_table_t g_conc_lock_table;

static void conc_lock_table_init(conc_lock_table_t *t) {
    pthread_mutex_init(&t->table_lock, NULL);
    memset(t->table, 0, sizeof(t->table));
}

static void conc_lock_table_destroy(conc_lock_table_t *t) {
    for (int i = 0; i < CONC_LOCK_TABLE_SIZE; i++) {
        if (t->table[i].key != NULL) {
            pthread_mutex_destroy(&t->table[i].mutex);
            t->table[i].key = NULL;
        }
    }
    pthread_mutex_destroy(&t->table_lock);
}

static pthread_mutex_t *conc_lock_table_get(conc_lock_table_t *t, void *key) {
    uint64_t hash = ((uint64_t)key >> 3) % CONC_LOCK_TABLE_SIZE;
    pthread_mutex_lock(&t->table_lock);
    for (int i = 0; i < CONC_LOCK_TABLE_SIZE; i++) {
        int idx = (hash + i) % CONC_LOCK_TABLE_SIZE;
        if (t->table[idx].key == key) {
            pthread_mutex_unlock(&t->table_lock);
            return &t->table[idx].mutex;
        }
        if (t->table[idx].key == NULL) {
            t->table[idx].key = key;
            pthread_mutex_init(&t->table[idx].mutex, NULL);
            pthread_mutex_unlock(&t->table_lock);
            return &t->table[idx].mutex;
        }
    }
    pthread_mutex_unlock(&t->table_lock);
    assert(0 && "conc_lock_table full");
    return NULL;
}

/* =========================================================================
 * tq -> pthread_cond hash table
 * ========================================================================= */

#define CONC_COND_TABLE_SIZE 64

typedef struct {
    void *key;              /* tq_t pointer (NULL = empty) */
    pthread_cond_t cond;
} conc_cond_entry_t;

typedef struct {
    conc_cond_entry_t table[CONC_COND_TABLE_SIZE];
    pthread_mutex_t table_lock;
} conc_cond_table_t;

static conc_cond_table_t g_conc_cond_table;

static void conc_cond_table_init(conc_cond_table_t *t) {
    pthread_mutex_init(&t->table_lock, NULL);
    memset(t->table, 0, sizeof(t->table));
}

static void conc_cond_table_destroy(conc_cond_table_t *t) {
    for (int i = 0; i < CONC_COND_TABLE_SIZE; i++) {
        if (t->table[i].key != NULL) {
            pthread_cond_destroy(&t->table[i].cond);
            t->table[i].key = NULL;
        }
    }
    pthread_mutex_destroy(&t->table_lock);
}

static pthread_cond_t *conc_cond_table_get(conc_cond_table_t *t, void *key) {
    uint64_t hash = ((uint64_t)key >> 3) % CONC_COND_TABLE_SIZE;
    pthread_mutex_lock(&t->table_lock);
    for (int i = 0; i < CONC_COND_TABLE_SIZE; i++) {
        int idx = (hash + i) % CONC_COND_TABLE_SIZE;
        if (t->table[idx].key == key) {
            pthread_mutex_unlock(&t->table_lock);
            return &t->table[idx].cond;
        }
        if (t->table[idx].key == NULL) {
            t->table[idx].key = key;
            pthread_cond_init(&t->table[idx].cond, NULL);
            pthread_mutex_unlock(&t->table_lock);
            return &t->table[idx].cond;
        }
    }
    pthread_mutex_unlock(&t->table_lock);
    assert(0 && "conc_cond_table full");
    return NULL;
}

/* =========================================================================
 * Global state
 * ========================================================================= */

_Bool g_concurrency_mode = false;

void concurrency_mode_enable(void) {
    conc_lock_table_init(&g_conc_lock_table);
    conc_cond_table_init(&g_conc_cond_table);
    __atomic_store_n(&g_concurrency_mode, true, __ATOMIC_SEQ_CST);
}

void concurrency_mode_disable(void) {
    __atomic_store_n(&g_concurrency_mode, false, __ATOMIC_SEQ_CST);
    conc_lock_table_destroy(&g_conc_lock_table);
    conc_cond_table_destroy(&g_conc_cond_table);
}

/* =========================================================================
 * Concurrency-aware spinlock
 * ========================================================================= */

void conc_spin_lock(void *lock_ptr) {
    pthread_mutex_t *m = conc_lock_table_get(&g_conc_lock_table, lock_ptr);
    pthread_mutex_lock(m);
}

void conc_spin_unlock(void *lock_ptr) {
    pthread_mutex_t *m = conc_lock_table_get(&g_conc_lock_table, lock_ptr);
    pthread_mutex_unlock(m);
}

/* =========================================================================
 * Concurrency-aware tq
 *
 * tq_wait semantics: release the associated spinlock, block on
 * the condvar, and leave the spinlock released on return (pcache caller
 * re-acquires tree_lock explicitly).
 * ========================================================================= */

void conc_tq_wait(void *queue_ptr, void *lock_ptr) {
    pthread_cond_t  *cv = conc_cond_table_get(&g_conc_cond_table, queue_ptr);
    pthread_mutex_t *m  = conc_lock_table_get(&g_conc_lock_table, lock_ptr);
    /* pthread_cond_wait atomically releases m and blocks on cv,
     * then re-acquires m upon wakeup.  We immediately unlock m to
     * match the kernel tq_wait semantic. */
    pthread_cond_wait(cv, m);
    pthread_mutex_unlock(m);
}

void conc_tq_wakeup_all(void *queue_ptr) {
    pthread_cond_t *cv = conc_cond_table_get(&g_conc_cond_table, queue_ptr);
    pthread_cond_broadcast(cv);
}

/* =========================================================================
 * Thread management
 * ========================================================================= */

static pthread_t g_threads[CONC_MAX_THREADS];

int conc_thread_create(int slot, conc_thread_fn_t fn, void *arg) {
    assert(slot >= 0 && slot < CONC_MAX_THREADS);
    return pthread_create(&g_threads[slot], NULL, fn, arg);
}

int conc_thread_join(int slot, void **retval) {
    assert(slot >= 0 && slot < CONC_MAX_THREADS);
    return pthread_join(g_threads[slot], retval);
}

/* =========================================================================
 * Barrier
 * ========================================================================= */

static pthread_barrier_t g_barrier;

int conc_barrier_init(int count) {
    return pthread_barrier_init(&g_barrier, NULL, count);
}

void conc_barrier_wait(void) {
    pthread_barrier_wait(&g_barrier);
}

void conc_barrier_destroy(void) {
    pthread_barrier_destroy(&g_barrier);
}

/* =========================================================================
 * Sleep helper
 * ========================================================================= */

void conc_sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
