/*
 * Signal handling for xv6
 *
 * LOCKING:
 * Signal operations use a unified lock approach (like Linux sighand->siglock).
 * All signal state is protected by sigacts->lock:
 *   - Signal actions (sigacts->sa[])
 *   - Signal masks (sigacts->sa_sigmask, sa_original_mask)
 *   - Per-thread pending signals (proc->signal.sig_pending_mask, sig_pending[])
 *
 * Key rules:
 * - sigacts->lock must be held when reading/writing any signal state
 * - Release sigacts->lock BEFORE scheduler operations (wakeup, yield)
 * - Copy data from protected structures before releasing lock if needed after
 *
 * This is simpler than the old two-lock (proc_lock + sigacts_lock) approach
 * and matches Linux's design where sighand->siglock is THE signal lock.
 *
 * The PROC_FLAG_SIGPENDING flag provides O(1) checks for pending signals.
 * recalc_sigpending_tsk() updates this flag and must be called after any
 * change to signal.sig_pending_mask or sa_sigmask.
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "signal.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "proc/proc.h"
#include <mm/slab.h>
#include "proc/sched.h"
#include "list.h"
#include "bits.h"
#include "smp/ipi.h"
#include "clone_flags.h"
#include "errno.h"

static slab_cache_t __sigacts_pool;
static slab_cache_t __ksiginfo_pool;

// Forward declarations for helper functions
static void sigacts_assert_holding(sigacts_t *sa);

sig_defact signo_default_action(int signo) {
    switch (signo) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
        return SIG_ACT_IGN;
    case SIGALRM:
    case SIGUSR1:
    case SIGUSR2:
    // case SIGCLD:
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

/*
 * Recalculate the TIF_SIGPENDING flag for a task.
 * 
 * This checks if there are any pending signals that are not blocked.
 * If so, the SIGPENDING flag is set; otherwise it's cleared.
 * 
 * Following Linux's approach:
 * - Set flag if pending & ~blocked has any bits set
 * - Returns true if flag was set, false otherwise
 * 
 * Caller must hold sigacts_lock (or ensure sigacts won't change).
 */
bool recalc_sigpending_tsk(struct proc *p) {
    if (!p || !p->sigacts) {
        return false;
    }
    
    sigacts_t *sa = p->sigacts;
    sigset_t pending = smp_load_acquire(&p->signal.sig_pending_mask);
    sigset_t blocked = sa->sa_sigmask;
    
    if ((pending & ~blocked) != 0) {
        PROC_SET_SIGPENDING(p);
        return true;
    }
    
    /*
     * We must never clear the flag in another thread, or in current
     * when it's possible the current syscall is returning -ERESTART*.
     * So we only clear it for the current process.
     */
    return false;
}

/*
 * Recalculate TIF_SIGPENDING for the current process.
 * This can clear the flag if no signals are pending.
 */
void recalc_sigpending(void) {
    struct proc *p = myproc();
    if (!p || !p->sigacts) {
        return;
    }
    
    sigacts_t *sa = p->sigacts;
    sigacts_lock(sa);
    if (!recalc_sigpending_tsk(p)) {
        // No pending signals, safe to clear flag for current process
        PROC_CLEAR_SIGPENDING(p);
    }
    sigacts_unlock(sa);
}

void sigpending_init(struct proc *p) {
    if (!p) {
        return; // Invalid pointer
    }
    for (int i = 0; i < NSIG; i++) {
        sigpending_t *sq = &p->signal.sig_pending[i];
        list_entry_init(&sq->queue);
    }
}

void sigpending_destroy(struct proc *p) {
    if (!p) {
        return; // Invalid pointer
    }
    // Called at process exit - sigacts should already be locked or no longer shared
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }
    // Ensure all per-signal queues are already empty. Do NOT silently purge here.
    for (int i = 1; i <= NSIG; i++) {
        sigpending_t *sq = &p->signal.sig_pending[i - 1];
        assert(LIST_IS_EMPTY(&sq->queue),
               "sigpending_destroy: pending signals not empty for signal %d", i);
    }
    assert(p->signal.sig_pending_mask == 0, "sigpending_destroy: pending mask not zero");
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

