/**
 * SBI (Supervisor Binary Interface) for RISC-V
 *
 * This module provides an interface to the SBI firmware (e.g., OpenSBI)
 * for operations that require machine-mode privileges.
 */

#ifndef __KERNEL_SBI_H
#define __KERNEL_SBI_H

#include "types.h"
#include "riscv.h"

// SBI return error codes
#define SBI_SUCCESS 0
#define SBI_ERR_FAILED -1
#define SBI_ERR_NOT_SUPPORTED -2
#define SBI_ERR_INVALID_PARAM -3
#define SBI_ERR_DENIED -4
#define SBI_ERR_INVALID_ADDRESS -5
#define SBI_ERR_ALREADY_AVAILABLE -6
#define SBI_ERR_ALREADY_STARTED -7
#define SBI_ERR_ALREADY_STOPPED -8

// SBI extension IDs
#define SBI_EXT_BASE 0x10
#define SBI_EXT_TIMER 0x54494D45
#define SBI_EXT_IPI 0x735049
#define SBI_EXT_RFENCE 0x52464E43
#define SBI_EXT_HSM 0x48534D
#define SBI_EXT_SRST 0x53525354
#define SBI_EXT_PMU 0x504D55
#define SBI_EXT_DBCN 0x4442434E
#define SBI_EXT_SUSP 0x53555350
#define SBI_EXT_CPPC 0x43505043
#define SBI_EXT_NACL 0x4E41434C
#define SBI_EXT_STA 0x535441

// SBI extension indices (for use with sbi_ext_is_available)
enum sbi_ext_id {
    SBI_EXT_ID_BASE = 0,
    SBI_EXT_ID_TIMER,
    SBI_EXT_ID_IPI,
    SBI_EXT_ID_RFENCE,
    SBI_EXT_ID_HSM,
    SBI_EXT_ID_SRST,
    SBI_EXT_ID_PMU,
    SBI_EXT_ID_DBCN,
    SBI_EXT_ID_SUSP,
    SBI_EXT_ID_CPPC,
    SBI_EXT_ID_NACL,
    SBI_EXT_ID_STA,
    SBI_EXT_ID_COUNT // Must be last
};

// SBI Base extension function IDs
#define SBI_BASE_GET_SPEC_VERSION 0
#define SBI_BASE_GET_IMPL_ID 1
#define SBI_BASE_GET_IMPL_VERSION 2
#define SBI_BASE_PROBE_EXT 3
#define SBI_BASE_GET_MVENDORID 4
#define SBI_BASE_GET_MARCHID 5
#define SBI_BASE_GET_MIMPID 6

// SBI TIMER extension function IDs
#define SBI_TIMER_SET_TIMER 0

// SBI IPI extension function IDs
#define SBI_IPI_SEND_IPI 0

// SBI RFENCE extension function IDs
#define SBI_RFENCE_REMOTE_HFENCE_I 0
#define SBI_RFENCE_REMOTE_HFENCE_VMA 1
#define SBI_RFENCE_REMOTE_HFENCE_VMA_ASID 2
#define SBI_RFENCE_REMOTE_HFENCE_GVMA_VMID 3
#define SBI_RFENCE_REMOTE_HFENCE_GVMA 4
#define SBI_RFENCE_REMOTE_HFENCE_VVMA_ASID 5
#define SBI_RFENCE_REMOTE_HFENCE_VVMA 6

// SBI HSM (Hart State Management) function IDs
#define SBI_HSM_HART_START 0
#define SBI_HSM_HART_STOP 1
#define SBI_HSM_HART_GET_STATUS 2
#define SBI_HSM_HART_SUSPEND 3

// SBI HSM hart states
#define SBI_HSM_STATE_STARTED 0
#define SBI_HSM_STATE_STOPPED 1
#define SBI_HSM_STATE_START_PENDING 2
#define SBI_HSM_STATE_STOP_PENDING 3
#define SBI_HSM_STATE_SUSPENDED 4
#define SBI_HSM_STATE_SUSPEND_PENDING 5
#define SBI_HSM_STATE_RESUME_PENDING 6

// SBI SRST (System Reset) function IDs
#define SBI_SRST_RESET 0

// SBI SRST reset types
#define SBI_SRST_TYPE_SHUTDOWN 0
#define SBI_SRST_TYPE_COLD_REBOOT 1
#define SBI_SRST_TYPE_WARM_REBOOT 2

// SBI SRST reset reasons
#define SBI_SRST_REASON_NONE 0
#define SBI_SRST_REASON_SYSFAIL 1

