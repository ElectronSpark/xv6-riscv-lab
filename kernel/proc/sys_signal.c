#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "mm/vm.h"
#include "signal.h"
#include "syscall.h"
#include "errno.h"
#include "timer/timer.h"

uint64 sys_sigprocmask(void) {
    int how;
    uint64 set_addr, oldset_addr;
    sigset_t set, oldset;

    argint(0, &how);
    argaddr(1, &set_addr);
    argaddr(2, &oldset_addr);

    if (set_addr != 0) {
        if (either_copyin(&set, 1, set_addr, sizeof(sigset_t)) < 0) {
            return -EFAULT; // Copy failed
        }
    } else {
        set = 0; // No set provided
    }

    int ret = sigprocmask(how, set_addr ? &set : NULL, &oldset);
    if (ret < 0) {
        return ret;
    }

    if (oldset_addr != 0) {
        if (either_copyout(1, oldset_addr, &oldset, sizeof(sigset_t)) < 0) {
            return -EFAULT; // Copy failed
        }
    }

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
            return -EFAULT; // Copy failed
        }
    }
    if (oldact_addr != 0) {
        p_oldact = &oldact;
    }

    int ret = sigaction(signum, p_act, p_oldact);
    if (ret < 0) {
        return ret;
    }

    if (p_oldact != 0) {
        if (either_copyout(1, oldact_addr, p_oldact, sizeof(struct sigaction)) <
            0) {
            return -EFAULT; // Copy failed
        }
    }

    return 0; // Success
}

uint64 sys_sigpending(void) {
    uint64 set_addr;
    sigset_t set;
    argaddr(0, &set_addr);
    int ret = sigpending(current, &set);
    if (ret < 0) {
        return ret;
    }
    if (set_addr != 0) {
        if (either_copyout(1, set_addr, &set, sizeof(sigset_t)) < 0) {
            return -EFAULT; // Copy failed
        }
    }
    return 0; // Success
}

uint64 sys_sigreturn(void) {
    int ret = sigreturn();
    if (ret < 0) {
        return ret;
    }

    struct thread *p = current;
    assert(p != NULL, "sys_sigreturn: current returned NULL");

    // Return the restored return-value register from the sigframe so the
    // syscall dispatcher doesn't overwrite it.  This preserves the original
    // return value (e.g. -EINTR from sigsuspend) across signal handler
    // execution.
#ifdef __x86_64__
    return p->trapframe->trapframe.rax;
#else
    return p->trapframe->trapframe.a0;
#endif
}

uint64 sys_pause(void) {
    struct thread *p = current;
    // Mark interruptible before checking signals to close the race where
    // a signal arrives between the check and the yield.
    __thread_state_set(p, THREAD_INTERRUPTIBLE);
    tcb_lock(p);
    // If an unblocked pending signal already exists, return immediately
    if (signal_pending(p)) {
        __thread_state_set(p, THREAD_RUNNING);
        tcb_unlock(p);
        return (uint64)-EINTR;
    }
    tcb_unlock(p);
    scheduler_yield();
    return (uint64)-EINTR;
}

uint64 sys_kill(void) {
    int pid;
    int signum;

    argint(0, &pid);
    argint(1, &signum);

    return kill(pid, signum);
}

// tgkill() sends a signal to a specific thread within a thread group.
// This provides race-free signal delivery by verifying the thread
// still belongs to the specified thread group.
uint64 sys_tgkill(void) {
    int tgid, tid, sig;
    argint(0, &tgid);
    argint(1, &tid);
    argint(2, &sig);
    return tgkill(tgid, tid, sig);
}

// tkill() sends a signal to a specific thread by TID.
// This is the kernel-side implementation of pthread_kill().
uint64 sys_tkill(void) {
    int tid, sig;
    argint(0, &tid);
    argint(1, &sig);
    return tkill(tid, sig);
}

