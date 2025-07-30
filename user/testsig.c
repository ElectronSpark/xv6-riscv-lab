#include "kernel/types.h"
#include "kernel/signo.h"
#include "user/user.h"

void handler2(int signo) {
    printf("Signal handler 2 called with signo: %d\n", signo);
    sigreturn();
}

void handler1(int signo) {
  printf("Signal handler 1 called with signo: %d\n", signo);
  kill(getpid(), SIGUSR1); // Send SIGUSR1 to self to trigger handler
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
    } else {
        printf("No siginfo provided\n");
    }

    kill(getpid(), SIGUSR1); // Send SIGUSR1 to self to trigger handler
    printf("Signal 2 returned\n");

    sigreturn();
}

int main(void) {
    int pid = getpid();
    printf("Test signal handling in process %d\n", pid);
    int pid1 = fork();
    if (pid1 < 0) {
        fprintf(2, "Fork failed\n");
        exit(-1);
    } else if (pid1 == 0) {
        // Child process
        printf("Child process %d started\n", getpid());
        sleep(5);
        printf("Child process sending SIGALRM to parent\n");
        kill(pid, SIGALRM); // Send SIGALRM to parent
        sleep(5);
        printf("Child process sending SIGALRM to parent\n");
        kill(pid, SIGALRM); // Send SIGALRM to parent

        printf("Child process paused\n");
        sleep(5);
        printf("Child process sending SIGALRM to parent\n");
        kill(pid, SIGALRM); // Send SIGALRM to parent
        sleep(5);
        printf("Child process sending SIGALRM to parent\n");
        kill(pid, SIGALRM); // Send SIGALRM to parent
        printf("Child process exiting\n");

        printf("Child process %d exiting\n", getpid());
        sleep(5);
        printf("Child process exiting\n");
        exit(0); // Child exits
    }

    printf("Parent process %d started\n", pid);
    sigaction_t sa1 = {0};
    sa1.sa_handler = handler1;
    if (sigaction(SIGALRM, &sa1, NULL) != 0) {
        fprintf(2, "Failed to set signal handler\n");
        exit(-1);
    }
    sigaction_t sa2 = {0};
    sa2.sa_handler = handler2;
    if (sigaction(SIGUSR1, &sa2, NULL) != 0) {
        fprintf(2, "Failed to set signal handler for SIGUSR1\n");
        exit(-1);
    }
    pause(); // Wait for the signal to be handled
    printf("signal returned for the first time\n");
    pause();
    printf("signal returned for the second time\n");
    
    printf("Test siginfo\n");
    sa1.sa_sigaction = handler3;
    sa1.sa_flags = SA_SIGINFO; // Use sa_sigaction instead of sa_handler
    if (sigaction(SIGALRM, &sa1, NULL) != 0) {
        fprintf(2, "Failed to set signal handler\n");
        exit(-1);
    }

    kill(pid1, SIGALRM);
    pause(); // Wait for the signal to be handled
    printf("signal returned for the first time\n");
    pause(); // Wait for the signal to be handled
    printf("signal returned for the second time\n");

    sleep(2);
    kill(pid1, SIGSTOP); // Terminate child process

    return 0;
}
