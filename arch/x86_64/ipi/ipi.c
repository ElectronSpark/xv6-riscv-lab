#include "types.h"
#include "param.h"
#include "string.h"
#include "x86.h"
#include "memlayout.h"
#include "seg.h"
#include "printf.h"
#include "defs.h"
#include "lapic.h"
#include "smp/percpu.h"
#include "smp/ipi.h"
#include "proc/sched.h"
#include "proc/rq.h"
#include "proc/thread.h"
#include "bits.h"
#include "errno.h"

__attribute__((section("cpu_local_sec")))
__attribute__((aligned(4096))) struct cpu_local cpus[NCPU] = {0};

static cpumask_t cpu_active_mask = 0;
uint64 __x86_tp = 0;

/** @brief Per-CPU pending IPI bitmask (indexed by CPU id). */
static volatile uint64 ipi_pending[NCPU];

/* ── Internal: wait for LAPIC ICR idle ── */
static inline void lapic_icr_wait(void) {
    while (lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_STATUS)
        asm volatile("pause");
}

/* ── Internal: fire IPI to a single LAPIC ── */
static void lapic_send_ipi_single(int dest_apicid) {
    lapic_icr_wait();
    lapic_write(LAPIC_ICR_HI, (uint32)dest_apicid << 24);
    lapic_write(LAPIC_ICR_LO,
                LAPIC_ICR_FIXED | LAPIC_IPI_VEC | LAPIC_ICR_DEST_NONE);
}

/* ── Internal: broadcast IPI to all-except-self ── */
static void lapic_send_ipi_allbutself(void) {
    lapic_icr_wait();
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO,
                LAPIC_ICR_FIXED | LAPIC_IPI_VEC | LAPIC_ICR_DEST_ALLBUTSELF);
}

/* ── Internal: broadcast IPI to all (including self) ── */
static void lapic_send_ipi_all(void) {
    lapic_icr_wait();
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO,
                LAPIC_ICR_FIXED | LAPIC_IPI_VEC | LAPIC_ICR_DEST_ALL);
}

/* ========================================================================== */
/*  IPI handler — called from trap.c on vector LAPIC_IPI_VEC */
/* ========================================================================== */

/**
 * x86_ipi_handler - drain pending IPI reasons and dispatch.
 *
 * Called from x86_trap_handler() when vector == LAPIC_IPI_VEC.
 * Must be called with interrupts disabled (as is standard for ISR context).
 */
void x86_ipi_handler(void) {
    int cpu = cpuid();
    uint64 pending =
        __atomic_exchange_n(&ipi_pending[cpu], 0, __ATOMIC_ACQ_REL);

    while (pending != 0) {
        int reason = __builtin_ctzll(pending);
        if (reason >= NR_IPI_REASON)
            break;

        pending &= ~(1ULL << reason);

        switch (reason) {
        case IPI_REASON_CRASH:
            SET_CPU_CRASHED();
            panic_msg_lock();
            printf("[Core: %d] Received IPI_REASON_CRASH, halting...\n", cpu);
            printf("backtrace:\n");
            print_backtrace(r_fp(), KERNBASE, PHYSTOP);
            panic_msg_unlock();
            /* Propagate to remaining cores, then halt forever. */
            ipi_send_all_but_self(IPI_REASON_CRASH);
            for (;;)
                asm volatile("cli; hlt");
            break;

        case IPI_REASON_RESCHEDULE:
            rq_flush_wake_list(cpu);
            SET_NEEDS_RESCHED();
            break;

        case IPI_REASON_TLB_FLUSH:
            /*
             * Full TLB flush by reloading CR3.
             * Now that alltraps / syscall_entry no longer switch CR3,
             * kernel page-table changes (e.g. new mappings in PML4[256-511])
             * must be propagated proactively via IPI.
             */
            asm volatile(
                "movq %%cr3, %%rax\n\t"
                "movq %%rax, %%cr3"
                ::: "rax", "memory");
            break;

        case IPI_REASON_CALL_FUNC:
        case IPI_REASON_GENERIC:
        default:
            break;
        }
    }
}

/* ========================================================================== */
/*  Public IPI API */
/* ========================================================================== */

void ipi_init(void) {
    for (int i = 0; i < NCPU; i++)
        __atomic_store_n(&ipi_pending[i], 0, __ATOMIC_RELEASE);
    printf("ipi_init: IPI subsystem initialized (vector 0x%x)\n",
           LAPIC_IPI_VEC);
}

int ipi_send_single(int hartid, int reason) {
    if (hartid < 0 || hartid >= NCPU)
        return -EINVAL;
    if (reason < 0 || reason >= NR_IPI_REASON)
        return -EINVAL;

    __atomic_fetch_or(&ipi_pending[hartid], 1ULL << reason, __ATOMIC_RELEASE);

    /*
     * On QEMU with default -cpu qemu64, APIC IDs equal CPU indices.
     * For real hardware a cpu-to-apicid table would be needed.
     */
    lapic_send_ipi_single(hartid);
    return 0;
}

int ipi_send_mask(unsigned long hart_mask, unsigned long hart_mask_base,
                  int reason) {
    if (reason < 0 || reason >= NR_IPI_REASON)
        return -EINVAL;

    for (int i = 0; i < NCPU; i++) {
        if (hart_mask & (1UL << i)) {
            int cpu = i + (int)hart_mask_base;
            if (cpu >= 0 && cpu < NCPU)
                __atomic_fetch_or(&ipi_pending[cpu], 1ULL << reason,
                                  __ATOMIC_RELEASE);
        }
    }

    for (int i = 0; i < NCPU; i++) {
        if (hart_mask & (1UL << i)) {
            int cpu = i + (int)hart_mask_base;
            if (cpu >= 0 && cpu < NCPU)
                lapic_send_ipi_single(cpu);
        }
    }
    return 0;
}

int ipi_send_all_but_self(int reason) {
    if (reason < 0 || reason >= NR_IPI_REASON)
        return -EINVAL;

    int self = cpuid();
    for (int i = 0; i < NCPU; i++) {
        if (i != self)
            __atomic_fetch_or(&ipi_pending[i], 1ULL << reason,
                              __ATOMIC_RELEASE);
    }
    lapic_send_ipi_allbutself();
    return 0;
}

int ipi_send_all(int reason) {
    if (reason < 0 || reason >= NR_IPI_REASON)
        return -EINVAL;

    for (int i = 0; i < NCPU; i++)
        __atomic_fetch_or(&ipi_pending[i], 1ULL << reason, __ATOMIC_RELEASE);
    lapic_send_ipi_all();
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
    wrmsr(MSR_KERNEL_GS_BASE, 0); /* user GS starts at 0 */

    __atomic_fetch_or(&cpu_active_mask, (1UL << hartid), __ATOMIC_RELAXED);
}

cpumask_t get_cpu_active_mask(void) { return cpu_active_mask; }
