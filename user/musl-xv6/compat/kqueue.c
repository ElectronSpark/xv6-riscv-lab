/**
 * @file kqueue.c
 * @brief kqueue/kevent implementation for musl on xv6
 *
 * Provides kqueue(), kevent_register(), kevent_wait(), and the BSD-compatible
 * combined kevent() wrapper.  These issue raw ecall syscalls to the xv6 kernel.
 *
 * xv6 syscall numbers:
 *   SYS_kqueue          = 65
 *   SYS_kevent_register = 66
 *   SYS_kevent_wait     = 67
 */

#include <sys/event.h>
#include <errno.h>
#include <limits.h>

/* ---- raw syscall helpers via syscall ---- */
/*
 * Linux x86-64 syscall convention:
 *   rcx and r11 are clobbered by SYSCALL (transaction registers).
 *   Arguments in rdi, rsi, rdx, r10, r8, r9.  Number in rax, return in rax.
 */

#if defined(__x86_64__)

static inline long __syscall0(long n)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}

static inline long __syscall3(long n, long a, long b, long c)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "a"(n), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
    long ret;
    register long r10 __asm__("r10") = d;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

#elif defined(__riscv)

static inline long __syscall0(long n)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0");
    __asm__ volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline long __syscall3(long n, long a, long b, long c)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    __asm__ volatile ("ecall"
        : "+r"(a0)
        : "r"(a7), "r"(a1), "r"(a2)
        : "memory");
    return a0;
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    __asm__ volatile ("ecall"
        : "+r"(a0)
        : "r"(a7), "r"(a1), "r"(a2), "r"(a3)
        : "memory");
    return a0;
}

#else
#error "Unsupported architecture"
#endif

#define SYS_kqueue          65
#define SYS_kevent_register 66
#define SYS_kevent_wait     67

int kqueue(void)
{
    long ret = __syscall0(SYS_kqueue);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int kevent_register(int kqfd, struct kevent *changelist, int nchanges)
{
    long ret = __syscall3(SYS_kevent_register,
                          (long)kqfd, (long)changelist, (long)nchanges);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int kevent_wait(int kqfd, struct kevent *eventlist, int nevents,
                int timeout_ms)
{
    long ret = __syscall4(SYS_kevent_wait,
                          (long)kqfd, (long)eventlist,
                          (long)nevents, (long)timeout_ms);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

/**
 * BSD-compatible kevent() — combines register + wait in one call.
 * Converts struct timespec to the integer milliseconds the xv6 kernel expects.
 */
int kevent(int kq, const struct kevent *changelist, int nchanges,
           struct kevent *eventlist, int nevents,
           const struct timespec *timeout)
{
    /* 1. Register phase */
    if (nchanges > 0 && changelist != NULL) {
        int ret = kevent_register(kq, (struct kevent *)changelist, nchanges);
        if (ret < 0)
            return -1;  /* errno already set */
    }

    /* 2. Wait phase */
    if (nevents <= 0 || eventlist == NULL)
        return 0;

    int timeout_ms;
    if (timeout == NULL) {
        timeout_ms = -1;  /* block indefinitely */
    } else {
        /* Convert timespec → ms, clamping to INT_MAX */
        long long ms = (long long)timeout->tv_sec * 1000
                     + (timeout->tv_nsec + 999999) / 1000000;
        if (ms > INT_MAX)
            ms = INT_MAX;
        timeout_ms = (int)ms;
    }

    return kevent_wait(kq, eventlist, nevents, timeout_ms);
}
