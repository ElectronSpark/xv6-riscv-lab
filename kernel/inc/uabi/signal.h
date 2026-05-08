#ifndef __USER_ABI_SIGNAL_H
#define __USER_ABI_SIGNAL_H

#include "types.h"
#include "signo.h"

#if !defined(ON_HOST_OS)
typedef uint64 sigset_t;
#endif

typedef union sigval {
    int sival_int;
    void *sival_ptr;
} sigval_t;

typedef struct siginfo siginfo_t;

typedef struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    unsigned long sa_flags;
#if defined(CONFIG_ARCH_X86_64) || defined(__x86_64__)
    void (*sa_restorer)(void);
#endif
    sigset_t sa_mask;
#if !(defined(CONFIG_ARCH_X86_64) || defined(__x86_64__))
    void *_sa_unused;
#endif
} sigaction_t;

typedef struct stack {
    void *ss_sp;         /* stack base pointer */
    int ss_flags;        /* flags */
#define SS_AUTOREARM 0x1 /* automatically rearm the signal stack */
#define SS_ONSTACK 0x2   /* use the alternate stack */
#define SS_DISABLE 0x4   /* disable the signal stack */
    size_t ss_size;      /* size */
} stack_t;

typedef enum {
    SIG_ACT_INVALID = -1,
    SIG_ACT_IGN = 0,
    SIG_ACT_TERM,
    SIG_ACT_CORE,
    SIG_ACT_STOP,
    SIG_ACT_CONT
} sig_defact;

// The following structures are copied from Advanced Programming in the UNIX
// Environment
// @TODO: currently unused
typedef struct siginfo {
    int si_signo; /* signal number */
    int si_errno; /* if nonzero, errno value from errno.h */
    int si_code;  /* additional info (depends on signal) */
    int si_pid;   /* sending process ID */
    // int si_uid; /* sending process real user ID */
    void *si_addr;         /* address that caused the fault */
    int si_status;         /* exit value */
    union sigval si_value; /* application-specific value */
                           /* possibly other fields also */
} siginfo_t;

/*
 * RISC-V general-purpose register state (POSIX / Linux sigcontext compatible)
 *
 * Register ordering follows the Linux RISC-V ABI:
 *   pc, ra (x1), sp (x2), gp (x3), tp (x4),
 *   t0-t2 (x5-x7), s0/fp (x8), s1 (x9),
 *   a0-a7 (x10-x17), s2-s11 (x18-x27), t3-t6 (x28-x31)
 */
typedef struct {
    uint64 pc;  /* x0 slot used for pc (x0 is hardwired zero) */
    uint64 ra;  /* x1  - return address */
    uint64 sp;  /* x2  - stack pointer */
    uint64 gp;  /* x3  - global pointer */
    uint64 tp;  /* x4  - thread pointer */
    uint64 t0;  /* x5  */
    uint64 t1;  /* x6  */
    uint64 t2;  /* x7  */
    uint64 s0;  /* x8  - frame pointer */
    uint64 s1;  /* x9  */
    uint64 a0;  /* x10 */
    uint64 a1;  /* x11 */
    uint64 a2;  /* x12 */
    uint64 a3;  /* x13 */
    uint64 a4;  /* x14 */
    uint64 a5;  /* x15 */
    uint64 a6;  /* x16 */
    uint64 a7;  /* x17 */
    uint64 s2;  /* x18 */
    uint64 s3;  /* x19 */
    uint64 s4;  /* x20 */
    uint64 s5;  /* x21 */
    uint64 s6;  /* x22 */
    uint64 s7;  /* x23 */
    uint64 s8;  /* x24 */
    uint64 s9;  /* x25 */
    uint64 s10; /* x26 */
    uint64 s11; /* x27 */
    uint64 t3;  /* x28 */
    uint64 t4;  /* x29 */
    uint64 t5;  /* x30 */
    uint64 t6;  /* x31 */
} __riscv_mc_gp_state;

/*
 * RISC-V double-precision floating-point register state (placeholder)
 * TODO: Save/restore FP state when FPU support is enabled.
 */
struct __riscv_mc_d_ext_state {
    uint64 f[32]; /* f0-f31 */
    uint32 fcsr;  /* FP control/status register */
};

typedef union {
    struct __riscv_mc_d_ext_state d;
    /* Add __riscv_mc_q_ext_state here for quad-precision if needed */
} __riscv_mc_fp_state;

/*
 * Machine context - POSIX mcontext_t for RISC-V
 * Modelled after Linux struct sigcontext
 * (arch/riscv/include/uapi/asm/sigcontext.h)
 */
typedef struct {
    __riscv_mc_gp_state sc_regs;   /* general-purpose registers */
    __riscv_mc_fp_state sc_fpregs; /* floating-point registers (placeholder) */
} mcontext_t;

typedef struct ucontext {
    struct ucontext *uc_link; /* pointer to context resumed when */
    /* this context returns */
    sigset_t uc_sigmask; /* signals blocked when this context */
    /* is active */
    stack_t uc_stack;       /* stack used by this context */
    mcontext_t uc_mcontext; /* machine-specific representation of */
                            /* saved context */
} ucontext_t;

#define SIGBAD(signo) ((signo) < 1 || (signo) > NSIG)

#define SIGMASK(signo) (SIGBAD(signo) ? 0 : (1UL << ((uint64)(signo) - 1)))

// All four return: 0 if OK, −1 on error
static inline int sigemptyset(sigset_t *set) {
    if (!set) {
        return -1; // Invalid set pointer
    }
    *set = 0; // Clear the signal set
    return 0; // Success
}

static inline int sigfillset(sigset_t *set) {
    if (!set) {
        return -1; // Invalid set pointer
    }
    *set = (1UL << NSIG) - 1; // Set all signals
    return 0;                 // Success
}

static inline int sigaddset(sigset_t *set, int signo) {
    sigset_t mask = SIGMASK(signo);
    if (mask == 0) {
        return -1; // Invalid set pointer or signal number
    }
    *set |= mask;
    return 0; // Success
}

static inline int sigdelset(sigset_t *set, int signo) {
    sigset_t mask = SIGMASK(signo);
    if (mask == 0) {
        return -1; // Invalid set pointer or signal number
    }
    *set &= ~mask;
    return 0; // Success
}

// Returns: 1 if true, 0 if false, −1 on error
static inline int sigismember(const sigset_t *set, int signo) {
    sigset_t mask = SIGMASK(signo);
    if (mask == 0) {
        return -1; // Invalid set pointer or signal number
    }
    return (*set & mask) != 0; // Return 1 if member, 0 otherwise
}

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define MINSIGSTKSZ (1UL << PGSHIFT)
#define SIGSTKSZ (1UL << (PGSHIFT + 2))

#endif /* __USER_ABI_SIGNAL_H */
