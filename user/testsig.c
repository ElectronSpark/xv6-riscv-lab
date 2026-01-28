#include "kernel/inc/types.h"
#include "kernel/inc/signo.h"
// Removed inclusion of kernel/signal.h to avoid prototype conflicts
#include "user/user.h"

#ifndef SIG_BLOCK
#define SIG_BLOCK   1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3
#endif

#ifndef SIGMASK
#define SIGMASK(signo) (1UL << ((signo) - 1))
#endif

// Global counters for validations
static volatile int siginfo_count = 0;            // For queue cap test (SIGALRM)
static volatile int rese_count = 0;               // For SA_RESETHAND test (SIGUSR1)
static volatile int nodefer_depth_max = 0;        // For SA_NODEFER recursion depth
static volatile int nodefer_current_depth = 0;
static volatile int cont_handler_count = 0;       // SIGCONT handler invocations
static volatile int change_handler_count = 0;     // Post-change handler delivery count (SIGALRM)
static int test_failures = 0;                     // Track test failures

/* ---------------- Basic Handlers ---------------- */
static void simple_handler(int signo) {
    printf("simple_handler signo=%d\n", signo);
    sigreturn();
}

static void rese_handler(int signo) {
    rese_count++;
    printf("SA_RESETHAND first delivery signo=%d rese_count=%d (will reset to default)\n", signo, rese_count);
    sigreturn();
}

static void nodefer_handler(int signo) {
    nodefer_current_depth++;
    if (nodefer_current_depth > nodefer_depth_max)
        nodefer_depth_max = nodefer_current_depth;
    printf("SA_NODEFER handler depth=%d signo=%d\n", nodefer_current_depth, signo);
    if (nodefer_current_depth == 1) {
        // Re-enter by raising again; with SA_NODEFER we should immediately recurse.
        kill(getpid(), signo);
    }
    nodefer_current_depth--;
    sigreturn();
}

static void siginfo_handler(int signo, siginfo_t *info, void *context) {
    siginfo_count++;
    printf("SIGINFO handler signo=%d count=%d ", signo, siginfo_count);
    if (info) {
        printf("[si_code=%d sival_int=%d pid=%d]", info->si_code, info->si_value.sival_int, info->si_pid);
    }
    printf("\n");
    sigreturn();
}

static void cont_handler(int signo) {
    cont_handler_count++;
    printf("SIGCONT handler invoked count=%d signo=%d\n", cont_handler_count, signo);
    sigreturn();
}

static void post_change_handler(int signo) {
    change_handler_count++;
    printf("Post-change handler delivered signo=%d change_handler_count=%d (old pending should be gone)\n", signo, change_handler_count);
    sigreturn();
}

/* --------------- Utility Helpers --------------- */
static void block_signal(int signo, sigset_t *saved) {
    sigset_t set = 0;
    set |= SIGMASK(signo); // replaced SIGNO_MASK with SIGMASK
    sigprocmask(SIG_BLOCK, &set, saved);
}
static void unblock_signal(int signo) {
    sigset_t set = 0;
    set |= SIGMASK(signo); // replaced SIGNO_MASK with SIGMASK
    sigprocmask(SIG_UNBLOCK, &set, 0);
}

