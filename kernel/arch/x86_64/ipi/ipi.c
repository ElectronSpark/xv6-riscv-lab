#include "types.h"
#include "param.h"
#include "string.h"
#include "x86.h"
#include "memlayout.h"
#include "seg.h"
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

    uint64 tp;
    if (!trampoline) {
        /* Early boot: before high shared mappings, use identity VA. */
        tp = (uint64)&cpus[hartid];
    } else {
        /*
         * After arch_vm_init(), switch to high shared alias so mycpu()
         * remains valid in both kernel and user page tables.
         */
        uint64 cpus_base = PGROUNDDOWN((uint64)cpus);
        uint64 cpu_off = (uint64)&cpus[hartid] - cpus_base;
        tp = TRAMPOLINE_CPULOCAL + cpu_off;
    }

    /* Keep the global in sync (for legacy code), but the authoritative
     * per-CPU value is in MSR_GS_BASE (read by r_tp()). */
    __x86_tp = tp;

    /*
     * Set kernel GS base to the per-CPU struct so that:
     *   - Kernel code can use GS-relative accesses for per-CPU data
     *   - swapgs on user→kernel transitions restores this value
     */
    wrmsr(MSR_GS_BASE, tp);
    wrmsr(MSR_KERNEL_GS_BASE, 0);  /* user GS starts at 0 */

    __atomic_fetch_or(&cpu_active_mask, (1UL << hartid), __ATOMIC_RELAXED);
}

cpumask_t get_cpu_active_mask(void) { return cpu_active_mask; }
