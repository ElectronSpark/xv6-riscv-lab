#ifndef __KERNEL_SIGNAL_TYPES_H
#define __KERNEL_SIGNAL_TYPES_H

#include "types.h"
#include "signo.h"
#include "lock/spinlock.h"
#include "list_type.h"
#include "trapframe.h"
#include "lock/spinlock.h"
#include "uabi/signal.h"
#include "list_type.h"

typedef struct sigacts {
    spinlock_t lock; // protect the sigacts structure
    struct sigaction sa[NSIG + 1];
    // NOTE: Signal masks (blocked set) are per-thread, stored in
    //       thread_signal_t::sig_mask / sig_saved_mask.
    sigset_t sa_sigterm; // signals that terminate the process
    // sigset_t sa_usercatch;  // user-defined signal handlers
    sigset_t sa_sigstop; // signals that stop the process
    sigset_t sa_sigcont; // signals that continue the process
    // sigset_t sa_sigcore;    // signals that generate a core dump (not used)
    sigset_t sa_sigignore; // signals ignored by this process
    _Atomic int refcount;  // reference count for shared usage
} sigacts_t;

typedef struct sigpending {
    list_node_t queue;
} sigpending_t;

// Thread local signal state
// Protected by sigacts lock
typedef struct thread_signal {
    sigset_t sig_mask;         // signals currently blocked (per-thread)
    sigset_t sig_saved_mask;   // saved mask (restored after sigsuspend/handler)
    sigset_t sig_pending_mask; // Mark none empty signal pending queue
    sigpending_t sig_pending[NSIG]; // Queue of pending signals
    // signal trap frames would be put at the user stack.
    // This is used to restore the user context when a signal is delivered.
    uint64 sig_ucontext; // Address of the signal user context
    stack_t sig_stack;   // Alternate signal stack
    uint64 esignal;      // Signal to be sent to parent on exit
    int stop_signal;     // Signal number that caused THREAD_STOPPED
} thread_signal_t;

typedef struct ksiginfo {
    list_node_t list_entry;
    struct thread *receiver;
    struct thread *sender; // Process that sent the signal. May be NULL
    int signo;             // Signal number
    siginfo_t info;        // Signal information
} ksiginfo_t;

#endif /* __KERNEL_SIGNAL_TYPES_H */
