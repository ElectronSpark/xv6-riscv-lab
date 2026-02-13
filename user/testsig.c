#include "kernel/inc/types.h"
#include "kernel/inc/signo.h"
// Removed inclusion of kernel/signal.h to avoid prototype conflicts
#include "user/user.h"

#ifndef SIG_BLOCK
#define SIG_BLOCK 1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3
#endif

#ifndef SIGMASK
#define SIGMASK(signo) (1UL << ((signo) - 1))
#endif

#ifndef EINTR
#define EINTR 4
#endif

// Stack allocation helper for clone
#define THREAD_STACK_SIZE (4096 * 4) // 16KB

// Global counters for validations
static volatile int siginfo_count = 0;     // For queue cap test (SIGALRM)
static volatile int rese_count = 0;        // For SA_RESETHAND test (SIGUSR1)
static volatile int nodefer_depth_max = 0; // For SA_NODEFER recursion depth
static volatile int nodefer_current_depth = 0;
static volatile int cont_handler_count = 0; // SIGCONT handler invocations
static volatile int change_handler_count =
    0;                        // Post-change handler delivery count (SIGALRM)
static volatile int sigsuspend_caught = 0;  // For sigsuspend test
static volatile int sigwait_ready = 0;      // For sigwait synchronization
static int test_failures = 0; // Track test failures

// Thread-group test globals (shared via CLONE_VM)
static volatile int tg_child_tid = 0;      // Child thread's TID
static volatile int tg_child_ready = 0;    // Child thread is ready
static volatile int tg_child_caught = 0;   // Signal caught count in child
static volatile int tg_child_signo = 0;    // Signal number caught by child
static volatile int tg_child_done = 0;     // Child thread finished

/* ---------------- Basic Handlers ---------------- */
static void simple_handler(int signo) {
    printf("simple_handler signo=%d\n", signo);
    sigreturn();
}

static void rese_handler(int signo) {
    rese_count++;
    printf("SA_RESETHAND first delivery signo=%d rese_count=%d (will reset to "
           "default)\n",
           signo, rese_count);
    sigreturn();
}

static void nodefer_handler(int signo) {
    nodefer_current_depth++;
    if (nodefer_current_depth > nodefer_depth_max)
        nodefer_depth_max = nodefer_current_depth;
    printf("SA_NODEFER handler depth=%d signo=%d\n", nodefer_current_depth,
           signo);
    if (nodefer_current_depth == 1) {
        // Re-enter by raising again; with SA_NODEFER we should immediately
        // recurse.
        kill(getpid(), signo);
    }
    nodefer_current_depth--;
    sigreturn();
}

static void siginfo_handler(int signo, siginfo_t *info, void *context) {
    siginfo_count++;
    printf("SIGINFO handler signo=%d count=%d ", signo, siginfo_count);
    if (info) {
        printf("[si_code=%d sival_int=%d pid=%d]", info->si_code,
               info->si_value.sival_int, info->si_pid);
    }
    printf("\n");
    sigreturn();
}

static void cont_handler(int signo) {
    cont_handler_count++;
    printf("SIGCONT handler invoked count=%d signo=%d\n", cont_handler_count,
           signo);
    sigreturn();
}

static void post_change_handler(int signo) {
    change_handler_count++;
    printf("Post-change handler delivered signo=%d change_handler_count=%d "
           "(old pending should be gone)\n",
           signo, change_handler_count);
    sigreturn();
}