void sigacts_lock(sigacts_t *sa) {
    spin_lock(&sa->lock);
}

void sigacts_unlock(sigacts_t *sa) {
    spin_unlock(&sa->lock);
}

int sigacts_holding(sigacts_t *sa) {
    return spin_holding(&sa->lock);
}

static void sigacts_assert_holding(sigacts_t *sa) {
    assert(sigacts_holding(sa), "sigacts lock not held");
}

void ksiginfo_free(ksiginfo_t *ksi) {
    if (ksi) {
        slab_free(ksi);
    }
}

// Clean the signal queue of the given process for the specified signal number.
// If signo is 0, all signals in the queue are cleaned.
// Ksiginfo being cleaned will be freed.
// The caller must hold sigacts->lock.
// Returns 0 on success, -1 on error.
int sigpending_empty(struct proc *p, int signo) {
    if (!p) {
        return -EINVAL; // Invalid process pointer
    }
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }

    if (signo == 0) {
        // Purge all signal queues (signals numbered 1..NSIG map to index signo-1)
        for (int i = 1; i <= NSIG; i++) {
            ksiginfo_t *ksi = NULL;
            ksiginfo_t *tmp = NULL;
            sigpending_t *sq = &p->signal.sig_pending[i - 1];
            list_foreach_node_safe(&sq->queue, ksi, tmp, list_entry) {
                list_node_detach(ksi, list_entry);
                ksiginfo_free(ksi);
            }
        }
        p->signal.sig_pending_mask = 0;
        // Update sigpending flag after clearing all pending signals
        PROC_CLEAR_SIGPENDING(p);
        return 0;
    }

    if (SIGBAD(signo)) {
        return -EINVAL; // Invalid signal number
    }

    ksiginfo_t *ksi = NULL;
    ksiginfo_t *tmp = NULL;
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    list_foreach_node_safe(&sq->queue, ksi, tmp, list_entry) {
        list_node_detach(ksi, list_entry);
        ksiginfo_free(ksi); // Free the ksiginfo after removing it
    }
    sigdelset(&p->signal.sig_pending_mask, signo);
    // Update sigpending flag after modifying pending mask (caller already holds sigacts lock)
    recalc_sigpending_tsk(p);
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
        return -EINVAL; // Invalid signal number or signal actions
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
        return -EINVAL;
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
    spin_init(&sa->lock, "sigacts_lock");
    sa->refcount = 1;
    
    for (int i = 1; i <= NSIG; i++) {
        assert(__sig_setdefault(sa, i) == 0, "sigacts_init: failed to set default action for signal %d", i);
    }
    return sa;
}

sigacts_t *sigacts_dup(sigacts_t *psa, uint64 clone_flags) {
    if (!psa) {
        return NULL;
    }
    if (clone_flags & CLONE_SIGHAND) {
        // Share the signal actions
        // simply increase the reference count
        atomic_inc(&psa->refcount);
        return psa;
    }
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (sa) {
        sigacts_lock(psa);
        memmove(sa, psa, sizeof(sigacts_t));
        // Correctly copy process-level masks; do not derive from sa[0].
        sa->sa_sigmask = psa->sa_sigmask;
        sa->sa_original_mask = psa->sa_original_mask;
        sigacts_unlock(psa);
        
        // CRITICAL: Reinitialize the lock and refcount after copying!
        // memmove copies the locked spinlock state, which would make
        // the new sigacts appear to be locked by someone else.
        spin_init(&sa->lock, "sigacts_lock");
        sa->refcount = 1;
    }
    return sa;
}

void sigacts_put(sigacts_t *sa) {
    if (sa != NULL && !atomic_dec_unless(&sa->refcount, 1)) {
        slab_free(sa);
    }
}

