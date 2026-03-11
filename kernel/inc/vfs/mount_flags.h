/*
 * VFS mount flags — matching Linux/musl definitions
 *
 * These values must match the MS_* constants defined in
 * musl's <sys/mount.h> so that user-space mount(2) calls
 * pass through correctly.
 */
#ifndef __KERNEL_VFS_MOUNT_FLAGS_H
#define __KERNEL_VFS_MOUNT_FLAGS_H

/* mount(2) flags */
#define MS_RDONLY       1            /* Mount read-only */
#define MS_NOSUID       2            /* Ignore suid and sgid bits */
#define MS_NODEV        4            /* Disallow access to device special files */
#define MS_NOEXEC       8            /* Disallow program execution */
#define MS_SYNCHRONOUS  16           /* Writes are synced at once */
#define MS_REMOUNT      32           /* Alter flags of a mounted filesystem */
#define MS_MANDLOCK     64           /* Allow mandatory locks on an FS */
#define MS_DIRSYNC      128          /* Directory modifications are synchronous */
#define MS_NOATIME      1024         /* Do not update access times */
#define MS_NODIRATIME   2048         /* Do not update directory access times */
#define MS_BIND         4096         /* Bind mount */
#define MS_MOVE         8192         /* Move a mount from one place to another */
#define MS_REC          16384        /* Recursive mount */
#define MS_SILENT       32768        /* Suppress some printk messages */
#define MS_RELATIME     (1 << 21)    /* Update atime relative to mtime/ctime */
#define MS_NOUSER       (1U << 31)   /* Not for user-space use */

/* Mask of flags that can be changed via MS_REMOUNT */
#define MS_RMT_MASK     (MS_RDONLY | MS_SYNCHRONOUS | MS_MANDLOCK)

/* umount2(2) flags */
#define MNT_FORCE       1            /* Force unmount */
#define MNT_DETACH      2            /* Lazy unmount */
#define MNT_EXPIRE      4            /* Mark for expiry */
#define UMOUNT_NOFOLLOW 8            /* Don't follow symlinks on umount */

#endif /* __KERNEL_VFS_MOUNT_FLAGS_H */
