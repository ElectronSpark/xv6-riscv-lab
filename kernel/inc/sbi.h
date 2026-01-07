/**
 * SBI (Supervisor Binary Interface) for RISC-V
 * 
 * This module provides an interface to the SBI firmware (e.g., OpenSBI)
 * for operations that require machine-mode privileges.
 */

#ifndef __KERNEL_SBI_H
#define __KERNEL_SBI_H

#include "types.h"

// SBI return error codes
#define SBI_SUCCESS               0
#define SBI_ERR_FAILED           -1
#define SBI_ERR_NOT_SUPPORTED    -2
#define SBI_ERR_INVALID_PARAM    -3
#define SBI_ERR_DENIED           -4
#define SBI_ERR_INVALID_ADDRESS  -5
#define SBI_ERR_ALREADY_AVAILABLE -6
#define SBI_ERR_ALREADY_STARTED  -7
#define SBI_ERR_ALREADY_STOPPED  -8

// SBI extension IDs
#define SBI_EXT_BASE             0x10
#define SBI_EXT_TIME             0x54494D45
#define SBI_EXT_IPI              0x735049
#define SBI_EXT_RFENCE           0x52464E43
#define SBI_EXT_HSM              0x48534D
#define SBI_EXT_SRST             0x53525354

// SBI Base extension function IDs
#define SBI_BASE_GET_SPEC_VERSION    0
#define SBI_BASE_GET_IMPL_ID         1
#define SBI_BASE_GET_IMPL_VERSION    2
#define SBI_BASE_PROBE_EXT           3
#define SBI_BASE_GET_MVENDORID       4
#define SBI_BASE_GET_MARCHID         5
#define SBI_BASE_GET_MIMPID          6

// SBI TIME extension function IDs
#define SBI_TIME_SET_TIMER       0

// SBI IPI extension function IDs
#define SBI_IPI_SEND_IPI         0

// SBI HSM (Hart State Management) function IDs
#define SBI_HSM_HART_START       0
#define SBI_HSM_HART_STOP        1
#define SBI_HSM_HART_GET_STATUS  2
#define SBI_HSM_HART_SUSPEND     3

// SBI HSM hart states
#define SBI_HSM_STATE_STARTED         0
#define SBI_HSM_STATE_STOPPED         1
#define SBI_HSM_STATE_START_PENDING   2
#define SBI_HSM_STATE_STOP_PENDING    3
#define SBI_HSM_STATE_SUSPENDED       4
#define SBI_HSM_STATE_SUSPEND_PENDING 5
#define SBI_HSM_STATE_RESUME_PENDING  6

// SBI SRST (System Reset) function IDs
#define SBI_SRST_RESET           0

// SBI SRST reset types
#define SBI_SRST_TYPE_SHUTDOWN   0
#define SBI_SRST_TYPE_COLD_REBOOT 1
#define SBI_SRST_TYPE_WARM_REBOOT 2

// SBI SRST reset reasons
#define SBI_SRST_REASON_NONE     0
#define SBI_SRST_REASON_SYSFAIL  1

#ifndef __ASSEMBLER__

// SBI return value structure
struct sbiret {
  long error;
  long value;
};

// Generic SBI ecall - implemented in sbi.c
struct sbiret sbi_ecall(int ext, int fid, unsigned long arg0,
                        unsigned long arg1, unsigned long arg2,
                        unsigned long arg3, unsigned long arg4,
                        unsigned long arg5);

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

// Convenience functions
const char *sbi_error_str(long error);
const char *sbi_hart_state_str(long state);
void sbi_print_version(void);

// Multi-hart management
void sbi_start_secondary_harts(unsigned long start_addr);

#endif // __ASSEMBLER__

#endif // __KERNEL_SBI_H
