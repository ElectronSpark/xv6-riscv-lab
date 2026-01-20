/**
 * SBI (Supervisor Binary Interface) implementation
 *
 * Provides interface to OpenSBI or other SBI firmware for
 * machine-mode operations.
 *
 * Note: libsbi is for M-mode firmware, not S-mode kernels.
 * S-mode kernels must use ecall to invoke SBI services.
 */

#include "types.h"
#include "param.h"
#include "sbi.h"
#include "printf.h"
#include "percpu.h"

// Extension ID to probe value mapping
static const long sbi_ext_ids[SBI_EXT_ID_COUNT] = {
    [SBI_EXT_ID_BASE] = SBI_EXT_BASE, [SBI_EXT_ID_TIMER] = SBI_EXT_TIMER,
    [SBI_EXT_ID_IPI] = SBI_EXT_IPI,   [SBI_EXT_ID_RFENCE] = SBI_EXT_RFENCE,
    [SBI_EXT_ID_HSM] = SBI_EXT_HSM,   [SBI_EXT_ID_SRST] = SBI_EXT_SRST,
    [SBI_EXT_ID_PMU] = SBI_EXT_PMU,   [SBI_EXT_ID_DBCN] = SBI_EXT_DBCN,
    [SBI_EXT_ID_SUSP] = SBI_EXT_SUSP, [SBI_EXT_ID_CPPC] = SBI_EXT_CPPC,
    [SBI_EXT_ID_NACL] = SBI_EXT_NACL, [SBI_EXT_ID_STA] = SBI_EXT_STA,
};

// Extension ID to optional mapping
static const bool sbi_ext_optional[SBI_EXT_ID_COUNT] = {
    [SBI_EXT_ID_BASE] = false, [SBI_EXT_ID_TIMER] = true,
    [SBI_EXT_ID_IPI] = false,  [SBI_EXT_ID_RFENCE] = false,
    [SBI_EXT_ID_HSM] = false,  [SBI_EXT_ID_SRST] = true,
    [SBI_EXT_ID_PMU] = true,   [SBI_EXT_ID_DBCN] = true,
    [SBI_EXT_ID_SUSP] = true,  [SBI_EXT_ID_CPPC] = true,
    [SBI_EXT_ID_NACL] = true,  [SBI_EXT_ID_STA] = true,
};

// Extension name strings
static const char *sbi_ext_names[SBI_EXT_ID_COUNT] = {
    [SBI_EXT_ID_BASE] = "BASE", [SBI_EXT_ID_TIMER] = "TIMER",
    [SBI_EXT_ID_IPI] = "IPI",   [SBI_EXT_ID_RFENCE] = "RFENCE",
    [SBI_EXT_ID_HSM] = "HSM",   [SBI_EXT_ID_SRST] = "SRST",
    [SBI_EXT_ID_PMU] = "PMU",   [SBI_EXT_ID_DBCN] = "DBCN",
    [SBI_EXT_ID_SUSP] = "SUSP", [SBI_EXT_ID_CPPC] = "CPPC",
    [SBI_EXT_ID_NACL] = "NACL", [SBI_EXT_ID_STA] = "STA",
};

// Array storing extension availability (0 = not available, 1 = available)
static int sbi_ext_available[SBI_EXT_ID_COUNT];

// Base extension functions
long sbi_get_spec_version(void) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_SPEC_VERSION, 0, 0, 0, 0, 0, 0);
    return __SBI_RETVAL(ret);
}

long sbi_get_impl_id(void) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_IMPL_ID, 0, 0, 0, 0, 0, 0);
    return __SBI_RETVAL(ret);
}

long sbi_get_impl_version(void) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_IMPL_VERSION, 0, 0, 0, 0, 0, 0);
    return __SBI_RETVAL(ret);
}

long sbi_probe_extension(long extid) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_PROBE_EXT, extid, 0, 0, 0, 0, 0);
    return __SBI_RETVAL(ret);
}

long sbi_get_mvendorid(void) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_MVENDORID, 0, 0, 0, 0, 0, 0);
    return __SBI_RETVAL(ret);
}

long sbi_get_marchid(void) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_MARCHID, 0, 0, 0, 0, 0, 0);
    return __SBI_RETVAL(ret);
}

long sbi_get_mimpid(void) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_MIMPID, 0, 0, 0, 0, 0, 0);
    return __SBI_RETVAL(ret);
}

// Timer extension
void sbi_set_timer(uint64 stime_value) {
    sbi_ecall(SBI_EXT_TIMER, SBI_TIMER_SET_TIMER, stime_value, 0, 0, 0, 0, 0);
}

