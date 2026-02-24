/*
 * fptest.c — Floating-point lazy switching test program
 *
 * Validates:
 *  1. Basic FP arithmetic in user space
 *  2. FP state preservation across fork()
 *  3. FP state isolation between concurrent processes
 *  4. Extended FP operations (single and double precision)
 *  5. FP across nested forks (grandchild)
 *
 * Compiled against musl libc so real FP instructions are emitted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* Avoid libm — use raw FP ops + epsilon comparison.
   volatile prevents the compiler from constant-folding. */

static int approx_eq(double a, double b)
{
    double diff = a - b;
    if (diff < 0)
        diff = -diff;
    return diff < 0.000001;
}

/* Test 1: basic double-precision arithmetic */
static void test_basic(void)
{
    printf("fptest: basic arithmetic... ");

    volatile double a = 3.14;
    volatile double b = 2.72;
    volatile double sum  = a + b;
    volatile double diff = a - b;
    volatile double prod = a * b;
    volatile double quot = a / b;

    if (!approx_eq(sum, 5.86)) {
        printf("FAIL sum\n");
        exit(1);
    }
    if (!approx_eq(diff, 0.42)) {
        printf("FAIL diff\n");
        exit(1);
    }
    if (!approx_eq(prod, 8.5408)) {
        printf("FAIL prod\n");
        exit(1);
    }
    if (!approx_eq(quot, 1.154411)) {
        printf("FAIL quot\n");
        exit(1);
    }
    printf("ok\n");
}

/* Test 2: FP state preserved across fork() */
static void test_fork(void)
{
    printf("fptest: fork preservation... ");

    volatile double val   = 1.23456789;
    volatile double accum = 0.0;

    /* Dirty the FP registers */
    for (int i = 0; i < 10; i++)
        accum = accum + val;

    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL fork\n");
        exit(1);
    }

    if (pid == 0) {
        /* Child: verify inherited FP state */
        if (!approx_eq(accum, 12.3456789)) {
            printf("FAIL child accum\n");
            exit(1);
        }
        /* More FP in child */
        volatile double c = accum * 2.0;
        if (!approx_eq(c, 24.6913578)) {
            printf("FAIL child c\n");
            exit(1);
        }
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL child exited %d\n", status);
        exit(1);
    }

    /* Parent: FP state must be unchanged */
    if (!approx_eq(accum, 12.3456789)) {
        printf("FAIL parent accum\n");
        exit(1);
    }
    printf("ok\n");
}

/* Test 3: concurrent processes with different FP states */
static void test_concurrent(void)
{
    printf("fptest: concurrent isolation... ");

    int nprocs = 4;

    for (int i = 0; i < nprocs; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("FAIL fork %d\n", i);
            exit(1);
        }
        if (pid == 0) {
            /* Each child uses a distinct constant */
            volatile double base  = (double)(i + 1) * 1.111111;
            volatile double accum = 0.0;

            /* Heavy FP work → forces context switches between children */
            for (int j = 0; j < 100000; j++)
                accum = accum + base;

            volatile double expected = base * 100000.0;
            if (!approx_eq(accum, expected)) {
                printf("FAIL child %d\n", i);
                exit(1);
            }
            exit(0);
        }
    }

    /* Wait for all */
    for (int i = 0; i < nprocs; i++) {
        int status;
        wait(&status);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("FAIL child exited %d\n", status);
            exit(1);
        }
    }
    printf("ok\n");
}

/* Test 4: mix of double and single precision */
static void test_operations(void)
{
    printf("fptest: extended operations... ");

    volatile double a = 100.0;
    volatile double b = 3.0;
    volatile double c;

    /* Division */
    c = a / b;
    if (!approx_eq(c, 33.333333)) {
        printf("FAIL div\n");
        exit(1);
    }

    /* Negate */
    c = -a;
    if (!approx_eq(c, -100.0)) {
        printf("FAIL neg\n");
        exit(1);
    }

    /* Cascade: ((a * b) + a) / b */
    c = ((a * b) + a) / b;
    if (!approx_eq(c, 133.333333)) {
        printf("FAIL cascade\n");
        exit(1);
    }

    /* Single precision */
    volatile float fa = 1.5f;
    volatile float fb = 2.5f;
    volatile float fc = fa * fb;
    if (fc < 3.74f || fc > 3.76f) {
        printf("FAIL float mul\n");
        exit(1);
    }

    printf("ok\n");
}

/* Test 5: FP across nested forks (grandchild) */
static void test_grandchild(void)
{
    printf("fptest: grandchild FP... ");

    volatile double val = 42.42;

    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL fork1\n");
        exit(1);
    }
    if (pid == 0) {
        volatile double child_val = val + 10.0;

        pid_t pid2 = fork();
        if (pid2 < 0) {
            printf("FAIL fork2\n");
            exit(1);
        }
        if (pid2 == 0) {
            /* Grandchild */
            if (!approx_eq(child_val, 52.42)) {
                printf("FAIL grandchild val\n");
                exit(1);
            }
            exit(0);
        }
        int status;
        waitpid(pid2, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("FAIL grandchild exited %d\n", status);
            exit(1);
        }
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL child exited %d\n", status);
        exit(1);
    }
    printf("ok\n");
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("fptest: starting floating-point tests\n");

    test_basic();
    test_fork();
    test_concurrent();
    test_operations();
    test_grandchild();

    printf("fptest: all tests passed\n");
    return 0;
}
