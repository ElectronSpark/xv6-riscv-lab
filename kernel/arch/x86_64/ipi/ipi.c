#include "types.h"
#include "param.h"
#include "string.h"
#include "x86.h"
#include "memlayout.h"
#include "smp/percpu.h"
#include "smp/ipi.h"

__attribute__((section("cpu_local_sec")))
__attribute__((aligned(4096)))
struct cpu_local cpus[NCPU] = {0};

static cpumask_t cpu_active_mask = 0;
uint64 __x86_tp = 0;

void ipi_init(void) {}

int ipi_send_single(int hartid, int reason) {
    (void)hartid;
    (void)reason;
    return 0;
}

int ipi_send_mask(unsigned long hart_mask, unsigned long hart_mask_base,
                  int reason) {
    (void)hart_mask;
    (void)hart_mask_base;
    (void)reason;
    return 0;
}

int ipi_send_all_but_self(int reason) {
    (void)reason;
    return 0;
}

int ipi_send_all(int reason) {
    (void)reason;
    return 0;
}

void cpus_init(void) { memset(cpus, 0, sizeof(cpus)); }

void mycpu_init(uint64 hartid, bool trampoline) {
    if (hartid >= NCPU) {
        hartid = 0;
    }

    if (!trampoline) {
        /* Early boot: before high shared mappings, use identity VA. */
        __x86_tp = (uint64)&cpus[hartid];
    } else {
        /*
         * After arch_vm_init(), switch to high shared alias so mycpu()
         * remains valid in both kernel and user page tables.
         */
        uint64 cpus_base = PGROUNDDOWN((uint64)cpus);
        uint64 cpu_off = (uint64)&cpus[hartid] - cpus_base;
        __x86_tp = TRAMPOLINE_CPULOCAL + cpu_off;
    }

    cpu_active_mask |= (1UL << hartid);
}

cpumask_t get_cpu_active_mask(void) { return cpu_active_mask; }
