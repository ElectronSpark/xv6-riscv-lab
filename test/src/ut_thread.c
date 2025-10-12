#include "ut_thread.h"

#include <pthread.h>
#include <stdlib.h>

struct ut_thread {
    pthread_t tid;
};

int ut_thread_start(ut_thread_t **thread, ut_thread_fn fn, void *arg) {
    if (!thread || !fn) {
        return -1;
    }

    ut_thread_t *thr = calloc(1, sizeof(*thr));
    if (!thr) {
        return -1;
    }

    int rc = pthread_create(&thr->tid, NULL, fn, arg);
    if (rc != 0) {
        free(thr);
        return rc;
    }

    *thread = thr;
    return 0;
}

int ut_thread_join(ut_thread_t *thread, void **retval) {
    if (!thread) {
        return -1;
    }
    return pthread_join(thread->tid, retval);
}

void ut_thread_destroy(ut_thread_t *thread) {
    if (!thread) {
        return;
    }
    free(thread);
}
