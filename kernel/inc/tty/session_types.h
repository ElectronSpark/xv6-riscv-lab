#ifndef __KERNEL_SESSION_TYPES_H
#define __KERNEL_SESSION_TYPES_H

#include "types.h"
#include "lock/spinlock.h"

struct tty;

/*
 * struct session - terminal session
 *
 * A session groups one or more process groups and optionally has a
 * controlling terminal.  The session leader (pid == sid) creates the
 * session via setsid().
 *
 * The session's foreground process group receives keyboard-generated
 * signals (SIGINT, SIGQUIT, SIGTSTP) from the controlling terminal.
 *
 * Protected by @lock for fg_pgid / ctrl_tty mutations.
 * The thread->session pointer (membership) is protected by pid_lock.
 */
struct session {
    pid_t sid;            /* Session ID (pid of session leader) */
    struct tty *ctrl_tty; /* Controlling terminal, may be NULL  */
    pid_t fg_pgid;        /* Foreground process group ID        */
    int ref_count;        /* Reference count                    */
    spinlock_t lock;      /* Protects ctrl_tty and fg_pgid      */

    struct {
        uint64 is_kernel : 1; /* set for kernel-internal sessions */
    };
};

#endif /* __KERNEL_SESSION_TYPES_H */
