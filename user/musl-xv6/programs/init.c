/*
 * init.c — musl libc version for xv6
 *
 * The initial user-level program. Opens console, creates device nodes,
 * mounts filesystems, and spawns the shell in a loop.
 *
 * Uses standard POSIX headers. Device major/minor numbers are
 * xv6-specific constants (not kernel headers).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

/*
 * xv6 device major/minor numbers — these are xv6-specific constants.
 * Copied here to avoid referencing kernel headers.
 */
#define CONSOLE_MAJOR   1
#define CONSOLE_MINOR   1
#define NULL_MAJOR      1
#define NULL_MINOR      2
#define RANDOM_MAJOR    1
#define RANDOM_MINOR    3
#define TTY_DEV_MAJOR   5
#define TTY_DEV_MINOR   1
#define DISK1_MAJOR     2
#define DISK1_MINOR     2

/* xv6 ioctl for setting controlling terminal */
#ifndef TIOCSCTTY
#define TIOCSCTTY       0x540E
#endif

/* xv6 uses mkdev(major, minor) = ((major) << 16) | (minor) */
#ifndef makedev
#define makedev(maj, min)  (((unsigned int)(maj) << 16) | (unsigned int)(min))
#endif

/*
 * xv6-specific syscalls not in POSIX: mount, ioctl
 * musl provides ioctl(). For mount(), we use a raw syscall.
 */

/* Forward declaration — mount is defined via syscall since musl's
 * mount() expects Linux-style args. We use a thin inline wrapper. */
static long xv6_mount(const char *source, const char *target, const char *fstype)
{
    /* xv6 SYS_mount = 34, args: a0=source, a1=target, a2=fstype */
    return syscall(34, source, target, fstype);
}

static char *argv[] = { "sh", NULL };

int main(void)
{
    int pid, wpid;

    /* Open console as stdin (fd 0), stdout (fd 1), stderr (fd 2) */
    if (open("/dev/console", O_RDWR) < 0) {
        mknod("/dev/console", S_IFCHR | 0666, makedev(CONSOLE_MAJOR, CONSOLE_MINOR));
        open("/dev/console", O_RDWR);
    }
    dup(0);  /* stdout (fd 1) */
    dup(0);  /* stderr (fd 2) */

    /* Set the console as the controlling terminal */
    if (ioctl(0, TIOCSCTTY, (void *)0) < 0)
        printf("init: warning: TIOCSCTTY failed\n");

    /* Ensure device nodes exist */
    mknod("/dev/null",   S_IFCHR | 0666, makedev(NULL_MAJOR, NULL_MINOR));
    mknod("/dev/random", S_IFCHR | 0666, makedev(RANDOM_MAJOR, RANDOM_MINOR));
    mknod("/dev/tty",    S_IFCHR | 0666, makedev(TTY_DEV_MAJOR, TTY_DEV_MINOR));

    /* The ext4 rootfs already contains /usr with Python stdlib.
     * No separate disk mount needed. */

    for (;;) {
        printf("init: starting sh\n");
        pid = fork();
        if (pid < 0) {
            printf("init: fork failed\n");
            _exit(1);
        }
        if (pid == 0) {
            execv("/bin/sh", argv);
            printf("init: exec sh failed\n");
            _exit(1);
        }

        for (;;) {
            wpid = wait(NULL);
            if (wpid == pid) {
                break;
            } else if (wpid < 0) {
                printf("init: wait returned an error\n");
                _exit(1);
            }
            /* else: reaped a parentless process, continue */
        }
    }
}
