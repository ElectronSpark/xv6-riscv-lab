#ifndef __KERNEL_SIGNAL_H
#define __KERNEL_SIGNAL_H

#include "types.h"
#include "spinlock.h"
#include "signal_types.h"

void signal_init(void);

sigacts_t *sigacts_init(void);
sigacts_t *sigacts_dup(sigacts_t *psa);
void sigacts_free(sigacts_t *sa);

int __signal_send(struct proc *p, int signo, siginfo_t *info);
int signal_send(int pid, int signo, siginfo_t *info);
int signal_terminated(sigacts_t *sa);
sigaction_t *signal_take(sigacts_t *sa, int *ret_signo);

int sigaction(int signum, struct sigaction *act, struct sigaction *oldact);


#endif /* __KERNEL_SIGNAL_H */
