/*
 * CPU/interrupt wrappers for unit tests
 */
#include "types.h"

static int g_noff = 0;

int __wrap_cpuid(void) {
    return 0; // Always CPU 0 in tests
}

void __wrap_push_off(void) {
    g_noff++;
}

void __wrap_pop_off(void) {
    if (g_noff > 0) {
        g_noff--;
    }
}