void signal_init(void) {
    slab_cache_init(&__sigacts_pool, "sigacts", sizeof(sigacts_t), SLAB_FLAG_STATIC);
    slab_cache_init(&__ksiginfo_pool, "ksiginfo", sizeof(ksiginfo_t), SLAB_FLAG_STATIC);
}

// Cap for number of queued ksiginfo entries per signal when SA_SIGINFO set.
#define MAX_SIGINFO_PER_SIGNAL 8

// Helper: count ksiginfo entries currently queued for a signal.
static int __siginfo_queue_len(struct proc *p, int signo) {
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    int n = 0;
    ksiginfo_t *pos = NULL; ksiginfo_t *tmp = NULL;
    list_foreach_node_safe(&sq->queue, pos, tmp, list_entry) {
        n++;
    }
    return n;
}

int __signal_send(struct proc *p, ksiginfo_t *info) {
    int ret = -EINVAL; // Default to error
    if (p == NULL || info == NULL) {
        return -EINVAL; // No process available
    }
    if (SIGBAD(info->signo)) {
        return -EINVAL; // Invalid process or signal number
    }

    // Check process validity - use atomic load for lockless initial check
    enum procstate pstate = __proc_get_pstate(p);
    if (pstate == PSTATE_UNUSED || pstate == PSTATE_ZOMBIE || PROC_KILLED(p)) {
        return -EINVAL; // Process is not valid or already killed
    }

    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return -EINVAL; // No signal actions available
    }

    // Lock sigacts - this is the unified signal lock
    sigacts_lock(sa);

    // ignored signals are not sent
    if (sigismember(&sa->sa_sigignore, info->signo)) {
        sigacts_unlock(sa);
        return 0;
    }
    
    sigaction_t *act = &sa->sa[info->signo];
    if (act->sa_flags & SA_SIGINFO) {
        assert(info->signo != SIGKILL && info->signo != SIGSTOP, 
               "signal_send: SA_SIGINFO set for SIGKILL or SIGSTOP");
        // Enforce per-signal queue cap. If cap reached, drop oldest entry.
        int qlen = __siginfo_queue_len(p, info->signo);
        if (qlen >= MAX_SIGINFO_PER_SIGNAL) {
            // Drop head (oldest) then append new info to tail.
            sigpending_t *sq = &p->signal.sig_pending[info->signo - 1];
            if (!LIST_IS_EMPTY(&sq->queue)) {
                ksiginfo_t *old = LIST_FIRST_NODE(&sq->queue, ksiginfo_t, list_entry);
                if (old) {
                    list_entry_detach(&old->list_entry);
                    ksiginfo_free(old);
                }
            }
        }
        ksiginfo_t *ksi = ksiginfo_alloc();
        if (!ksi) {
            ret = -EINVAL; // Allocation failed (still set pending bit below)
            goto after_enqueue; // fall through to set pending bit & notify
        }
        *ksi = *info; // Copy the signal info
        list_entry_init(&ksi->list_entry);
        // Add to pending queue
        list_node_push(&p->signal.sig_pending[info->signo - 1].queue, ksi, list_entry);
    }