static void sigsuspend_handler(int signo) {
    sigsuspend_caught++;
    printf("sigsuspend_handler signo=%d caught=%d\n", signo, sigsuspend_caught);
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
    int sends = 12;              // exceed kernel cap (8)
    for (int i = 0; i < sends; i++) {
        kill(getpid(), SIGALRM);
    }
    printf("Sent %d SIGALRM while blocked; now unblocking (cap expected 8 "
           "deliveries)\n",
           sends);
    unblock_signal(SIGALRM);
    // Allow deliveries
    for (;;) {
        if (siginfo_count >= 8)
            break; // expected
        if (siginfo_count > 8)
            break; // anomaly
        // pause returns after each signal
        pause();
    }
    printf("Delivered %d SIGALRM (should be 8 due to cap)\n", siginfo_count);
    if (siginfo_count == 8) {
        printf("[Test 1] PASS\n");
    } else {
        printf("[Test 1] FAIL: Queue cap mismatch (got %d, expected 8)\n",
               siginfo_count);
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
        // the handler is SIG_DFL, and SIG_DFL for SIGUSR1 terminates the
        // process.
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

    printf("SA_RESETHAND rese_count=%d (expected 1), handler_reset=%d\n",
           rese_count, handler_was_reset);
    if (rese_count == 1 && handler_was_reset) {
        printf("[Test 2] PASS\n");
    } else {
        printf("[Test 2] FAIL: rese_count=%d (expected 1), handler_reset=%d "
               "(expected 1)\n",
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
    printf("SA_NODEFER max recursion depth observed=%d (expected 2)\n",
           nodefer_depth_max);
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
        // Child process: install SIGCONT handler, then wait to be
        // stopped/continued.
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
            printf("Child: woke from pause, cont_handler_count=%d\n",
                   cont_handler_count);
        }
        printf("Child %d exiting (cont_handler_count=%d)\n", getpid(),
               cont_handler_count);
        exit(0);
    }

    // Parent: allow child to setup and enter pause
    sleep(100);

    // First stop/continue cycle
    printf("Parent: sending SIGSTOP to child %d\n", child);
    kill(child, SIGSTOP);
    sleep(200); // Give time for child to actually stop

    printf("Parent: sending SIGCONT to resume child\n");
    kill(child, SIGCONT); // Should resume child and invoke handler
    sleep(200);           // Give time for child to wake and run handler

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

/* --------------- Test 5: Change handler keeps pending (non-ignored) ---- */
static void test_change_handler_clears_pending(void) {
    printf("\n[Test 5] Changing handler preserves pending non-ignored instances\n");
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
    // Change handler while still blocked; pending SIGALRM should be preserved
    // (not cleared) because new disposition is still a catching handler.
    sigaction_t sa_new = {0};
    sa_new.sa_handler = post_change_handler;
    if (sigaction(SIGALRM, &sa_new, 0) != 0) {
        printf("Failed to change handler for SIGALRM\n");
    }
    unblock_signal(SIGALRM); // Pending blocked SIGALRM should now deliver once.
    sleep(100);
    // Send one additional SIGALRM, so total expected deliveries become 2.
    kill(getpid(), SIGALRM);
    if (change_handler_count < 2) {
        pause();
    }
    printf("Post-change handler count=%d (expected 2)\n", change_handler_count);
    if (change_handler_count == 2) {
        printf("[Test 5] PASS\n");
    } else {
        printf("[Test 5] FAIL: change_handler_count=%d, expected 2\n",
               change_handler_count);
        test_failures++;
    }
}

/* --------------- Test 6: sigsuspend --------------- */
static void test_sigsuspend(void) {
    printf("\n[Test 6] sigsuspend: atomically replace mask and wait\n");
    sigsuspend_caught = 0;

    // Install handler for SIGUSR1
    sigaction_t sa = {0};
    sa.sa_handler = sigsuspend_handler;
    if (sigaction(SIGUSR1, &sa, 0) != 0) {
        printf("Failed to install sigsuspend handler\n");
        return;
    }

    // Block SIGUSR1 in the normal mask
    sigset_t old;
    block_signal(SIGUSR1, &old);

    int parent = getpid();
    int kid = fork();
    if (kid == 0) {
        // Child: let parent enter sigsuspend, then send signal
        sleep(200);
        kill(parent, SIGUSR1);
        exit(0);
    }

    // Parent: call sigsuspend with empty mask (unblocks SIGUSR1 temporarily)
    sigset_t empty = 0;
    int ret = sigsuspend(&empty);
    // sigsuspend should return -EINTR
    printf("sigsuspend returned %d, sigsuspend_caught=%d\n", ret,
           sigsuspend_caught);

    // Verify SIGUSR1 is still blocked in the restored mask
    sigset_t current_mask = 0;
    sigprocmask(SIG_SETMASK, 0, &current_mask);
    int still_blocked = (current_mask & SIGMASK(SIGUSR1)) != 0;
    printf("SIGUSR1 still blocked after sigsuspend=%d (expected 1)\n",
           still_blocked);

    // Restore original mask
    sigprocmask(SIG_SETMASK, &old, 0);

    if (sigsuspend_caught == 1 && ret == -EINTR && still_blocked) {
        printf("[Test 6] PASS\n");
    } else {
        printf("[Test 6] FAIL: caught=%d ret=%d still_blocked=%d\n",
               sigsuspend_caught, ret, still_blocked);
        test_failures++;
    }
    wait(0);
}

/* --------------- Test 7: sigwait --------------- */
static void test_sigwait(void) {
    printf("\n[Test 7] sigwait: dequeue signal without handler\n");

    // Block SIGUSR2 so it can be consumed by sigwait
    sigset_t old;
    block_signal(SIGUSR2, &old);

    // Remove any handler — sigwait should consume without invoking handler
    sigaction_t sa = {0};
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR2, &sa, 0);

    int parent = getpid();
    int kid = fork();
    if (kid == 0) {
        sleep(200);
        kill(parent, SIGUSR2);
        exit(0);
    }

    sigset_t wait_set = SIGMASK(SIGUSR2);
    int sig = 0;
    int ret = sigwait(&wait_set, &sig);
    printf("sigwait returned %d, sig=%d (expected SIGUSR2=%d)\n", ret, sig,
           SIGUSR2);

    // Verify signal was consumed (no longer pending)
    sigset_t pending = 0;
    sigpending(&pending);
    int still_pending = (pending & SIGMASK(SIGUSR2)) != 0;
    printf("SIGUSR2 still pending=%d (expected 0)\n", still_pending);

    sigprocmask(SIG_SETMASK, &old, 0);

    if (ret == 0 && sig == SIGUSR2 && !still_pending) {
        printf("[Test 7] PASS\n");
    } else {
        printf("[Test 7] FAIL: ret=%d sig=%d still_pending=%d\n", ret, sig,
               still_pending);
        test_failures++;
    }
    wait(0);
}

/* --------------- Test 8: tkill --------------- */
static void test_tkill(void) {
    printf("\n[Test 8] tkill: send signal to specific thread by TID\n");
    sigsuspend_caught = 0; // reuse counter

    // Install handler for SIGUSR1 
    sigaction_t sa = {0};
    sa.sa_handler = sigsuspend_handler;
    if (sigaction(SIGUSR1, &sa, 0) != 0) {
        printf("Failed to install tkill handler\n");
        return;
    }

    int tid = gettid();
    int ret = tkill(tid, SIGUSR1);
    // Handler should have run synchronously (signal to self)
    printf("tkill returned %d, caught=%d\n", ret, sigsuspend_caught);

    // Test error case: invalid TID
    int ret_bad = tkill(-1, SIGUSR1);
    printf("tkill(-1, SIGUSR1) returned %d (expected negative)\n", ret_bad);

    // Test signal 0 (existence check) — tkill currently doesn't special-case
    // sig 0 like kill(), so it returns -EINVAL. That's fine.

    if (ret == 0 && sigsuspend_caught == 1 && ret_bad < 0) {
        printf("[Test 8] PASS\n");
    } else {
        printf("[Test 8] FAIL: ret=%d caught=%d ret_bad=%d\n", ret,
               sigsuspend_caught, ret_bad);
        test_failures++;
    }
}

static void tg_signal_handler(int signo) {
    tg_child_caught++;
    tg_child_signo = signo;
    sigreturn();
}

/* --------------- Thread entry points for thread-group tests --------------- */

// Thread entry for Test 9: tgkill targets specific thread
static void tg_tgkill_thread_entry(void) {
    tg_child_tid = gettid();
    tg_child_ready = 1;
    // Wait for signal
    while (tg_child_caught == 0) {
        pause();
    }
    tg_child_done = 1;
    exit(0);
}

// Thread entry for Test 10: process-directed kill to thread group
// This thread blocks SIGUSR1, so the signal should go to the leader instead.
static void tg_kill_block_thread_entry(void) {
    // Block SIGUSR1 in this thread only (per-thread mask)
    sigset_t set = SIGMASK(SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, 0);
    tg_child_tid = gettid();
    tg_child_ready = 1;
    // Spin until parent says done (don't use pause — signal is blocked)
    while (!tg_child_done) {
        sleep(10);
    }
    exit(0);
}

// Thread entry for Test 11: SIGKILL kills all threads in group
static void tg_sigkill_thread_entry(void) {
    tg_child_tid = gettid();
    tg_child_ready = 1;
    // Spin forever — SIGKILL will terminate us
    for (;;) {
        sleep(100);
    }
    // Should never reach here
    exit(0);
}

// Thread entry for Test 12: sigsuspend in child thread
// Now that signal masks are per-thread, we can safely block SIGUSR1
// in this thread without affecting the leader.
static void tg_sigsuspend_thread_entry(void) {
    tg_child_tid = gettid();
    // Block SIGUSR1 before signaling ready
    sigset_t set = SIGMASK(SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, 0);
    tg_child_ready = 1;
    // sigsuspend with empty mask — temporarily unblocks SIGUSR1
    sigset_t empty = 0;
    sigsuspend(&empty);
    // If we get here, signal was caught and sigsuspend returned
    tg_child_done = 1;
    exit(0);
}

// Thread entry for Test 13: sigwait in child thread
// Now that signal masks are per-thread, we can safely block SIGUSR2
// in this thread without affecting the leader.
static void tg_sigwait_thread_entry(void) {
    tg_child_tid = gettid();
    // Block SIGUSR2 so sigwait can consume it
    sigset_t set = SIGMASK(SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, 0);
    tg_child_ready = 1;
    // Wait for SIGUSR2
    sigset_t wait_set = SIGMASK(SIGUSR2);
    int sig = 0;
    int ret = sigwait(&wait_set, &sig);
    tg_child_signo = sig;
    tg_child_caught = (ret == 0 && sig == SIGUSR2) ? 1 : -1;
    tg_child_done = 1;
    exit(0);
}

// Helper: create a thread in same thread group using clone
static int create_thread(void (*entry)(void)) {
    char *stack = sbrk(THREAD_STACK_SIZE);
    if (stack == (char *)-1) {
        printf("sbrk failed for thread stack\n");
        return -1;
    }
    struct clone_args args = {
        .flags = CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_FILES |
                 CLONE_FS | SIGCHLD,
        .stack = (uint64)stack,
        .stack_size = THREAD_STACK_SIZE,
        .entry = (uint64)entry,
    };
    return clone(&args);
}

/* --------------- Test 9: tgkill to thread in multi-thread group ---------- */
static void test_tgkill_thread_group(void) {
    printf("\n[Test 9] tgkill: signal specific thread in thread group\n");

    // Fork a new process so we don't pollute our thread group
    int child = fork();
    if (child < 0) {
        printf("fork failed\n");
        test_failures++;
        return;
    }
    if (child == 0) {
        // Child process: create a thread group
        tg_child_ready = 0;
        tg_child_caught = 0;
        tg_child_done = 0;

        // Install handler for SIGUSR1 (shared via CLONE_SIGHAND)
        sigaction_t sa = {0};
        sa.sa_handler = tg_signal_handler;
        sigaction(SIGUSR1, &sa, 0);

        int tid = create_thread(tg_tgkill_thread_entry);
        if (tid < 0) {
            printf("clone failed\n");
            exit(1);
        }

        // Wait for child thread to be ready
        while (!tg_child_ready) {
            sleep(10);
        }

        int tgid = getpid();
        int child_thread_tid = tg_child_tid;
        printf("Leader tid=%d, child thread tid=%d, tgid=%d\n", gettid(),
               child_thread_tid, tgid);

        // Send SIGUSR1 to the child thread specifically
        int ret = tgkill(tgid, child_thread_tid, SIGUSR1);
        printf("tgkill(%d, %d, SIGUSR1) returned %d\n", tgid,
               child_thread_tid, ret);

        // Wait for child thread to handle signal
        while (!tg_child_done) {
            sleep(10);
        }
        printf("Child thread caught=%d signo=%d\n", tg_child_caught,
               tg_child_signo);

        // Test error: wrong tgid
        int ret_bad = tgkill(9999, child_thread_tid, SIGUSR1);
        printf("tgkill(9999, %d, SIGUSR1) returned %d (expected -ESRCH)\n",
               child_thread_tid, ret_bad);

        if (ret == 0 && tg_child_caught == 1 && tg_child_signo == SIGUSR1 &&
            ret_bad < 0) {
            printf("[Test 9] PASS\n");
            exit(0);
        } else {
            printf("[Test 9] FAIL\n");
            exit(1);
        }
    }
    int status;
    wait(&status);
    if (status != 0) {
        printf("[Test 9] FAIL (child exited %d)\n", status);
        test_failures++;
    }
}

/* --------------- Test 10: kill() to thread group (process-directed) ------ */
static void test_kill_thread_group(void) {
    printf("\n[Test 10] kill: process-directed signal to thread group\n");

    int child = fork();
    if (child < 0) {
        printf("fork failed\n");
        test_failures++;
        return;
    }
    if (child == 0) {
        tg_child_ready = 0;
        tg_child_caught = 0;
        tg_child_done = 0;
        tg_child_signo = 0;

        // Install handler for SIGUSR1 (shared via CLONE_SIGHAND)
        sigaction_t sa = {0};
        sa.sa_handler = tg_signal_handler;
        sigaction(SIGUSR1, &sa, 0);

        // Create a thread that BLOCKS SIGUSR1 (per-thread mask)
        int tid = create_thread(tg_kill_block_thread_entry);
        if (tid < 0) {
            printf("clone failed\n");
            exit(1);
        }
        while (!tg_child_ready) {
            sleep(10);
        }

        int tgid = getpid();
        printf("Leader tid=%d, child thread tid=%d (blocks SIGUSR1), "
               "tgid=%d\n",
               gettid(), tg_child_tid, tgid);

        // Leader does NOT block SIGUSR1, so process-directed kill should
        // deliver to the leader (the child thread has it blocked per-thread).
        kill(tgid, SIGUSR1);

        // Give time for delivery (handler runs on return to userspace)
        // The handler may fire on this thread or the child thread
        int timeout = 0;
        while (tg_child_caught == 0 && timeout < 50) {
            sleep(10);
            timeout++;
        }

        printf("Leader caught=%d signo=%d\n", tg_child_caught,
               tg_child_signo);

        // Tell child thread to exit
        tg_child_done = 1;
        sleep(100);

        if (tg_child_caught >= 1 && tg_child_signo == SIGUSR1) {
            printf("[Test 10] PASS\n");
            exit(0);
        } else {
            printf("[Test 10] FAIL: caught=%d signo=%d\n",
                   tg_child_caught, tg_child_signo);
            exit(1);
        }
    }
    int status;
    wait(&status);
    if (status != 0) {
        printf("[Test 10] FAIL (child exited %d)\n", status);
        test_failures++;
    }
}

/* --------------- Test 11: SIGKILL to thread group kills all threads ------ */
static void test_sigkill_thread_group(void) {
    printf("\n[Test 11] SIGKILL: kills entire thread group\n");

    int child = fork();
    if (child < 0) {
        printf("fork failed\n");
        test_failures++;
        return;
    }
    if (child == 0) {
        tg_child_ready = 0;
        tg_child_done = 0;

        int tid = create_thread(tg_sigkill_thread_entry);
        if (tid < 0) {
            printf("clone failed\n");
            exit(1);
        }

        while (!tg_child_ready) {
            sleep(10);
        }

        printf("Thread group: leader=%d child_thread=%d\n", getpid(),
               tg_child_tid);

        // Signal parent we're ready by sleeping (parent will send SIGKILL)
        // Spin forever — SIGKILL should terminate us
        for (;;) {
            sleep(100);
        }
        // Should never reach here
        exit(0);
    }

    // Parent: give child time to create thread group, then SIGKILL
    sleep(200);
    printf("Parent: sending SIGKILL to child process %d (thread group)\n",
           child);
    kill(child, SIGKILL);

    int status;
    wait(&status);
    // Child should have been killed (xstate will be -1 from SIGKILL)
    printf("Child exited with status %d\n", status);

    // Success: child was terminated by SIGKILL. The exact status depends
    // on kernel implementation, but it shouldn't be 0 (normal exit).
    if (status != 0) {
        printf("[Test 11] PASS\n");
    } else {
        printf("[Test 11] FAIL: expected non-zero exit status\n");
        test_failures++;
    }
}

/* --------------- Test 12: sigsuspend in child thread of thread group ----- */
static void test_sigsuspend_thread_group(void) {
    printf("\n[Test 12] sigsuspend: in child thread of thread group\n");

    int child = fork();
    if (child < 0) {
        printf("fork failed\n");
        test_failures++;
        return;
    }
    if (child == 0) {
        tg_child_ready = 0;
        tg_child_caught = 0;
        tg_child_done = 0;

        // Install handler for SIGUSR1 (shared via CLONE_SIGHAND)
        sigaction_t sa = {0};
        sa.sa_handler = tg_signal_handler;
        sigaction(SIGUSR1, &sa, 0);

        int tid = create_thread(tg_sigsuspend_thread_entry);
        if (tid < 0) {
            printf("clone failed\n");
            exit(1);
        }
        while (!tg_child_ready) {
            sleep(10);
        }

        int tgid = getpid();
        int child_thread_tid = tg_child_tid;
        printf("Leader=%d, child thread=%d in sigsuspend\n", gettid(),
               child_thread_tid);

        // Give time for child to enter sigsuspend
        sleep(200);

        // Send SIGUSR1 directly to the child thread (thread-directed)
        tgkill(tgid, child_thread_tid, SIGUSR1);

        // Wait for child to finish
        int timeout = 0;
        while (!tg_child_done && timeout < 100) {
            sleep(10);
            timeout++;
        }

        printf("Child thread sigsuspend: caught=%d done=%d\n",
               tg_child_caught, tg_child_done);
        if (tg_child_done && tg_child_caught >= 1) {
            printf("[Test 12] PASS\n");
            exit(0);
        } else {
            printf("[Test 12] FAIL: done=%d caught=%d\n", tg_child_done,
                   tg_child_caught);
            exit(1);
        }
    }
    int status;
    wait(&status);
    if (status != 0) {
        printf("[Test 12] FAIL (child exited %d)\n", status);
        test_failures++;
    }
}

/* --------------- Test 13: sigwait in child thread of thread group -------- */
static void test_sigwait_thread_group(void) {
    printf("\n[Test 13] sigwait: in child thread of thread group\n");

    int child = fork();
    if (child < 0) {
        printf("fork failed\n");
        test_failures++;
        return;
    }
    if (child == 0) {
        tg_child_ready = 0;
        tg_child_caught = 0;
        tg_child_done = 0;
        tg_child_signo = 0;

        // Install a user handler for SIGUSR2 so it's not in the termination
        // category (SIG_DFL would cause THREAD_SET_KILLED in __signal_send).
        // sigwait will dequeue it before handle_signal delivers the handler.
        sigaction_t sa = {0};
        sa.sa_handler = tg_signal_handler;
        sigaction(SIGUSR2, &sa, 0);

        int tid = create_thread(tg_sigwait_thread_entry);
        if (tid < 0) {
            printf("clone failed\n");
            exit(1);
        }
        while (!tg_child_ready) {
            sleep(10);
        }

        int tgid = getpid();
        int child_thread_tid = tg_child_tid;
        printf("Leader=%d, child thread=%d in sigwait\n", gettid(),
               child_thread_tid);

        // Give time for child to enter sigwait
        sleep(200);

        // Send SIGUSR2 directly to the child thread
        tgkill(tgid, child_thread_tid, SIGUSR2);

        // Wait for child to finish
        int timeout = 0;
        while (!tg_child_done && timeout < 100) {
            sleep(10);
            timeout++;
        }

        printf("Child thread sigwait: caught=%d signo=%d done=%d\n",
               tg_child_caught, tg_child_signo, tg_child_done);
        if (tg_child_done && tg_child_caught == 1 &&
            tg_child_signo == SIGUSR2) {
            printf("[Test 13] PASS\n");
            exit(0);
        } else {
            printf("[Test 13] FAIL: done=%d caught=%d signo=%d\n",
                   tg_child_done, tg_child_caught, tg_child_signo);
            exit(1);
        }
    }
    int status;
    wait(&status);
    if (status != 0) {
        printf("[Test 13] FAIL (child exited %d)\n", status);
        test_failures++;
    }
}

#define TOTAL_TESTS 13

int main(void) {
    printf("Comprehensive signal tests (pid=%d) start\n", getpid());

    test_siginfo_queue_cap();
    test_resehand();
    test_nodefer();
    test_stop_continue();
    test_change_handler_clears_pending();
    test_sigsuspend();
    test_sigwait();
    test_tkill();
    test_tgkill_thread_group();
    test_kill_thread_group();
    test_sigkill_thread_group();
    test_sigsuspend_thread_group();
    test_sigwait_thread_group();

    printf("\n========================================\n");
    if (test_failures == 0) {
        printf("ALL TESTS PASSED (%d/%d)\n", TOTAL_TESTS, TOTAL_TESTS);
    } else {
        printf("TESTS FAILED: %d/%d failed\n", test_failures, TOTAL_TESTS);
    }
    printf("========================================\n");
    return test_failures;
}
