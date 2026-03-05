#ifndef __KERNEL_CRED_H
#define __KERNEL_CRED_H

/**
 * @file cred.h
 * @brief Process credential helpers
 *
 * Inline accessors for the current process's user/group credentials.
 * Credentials live in struct thread_group, shared by all threads in
 * the same POSIX process.
 */

#include "proc/thread.h"
#include "proc/thread_group.h"
#include <smp/percpu.h>

/* ── Read accessors ─────────────────────────────────────────────────────── */

static inline uint32 current_uid(void)  { return current->thread_group->uid;  }
static inline uint32 current_gid(void)  { return current->thread_group->gid;  }
static inline uint32 current_euid(void) { return current->thread_group->euid; }
static inline uint32 current_egid(void) { return current->thread_group->egid; }
static inline uint32 current_suid(void) { return current->thread_group->suid; }
static inline uint32 current_sgid(void) { return current->thread_group->sgid; }
static inline mode_t current_umask(void){ return current->thread_group->umask;}

/**
 * current_in_group - check if the current process is in a given group
 * @gid: group ID to test
 *
 * Returns 1 if @gid matches the effective GID or any supplementary group.
 */
static inline int current_in_group(uint32 gid)
{
    struct thread_group *tg = current->thread_group;
    if (tg->egid == gid)
        return 1;
    for (int i = 0; i < tg->ngroups; i++) {
        if (tg->groups[i] == gid)
            return 1;
    }
    return 0;
}

/**
 * capable - check if the current process has root privileges
 *
 * In this simple model, root is euid == 0.
 * Returns 1 if privileged, 0 otherwise.
 */
static inline int capable(void)
{
    return current_euid() == 0;
}

#endif /* __KERNEL_CRED_H */
