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

static const void *__DEFAULT_SIGACTION[] = {
    [SIGABRT]   = SIG_CORE,
    [SIGALRM]   = SIG_TERM,
    [SIGBUS]    = SIG_CORE,
    [SIGCHLD]   = SIG_IGN,
    // [SIGCLD]    = SIG_IGN,
    [SIGCONT]   = SIG_CONT,
    // [SIGEMT]    = SIG_TERM,
    [SIGFPE]    = SIG_CORE,
    [SIGHUP]    = SIG_TERM,
    [SIGILL]    = SIG_CORE,
    // [SIGINFO]   = SIG_TERM,
    [SIGINT]    = SIG_TERM,
    [SIGIO]     = SIG_TERM,
    [SIGIOT]    = SIG_CORE,
    [SIGKILL]   = SIG_TERM,
    // [SIGLOST]   = SIG_TERM,
    [SIGPIPE]   = SIG_TERM,
    [SIGPOLL]   = SIG_TERM,
    [SIGPROF]   = SIG_TERM,
    [SIGPWR]    = SIG_TERM,
    [SIGQUIT]   = SIG_CORE,
    [SIGSEGV]   = SIG_CORE,
    [SIGSTKFLT] = SIG_TERM,
    [SIGSTOP]   = SIG_STOP,
    [SIGTSTP]   = SIG_STOP,
    [SIGSYS]    = SIG_CORE,
    [SIGTERM]   = SIG_TERM,
    [SIGTRAP]   = SIG_CORE,
    [SIGTTIN]   = SIG_STOP,
    [SIGTTOU]   = SIG_STOP,
    [SIGUNUSED] = SIG_CORE,
    [SIGURG]    = SIG_IGN,
    [SIGUSR1]   = SIG_TERM,
    [SIGUSR2]   = SIG_TERM,
    [SIGVTALRM] = SIG_TERM,
    [SIGXCPU]   = SIG_CORE,
    [SIGXFSZ]   = SIG_CORE,
    [SIGWINCH]  = SIG_IGN,
    [NSIG+1]    = SIG_IGN,
    SIG_IGN, // Default for all other signals
};


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
    for (int i = 0; i <= NSIG; i++) {
        sa->sa[i].handler = __DEFAULT_SIGACTION[i];
        sa->sa[i].sa_flags = 0;
        sa->sa[i].sa_mask = __SIGNAL_MAKE_MASK(sa, SIGNONE);
    }
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
    if (signo < 1 || signo > NSIG) {
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
    if (proctab_get_pid_proc(pid, &p) != 0) {
        return -1; // No process found
    }
    assert(p != NULL, "signal_send: proc is NULL");
    spin_acquire(&p->lock);
    if (p->state == UNUSED || p->state == ZOMBIE || p->state == EXITING) {
        spin_release(&p->lock);
        return -1; // Process is not usable
    }
    int ret = __signal_send(p, signo, info);
    spin_release(&p->lock);

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

    spin_acquire(&p->lock);
    sigacts_t *sa = p->sigacts;

    if (oldact) {
        memmove(oldact, &sa->sa[signum], sizeof(struct sigaction));
    }

    if (act) {
        sa->sa[signum] = *act;
    }
    
    spin_release(&p->lock);
    return 0; // Success
}

uint64 sys_sigaction(void) {
    int signum;
    uint64 act_addr, oldact_addr;
    struct sigaction act, oldact;
    struct sigaction *p_act = NULL;
    struct sigaction *p_oldact = NULL;
    argint(0, &signum);
    argaddr(1, &act_addr);
    argaddr(2, &oldact_addr);

    if (act_addr != 0) {
        p_act = &act;
        if (either_copyin(p_act, 1, act_addr, sizeof(struct sigaction)) < 0) {
            return -1; // Copy failed
        }
    }
    if (oldact_addr != 0) {
        p_oldact = &oldact;
    }

    if (sigaction(signum, p_act, p_oldact) < 0) {
        return -1; // sigaction failed
    }

    if (p_oldact != 0) {
        if (either_copyout(1, oldact_addr, p_oldact, sizeof(struct sigaction)) < 0) {
            return -1; // Copy failed
        }
    }

    return 0; // Success
}

int sigreturn(void) {
    struct proc *p = myproc();
    assert(p != NULL, "sys_sigreturn: myproc returned NULL");

    spin_acquire(&p->lock);
    if (p->sigtrapframe == 0) {
        spin_release(&p->lock);
        return -1; // No signal trap frame to restore
    }

    if (restore_sigtrapframe(p) != 0) {
        spin_release(&p->lock);
        // @TODO:
        exit(-1); // Restore failed, exit the process
    }

    spin_release(&p->lock);

    return 0; // Success
}

uint64 sys_sigreturn(void) {
    if (sigreturn() < 0) {
        return -1; // sigreturn failed
    }
    
    struct proc *p = myproc();
    assert(p != NULL, "sys_sigreturn: myproc returned NULL");

    return 0;
}
