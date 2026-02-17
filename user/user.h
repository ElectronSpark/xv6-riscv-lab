#ifndef __XV6_USER_DEFINES_H
#define __XV6_USER_DEFINES_H

struct stat;

#include "uabi/signal.h"
#include "uabi/memstat.h"
#include "uabi/clone_flags.h"
#include "uabi/time.h"
#include "uabi/utsname.h"
#include "uabi/wait.h"
#include "uabi/mman.h"
#include "uabi/access.h"

// system calls
int clone(struct clone_args *);
int exit(int) __attribute__((noreturn));
int wait(int *);
int pipe(int *);
int write(int, const void *, int);
int read(int, void *, int);
int close(int);
int kill(int, int);
int exec(const char *, char **);
int open(const char *, int);
int mknod(const char *, int mode, int major, int minor);
int unlink(const char *);
int fstat(int fd, struct stat *);
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int readlink(const char *path, char *buf, int bufsiz);
int link(const char *, const char *);
int symlink(const char *, const char *);
int mkdir(const char *);
int chdir(const char *);
int dup(int);
int dup2(int oldfd, int newfd);
int lseek(int fd, int64 offset, int whence);
int fcntl(int fd, int cmd, int arg);
int access(const char *path, int mode);
int rename(const char *oldpath, const char *newpath);
int ftruncate(int fd, int64 length);
int getpid(void);
int getppid(void);
int gettid(void);
int getrandom(void *buf, int len);
int tgkill(int tgid, int tid, int sig);
int tkill(int tid, int sig);
void exit_group(int) __attribute__((noreturn));
char *sbrk(int64);
int sleep(int);
int nanosleep(const struct timespec *req, struct timespec *rem);
int uptime(void);
int gettimeofday(struct timeval *tv, void *tz);
int waitpid(int pid, int *status, int options);
int uname(struct utsname *buf);
// int sigalarm(int ticks, void (*handler)());
int sigaction(int signum, struct sigaction *act, struct sigaction *oldact);
int sigreturn(void);
int sigpending(sigset_t *set);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigsuspend(const sigset_t *mask);
int sigwait(const sigset_t *set, int *sig);
void pause(void);

// Memory mapping
void *mmap(void *addr, int length, int prot, int flags, int fd, int offset);
int munmap(void *addr, int length);
int mprotect(void *addr, int length, int prot);
void *mremap(void *old_addr, int old_size, int new_size, int flags, void *new_addr);
int msync(void *addr, int length, int flags);
int mincore(void *addr, int length, unsigned char *vec);
int madvise(void *addr, int length, int advice);

// ulib wrapper functions
int fork(void);

// syscall (pure asm stub — must not be a C wrapper due to shared user stack)
int vfork(void);

uint64 memstat(uint64 flags);
int dumpproc(int mode, int id);
int dumpchan(void);
int dumppcache(void);
int dumprq(void);
int dumpinode(const char *path);
uint64 kernbase(void);

// New VFS syscalls
int getdents(int fd, void *dirp, int count);
int chroot(const char *path);
int mount(const char *source, const char *target, const char *fstype);
int umount(const char *target);
char *getcwd(char *buf, int size);

// Terminal I/O
int ioctl(int fd, int cmd, void *arg);
int tcgetattr(int fd, void *termios_p);
int tcsetattr(int fd, int optional_actions, const void *termios_p);

// Process group / session
int setpgid(int pid, int pgid);
int getpgid(int pid);
int setsid(void);
int getsid(int pid);

void sync(void);
int statfs(const char *path, void *buf);

// ulib.c
char *strcpy(char *, const char *);
void *memmove(void *, const void *, int);
char *strchr(const char *, char c);
int strcmp(const char *, const char *);
void fprintf(int, const char *, ...) __attribute__((format(printf, 2, 3)));
void printf(const char *, ...) __attribute__((format(printf, 1, 2)));
void vprintf(int, const char *, __builtin_va_list);
int snprintf(char *, size_t, const char *, ...) __attribute__((format(printf, 3, 4)));
int sprintf(char *, const char *, ...) __attribute__((format(printf, 2, 3)));
int vsnprintf(char *, size_t, const char *, __builtin_va_list);
char *gets(char *, int max);
uint strlen(const char *);
void *memset(void *, int, uint);
int atoi(const char *);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);

// umalloc.c
void *malloc(uint);
void free(void *);

#endif /* __XV6_USER_DEFINES_H */
