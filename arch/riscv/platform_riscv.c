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
#include "dev/x1_i2c.h"
#include "sbi.h"
#include "proc/sched.h"
#include "lock/rcu.h"
#include <smp/percpu.h>

/*
 * SPM8821 PMIC power control register (at I2C register address 0x7E).
 * Bit 1 (0x02): SW_RESET   — triggers a full system reboot.
 * Bit 2 (0x04): SW_SHUTDOWN — triggers a full power-off.
 */
#define SPM8821_PWR_CTRL2           0x7e
#define SPM8821_SW_RESET_BIT        0x02
#define SPM8821_SW_SHUTDOWN_BIT     0x04

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
    x1_i2c_init();
    x1_emac_init();
    x1_sdhci_init();
}

void platform_boot_mark(const char *msg)
{
    /* No-op — RISC-V has no debugcon; use printf for diagnostics. */
    (void)msg;
}

/**
 * Attempt power-off via the SPM8821 PMIC over I2C (reg 0x7e bit 2).
 * Returns 0 on success (caller should WFI-loop), -1 if unavailable.
 */
static int pmic_power_action(uint8 bit)
{
    if (!platform.has_pmic_power)
        return -1;

    int bus  = (int)platform.pmic_power_bus;
    uint8 addr = (uint8)platform.pmic_power_addr;

    if (!x1_i2c_is_ready(bus)) {
        printf("pmic: I2C bus %d not ready\n", bus);
        return -1;
    }

    uint8 val = 0;
    int rc = x1_i2c_read_reg8(bus, addr, SPM8821_PWR_CTRL2, &val);
    if (rc < 0) {
        printf("pmic: read reg 0x%x failed (%d)\n", SPM8821_PWR_CTRL2, rc);
        return -1;
    }

    printf("pmic: PWR_CTRL2=0x%x -> 0x%x\n", val, val | bit);
    rc = x1_i2c_write_reg8(bus, addr, SPM8821_PWR_CTRL2, val | bit);
    if (rc < 0) {
        printf("pmic: write reg 0x%x failed (%d)\n", SPM8821_PWR_CTRL2, rc);
        return -1;
    }
    return 0;
}

void platform_shutdown(void)
{
    printf("System shutting down...\n");

    if (sbi_ext_is_available(SBI_EXT_ID_SRST)) {
        sbi_shutdown();
    } else if (pmic_power_action(SPM8821_SW_SHUTDOWN_BIT) == 0) {
        printf("pmic: shutdown requested\n");
    } else {
        /* Fallback: legacy SBI shutdown (EID 0x08) */
        printf("SRST unavailable, using legacy SBI shutdown\n");
        sbi_ecall(SBI_EXT_LEGACY_SHUTDOWN, 0, 0, 0, 0, 0, 0, 0);
    }

    /* Should not return; spin just in case */
    for (;;)
        asm volatile("wfi");
}

void platform_reboot(void)
{
    printf("System rebooting...\n");

    if (sbi_ext_is_available(SBI_EXT_ID_SRST)) {
        sbi_reboot();
    } else if (pmic_power_action(SPM8821_SW_RESET_BIT) == 0) {
        printf("pmic: reboot requested\n");
    } else {
        /* No SRST and no PMIC — shut down instead */
        printf("cannot reboot; shutting down\n");
        sbi_ecall(SBI_EXT_LEGACY_SHUTDOWN, 0, 0, 0, 0, 0, 0, 0);
    }

    /* Should not return; spin just in case */
    for (;;)
        asm volatile("wfi");
}
