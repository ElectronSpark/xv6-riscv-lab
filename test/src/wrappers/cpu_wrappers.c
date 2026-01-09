/*
 * RISC-V specific function stubs for host unit tests.
 * These are normally inline assembly functions in riscv.h but are excluded
 * when ON_HOST_OS is defined.
 *
 * Note: The __wrap_* functions (cpuid, push_off, pop_off, mycpu, etc.)
 * are defined in their respective wrapper files (proc_wrappers.c,
 * spinlock_wrappers.c, etc.) - this file only provides the low-level
 * RISC-V instruction stubs.
 */
#include "types.h"

static int g_intr_enabled = 0;
static uint64 g_tp_value = 0;

uint64 r_tp(void) {
    return g_tp_value;
}

void w_tp(uint64 x) {
    g_tp_value = x;
}

int intr_get(void) {
    return g_intr_enabled;
}

void intr_on(void) {
    g_intr_enabled = 1;
}

void intr_off(void) {
    g_intr_enabled = 0;
}
