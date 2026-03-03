/**
 * @file platform_riscv.c
 * @brief RISC-V platform parser implementation.
 *
 * Wraps FDT and SBI calls behind the architecture-independent
 * platform interface for start_kernel.c.
 */

#include "types.h"
#include "platform.h"
#include "defs.h"
#include "printf.h"
#include "dev/fdt.h"
#include "sbi.h"
#include "proc/sched.h"
#include "lock/rcu.h"
#include <smp/percpu.h>

uint64 platform_default_mem_base(void)
{
    return 0x80000000; /* QEMU virt machine default */
}

int platform_early_memory(void *boot_data, uint64 *base_out, uint64 *size_out)
{
    return fdt_early_scan_memory(boot_data, base_out, size_out);
}

int platform_init(void *boot_data)
{
    return fdt_init(boot_data);
}

void platform_apply_config(void)
{
    fdt_apply_platform_config();
}

void platform_probe_extensions(void)
{
    sbi_probe_extensions();
}

void platform_print_mem_summary(void)
{
    /* RISC-V: use printf (available after printfinit) */
    if (platform.mem_count > 0) {
        uint64 first_base = platform.mem[0].base;
        uint64 first_end = platform.mem[0].base + platform.mem[0].size;
        printf("boot memory: regions=%d reserved=%d total=%ld MB first=[0x%lx-0x%lx)\n",
               platform.mem_count, platform.reserved_count,
               platform.total_mem / (1024 * 1024), first_base, first_end);
    }
}

void platform_post_vm_init(void)
{
    arch_vm_init_hart(); /* turn on paging */
    printf("paging enabled\n");
}

void platform_start_secondary_cpus(uint64 entry)
{
    sbi_start_secondary_harts((unsigned long)entry);
}

void platform_secondary_cpu_init(void)
{
    arch_vm_init_hart(); /* turn on paging */
}

void platform_start_per_cpu_services(int cpu)
{
    printf("hart %d initialized. intr_sp: %p\n", cpu,
           (void *)mycpu()->intr_sp);
    rcu_kthread_start_cpu(cpu);
}

void platform_late_device_init(void)
{
    sched_timer_init();
    pci_init();
    x1_emac_init();
    x1_sdhci_init();
}

void platform_boot_mark(const char *msg)
{
    /* No-op — RISC-V has no debugcon; use printf for diagnostics. */
    (void)msg;
}
