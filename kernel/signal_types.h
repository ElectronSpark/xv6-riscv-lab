#ifndef __KERNEL_SIGNAL_TYPES_H
#define __KERNEL_SIGNAL_TYPES_H

#include "types.h"
#include "signo.h"

typedef uint64 sigset_t;

typedef union sigval {
	int sival_int;
	void *sival_ptr;
} sigval_t;

typedef struct {
	int si_signo;
	int si_code;
	union sigval si_value;
	int si_errno;
	int si_pid;
	// int si_uid;
	void *si_addr;
	int si_status;
	int si_band;
} siginfo_t;

typedef struct sigaction {
    void        (*handler)(int);
    sigset_t    sa_mask;
    int         sa_flags;
} sigaction_t;

typedef struct sigacts {
    struct sigaction sa[NSIG+1];
	sigset_t sa_sigmask;  // signals currently blocked at the process level
    sigset_t sa_sigpending;  // signals pending for this process
	sigset_t sa_sigblock;   // signals blocked by this process
	sigset_t sa_sigterm;    // signals that terminate the process
    sigset_t sa_sigignore;  // signals ignored by this process
} sigacts_t;


#endif /* __KERNEL_SIGNAL_TYPES_H */