// sigsuspend() atomically replaces the signal mask and suspends
// until a signal is caught. Always returns -1 (EINTR).
uint64 sys_sigsuspend(void) {
    uint64 mask_addr;
    sigset_t mask;

    argaddr(0, &mask_addr);

    if (mask_addr == 0) {
        return -EINVAL;
    }

    if (either_copyin(&mask, 1, mask_addr, sizeof(sigset_t)) < 0) {
        return -EFAULT;
    }

    return sigsuspend(&mask); // Usually -EINTR
}

// sigwait() waits for a signal from a specified set.
// Returns 0 on success with the signal number stored at *sig_addr.
uint64 sys_sigwait(void) {
    uint64 set_addr;
    uint64 sig_addr;
    sigset_t set;
    int sig;

    argaddr(0, &set_addr);
    argaddr(1, &sig_addr);

    if (set_addr == 0 || sig_addr == 0) {
        return -EINVAL;
    }

    if (either_copyin(&set, 1, set_addr, sizeof(sigset_t)) < 0) {
        return -EFAULT;
    }

    int ret = sigwait(&set, &sig);
    if (ret < 0) {
        return ret;
    }

    if (either_copyout(1, sig_addr, &sig, sizeof(int)) < 0) {
        return -EFAULT;
    }

    return 0;
}

/* ── sigaltstack ─────────────────────────────────────────────────────── */

/**
 * sys_sigaltstack - get/set alternate signal stack
 * args: const stack_t *ss (new stack), stack_t *old_ss (old stack)
 */
uint64 sys_sigaltstack(void) {
    uint64 ss_addr, oss_addr;
    argaddr(0, &ss_addr);
    argaddr(1, &oss_addr);

    struct thread *p = current;
    stack_t *cur = &p->signal.sig_stack;

    /* Copy out old stack if requested */
    if (oss_addr != 0) {
        if (either_copyout(1, oss_addr, cur, sizeof(stack_t)) < 0)
            return -EFAULT;
    }

    /* Set new stack if requested */
    if (ss_addr != 0) {
        /* Cannot change the alt stack while executing on it */
        if (cur->ss_flags & SS_ONSTACK)
            return -EPERM;

        stack_t ss;
        if (either_copyin(&ss, 1, ss_addr, sizeof(stack_t)) < 0)
            return -EFAULT;

        if (ss.ss_flags & ~(SS_DISABLE | SS_ONSTACK | SS_AUTOREARM))
            return -EINVAL;

        if (ss.ss_flags & SS_DISABLE) {
            cur->ss_sp    = NULL;
            cur->ss_flags = SS_DISABLE;
            cur->ss_size  = 0;
        } else {
            if (ss.ss_size < MINSIGSTKSZ)
                return -ENOMEM;
            cur->ss_sp    = ss.ss_sp;
            cur->ss_flags = ss.ss_flags;
            cur->ss_size  = ss.ss_size;
        }
    }

    return 0;
}

/* ── ITIMER_REAL (setitimer / getitimer) ──────────────────────────────── */

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct k_itimerval {
    struct {
        uint64 tv_sec;
        uint64 tv_usec;
    } it_interval;
    struct {
        uint64 tv_sec;
        uint64 tv_usec;
    } it_value;
};

static void __itimer_real_fire(void *arg) {
    /* arg is (uint64)tgid | (gen << 32) packed value */
    uint64 packed = (uint64)arg;
    int tgid = (int)(packed & 0xFFFFFFFF);
    uint64 gen = packed >> 32;

    /* Look up the thread group leader; verify generation still matches */
    struct thread *p = NULL;
    rcu_read_lock();
    if (get_pid_thread(tgid, &p) != 0 || p == NULL) {
        rcu_read_unlock();
        return; /* process exited */
    }
    struct thread_group *tg = p->thread_group;
    if (tg == NULL || tg->itimer_real_gen != gen) {
        rcu_read_unlock();
        return; /* timer was cancelled or replaced */
    }

    /* Send SIGALRM to the process (thread group) */
    ksiginfo_t info = {0};
    info.signo = SIGALRM;
    info.sender = NULL;  /* kernel-generated */
    info.info.si_pid = 0;
    signal_send(tgid, &info);

    /* If interval is set, re-arm the timer */
    if (tg->itimer_interval_ms > 0) {
        tg->itimer_expire_ms = get_jiffs() + tg->itimer_interval_ms;
        uint64 next_packed = ((uint64)gen << 32) | (uint64)(uint32)tgid;
        sched_timer_add(__itimer_real_fire, (void *)next_packed,
                        tg->itimer_interval_ms);
    } else {
        tg->itimer_expire_ms = 0;
    }
    rcu_read_unlock();
}

