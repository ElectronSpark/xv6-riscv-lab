// init: The initial user-level program

#include "uabi/stat.h"
#include "user/user.h"
#include "uabi/fcntl.h"

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

    // Open console as stdin (fd 0), stdout (fd 1), stderr (fd 2)
    if (open("/dev/console", O_RDWR) < 0) {
        mknod("/dev/console", S_IFCHR | 0666, CONSOLE_MAJOR, CONSOLE_MINOR);
        open("/dev/console", O_RDWR);
    }
    dup(0); // stdout (fd 1)
    dup(0); // stderr (fd 2)

    // Ensure device nodes exist (devtmpfs may have created them already,
    // so mknod errors are ignored)
    mknod("/dev/null", S_IFCHR | 0666, NULL_MAJOR, NULL_MINOR);
    mknod("/dev/random", S_IFCHR | 0666, RANDOM_MAJOR, RANDOM_MINOR);

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
