#ifndef UT_THREAD_H
#define UT_THREAD_H

#include <stddef.h>

typedef struct ut_thread ut_thread_t;

typedef void *(*ut_thread_fn)(void *);

int ut_thread_start(ut_thread_t **thread, ut_thread_fn fn, void *arg);
int ut_thread_join(ut_thread_t *thread, void **retval);
void ut_thread_destroy(ut_thread_t *thread);

#endif /* UT_THREAD_H */
