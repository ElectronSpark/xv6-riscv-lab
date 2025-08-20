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

#define SIG_ERR      ((void (*)(int))-1) // Error return from signal handler
#define SIG_DFL      ((void (*)(int))0)  // Default signal handler
#define SIG_IGN      ((void (*)(int))1)  // Ignore signal


#define SA_NOCLDSTOP 0x00000001 // Don't receive SIGCHLD when children stop.
#define SA_NOCLDWAIT 0x00000002 // Don't create zombie processes on child exit.
#define SA_SIGINFO   0x00000004 // Use sa_sigaction instead of sa_handler.
#define SA_ONSTACK   0x00000008 // Use alternate signal stack.
// #define SA_RESTART   0x00000010 // Restart system calls if interrupted by handler
#define SA_NODEFER   0x00000020 // Don't block the signal in the handler.
#define SA_RESETHAND 0x00000040 // Reset the signal handler to SIG_DFL after the first delivery.


#endif /* __SIGNO_H */
