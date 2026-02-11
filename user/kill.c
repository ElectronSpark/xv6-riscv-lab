#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/signo.h"
#include "user/user.h"

int main(int argc, char **argv) {
    int i;
    int signo = SIGKILL;

    if (argc < 2) {
        fprintf(2, "usage: kill pid...\n");
        exit(1);
    }
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            signo = atoi(argv[i] + 1);
            if (signo < 0 || signo >= NSIG) {
                fprintf(2, "kill: bad signal %s\n", argv[i]);
                exit(1);
            }
        }
    }
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            continue; // Skip the signal option
        }
        kill(atoi(argv[i]), signo);
    }
    exit(0);
}
