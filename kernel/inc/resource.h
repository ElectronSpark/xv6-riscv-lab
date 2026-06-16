/**
 * @file resource.h
 * @brief POSIX resource limits (rlimit) for the kernel
 *
 * Defines struct rlimit and RLIMIT_* constants matching the POSIX/Linux
 * ABI.  User-space declarations live in newlib's <sys/resource.h>;
 * this header is the kernel-side mirror used by thread_group and the
 * prlimit64 syscall.
 */

#ifndef __KERNEL_RESOURCE_H
#define __KERNEL_RESOURCE_H

#include "types.h"

/**
 * @brief Per-resource soft/hard limit pair.
 *
 * rlim_cur is the effective (soft) limit — the kernel enforces this.
 * rlim_max is the ceiling (hard limit) — unprivileged processes may
 * only lower it.  RLIM_INFINITY means "no limit".
 */
struct rlimit {
    uint64 rlim_cur;   /* soft limit (enforced) */
    uint64 rlim_max;   /* hard limit (ceiling)  */
};

#define RLIM_INFINITY  ((uint64)-1ULL)

/* Resource identifiers — match the x86_64 Linux values exactly. */
#define RLIMIT_CPU        0    /* CPU time per process (seconds)      */
#define RLIMIT_FSIZE      1    /* max file size (bytes)               */
#define RLIMIT_DATA       2    /* max data segment size               */
#define RLIMIT_STACK      3    /* max stack size (bytes)              */
#define RLIMIT_CORE       4    /* max core file size (bytes)          */
#define RLIMIT_RSS        5    /* max resident set size               */
#define RLIMIT_NPROC      6    /* max number of processes             */
#define RLIMIT_NOFILE     7    /* max number of open files            */
#define RLIMIT_MEMLOCK    8    /* max locked-in-memory address space  */
#define RLIMIT_AS         9    /* max address space (virtual memory)  */
#define RLIMIT_LOCKS      10   /* max file locks held                 */
#define RLIMIT_SIGPENDING 11   /* max queued signals                  */
#define RLIMIT_MSGQUEUE   12   /* POSIX message queue bytes           */
#define RLIMIT_NICE       13   /* max nice priority raise             */
#define RLIMIT_RTPRIO     14   /* max realtime priority               */
#define RLIMIT_RTTIME     15   /* realtime CPU time in microseconds   */

#define RLIMIT_NLIMITS   16    /* number of resource limit types      */

/**
 * @brief Initialise a rlimit array to sensible defaults.
 *
 * Called when a new thread_group is created.  Most limits default to
 * RLIM_INFINITY; RLIMIT_NOFILE and RLIMIT_STACK are configured to
 * match the compile-time constants.
 */
void rlimit_init_defaults(struct rlimit rlim[RLIMIT_NLIMITS]);

#endif /* __KERNEL_RESOURCE_H */
