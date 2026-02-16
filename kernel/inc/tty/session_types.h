#include "types.h"
#include "list_type.h"

struct thread;
struct pgroup;
struct tty;

struct session {
    pid_t sid;   /* session ID */
    int ref_cnt; /* reference count for this session */

    struct {
        uint64 exited : 1; /* whether the session has exited */
    };

    /* list of threads in this session */
    int t_cnt; /* number of live threads in this session */
    list_node_t threads;

    /* list of process groups structures in this session */
    int pg_cnt; /* number of process groups in this session */
    list_node_t pgrps;

    /* pointer to the foreground process group in this session */
    struct pgroup *fg_pgrp;
    /* pointer to the controlling terminal for this session (NULL if none) */
    struct tty *ctrl_tty;
};
