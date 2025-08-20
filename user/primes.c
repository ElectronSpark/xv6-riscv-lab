#include "kernel/inc/types.h"
#include "kernel/inc/stat.h"
#include "user/user.h"

void primes(int pip) __attribute__((noreturn));

void
primes(int pip)
{
    int msg_pip[2] = { -1, -1 };
    int pid = -1;
    int cur_num = 0;
    int rcv = 0;

    if (read(pip, &cur_num, sizeof(int)) < sizeof(int)) {
        printf("read pip error\n");
        exit(1);
    }

    if (cur_num >= 280) {
        close(pip);
        exit(0);
    }
    

    printf("prime %d\n", cur_num);

    if (pipe(msg_pip) < 0) {
        printf("creating pip error\n");
        close(pip);
        exit(1);
    }

    if ((pid = fork()) == 0) {
        close(msg_pip[1]);
        primes(msg_pip[0]);
    } else if (pid < 0) {
        printf("forking error\n");
        close(pip);
        close(msg_pip[0]);
        close(msg_pip[1]);
        exit(1);
    }

    close(msg_pip[0]);

    while (rcv < 280) {
        if (read(pip, &rcv, sizeof(int)) < sizeof(int)) {
            printf("read pip error\n");
            exit(1);
        }

        if (rcv % cur_num) {
            write(msg_pip[1], &rcv, sizeof(int));
        }
    }
    
    close(pip);
    write(msg_pip[1], &rcv, sizeof(int));
    close(msg_pip[1]);

    wait(0);

    exit(0);
} 

int
main(void)
{
    int msg_pip[2];
    int pid;

    if (pipe(msg_pip) < 0) {
        printf("creating pip error\n");
        exit(1);
    }

    if ((pid = fork()) == 0) {
        close(msg_pip[1]);
        primes(msg_pip[0]);
    } else if (pid > 0) {
        close(msg_pip[0]);
        for (int i = 2; i <= 280; i++) {
            write(msg_pip[1], &i, sizeof(i));
        }
        close(msg_pip[1]);
        wait(0);
    } else {
        printf("fork error\n");
        close(msg_pip[0]);
        close(msg_pip[1]);
        exit(1);
    }

    exit(0);
}