after_enqueue:
    // Always record the signal as pending (even for stop signals) to allow
    // later logic (e.g., mask changes) to notice it.
    sigaddset(&p->signal.sig_pending_mask, info->signo);
    
    // Update sigpending flag after adding to pending mask
    recalc_sigpending_tsk(p);

    bool is_stop = sigismember(&sa->sa_sigstop, info->signo) && !sigismember(&sa->sa_sigmask, info->signo);
    bool is_cont = sigismember(&sa->sa_sigcont, info->signo) && !sigismember(&sa->sa_sigmask, info->signo);
    bool is_term = sigismember(&sa->sa_sigterm, info->signo);
    sigset_t sigmask = sa->sa_sigmask;
    
    // Release sigacts lock before scheduler operations
    sigacts_unlock(sa);

    // For scheduler operations, we need proc_lock to check/modify state
    if (is_stop) {
        // Stop signals: The process will enter PSTATE_STOPPED voluntarily when it
        // processes signals in handle_signal(). If it's currently sleeping in an
        // interruptible state, wake it up so it can process the stop signal.
        proc_lock(p);
        pstate = __proc_get_pstate(p);
        if (PSTATE_IS_INTERRUPTIBLE(pstate)) {
            // Wake up interruptible sleeper so it can handle the stop signal
            proc_unlock(p);
            scheduler_wakeup(p);
        } else if (pstate == PSTATE_RUNNING) {
            proc_unlock(p);
            // Process is running, send IPI so it handles the stop signal promptly
            int target_cpu = smp_load_acquire(&p->sched_entity->cpu_id);
            if (target_cpu != cpuid()) {
                ipi_send_single(target_cpu, IPI_REASON_RESCHEDULE);
            } else {
                SET_NEEDS_RESCHED();
            }
        } else {
            proc_unlock(p);
        }
        // If uninterruptible, the process will handle the stop signal when it wakes up
    }
    if (is_cont) {
        // Continue signal: Wake up the process from PSTATE_STOPPED state.
        scheduler_wakeup_stopped(p);
    }

    ret = (ret == -EINVAL) ? 0 : ret; // Treat enqueue allocation failure as soft failure.

    // If the action is to terminate the process, set the killed flag
    if (is_term) {
        PROC_SET_KILLED(p);
        if (PROC_STOPPED(p)) {
            // If the process is stopped, we need to wake it up.
            scheduler_wakeup_stopped(p);
        }
    }
    
    // Check if signal is pending (unmasked) and notify if process is sleeping
    sigset_t pending_unmasked = smp_load_acquire(&p->signal.sig_pending_mask) & ~sigmask;
    if (pending_unmasked != 0) {
        proc_lock(p);
        signal_notify(p);
        proc_unlock(p);
    }

    return ret; // Signal sent successfully
}

int signal_send(int pid, ksiginfo_t *info) {
    struct proc *p = NULL;
    if (pid < 0 || info == NULL || SIGBAD(info->signo)) {
        return -EINVAL; // Invalid PID or signal number
    }
    rcu_read_lock();
    if (proctab_get_pid_proc(pid, &p) != 0) {
        rcu_read_unlock();
        return -EINVAL; // No process found
    }
    if (p == NULL) {
        rcu_read_unlock();
        return -EINVAL; // No process found
    }
    assert(p != NULL, "signal_send: proc is NULL");
    
    int ret = __signal_send(p, info);
    rcu_read_unlock();
    return ret;
}

bool signal_pending(struct proc *p) {
    if (!p) {
        return false;
    }
    // Fast path: just check the flag
    return PROC_SIGPENDING(p);
}

// Version that takes sigacts as parameter when caller already holds sigacts_lock
// Caller must hold proc_lock but NOT sigacts_lock (we acquire it here)
bool signal_pending_locked(struct proc *p, sigacts_t *sa) {
    if (!p || !sa) {
        return false;
    }
    // Fast path: just check the flag
    return PROC_SIGPENDING(p);
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
        // Must follow wakeup locking protocol:
        // - Release proc_lock (must NOT be held during wakeup)
        // - Call wakeup (no pi_lock needed - rq_lock serializes)
        // - Reacquire proc_lock
        proc_unlock(p);
        scheduler_wakeup_interruptible(p);
        proc_lock(p);
        return 0; // Success
    }
    return -1; // No signals pending or process not in interruptible state
}

bool signal_terminated(struct proc *p) {
    if (!p) {
        return 0;
    }
    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return false;  // No sigacts means no termination signals
    }
    sigacts_lock(sa);
    sigset_t masked = p->signal.sig_pending_mask & ~sa->sa_sigmask;
    bool terminated = (masked & sa->sa_sigterm) != 0;
    sigacts_unlock(sa);
    return terminated;
}

