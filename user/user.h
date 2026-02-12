#ifndef __XV6_USER_DEFINES_H
#define __XV6_USER_DEFINES_H

struct stat;

#include "kernel/inc/types.h"
#include "kernel/inc/signal_types.h"
#include "kernel/inc/mm/memstat.h"
#include "kernel/inc/clone_flags.h"

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
int link(const char *, const char *);
int symlink(const char *, const char *);
int mkdir(const char *);
int chdir(const char *);
int dup(int);
int getpid(void);
int gettid(void);
int tgkill(int tgid, int tid, int sig);
void exit_group(int) __attribute__((noreturn));
char *sbrk(int64);
int sleep(int);
int uptime(void);
// int sigalarm(int ticks, void (*handler)());
int sigaction(int signum, struct sigaction *act, struct sigaction *oldact);
int sigreturn(void);
int sigpending(sigset_t *set);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
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
int dumpproc(void);
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

void sync(void);

// ulib.c
int stat(const char *, struct stat *);
char *strcpy(char *, const char *);
void *memmove(void *, const void *, int);
char *strchr(const char *, char c);
int strcmp(const char *, const char *);
void fprintf(int, const char *, ...) __attribute__((format(printf, 2, 3)));
void printf(const char *, ...) __attribute__((format(printf, 1, 2)));
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
