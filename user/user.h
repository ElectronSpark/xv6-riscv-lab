#ifndef __XV6_USER_DEFINES_H
#define __XV6_USER_DEFINES_H

struct stat;

#include "kernel/inc/types.h"
#include "kernel/inc/signal_types.h"
#include "kernel/inc/mm/memstat.h"
#include "kernel/inc/clone_flags.h"

struct timeval {
	int64 tv_sec;
	int64 tv_usec;
};

struct timespec {
	int64 tv_sec;
	int64 tv_nsec;
};

struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
};

#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef WUNTRACED
#define WUNTRACED 2
#endif

// Wait status macros (POSIX)
#define WIFEXITED(w)    (((w) & 0xff) == 0)
#define WIFSTOPPED(w)   (((w) & 0xff) == 0x7f)
#define WIFSIGNALED(w)  (((w) & 0x7f) > 0 && ((w) & 0x7f) < 0x7f)
#define WEXITSTATUS(w)  (((w) >> 8) & 0xff)
#define WTERMSIG(w)     ((w) & 0x7f)
#define WSTOPSIG(w)     (((w) >> 8) & 0xff)

// mmap protection flags (POSIX)
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

// mmap mapping flags (POSIX)
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)(uint64)-1)

// mremap flags
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

// msync flags
#define MS_ASYNC 1
#define MS_SYNC 4
#define MS_INVALIDATE 2

// madvise advice
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8

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

// kqueue
struct kevent; // forward declaration
int kqueue(void);
int kevent_register(int kqfd, struct kevent *changelist, int nchanges);
int kevent_wait(int kqfd, struct kevent *eventlist, int nevents, int timeout_ms);

// gdb support — execute EBREAK to pause and wait for a debugger.
// The kernel prints the PID and connection instructions.
static inline void waitgdb(void) {
    asm volatile("ebreak");
}

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
