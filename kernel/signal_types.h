#ifndef __KERNEL_SIGNAL_TYPES_H
#define __KERNEL_SIGNAL_TYPES_H

#include "types.h"
#include "signo.h"
#include "spinlock.h"
#include "list_type.h"
#include "trapframe.h"

typedef uint64 sigset_t;
struct proc;

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

#define SA_NOCLDSTOP 0x00000001 // Don't receive SIGCHLD when children stop.
#define SA_NOCLDWAIT 0x00000002 // Don't create zombie processes on child exit.
#define SA_SIGINFO   0x00000004 // Use sa_sigaction instead of sa_handler.
// #define SA_ONSTACK   0x00000008 // Use alternate signal stack.
// #define SA_RESTART   0x00000010 // Restart system calls if interrupted by handler
#define SA_NODEFER   0x00000020 // Don't block the signal in the handler.
#define SA_RESETHAND 0x00000040 // Reset the signal handler to SIG_DFL after the first delivery.


typedef struct sigacts {
    struct sigaction sa[NSIG+1];
	sigset_t sa_sigmask;  // signals currently blocked at the process level
    sigset_t sa_sigpending;  // signals pending for this process
	sigset_t sa_sigblock;   // signals blocked by this process
	sigset_t sa_sigterm;    // signals that terminate the process
	sigset_t sa_usercatch;  // user-defined signal handlers
	// sigset_t sa_sigstop;    // signals that stop the process
	// sigset_t sa_sigcont;    // signals that continue the process (not used)
	// sigset_t sa_sigcore;    // signals that generate a core dump (not used)
    sigset_t sa_sigignore;  // signals ignored by this process
} sigacts_t;

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

typedef struct stack {
	void *ss_sp;    /* stack base pointer */
	int ss_flags;   /* flags */
	size_t ss_size; /* size */
} stack_t;

typedef struct trapframe mcontext_t;

typedef struct ucontext {
	struct ucontext *uc_link; /* pointer to context resumed when */
	/* this context returns */
	sigset_t uc_sigmask; /* signals blocked when this context */
	/* is active */
	stack_t uc_stack; /* stack used by this context */
	mcontext_t uc_mcontext; /* machine-specific representation of */
	/* saved context */
} ucontext_t;

typedef struct sigqueue {
	list_node_t queue;
	int count;
} sigqueue_t;

typedef struct ksiginfo {
	list_node_t list_entry;
	struct proc *receiver;
	struct proc *sender;	// Process that sent the signal. May be NULL
	int signo;          	// Signal number
	siginfo_t info;    		// Signal information
} ksiginfo_t;

#endif /* __KERNEL_SIGNAL_TYPES_H */
