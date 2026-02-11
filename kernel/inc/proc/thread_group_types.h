#ifndef __KERNEL_THREAD_GROUP_TYPES_H
#define __KERNEL_THREAD_GROUP_TYPES_H

/**
 * @file thread_group_types.h
 * @brief POSIX thread group (process) abstraction — type definitions
 *
 * A thread_group is the kernel representation of a POSIX "process".
 * All threads created with CLONE_THREAD share the same thread_group,
 * giving them the same TGID (which user-space sees as getpid()).
 *
 * Thread groups follow the Linux model:
 *  - TGID = PID of the group leader (the first thread)
 *  - kill(pid, sig) delivers to the thread group's shared_pending queue
 *  - Each thread still has its own TID (kernel pid) for tgkill/tkill
 *  - Shared pending signals are delivered to any eligible thread
 *  - exit_group() terminates all threads in the group
 *  - Only the group leader becomes a zombie visible to the parent's wait()
 *
 * Locking:
 *   All thread_group fields are protected by the global pid_lock (rwlock).
 *   pid_wlock must be held for mutations (add/remove members, group_exit).
 *   pid_rlock suffices for read-only access (signal delivery, queries).
 *   Shared pending signal enqueue/dequeue is serialized by sigacts->lock
 *   (which is shared among all threads in the group via CLONE_SIGHAND).
 *
 * Lock ordering:
 *   pid_lock > sigacts.lock > tcb_lock
 */

#include "types.h"
#include "list_type.h"
#include "signal_types.h"
#include <smp/atomic.h>

/**
 * @brief Shared signal state for a thread group (process-directed signals)
 *
 * This is analogous to Linux's struct signal_struct.shared_pending.
 * Process-directed signals (from kill()) go here; thread-directed signals
 * (from tgkill/tkill) go to the individual thread's signal.sig_pending.
 */
struct tg_shared_pending {
    sigset_t sig_pending_mask;      // Bitmask of pending shared signals
    sigpending_t sig_pending[NSIG]; // Per-signal queues (shared)
};

/**
 * @brief Thread group structure — the kernel's representation of a process
 *
 * All threads created with CLONE_THREAD share one thread_group instance.
 * Regular fork()/clone() without CLONE_THREAD creates a new thread_group
 * with a single thread.
 */
struct thread_group {
    // No per-object lock: all fields are protected by the global pid_lock
    // (rwlock). See file header for locking details.

    int tgid;                       // Thread group ID = leader's PID
    struct thread *group_leader;    // Thread that created this group (leader)
    list_node_t thread_list;        // List head for all threads in group
    _Atomic int live_threads;       // Number of live (non-exited) threads
    _Atomic int refcount;           // Reference count for lifetime management

    // Shared pending signals (process-directed, from kill())
    struct tg_shared_pending shared_pending;

    // Group-wide exit coordination
    _Atomic int group_exit;         // Non-zero if exit_group() has been called
    int group_exit_code;            // Exit code from exit_group()
    struct thread *group_exit_task; // Thread that initiated exit_group()

    // Group stop support  (SIGSTOP/SIGTSTP to the process)
    int group_stop_count;           // Threads still needing to stop
    int group_stop_signo;           // Signal that caused the group stop
};

#endif /* __KERNEL_THREAD_GROUP_TYPES_H */
