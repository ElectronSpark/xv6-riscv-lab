/*
 * Panic function wrappers for unit tests
 * Uses --wrap linker feature to intercept panic calls
 */

#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmocka.h>

#include "types.h"

static int __panic_panicked = 0;

void __wrap___panic_start(void)
{
    __panic_panicked = 1;
}

void __wrap___panic_end(void) __attribute__((noreturn));
void __wrap___panic_end(void)
{
    fail_msg("kernel panic reached in host test (see preceding log)");
    abort();
}

int __wrap_panic_state(void)
{
    return __panic_panicked;
}

void __wrap_panic_disable_bt(void)
{
    /* backtrace printing is not available in host tests */
}

void __wrap_printfinit(void)
{
    /* serial output init not required for host tests */
}

// Syscall argument helpers (used by some kernel code)
void __wrap_argint(int n, int *ip)
{
    (void)n;
    if (ip) {
        *ip = 0;
    }
}
