#ifndef __KERNEL_PGROUP_H
#define __KERNEL_PGROUP_H

/**
 * @file pgroup.h
 * @brief Process group (POSIX job-control) — API
 *
 * A process group is a collection of thread groups (processes) used
 * for job control.  Each process group belongs to exactly one session.
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
#include "proc/thread_group_types.h"
#include "proc/pgroup_types.h"

struct thread;

/**
 * @brief Initialize the pgroup subsystem and create pgroup 1 for init.
 * Called once during boot after thread_group_init().
 * Caller must hold pid_wlock.
 */
void pgroup_init(struct thread *initproc);

/**
 * @brief Allocate a new process group.
 * @param pgid  The process group ID
 * @param leader  The thread group that leads this process group (may be NULL)
 * @return Allocated pgroup, or NULL on failure
 */
struct pgroup *pgroup_alloc(pid_t pgid, struct thread_group *leader);

/**
 * @brief Add a thread to a process group's thread list.
 * Caller must hold pid_wlock.
 */
int pgroup_add_thread(struct pgroup *pg, struct thread *t);

/**
 * @brief Remove a thread from its process group.
 * Caller must hold pid_wlock.
 */
void pgroup_remove_thread(struct thread *t);

/**
 * @brief Add a thread group (process) to a process group.
 * Caller must hold pid_wlock.
 */
int pgroup_add_tg(struct pgroup *pg, struct thread_group *tg);

/**
 * @brief Remove a thread group from its process group.
 * Triggers cleanup if the group becomes empty.
 * Caller must hold pid_wlock.
 */
void pgroup_remove_tg(struct thread_group *tg);

/**
 * @brief Migrate a thread group and all its member threads to a new pgroup.
 * Caller must hold pid_wlock.
 */
void pgroup_migrate_tg(struct thread_group *tg, struct pgroup *new_pg);

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
 * Snapshots the pgroup's thread_groups list under pid_rlock, then sends
 * @signum to each thread group after dropping pid_lock.
 *
 * Returns 0 on success, negative errno if no processes were found.
 */
int pgroup_kill(pid_t pgid, int signum);

/**
 * @brief Look up a process group by PGID.
 *
 * Finds the pgroup by looking up the group leader thread and returning
 * its pgroup pointer.
 *
 * Caller must be inside rcu_read_lock() or hold pid_lock.
 * Returns pgroup pointer, or NULL if not found.
 */
struct pgroup *get_pgroup(pid_t pgid);

#endif /* __KERNEL_PGROUP_H */
