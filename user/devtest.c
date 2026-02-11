//
// Test for device subsystem refcount correctness.
// Tests concurrent fork/exit with device file descriptors.
//

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define NR_THREAD 10
#define NITER 50

// Test 1: Many processes opening/closing console concurrently
void concurrent_open_close() {
    int pids[NR_THREAD];

    printf("devtest: concurrent_open_close... ");

    for (int i = 0; i < NR_THREAD; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            printf("fork failed\n");
            exit(-1);
        }
        if (pids[i] == 0) {
            // Child: repeatedly open and close console
            for (int j = 0; j < NITER; j++) {
                int fd = open("/dev/console", O_RDWR);
                if (fd < 0) {
                    // May fail if system is busy, that's ok
                    continue;
                }
                // Write something small
                write(fd, ".", 1);
                close(fd);
            }
            exit(0);
        }
    }

    // Parent: wait for all children
    for (int i = 0; i < NR_THREAD; i++) {
        int status;
        wait(&status);
        if (status != 0) {
            printf("child exited with status %d\n", status);
            exit(-1);
        }
    }

    printf("ok\n");
}

// Test 2: Fork with open device file descriptors
// This tests cdev_dup during fork
void fork_with_device_fd() {
    printf("devtest: fork_with_device_fd... ");

    for (int iter = 0; iter < NITER; iter++) {
        // Open console
        int fd = open("/dev/console", O_RDWR);
        if (fd < 0) {
            printf("open console failed\n");
            exit(-1);
        }

        int pid = fork();
        if (pid < 0) {
            printf("fork failed\n");
            exit(-1);
        }

        if (pid == 0) {
            // Child: use the inherited fd, then close and exit
            write(fd, "c", 1);
            close(fd);
            exit(0);
        }

        // Parent: also use the fd
        write(fd, "p", 1);

        // Wait for child
        int status;
        wait(&status);
        if (status != 0) {
            printf("child exited with status %d\n", status);
            exit(-1);
        }

        // Parent closes its fd
        close(fd);
    }

    printf("ok\n");
}

// Test 3: Many forks in parallel, all sharing device fd
// Tests concurrent cdev_dup and cdev_put
void parallel_fork_device() {
    printf("devtest: parallel_fork_device... ");

    // Open console once
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) {
        printf("open console failed\n");
        exit(-1);
    }

    int pids[NR_THREAD];

    // Fork many children that all inherit the fd
    for (int i = 0; i < NR_THREAD; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            printf("fork failed\n");
            exit(-1);
        }
        if (pids[i] == 0) {
            // Child: use fd and exit
            for (int j = 0; j < 10; j++) {
                write(fd, "x", 1);
            }
            close(fd);
            exit(0);
        }
    }

    // Parent waits
    for (int i = 0; i < NR_THREAD; i++) {
        int status;
        wait(&status);
        if (status != 0) {
            printf("child exited with status %d\n", status);
            exit(-1);
        }
    }

    close(fd);
    printf("ok\n");
}

// Test 4: Nested forks with device
// Each level forks, creating a tree
void nested_fork_device() {
    printf("devtest: nested_fork_device... ");

    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) {
        printf("open console failed\n");
        exit(-1);
    }

    int depth = 4;        // Creates chain of depth processes
    int my_depth = depth; // If we complete the loop, we're a leaf at max depth

    for (int d = 0; d < depth; d++) {
        int pid = fork();
        if (pid < 0) {
            printf("fork failed at depth %d\n", d);
            exit(-1);
        }
        if (pid == 0) {
            // Child continues forking
            continue;
        } else {
            // Parent: record our depth, wait for child, then break
            my_depth = d;
            int status;
            wait(&status);
            if (status != 0) {
                close(fd);
                exit(status);
            }
            break;
        }
    }

    // Leaf processes write "L" and exit
    if (my_depth == depth) {
        write(fd, "L", 1);
        close(fd);
        exit(0);
    }

    close(fd);

    // Only the original caller (my_depth == 0) prints "ok"
    if (my_depth == 0) {
        printf("ok\n");
    } else {
        // Intermediate parents just exit silently
        exit(0);
    }
}

// Test 5: Stress test - rapid fork/exit cycles
void stress_fork_exit() {
    printf("devtest: stress_fork_exit... ");

    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) {
        printf("open console failed\n");
        exit(-1);
    }

    for (int i = 0; i < NITER * 2; i++) {
        int pid = fork();
        if (pid < 0) {
            // May run out of procs, wait and retry
            wait(0);
            continue;
        }
        if (pid == 0) {
            // Child immediately exits
            close(fd);
            exit(0);
        }
    }

    // Collect all children
    while (wait(0) >= 0)
        ;

    close(fd);
    printf("ok\n");
}

int main(int argc, char *argv[]) {
    printf("devtest starting\n");

    concurrent_open_close();
    fork_with_device_fd();
    parallel_fork_device();
    nested_fork_device();
    stress_fork_exit();

    printf("\ndevtest: all tests passed!\n");
    exit(0);
}
