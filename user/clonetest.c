// Test program for clone() syscall with shared resources (thread-like behavior)
#include "kernel/inc/types.h"
#include "kernel/inc/clone_flags.h"
#include "user/user.h"

#define STACK_SIZE (4096 * 4)  // 16KB - must be >= USERSTACK_MINSZ

// Shared variable to verify CLONE_VM works
volatile int shared_counter = 0;
volatile int child_done = 0;

// Child thread entry point
void child_func(void) {
    // First thing - confirm we're running
    write(1, "CHILD\n", 6);
    
    printf("clonetest: child started, pid=%d\n", getpid());
    
    // Increment shared counter to prove memory is shared
    for (int i = 0; i < 100; i++) {
        shared_counter++;
    }
    
    printf("clonetest: child incremented counter to %d\n", shared_counter);
    
    // Signal that we're done
    child_done = 1;
    
    // Exit the thread
    exit(0);
}

// Test 1: Simple fork behavior (no sharing)
void test_fork(void) {
    printf("\n=== Test 1: fork() (no sharing) ===\n");
    
    shared_counter = 0;
    
    int pid = fork();
    if (pid < 0) {
        printf("clonetest: fork failed\n");
        exit(1);
    }
    
    if (pid == 0) {
        // Child
        shared_counter = 42;
        printf("clonetest: child set counter to %d\n", shared_counter);
        exit(0);
    } else {
        // Parent
        wait(0);
        printf("clonetest: parent sees counter = %d (should be 0, not 42)\n", shared_counter);
        if (shared_counter == 0) {
            printf("clonetest: PASSED - memory not shared in fork\n");
        } else {
            printf("clonetest: FAILED - memory unexpectedly shared\n");
            exit(1);
        }
    }
}

// Test 2: clone with CLONE_VM (shared memory)
void test_clone_vm(void) {
    printf("\n=== Test 2: clone() with CLONE_VM ===\n");
    
    shared_counter = 0;
    child_done = 0;
    
    // Allocate stack for child
    char *stack = sbrk(STACK_SIZE);
    if (stack == (char*)-1) {
        printf("clonetest: sbrk failed\n");
        exit(1);
    }
    
    // Set up clone args with CLONE_VM
    // Pass stack base - kernel will calculate stack top from base + size
    struct clone_args args = {
        .flags = CLONE_VM | SIGCHLD,
        .stack = (uint64)stack,
        .stack_size = STACK_SIZE,
        .entry = (uint64)child_func,
    };
    
    printf("clonetest: calling clone with CLONE_VM\n");
    printf("clonetest: stack=%p entry=%p\n", (void*)args.stack, (void*)args.entry);
    int pid = clone(&args);
    
    if (pid < 0) {
        printf("clonetest: clone failed with %d\n", pid);
        exit(1);
    }
    
    if (pid == 0) {
        // We shouldn't get here - child should start at child_func
        // But in case clone returns in child:
        child_func();
    } else {
        // Parent - wait for child
        printf("clonetest: parent waiting for child %d\n", pid);
        
        // Spin wait for child to finish (since we share VM, child_done will update)
        while (!child_done) {
            // busy wait
        }
        
        wait(0);
        
        printf("clonetest: parent sees counter = %d (should be 100)\n", shared_counter);
        if (shared_counter == 100) {
            printf("clonetest: PASSED - memory shared via CLONE_VM\n");
        } else {
            printf("clonetest: FAILED - expected 100, got %d\n", shared_counter);
            exit(1);
        }
    }
}

// Test 3: Simple clone with all thread-like flags
void test_clone_thread(void) {
    printf("\n=== Test 3: clone() with thread flags ===\n");
    
    shared_counter = 0;
    child_done = 0;
    
    // Allocate stack for child
    char *stack = sbrk(STACK_SIZE);
    if (stack == (char*)-1) {
        printf("clonetest: sbrk failed\n");
        exit(1);
    }
    
    // Set up clone args with all sharing flags
    // Pass stack base - kernel will calculate stack top from base + size
    struct clone_args args = {
        .flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | SIGCHLD,
        .stack = (uint64)stack,
        .stack_size = STACK_SIZE,
        .entry = (uint64)child_func,
    };
    
    printf("clonetest: calling clone with thread flags\n");
    int pid = clone(&args);
    
    if (pid < 0) {
        printf("clonetest: clone failed with %d\n", pid);
        exit(1);
    }
    
    if (pid == 0) {
        child_func();
    } else {
        // Parent
        while (!child_done) {
            // busy wait
        }
        
        wait(0);
        
        printf("clonetest: parent sees counter = %d\n", shared_counter);
        if (shared_counter == 100) {
            printf("clonetest: PASSED - thread-like clone works\n");
        } else {
            printf("clonetest: FAILED\n");
            exit(1);
        }
    }
}

int main(int argc, char *argv[]) {
    printf("clonetest: starting clone tests\n");
    
    // Test basic fork (should not share memory)
    test_fork();
    
    // Test clone with CLONE_VM (should share memory)
    test_clone_vm();
    
    // Test clone with thread-like flags
    test_clone_thread();
    
    printf("\n=== All clone tests passed! ===\n");
    exit(0);
}
