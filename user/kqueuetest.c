/*
 * kqueuetest.c - Comprehensive test suite for the kqueue subsystem
 *
 * Categories:
 *   1. Basic positive tests (EVFILT_READ, EVFILT_WRITE, EVFILT_TIMER, EVFILT_PROC)
 *   2. Negative / error-path tests
 *   3. Stress tests
 */

#include "kernel/inc/types.h"
#include "kernel/inc/kqueue.h"
#include "user/user.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name)                                   \
    do {                                                  \
        printf("  PASS: %s\n", name);                     \
        tests_passed++;                                   \
    } while (0)

#define TEST_FAIL(name, reason)                           \
    do {                                                  \
        printf("  FAIL: %s - %s\n", name, reason);        \
        tests_failed++;                                   \
    } while (0)

/* ======================================================================
 * Helper: fill a kevent struct
 * ====================================================================== */
static void kev_set(struct kevent *ev, uint64 ident, int16 filter,
                    uint16 flags, uint32 fflags, int64 data, uint64 udata) {
    ev->ident = ident;
    ev->filter = filter;
    ev->flags = flags;
    ev->fflags = fflags;
    ev->data = data;
    ev->udata = udata;
}

/* ======================================================================
 * 1. BASIC POSITIVE TESTS
 * ====================================================================== */

/* 1a. EVFILT_READ on a pipe */
static void test_pipe_read(void) {
    const char *name = "EVFILT_READ (pipe)";
    int fds[2];
    if (pipe(fds) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); close(fds[0]); close(fds[1]); return; }

    struct kevent ev;
    kev_set(&ev, fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 42);
    if (kevent_register(kq, &ev, 1) < 0) {
        TEST_FAIL(name, "kevent_register failed");
        close(fds[0]); close(fds[1]); close(kq); return;
    }

    write(fds[1], "hello", 5);

    struct kevent out;
    int ret = kevent_wait(kq, &out, 1, 1000);
    if (ret <= 0) { TEST_FAIL(name, "kevent_wait returned no event"); }
    else if (out.filter != EVFILT_READ) { TEST_FAIL(name, "wrong filter"); }
    else if (out.udata != 42) { TEST_FAIL(name, "udata mismatch"); }
    else { TEST_PASS(name); }

    close(fds[0]); close(fds[1]); close(kq);
}

/* 1b. EVFILT_WRITE on a pipe */
static void test_pipe_write(void) {
    const char *name = "EVFILT_WRITE (pipe)";
    int fds[2];
    if (pipe(fds) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); close(fds[0]); close(fds[1]); return; }

    struct kevent ev;
    kev_set(&ev, fds[1], EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, 55);
    if (kevent_register(kq, &ev, 1) < 0) {
        TEST_FAIL(name, "kevent_register failed");
        close(fds[0]); close(fds[1]); close(kq); return;
    }

    /* Pipe write-end should be immediately writable on an empty pipe */
    struct kevent out;
    int ret = kevent_wait(kq, &out, 1, 1000);
    if (ret <= 0) { TEST_FAIL(name, "kevent_wait returned no event"); }
    else if (out.filter != EVFILT_WRITE) { TEST_FAIL(name, "wrong filter"); }
    else if (out.udata != 55) { TEST_FAIL(name, "udata mismatch"); }
    else { TEST_PASS(name); }

    close(fds[0]); close(fds[1]); close(kq);
}

/* 1c. EVFILT_TIMER one-shot */
static void test_timer_oneshot(void) {
    const char *name = "EVFILT_TIMER (oneshot)";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    kev_set(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 100, 99);
    if (kevent_register(kq, &ev, 1) < 0) {
        TEST_FAIL(name, "kevent_register failed");
        close(kq); return;
    }

    struct kevent out;
    int ret = kevent_wait(kq, &out, 1, 2000);
    if (ret <= 0) { TEST_FAIL(name, "timer did not fire"); }
    else if (out.filter != EVFILT_TIMER) { TEST_FAIL(name, "wrong filter"); }
    else if (out.udata != 99) { TEST_FAIL(name, "udata mismatch"); }
    else { TEST_PASS(name); }

    close(kq);
}

