#ifndef __SIGNO_H
#define __SIGNO_H

#define SIGNONE 0  // No signal

/* The signal numbers and actions are defined according to the Linux MAN pages.
   Here it follows the x86/ARM signal numbering scheme.

   SIGKILL and SIGSTOP cannot be caught, blocked, or ignored.
*/
#define SIGHUP           1
#define SIGINT           2
#define SIGQUIT          3
#define SIGILL           4
#define SIGTRAP          5
#define SIGABRT          6
#define SIGIOT           6
#define SIGBUS           7
// #define SIGEMT           -
#define SIGFPE           8
#define SIGKILL          9
#define SIGUSR1         10
#define SIGSEGV         11
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGTERM         15
#define SIGSTKFLT       16
#define SIGCHLD         17
// #define SIGCLD           -
#define SIGCONT         18
#define SIGSTOP         19
#define SIGTSTP         20
#define SIGTTIN         21
#define SIGTTOU         22
#define SIGURG          23
#define SIGXCPU         24
#define SIGXFSZ         25
#define SIGVTALRM       26
#define SIGPROF         27
#define SIGWINCH        28
#define SIGIO           29
#define SIGPOLL         SIGIO
#define SIGPWR          30
// #define SIGINFO          -
// #define SIGLOST          -
#define SIGSYS          31
// #define SIGUNUSED       31

// Maximum number of signals
#define NSIG 32

#define SIGNO_MASK(__SIG_NUMBER)    \
(((__SIG_NUMBER) <= 0 || (__SIG_NUMBER) > NSIG) ? 0 : ((1UL << (__SIG_NUMBER)) >> 1))

#define SIG_ERR      ((void (*)(void))-1) // Error return from signal handler
#define SIG_DFL      ((void (*)(void))0)  // Default signal handler
#define SIG_IGN      ((void (*)(void))1)  // Ignore signal


#endif /* __SIGNO_H */