bool signal_test_clear_stopped(struct proc *p) {
    if (!p) {
        return 0;
    }
    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return PROC_STOPPED(p);  // No sigacts, just return current stopped state
    }
    
    sigacts_lock(sa);
    sigset_t sigmask = sa->sa_sigmask;
    sigset_t sigstop_mask = sa->sa_sigstop;
    sigset_t sigcont_mask = sa->sa_sigcont;
    sigset_t masked = p->signal.sig_pending_mask & ~sigmask;
    sigset_t pending_stopped = masked & sigstop_mask;
    sigset_t pending_cont = masked & sigcont_mask;

    if (pending_cont) {
        // A continue-category signal is pending. Determine if any of them
        // have user handlers installed. We resume the process in all cases.
        bool user_handler = false;
        for (int signo = 1; signo <= NSIG; signo++) {
            if (sigismember(&sigcont_mask, signo) > 0 && sigismember(&pending_cont, signo) > 0) {
                sigaction_t *act = &sa->sa[signo];
                if (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
                    user_handler = true;
                    break;
                }
            }
        }
        // Clear all pending stop signals (they are canceled by any continue)
        p->signal.sig_pending_mask &= ~sigstop_mask;
        if (!user_handler) {
            // Default action: consume the continue signals here so they are not delivered.
            p->signal.sig_pending_mask &= ~pending_cont;
        }
        // Recalc after modifying pending mask
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);
        return 0; // Do not request stop.
    }

    if (pending_stopped) {
        // Consume all pending stop signals (they stop the process) and request STOPPED state.
        p->signal.sig_pending_mask &= ~pending_stopped;
        // Recalc after modifying pending mask
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);
        return 1; // Caller will transition to PSTATE_STOPPED.
    }

    sigacts_unlock(sa);
    // No new stop/cont signals; indicate whether process is already stopped.
    return PROC_STOPPED(p);
}

int signal_restore(struct proc *p, ucontext_t *context) {
    if (p == NULL || context == NULL) {
        return -1; // Invalid process or context
    }
    
    sigacts_t *sa = p->sigacts;
    sigacts_lock(sa);
    
    p->signal.sig_stack = context->uc_stack;
    p->signal.sig_ucontext = (uint64)context->uc_link;
    
    if (p->signal.sig_ucontext == 0) {
        sa->sa_sigmask = sa->sa_original_mask; // Reset to original mask
    } else {
        sa->sa_sigmask = context->uc_sigmask;
        sa->sa_sigmask |= sa->sa_original_mask; // Update the signal mask
    }

    sigdelset(&sa->sa_sigmask, SIGKILL);
    sigdelset(&sa->sa_sigmask, SIGSTOP);
    sigdelset(&sa->sa_sigignore, SIGKILL);
    sigdelset(&sa->sa_sigignore, SIGSTOP);
    // Recalc sigpending after changing blocked mask
    recalc_sigpending_tsk(p);
    sigacts_unlock(sa);

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

    if (!PROC_USER_SPACE(p)) {
        return -1; // sigaction can only be called in user space
    }
    
    sigacts_t *sa = p->sigacts;
    
    // Use only sigacts_lock for signal state protection
    sigacts_lock(sa);
    
    if (oldact) {
        *oldact = sa->sa[signum];
    }
    
    if (act) {
        __sig_reset_act_mask(sa, signum);
        if (act->sa_handler == SIG_IGN) {
            sigaddset(&sa->sa_sigignore, signum);
        } else if (act->sa_handler == SIG_DFL) {
            if (__sig_setdefault(sa, signum) != 0) {
                sigacts_unlock(sa);
                return -1; // Failed to set default action
            }
            // After changing to SIG_DFL, check if any pending signals 
            // are now termination signals and update PROC_KILLED accordingly
            sigset_t pending_term = p->signal.sig_pending_mask & sa->sa_sigterm & ~sa->sa_sigmask;
            if (pending_term != 0) {
                PROC_SET_KILLED(p);
            }
        }
        sa->sa[signum] = *act;
        sigdelset(&sa->sa[signum].sa_mask, SIGKILL); // Unblock the signal
        sigdelset(&sa->sa[signum].sa_mask, SIGSTOP); // Unblock the signal
        // Clear pending signals for this signum (already holding sigacts_lock)
        if (sigpending_empty(p, signum) != 0) {
            sigacts_unlock(sa);
            return -1; // Failed to clear pending signals
        }
    }
    
    sigacts_unlock(sa);
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

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigprocmask: sigacts is NULL");
    
    // Use only sigacts_lock for signal state protection
    sigacts_lock(sa);
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

    // Mandatory signals cannot be blocked
    sigdelset(&sa->sa_original_mask, SIGKILL);
    sigdelset(&sa->sa_original_mask, SIGSTOP);
    sigdelset(&sa->sa_sigmask, SIGKILL);
    sigdelset(&sa->sa_sigmask, SIGSTOP);
    
    // Recalc sigpending flag after changing blocked mask
    recalc_sigpending_tsk(p);
    
    // Check if newly unmasked signals are pending
    sigset_t pending_unmasked = p->signal.sig_pending_mask & ~sa->sa_sigmask;
    
    // If newly unmasked termination signals are pending, set PROC_KILLED
    sigset_t pending_term = pending_unmasked & sa->sa_sigterm;
    if (pending_term != 0) {
        PROC_SET_KILLED(p);
    }
    sigacts_unlock(sa);

    // If newly unmasked signals are pending and process is sleeping, wake it.
    // Need proc_lock for signal_notify (which checks process state)
    if (pending_unmasked != 0) {
        proc_lock(p);
        (void)signal_notify(p);
        proc_unlock(p);
    }
    return 0; // Success
}

