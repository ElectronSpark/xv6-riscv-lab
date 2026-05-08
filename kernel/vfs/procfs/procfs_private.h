/*
 * procfs/procfs_private.h - Internal types and declarations for procfs
 *
 * procfs exposes process information under /proc in a read-only hierarchy:
 *
 *   /proc/
 *     self                 → symlink  → /proc/<current-tgid>
 *     meminfo              → regular file
 *     cpuinfo              → regular file
 *     zoneinfo             → regular file
 *     version              → regular file
 *     uptime               → regular file
 *     stat                 → regular file
 *     loadavg              → regular file
 *     filesystems          → regular file
 *     mounts               → regular file
 *     sys/                 → compatibility sysctl tree
 *     <tgid>/
 *       status             → regular file
 *       stat               → regular file
 *       cmdline            → regular file
 *       comm               → regular file
 *       statm              → regular file
 *       cgroup             → regular file
 *       task/
 *         <tid>/           → per-thread directory
 *           stat           → regular file
 *           status         → regular file
 *           comm           → regular file
 *           statm          → regular file
 *           cmdline        → regular file
 *           maps           → regular file
 *           smaps          → regular file
 *           cgroup         → regular file
 *       maps               → regular file
 *       smaps              → regular file
 *       mountinfo          → regular file
 *       mounts             → regular file
 *       limits             → regular file
 *       environ            → regular file
 *       auxv               → regular file
 *       exe                → symlink  → executable path
 *       fd/
 *         <n>              → symlink  → target path
 *       fdinfo/
 *         <n>              → regular file
 *
 * Inode number encoding (deterministic from content type + PID):
 *
 *   1                             /proc/ (root directory)
 *   2                             /proc/self
 *   3                             /proc/meminfo
 *   4                             /proc/cpuinfo
 *   7                             /proc/zoneinfo
 *   PROCFS_PID_BASE + tgid*32 + 0  /proc/<tgid>/   (directory)
 *   PROCFS_PID_BASE + tgid*32 + 1  /proc/<tgid>/status
 *   PROCFS_PID_BASE + tgid*32 + 2  /proc/<tgid>/maps
 *   PROCFS_PID_BASE + tgid*32 + 3  /proc/<tgid>/exe
 *   PROCFS_PID_BASE + tgid*32 + 4  /proc/<tgid>/fd/
 *   PROCFS_PID_BASE + tgid*32 + 6  /proc/<tgid>/statm
 *   PROCFS_PID_BASE + tgid*32 + 7  /proc/<tgid>/cgroup
 *   PROCFS_PID_BASE + tgid*32 + 8  /proc/<tgid>/stat
 *   PROCFS_PID_BASE + tgid*32 + 9  /proc/<tgid>/cmdline
 *   PROCFS_PID_BASE + tgid*32 + 10 /proc/<tgid>/comm
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
    PROC_ZONEINFO,    /* /proc/zoneinfo       */
    PROC_PID_DIR,     /* /proc/<tgid>/        */
    PROC_STATUS,      /* /proc/<tgid>/status  */
    PROC_MAPS,        /* /proc/<tgid>/maps    */
    PROC_STATM,       /* /proc/<tgid>/statm   */
    PROC_CGROUP,      /* /proc/<tgid>/cgroup  */
    PROC_EXE,         /* /proc/<tgid>/exe     */
    PROC_FDDIR,       /* /proc/<tgid>/fd/     */
    PROC_FD_ENTRY,    /* /proc/<tgid>/fd/<n>  */
    PROC_FDINFODIR,   /* /proc/<tgid>/fdinfo/ */
    PROC_FDINFO_ENTRY, /* /proc/<tgid>/fdinfo/<n> */
    PROC_RESOURCES,   /* /proc/<tgid>/resources */
    PROC_CRASHES,     /* /proc/crashes          */
    PROC_CMDLINE,     /* /proc/cmdline          */
    PROC_VERSION,     /* /proc/version          */
    PROC_UPTIME,      /* /proc/uptime           */
    PROC_STAT,        /* /proc/stat             */
    PROC_LOADAVG,     /* /proc/loadavg          */
    PROC_FILESYSTEMS, /* /proc/filesystems      */
    PROC_MOUNTS,      /* /proc/mounts           */
    PROC_SYS_DIR,     /* /proc/sys              */
    PROC_SYS_KERNEL_DIR,
    PROC_SYS_VM_DIR,
    PROC_SYS_FS_DIR,
    PROC_PID_STAT,    /* /proc/<tgid>/stat      */
    PROC_PID_CMDLINE, /* /proc/<tgid>/cmdline   */
    PROC_PID_COMM,    /* /proc/<tgid>/comm      */
    PROC_PID_MOUNTINFO, /* /proc/<tgid>/mountinfo */
    PROC_PID_MOUNTS,    /* /proc/<tgid>/mounts    */
    PROC_PID_LIMITS,    /* /proc/<tgid>/limits    */
    PROC_PID_ENVIRON,   /* /proc/<tgid>/environ   */
    PROC_PID_AUXV,      /* /proc/<tgid>/auxv      */
    PROC_PID_SMAPS,     /* /proc/<tgid>/smaps     */
    PROC_TASKDIR,       /* /proc/<tgid>/task      */
    PROC_TASK_TID_DIR,  /* /proc/<tgid>/task/<tid> */
    PROC_TASK_STATUS,
    PROC_TASK_STAT,
    PROC_TASK_STATM,
    PROC_TASK_COMM,
    PROC_TASK_CMDLINE,
    PROC_TASK_MAPS,
    PROC_TASK_SMAPS,
    PROC_TASK_CGROUP,
    PROC_TASK_MOUNTINFO,
    PROC_TASK_MOUNTS,
    PROC_TASK_LIMITS,
    PROC_TASK_ENVIRON,
    PROC_TASK_AUXV,
    PROC_TASK_FDDIR,
    PROC_TASK_FDINFODIR,
    PROC_SYS_KERNEL_OSTYPE,
    PROC_SYS_KERNEL_OSRELEASE,
    PROC_SYS_KERNEL_VERSION,
    PROC_SYS_KERNEL_HOSTNAME,
    PROC_SYS_KERNEL_DOMAINNAME,
    PROC_SYS_KERNEL_RANDOMIZE_VA_SPACE,
    PROC_SYS_KERNEL_PID_MAX,
    PROC_SYS_KERNEL_THREADS_MAX,
    PROC_SYS_VM_OVERCOMMIT_MEMORY,
    PROC_SYS_VM_OVERCOMMIT_RATIO,
    PROC_SYS_VM_MAX_MAP_COUNT,
    PROC_SYS_VM_MMAP_MIN_ADDR,
    PROC_SYS_VM_SWAPPINESS,
    PROC_SYS_VM_DIRTY_RATIO,
    PROC_SYS_VM_DIRTY_BACKGROUND_RATIO,
    PROC_SYS_FS_FILE_MAX,
    PROC_SYS_FS_FILE_NR,
    PROC_SYS_FS_NR_OPEN,
    PROC_SYS_FS_PIPE_MAX_SIZE,
};

