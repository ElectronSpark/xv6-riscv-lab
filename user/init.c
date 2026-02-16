// init: The initial user-level program

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/lock/spinlock.h"
#include "kernel/inc/lock/mutex_types.h"
#include "kernel/inc/vfs/xv6fs/ondisk.h"
#include "user/user.h"
#include "kernel/inc/vfs/fcntl.h"

#ifndef CONSOLE_MAJOR
#define CONSOLE_MAJOR 1
#endif
#ifndef CONSOLE_MINOR
#define CONSOLE_MINOR 1
#endif
#ifndef NULL_MAJOR
#define NULL_MAJOR 1
#endif
#ifndef NULL_MINOR
#define NULL_MINOR 2
#endif
#ifndef RANDOM_MAJOR
#define RANDOM_MAJOR 1
#endif
#ifndef RANDOM_MINOR
#define RANDOM_MINOR 3
#endif

char *argv[] = {"sh", 0};

int main(void) {
    int pid, wpid;

    if (open("/dev/console", O_RDWR) < 0) {
        mknod("/dev/console", S_IFCHR | 0666, CONSOLE_MAJOR, CONSOLE_MINOR);
        open("/dev/console", O_RDWR);
    }
    if (open("/dev/null", O_RDWR) < 0) {
        mknod("/dev/null", S_IFCHR | 0666, NULL_MAJOR, NULL_MINOR);
    }
    if (open("/dev/random", O_RDWR) < 0) {
        mknod("/dev/random", S_IFCHR | 0666, RANDOM_MAJOR, RANDOM_MINOR);
    }
    dup(0); // stdout
    dup(0); // stderr

    for (;;) {
        printf("init: starting sh\n");
        pid = fork(); // Use fork instead of vfork to debug OrangePi hang
        if (pid < 0) {
            printf("init: fork failed\n");
            exit(1);
        }
        if (pid == 0) {
            exec("/bin/sh", argv);
            printf("init: exec sh failed\n");
            exit(1);
        }

        for (;;) {
            // this call to wait() returns if the shell exits,
            // or if a parentless process exits.
            wpid = wait((int *)0);
            if (wpid == pid) {
                // the shell exited; restart it.
                break;
            } else if (wpid < 0) {
                printf("init: wait returned an error\n");
                exit(1);
            } else {
                // it was a parentless process; do nothing.
            }
        }
    }
}
