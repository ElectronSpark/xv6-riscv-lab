#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "signal.h"
#include "spinlock.h"
#include "proc.h"
#include "slab.h"
#include "sched.h"
#include "list.h"

static slab_cache_t __sigacts_pool;
static slab_cache_t __ksiginfo_pool;

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

void sigqueue_init(sigqueue_t *sq) {
    if (!sq) {
        return; // Invalid pointer
    }
    list_entry_init(&sq->queue);
    sq->count = 0;
}

void sigstack_init(stack_t *stack) {
    if (!stack) {
        return; // Invalid pointer
    }
    stack->ss_sp = NULL; // No stack allocated yet
    stack->ss_flags = SS_DISABLE;
    stack->ss_size = 0; // Size is zero initially
}

ksiginfo_t *ksiginfo_alloc(void) {
    ksiginfo_t *ksi = slab_alloc(&__ksiginfo_pool);
    if (!ksi) {
        return NULL; // Allocation failed
    }
    memset(ksi, 0, sizeof(ksiginfo_t));
    list_entry_init(&ksi->list_entry);
    ksi->sender = NULL; // No sender initially
    return ksi;
}

void ksiginfo_free(ksiginfo_t *ksi) {
    if (ksi) {
        slab_free(ksi);
    }
}

int sigqueue_push(struct proc *p, ksiginfo_t *ksi) {
    if (!p || !ksi) {
        return -1; // Invalid process or ksiginfo
    }
    proc_assert_holding(p);
    if (p->sigqueue.count < 0) {
        return -1; // Invalid signal queue count
    }
    ksi->receiver = p; // Set the receiver to the process
    list_node_push(&p->sigqueue.queue, ksi, list_entry);
    p->sigqueue.count++;
    return 0;
}

int __sigqueue_remove(struct proc *p, ksiginfo_t *ksi) {
    if (!p || !ksi) {
        return -1; // Invalid process or ksiginfo
    }
    proc_assert_holding(p);
    assert(ksi->receiver == p, "sig_queue_remove: receiver mismatch");
    assert(p->sigqueue.count >= 0, "Negative signal queue count");
    list_node_detach(ksi, list_entry);
    p->sigqueue.count--;
    return 0;
}

// Pop the first ksiginfo of the given signal number from the process's signal queue.
// If the signo is 0, then the first ksiginfo in the queue is returned.
// Returns NULL if no such signal is found.
// The caller must hold the process lock.
ksiginfo_t *sigqueue_pop(struct proc *p, int signo) {
    if (!p || signo < 0 || signo > NSIG) {
        return NULL; // Invalid process or signal number
    }
    proc_assert_holding(p);

    ksiginfo_t *ksi = NULL;
    ksiginfo_t *tmp = NULL;
    if (signo == 0)

    list_foreach_node_safe(&p->sigqueue.queue, ksi, tmp, list_entry) {
        if (ksi->signo == signo) {
            if (__sigqueue_remove(p, ksi) != 0) {
                return NULL;
            }
            return ksi; // Found the signal, return it
        }
    }

    return NULL; // No matching signal found
}

// Clean the signal queue of the given process for the specified signal number.
// If signo is 0, all signals in the queue are cleaned.
// Ksiginfo being cleaned will be freed.
// The caller must hold the process lock.
// Returns 0 on success, -1 on error.
int sigqueue_clean(struct proc *p, int signo) {
    if (!p || signo < 0 || signo > NSIG) {
        return -1; // Invalid process or signal number
    }
    proc_assert_holding(p);

    int ret = 0;
    ksiginfo_t *ksi = NULL;
    ksiginfo_t *tmp = NULL;
    list_foreach_node_safe(&p->sigqueue.queue, ksi, tmp, list_entry) {
        if (signo == 0 || ksi->signo == signo) {
            if (__sigqueue_remove(p, ksi) != 0) {
                ret = -1;
            } else {
                ksiginfo_free(ksi); // Free the ksiginfo after removing it
            }
        }
    }

    return ret;
}

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
    slab_cache_init(&__ksiginfo_pool, "ksiginfo", sizeof(ksiginfo_t), SLAB_FLAG_STATIC);
}

