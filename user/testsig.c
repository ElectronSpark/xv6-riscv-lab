#include "kernel/types.h"
#include "kernel/signo.h"
#include "user/user.h"

void handler2(int signo) {
    printf("Signal handler 2 called with signo: %d\n", signo);
    sigreturn();
}

void handler1(int signo) {
  printf("Signal handler 1 called with signo: %d\n", signo);
  sigaction_t sa2 = {0};
  sa2.handler = handler2;
  if (sigaction(SIGUSR1, &sa2, NULL) != 0) {
      fprintf(2, "Failed to set signal handler for SIGUSR1\n");
      exit(-1);
  }
  kill(getpid(), SIGUSR1); // Send SIGUSR1 to self to trigger handler
  sleep(10); // Give time for the signal to be handled
  printf("Signal 2 returned\n");

  sigreturn();
}

int main(void) {
    sigaction_t sa1 = {0};
    sa1.handler = handler1;
    if (sigaction(SIGALRM, &sa1, NULL) != 0) {
        fprintf(2, "Failed to set signal handler\n");
        exit(-1);
    }
    kill(getpid(), SIGALRM);
    sleep(10); // Give time for the signal to be handled

    printf("signal returned\n");

    return 0;
}
