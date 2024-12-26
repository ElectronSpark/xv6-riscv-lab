#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
    int msg_pipe[2];
    int pid, retpid;
    char buf[16];


    if (pipe(msg_pipe) < 0) {
        printf("pipe creating error\n");
        exit(1);
    }

    if ((retpid = fork()) > 0) {
        pid = getpid();
        write(msg_pipe[0], " ", 1);
        read(msg_pipe[1], buf, 1);
        printf("%d: received pong\n", pid);
        close(msg_pipe[1]);
        wait(0);
    } else if (retpid < 0) {
        close(msg_pipe[0]);
        close(msg_pipe[1]);
        printf("fork error\n");
        exit(1);
    } else {
        pid = getpid();
        read(msg_pipe[1], buf, 1);
        close(msg_pipe[0]);
        printf("%d: received ping\n", pid);
        write(msg_pipe[0], " ", 1);
    }

    exit(0);
}