#include "errno.h"
#include "fcntl.h"
#include "signal.h"
#include "stddef.h"
#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "sys/time.h"
#include "sys/types.h"
#include "time.h"
#include "unistd.h"

#include <stdarg.h>

extern int errno;

typedef void (*sighandler_t)(int);

// Placeholder implementations for APIs that musl will expect but xv6 does not
// currently provide. These should be replaced with real implementations during
// the porting effort. For now they are minimal stubs to keep the linker happy
// and return errors that surface feature gaps.

int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
  (void)clk_id;
  (void)tp;
  errno = ENOSYS;
  return -1;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
  (void)req;
  if (rem)
    memset(rem, 0, sizeof(*rem));
  errno = ENOSYS;
  return -1;
}

int gettimeofday(struct timeval *tv, void *tz)
{
  (void)tz;
  if (tv) {
    tv->tv_sec = 0;
    tv->tv_usec = 0;
  }
  errno = ENOSYS;
  return -1;
}

int raise(int sig)
{
  return kill(getpid(), sig);
}

sighandler_t signal(int sig, sighandler_t func)
{
  struct sigaction act = {
    .sa_handler = func,
    .sa_mask = 0,
    .sa_flags = 0,
  };
  struct sigaction old = {0};
  if (sigaction(sig, &act, &old) < 0)
    return SIG_ERR;
  return old.sa_handler;
}

int atexit(void (*func)(void))
{
  (void)func;
  errno = ENOSYS;
  return -1;
}

void abort(void)
{
  kill(getpid(), SIGABRT);
  for (;;)
    ;
}

void _exit(int status)
{
  exit(status);
  for (;;)
    ;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
  if ((uintptr_t)memptr == 0)
    return EINVAL;
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment < sizeof(void *))
    return EINVAL;
  void *ptr = malloc(size);
  if (!ptr) {
    *memptr = 0;
    return ENOMEM;
  }
  *memptr = ptr;
  return 0;
}

int puts(const char *s)
{
  if ((uintptr_t)s == 0)
    s = "(null)";
  size_t len = strlen(s);
  if (write(1, s, len) != (ssize_t)len) {
    errno = EIO;
    return EOF;
  }
  if (write(1, "\n", 1) != 1) {
    errno = EIO;
    return EOF;
  }
  return 0;
}

static int
return_enosys(void)
{
  errno = ENOSYS;
  return -1;
}

off_t lseek(int fd, off_t offset, int whence)
{
  (void)fd;
  (void)offset;
  (void)whence;
  errno = ENOSYS;
  return (off_t)-1;
}

int fcntl(int fd, int cmd, ...)
{
  (void)fd;
  (void)cmd;
  return return_enosys();
}

int ioctl(int fd, unsigned long request, ...)
{
  (void)fd;
  (void)request;
  return return_enosys();
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
  (void)fd;
  (void)buf;
  (void)count;
  (void)offset;
  errno = ENOSYS;
  return -1;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
  (void)fd;
  (void)buf;
  (void)count;
  (void)offset;
  errno = ENOSYS;
  return -1;
}

int fsync(int fd)
{
  (void)fd;
  return return_enosys();
}

int fdatasync(int fd)
{
  (void)fd;
  return return_enosys();
}

int ftruncate(int fd, off_t length)
{
  (void)fd;
  (void)length;
  return return_enosys();
}

int truncate(const char *path, off_t length)
{
  (void)path;
  (void)length;
  return return_enosys();
}

int access(const char *path, int mode)
{
  (void)path;
  (void)mode;
  return return_enosys();
}

int pipe2(int pipefd[2], int flags)
{
  (void)pipefd;
  (void)flags;
  return return_enosys();
}

int dup3(int oldfd, int newfd, int flags)
{
  (void)oldfd;
  (void)newfd;
  (void)flags;
  return return_enosys();
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
  (void)path;
  (void)buf;
  (void)bufsiz;
  errno = ENOSYS;
  return -1;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
{
  (void)dirfd;
  (void)path;
  (void)buf;
  (void)bufsiz;
  errno = ENOSYS;
  return -1;
}

int unlinkat(int dirfd, const char *path, int flags)
{
  (void)dirfd;
  (void)path;
  (void)flags;
  return return_enosys();
}

int symlinkat(const char *target, int newdirfd, const char *linkpath)
{
  (void)target;
  (void)newdirfd;
  (void)linkpath;
  return return_enosys();
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
  (void)olddirfd;
  (void)oldpath;
  (void)newdirfd;
  (void)newpath;
  (void)flags;
  return return_enosys();
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
  (void)dirfd;
  (void)path;
  (void)mode;
  return return_enosys();
}

int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
  (void)dirfd;
  (void)path;
  (void)buf;
  (void)flags;
  return return_enosys();
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
  (void)dirfd;
  (void)path;
  (void)mode;
  (void)dev;
  return return_enosys();
}

int futimens(int fd, const struct timespec times[2])
{
  (void)fd;
  (void)times;
  return return_enosys();
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
  (void)dirfd;
  (void)path;
  (void)times;
  (void)flags;
  return return_enosys();
}

int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
  (void)fd;
  (void)offset;
  (void)len;
  (void)advice;
  return return_enosys();
}

int posix_fallocate(int fd, off_t offset, off_t len)
{
  (void)fd;
  (void)offset;
  (void)len;
  return return_enosys();
}

int chmod(const char *path, mode_t mode)
{
  (void)path;
  (void)mode;
  return return_enosys();
}

int fchmod(int fd, mode_t mode)
{
  (void)fd;
  (void)mode;
  return return_enosys();
}

int chown(const char *path, uid_t owner, gid_t group)
{
  (void)path;
  (void)owner;
  (void)group;
  return return_enosys();
}

int fchown(int fd, uid_t owner, gid_t group)
{
  (void)fd;
  (void)owner;
  (void)group;
  return return_enosys();
}

int lchown(const char *path, uid_t owner, gid_t group)
{
  (void)path;
  (void)owner;
  (void)group;
  return return_enosys();
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
  (void)olddirfd;
  (void)oldpath;
  (void)newdirfd;
  (void)newpath;
  return return_enosys();
}
