#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmocka.h>

#include "types.h"

static int __panic_panicked = 0;

void __panic_start(void)
{
    __panic_panicked = 1;
}

void __panic_end(void) __attribute__((noreturn));
void __panic_end(void)
{
    fail_msg("kernel panic reached in host test (see preceding log)");
    abort();
}

int panic_state(void)
{
    return __panic_panicked;
}

void panic_disable_bt(void)
{
    /* backtrace printing is not available in host tests */
}

void printfinit(void)
{
    /* serial output init not required for host tests */
}

// syscall argument helpers (host-test stubs)
// Some kernel sources (e.g. syscalls embedded in otherwise-testable code)
// reference argint(). Unit tests don't run the syscall path, so a simple
// stub is sufficient to satisfy the linker.
void argint(int n, int *ip)
{
    (void)n;
    if (ip) {
        *ip = 0;
    }
}
