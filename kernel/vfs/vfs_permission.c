/**
 * @file vfs_permission.c
 * @brief POSIX file permission checking
 *
 * Central inode_permission() function used by VFS syscalls to enforce
 * POSIX file access permissions based on the calling process's credentials.
 */

#include "types.h"
#include "defs.h"
#include "errno.h"
#include "vfs/vfs_types.h"
#include "vfs/stat.h"
#include "proc/cred.h"

/* Permission mask flags */
#define MAY_READ    4
#define MAY_WRITE   2
#define MAY_EXEC    1

/**
 * inode_permission - check if the current process can access an inode
 * @inode: the inode to check
 * @mask:  desired permission bits (MAY_READ | MAY_WRITE | MAY_EXEC)
 *
 * Returns 0 if access is granted, -EACCES otherwise.
 *
 * Implements POSIX permission semantics:
 *   1. Root (euid == 0) bypasses all permission checks.
 *      For execute, at least one execute bit must be set.
 *   2. If euid == inode->uid, check user permission bits.
 *   3. If egid == inode->gid or gid is in supplementary groups,
 *      check group permission bits.
 *   4. Otherwise check other permission bits.
 */
int inode_permission(struct vfs_inode *inode, int mask)
{
    if (inode == NULL)
        return -EINVAL;

    mode_t mode = inode->mode;

    /* Root can do anything, except execute needs at least one x bit */
    if (capable()) {
        if (!(mask & MAY_EXEC))
            return 0;
        if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
            return 0;
        /* Root can also exec directories (for search) */
        if (S_ISDIR(mode))
            return 0;
        return -EACCES;
    }

    /* Determine which permission bits to check */
    mode_t granted = 0;

    if (current_euid() == inode->uid) {
        /* Owner: check user bits */
        if (mode & S_IRUSR) granted |= MAY_READ;
        if (mode & S_IWUSR) granted |= MAY_WRITE;
        if (mode & S_IXUSR) granted |= MAY_EXEC;
    } else if (current_in_group(inode->gid)) {
        /* Group member: check group bits */
        if (mode & S_IRGRP) granted |= MAY_READ;
        if (mode & S_IWGRP) granted |= MAY_WRITE;
        if (mode & S_IXGRP) granted |= MAY_EXEC;
    } else {
        /* Others */
        if (mode & S_IROTH) granted |= MAY_READ;
        if (mode & S_IWOTH) granted |= MAY_WRITE;
        if (mode & S_IXOTH) granted |= MAY_EXEC;
    }

    if ((mask & granted) == mask)
        return 0;

    return -EACCES;
}
