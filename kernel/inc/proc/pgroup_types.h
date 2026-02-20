#ifndef __KERNEL_PROCESS_GROUP_TYPES_H
#define __KERNEL_PROCESS_GROUP_TYPES_H

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

    /* list of threads in this group */
    int t_cnt; /* number of live threads in this process group */
    list_node_t threads;

    /* list of thread groups structures in this group */
    int p_cnt;
    list_node_t thread_groups;
    struct session *session; /* pointer to the session this group belongs to */
};

#endif /* __KERNEL_PROCESS_GROUP_TYPES_H */
