#ifndef __KERNEL_SIGNAL_TYPES_H
#define __KERNEL_SIGNAL_TYPES_H

#include "types.h"
#include "signo.h"
#include "lock/spinlock.h"
#include "list_type.h"
#include "trapframe.h"

#if !defined(ON_HOST_OS)
typedef uint64 sigset_t;
#endif

typedef union sigval {
	int sival_int;
	void *sival_ptr;
} sigval_t;

typedef struct siginfo siginfo_t;

typedef struct sigaction {
	union {
		void (*sa_handler)(int);
		void (*sa_sigaction)(int, siginfo_t *, void *);
	};
    sigset_t    sa_mask;
    int         sa_flags;
} sigaction_t;

typedef struct sigacts {
	spinlock_t lock; // protect the sigacts structure
    struct sigaction sa[NSIG+1];
	sigset_t sa_sigmask;  // signals currently blocked at the process level
	sigset_t sa_original_mask; // original signal mask before any changes
	sigset_t sa_sigterm;    // signals that terminate the process
	// sigset_t sa_usercatch;  // user-defined signal handlers
	sigset_t sa_sigstop;    // signals that stop the process
	sigset_t sa_sigcont;    // signals that continue the process
	// sigset_t sa_sigcore;    // signals that generate a core dump (not used)
    sigset_t sa_sigignore;  // signals ignored by this process
	_Atomic int refcount; // reference count for shared usage
} sigacts_t;

typedef struct stack {
	void *ss_sp;    /* stack base pointer */
	int ss_flags;   /* flags */
#define SS_AUTOREARM 0x1 /* automatically rearm the signal stack */
#define SS_ONSTACK   0x2 /* use the alternate stack */
#define SS_DISABLE   0x4 /* disable the signal stack */
	size_t ss_size; /* size */
} stack_t;

typedef struct sigpending {
	list_node_t queue;
} sigpending_t;

// Thread local signal state
// Protected by sigacts lock
typedef struct thread_signal {
	sigset_t sig_pending_mask;      // Mark none empty signal pending queue
    sigpending_t sig_pending[NSIG]; // Queue of pending signals
    // signal trap frames would be put at the user stack.
    // This is used to restore the user context when a signal is delivered.
    uint64 sig_ucontext; // Address of the signal user context
    stack_t sig_stack;   // Alternate signal stack
    uint64 esignal;     // Signal to be sent to parent on exit
} thread_signal_t;

typedef enum {
    SIG_ACT_INVALID = -1,
    SIG_ACT_IGN = 0,
    SIG_ACT_TERM,
    SIG_ACT_CORE,
	SIG_ACT_STOP,
    SIG_ACT_CONT
} sig_defact;

// The following structures are copied from Advanced Programming in the UNIX Environment
// @TODO: currently unused
typedef struct siginfo {
	int si_signo; /* signal number */
	int si_errno; /* if nonzero, errno value from errno.h */
	int si_code; /* additional info (depends on signal) */
	int si_pid; /* sending process ID */
	// int si_uid; /* sending process real user ID */
	void *si_addr; /* address that caused the fault */
	int si_status; /* exit value */
	union sigval si_value; /* application-specific value */
	/* possibly other fields also */
} siginfo_t;

typedef struct utrapframe mcontext_t;

typedef struct ucontext {
	struct ucontext *uc_link; /* pointer to context resumed when */
	/* this context returns */
	sigset_t uc_sigmask; /* signals blocked when this context */
	/* is active */
	stack_t uc_stack; /* stack used by this context */
	mcontext_t uc_mcontext; /* machine-specific representation of */
	/* saved context */
} ucontext_t;

typedef struct ksiginfo {
	list_node_t list_entry;
	struct thread *receiver;
	struct thread *sender;	// Process that sent the signal. May be NULL
	int signo;          	// Signal number
	siginfo_t info;    		// Signal information
} ksiginfo_t;

#endif /* __KERNEL_SIGNAL_TYPES_H */