/* ------------------------------------------------------------------ */
/*  Inode number encoding                                             */
/* ------------------------------------------------------------------ */
#define PROCFS_INO_ROOT    1ULL
#define PROCFS_INO_SELF    2ULL
#define PROCFS_INO_MEMINFO 3ULL
#define PROCFS_INO_CPUINFO 4ULL
#define PROCFS_INO_CRASHES 5ULL
#define PROCFS_INO_CMDLINE 6ULL
#define PROCFS_INO_ZONEINFO 7ULL
#define PROCFS_INO_VERSION 8ULL
#define PROCFS_INO_UPTIME 9ULL
#define PROCFS_INO_STAT 10ULL
#define PROCFS_INO_LOADAVG 11ULL
#define PROCFS_INO_FILESYSTEMS 12ULL
#define PROCFS_INO_MOUNTS 13ULL
#define PROCFS_INO_SYS 14ULL
#define PROCFS_INO_SYS_KERNEL 15ULL
#define PROCFS_INO_SYS_VM 16ULL
#define PROCFS_INO_SYS_FS 17ULL
#define PROCFS_INO_SYS_KERNEL_OSTYPE 18ULL
#define PROCFS_INO_SYS_KERNEL_OSRELEASE 19ULL
#define PROCFS_INO_SYS_KERNEL_VERSION 20ULL
#define PROCFS_INO_SYS_KERNEL_HOSTNAME 21ULL
#define PROCFS_INO_SYS_KERNEL_DOMAINNAME 22ULL
#define PROCFS_INO_SYS_KERNEL_RANDOMIZE_VA_SPACE 23ULL
#define PROCFS_INO_SYS_KERNEL_PID_MAX 24ULL
#define PROCFS_INO_SYS_KERNEL_THREADS_MAX 25ULL
#define PROCFS_INO_SYS_VM_OVERCOMMIT_MEMORY 26ULL
#define PROCFS_INO_SYS_VM_OVERCOMMIT_RATIO 27ULL
#define PROCFS_INO_SYS_VM_MAX_MAP_COUNT 28ULL
#define PROCFS_INO_SYS_VM_MMAP_MIN_ADDR 29ULL
#define PROCFS_INO_SYS_VM_SWAPPINESS 30ULL
#define PROCFS_INO_SYS_VM_DIRTY_RATIO 31ULL
#define PROCFS_INO_SYS_VM_DIRTY_BACKGROUND_RATIO 32ULL
#define PROCFS_INO_SYS_FS_FILE_MAX 33ULL
#define PROCFS_INO_SYS_FS_FILE_NR 34ULL
#define PROCFS_INO_SYS_FS_NR_OPEN 35ULL
#define PROCFS_INO_SYS_FS_PIPE_MAX_SIZE 36ULL

