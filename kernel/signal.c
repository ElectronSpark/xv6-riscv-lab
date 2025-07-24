#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "signal.h"
#include "spinlock.h"
#include "proc.h"
#include "slab.h"
#include "sched.h"

static slab_cache_t __sigacts_pool;

sig_defact signo_default_action(int signo) {
    switch (signo) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
    // case SIGCLD:
        return SIG_ACT_IGN;
    // case SIGEMT:
    case SIGHUP:
    // case SIGINFO:
    case SIGINT:
    case SIGIO:
    case SIGKILL:
    // case SIGLOST:
    case SIGPIPE:
    // case SIGPOLL:
    case SIGPROF:
    case SIGPWR:
    case SIGSTKFLT:
    case SIGTERM:
    case SIGUSR1:
    case SIGUSR2:
    case SIGVTALRM:
        return SIG_ACT_TERM;
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return SIG_ACT_STOP;
    case SIGCONT:
        return SIG_ACT_CONT;
    case SIGABRT:
    case SIGBUS:
    case SIGILL:
    // case SIGIOT:
    case SIGQUIT:
    case SIGSEGV:
    case SIGSYS:
    case SIGTRAP:
    // case SIGUNUSED
    case SIGXCPU:
    case SIGXFSZ:
    case SIGFPE:
        return SIG_ACT_CORE;
    default:
        return SIG_ACT_INVALID; // Invalid signal number
    }
}

#define SIG_MANDATORY_MASK (SIGNO_MASK(SIGKILL) | SIGNO_MASK(SIGSTOP))

#define __SIGNAL_MAKE_MASK(__sigacts, __signo)  \
    ((sigset_t)((~SIG_MANDATORY_MASK) &         \
     ((__sigacts)->sa_sigblock |                \
     (__sigacts)->sa[__signo].sa_mask |         \
     SIGNO_MASK(__signo))))

// Initialize the first signal actions
sigacts_t *sigacts_init(void) {
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (!sa) {
        return NULL;
    }
    memset(sa, 0, sizeof(sigacts_t));
    sa->sa_sigmask = sa->sa[0].sa_mask;
    sa->sa_sigpending = 0;
    return sa;
}

sigacts_t *sigacts_dup(sigacts_t *psa) {
    if (!psa) {
        return NULL;
    }
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (sa) {
        memmove(sa, psa, sizeof(sigacts_t));
        sa->sa_sigmask = __SIGNAL_MAKE_MASK(sa, SIGNONE);
        sa->sa_sigpending = 0;
    }
    return sa;
}

void sigacts_free(sigacts_t *sa) {
    slab_free(sa);
}

void signal_init(void) {
    slab_cache_init(&__sigacts_pool, "sigacts", sizeof(sigacts_t), SLAB_FLAG_STATIC);
}

int __signal_send(struct proc *p, int signo, siginfo_t *info) {
    if (!p && (p = myproc()) == NULL) {
        return -1; // No process available
    }
    if (SIGBAD(signo)) {
        return -1; // Invalid process or signal number
    }
    if (!spin_holding(&p->lock)) {
        return -1; // Lock must be held to send signal
    }

    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return -1; // No signal actions available
    }

    // ignored signals are not sent
    if (sa->sa_sigignore & SIGNO_MASK(signo)) {
        return 0; // Signal is ignored
    }

    sa->sa_sigpending |= SIGNO_MASK(signo);
    return 0; // Signal sent successfully
}

int signal_send(int pid, int signo, siginfo_t *info) {
    struct proc *p = NULL;
    if (pid < 0 || SIGBAD(signo)) {
        return -1; // Invalid PID or signal number
    }
    if (proctab_get_pid_proc(pid, &p) != 0) {
        return -1; // No process found
    }
    assert(p != NULL, "signal_send: proc is NULL");
    proc_lock(p);
    if (p->state == UNUSED || p->state == ZOMBIE || p->state == EXITING) {
        proc_unlock(p);
        return -1; // Process is not usable
    }
    int ret = __signal_send(p, signo, info);
    proc_unlock(p);

    scheduler_wakeup_on_chan(p->chan);

    return ret;
}

