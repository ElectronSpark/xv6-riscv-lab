#ifndef __KERNEL_SIGNAL_H
#define __KERNEL_SIGNAL_H

#include "types.h"
#include "signal_types.h"

#define SIGBAD(signo) \
    ((signo) < 1 || (signo) > NSIG)

// All four return: 0 if OK, −1 on error
static inline int sigemptyset(sigset_t *set) {
    if (!set) {
        return -1; // Invalid set pointer
    }
    *set = 0; // Clear the signal set
    return 0; // Success
}

static inline int sigfillset(sigset_t *set) {
    if (!set) {
        return -1; // Invalid set pointer
    }
    *set = (1UL << NSIG) - 1; // Set all signals
    return 0; // Success
}

static inline int sigaddset(sigset_t *set, int signo) {
    if (!set || SIGBAD(signo)) {
        return -1; // Invalid set pointer or signal number
    }
    int shift = signo - 1;
    *set |= (1UL << shift);
    return 0; // Success
}

static inline int sigdelset(sigset_t *set, int signo) {
    if (!set || SIGBAD(signo)) {
        return -1; // Invalid set pointer or signal number
    }
    int shift = signo - 1;
    *set &= ~(1UL << shift);
    return 0; // Success
}

// Returns: 1 if true, 0 if false, −1 on error
static inline int sigismember(const sigset_t *set, int signo) {
    if (!set || SIGBAD(signo)) {
        return -1; // Invalid set pointer or signal number
    }
    int shift = signo - 1;
    return (*set & (1UL << shift)) != 0; // Return 1 if member, 0 otherwise
}

void signal_init(void);
sigacts_t *sigacts_init(void);
sigacts_t *sigacts_dup(sigacts_t *psa);
void sigacts_free(sigacts_t *sa);

void sigqueue_init(sigqueue_t *sq);
ksiginfo_t *ksiginfo_alloc(void);
int sig_queue_push(struct proc *p, ksiginfo_t *ksi);

sig_defact signo_default_action(int signo);
int __signal_send(struct proc *p, int signo, siginfo_t *info);
int signal_send(int pid, int signo, siginfo_t *info);
int signal_terminated(sigacts_t *sa);
sigaction_t *signal_take(sigacts_t *sa, int *ret_signo);

int sigaction(int signum, struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigpending(sigset_t *set);
int sigreturn(void);

#define SIG_BLOCK   1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3


#endif /* __KERNEL_SIGNAL_H */