// SBI DBCN (Debug Console) function IDs
#define SBI_DBCN_WRITE 0
#define SBI_DBCN_READ 1
#define SBI_DBCN_WRITE_BYTE 2

// SBI Legacy Console extension (deprecated but widely supported)
#define SBI_EXT_LEGACY_CONSOLE_PUTCHAR 0x01
#define SBI_EXT_LEGACY_CONSOLE_GETCHAR 0x02

#ifndef __ASSEMBLER__

// SBI return value structure
struct sbiret {
    long error;
    long value;
};

// Generic SBI ecall - S-mode kernel uses ecall to invoke SBI services
static inline struct sbiret sbi_ecall(int ext, int fid, unsigned long arg0,
                                      unsigned long arg1, unsigned long arg2,
                                      unsigned long arg3, unsigned long arg4,
                                      unsigned long arg5) {
    struct sbiret ret;
    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a1 asm("a1") = arg1;
    register unsigned long a2 asm("a2") = arg2;
    register unsigned long a3 asm("a3") = arg3;
    register unsigned long a4 asm("a4") = arg4;
    register unsigned long a5 asm("a5") = arg5;
    register unsigned long a6 asm("a6") = fid;
    register unsigned long a7 asm("a7") = ext;
    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");
    ret.error = a0;
    ret.value = a1;
    return ret;
}

// Macro to extract return value or error from sbiret
#define __SBI_RETVAL(__RET)                                                    \
    ({                                                                         \
        long __ret = (__RET).error;                                            \
        if (__ret == SBI_SUCCESS) {                                            \
            __ret = (__RET).value;                                             \
        }                                                                      \
        __ret;                                                                 \
    })

#define __SBI_ERRNO(__RET) ((__RET).error)

// Base extension
long sbi_get_spec_version(void);
long sbi_get_impl_id(void);
long sbi_get_impl_version(void);
long sbi_probe_extension(long extid);
long sbi_get_mvendorid(void);
long sbi_get_marchid(void);
long sbi_get_mimpid(void);

// Timer extension
void sbi_set_timer(uint64 stime_value);

// IPI extension
long sbi_send_ipi(unsigned long hart_mask, unsigned long hart_mask_base);

// Remote Fence extension
void sbi_remote_hfence_i(unsigned long hart_mask, unsigned long hart_mask_base);
long sbi_remote_hfence_vma(unsigned long hart_mask,
                           unsigned long hart_mask_base,
                           unsigned long start_addr, unsigned long size);
long sbi_remote_hfence_vma_asid(unsigned long hart_mask,
                                unsigned long hart_mask_base,
                                unsigned long start_addr, unsigned long size,
                                unsigned long asid);
long sbi_remote_hfence_gvma_vmid(unsigned long hart_mask,
                                 unsigned long hart_mask_base,
                                 unsigned long start_addr, unsigned long size,
                                 unsigned long vmid);
long sbi_remote_hfence_gvma(unsigned long hart_mask,
                            unsigned long hart_mask_base,
                            unsigned long start_addr, unsigned long size);
long sbi_remote_hfence_vvma_asid(unsigned long hart_mask,
                                 unsigned long hart_mask_base,
                                 unsigned long start_addr, unsigned long size,
                                 unsigned long asid);
long sbi_remote_hfence_vvma(unsigned long hart_mask,
                            unsigned long hart_mask_base,
                            unsigned long start_addr, unsigned long size);

// HSM (Hart State Management) extension
long sbi_hart_start(unsigned long hartid, unsigned long start_addr,
                    unsigned long opaque);
long sbi_hart_stop(void);
long sbi_hart_get_status(unsigned long hartid);
long sbi_hart_suspend(uint32 suspend_type, unsigned long resume_addr,
                      unsigned long opaque);

// System Reset extension
void sbi_system_reset(uint32 reset_type, uint32 reset_reason);
void sbi_shutdown(void);
void sbi_reboot(void);

// Extension availability
void sbi_probe_extensions(void);
int sbi_ext_is_available(enum sbi_ext_id ext_id);
const char *sbi_ext_name(enum sbi_ext_id ext_id);

// Early console output (before UART init)
void sbi_console_putchar(int c);
void sbi_console_puts(const char *s);
int sbi_console_getchar(void);

// Convenience functions
const char *sbi_error_str(long error);
const char *sbi_hart_state_str(long state);
void sbi_print_version(void);

// Multi-hart management
void sbi_start_secondary_harts(unsigned long start_addr);

#endif // __ASSEMBLER__

#endif // __KERNEL_SBI_H