/* --------------- Test 1: SA_SIGINFO Queue Cap --------------- */
static void test_siginfo_queue_cap(void) {
    printf("\n[Test 1] SA_SIGINFO queue cap / blocking accumulation\n");
    siginfo_count = 0;
    sigaction_t sa = {0};
    sa.sa_sigaction = siginfo_handler;
    sa.sa_flags = SA_SIGINFO; // use sigaction field
    if (sigaction(SIGALRM, &sa, 0) != 0) {
        printf("Failed to install SA_SIGINFO handler\n");
        return;
    }
    sigset_t old;
    block_signal(SIGALRM, &old); // Block SIGALRM so they queue
    int sends = 12; // exceed kernel cap (8)
    for (int i = 0; i < sends; i++) {
        kill(getpid(), SIGALRM);
    }
    printf("Sent %d SIGALRM while blocked; now unblocking (cap expected 8 deliveries)\n", sends);
    unblock_signal(SIGALRM);
    // Allow deliveries
    for (;;) {
        if (siginfo_count >= 8) break; // expected
        if (siginfo_count > 8) break;   // anomaly
        // pause returns after each signal
        pause();
    }
    printf("Delivered %d SIGALRM (should be 8 due to cap)\n", siginfo_count);
    if (siginfo_count == 8) {
        printf("[Test 1] PASS\n");
    } else {
        printf("[Test 1] FAIL: Queue cap mismatch (got %d, expected 8)\n", siginfo_count);
        test_failures++;
    }
}

/* --------------- Test 2: SA_RESETHAND --------------- */
static void test_resehand(void) {
    printf("\n[Test 2] SA_RESETHAND behavior\n");
    rese_count = 0;
    sigaction_t sa = {0};
    sa.sa_handler = rese_handler;
    sa.sa_flags = SA_RESETHAND;
    if (sigaction(SIGUSR1, &sa, 0) != 0) {
        printf("Failed to install SA_RESETHAND handler\n");
        return;
    }
    int parent = getpid();
    int kid = fork();
    if (kid == 0) {
        // Child: send one signal after short delay so parent is already paused.
        sleep(100);
        kill(parent, SIGUSR1); // first (handled & resets to SIG_DFL)
        // Note: We do NOT send a second SIGUSR1 because after SA_RESETHAND,
        // the handler is SIG_DFL, and SIG_DFL for SIGUSR1 terminates the process.
        exit(0);
    }
    // Parent waits for first (only) handler run.
    while (rese_count == 0) {
        pause();
    }
    // Verify the handler was reset: check that sigaction now returns SIG_DFL
    sigaction_t old = {0};
    sigaction(SIGUSR1, 0, &old);
    int handler_was_reset = (old.sa_handler == SIG_DFL);
    
    printf("SA_RESETHAND rese_count=%d (expected 1), handler_reset=%d\n", rese_count, handler_was_reset);
    if (rese_count == 1 && handler_was_reset) {
        printf("[Test 2] PASS\n");
    } else {
        printf("[Test 2] FAIL: rese_count=%d (expected 1), handler_reset=%d (expected 1)\n", 
               rese_count, handler_was_reset);
        test_failures++;
    }
    wait(0); // reap child
}

/* --------------- Test 3: SA_NODEFER Reentrancy --------------- */
static void test_nodefer(void) {
    printf("\n[Test 3] SA_NODEFER reentrancy\n");
    nodefer_depth_max = 0;
    nodefer_current_depth = 0;
    sigaction_t sa = {0};
    sa.sa_handler = nodefer_handler;
    sa.sa_flags = SA_NODEFER;
    if (sigaction(SIGUSR2, &sa, 0) != 0) {
        printf("Failed to install SA_NODEFER handler\n");
        return;
    }
    int parent = getpid();
    int kid = fork();
    if (kid == 0) {
        // Ensure parent is paused before sending
        sleep(100);
        kill(parent, SIGUSR2);
        exit(0);
    }
    // Parent: wait for signal while paused so delivery wakes us afterwards.
    while (nodefer_depth_max == 0) {
        pause();
    }
    printf("SA_NODEFER max recursion depth observed=%d (expected 2)\n", nodefer_depth_max);
    if (nodefer_depth_max == 2) {
        printf("[Test 3] PASS\n");
    } else {
        printf("[Test 3] FAIL: max depth=%d, expected 2\n", nodefer_depth_max);
        test_failures++;
    }
    wait(0); // reap child
}

