#include "kernel/types.h"
#include "kernel/signo.h"
#include "user/user.h"

void handler1(int signo) {
  printf("Signal handler 1 called with signo: %d\n", signo);
  sigreturn();
}

int main(void) {
    sigaction_t sa1 = {0};
    sa1.handler = handler1;
    if (sigaction(SIGALARM, &sa1, NULL) != 0) {
        fprintf(2, "Failed to set signal handler\n");
        exit(-1);
    }
    kill(getpid(), SIGALARM);
    sleep(5); // Give time for the signal to be handled
    sleep(1); // Give time for the signal to be handled

    printf("signal returned\n");

    return 0;
}
