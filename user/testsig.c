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
  sa2.sa_handler = handler2;
  if (sigaction(SIGUSR1, &sa2, NULL) != 0) {
      fprintf(2, "Failed to set signal handler for SIGUSR1\n");
      exit(-1);
  }
  kill(getpid(), SIGUSR1); // Send SIGUSR1 to self to trigger handler
  sleep(5); // Give time for the signal to be handled
  printf("Signal 2 returned\n");

  sigreturn();
}

void handler3(int signo, siginfo_t *info, void *context) {
    printf("Signal handler 3 called with signo: %d\n", signo);
    if (info) {
        printf("Signal info: sival_int=%d, sival_ptr=%p\n", info->si_value.sival_int, info->si_value.sival_ptr);
        printf("Signal code: %d, pid: %d, addr: %p\n", info->si_code, info->si_pid, info->si_addr);
        printf("Signal status: %d\n", info->si_status);
        printf("Signal errno: %d\n", info->si_errno);
    }

    sigaction_t sa2 = {0};
    sa2.sa_handler = handler2;
    if (sigaction(SIGUSR1, &sa2, NULL) != 0) {
        fprintf(2, "Failed to set signal handler for SIGUSR1\n");
        exit(-1);
    }
    kill(getpid(), SIGUSR1); // Send SIGUSR1 to self to trigger handler
    sleep(5); // Give time for the signal to be handled
    printf("Signal 2 returned\n");

    sigreturn();
}

int main(void) {
    sigaction_t sa1 = {0};
    sa1.sa_handler = handler1;
    if (sigaction(SIGALRM, &sa1, NULL) != 0) {
        fprintf(2, "Failed to set signal handler\n");
        exit(-1);
    }
    kill(getpid(), SIGALRM);
    sleep(10); // Give time for the signal to be handled

    printf("signal returned for the first time\n");

    kill(getpid(), SIGALRM);
    sleep(10); // Give time for the signal to be handled

    printf("signal returned for the second time\n");
    
    printf("Test siginfo\n");
    sa1.sa_sigaction = handler3;
    sa1.sa_flags = SA_SIGINFO; // Use sa_sigaction instead of sa_handler
    if (sigaction(SIGALRM, &sa1, NULL) != 0) {
        fprintf(2, "Failed to set signal handler\n");
        exit(-1);
    }

    kill(getpid(), SIGALRM);
    sleep(10); // Give time for the signal to be handled

    printf("signal returned for the first time\n");

    kill(getpid(), SIGALRM);
    sleep(10); // Give time for the signal to be handled

    printf("signal returned for the second time\n");

    return 0;
}
