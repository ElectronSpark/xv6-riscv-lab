/*
 * bits/signal.h — xv6 signal definitions for musl
 *
 * These MUST match the kernel's signal numbers and structures.
 * Copied from kernel/inc/signo.h and kernel/inc/signal_types.h.
 */

#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) \
 || defined(_BSD_SOURCE)

/* xv6 uses 64-bit sigset_t (matching kernel's typedef uint64 sigset_t) */
#define _NSIG 65    /* NSIG = 32 signals, but musl wants _NSIG > max signal */
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

/* Signal numbers — match xv6 kernel (kernel/inc/signo.h) */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   29  /* Same as SIGIO */
#define SIGPWR    30
#define SIGSYS    31

/* SA_* flags — match xv6 kernel (kernel/inc/signo.h) */
#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002
#define SA_SIGINFO    0x00000004
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000
#define SA_RESTORER   0x04000000

/* SIG_BLOCK etc. — match Linux kernel ABI */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#endif

#if defined(_BSD_SOURCE) || defined(_GNU_SOURCE)
#define NSIG _NSIG
#endif

typedef struct {
    unsigned long __bits[_NSIG/8/sizeof(long)];  /* 64 bits / 8 = 1 long */
} __sigset_t;

/* sigaction structure — must match xv6 kernel layout */
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sa_handler;
    unsigned long sa_mask;      /* xv6: sigset_t = uint64 */
    int sa_flags;
};

#define sa_handler   __sa_handler.sa_handler
#define sa_sigaction __sa_handler.sa_sigaction

/* Stack state flags */
#define SS_ONSTACK  0x2
#define SS_DISABLE  0x4
