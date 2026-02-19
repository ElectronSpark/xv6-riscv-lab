#ifndef __KERNEL_SIGNAL_H
#define __KERNEL_SIGNAL_H

#include "types.h"
#include "signal_types.h"

void signal_init(void);
void sigstack_init(stack_t *stack);
void sigacts_lock(sigacts_t *sa);
void sigacts_unlock(sigacts_t *sa);
int sigacts_holding(sigacts_t *sa);
sigacts_t *sigacts_init(void);
void sigacts_exec(sigacts_t *sa);
sigacts_t *sigacts_dup(sigacts_t *psa, uint64 clone_flags);
void sigacts_put(sigacts_t *sa);

void sigpending_init(struct thread *p);
void sigpending_destroy(struct thread *p);
void sigpending_clone(struct thread_signal *dst, struct thread_signal *src,
                      uint64 clone_flags, int esignal);
ksiginfo_t *ksiginfo_alloc(void);
void ksiginfo_free(ksiginfo_t *ksi);
int sigpending_empty(struct thread *p, int signo);

sig_defact signo_default_action(int signo);
int __signal_send(struct thread *p, ksiginfo_t *info);
int signal_send(int pid, ksiginfo_t *info);
bool signal_pending(struct thread *p);
int signal_notify(struct thread *p);

// Recalculate and update TIF_SIGPENDING flag for process
// Call this after any change to signal.sig_pending_mask or signal.sig_mask
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
int kill_from_kernel(int pid, int signum);
int kill_proc(struct thread *p, int signum);
int tgkill(int tgid, int tid, int signum);
int tkill(int tid, int signum);
int killed(struct thread *);
int sigsuspend(const sigset_t *mask);
int sigwait(const sigset_t *set, int *sig);

#endif /* __KERNEL_SIGNAL_H */
