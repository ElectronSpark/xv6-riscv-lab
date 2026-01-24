#ifndef __XV6_USER_DEFINES_H
#define __XV6_USER_DEFINES_H

struct stat;

typedef uint64 sigset_t;

#include "kernel/inc/signal_types.h"
#include "kernel/inc/memstat.h"

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int, int);
int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, int mode, int major, int minor);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int symlink(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
int sleep(int);
int uptime(void);
// int sigalarm(int ticks, void (*handler)());
int sigaction(int signum, struct sigaction *act, struct sigaction *oldact);
int sigreturn(void);
int sigpending(sigset_t *set);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
void pause(void);

uint64 memstat(uint64 flags);
int dumpproc(void);
int dumpchan(void);
int dumppcache(void);
int dumprq(void);
uint64 kernbase(void);

// New VFS syscalls
int getdents(int fd, void *dirp, int count);
int chroot(const char *path);
int mount(const char *source, const char *target, const char *fstype);
int umount(const char *target);
char *getcwd(char *buf, int size);

void sync(void);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void fprintf(int, const char*, ...) __attribute__ ((format (printf, 2, 3)));
void printf(const char*, ...) __attribute__ ((format (printf, 1, 2)));
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);

// umalloc.c
void* malloc(uint);
void free(void*);

#endif              /* __XV6_USER_DEFINES_H */
