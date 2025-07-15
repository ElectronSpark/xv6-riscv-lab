#ifndef __SIGNO_H
#define __SIGNO_H

#define SIGNONE 0  // No signal
// Signal Numer starts from 1
#define SIGALARM 1
#define SIGKILL 9
#define SIGTERM 15
// #define SIGSTOP 17
// #define SIGCONT 19
// #define SIGCHLD 20

// Maximum number of signals
#define NSIG 64

#define SIGNO_MASK(__SIG_NUMBER)    \
    (((__SIG_NUMBER) <= 0 || (__SIG_NUMBER) > NSIG) ? 0 : ((1UL << (__SIG_NUMBER)) >> 1))

#define SIG_DFL ((void (*)(int))0)  // Default signal handler
#define SIG_IGN ((void (*)(int))1)  // Ignore signal

#endif /* __SIGNO_H */