int __signal_send(struct proc *p, ksiginfo_t *info) {
    if (p == NULL || info == NULL) {
        return -1; // No process available
    }
    if (SIGBAD(info->signo)) {
        return -1; // Invalid process or signal number
    }

    proc_lock(p);
    if (p->state == UNUSED || p->state == ZOMBIE || p->state == EXITING) {
        proc_unlock(p);
        return -1; // Process is not usable
    }

    sigacts_t *sa = p->sigacts;
    if (!sa) {
        proc_unlock(p);
        return -1; // No signal actions available
    }

    sigset_t signo_mask = SIGNO_MASK(info->signo);

    // ignored signals are not sent
    if (sa->sa_sigignore & signo_mask) {
        proc_unlock(p);
        return 0; // Signal is ignored
    }

    if (sigqueue_push(p, info) != 0) {
        proc_unlock(p);
        return -1; // Failed to push signal to queue
    }
    sa->sa_sigpending |= signo_mask;
    bool need_wakeup = (~sa->sa_sigmask & signo_mask) == 0;
    proc_unlock(p);

    if (need_wakeup) {
        scheduler_wakeup_on_chan(p->chan);
    }
    return 0; // Signal sent successfully
}

int signal_send(int pid, ksiginfo_t *info) {
    struct proc *p = NULL;
    if (pid < 0 || info == NULL || SIGBAD(info->signo)) {
        return -1; // Invalid PID or signal number
    }
    if (proctab_get_pid_proc(pid, &p) != 0) {
        return -1; // No process found
    }
    if (p == NULL) {
        return -1; // No process found
    }
    assert(p != NULL, "signal_send: proc is NULL");
    
    return __signal_send(p, info);
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
sigaction_t *signal_pick(struct proc *p, ksiginfo_t **ret_info) {
    if (p == NULL || ret_info == NULL || p->sigacts == NULL) {
        return NULL; // No signal actions available
    }
    proc_assert_holding(p);

    sigacts_t *sa = p->sigacts;
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

    ksiginfo_t *ksi = NULL;
    ksiginfo_t *tmp = NULL;

    list_foreach_node_safe(&p->sigqueue.queue, ksi, tmp, list_entry) {
        if (ksi->signo == signo) {
            if (__sigqueue_remove(p, ksi) != 0) {
                return NULL;
            }
            *ret_info = ksi;
            return &sa->sa[signo]; // Found the signal, return it
        }
    }

    return NULL;
}

int signal_deliver(struct proc *p, ksiginfo_t *info, sigaction_t *sa) {
    if (p == NULL || sa == NULL || info == NULL) {
        return -1; // Invalid process, signal action, or info
    }
    proc_assert_holding(p);

    // If the signal action is to ignore the signal, do nothing
    if ((void*)sa->sa_handler == SIG_IGN) {
        return 0; // Signal ignored
    }

    if ((p->sig_stack.ss_flags & SS_ONSTACK) && 
        (sa->sa_flags & SA_ONSTACK)) {
        // Use the alternate signal stack if it's set and the action requires it
        p->sig_stack.ss_flags |= SS_ONSTACK;
    }

    p->sigacts->sa_sigmask |= sa->sa_mask; // Block the signal during delivery
    // @TODO: signal pending
    return 0;
}

int signal_restore(struct proc *p, ucontext_t *context) {
    if (p == NULL || context == NULL) {
        return -1; // Invalid process or context
    }
    proc_assert_holding(p);

    p->sig_stack = context->uc_stack;
    p->sigacts->sa_sigmask = context->uc_sigmask;
    p->sig_ucontext = (uint64)context->uc_link;

    return 0; // Success
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
    if (p->sig_ucontext == 0) {
        proc_unlock(p);
        return -1; // No signal trap frame to restore
    }

    if (restore_sigframe(p) != 0) {
        proc_unlock(p);
        // @TODO:
        exit(-1); // Restore failed, exit the process
    }

    assert(signal_restore(p, (ucontext_t *)p->sig_ucontext) == 0,
           "sigreturn: signal_restore failed");

    proc_unlock(p);

    return 0; // Success
}