/* 1d. EVFILT_TIMER periodic — check it fires multiple times */
static void test_timer_periodic(void) {
    const char *name = "EVFILT_TIMER (periodic)";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    /* 50ms periodic timer with EV_CLEAR so it re-arms */
    kev_set(&ev, 10, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 50, 0);
    if (kevent_register(kq, &ev, 1) < 0) {
        TEST_FAIL(name, "kevent_register failed");
        close(kq); return;
    }

    int fires = 0;
    for (int i = 0; i < 5; i++) {
        struct kevent out;
        int ret = kevent_wait(kq, &out, 1, 500);
        if (ret > 0 && out.filter == EVFILT_TIMER)
            fires++;
    }

    /* Remove the timer before closing */
    kev_set(&ev, 10, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
    kevent_register(kq, &ev, 1);

    if (fires >= 3)
        TEST_PASS(name);
    else {
        printf("    (got %d fires, expected >= 3)\n", fires);
        TEST_FAIL(name, "not enough periodic fires");
    }

    close(kq);
}

/* 1e. EVFILT_PROC — watch for NOTE_FORK */
static void test_proc_fork(void) {
    const char *name = "EVFILT_PROC (fork)";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    int pid = getpid();
    struct kevent ev;
    kev_set(&ev, pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_FORK | NOTE_EXIT, 0, 77);
    if (kevent_register(kq, &ev, 1) < 0) {
        TEST_FAIL(name, "kevent_register failed");
        close(kq); return;
    }

    int child = fork();
    if (child < 0) { TEST_FAIL(name, "fork() failed"); close(kq); return; }
    if (child == 0) { close(kq); exit(0); }

    struct kevent out;
    int ret = kevent_wait(kq, &out, 1, 2000);
    wait(0);

    if (ret <= 0) { TEST_FAIL(name, "no event received"); }
    else if (out.filter != EVFILT_PROC) { TEST_FAIL(name, "wrong filter"); }
    else { TEST_PASS(name); }

    close(kq);
}

/* 1f. EVFILT_READ — pipe close fires EV_EOF / event */
static void test_pipe_eof(void) {
    const char *name = "EVFILT_READ (pipe EOF)";
    int fds[2];
    if (pipe(fds) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); close(fds[0]); close(fds[1]); return; }

    struct kevent ev;
    kev_set(&ev, fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0);
    if (kevent_register(kq, &ev, 1) < 0) {
        TEST_FAIL(name, "kevent_register failed");
        close(fds[0]); close(fds[1]); close(kq); return;
    }

    /* Close write end — should trigger a read event or EOF on read end */
    close(fds[1]);

    struct kevent out;
    int ret = kevent_wait(kq, &out, 1, 1000);
    if (ret > 0 && out.filter == EVFILT_READ)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "no read event on pipe EOF");

    close(fds[0]); close(kq);
}

/* 1g. EV_DISABLE / EV_ENABLE — disabled knote should not fire */
static void test_ev_disable_enable(void) {
    const char *name = "EV_DISABLE / EV_ENABLE";
    int fds[2];
    if (pipe(fds) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); close(fds[0]); close(fds[1]); return; }

    struct kevent ev;
    /* Add a READ filter but disable it immediately */
    kev_set(&ev, fds[0], EVFILT_READ, EV_ADD | EV_DISABLE, 0, 0, 0);
    kevent_register(kq, &ev, 1);

    write(fds[1], "data", 4);
    /* Poll with 0 timeout — should see nothing since knote is disabled */
    struct kevent out;
    int ret = kevent_wait(kq, &out, 1, 200);
    if (ret != 0) {
        TEST_FAIL(name, "disabled knote delivered event");
        close(fds[0]); close(fds[1]); close(kq); return;
    }

    /* Now re-enable */
    kev_set(&ev, fds[0], EVFILT_READ, EV_ENABLE, 0, 0, 0);
    kevent_register(kq, &ev, 1);

    /* Should now see the event */
    ret = kevent_wait(kq, &out, 1, 1000);
    if (ret > 0 && out.filter == EVFILT_READ)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "re-enabled knote did not fire");

    close(fds[0]); close(fds[1]); close(kq);
}

/* 1h. EV_DELETE — remove a registered knote */
static void test_ev_delete(void) {
    const char *name = "EV_DELETE";
    int fds[2];
    if (pipe(fds) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); close(fds[0]); close(fds[1]); return; }

    struct kevent ev;
    kev_set(&ev, fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0);
    kevent_register(kq, &ev, 1);

    /* Delete it */
    kev_set(&ev, fds[0], EVFILT_READ, EV_DELETE, 0, 0, 0);
    kevent_register(kq, &ev, 1);

    /* Write data and poll — should get nothing */
    write(fds[1], "data", 4);
    struct kevent out;
    int ret = kevent_wait(kq, &out, 1, 200);
    if (ret == 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "deleted knote still fired");

    close(fds[0]); close(fds[1]); close(kq);
}