int signal_terminated(sigacts_t *sa) {
    if (!sa) {
        return 0;
    }
    sigset_t masked = sa->sa_sigpending & ~sa->sa_sigmask;
    return (masked & sa->sa_sigterm) != 0;
}

// Claim the signal and set the signal actions to the corresponding state.
// and return the sigaction for the signal.
sigaction_t *signal_take(sigacts_t *sa, int *ret_signo) {
    if (!sa) {
        return NULL; // No signal actions available
    }

    sigset_t masked = sa->sa_sigpending & ~(sa->sa_sigmask | sa->sa_sigignore);

    if (masked == 0) {
        return NULL; // No pending signals
    }

    int signo = 0;
    for (signo = 1; signo <= NSIG; signo++) {
        if (masked & SIGNO_MASK(signo)) {
            break; // Found a pending signal
        }
    }
    if (signo > NSIG) {
        return NULL; // Invalid signal number
    }

    sa->sa_sigpending &= ~SIGNO_MASK(signo); // Clear the pending signal
    sa->sa_sigmask = __SIGNAL_MAKE_MASK(sa, signo); // Update the mask

    if (ret_signo) {
        *ret_signo = signo; // Return the signal number
    }

    return &sa->sa[signo]; // Return the sigaction for the signal
}

int sigaction(int signum, struct sigaction *act, struct sigaction *oldact) {
    if (signum < 1 || signum > NSIG) {
        return -1; // Invalid signal number
    }
    if (signum == SIGKILL || signum == SIGSTOP) {
        return -1; // SIGKILL and SIGSTOP cannot be caught or ignored
    }

    struct proc *p = myproc();
    assert(p != NULL, "sys_sigaction: myproc returned NULL");

    proc_lock(p);
    sigacts_t *sa = p->sigacts;

    if (oldact) {
        memmove(oldact, &sa->sa[signum], sizeof(struct sigaction));
    }

    if (act) {
        sa->sa[signum] = *act;
    }
    
    proc_unlock(p);
    return 0; // Success
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (!set && how != SIG_SETMASK) {
        return -1; // Invalid set pointer for non-SIG_SETMASK operations
    }
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
        return -1; // Invalid operation
    }
    struct proc *p = myproc();
    assert(p != NULL, "sigprocmask: myproc returned NULL");
    proc_lock(p);

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigprocmask: sigacts is NULL");
    if (oldset) {
        *oldset = sa->sa_sigmask; // Save the old mask
    }
    
    if (how == SIG_SETMASK) {
        sa->sa_sigmask = *set; // Set the new mask
    } else if (how == SIG_BLOCK) {
        sa->sa_sigmask |= *set; // Block the signals in the set
    } else if (how == SIG_UNBLOCK) {
        sa->sa_sigmask &= ~(*set); // Unblock the signals in the set
    }

    // Ensure the mandatory signals are always unblocked
    sa->sa_sigmask &= ~SIG_MANDATORY_MASK;

    return 0; // Success
}

int sigpending(sigset_t *set) {
    if (!set) {
        return -1; // Invalid set pointer
    }
    struct proc *p = myproc();
    assert(p != NULL, "sigpending: myproc returned NULL");
    proc_lock(p);

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigpending: sigacts is NULL");
    
    *set = sa->sa_sigmask & sa->sa_sigpending; // Get the pending signals
    proc_unlock(p);
    
    return 0; // Success
}

int sigreturn(void) {
    struct proc *p = myproc();
    assert(p != NULL, "sys_sigreturn: myproc returned NULL");

    proc_lock(p);
    if (p->sigtrapframe == 0) {
        proc_unlock(p);
        return -1; // No signal trap frame to restore
    }

    if (restore_sigtrapframe(p) != 0) {
        proc_unlock(p);
        // @TODO:
        exit(-1); // Restore failed, exit the process
    }

    proc_unlock(p);

    return 0; // Success
}