int sigpending(struct proc *p, sigset_t *set) {
    if (!set) {
        return -1; // Invalid set pointer
    }
    assert(p != NULL, "sigpending: myproc returned NULL");

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "sigpending: sigacts is NULL");
    
    // Use only sigacts_lock - it protects both masks
    sigacts_lock(sa);
    sigset_t mask = sa->sa_sigmask;
    *set = mask & p->signal.sig_pending_mask; // Get the pending signals
    sigacts_unlock(sa);
    
    return 0; // Success
}

int sigreturn(void) {
    struct proc *p = myproc();
    assert(p != NULL, "sys_sigreturn: myproc returned NULL");

    if (!PROC_USER_SPACE(p)) {
        return -1; // sigreturn can only be called in user space
    }

    sigacts_t *sa = p->sigacts;
    sigacts_lock(sa);
    if (p->signal.sig_ucontext == 0) {
        sigacts_unlock(sa);
        return -1; // No signal trap frame to restore
    }
    sigacts_unlock(sa);

    // Call restore_sigframe without holding sigacts_lock since it calls
    // vm_copyin which needs vm_rlock (sleep lock)
    ucontext_t uc = {0};
    if (restore_sigframe(p, &uc) != 0) {
        // @TODO:
        exit(-1); // Restore failed, exit the process
    }
    
    // signal_restore now acquires sigacts_lock internally
    assert(signal_restore(p, &uc) == 0, "sigreturn: signal_restore failed");

    return 0; // Success
}

// Dequeue signal - caller provides the sigaction copy.
// Caller must hold sigacts->lock.
static ksiginfo_t *__dequeue_signal_update_pending_nolock(struct proc *p, int signo, 
                                                           sigaction_t *act) {
    if (p == NULL || act == NULL) {
        return ERR_PTR(-EINVAL);
    }
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }
    
    if (SIGBAD(signo)) {
        return ERR_PTR(-EINVAL);
    }

    assert(act->sa_handler != SIG_IGN,
           "__dequeue_signal_update_pending_nolock: signal handler is SIG_IGN");
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    if ((act->sa_flags & SA_SIGINFO) == 0) {
        assert(LIST_IS_EMPTY(&sq->queue),
               "sig_pending is not empty for a non-SA_SIGINFO signal");
        sigdelset(&p->signal.sig_pending_mask, signo);
        // Caller should call recalc_sigpending while still holding lock
        return NULL; // No signal info to return
    }

    // Pop exactly one ksiginfo (FIFO order: head of list).
    if (LIST_IS_EMPTY(&sq->queue)) {
        // Queue empty but bit set implies inconsistency; clear defensively.
        sigdelset(&p->signal.sig_pending_mask, signo);
        return NULL;
    }
    ksiginfo_t *info = LIST_FIRST_NODE(&sq->queue, ksiginfo_t, list_entry);
    assert(info->signo == signo, "__dequeue_signal_update_pending_nolock: pos->signo != signo");
    list_entry_detach(&info->list_entry);
    // If queue now empty, clear pending bit; else leave it set for further delivery.
    if (LIST_IS_EMPTY(&sq->queue)) {
        sigdelset(&p->signal.sig_pending_mask, signo);
    }
    return info;
}

