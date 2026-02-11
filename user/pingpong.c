/*
 * pingpong.c - Pipe communication example
 *
 * Demonstrates correct pipe usage in xv6:
 *   - pipe(fd) creates fd[0]=read end, fd[1]=write end
 *   - For bidirectional communication, use TWO pipes:
 *     p2c[2] for parent-to-child, c2p[2] for child-to-parent
 *   - Each process closes the ends it doesn't use
 *   - Parent writes to p2c[1], child reads from p2c[0]
 *   - Child writes to c2p[1], parent reads from c2p[0]
 */
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

int main(void) {
    int p2c[2]; // parent to child pipe
    int c2p[2]; // child to parent pipe
    int pid;
    char buf[16];

    // Create both pipes
    if (pipe(p2c) < 0 || pipe(c2p) < 0) {
        printf("pipe creating error\n");
        exit(1);
    }

    pid = fork();
    if (pid < 0) {
        printf("fork error\n");
        exit(1);
    } else if (pid == 0) {
        // Child process
        close(p2c[1]); // Close write end of parent-to-child
        close(c2p[0]); // Close read end of child-to-parent

        read(p2c[0], buf, 1); // Read ping from parent
        printf("%d: received ping\n", getpid());
        write(c2p[1], " ", 1); // Send pong to parent

        close(p2c[0]);
        close(c2p[1]);
        exit(0);
    } else {
        // Parent process
        close(p2c[0]); // Close read end of parent-to-child
        close(c2p[1]); // Close write end of child-to-parent

        write(p2c[1], " ", 1); // Send ping to child
        read(c2p[0], buf, 1);  // Read pong from child
        printf("%d: received pong\n", getpid());

        close(p2c[1]);
        close(c2p[0]);
        wait(0);
    }

    exit(0);
}