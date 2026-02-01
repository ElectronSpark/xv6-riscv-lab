/**
 * @file vforktest.c
 * @brief Test vfork() implementation
 *
 * Tests that:
 * 1. vfork() creates a child that shares address space with parent
 * 2. Parent blocks until child calls exec or exit
 * 3. Child can modify parent's stack (since they share address space)
 */

#include "user.h"

volatile int shared_var = 0;

void test_vforkexit(void) {
    printf("=== Test 1: vfork with exit ===\n");

    int before = shared_var;
    printf("Before vfork: shared_var = %d\n", before);

    int pid = vfork();
    if (pid < 0) {
        printf("FAIL: vfork failed\n");
        exit(1);
    }

    if (pid == 0) {
        // Child: modify shared variable, then exit
        printf("Child: modifying shared_var\n");
        shared_var = 42;
        printf("Child: shared_var = %d, calling exit\n", shared_var);
        exit(0);
    }

    // Parent: should see child's modification
    printf("Parent resumed: shared_var = %d\n", shared_var);
    if (shared_var == 42) {
        printf("PASS: Parent sees child's modification\n");
    } else {
        printf("FAIL: shared_var should be 42, got %d\n", shared_var);
        exit(1);
    }

    // Wait for child
    int status;
    wait(&status);
    printf("Child exited with status %d\n", status);
    printf("Test 1 passed!\n\n");
}

void test_vfork_exec(void) {
    printf("=== Test 2: vfork with exec ===\n");

    shared_var = 100;
    printf("Before vfork: shared_var = %d\n", shared_var);

    int pid = vfork();
    if (pid < 0) {
        printf("FAIL: vfork failed\n");
        exit(1);
    }

    if (pid == 0) {
        // Child: modify variable, then exec
        shared_var = 200;
        printf("Child: shared_var = %d, calling exec echo\n", shared_var);
        char *argv[] = {"echo", "Child exec'd successfully", 0};
        exec("echo", argv);
        printf("FAIL: exec failed\n");
        exit(1);
    }

    // Parent: should see child's modification before exec
    printf("Parent resumed: shared_var = %d\n", shared_var);
    if (shared_var == 200) {
        printf("PASS: Parent sees child's modification before exec\n");
    } else {
        printf("FAIL: shared_var should be 200, got %d\n", shared_var);
        exit(1);
    }

    // Wait for child
    int status;
    wait(&status);
    printf("Child exited with status %d\n", status);
    printf("Test 2 passed!\n\n");
}

void test_vfork_ordering(void) {
    printf("=== Test 3: vfork parent blocks until child finishes ===\n");

    volatile int sequence = 0;

    int pid = vfork();
    if (pid < 0) {
        printf("FAIL: vfork failed\n");
        exit(1);
    }

    if (pid == 0) {
        // Child runs first
        sequence = 1;
        printf("Child: set sequence = %d\n", sequence);
        exit(0);
    }

    // Parent: sequence should already be 1 because parent was blocked
    printf("Parent: sequence = %d\n", sequence);
    if (sequence == 1) {
        printf("PASS: Parent correctly blocked until child finished\n");
    } else {
        printf("FAIL: sequence should be 1, got %d (parent ran before child finished)\n", sequence);
        exit(1);
    }

    wait(0);
    printf("Test 3 passed!\n\n");
}

int main(int argc, char *argv[]) {
    printf("vforktest: starting\n\n");

    test_vforkexit();
    test_vfork_exec();
    test_vfork_ordering();

    printf("All vfork tests passed!\n");
    exit(0);
}