static int __deliver_signal(struct proc *p, int signo, ksiginfo_t *info, sigaction_t *sa, bool *repeat) {
    // NOTE: This function is called WITHOUT proc_lock held to allow
    // push_sigframe to acquire vm_wlock (sleep lock). The caller must
    // ensure the signal state (sa, info) was captured while holding the lock.
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

    // Other than SIG_IGN and SIG_CONT, all signal handlers must be placed 
    // beyond the first page of the address space.
    if ((uint64)sa->sa_handler < PAGE_SIZE) {
        printf("__deliver_signal: invalid signal handler address %p for signal %d\n", 
               sa->sa_handler, signo);
        proc_lock(p);
        PROC_SET_KILLED(p);
        proc_unlock(p);
        return 0;
    }

    int ret = 0;
    if (PROC_USER_SPACE(myproc())) {
        // If the process has user space, push the signal to its user stack
        // This may call vm_try_growstack which needs vm_wlock (sleep lock)
        ret = push_sigframe(p, signo, sa, info);
    }

    // Acquire sigacts_lock to update signal masks
    sigacts_t *sigacts = p->sigacts;
    sigacts_lock(sigacts);
    if ((sa->sa_flags & SA_NODEFER) == 0) {
        sigaddset(&sigacts->sa_sigmask, signo);
    }

    sigacts->sa_sigmask |= sa->sa_mask; // Block the signal during delivery
    sigdelset(&sigacts->sa_sigmask, SIGKILL);
    sigdelset(&sigacts->sa_sigmask, SIGSTOP);

    // Recalc sigpending flag after blocking signals
    recalc_sigpending_tsk(p);

    if ((sa->sa_flags & SA_RESETHAND) != 0) {
        // Reset the signal handler to default action
        assert(__sig_setdefault(sigacts, signo) == 0,
               "__deliver_signal: __sig_setdefault failed");
    }
    sigacts_unlock(sigacts);
    
    return ret;
}

