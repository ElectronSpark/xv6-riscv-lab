#ifndef __KERNEL_PGROUP_H
#define __KERNEL_PGROUP_H

#include "types.h"

struct thread;

/*
 * pgroup_init_thread - initialise pgid/sid for a newly-cloned thread
 *
 * Called from thread_clone() after the child thread is created.
 * The child inherits the parent's pgid and sid.
 */
void pgroup_init_thread(struct thread *child, struct thread *parent);

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

/*
 * proctab_for_each_rcu - iterate all threads under RCU protection
 *
 * The callback receives each thread and an opaque argument.
 * Caller must NOT hold pid_wlock (RCU read-side is sufficient).
 */
void proctab_for_each_rcu(void (*fn)(struct thread *, void *), void *arg);

#endif /* __KERNEL_PGROUP_H */
