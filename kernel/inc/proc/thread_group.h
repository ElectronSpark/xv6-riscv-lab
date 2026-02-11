#ifndef __KERNEL_THREAD_GROUP_H
#define __KERNEL_THREAD_GROUP_H

/**
 * @file thread_group.h
 * @brief POSIX thread group (process) abstraction — API
 *
 * Provides the public API for managing thread groups. A thread group
 * represents a POSIX "process" containing one or more threads that
 * share a common TGID, signal actions, VM, and file descriptors.
 *
 * Key operations:
 *  - thread_group_alloc()  — create a new thread group for a leader thread
 *  - thread_group_add()    — add a CLONE_THREAD child to an existing group
 *  - thread_group_remove() — remove a thread from its group on exit
 *  - thread_group_put()    — drop a reference (frees when refcount → 0)
 *  - thread_group_exit()   — initiate group-wide exit (exit_group)
 *
 * Signal helpers:
 *  - tg_signal_send()      — send a process-directed signal to a thread group
 *  - tg_signal_pick_thread() — pick a thread to handle a shared pending signal
 *  - tg_recalc_sigpending()  — recalc SIGPENDING for all threads in group
 *
 * Locking:
 *   All thread_group operations are serialized by the global pid_lock (rwlock).
 *   - pid_wlock for mutations: thread_group_add(), thread_group_remove(),
 *     thread_group_exit()
 *   - pid_rlock for reads: tg_signal_send(), tg_signal_pending(), queries
 *   - sigacts->lock for shared_pending enqueue/dequeue (shared via CLONE_SIGHAND)
 *
 * Lock ordering:
 *   pid_lock > sigacts.lock > tcb_lock
 */

#include "proc/thread_group_types.h"

struct thread;
struct thread_group;
struct ksiginfo;

/**
 * @brief Allocate and initialize a new thread group.
 *
 * The given thread becomes the group leader with tgid = thread's pid.
 * Initial live_threads = 1, refcount = 1.
 * Called during fork/clone when CLONE_THREAD is NOT set.
 *
 * @param leader  The thread that will lead this group
 * @return 0 on success, negative errno on failure
 */
int thread_group_alloc(struct thread *leader);

/**
 * @brief Add a thread to an existing thread group.
 *
 * Called during clone() when CLONE_THREAD IS set.
 * The child shares the parent's thread_group.
 * Increments live_threads and refcount.
 * Caller must hold pid_wlock.
 *
 * @param tg     The thread group to join
 * @param child  The new thread to add
 */
void thread_group_add(struct thread_group *tg, struct thread *child);

/**
 * @brief Remove a thread from its thread group.
 *
 * Called during thread exit. Decrements live_threads.
 * If this was the last live thread, marks the group for cleanup.
 * Does NOT free the thread_group — use thread_group_put() for that.
 * Caller must hold pid_wlock.
 *
 * @param p  The thread exiting the group
 * @return true if this was the last live thread in the group
 */
bool thread_group_remove(struct thread *p);

/**
 * @brief Drop a reference to a thread group. Frees when refcount → 0.
 * @param tg  The thread group
 */
void thread_group_put(struct thread_group *tg);

/**
 * @brief Get a reference to a thread group (increment refcount).
 * @param tg  The thread group
 */
void thread_group_get(struct thread_group *tg);

/**
 * @brief Initialize the thread group subsystem (slab cache, etc.).
 */
void thread_group_init(void);

/**
 * @brief Initiate group-wide exit.
 *
 * Sets group_exit flag and sends SIGKILL to all other threads in the group.
 * Called by exit_group() syscall. Only the first caller's exit_code is used.
 * Acquires pid_rlock internally to iterate thread list.
 *
 * @param p     The thread calling exit_group()
 * @param code  The exit code
 */
void thread_group_exit(struct thread *p, int code);

/**
 * @brief Check if the thread group is in group-exit mode.
 * @param tg  The thread group
 * @return true if exit_group() has been called
 */
static inline bool thread_group_exiting(struct thread_group *tg) {
    if (tg == NULL) return false;
    return __atomic_load_n(&tg->group_exit, __ATOMIC_ACQUIRE) != 0;
}

/**
 * @brief Check if this thread is the group leader.
 * @param p  The thread to check
 * @return true if p is its thread group's leader
 */
bool thread_is_group_leader(struct thread *p);

/**
 * @brief Get the TGID (process ID) for user-space getpid().
 * @param p  The thread
 * @return The TGID, or p->pid if no thread group
 */
int thread_tgid(struct thread *p);

// ───── Signal operations on thread groups ─────

/**
 * @brief Send a process-directed signal to a thread group.
 *
 * The signal is added to the group's shared_pending queue.
 * An eligible thread is chosen and woken to handle it.
 * This is what kill(tgid, sig) calls.
 *
 * @param tg    The target thread group
 * @param info  The signal info (must be pre-allocated)
 * @return 0 on success, negative errno on failure
 */
int tg_signal_send(struct thread_group *tg, struct ksiginfo *info);

/**
 * @brief Initialize shared pending signals for a thread group.
 * @param tg  The thread group
 */
void tg_shared_pending_init(struct thread_group *tg);

/**
 * @brief Clean up shared pending signals for a thread group.
 * @param tg  The thread group
 */
void tg_shared_pending_destroy(struct thread_group *tg);

/**
 * @brief Check if the thread group has shared pending signals.
 * @param tg  The thread group
 * @return true if there are unmasked shared pending signals
 */
bool tg_signal_pending(struct thread_group *tg, struct thread *p);

/**
 * @brief Dequeue a shared pending signal for delivery.
 *
 * Removes the signal from shared_pending and returns its ksiginfo.
 * Caller must hold sigacts lock. Caller must hold pid_rlock or pid_wlock.
 *
 * @param tg     The thread group
 * @param signo  The signal number to dequeue
 * @return The dequeued ksiginfo, or NULL
 */
struct ksiginfo *tg_dequeue_signal(struct thread_group *tg, int signo);

/**
 * @brief Recalculate SIGPENDING flag for all threads in the group.
 *
 * Called after changes to shared_pending or signal masks.
 * Caller must hold pid_rlock or pid_wlock.
 *
 * @param tg  The thread group
 */
void tg_recalc_sigpending(struct thread_group *tg);

#endif /* __KERNEL_THREAD_GROUP_H */