// IPI extension
long sbi_send_ipi(unsigned long hart_mask, unsigned long hart_mask_base) {
    struct sbiret ret = sbi_ecall(SBI_EXT_IPI, SBI_IPI_SEND_IPI, hart_mask,
                                  hart_mask_base, 0, 0, 0, 0);
    return __SBI_ERRNO(ret);
}

// Remote Fence extension
void sbi_remote_hfence_i(unsigned long hart_mask,
                         unsigned long hart_mask_base) {
    sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_I, hart_mask,
              hart_mask_base, 0, 0, 0, 0);
}

long sbi_remote_hfence_vma(unsigned long hart_mask,
                           unsigned long hart_mask_base,
                           unsigned long start_addr, unsigned long size) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_VMA, hart_mask,
                  hart_mask_base, start_addr, size, 0, 0);

    return __SBI_ERRNO(ret);
}

long sbi_remote_hfence_vma_asid(unsigned long hart_mask,
                                unsigned long hart_mask_base,
                                unsigned long start_addr, unsigned long size,
                                unsigned long asid) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_VMA_ASID, hart_mask,
                  hart_mask_base, start_addr, size, asid, 0);
    return __SBI_ERRNO(ret);
}

long sbi_remote_hfence_gvma_vmid(unsigned long hart_mask,
                                 unsigned long hart_mask_base,
                                 unsigned long start_addr, unsigned long size,
                                 unsigned long vmid) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_GVMA_VMID, hart_mask,
                  hart_mask_base, start_addr, size, vmid, 0);
    return __SBI_ERRNO(ret);
}

long sbi_remote_hfence_gvma(unsigned long hart_mask,
                            unsigned long hart_mask_base,
                            unsigned long start_addr, unsigned long size) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_GVMA, hart_mask,
                  hart_mask_base, start_addr, size, 0, 0);
    return __SBI_ERRNO(ret);
}

long sbi_remote_hfence_vvma_asid(unsigned long hart_mask,
                                 unsigned long hart_mask_base,
                                 unsigned long start_addr, unsigned long size,
                                 unsigned long asid) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_VVMA_ASID, hart_mask,
                  hart_mask_base, start_addr, size, asid, 0);
    return __SBI_ERRNO(ret);
}

long sbi_remote_hfence_vvma(unsigned long hart_mask,
                            unsigned long hart_mask_base,
                            unsigned long start_addr, unsigned long size) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_HFENCE_VVMA, hart_mask,
                  hart_mask_base, start_addr, size, 0, 0);
    return __SBI_ERRNO(ret);
}

// HSM (Hart State Management) extension functions
long sbi_hart_start(unsigned long hartid, unsigned long start_addr,
                    unsigned long opaque) {
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_START, hartid,
                                  start_addr, opaque, 0, 0, 0);
    return __SBI_ERRNO(ret);
}

long sbi_hart_stop(void) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_STOP, 0, 0, 0, 0, 0, 0);
    return __SBI_ERRNO(ret);
}

long sbi_hart_get_status(unsigned long hartid) {
    struct sbiret ret =
        sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_GET_STATUS, hartid, 0, 0, 0, 0, 0);
    return __SBI_RETVAL(ret);
}

long sbi_hart_suspend(uint32 suspend_type, unsigned long resume_addr,
                      unsigned long opaque) {
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_SUSPEND,
                                  suspend_type, resume_addr, opaque, 0, 0, 0);
    return __SBI_ERRNO(ret);
}

// System Reset extension
void sbi_system_reset(uint32 reset_type, uint32 reset_reason) {
    sbi_ecall(SBI_EXT_SRST, SBI_SRST_RESET, reset_type, reset_reason, 0, 0, 0,
              0);
    // Should not return
    for (;;)
        asm volatile("wfi");
}

void sbi_shutdown(void) {
    sbi_system_reset(SBI_SRST_TYPE_SHUTDOWN, SBI_SRST_REASON_NONE);
}

void sbi_reboot(void) {
    sbi_system_reset(SBI_SRST_TYPE_COLD_REBOOT, SBI_SRST_REASON_NONE);
}

// Extension probing
void sbi_probe_extensions(void) {
    printf("SBI extensions:\n");
    for (int i = 0; i < SBI_EXT_ID_COUNT; i++) {
        long result = sbi_probe_extension(sbi_ext_ids[i]);
        sbi_ext_available[i] = (result > 0) ? 1 : 0;
        printf("  %s: %s%s\n", sbi_ext_names[i],
               sbi_ext_available[i] ? "AVAILABLE" : "UNSUPPORTED",
               sbi_ext_optional[i] ? " (OPTIONAL)" : "");
        assert(sbi_ext_available[i] || sbi_ext_optional[i],
               "Required SBI extension %s not available!", sbi_ext_names[i]);
    }
}

int sbi_ext_is_available(enum sbi_ext_id ext_id) {
    if (ext_id < 0 || ext_id >= SBI_EXT_ID_COUNT)
        return 0;
    return sbi_ext_available[ext_id];
}

