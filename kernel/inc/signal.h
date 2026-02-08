#ifndef __KERNEL_SIGNAL_H
#define __KERNEL_SIGNAL_H

#include "types.h"
#include "signal_types.h"

#define SIGBAD(signo) \
    ((signo) < 1 || (signo) > NSIG)

#define SIGMASK(signo) \
    (SIGBAD(signo) ? 0 : (1UL << ((uint64)(signo) - 1)))

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
    sigset_t mask = SIGMASK(signo);
    if (mask == 0) {
        return -1; // Invalid set pointer or signal number
    }
    *set |= mask;
    return 0; // Success
}

static inline int sigdelset(sigset_t *set, int signo) {
    sigset_t mask = SIGMASK(signo);
    if (mask == 0) {
        return -1; // Invalid set pointer or signal number
    }
    *set &= ~mask;
    return 0; // Success
}

// Returns: 1 if true, 0 if false, −1 on error
static inline int sigismember(const sigset_t *set, int signo) {
    sigset_t mask = SIGMASK(signo);
    if (mask == 0) {
        return -1; // Invalid set pointer or signal number
    }
    return (*set & mask) != 0; // Return 1 if member, 0 otherwise
}

void signal_init(void);
void sigstack_init(stack_t *stack);
void sigacts_lock(sigacts_t *sa);
void sigacts_unlock(sigacts_t *sa);
int sigacts_holding(sigacts_t *sa);
sigacts_t *sigacts_init(void);
sigacts_t *sigacts_dup(sigacts_t *psa, uint64 clone_flags);
void sigacts_put(sigacts_t *sa);

void sigpending_init(struct thread *p);
void sigpending_destroy(struct thread *p);
ksiginfo_t *ksiginfo_alloc(void);
void ksiginfo_free(ksiginfo_t *ksi);
int sigpending_empty(struct thread *p, int signo);

sig_defact signo_default_action(int signo);
int __signal_send(struct thread *p, ksiginfo_t *info);
int signal_send(int pid, ksiginfo_t *info);
bool signal_pending(struct thread *p);
int signal_notify(struct thread *p);

// Recalculate and update TIF_SIGPENDING flag for process
// Call this after any change to signal.sig_pending_mask or sa_sigmask
void recalc_sigpending(void);
bool recalc_sigpending_tsk(struct thread *p);
bool signal_terminated(struct thread *p);
void handle_signal(void);

int sigaction(int signum, struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigpending(struct thread *p, sigset_t *set);
int sigreturn(void);

int kill(int, int);
int kill_thread(struct thread *p, int signum);
int killed(struct thread*);

#define SIG_BLOCK   1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3

#define MINSIGSTKSZ (1UL << PGSHIFT)
#define SIGSTKSZ    (1UL << (PGSHIFT + 2))


#endif /* __KERNEL_SIGNAL_H */