/* --------------- Test 4: Stop / Continue semantics --------------- */
static void test_stop_continue(void) {
    printf("\n[Test 4] Stop / Continue semantics with SIGCONT handler\n");
    cont_handler_count = 0;
    
    int child = fork();
    if (child < 0) {
        printf("fork failed\n");
        return;
    }
    if (child == 0) {
        // Child process: install SIGCONT handler, then wait to be stopped/continued.
        sigaction_t sa = {0};
        sa.sa_handler = cont_handler;
        if (sigaction(SIGCONT, &sa, 0) != 0) {
            printf("Child: failed to set SIGCONT handler\n");
            exit(-1);
        }
        printf("Child %d ready, entering pause loop...\n", getpid());
        
        // Loop: each SIGCONT should wake us from pause and invoke handler
        // We expect 2 stop/continue cycles
        while (cont_handler_count < 2) {
            pause();
            printf("Child: woke from pause, cont_handler_count=%d\n", cont_handler_count);
        }
        printf("Child %d exiting (cont_handler_count=%d)\n", getpid(), cont_handler_count);
        exit(0);
    }
    
    // Parent: allow child to setup and enter pause
    sleep(100);
    
    // First stop/continue cycle
    printf("Parent: sending SIGSTOP to child %d\n", child);
    kill(child, SIGSTOP);
    sleep(200);  // Give time for child to actually stop
    
    printf("Parent: sending SIGCONT to resume child\n");
    kill(child, SIGCONT);  // Should resume child and invoke handler
    sleep(200);  // Give time for child to wake and run handler
    
    // Second stop/continue cycle
    printf("Parent: sending second SIGSTOP\n");
    kill(child, SIGSTOP);
    sleep(200);
    
    printf("Parent: sending second SIGCONT\n");
    kill(child, SIGCONT);
    sleep(200);
    
    printf("Parent: waiting for child to exit\n");
    int status;
    wait(&status);
    
    // Child sets cont_handler_count=2 before exit; we check exit status.
    if (status == 0) {
        printf("[Test 4] PASS\n");
    } else {
        printf("[Test 4] FAIL: child exited with status %d\n", status);
        test_failures++;
    }
}

/* --------------- Test 5: Change handler clears pending --------------- */
static void test_change_handler_clears_pending(void) {
    printf("\n[Test 5] Changing handler clears pending instances\n");
    change_handler_count = 0;
    // Install a blocking simple handler then queue signals, then change.
    sigaction_t sa = {0};
    sa.sa_handler = simple_handler;
    if (sigaction(SIGALRM, &sa, 0) != 0) {
        printf("Failed to install initial handler for SIGALRM\n");
        return;
    }
    sigset_t old;
    block_signal(SIGALRM, &old);
    for (int i = 0; i < 5; i++) {
        kill(getpid(), SIGALRM);
    }
    // Change handler while still blocked; sigaction() should clear pending queue.
    sigaction_t sa_new = {0};
    sa_new.sa_handler = post_change_handler;
    if (sigaction(SIGALRM, &sa_new, 0) != 0) {
        printf("Failed to change handler for SIGALRM\n");
    }
    unblock_signal(SIGALRM); // If pending were cleared, none delivered yet.
    sleep(100);
    // Now send one SIGALRM which should invoke new handler exactly once.
    kill(getpid(), SIGALRM);
    if (change_handler_count == 0) {
        pause();
    }
    printf("Post-change handler count=%d (expected 1)\n", change_handler_count);
    if (change_handler_count == 1) {
        printf("[Test 5] PASS\n");
    } else {
        printf("[Test 5] FAIL: change_handler_count=%d, expected 1\n", change_handler_count);
        test_failures++;
    }
}

int main(void) {
    printf("Comprehensive signal tests (pid=%d) start\n", getpid());

    test_siginfo_queue_cap();
    test_resehand();
    test_nodefer();
    test_stop_continue();
    test_change_handler_clears_pending();

    printf("\n========================================\n");
    if (test_failures == 0) {
        printf("ALL TESTS PASSED (5/5)\n");
    } else {
        printf("TESTS FAILED: %d/5 failed\n", test_failures);
    }
    printf("========================================\n");
    return test_failures;
}
