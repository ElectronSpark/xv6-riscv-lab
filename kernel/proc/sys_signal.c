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

uint64 sys_sigprocmask(void) {
    int how;
    uint64 set_addr, oldset_addr;
    sigset_t set, oldset;

    argint(0, &how);
    argaddr(1, &set_addr);
    argaddr(2, &oldset_addr);

    if (set_addr != 0) {
        if (either_copyin(&set, 1, set_addr, sizeof(sigset_t)) < 0) {
            return -1; // Copy failed
        }
    } else {
        set = 0; // No set provided
    }

    if (sigprocmask(how, &set, &oldset) < 0) {
        return -1; // sigprocmask failed
    }

    if (oldset_addr != 0) {
        if (either_copyout(1, oldset_addr, &oldset, sizeof(sigset_t)) < 0) {
            return -1; // Copy failed
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

uint64 sys_sigpending(void) {
    uint64 set_addr;
    sigset_t set;
    argaddr(0, &set_addr);
    if (sigpending(current, &set) < 0) {
        return -1; // sigpending failed
    }
    if (set_addr != 0) {
        if (either_copyout(1, set_addr, &set, sizeof(sigset_t)) < 0) {
            return -1; // Copy failed
        }
    }
    return 0; // Success
}

uint64 sys_sigreturn(void) {
    if (sigreturn() < 0) {
        return -1; // sigreturn failed
    }
    
    struct thread *p = current;
    assert(p != NULL, "sys_sigreturn: current returned NULL");

    return 0;
}

uint64 sys_pause(void) {
    struct thread *p = current;
    tcb_lock(p);
    // If an unblocked pending signal already exists, return immediately
    if (signal_pending(p)) {
        tcb_unlock(p);
        return 0;
    }
    tcb_unlock(p);
    scheduler_sleep(NULL, THREAD_INTERRUPTIBLE); // Pause the current thread
    return 0;
}

uint64 sys_kill(void) {
    int pid;
    int signum;

    argint(0, &pid);
    argint(1, &signum);

    return kill(pid, signum);
}

