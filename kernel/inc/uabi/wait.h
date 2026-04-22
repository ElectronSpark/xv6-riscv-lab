#ifndef __USER_ABI_WAIT_H
#define __USER_ABI_WAIT_H

/* waitpid() option flags */
#define WNOHANG 1
#define WUNTRACED 2

/* wait status macros (POSIX-style encoding) */
#define WIFEXITED(w)    (((w) & 0xff) == 0)
#define WIFSTOPPED(w)   (((w) & 0xff) == 0x7f)
#define WIFSIGNALED(w)  (((w) & 0x7f) > 0 && ((w) & 0x7f) < 0x7f)
#define WEXITSTATUS(w)  (((w) >> 8) & 0xff)
#define WTERMSIG(w)     ((w) & 0x7f)
#define WSTOPSIG(w)     (((w) >> 8) & 0xff)

#endif /* __USER_ABI_WAIT_H */
