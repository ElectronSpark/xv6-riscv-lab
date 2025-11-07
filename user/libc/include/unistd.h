#ifndef _XV6_UNISTD_H
#define _XV6_UNISTD_H

#include "stddef.h"
#include "stdint.h"
#include "sys/stat.h"
#include "sys/types.h"
#include "signal.h"

#ifdef __cplusplus
extern "C" {
#endif

int fork(void);
void _exit(int) __attribute__((noreturn));
int wait(int *status);
int pipe(int fds[2]);
ssize_t read(int fd, void *buf, size_t nbytes);
ssize_t write(int fd, const void *buf, size_t nbytes);
int close(int fd);
int kill(pid_t pid, int sig);
int exec(const char *path, char *const argv[]);
int open(const char *path, int flags);
int mknod(const char *path, short major, short minor);
int unlink(const char *path);
int fstat(int fd, struct stat *st);
int link(const char *oldpath, const char *newpath);
int symlink(const char *target, const char *linkpath);
int mkdir(const char *path);
int chdir(const char *path);
int dup(int fd);
pid_t getpid(void);
char* sbrk(intptr_t increment);
unsigned int sleep(unsigned int ticks);
unsigned int uptime(void);
int sigaction(int signum, struct sigaction *act, struct sigaction *oldact);
int sigalarm(int ticks, void (*handler)(int));
int sigreturn(void);
int sigpending(sigset_t *set);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
void pause(void);
int memstat(void);
int dumpproc(void);
int dumpchan(void);

#ifdef __cplusplus
}
#endif

#endif /* _XV6_UNISTD_H */
