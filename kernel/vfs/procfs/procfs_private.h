/*
 * procfs/procfs_private.h - Internal types and declarations for procfs
 *
 * procfs exposes process information under /proc in a read-only hierarchy:
 *
 *   /proc/
 *     self                 → symlink  → /proc/<current-tgid>
 *     meminfo              → regular file
 *     cpuinfo              → regular file
 *     <tgid>/
 *       status             → regular file
 *       maps               → regular file
 *       exe                → symlink  → executable path
 *       fd/
 *         <n>              → symlink  → target path
 *
 * Inode number encoding (deterministic from content type + PID):
 *
 *   1                             /proc/ (root directory)
 *   2                             /proc/self
 *   3                             /proc/meminfo
 *   4                             /proc/cpuinfo
 *   PROCFS_PID_BASE + tgid*10 + 0  /proc/<tgid>/   (directory)
 *   PROCFS_PID_BASE + tgid*10 + 1  /proc/<tgid>/status
 *   PROCFS_PID_BASE + tgid*10 + 2  /proc/<tgid>/maps
 *   PROCFS_PID_BASE + tgid*10 + 3  /proc/<tgid>/exe
 *   PROCFS_PID_BASE + tgid*10 + 4  /proc/<tgid>/fd/
 *   PROCFS_FD_BASE  + tgid*1000 + fd  /proc/<tgid>/fd/<fd>
 */

#ifndef KERNEL_VFS_PROCFS_PRIVATE_H
#define KERNEL_VFS_PROCFS_PRIVATE_H

#include "vfs/vfs_types.h"

/* ------------------------------------------------------------------ */
/*  VFS directory iteration cookie sentinels (same as tmpfs)          */
/* ------------------------------------------------------------------ */
#define VFS_DENTRY_COOKIE_END    ((int64)0)
#define VFS_DENTRY_COOKIE_SELF   ((int64)1)
#define VFS_DENTRY_COOKIE_PARENT ((int64)2)

/* Per-inode entry classification */
enum procfs_entry_type {
    PROC_ROOT    = 0, /* /proc/               */
    PROC_SELF,        /* /proc/self           */
    PROC_MEMINFO,     /* /proc/meminfo        */
    PROC_CPUINFO,     /* /proc/cpuinfo        */
    PROC_PID_DIR,     /* /proc/<tgid>/        */
    PROC_STATUS,      /* /proc/<tgid>/status  */
    PROC_MAPS,        /* /proc/<tgid>/maps    */
    PROC_EXE,         /* /proc/<tgid>/exe     */
    PROC_FDDIR,       /* /proc/<tgid>/fd/     */
    PROC_FD_ENTRY,    /* /proc/<tgid>/fd/<n>  */
    PROC_RESOURCES,   /* /proc/<tgid>/resources */
    PROC_CRASHES,     /* /proc/crashes          */
};

/* ------------------------------------------------------------------ */
/*  Inode number encoding                                             */
/* ------------------------------------------------------------------ */
#define PROCFS_INO_ROOT    1ULL
#define PROCFS_INO_SELF    2ULL
#define PROCFS_INO_MEMINFO 3ULL
#define PROCFS_INO_CPUINFO 4ULL
#define PROCFS_INO_CRASHES 5ULL

/* Each pid occupies 10 slots; max pid in xv6 fits well within 64-bit */
#define PROCFS_PID_BASE    100ULL
#define PROCFS_FD_BASE     (PROCFS_PID_BASE + 100000ULL * 10ULL)

#define PROCFS_PID_DIR_INO(tgid)    (PROCFS_PID_BASE + (uint64)(tgid)*10ULL + 0)
#define PROCFS_PID_STATUS_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*10ULL + 1)
#define PROCFS_PID_MAPS_INO(tgid)   (PROCFS_PID_BASE + (uint64)(tgid)*10ULL + 2)
#define PROCFS_PID_EXE_INO(tgid)    (PROCFS_PID_BASE + (uint64)(tgid)*10ULL + 3)
#define PROCFS_PID_FDDIR_INO(tgid)  (PROCFS_PID_BASE + (uint64)(tgid)*10ULL + 4)
#define PROCFS_PID_RESOURCES_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*10ULL + 5)
#define PROCFS_FD_INO(tgid, fd)     (PROCFS_FD_BASE + (uint64)(tgid)*1000ULL + (uint64)(fd))

/* ------------------------------------------------------------------ */
/*  Embedded VFS inode                                                */
/* ------------------------------------------------------------------ */
struct procfs_inode {
    struct vfs_inode       vfs_inode; /* must be first — cast-compatible */
    enum procfs_entry_type type;
    int                    pid;       /* tgid; 0 for global entries       */
    int                    fd;        /* only valid for PROC_FD_ENTRY      */
};

/* Cast helper: get procfs_inode from embedded vfs_inode */
#define procfs_i(inode) \
    container_of((inode), struct procfs_inode, vfs_inode)

/* ------------------------------------------------------------------ */
/*  Snapshot used for root-dir pid enumeration during getdents        */
/* ------------------------------------------------------------------ */
struct procfs_pid_snap {
    int n;       /* number of tgids */
    int pids[0]; /* n tgids         */
};

/* ------------------------------------------------------------------ */
/*  Module-internal symbols                                           */
/* ------------------------------------------------------------------ */
/* Exported by superblock.c */
extern struct vfs_superblock_ops procfs_sb_ops;
void procfs_free_inode(struct vfs_inode *inode);
struct vfs_inode *procfs_alloc_inode(struct vfs_superblock *sb);

/* Exported by inode.c */
extern struct vfs_inode_ops procfs_inode_ops;

/* Exported by file.c */
extern struct vfs_file_ops procfs_reg_file_ops;
extern struct vfs_file_ops procfs_dir_file_ops;

#endif /* KERNEL_VFS_PROCFS_PRIVATE_H */
