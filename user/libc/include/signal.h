#ifndef _XV6_SIGNAL_H
#define _XV6_SIGNAL_H

#include "kernel/inc/signal_types.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sighandler_t)(int);

typedef enum {
    SIG_BLOCK = 0,
    SIG_UNBLOCK = 1,
    SIG_SETMASK = 2
} sigmask_action_t;

int raise(int sig);
sighandler_t signal(int sig, sighandler_t func);

#ifdef __cplusplus
}
#endif

#endif /* _XV6_SIGNAL_H */