const char *sbi_ext_name(enum sbi_ext_id ext_id) {
    if (ext_id < 0 || ext_id >= SBI_EXT_ID_COUNT)
        return "UNKNOWN";
    return sbi_ext_names[ext_id];
}

// Convenience functions
const char *sbi_error_str(long error) {
    switch (error) {
    case SBI_SUCCESS:
        return "success";
    case SBI_ERR_FAILED:
        return "failed";
    case SBI_ERR_NOT_SUPPORTED:
        return "not supported";
    case SBI_ERR_INVALID_PARAM:
        return "invalid parameter";
    case SBI_ERR_DENIED:
        return "denied";
    case SBI_ERR_INVALID_ADDRESS:
        return "invalid address";
    case SBI_ERR_ALREADY_AVAILABLE:
        return "already available";
    case SBI_ERR_ALREADY_STARTED:
        return "already started";
    case SBI_ERR_ALREADY_STOPPED:
        return "already stopped";
    default:
        return "unknown error";
    }
}

const char *sbi_hart_state_str(long state) {
    switch (state) {
    case SBI_HSM_STATE_STARTED:
        return "started";
    case SBI_HSM_STATE_STOPPED:
        return "stopped";
    case SBI_HSM_STATE_START_PENDING:
        return "start pending";
    case SBI_HSM_STATE_STOP_PENDING:
        return "stop pending";
    case SBI_HSM_STATE_SUSPENDED:
        return "suspended";
    case SBI_HSM_STATE_SUSPEND_PENDING:
        return "suspend pending";
    case SBI_HSM_STATE_RESUME_PENDING:
        return "resume pending";
    default:
        return "unknown state";
    }
}

void sbi_print_version(void) {
    long spec_ver = sbi_get_spec_version();
    long impl_id = sbi_get_impl_id();
    long impl_ver = sbi_get_impl_version();

    int major = (spec_ver >> 24) & 0x7f;
    int minor = spec_ver & 0xffffff;

    const char *impl_name;
    switch (impl_id) {
    case 0:
        impl_name = "Berkeley Boot Loader (BBL)";
        break;
    case 1:
        impl_name = "OpenSBI";
        break;
    case 2:
        impl_name = "Xvisor";
        break;
    case 3:
        impl_name = "KVM";
        break;
    case 4:
        impl_name = "RustSBI";
        break;
    case 5:
        impl_name = "Diosix";
        break;
    default:
        impl_name = "Unknown";
        break;
    }

    printf("SBI specification v%d.%d\n", major, minor);
    printf("SBI implementation: %s (id=%ld, version=0x%lx)\n", impl_name,
           impl_id, impl_ver);
}

void sbi_start_secondary_harts(unsigned long start_addr) {
    int boot_hart = cpuid();

    printf("Starting secondary harts...\n");
    for (int i = 0; i < NCPU; i++) {
        if (i == boot_hart)
            continue;

        long status = sbi_hart_get_status(i);
        if (status == SBI_HSM_STATE_STOPPED) {
            long ret = sbi_hart_start(i, start_addr, 0);
            if (ret != SBI_SUCCESS && ret != SBI_ERR_ALREADY_AVAILABLE &&
                ret != SBI_ERR_ALREADY_STARTED) {
                printf("hart %d: failed to start (%s)\n", i,
                       sbi_error_str(ret));
            }
        }
    }
}

/**
 * @brief Output a character via SBI console
 *
 * Used for early boot console output before UART is initialized.
 * Uses legacy SBI console putchar which is widely supported.
 *
 * @param c Character to output
 * @note Cannot use DBCN extension here due to circular dependency with printf
 */
void sbi_console_putchar(int c) {
    // Try DBCN extension first (modern SBI)
    // Note: We can't check sbi_ext_available here because that requires
    // sbi_probe_extensions() which uses printf, causing a circular dependency.
    // Use legacy console putchar which is widely supported.
    sbi_ecall(SBI_EXT_LEGACY_CONSOLE_PUTCHAR, 0, c, 0, 0, 0, 0, 0);
}

/**
 * @brief Output a string via SBI console
 * @param s Null-terminated string to output
 */
void sbi_console_puts(const char *s) {
    while (*s) {
        sbi_console_putchar(*s++);
    }
}

/**
 * @brief Read a character from SBI console
 * @return Character read, or -1 if no character available
 */
int sbi_console_getchar(void) {
    struct sbiret ret = sbi_ecall(SBI_EXT_LEGACY_CONSOLE_GETCHAR, 0, 0, 0, 0, 0, 0, 0);
    if (ret.error < 0)
        return -1;
    return (int)ret.error;  // Legacy returns char in error field
}