/* Each pid occupies 32 slots; max pid in xv6 fits well within 64-bit */
#define PROCFS_PID_BASE    100ULL
#define PROCFS_PID_STRIDE  32ULL
#define PROCFS_FD_BASE     (PROCFS_PID_BASE + 100000ULL * PROCFS_PID_STRIDE)

#define PROCFS_PID_DIR_INO(tgid)    (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 0)
#define PROCFS_PID_STATUS_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 1)
#define PROCFS_PID_MAPS_INO(tgid)   (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 2)
#define PROCFS_PID_EXE_INO(tgid)    (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 3)
#define PROCFS_PID_FDDIR_INO(tgid)  (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 4)
#define PROCFS_PID_RESOURCES_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 5)
#define PROCFS_PID_STATM_INO(tgid)  (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 6)
#define PROCFS_PID_CGROUP_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 7)
#define PROCFS_PID_STAT_INO(tgid)   (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 8)
#define PROCFS_PID_CMDLINE_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 9)
#define PROCFS_PID_COMM_INO(tgid)   (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 10)
#define PROCFS_PID_MOUNTINFO_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 11)
#define PROCFS_PID_MOUNTS_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 12)
#define PROCFS_PID_LIMITS_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 13)
#define PROCFS_PID_ENVIRON_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 14)
#define PROCFS_PID_AUXV_INO(tgid)   (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 15)
#define PROCFS_PID_SMAPS_INO(tgid)  (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 16)
#define PROCFS_PID_TASKDIR_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 17)
#define PROCFS_PID_FDINFODIR_INO(tgid) (PROCFS_PID_BASE + (uint64)(tgid)*PROCFS_PID_STRIDE + 18)
#define PROCFS_FD_INO(tgid, fd)     (PROCFS_FD_BASE + (uint64)(tgid)*1000ULL + (uint64)(fd))

/*
 * Per-thread entries sit above fd symlinks.  Decode these before PROCFS_FD_BASE
 * consumers, since every value above PROCFS_FD_BASE was historically treated as
 * an fd inode.
 */
#define PROCFS_TASK_BASE        (PROCFS_FD_BASE + 100000ULL * 1000ULL)
#define PROCFS_TASK_TID_STRIDE  32ULL
#define PROCFS_TASK_TGID_STRIDE (100000ULL * PROCFS_TASK_TID_STRIDE)
#define PROCFS_TASK_INO(tgid, tid, slot) \
    (PROCFS_TASK_BASE + (uint64)(tgid) * PROCFS_TASK_TGID_STRIDE + \
     (uint64)(tid) * PROCFS_TASK_TID_STRIDE + (uint64)(slot))
#define PROCFS_TASK_TID_DIR_INO(tgid, tid) PROCFS_TASK_INO(tgid, tid, 0)

#define PROCFS_FDINFO_BASE \
    (PROCFS_TASK_BASE + 100000ULL * PROCFS_TASK_TGID_STRIDE)
#define PROCFS_FDINFO_INO(tgid, fd) \
    (PROCFS_FDINFO_BASE + (uint64)(tgid) * 1000ULL + (uint64)(fd))

/* ------------------------------------------------------------------ */
/*  Embedded VFS inode                                                */
/* ------------------------------------------------------------------ */
struct procfs_inode {
    struct vfs_inode       vfs_inode; /* must be first — cast-compatible */
    enum procfs_entry_type type;
    int                    pid;       /* tgid; 0 for global entries       */
    int                    tid;       /* only valid for PROC_TASK_*       */
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

struct procfs_blob {
    size_t len;
    char data[0];
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
extern struct vfs_file_ops procfs_blob_file_ops;
extern struct vfs_file_ops procfs_dir_file_ops;

#endif /* KERNEL_VFS_PROCFS_PRIVATE_H */
