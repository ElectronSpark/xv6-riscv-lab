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
    case SIGALRM:
    case SIGUSR1:
    case SIGUSR2:
    // case SIGCLD:
        return SIG_ACT_IGN;
    // case SIGEMT:
    case SIGHUP:
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

void sigpending_init(struct proc *p) {
    if (!p) {
        return; // Invalid pointer
    }
    for (int i = 0; i < NSIG; i++) {
        sigpending_t *sq = &p->sig_pending[i];
        list_entry_init(&sq->queue);
    }
}

void sigpending_destroy(struct proc *p) {
    if (!p) {
        return; // Invalid pointer
    }
    proc_assert_holding(p);
    for (int i = 1; i <= NSIG; i++) {
        assert(sigpending_empty(p, i) == 0, 
               "sigpending_destroy: pending signals not empty for signal %d", i);
    }
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

// Clean the signal queue of the given process for the specified signal number.
// If signo is 0, all signals in the queue are cleaned.
// Ksiginfo being cleaned will be freed.
// The caller must hold the process lock.
// Returns 0 on success, -1 on error.
int sigpending_empty(struct proc *p, int signo) {
    if (!p || SIGBAD(signo)) {
        return -1; // Invalid process or signal number
    }
    proc_assert_holding(p);

    ksiginfo_t *ksi = NULL;
    ksiginfo_t *tmp = NULL;
    sigpending_t *sq = &p->sig_pending[signo];
    list_foreach_node_safe(&sq->queue, ksi, tmp, list_entry) {
        list_node_detach(ksi, list_entry);
        ksiginfo_free(ksi); // Free the ksiginfo after removing it
    }
    sigdelset(&p->sig_pending_mask, signo);
    return 0;
}

static void __sig_reset_act_mask(sigacts_t *sa, int signo) {
    sigdelset(&sa->sa_sigterm, signo);
    sigdelset(&sa->sa_sigignore, signo);
    // sigdelset(&sa->sa_usercatch, signo);
    if (signo != SIGSTOP) {
        sigdelset(&sa->sa_sigstop, signo);
    }
    if (signo != SIGCONT) {
        sigdelset(&sa->sa_sigcont, signo);
    }
    // sigdelset(&sa->sa_sigcore, signo);
}

static int __sig_setdefault(sigacts_t *sa, int signo) {
    if (!sa || SIGBAD(signo)) {
        return -1; // Invalid signal number or signal actions
    }
    sig_defact defact = signo_default_action(signo);
    if (defact == SIG_ACT_INVALID) {
        return 0; // Ignore invalid signal number
    }

    __sig_reset_act_mask(sa, signo);
    switch (defact) {
    case SIG_ACT_IGN:
        sigaddset(&sa->sa_sigignore, signo);
        break;
    case SIG_ACT_CONT:
        sigaddset(&sa->sa_sigcont, signo);
        break;
    case SIG_ACT_STOP:
        sigaddset(&sa->sa_sigstop, signo);
        break;
    case SIG_ACT_TERM:
    // @TODO: For now handle SIG_ACT_CORE and SIG_ACT_INVALID by terminating the process
    case SIG_ACT_CORE:
    case SIG_ACT_INVALID:
        sigaddset(&sa->sa_sigterm, signo);
        break;
    default:
        return -1;
    }

    sa->sa[signo].sa_handler = SIG_DFL;
    sa->sa[signo].sa_flags = 0; // Reset flags
    sigemptyset(&(sa->sa[signo].sa_mask)); // Reset signal mask
    return 0;
}

// Initialize the first signal actions
sigacts_t *sigacts_init(void) {
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (!sa) {
        return NULL;
    }
    memset(sa, 0, sizeof(sigacts_t));
    sigemptyset(&sa->sa_sigmask);
    sigemptyset(&sa->sa_original_mask);
    sigemptyset(&sa->sa_sigterm);
    sigemptyset(&sa->sa_sigstop);
    sigemptyset(&sa->sa_sigcont);
    sigemptyset(&sa->sa_sigignore);
    
    for (int i = 1; i <= NSIG; i++) {
        assert(__sig_setdefault(sa, i) == 0, "sigacts_init: failed to set default action for signal %d", i);
    }
    return sa;
}

sigacts_t *sigacts_dup(sigacts_t *psa) {
    if (!psa) {
        return NULL;
    }
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (sa) {
        memmove(sa, psa, sizeof(sigacts_t));
        sa->sa_sigmask = sa->sa[0].sa_mask;
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
    int ret = -1; // Default to error
    if (p == NULL || info == NULL) {
        return -1; // No process available
    }
    if (SIGBAD(info->signo)) {
        return -1; // Invalid process or signal number
    }

    proc_lock(p);
    if (p->state == PSTATE_UNUSED || p->state == PSTATE_ZOMBIE || PROC_KILLED(p)) {
        ret = -1; // Process is not usable
        goto done;
    }

    sigacts_t *sa = p->sigacts;
    if (!sa) {
        ret = -1; // No signal actions available
        goto done;
    }

    // ignored signals are not sent
    if (sigismember(&sa->sa_sigignore, info->signo)) {
        proc_unlock(p);
        return 0;
    }
    
    sigaction_t *act = &sa->sa[info->signo];
    if (act->sa_flags & SA_SIGINFO) {
        assert(info->signo != SIGKILL && info->signo != SIGSTOP, 
               "signal_send: SA_SIGINFO set for SIGKILL or SIGSTOP");
        ksiginfo_t *ksi = ksiginfo_alloc();
        if (!ksi) {
            ret = -1; // Allocation failed
            goto done;
        }
        *ksi = *info; // Copy the signal info
        list_entry_init(&ksi->list_entry);
        // Add to pending queue
        list_node_push(&p->sig_pending[info->signo - 1].queue, ksi, list_entry);
    }

    if (sigismember(&sa->sa_sigstop, info->signo) && !sigismember(&sa->sa_sigmask, info->signo)) {
        // Pause the process
        PROC_SET_STOPPED(p);
    } else {
        sigaddset(&p->sig_pending_mask, info->signo);
        if (sigismember(&sa->sa_sigcont, info->signo) && !sigismember(&sa->sa_sigmask, info->signo)) {
            p->sig_pending_mask &= ~sa->sa_sigstop;
            if (PROC_STOPPED(p)) {
                // If the process is stopped, we need to wake it up
                sched_lock();
                scheduler_continue(p);
                sched_unlock();
            }
        }
    }

    ret = 0;

done:
    // If the action is to terminate the process, set the killed flag
    if (sigismember(&sa->sa_sigterm, info->signo)) {
        PROC_SET_KILLED(p);
        if (PROC_STOPPED(p)) {
            // If the process is stopped, we need to wake it up
            sched_lock();
            scheduler_continue(p);
            sched_unlock();
        }
    }
    if (ret == 0 && signal_pending(p)) {
        signal_notify(p); // Notify the process if needed
    }
    proc_unlock(p);
    return ret; // Signal sent successfully
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

bool signal_pending(struct proc *p) {
    if (!p) {
        return false;
    }
    proc_assert_holding(p);
    sigset_t masked = p->sig_pending_mask & ~p->sigacts->sa_sigmask;
    return masked != 0;
}

int signal_notify(struct proc *p) {
    if (!p) {
        return -1;
    }
    proc_assert_holding(p);
    if (PROC_AWOKEN(p)) {
        return 0;
    }
    if (!PROC_SLEEPING(p)) {
        return -1; // Process is not sleeping
    }
    if (__proc_get_pstate(p) == PSTATE_INTERRUPTIBLE) {
        sched_lock();
        scheduler_wakeup(p);
        sched_unlock();
        return 0; // Success
    }
    return -1; // No signals pending or process not in interruptible state
}

bool signal_terminated(struct proc *p) {
    if (!p) {
        return 0;
    }
    proc_assert_holding(p);
    sigset_t masked = p->sig_pending_mask & ~p->sigacts->sa_sigmask;
    return (masked & p->sigacts->sa_sigterm) != 0;
}

bool signal_test_clear_stopped(struct proc *p) {
    if (!p) {
        return 0;
    }
    proc_assert_holding(p);
    sigset_t masked = p->sig_pending_mask & ~p->sigacts->sa_sigmask;
    sigset_t pending_stopped = masked & p->sigacts->sa_sigstop;
    p->sig_pending_mask &= ~pending_stopped; // Clear the stopped signals
    return pending_stopped != 0;
}

int signal_restore(struct proc *p, ucontext_t *context) {
    if (p == NULL || context == NULL) {
        return -1; // Invalid process or context
    }
    proc_assert_holding(p);

    p->sig_stack = context->uc_stack;
    p->sig_ucontext = (uint64)context->uc_link;
    if (p->sig_ucontext == 0) {
        p->sigacts->sa_sigmask = p->sigacts->sa_original_mask; // Reset to original mask
    } else {
        p->sigacts->sa_sigmask = context->uc_sigmask;
        p->sigacts->sa_sigmask |= p->sigacts->sa_original_mask; // Update the signal mask
    }

    sigdelset(&p->sigacts->sa_sigmask, SIGKILL);
    sigdelset(&p->sigacts->sa_sigmask, SIGSTOP);
    sigdelset(&p->sigacts->sa_sigignore, SIGKILL);
    sigdelset(&p->sigacts->sa_sigignore, SIGSTOP);

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
        *oldact = sa->sa[signum];
    }
    
    if (act) {
        __sig_reset_act_mask(sa, signum);
        if (act->sa_handler == SIG_IGN) {
            sigaddset(&sa->sa_sigignore, signum);
        } else if (act->sa_handler == SIG_DFL) {
            if (__sig_setdefault(sa, signum) != 0) {
                proc_unlock(p);
                return -1; // Failed to set default action
            }
        }
        sa->sa[signum] = *act;
        sigdelset(&sa->sa[signum].sa_mask, SIGKILL); // Unblock the signal
        sigdelset(&sa->sa[signum].sa_mask, SIGSTOP); // Unblock the signal
        // Ignore all the previous pending signals
        if (sigpending_empty(p, signum) != 0) {
            proc_unlock(p);
            return -1; // Failed to clear pending signals
        }
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
        *oldset = sa->sa_original_mask; // Save the old mask
    }
    
    if (how == SIG_SETMASK) {
        sa->sa_original_mask = *set; // Set the new mask
        sa->sa_sigmask = *set; // Update the signal mask
    } else if (how == SIG_BLOCK) {
        sa->sa_original_mask |= *set; // Block the signals in the set
        sa->sa_sigmask |= *set; // Update the signal mask
    } else if (how == SIG_UNBLOCK) {
        sa->sa_original_mask &= ~(*set); // Unblock the signals in the set
        sa->sa_sigmask &= ~(*set); // Update the signal mask
    }

    sigdelset(&sa->sa_original_mask, SIGKILL);
    sigdelset(&sa->sa_original_mask, SIGSTOP);
    sigdelset(&sa->sa_sigmask, SIGKILL);
    sigdelset(&sa->sa_sigmask, SIGSTOP);

    return 0; // Success
}

int sigpending(struct proc *p, sigset_t *set) {
    if (!set) {
        return -1; // Invalid set pointer
    }
    assert(p != NULL, "sigpending: myproc returned NULL");
    proc_lock(p);

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigpending: sigacts is NULL");
    *set = sa->sa_sigmask & p->sig_pending_mask; // Get the pending signals
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

    ucontext_t uc = {0};
    if (restore_sigframe(p, &uc) != 0) {
        proc_unlock(p);
        // @TODO:
        exit(-1); // Restore failed, exit the process
    }
    
    assert(signal_restore(p, &uc) == 0, "sigreturn: signal_restore failed");

    proc_unlock(p);

    return 0; // Success
}

// Pick a pending signal for the process.
// Returns the signal number if a signal is found, 0 if no pending signals,
// or -1 on error.
static int __pick_signal(struct proc *p) {
    proc_assert_holding(p);

    sigset_t pending = p->sig_pending_mask;
    pending &= ~p->sigacts->sa_sigmask; // Remove blocked signals
    if (pending == 0) {
        return 0; // No pending signals
    }

    if (pending & p->sigacts->sa_sigterm) {
        // @TODO: exit code
        proc_unlock(p);
        exit(-1);
    }

    for (int signo = 1; signo <= NSIG; signo++) {
        if (sigismember(&pending, signo) > 0) {
            return signo; // Return the first pending signal
        }
    }

    return -1; // No valid signal found, should not happen
}

static int __dequeue_signal_update_pending(struct proc *p, int signo, ksiginfo_t **ret_info) {
    sigaction_t *sa = &p->sigacts->sa[signo];
    if (p == NULL || ret_info == NULL) {
        return -1;
    }
    if (SIGBAD(signo) || sigismember(&p->sigacts->sa_sigmask, signo) > 0) {
        return -1;
    }

    assert(sa->sa_handler != SIG_IGN,
           "__dequeue_signal_update_pending: signal handler is SIG_IGN");
    sigpending_t *sq = &p->sig_pending[signo - 1];
    if ((sa->sa_flags & SA_SIGINFO) == 0) {
        assert(LIST_IS_EMPTY(&sq->queue), 
               "sig_pending is not empty for a non-SA_SIGINFO signal");
        sigdelset(&p->sig_pending_mask, signo);
        *ret_info = NULL;
        return 0; // No signal info to return
    }

    ksiginfo_t *info = NULL;
    ksiginfo_t *pos = NULL;
    ksiginfo_t *tmp = NULL;
    list_foreach_node_safe(&sq->queue, pos, tmp, list_entry) {
        assert(pos->signo == signo, 
               "__dequeue_signal_update_pending: pos->signo != signo");
        if (info != NULL) {
            goto still_pending; // More than one signal info found
        }
        info = pos;
    }

    assert(info != NULL, "sigpending: no signal info found");
    // If only one signal info is found, also remove it from the pending mask
    sigdelset(&p->sig_pending_mask, signo);

still_pending:
    list_entry_detach(&info->list_entry);
    *ret_info = info;
    return 0; // Success
}

static int __deliver_signal(struct proc *p, int signo, ksiginfo_t *info, sigaction_t *sa, bool *repeat) {
    if (repeat) {
        *repeat = false; // Default to not repeat
    }

    if (p == NULL || sa == NULL) {
        return -1;
    }

    if (sa->sa_handler == SIG_IGN) {
        return 0; // Signal is ignored
    }

    if (sa->sa_flags & SA_SIGINFO) {
        assert(info != NULL, 
               "__deliver_signal: SA_SIGINFO but info is NULL");
    }

    if (sigismember(&p->sigacts->sa_sigcont, signo)) {
        p->sig_pending_mask &= ~p->sigacts->sa_sigstop;
        if (sa->sa_handler == SIG_DFL) {
            if (repeat) {
                *repeat = true; // Indicate that the signal should be repeated
            }
            return 0;
        }
    }

    int ret = push_sigframe(p, signo, sa, info);

    if ((sa->sa_flags &SA_NODEFER) == 0) {
        sigaddset(&p->sigacts->sa_sigmask, signo);
    }

    p->sigacts->sa_sigmask |= sa->sa_mask; // Block the signal during delivery
    sigdelset(&p->sigacts->sa_sigmask, SIGKILL);
    sigdelset(&p->sigacts->sa_sigmask, SIGSTOP);

    if ((sa->sa_flags & SA_RESETHAND) != 0) {
        // Reset the signal handler to default action
        assert(__sig_setdefault(p->sigacts, signo) == 0,
               "__deliver_signal: __sig_setdefault failed");
    }
    
    return ret;
}

void handle_signal(void) {
    struct proc *p = myproc();
    assert(p != NULL, "handle_signal: myproc returned NULL");
    if (p->sigacts == NULL) {
        return; // No signal actions defined
    }

    for (;;) {
        proc_lock(p);
        if (signal_terminated(p)) {
            PROC_SET_KILLED(p);
            proc_unlock(p);
            return;
        }
    
        if (signal_test_clear_stopped(p)) {
            PROC_SET_STOPPED(p);
            proc_unlock(p);
            return;
        }
    
        int signo = __pick_signal(p);
        assert(signo == 0 || !SIGBAD(signo), "handle_signal: __pick_signal failed");
        if (signo == 0) {
            proc_unlock(p);
            return; // No pending signals to handle
        }
    
        sigaction_t *sa = &p->sigacts->sa[signo];
        ksiginfo_t *info = NULL;
        bool repeat = false;
    
        assert(__dequeue_signal_update_pending(p, signo, &info) == 0,
               "handle_signal: __dequeue_signal_update_pending failed");
        assert(__deliver_signal(p, signo, info, sa, &repeat) == 0,
               "handle_signal: __deliver_signal failed");
               
        proc_unlock(p);
    
        if (info) {
            ksiginfo_free(info); // Free the ksiginfo after delivering the signal
        }

        // continue signals with default action need to be skipped, so in that case
        // we repeat the loop to check for more pending signals
        if (!repeat) {
            break;
        }
    }
}

