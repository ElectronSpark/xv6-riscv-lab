#ifndef __KERNEL_SESSION_TYPES_H
#define __KERNEL_SESSION_TYPES_H

/**
 * @file session_types.h
 * @brief Terminal session — type definitions
 *
 * A session groups one or more process groups and optionally has a
 * controlling terminal.  The session leader (pid == sid) creates the
 * session via setsid().
 *
 * The session's foreground process group receives keyboard-generated
 * signals (SIGINT, SIGQUIT, SIGTSTP) from the controlling terminal.
 *
 * Hierarchy:  session → pgroup → thread_group → thread
 *
 * All hierarchy pointers and membership lists are protected by the
 * global pid_lock (rwlock).
 *   - pid_wlock for mutations (setsid, add/remove thread/pg)
 *   - pid_rlock for read-only access
 *
 * Lock ordering:  pid_lock > sigacts.lock > tcb_lock
 */

#include "types.h"
#include "list_type.h"

struct pgroup;
struct tty;

struct session {
    /* link in the global session list */
    list_node_t global_entry;

    pid_t sid;   /* Session ID (pid of session leader) */
    int ref_cnt; /* reference count for this session   */

    struct {
        uint64 exited : 1;    /* whether the session has exited */
        uint64 is_kernel : 1; /* set for kernel-internal sessions */
    };

    /* list of threads in this session (via thread->sid_entry) */
    int t_cnt; /* number of live threads in this session */
    list_node_t threads;

    /* list of process groups in this session (via pgroup->list_entry) */
    int pg_cnt; /* number of process groups in this session */
    list_node_t pgrps;

    /* pointer to the foreground process group in this session */
    struct pgroup *fg_pgrp;
    /* pointer to the controlling terminal for this session (NULL if none) */
    struct tty *ctrl_tty;
};

#endif /* __KERNEL_SESSION_TYPES_H */