void handle_signal(void) {
    struct proc *p = myproc();
    assert(p != NULL, "handle_signal: myproc returned NULL");
    if (p->sigacts == NULL) {
        return; // No signal actions defined
    }
    sigacts_t *sa = p->sigacts;

    for (;;) {
        // Gather all signal info with sigacts_lock - this protects all signal state
        sigacts_lock(sa);
        sigset_t sigmask = sa->sa_sigmask;
        sigset_t sigterm = sa->sa_sigterm;
        sigset_t sigstop = sa->sa_sigstop;
        sigset_t sigcont = sa->sa_sigcont;
        sigset_t pending = p->signal.sig_pending_mask;
        sigset_t masked = pending & ~sigmask;
        
        // Check termination
        if ((masked & sigterm) || PROC_KILLED(p)) {
            PROC_SET_KILLED(p);
            sigacts_unlock(sa);
            break;
        }
        
        // Check stop/continue
        sigset_t pending_cont = masked & sigcont;
        sigset_t pending_stop = masked & sigstop;
        
        if (pending_cont) {
            // Continue cancels stop - clear stop signals
            p->signal.sig_pending_mask &= ~sigstop;
            
            // Check if any pending SIGCONT has a user handler
            bool user_handler = false;
            for (int signo = 1; signo <= NSIG; signo++) {
                if (sigismember(&sigcont, signo) > 0 && sigismember(&pending_cont, signo) > 0) {
                    sigaction_t *act = &sa->sa[signo];
                    if (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
                        user_handler = true;
                        break;
                    }
                }
            }
            
            if (!user_handler) {
                // Default action: consume the continue signals here
                p->signal.sig_pending_mask &= ~pending_cont;
                // Recalc sigpending flag after modifying pending mask
                recalc_sigpending_tsk(p);
                sigacts_unlock(sa);
                continue;  // No handler to call, loop back
            }
            // If user_handler is true, leave pending_cont bits set
            // and fall through to deliver the signal to the user handler
            // (don't call continue - let the delivery code below handle it)
        } else if (pending_stop) {
            // Clear stop signals and enter stopped state
            p->signal.sig_pending_mask &= ~pending_stop;
            // Recalc sigpending flag after modifying pending mask
            recalc_sigpending_tsk(p);
            sigacts_unlock(sa);
            
            // Use proc_lock for state transition
            proc_lock(p);
            __proc_set_pstate(p, PSTATE_STOPPED);
            proc_unlock(p);
            scheduler_yield();
            continue; // Re-check after wakeup
        }
        
        // Find first deliverable signal
        int signo = (masked != 0) ? bits_ffsg(masked) : 0;
        if (signo == 0 || signo > NSIG) {
            sigacts_unlock(sa);
            break; // No pending signals
        }
        
        // Skip stop signals (they were handled above and consumed)
        // Note: SIGCONT with user handler was NOT consumed above, so don't skip it
        if (sigismember(&sigstop, signo)) {
            sigacts_unlock(sa);
            continue;
        }
        
        // Copy sigaction and dequeue while holding sigacts_lock
        sigaction_t sa_copy = sa->sa[signo];
        
        // Re-check if signal is still pending (might have been handled by another path)
        if (!sigismember(&p->signal.sig_pending_mask, signo)) {
            sigacts_unlock(sa);
            continue; // Signal was consumed elsewhere, try again
        }
        
        bool repeat = false;
        ksiginfo_t *info = __dequeue_signal_update_pending_nolock(p, signo, &sa_copy);
    
        assert(!IS_ERR(info), "handle_signal: __dequeue_signal_update_pending failed");
        
        // Recalc sigpending after dequeue modified the pending mask
        recalc_sigpending_tsk(p);
        
        // Release sigacts_lock before calling __deliver_signal, which may need
        // to acquire vm_wlock (sleep lock) via push_sigframe/vm_try_growstack
        sigacts_unlock(sa);
        
        assert(__deliver_signal(p, signo, info, &sa_copy, &repeat) == 0,
               "handle_signal: __deliver_signal failed");

        // Check repeat condition with sigacts_lock only
        if (sa_copy.sa_flags & SA_SIGINFO) {
            sigacts_lock(sa);
            bool unmasked = sigismember(&sa->sa_sigmask, signo) == 0;
            bool still_pending = sigismember(&p->signal.sig_pending_mask, signo) > 0;
            sigacts_unlock(sa);
            
            if (unmasked && still_pending) {
                repeat = true;
            }
        }
    
        if (info) {
            ksiginfo_free(info);
        }

        if (!repeat) {
            break;
        }
    }
    if (PROC_KILLED(p)) {
        exit(-1);
    }
}


// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid, int signum) {
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = myproc();
    info.info.si_pid = myproc()->pid;

    return signal_send(pid, &info);
}

// Kill the given process.
// Instead of looking up by pid, directly send signal to the given proc.
int kill_proc(struct proc *p, int signum) {
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = myproc();
    info.info.si_pid = myproc()->pid;

    rcu_read_lock();
    int ret = __signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

// Check if process should be terminated.
// This only checks the PROC_KILLED flag which is set atomically by __signal_send
// when a termination signal is delivered. No locks needed.
int killed(struct proc *p) {
    if (!p) {
        return 0;
    }
    return PROC_KILLED(p);
}