/**
 * sys_setitimer - POSIX setitimer(2)
 * args: int which, const struct itimerval *new, struct itimerval *old
 *
 * Only ITIMER_REAL is supported. Sends SIGALRM when timer expires.
 */
uint64 sys_setitimer(void) {
    int which;
    uint64 new_addr, old_addr;
    argint(0, &which);
    argaddr(1, &new_addr);
    argaddr(2, &old_addr);

    if (which != ITIMER_REAL)
        return -EINVAL;

    struct thread *p = current;
    struct thread_group *tg = p->thread_group;
    if (tg == NULL)
        return -EINVAL;

    /* Read the old value before updating (if requested) */
    if (old_addr != 0) {
        struct k_itimerval old = {0};
        /* Return interval */
        old.it_interval.tv_sec = tg->itimer_interval_ms / 1000;
        old.it_interval.tv_usec = (tg->itimer_interval_ms % 1000) * 1000;
        /* Compute remaining time */
        uint64 now_ms = get_jiffs();
        if (tg->itimer_expire_ms > now_ms) {
            uint64 remain = tg->itimer_expire_ms - now_ms;
            old.it_value.tv_sec = remain / 1000;
            old.it_value.tv_usec = (remain % 1000) * 1000;
        }
        if (either_copyout(1, old_addr, &old, sizeof(old)) < 0)
            return -EFAULT;
    }

    /* Cancel any pending timer by bumping generation */
    tg->itimer_real_gen++;
    tg->itimer_expire_ms = 0;

    if (new_addr == 0)
        return 0;

    struct k_itimerval nv;
    if (either_copyin(&nv, 1, new_addr, sizeof(nv)) < 0)
        return -EFAULT;

    uint64 value_ms = nv.it_value.tv_sec * 1000 + nv.it_value.tv_usec / 1000;
    uint64 interval_ms = nv.it_interval.tv_sec * 1000 +
                         nv.it_interval.tv_usec / 1000;

    tg->itimer_interval_ms = interval_ms;

    if (value_ms == 0)
        return 0; /* disarm */

    /* Record when the timer will expire */
    tg->itimer_expire_ms = get_jiffs() + value_ms;

    /* Pack tgid + generation into a single pointer-sized value */
    uint64 packed = ((uint64)tg->itimer_real_gen << 32) |
                    (uint64)(uint32)tg->tgid;

    int ret = sched_timer_add(__itimer_real_fire, (void *)packed, value_ms);
    return ret;
}

/**
 * sys_getitimer - POSIX getitimer(2)
 * args: int which, struct itimerval *curr_value
 */
uint64 sys_getitimer(void) {
    int which;
    uint64 val_addr;
    argint(0, &which);
    argaddr(1, &val_addr);

    if (which != ITIMER_REAL)
        return -EINVAL;
    if (val_addr == 0)
        return -EINVAL;

    struct thread *p = current;
    struct thread_group *tg = p->thread_group;
    if (tg == NULL)
        return -EINVAL;

    struct k_itimerval val = {0};
    val.it_interval.tv_sec = tg->itimer_interval_ms / 1000;
    val.it_interval.tv_usec = (tg->itimer_interval_ms % 1000) * 1000;
    /* Compute remaining time */
    uint64 now_ms = get_jiffs();
    if (tg->itimer_expire_ms > now_ms) {
        uint64 remain = tg->itimer_expire_ms - now_ms;
        val.it_value.tv_sec = remain / 1000;
        val.it_value.tv_usec = (remain % 1000) * 1000;
    }

    if (either_copyout(1, val_addr, &val, sizeof(val)) < 0)
        return -EFAULT;
    return 0;
}
