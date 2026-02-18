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

// Second virtio disk: major=2, minor=2
#ifndef DISK1_MAJOR
#define DISK1_MAJOR 2
#endif
#ifndef DISK1_MINOR
#define DISK1_MINOR 2
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

    // Mount ext4 filesystem (disk1) at /usr for Python standard library
    mkdir("/usr");
    mknod("/dev/disk1", S_IFBLK | 0600, DISK1_MAJOR, DISK1_MINOR);
    if (mount("/dev/disk1", "/usr", "ext4") < 0) {
        printf("init: mount ext4 at /usr failed\n");
    } else {
        printf("init: ext4 mounted at /usr\n");
    }

    // Launch userspace telnet daemon in the background.
    // Sleep briefly to let lwIP finish initialisation before utelnetd
    // tries to create sockets.
    sleep(10);
    pid = fork();
    if (pid == 0) {
        char *telnetd_argv[] = {"utelnetd", 0};
        exec("/bin/utelnetd", telnetd_argv);
        printf("init: exec utelnetd failed\n");
        exit(1);
    } else if (pid < 0) {
        printf("init: fork for utelnetd failed\n");
    } else {
        printf("init: utelnetd started (pid %d)\n", pid);
    }

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