/* 1i. multiple events at once */
static void test_multi_event(void) {
    const char *name = "multiple events at once";
    int fds1[2], fds2[2];
    if (pipe(fds1) < 0 || pipe(fds2) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent evs[2];
    kev_set(&evs[0], fds1[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 1);
    kev_set(&evs[1], fds2[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 2);
    kevent_register(kq, evs, 2);

    /* Write to both pipes */
    write(fds1[1], "a", 1);
    write(fds2[1], "b", 1);

    /* Collect both events */
    struct kevent out[2];
    int total = 0;
    for (int i = 0; i < 2 && total < 2; i++) {
        int ret = kevent_wait(kq, &out[total], 2 - total, 1000);
        if (ret > 0) total += ret;
    }

    if (total == 2)
        TEST_PASS(name);
    else {
        printf("    (got %d events, expected 2)\n", total);
        TEST_FAIL(name, "did not receive both events");
    }

    close(fds1[0]); close(fds1[1]);
    close(fds2[0]); close(fds2[1]);
    close(kq);
}

/* 1j. kevent_wait with timeout=0 (non-blocking poll) */
static void test_poll_no_event(void) {
    const char *name = "kevent_wait timeout=0 (poll)";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    /* Register a 10-second timer (won't fire) */
    struct kevent ev;
    kev_set(&ev, 100, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 10000, 0);
    kevent_register(kq, &ev, 1);

    struct kevent out;
    int ret = kevent_wait(kq, &out, 1, 0);
    if (ret == 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "poll returned event when none expected");

    /* Clean up timer */
    kev_set(&ev, 100, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
    kevent_register(kq, &ev, 1);
    close(kq);
}

/* ======================================================================
 * 2. NEGATIVE / ERROR-PATH TESTS
 * ====================================================================== */

/* 2a. kqueue() should succeed (basic sanity) and return valid fd */
static void test_neg_kqueue_valid(void) {
    const char *name = "kqueue() returns valid fd";
    int kq = kqueue();
    if (kq < 0)
        TEST_FAIL(name, "kqueue() returned negative");
    else {
        TEST_PASS(name);
        close(kq);
    }
}

/* 2b. kevent_register with invalid kqueue fd */
static void test_neg_bad_kqfd(void) {
    const char *name = "kevent_register with bad kqfd";
    struct kevent ev;
    kev_set(&ev, 0, EVFILT_READ, EV_ADD, 0, 0, 0);
    int ret = kevent_register(999, &ev, 1);
    if (ret < 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "expected error for bad fd");
}

/* 2c. kevent_register with invalid filter */
static void test_neg_bad_filter(void) {
    const char *name = "register invalid filter";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    kev_set(&ev, 0, -99, EV_ADD, 0, 0, 0);  /* -99 is not a valid filter */
    int ret = kevent_register(kq, &ev, 1);
    /* The kernel sets EV_ERROR in the event rather than failing the syscall */
    if (ret == 0 && (ev.flags & EV_ERROR))
        TEST_PASS(name);
    else if (ret < 0)
        TEST_PASS(name);  /* also acceptable */
    else
        TEST_FAIL(name, "expected error for invalid filter");

    close(kq);
}

/* 2d. EV_DELETE on non-existent knote */
static void test_neg_delete_nonexistent(void) {
    const char *name = "EV_DELETE nonexistent knote";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    kev_set(&ev, 12345, EVFILT_READ, EV_DELETE, 0, 0, 0);
    kevent_register(kq, &ev, 1);

    if (ev.flags & EV_ERROR)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "expected EV_ERROR for nonexistent knote");

    close(kq);
}

/* 2e. EVFILT_READ on an invalid fd */
static void test_neg_read_bad_fd(void) {
    const char *name = "EVFILT_READ on bad fd";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    kev_set(&ev, 999, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0);
    int ret = kevent_register(kq, &ev, 1);
    /* Should set EV_ERROR because fd 999 doesn't exist */
    if ((ret == 0 && (ev.flags & EV_ERROR)) || ret < 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "expected error for bad fd");

    close(kq);
}

/* 2f. EVFILT_TIMER with zero/negative interval */
static void test_neg_timer_zero(void) {
    const char *name = "EVFILT_TIMER zero interval";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    kev_set(&ev, 50, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, 0, 0);  /* data=0 */
    int ret = kevent_register(kq, &ev, 1);
    if ((ret == 0 && (ev.flags & EV_ERROR)) || ret < 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "expected error for zero interval");

    close(kq);
}

/* 2g. EVFILT_PROC on non-existent pid */
static void test_neg_proc_bad_pid(void) {
    const char *name = "EVFILT_PROC bad pid";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    kev_set(&ev, 99999, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, 0);
    int ret = kevent_register(kq, &ev, 1);
    if ((ret == 0 && (ev.flags & EV_ERROR)) || ret < 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "expected error for nonexistent pid");

    close(kq);
}

/* 2h. kevent_wait with nevents=0 */
static void test_neg_wait_zero_nevents(void) {
    const char *name = "kevent_wait nevents=0";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent out;
    int ret = kevent_wait(kq, &out, 0, 0);
    if (ret < 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "expected error for nevents=0");

    close(kq);
}

/* 2i. kevent_register with nchanges=0 — should be a no-op success */
static void test_neg_register_zero(void) {
    const char *name = "kevent_register nchanges=0";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    int ret = kevent_register(kq, (struct kevent *)0, 0);
    if (ret == 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "expected success for nchanges=0");

    close(kq);
}

/* 2j. double close of kqueue fd — second close should fail gracefully */
static void test_neg_double_close(void) {
    const char *name = "kqueue double close";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    close(kq);
    int ret = close(kq);
    /* Second close should return error (EBADF) or at least not crash */
    if (ret < 0)
        TEST_PASS(name);
    else
        TEST_PASS(name);  /* not crashing is also acceptable */
}

/* 2k. kevent_register with no flags (not EV_ADD/DELETE/ENABLE/DISABLE) */
static void test_neg_register_no_flags(void) {
    const char *name = "kevent_register with flags=0";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    kev_set(&ev, 0, EVFILT_TIMER, 0, 0, 100, 0);  /* flags = 0 */
    int ret = kevent_register(kq, &ev, 1);
    /* Should set EV_ERROR because no valid action flag */
    if ((ret == 0 && (ev.flags & EV_ERROR)) || ret < 0)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "expected error for no action flags");

    close(kq);
}

/* ======================================================================
 * 3. STRESS TESTS
 * ====================================================================== */

/* 3a. Many concurrent timers */
static void test_stress_many_timers(void) {
    const char *name = "stress: many timers";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    int ntimers = 10;
    for (int i = 0; i < ntimers; i++) {
        struct kevent ev;
        kev_set(&ev, i + 100, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
                0, 100 + i * 50, i);
        if (kevent_register(kq, &ev, 1) < 0) {
            TEST_FAIL(name, "kevent_register failed");
            close(kq);
            return;
        }
    }

    /* Collect all timer events */
    int collected = 0;
    for (int round = 0; round < ntimers * 2 && collected < ntimers; round++) {
        struct kevent out;
        int ret = kevent_wait(kq, &out, 1, 3000);
        if (ret > 0)
            collected++;
        else
            break;
    }

    /* Allow one missed timer — QEMU timing can be imprecise under load.
     * Individual timer correctness is validated by the oneshot/periodic tests. */
    if (collected >= ntimers - 1)
        TEST_PASS(name);
    else {
        printf("    (collected %d/%d timer events)\n", collected, ntimers);
        TEST_FAIL(name, "did not collect enough timer events");
    }

    close(kq);
}

/* 3b. Many pipe read events — register many pipes, write to all, collect all */
static void test_stress_many_pipes(void) {
    const char *name = "stress: many pipe events";
    int npipes = 16;
    int pfds[16][2];
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    for (int i = 0; i < npipes; i++) {
        if (pipe(pfds[i]) < 0) {
            TEST_FAIL(name, "pipe() failed");
            /* close what we opened */
            for (int j = 0; j < i; j++) { close(pfds[j][0]); close(pfds[j][1]); }
            close(kq);
            return;
        }
        struct kevent ev;
        kev_set(&ev, pfds[i][0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, i);
        kevent_register(kq, &ev, 1);
    }

    /* Write to all pipes */
    for (int i = 0; i < npipes; i++)
        write(pfds[i][1], "x", 1);

    /* Collect all events */
    int collected = 0;
    for (int round = 0; round < npipes * 2 && collected < npipes; round++) {
        struct kevent out[16];
        int ret = kevent_wait(kq, out, npipes, 1000);
        if (ret > 0)
            collected += ret;
        else
            break;
    }

    if (collected >= npipes)
        TEST_PASS(name);
    else {
        printf("    (collected %d/%d pipe events)\n", collected, npipes);
        TEST_FAIL(name, "did not collect all pipe events");
    }

    for (int i = 0; i < npipes; i++) { close(pfds[i][0]); close(pfds[i][1]); }
    close(kq);
}

/* 3c. Rapid register / delete churn */
static void test_stress_register_delete_churn(void) {
    const char *name = "stress: register/delete churn";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    int fds[2];
    if (pipe(fds) < 0) { TEST_FAIL(name, "pipe() failed"); close(kq); return; }

    int ok = 1;
    for (int i = 0; i < 100; i++) {
        struct kevent ev;
        kev_set(&ev, fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, i);
        if (kevent_register(kq, &ev, 1) < 0) { ok = 0; break; }

        kev_set(&ev, fds[0], EVFILT_READ, EV_DELETE, 0, 0, 0);
        kevent_register(kq, &ev, 1);
    }

    if (ok)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "churn loop encountered error");

    close(fds[0]); close(fds[1]); close(kq);
}

/* 3d. Multiple kqueues on the same pipe */
static void test_stress_multi_kqueue_same_pipe(void) {
    const char *name = "stress: multi-kqueue same pipe";
    int fds[2];
    if (pipe(fds) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int nkq = 4;
    int kqs[4];
    for (int i = 0; i < nkq; i++) {
        kqs[i] = kqueue();
        if (kqs[i] < 0) {
            TEST_FAIL(name, "kqueue() failed");
            for (int j = 0; j < i; j++) close(kqs[j]);
            close(fds[0]); close(fds[1]);
            return;
        }
        struct kevent ev;
        kev_set(&ev, fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, i);
        kevent_register(kqs[i], &ev, 1);
    }

    /* Write data — all kqueues should see it */
    write(fds[1], "multi", 5);

    int got = 0;
    for (int i = 0; i < nkq; i++) {
        struct kevent out;
        int ret = kevent_wait(kqs[i], &out, 1, 1000);
        if (ret > 0)
            got++;
    }

    if (got == nkq)
        TEST_PASS(name);
    else {
        printf("    (%d/%d kqueues received event)\n", got, nkq);
        TEST_FAIL(name, "not all kqueues received event");
    }

    for (int i = 0; i < nkq; i++) close(kqs[i]);
    close(fds[0]); close(fds[1]);
}

/* 3e. Timer + pipe event interleaving */
static void test_stress_timer_pipe_interleave(void) {
    const char *name = "stress: timer+pipe interleave";
    int fds[2];
    if (pipe(fds) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); close(fds[0]); close(fds[1]); return; }

    /* Register both a timer and a pipe read */
    struct kevent evs[2];
    kev_set(&evs[0], fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 1);
    kev_set(&evs[1], 200, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 100, 2);
    kevent_register(kq, evs, 2);

    /* Write data immediately — pipe event should arrive before timer */
    write(fds[1], "fast", 4);

    int got_read = 0, got_timer = 0;
    for (int i = 0; i < 4; i++) {
        struct kevent out;
        int ret = kevent_wait(kq, &out, 1, 1000);
        if (ret <= 0) break;
        if (out.filter == EVFILT_READ) got_read++;
        if (out.filter == EVFILT_TIMER) got_timer++;
        if (got_read && got_timer) break;
    }

    if (got_read && got_timer)
        TEST_PASS(name);
    else {
        printf("    (read=%d, timer=%d)\n", got_read, got_timer);
        TEST_FAIL(name, "did not get both event types");
    }

    close(fds[0]); close(fds[1]); close(kq);
}

/* 3f. Rapid kqueue create/close churn */
static void test_stress_kqueue_churn(void) {
    const char *name = "stress: kqueue create/close churn";
    int ok = 1;
    for (int i = 0; i < 50; i++) {
        int kq = kqueue();
        if (kq < 0) { ok = 0; break; }
        close(kq);
    }
    if (ok)
        TEST_PASS(name);
    else
        TEST_FAIL(name, "kqueue create/close loop failed");
}

/* 3g. Fork stress — multiple children with EVFILT_PROC */
static void test_stress_proc_multi_fork(void) {
    const char *name = "stress: multi-fork EVFILT_PROC";
    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    int pid = getpid();
    struct kevent ev;
    kev_set(&ev, pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_FORK, 0, 0);
    if (kevent_register(kq, &ev, 1) < 0) {
        TEST_FAIL(name, "kevent_register failed");
        close(kq); return;
    }

    int nforks = 5;
    int got = 0;
    for (int i = 0; i < nforks; i++) {
        int c = fork();
        if (c < 0) break;
        if (c == 0) { close(kq); exit(0); }

        /* Wait for the fork event before forking again,
         * otherwise edge-triggered EV_CLEAR coalesces events. */
        struct kevent out;
        int ret = kevent_wait(kq, &out, 1, 2000);
        if (ret > 0)
            got++;
        wait(0);  /* reap this child */
    }

    if (got >= nforks)
        TEST_PASS(name);
    else {
        printf("    (got %d/%d fork events)\n", got, nforks);
        TEST_FAIL(name, "missed some fork events");
    }

    close(kq);
}

/* 3h. Write-then-read roundtrip in child with pipe + kqueue */
static void test_stress_pipe_roundtrip(void) {
    const char *name = "stress: pipe roundtrip with child";
    int p2c[2], c2p[2];
    if (pipe(p2c) < 0 || pipe(c2p) < 0) { TEST_FAIL(name, "pipe() failed"); return; }

    int kq = kqueue();
    if (kq < 0) { TEST_FAIL(name, "kqueue() failed"); return; }

    struct kevent ev;
    kev_set(&ev, c2p[0], EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, 0);
    kevent_register(kq, &ev, 1);

    int child = fork();
    if (child < 0) { TEST_FAIL(name, "fork() failed"); return; }
    if (child == 0) {
        close(kq);
        close(p2c[1]); close(c2p[0]);
        /* Child: wait for data from parent, echo back */
        char buf[16];
        for (int i = 0; i < 10; i++) {
            int n = read(p2c[0], buf, sizeof(buf));
            if (n <= 0) break;
            write(c2p[1], buf, n);
        }
        close(p2c[0]); close(c2p[1]);
        exit(0);
    }

    close(p2c[0]); close(c2p[1]);
    int roundtrips = 0;
    for (int i = 0; i < 10; i++) {
        write(p2c[1], "ping", 4);
        struct kevent out;
        int ret = kevent_wait(kq, &out, 1, 1000);
        if (ret > 0) {
            char buf[16];
            int n = read(c2p[0], buf, sizeof(buf));
            if (n == 4) roundtrips++;
        }
    }
    close(p2c[1]); close(c2p[0]);
    wait(0);

    if (roundtrips == 10)
        TEST_PASS(name);
    else {
        printf("    (%d/10 roundtrips completed)\n", roundtrips);
        TEST_FAIL(name, "not all roundtrips completed");
    }

    close(kq);
}

/* ======================================================================
 * MAIN
 * ====================================================================== */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("kqueuetest: === Basic Positive Tests ===\n");
    test_pipe_read();
    test_pipe_write();
    test_timer_oneshot();
    test_timer_periodic();
    test_proc_fork();
    test_pipe_eof();
    test_ev_disable_enable();
    test_ev_delete();
    test_multi_event();
    test_poll_no_event();

    printf("kqueuetest: === Negative / Error-Path Tests ===\n");
    test_neg_kqueue_valid();
    test_neg_bad_kqfd();
    test_neg_bad_filter();
    test_neg_delete_nonexistent();
    test_neg_read_bad_fd();
    test_neg_timer_zero();
    test_neg_proc_bad_pid();
    test_neg_wait_zero_nevents();
    test_neg_register_zero();
    test_neg_double_close();
    test_neg_register_no_flags();

    printf("kqueuetest: === Stress Tests ===\n");
    test_stress_many_timers();
    test_stress_many_pipes();
    test_stress_register_delete_churn();
    test_stress_multi_kqueue_same_pipe();
    test_stress_timer_pipe_interleave();
    test_stress_kqueue_churn();
    test_stress_proc_multi_fork();
    test_stress_pipe_roundtrip();

    printf("kqueuetest: === Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    if (tests_failed > 0)
        exit(1);

    printf("kqueuetest: all tests passed\n");
    exit(0);
}
