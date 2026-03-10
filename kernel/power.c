/**
 * @file power.c
 * @brief Power management syscalls — shutdown and reboot.
 *
 * Provides sys_poweroff() and sys_reboot() syscall handlers.
 * Both require root privilege (euid == 0).
 * The actual hardware interaction is delegated to the platform
 * abstraction layer (platform_shutdown / platform_reboot).
 */

#include "types.h"
#include "platform.h"
#include "printf.h"
#include "errno.h"
#include "proc/cred.h"

/**
 * sys_poweroff - power off the machine.
 *
 * Requires root privilege (euid == 0).
 * Calls platform_shutdown() which does not return on success.
 *
 * @return -EPERM if the caller is not root; does not return on success.
 */
uint64 sys_poweroff(void)
{
    if (!capable()) {
        printf("poweroff: permission denied (euid=%d)\n", current_euid());
        return (uint64)-EPERM;
    }

    printf("poweroff: initiated by pid %d\n", current->pid);
    platform_shutdown();

    /* Should never reach here */
    return (uint64)-ENOSYS;
}

/**
 * sys_reboot - reboot the machine.
 *
 * Requires root privilege (euid == 0).
 * Calls platform_reboot() which does not return on success.
 *
 * @return -EPERM if the caller is not root; does not return on success.
 */
uint64 sys_reboot(void)
{
    if (!capable()) {
        printf("reboot: permission denied (euid=%d)\n", current_euid());
        return (uint64)-EPERM;
    }

    printf("reboot: initiated by pid %d\n", current->pid);
    platform_reboot();

    /* Should never reach here */
    return (uint64)-ENOSYS;
}
