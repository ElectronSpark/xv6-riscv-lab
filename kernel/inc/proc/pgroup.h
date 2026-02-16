#ifndef __KERNEL_PGROUP_H
#define __KERNEL_PGROUP_H

#include "types.h"
#include "proc/thread_group_types.h"
#include "proc/pgroup_types.h"

struct thread;
void pgroup_init(struct thread *initproc);
struct pgroup *pgroup_alloc(pid_t pgid, struct thread_group *leader);

int pgroup_add_thread(struct pgroup *pg, struct thread *t);
void pgroup_remove_thread(struct thread *t);
int pgroup_add_tg(struct pgroup *pg, struct thread_group *tg);
void pgroup_remove_tg(struct thread_group *tg);
void pgroup_migrate_tg(struct thread_group *tg, struct pgroup *new_pg);

void pgroup_live_dec(struct pgroup *pg);

/*
 * Look up a process group by PGID.
 *
 * Caller must be inside rcu_read_lock() or hold pid_lock.
 * Returns pgroup pointer, or ERR_PTR(-ESRCH) if not found.
 */
struct pgroup *get_pgroup(pid_t pgid);

/*
 * setpgid - set the process group ID of a process
 *
 * If @pid is 0, use the caller's pid.
 * If @pgid is 0, use the target's pid as the new pgid (create new group).
 *
 * Constraints (POSIX):
 *   - May only set the pgid of itself or a child that has not yet exec'd.
 *   - The target must be in the same session as the caller.
 *   - Cannot change pgid of a session leader.
 *
 * Returns 0 on success, negative errno on failure.
 */
int pgroup_setpgid(pid_t pid, pid_t pgid);

/*
 * getpgid - get the process group ID of a process
 *
 * If @pid is 0, return the caller's pgid.
 * Returns the pgid, or negative errno on failure.
 */
pid_t pgroup_getpgid(pid_t pid);

/*
 * pgroup_kill - send a signal to every process in a process group
 *
 * Iterates the proc table under RCU, sending @signum to each
 * thread-group leader whose pgid matches @pgid.
 *
 * Returns 0 on success, negative errno if no processes were found.
 */
int pgroup_kill(pid_t pgid, int signum);

#endif /* __KERNEL_PGROUP_H */
