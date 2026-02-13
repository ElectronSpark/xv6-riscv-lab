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

    // Return the restored a0 from the sigframe so the syscall dispatcher
    // doesn't overwrite it. This preserves the original return value
    // (e.g. -EINTR from sigsuspend) across signal handler execution.
    return p->trapframe->trapframe.a0;
}

uint64 sys_pause(void) {
    struct thread *p = current;
    // Mark interruptible before checking signals to close the race where
    // a signal arrives between the check and the yield.
    // Note: a tiny window remains where a wakeup can transition the state
    // back to RUNNING before scheduler_yield runs, causing the signal to
    // be missed.  See @TODO in thread_types.h ("stop signal may miss").
    __thread_state_set(p, THREAD_INTERRUPTIBLE);
    tcb_lock(p);
    // If an unblocked pending signal already exists, return immediately
    if (signal_pending(p)) {
        __thread_state_set(p, THREAD_RUNNING);
        tcb_unlock(p);
        return 0;
    }
    tcb_unlock(p);
    scheduler_yield();
    return 0;
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
