/**
 * @file platform.h
 * @brief Architecture-independent platform initialization interface.
 *
 * Each architecture (RISC-V, x86_64, ...) provides its own implementation
 * that parses the bootloader-provided data (FDT, PVH start_info, multiboot,
 * ACPI, etc.) and populates the shared `platform_info` structure defined in
 * dev/fdt.h.
 *
 * start_kernel.c calls these functions instead of directly referencing
 * architecture-specific FDT / bootloader APIs.
 */

#ifndef __KERNEL_PLATFORM_H
#define __KERNEL_PLATFORM_H

#include "types.h"

/**
 * Return the default physical memory base for this architecture.
 * Used as fallback if bootloader memory parsing fails.
 *   RISC-V: 0x80000000 (QEMU virt), x86_64: 0x00100000 (1 MiB)
 */
uint64 platform_default_mem_base(void);

/**
 * Early, lightweight memory scan from bootloader data.
 * Must not allocate memory.  Populates platform.mem[] / platform.reserved[]
 * and returns the first usable region via base_out/size_out.
 *
 * @param boot_data  Bootloader-provided pointer (FDT blob, PVH start_info, etc.)
 * @param base_out   [out] base address of the first usable region
 * @param size_out   [out] size of the first usable region
 * @return 0 on success, -1 on failure
 */
int platform_early_memory(void *boot_data, uint64 *base_out, uint64 *size_out);

/**
 * Full platform initialization from bootloader data.
 * Called after printf is available.  The RISC-V implementation calls
 * fdt_init() to build the full device tree; x86 installs a fallback
 * region if the early parse didn't populate platform.mem[].
 *
 * @param boot_data  Bootloader-provided pointer
 * @return 0 on success, negative on failure
 */
int platform_init(void *boot_data);

/**
 * Apply platform configuration to kernel globals.
 * RISC-V: sets UART, PLIC, PCIe, VirtIO addresses from FDT;
 *         splits cross-4-GiB memory regions; refines physical memory info.
 * x86_64: currently a no-op (ACPI support would go here).
 */
void platform_apply_config(void);

/**
 * Probe firmware extensions.
 * RISC-V: probes SBI extensions.  x86: no-op.
 */
void platform_probe_extensions(void);

/**
 * Print a summary of the parsed memory regions (for early boot diagnostics).
 * x86 uses debugcon; RISC-V uses printf.
 */
void platform_print_mem_summary(void);

/**
 * Post-VM-init hook.
 * Called after arch_vm_init() sets up kernel page tables.
 * RISC-V: calls arch_vm_init_hart() to activate paging.
 * x86:    no-op (paging was already on from boot assembly).
 */
void platform_post_vm_init(void);

/**
 * Return the number of CPU slots this boot should prepare.
 * This is a runtime value discovered from firmware/CPU topology where
 * available, clamped to MAX_CPUS.  MAX_CPUS remains the compile-time upper
 * bound.
 */
int platform_boot_cpu_limit(void);

/**
 * Let the platform move the current CPU from an early firmware/bootstrap stack
 * to an allocator-backed kernel stack before idle_thread_init() records it.
 */
void platform_prepare_current_cpu_stack(void);

/**
 * Start secondary CPUs.
 * RISC-V: uses SBI HSM to start other harts.
 * x86:    will use APIC INIT/SIPI (stub for now).
 *
 * @param entry  Physical address of the entry point.
 */
void platform_start_secondary_cpus(uint64 entry);

/**
 * Per-CPU platform init after secondary CPU comes online.
 * RISC-V: calls arch_vm_init_hart() to enable paging.
 * x86:    no-op.
 */
void platform_secondary_cpu_init(void);

/**
 * Start per-CPU platform services (e.g. timer, RCU kthread).
 * RISC-V: starts sched_timer and RCU kthread.
 * x86:    stub (APIC timer will go here).
 */
void platform_start_per_cpu_services(int cpu);

/**
 * Start platform-specific device drivers (late init, requires kthreads).
 * RISC-V: starts EMAC, SDHCI drivers.
 * x86:    stub for now.
 */
void platform_late_device_init(void);

/**
 * Emit a boot-progress trace message.
 * x86: writes to debugcon (port 0xE9).
 * RISC-V: no-op (uses printf for diagnostics).
 */
void platform_boot_mark(const char *msg);

/**
 * Paint a simple full-screen boot checkpoint on any firmware framebuffer.
 * Intended for display bring-up diagnostics; platforms without an early
 * framebuffer may implement this as a no-op.
 */
void platform_visual_checkpoint(uint32 color);

/**
 * Shut down the machine (power off).
 * RISC-V: SBI SRST shutdown.
 * x86:    ACPI PM1a control register.
 * Does not return on success.
 */
void platform_shutdown(void);

/**
 * Reboot the machine (cold reset).
 * RISC-V: SBI SRST cold reboot.
 * x86:    keyboard controller reset (PS/2 port 0x64).
 * Does not return on success.
 */
void platform_reboot(void);

#endif /* __KERNEL_PLATFORM_H */
