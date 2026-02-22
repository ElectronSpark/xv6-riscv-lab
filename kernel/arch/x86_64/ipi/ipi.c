#include "types.h"
#include "param.h"
#include "string.h"
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
    (void)trampoline;
    if (hartid >= NCPU) {
        hartid = 0;
    }
    __x86_tp = (uint64)&cpus[hartid];
    cpu_active_mask |= (1UL << hartid);
}

cpumask_t get_cpu_active_mask(void) { return cpu_active_mask; }
