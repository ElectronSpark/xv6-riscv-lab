#ifndef __KERNEL_PROCESS_GROUP_TYPES_H
#define __KERNEL_PROCESS_GROUP_TYPES_H

/**
 * @file pgroup_types.h
 * @brief Process group (POSIX job-control) — type definitions
 *
 * A process group is a collection of thread groups (processes) used for
 * job control.  Each process group belongs to exactly one session.
 *
 * Hierarchy:  session → pgroup → thread_group → thread
 *
 * All hierarchy pointers are protected by the global pid_lock (rwlock):
 *   - pid_wlock for mutations (setpgid, add/remove)
 *   - pid_rlock for reads and signal delivery
 *
 * Lock ordering:  pid_lock > sigacts.lock > tcb_lock
 */

#include "types.h"
#include "list_type.h"

struct session;
struct thread_group;

struct pgroup {
    /* list entry for the list of process groups in the session */
    list_node_t list_entry;
    pid_t pgid; /* process group ID */
    /* pointer to the thread group that is the leader of this process group */
    struct thread_group *leader;

    struct {
        uint64 exited : 1; /* whether the process group has exited */
        uint64 is_kernel : 1;
    };

    /* list of threads in this group (via thread->pg_entry) */
    int t_cnt; /* number of live threads in this process group */
    list_node_t threads;

    /* list of thread group structures in this group (via tg->list_entry) */
    int p_cnt;
    list_node_t thread_groups;
    struct session *session; /* pointer to the session this group belongs to */
};

#endif /* __KERNEL_PROCESS_GROUP_TYPES_H */
