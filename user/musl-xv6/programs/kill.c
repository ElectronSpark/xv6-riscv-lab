/*
 * kill.c — musl libc version for xv6
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

int main(int argc, char **argv)
{
    int signo = SIGKILL;

    if (argc < 2) {
        fprintf(stderr, "usage: kill [-signal] pid...\n");
        exit(1);
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            signo = atoi(argv[i] + 1);
            if (signo < 0 || signo >= _NSIG) {
                fprintf(stderr, "kill: bad signal %s\n", argv[i]);
                exit(1);
            }
        }
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-')
            continue;
        kill(atoi(argv[i]), signo);
    }
    return 0;
}
