/*
 * procfs/inode.c - inode operations: lookup, dir_iter, readlink, getattr,
 *                  open (with content generation for regular files)
 *
 * Terminology note
 * ----------------
 * VFS pre-increments iter->index from 2 ("PARENT") to 3 before invoking
 * dir_iter for the first real child.  So:
 *   iter->index == 1  →  return the ".." entry (driver's job for non-root)
 *   iter->index == 3  →  first real child
 *   iter->index == 4  →  second real child, etc.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "lock/mutex_types.h"
#include "lock/rwsem.h"
#include "lock/rcu.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include <mm/mm_watermark.h>
#include "arch/vm.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/rq.h"
#include "proc/sched.h"
#include "maple_tree.h"
#include "printf.h"
#include "procfs_private.h"
#include "dev/cdev.h"
#include "accounting.h"
#include "timer/timer.h"
#include "timer/goldfish_rtc.h"
#include "signal.h"
#include "dev/fdt.h"
#include "smp/percpu.h"

/* snprintf is provided by lwip_port/sys_arch.c – forward-declare it here */
int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/*  VFS_DITER_INDEX_CURRENT is the value iter->index holds before     */
/*  the VFS advances it to VFS_DITER_INDEX_PARENT (= 2).              */
/*  Children start at index 3 (VFS increments from 2 → 3 first).     */
/* ------------------------------------------------------------------ */
#define PROCFS_CHILDREN_START 3

struct procfs_static_entry {
    const char *name;
    uint64 ino;
};

static const struct procfs_static_entry procfs_root_entries[] = {
    {"self", PROCFS_INO_SELF},
    {"meminfo", PROCFS_INO_MEMINFO},
    {"cpuinfo", PROCFS_INO_CPUINFO},
    {"crashes", PROCFS_INO_CRASHES},
    {"cmdline", PROCFS_INO_CMDLINE},
    {"zoneinfo", PROCFS_INO_ZONEINFO},
    {"version", PROCFS_INO_VERSION},
    {"uptime", PROCFS_INO_UPTIME},
    {"stat", PROCFS_INO_STAT},
    {"loadavg", PROCFS_INO_LOADAVG},
    {"filesystems", PROCFS_INO_FILESYSTEMS},
    {"mounts", PROCFS_INO_MOUNTS},
    {"sys", PROCFS_INO_SYS},
};

static const struct procfs_static_entry procfs_pid_entries[] = {
    {"status", 0},
    {"statm", 0},
    {"cgroup", 0},
    {"maps", 0},
    {"smaps", 0},
    {"exe", 0},
    {"fd", 0},
    {"resources", 0},
    {"stat", 0},
    {"cmdline", 0},
    {"comm", 0},
    {"mountinfo", 0},
    {"mounts", 0},
    {"limits", 0},
    {"environ", 0},
    {"auxv", 0},
    {"oom_score_adj", 0},
    {"mem", 0},
    {"task", 0},
    {"fdinfo", 0},
    {"ns", 0},
    {"setgroups", 0},
};

static const struct procfs_static_entry procfs_task_entries[] = {
    {"status", 0},
    {"stat", 0},
    {"statm", 0},
    {"comm", 0},
    {"cmdline", 0},
    {"maps", 0},
    {"smaps", 0},
    {"cgroup", 0},
    {"mountinfo", 0},
    {"mounts", 0},
    {"limits", 0},
    {"environ", 0},
    {"auxv", 0},
    {"mem", 0},
    {"fd", 0},
    {"fdinfo", 0},
    {"ns", 0},
};

struct procfs_ns_desc {
    const char *name;
    uint64 initial_ino;
};

static const struct procfs_ns_desc procfs_ns_entries[] = {
    {"cgroup", 4026531835ULL},
    {"ipc", 4026531839ULL},
    {"mnt", 4026531841ULL},
    {"net", 4026531840ULL},
    {"pid", 4026531836ULL},
    {"pid_for_children", 4026531836ULL},
    {"time", 4026531834ULL},
    {"time_for_children", 4026531834ULL},
    {"user", 4026531837ULL},
    {"uts", 4026531838ULL},
};

static const struct procfs_static_entry procfs_sys_entries[] = {
    {"kernel", PROCFS_INO_SYS_KERNEL},
    {"vm", PROCFS_INO_SYS_VM},
    {"fs", PROCFS_INO_SYS_FS},
};

static const struct procfs_static_entry procfs_sys_kernel_entries[] = {
    {"ostype", PROCFS_INO_SYS_KERNEL_OSTYPE},
    {"osrelease", PROCFS_INO_SYS_KERNEL_OSRELEASE},
    {"version", PROCFS_INO_SYS_KERNEL_VERSION},
    {"hostname", PROCFS_INO_SYS_KERNEL_HOSTNAME},
    {"domainname", PROCFS_INO_SYS_KERNEL_DOMAINNAME},
    {"randomize_va_space", PROCFS_INO_SYS_KERNEL_RANDOMIZE_VA_SPACE},
    {"pid_max", PROCFS_INO_SYS_KERNEL_PID_MAX},
    {"threads-max", PROCFS_INO_SYS_KERNEL_THREADS_MAX},
};

static const struct procfs_static_entry procfs_sys_vm_entries[] = {
    {"overcommit_memory", PROCFS_INO_SYS_VM_OVERCOMMIT_MEMORY},
    {"overcommit_ratio", PROCFS_INO_SYS_VM_OVERCOMMIT_RATIO},
    {"max_map_count", PROCFS_INO_SYS_VM_MAX_MAP_COUNT},
    {"mmap_min_addr", PROCFS_INO_SYS_VM_MMAP_MIN_ADDR},
    {"swappiness", PROCFS_INO_SYS_VM_SWAPPINESS},
    {"dirty_ratio", PROCFS_INO_SYS_VM_DIRTY_RATIO},
    {"dirty_background_ratio", PROCFS_INO_SYS_VM_DIRTY_BACKGROUND_RATIO},
};

static const struct procfs_static_entry procfs_sys_fs_entries[] = {
    {"file-max", PROCFS_INO_SYS_FS_FILE_MAX},
    {"file-nr", PROCFS_INO_SYS_FS_FILE_NR},
    {"nr_open", PROCFS_INO_SYS_FS_NR_OPEN},
    {"pipe-max-size", PROCFS_INO_SYS_FS_PIPE_MAX_SIZE},
    {"inotify", PROCFS_INO_SYS_FS_INOTIFY},
};

static const struct procfs_static_entry procfs_sys_fs_inotify_entries[] = {
    {"max_user_watches", PROCFS_INO_SYS_FS_INOTIFY_MAX_USER_WATCHES},
    {"max_user_instances", PROCFS_INO_SYS_FS_INOTIFY_MAX_USER_INSTANCES},
    {"max_queued_events", PROCFS_INO_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS},
};

static int procfs_lookup_static(const struct procfs_static_entry *entries,
                                int nentries, const char *name,
                                size_t name_len, uint64 *ino)
{
    for (int i = 0; i < nentries; i++) {
        size_t len = strlen(entries[i].name);
        if (len == name_len && memcmp(name, entries[i].name, len) == 0) {
            *ino = entries[i].ino;
            return 0;
        }
    }
    return -ENOENT;
}

static int procfs_emit_static(struct vfs_dir_iter *iter,
                              struct vfs_dentry *ret_dentry,
                              const struct procfs_static_entry *entries,
                              int nentries, int child_idx)
{
    if (child_idx < 0 || child_idx >= nentries) {
        vfs_release_dentry(ret_dentry);
        ret_dentry->name = NULL;
        return 0;
    }

    const char *name = entries[child_idx].name;
    vfs_release_dentry(ret_dentry);
    ret_dentry->name = strdup(name);
    if (ret_dentry->name == NULL)
        return -ENOMEM;
    ret_dentry->name_len = (uint16)strlen(name);
    ret_dentry->ino = entries[child_idx].ino;
    ret_dentry->cookies = (int64)iter->index;
    return 0;
}

static uint64 procfs_pid_entry_ino(int tgid, int child_idx)
{
    switch (child_idx) {
    case 0: return PROCFS_PID_STATUS_INO(tgid);
    case 1: return PROCFS_PID_STATM_INO(tgid);
    case 2: return PROCFS_PID_CGROUP_INO(tgid);
    case 3: return PROCFS_PID_MAPS_INO(tgid);
    case 4: return PROCFS_PID_SMAPS_INO(tgid);
    case 5: return PROCFS_PID_EXE_INO(tgid);
    case 6: return PROCFS_PID_FDDIR_INO(tgid);
    case 7: return PROCFS_PID_RESOURCES_INO(tgid);
    case 8: return PROCFS_PID_STAT_INO(tgid);
    case 9: return PROCFS_PID_CMDLINE_INO(tgid);
    case 10: return PROCFS_PID_COMM_INO(tgid);
    case 11: return PROCFS_PID_MOUNTINFO_INO(tgid);
    case 12: return PROCFS_PID_MOUNTS_INO(tgid);
    case 13: return PROCFS_PID_LIMITS_INO(tgid);
    case 14: return PROCFS_PID_ENVIRON_INO(tgid);
    case 15: return PROCFS_PID_AUXV_INO(tgid);
    case 16: return PROCFS_PID_OOM_SCORE_ADJ_INO(tgid);
    case 17: return PROCFS_PID_MEM_INO(tgid);
    case 18: return PROCFS_PID_TASKDIR_INO(tgid);
    case 19: return PROCFS_PID_FDINFODIR_INO(tgid);
    case 20: return PROCFS_PID_NS_DIR_INO(tgid);
    case 21: return PROCFS_PID_SETGROUPS_INO(tgid);
    default: return 0;
    }
}

static uint64 procfs_task_entry_ino(int tgid, int tid, int child_idx)
{
    return PROCFS_TASK_INO(tgid, tid, child_idx + 1);
}

static int procfs_lookup_ns_entry(const char *name, size_t name_len)
{
    for (int i = 0; i < NELEM(procfs_ns_entries); i++) {
        size_t len = strlen(procfs_ns_entries[i].name);
        if (len == name_len &&
            memcmp(name, procfs_ns_entries[i].name, len) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Helper: find the n-th group leader (0-based) in the pid table     */
/* ------------------------------------------------------------------ */

struct __nth_tgid_arg {
    int target;
    int cnt;     /* counts seen so far */
    int result;
};

static void __nth_tgid_cb(int tgid, void *arg) {
    struct __nth_tgid_arg *a = arg;
    if (a->result >= 0)
        return; /* already found */
    if (a->cnt++ == a->target)
        a->result = tgid;
}

static int procfs_nth_tgid(int n) {
    struct __nth_tgid_arg arg = {.target = n, .cnt = 0, .result = -1};
    proctab_for_each_tgid(__nth_tgid_cb, &arg);
    return arg.result;
}

/* ------------------------------------------------------------------ */
/*  Helper: find the n-th thread (0-based) in a thread group          */
/* ------------------------------------------------------------------ */

struct __nth_tid_arg {
    int tgid;
    int target;
    int cnt;
    int result;
};

static void __nth_tid_cb(struct thread *p, void *arg)
{
    struct __nth_tid_arg *a = arg;
    if (a->result >= 0 || p->tgid != a->tgid)
        return;
    if (a->cnt++ == a->target)
        a->result = p->pid;
}

static int procfs_nth_tid(int tgid, int n)
{
    struct __nth_tid_arg arg = {
        .tgid = tgid,
        .target = n,
        .cnt = 0,
        .result = -1,
    };
    proctab_for_each_rcu(__nth_tid_cb, &arg);
    return arg.result;
}

static int procfs_valid_tid_in_tgid(int tgid, int tid)
{
    int valid;
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tid, &p);
    valid = p != NULL && p->tgid == tgid;
    rcu_read_unlock();
    return valid;
}

struct __count_tid_arg {
    int tgid;
    int count;
};

static void __count_tid_cb(struct thread *p, void *arg)
{
    struct __count_tid_arg *a = arg;
    if (p->tgid == a->tgid)
        a->count++;
}

static int procfs_count_tids(int tgid)
{
    struct __count_tid_arg arg = {
        .tgid = tgid,
        .count = 0,
    };
    proctab_for_each_rcu(__count_tid_cb, &arg);
    return arg.count;
}

/* ------------------------------------------------------------------ */
/*  Helper: find the n-th open fd (0-based) in a fdtable              */
/* ------------------------------------------------------------------ */

static int procfs_nth_fd(int pid, int n) {
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(pid, &p);
    if (p == NULL || p->fdtable == NULL) {
        rcu_read_unlock();
        return -1;
    }
    struct vfs_fdtable *ft = p->fdtable;
    int count = 0;
    int found = -1;
    for (int fd = 0; fd < NOFILE; fd++) {
        if (ft->files[fd] != NULL) {
            if (count++ == n) {
                found = fd;
                break;
            }
        }
    }
    rcu_read_unlock();
    return found;
}

/* ------------------------------------------------------------------ */
/*  procfs_lookup – name-to-inode mapping within a procfs directory   */
/* ------------------------------------------------------------------ */

static int procfs_lookup(struct vfs_inode *dir, struct vfs_dentry *dentry,
                         const char *name, size_t name_len) {
    struct procfs_inode *pi = procfs_i(dir);

    /* Required by VFS: driver must set dentry->sb (and optionally ->parent) */
    dentry->sb       = dir->sb;
    dentry->parent   = dir;
    dentry->name     = strndup(name, name_len);
    if (dentry->name == NULL)
        return -ENOMEM;
    dentry->name_len = name_len;

    switch (pi->type) {

    case PROC_ROOT: {
        uint64 ino;
        if (procfs_lookup_static(procfs_root_entries,
                                 NELEM(procfs_root_entries), name, name_len,
                                 &ino) == 0) {
            dentry->ino = ino;
            return 0;
        }

        /* Try to interpret as a decimal tgid */
        if (name_len == 0 || name_len > 7)
            return -ENOENT;
        int tgid = 0;
        for (size_t i = 0; i < name_len; i++) {
            if (name[i] < '0' || name[i] > '9')
                return -ENOENT;
            tgid = tgid * 10 + (name[i] - '0');
        }
        if (tgid <= 0)
            return -ENOENT;

        rcu_read_lock();
        struct thread *p = NULL;
        get_pid_thread(tgid, &p);
        rcu_read_unlock();

        if (p == NULL || p->tgid != tgid || p->pid != p->tgid)
            return -ENOENT;

        dentry->ino = PROCFS_PID_DIR_INO(tgid);
        return 0;
    }

    case PROC_PID_DIR: {
        for (int i = 0; i < NELEM(procfs_pid_entries); i++) {
            size_t len = strlen(procfs_pid_entries[i].name);
            if (len == name_len &&
                memcmp(name, procfs_pid_entries[i].name, len) == 0) {
                dentry->ino = procfs_pid_entry_ino(pi->pid, i);
                return 0;
            }
        }
        return -ENOENT;
    }

    case PROC_TASKDIR: {
        if (name_len == 0 || name_len > 7)
            return -ENOENT;
        int tid = 0;
        for (size_t i = 0; i < name_len; i++) {
            if (name[i] < '0' || name[i] > '9')
                return -ENOENT;
            tid = tid * 10 + (name[i] - '0');
        }
        if (tid <= 0 || !procfs_valid_tid_in_tgid(pi->pid, tid))
            return -ENOENT;

        dentry->ino = PROCFS_TASK_TID_DIR_INO(pi->pid, tid);
        return 0;
    }

    case PROC_TASK_TID_DIR: {
        for (int i = 0; i < NELEM(procfs_task_entries); i++) {
            size_t len = strlen(procfs_task_entries[i].name);
            if (len == name_len &&
                memcmp(name, procfs_task_entries[i].name, len) == 0) {
                dentry->ino = procfs_task_entry_ino(pi->pid, pi->tid, i);
                return 0;
            }
        }
        return -ENOENT;
    }

    case PROC_PID_NS_DIR: {
        int ns_idx = procfs_lookup_ns_entry(name, name_len);
        if (ns_idx < 0)
            return -ENOENT;
        dentry->ino = PROCFS_PID_NS_ENTRY_INO(pi->pid, ns_idx);
        return 0;
    }

    case PROC_TASK_NS_DIR: {
        int ns_idx = procfs_lookup_ns_entry(name, name_len);
        if (ns_idx < 0)
            return -ENOENT;
        dentry->ino = PROCFS_TASK_NS_ENTRY_INO(pi->pid, pi->tid, ns_idx);
        return 0;
    }

    case PROC_SYS_DIR: {
        uint64 ino;
        if (procfs_lookup_static(procfs_sys_entries, NELEM(procfs_sys_entries),
                                 name, name_len, &ino) == 0) {
            dentry->ino = ino;
            return 0;
        }
        return -ENOENT;
    }

    case PROC_SYS_KERNEL_DIR: {
        uint64 ino;
        if (procfs_lookup_static(procfs_sys_kernel_entries,
                                 NELEM(procfs_sys_kernel_entries), name,
                                 name_len, &ino) == 0) {
            dentry->ino = ino;
            return 0;
        }
        return -ENOENT;
    }

    case PROC_SYS_VM_DIR: {
        uint64 ino;
        if (procfs_lookup_static(procfs_sys_vm_entries,
                                 NELEM(procfs_sys_vm_entries), name,
                                 name_len, &ino) == 0) {
            dentry->ino = ino;
            return 0;
        }
        return -ENOENT;
    }

    case PROC_SYS_FS_DIR: {
        uint64 ino;
        if (procfs_lookup_static(procfs_sys_fs_entries,
                                 NELEM(procfs_sys_fs_entries), name,
                                 name_len, &ino) == 0) {
            dentry->ino = ino;
            return 0;
        }
        return -ENOENT;
    }

    case PROC_SYS_FS_INOTIFY_DIR: {
        uint64 ino;
        if (procfs_lookup_static(procfs_sys_fs_inotify_entries,
                                 NELEM(procfs_sys_fs_inotify_entries), name,
                                 name_len, &ino) == 0) {
            dentry->ino = ino;
            return 0;
        }
        return -ENOENT;
    }

    case PROC_FDDIR:
    case PROC_FDINFODIR:
    case PROC_TASK_FDDIR:
    case PROC_TASK_FDINFODIR: {
        if (name_len == 0 || name_len > 7)
            return -ENOENT;
        int fdnum = 0;
        for (size_t i = 0; i < name_len; i++) {
            if (name[i] < '0' || name[i] > '9')
                return -ENOENT;
            fdnum = fdnum * 10 + (name[i] - '0');
        }
        if (fdnum < 0 || fdnum >= NOFILE)
            return -ENOENT;

        /* Verify fd is open for this process */
        rcu_read_lock();
        struct thread *p = NULL;
        get_pid_thread(pi->tid > 0 ? pi->tid : pi->pid, &p);
        int valid = (p != NULL && p->fdtable != NULL &&
                     p->fdtable->files[fdnum] != NULL);
        rcu_read_unlock();

        if (!valid)
            return -ENOENT;

        dentry->ino = (pi->type == PROC_FDINFODIR ||
                       pi->type == PROC_TASK_FDINFODIR)
                          ? PROCFS_FDINFO_INO(pi->pid, fdnum)
                          : PROCFS_FD_INO(pi->pid, fdnum);
        return 0;
    }

    default:
        return -ENOENT;
    }
}

/* ------------------------------------------------------------------ */
/*  procfs_dir_iter – directory iteration                             */
/* ------------------------------------------------------------------ */

static int procfs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter,
                           struct vfs_dentry *ret_dentry) {
    struct procfs_inode *pi = procfs_i(dir);

    /*
     * Index 1 (VFS_DITER_INDEX_CURRENT): driver must return the ".." entry
     * for non-root directories.  The VFS will then advance iter->index to 2.
     */
    if (iter->index == 1) {
        uint64 parent_ino;
        if (pi->type == PROC_PID_DIR) {
            parent_ino = PROCFS_INO_ROOT;
        } else if (pi->type == PROC_FDDIR || pi->type == PROC_FDINFODIR ||
                   pi->type == PROC_TASKDIR || pi->type == PROC_PID_NS_DIR) {
            parent_ino = PROCFS_PID_DIR_INO(pi->pid);
        } else if (pi->type == PROC_TASK_TID_DIR) {
            parent_ino = PROCFS_PID_TASKDIR_INO(pi->pid);
        } else if (pi->type == PROC_TASK_FDDIR ||
                   pi->type == PROC_TASK_FDINFODIR ||
                   pi->type == PROC_TASK_NS_DIR) {
            parent_ino = PROCFS_TASK_TID_DIR_INO(pi->pid, pi->tid);
        } else if (pi->type == PROC_SYS_DIR) {
            parent_ino = PROCFS_INO_ROOT;
        } else if (pi->type == PROC_SYS_KERNEL_DIR ||
                   pi->type == PROC_SYS_VM_DIR ||
                   pi->type == PROC_SYS_FS_DIR) {
            parent_ino = PROCFS_INO_SYS;
        } else {
            /* Root dir should never get index 1 from driver side */
            return -EINVAL;
        }
        vfs_release_dentry(ret_dentry);
        ret_dentry->name = strndup("..", 2);
        if (ret_dentry->name == NULL)
            return -ENOMEM;
        ret_dentry->name_len = 2;
        ret_dentry->cookies  = VFS_DENTRY_COOKIE_PARENT;
        ret_dentry->ino      = parent_ino;
        return 0;
    }

    /*
     * Children: iter->index is ALREADY pre-incremented by the VFS
     * (from 2 to 3 on first call, then 3→4, etc.).
     * The 0-based child index = iter->index - PROCFS_CHILDREN_START.
     */
    int child_idx = (int)iter->index - PROCFS_CHILDREN_START; /* ≥ 0 */

    switch (pi->type) {

    /* ---- /proc/ root children ---- */
    case PROC_ROOT: {
        if (child_idx < NELEM(procfs_root_entries))
            return procfs_emit_static(iter, ret_dentry, procfs_root_entries,
                                      NELEM(procfs_root_entries), child_idx);
        else {
            /* pid entries start after the static root entries. */
            int nth  = child_idx - NELEM(procfs_root_entries);
            int tgid = procfs_nth_tgid(nth);
            if (tgid < 0) {
                /* End of directory */
                vfs_release_dentry(ret_dentry);
                ret_dentry->name = NULL;
                return 0;
            }
            /* Format tgid as decimal string */
            char nbuf[12];
            int  nlen = snprintf(nbuf, sizeof(nbuf), "%d", tgid);
            vfs_release_dentry(ret_dentry);
            ret_dentry->name = strndup(nbuf, nlen);
            if (ret_dentry->name == NULL)
                return -ENOMEM;
            ret_dentry->name_len = (uint16)nlen;
            ret_dentry->ino      = PROCFS_PID_DIR_INO(tgid);
            ret_dentry->cookies  = (int64)iter->index;
            return 0;
        }
    }

    /* ---- /proc/<tgid>/ children ---- */
    case PROC_PID_DIR: {
        if (child_idx < 0 || child_idx >= NELEM(procfs_pid_entries)) {
            vfs_release_dentry(ret_dentry);
            ret_dentry->name = NULL;
            return 0;
        }

        const char *name = procfs_pid_entries[child_idx].name;
        vfs_release_dentry(ret_dentry);
        ret_dentry->name     = strdup(name);
        if (ret_dentry->name == NULL)
            return -ENOMEM;
        ret_dentry->name_len = (uint16)strlen(name);
        ret_dentry->ino      = procfs_pid_entry_ino(pi->pid, child_idx);
        ret_dentry->cookies  = (int64)iter->index;
        return 0;
    }

    /* ---- /proc/<tgid>/task/ children ---- */
    case PROC_TASKDIR: {
        int tid = procfs_nth_tid(pi->pid, child_idx);
        if (tid < 0) {
            vfs_release_dentry(ret_dentry);
            ret_dentry->name = NULL;
            return 0;
        }

        char nbuf[12];
        int nlen = snprintf(nbuf, sizeof(nbuf), "%d", tid);
        vfs_release_dentry(ret_dentry);
        ret_dentry->name = strndup(nbuf, nlen);
        if (ret_dentry->name == NULL)
            return -ENOMEM;
        ret_dentry->name_len = (uint16)nlen;
        ret_dentry->ino = PROCFS_TASK_TID_DIR_INO(pi->pid, tid);
        ret_dentry->cookies = (int64)iter->index;
        return 0;
    }

    /* ---- /proc/<tgid>/task/<tid>/ children ---- */
    case PROC_TASK_TID_DIR: {
        if (child_idx < 0 || child_idx >= NELEM(procfs_task_entries)) {
            vfs_release_dentry(ret_dentry);
            ret_dentry->name = NULL;
            return 0;
        }

        const char *name = procfs_task_entries[child_idx].name;
        vfs_release_dentry(ret_dentry);
        ret_dentry->name = strdup(name);
        if (ret_dentry->name == NULL)
            return -ENOMEM;
        ret_dentry->name_len = (uint16)strlen(name);
        ret_dentry->ino = procfs_task_entry_ino(pi->pid, pi->tid, child_idx);
        ret_dentry->cookies = (int64)iter->index;
        return 0;
    }

    case PROC_SYS_DIR:
        return procfs_emit_static(iter, ret_dentry, procfs_sys_entries,
                                  NELEM(procfs_sys_entries), child_idx);

    case PROC_SYS_KERNEL_DIR:
        return procfs_emit_static(iter, ret_dentry, procfs_sys_kernel_entries,
                                  NELEM(procfs_sys_kernel_entries), child_idx);

    case PROC_SYS_VM_DIR:
        return procfs_emit_static(iter, ret_dentry, procfs_sys_vm_entries,
                                  NELEM(procfs_sys_vm_entries), child_idx);

    case PROC_SYS_FS_DIR:
        return procfs_emit_static(iter, ret_dentry, procfs_sys_fs_entries,
                                  NELEM(procfs_sys_fs_entries), child_idx);

    case PROC_SYS_FS_INOTIFY_DIR:
        return procfs_emit_static(iter, ret_dentry,
                                  procfs_sys_fs_inotify_entries,
                                  NELEM(procfs_sys_fs_inotify_entries),
                                  child_idx);

    case PROC_PID_NS_DIR:
    case PROC_TASK_NS_DIR: {
        if (child_idx < 0 || child_idx >= NELEM(procfs_ns_entries)) {
            vfs_release_dentry(ret_dentry);
            ret_dentry->name = NULL;
            return 0;
        }

        const char *name = procfs_ns_entries[child_idx].name;
        vfs_release_dentry(ret_dentry);
        ret_dentry->name = strdup(name);
        if (ret_dentry->name == NULL)
            return -ENOMEM;
        ret_dentry->name_len = (uint16)strlen(name);
        ret_dentry->ino = (pi->type == PROC_PID_NS_DIR)
                              ? PROCFS_PID_NS_ENTRY_INO(pi->pid, child_idx)
                              : PROCFS_TASK_NS_ENTRY_INO(pi->pid, pi->tid,
                                                         child_idx);
        ret_dentry->cookies = (int64)iter->index;
        return 0;
    }

    /* ---- fd and fdinfo children ---- */
    case PROC_FDDIR:
    case PROC_FDINFODIR:
    case PROC_TASK_FDDIR:
    case PROC_TASK_FDINFODIR: {
        int fd = procfs_nth_fd(pi->tid > 0 ? pi->tid : pi->pid, child_idx);
        if (fd < 0) {
            vfs_release_dentry(ret_dentry);
            ret_dentry->name = NULL;
            return 0;
        }

        char nbuf[12];
        int  nlen = snprintf(nbuf, sizeof(nbuf), "%d", fd);
        vfs_release_dentry(ret_dentry);
        ret_dentry->name = strndup(nbuf, nlen);
        if (ret_dentry->name == NULL)
            return -ENOMEM;
        ret_dentry->name_len = (uint16)nlen;
        ret_dentry->ino      = (pi->type == PROC_FDINFODIR ||
                                pi->type == PROC_TASK_FDINFODIR)
                                   ? PROCFS_FDINFO_INO(pi->pid, fd)
                                   : PROCFS_FD_INO(pi->pid, fd);
        ret_dentry->cookies  = (int64)iter->index;
        return 0;
    }

    default:
        return -ENOTDIR;
    }
}

/* ------------------------------------------------------------------ */
/*  procfs_readlink – resolve symlinks                                */
/* ------------------------------------------------------------------ */

static ssize_t procfs_readlink(struct vfs_inode *inode, char *buf,
                               size_t buflen) {
    struct procfs_inode *pi = procfs_i(inode);
    ssize_t n;

    switch (pi->type) {

    case PROC_SELF: {
        /* /proc/self → /proc/<current-tgid> */
        int tgid = current->tgid;
        n = snprintf(buf, buflen, "/proc/%d", tgid);
        return n;
    }

    case PROC_EXE: {
        rcu_read_lock();
        struct thread *p = NULL;
        get_pid_thread(pi->pid, &p);
        if (p == NULL) {
            rcu_read_unlock();
            return -ESRCH;
        }
        char exec_path[128];
        if (p->thread_group != NULL) {
            memmove(exec_path, p->thread_group->exec_path, 128);
            exec_path[127] = '\0';
        } else {
            exec_path[0] = '\0';
        }
        rcu_read_unlock();
        size_t len = strnlen(exec_path, 128);
        if (len + 1 > buflen)
            return -ENAMETOOLONG;
        memmove(buf, exec_path, len + 1);
        return (ssize_t)len;
    }

    case PROC_PID_NS_ENTRY:
    case PROC_TASK_NS_ENTRY: {
        if (pi->ns_idx < 0 || pi->ns_idx >= NELEM(procfs_ns_entries))
            return -EINVAL;
        n = snprintf(buf, buflen, "%s:[%llu]",
                     procfs_ns_entries[pi->ns_idx].name,
                     (unsigned long long)procfs_ns_entries[pi->ns_idx].initial_ino);
        return n;
    }

    case PROC_FD_ENTRY: {
        struct vfs_file *custom_file = NULL;
        rcu_read_lock();
        struct thread *p = NULL;
        get_pid_thread(pi->pid, &p);
        if (p == NULL) {
            rcu_read_unlock();
            return -ESRCH;
        }
        if (p->fdtable == NULL || pi->fd >= NOFILE) {
            rcu_read_unlock();
            return -EBADF;
        }
        struct vfs_file *f = p->fdtable->files[pi->fd];
        if (f == NULL) {
            rcu_read_unlock();
            return -EBADF;
        }
        if (f->ops != NULL && f->ops->readlink != NULL)
            custom_file = vfs_fdup(f);
        /* Read the inode pointer from the file reference under RCU */
        struct vfs_inode *fi = f->inode.inode;
        mode_t fmode = (fi != NULL) ? fi->mode : 0;
        uint64 fino  = (fi != NULL) ? fi->ino  : 0;
        uint32 fdev  = 0;
        if (fi != NULL && S_ISCHR(fmode))
            fdev = fi->cdev;
        rcu_read_unlock();

        if (custom_file != NULL) {
            n = custom_file->ops->readlink(custom_file, buf, buflen);
            vfs_fput(custom_file);
        } else if (S_ISFIFO(fmode))
            n = snprintf(buf, buflen, "pipe:[%llu]",
                         (unsigned long long)fino);
        else if (S_ISSOCK(fmode))
            n = snprintf(buf, buflen, "socket:[%llu]",
                         (unsigned long long)fino);
        else if (S_ISCHR(fmode) && fdev != 0) {
            /* Look up char device to get its devtmpfs name */
            cdev_t *cd = cdev_get(major(fdev), minor(fdev));
            if (!IS_ERR_OR_NULL(cd) && cd->dev.devname != NULL) {
                n = snprintf(buf, buflen, "/dev/%s", cd->dev.devname);
                cdev_put(cd);
            } else {
                if (!IS_ERR_OR_NULL(cd))
                    cdev_put(cd);
                n = snprintf(buf, buflen, "/dev/char/%u:%u",
                             major(fdev), minor(fdev));
            }
        } else
            n = snprintf(buf, buflen, "file:[%llu]",
                         (unsigned long long)fino);
        return n;
    }

    default:
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/*  procfs_getattr – fill a minimal stat structure                    */
/* ------------------------------------------------------------------ */

static int procfs_getattr(struct vfs_inode *inode, struct stat *st) {
    struct procfs_inode *pi = procfs_i(inode);

    memset(st, 0, sizeof(*st));
    st->st_ino   = inode->ino;
    st->st_mode  = inode->mode;
    st->st_nlink = inode->n_links;
    st->st_size  = S_ISREG(inode->mode) ? 0 : inode->size;
    st->st_dev   = 0;
    st->st_blksize = 4096;
    if (pi->type == PROC_TASKDIR) {
        int tids = procfs_count_tids(pi->pid);
        st->st_nlink = 2 + (tids > 0 ? (uint64)tids : 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Content generation helpers (called from procfs_open)             */
/* ------------------------------------------------------------------ */

#define PROCFS_BUF_SIZE 4096
#define PROCFS_MAPS_INITIAL_BUF_SIZE (64 * 1024)
#define PROCFS_MAPS_MAX_BUF_SIZE     (1024 * 1024)
#define PROCFS_MAPS_LINE_RESERVE     (VFS_USER_PATH_MAX + 160)
#define PROCFS_SMAPS_ENTRY_RESERVE   (VFS_USER_PATH_MAX + 1024)

struct procfs_vm_accounting {
    vm_t *vm;
    uint64 size_pages;
    uint64 resident_pages;
    uint64 shared_pages;
    uint64 text_pages;
    uint64 data_pages;
    uint64 stack_pages;
    uint64 exec_pages;
    uint64 file_pages;
};

static void procfs_vm_count_leaf(uint64 va, uint64 size,
                                 pte_t pte __attribute__((unused)),
                                 void *arg)
{
    struct procfs_vm_accounting *acct = arg;
    uint64 check = va;
    vma_t *vma = mt_find(&acct->vm->vm_mt, &check, va + size - 1);
    if (vma == NULL || vma->start > va || vma->end < va + size)
        return;

    uint64 pages = size / PGSIZE;
    acct->resident_pages += pages;
    if (vma->flags & VMA_FLAG_SHARED)
        acct->shared_pages += pages;
    if (vma->flags & PROT_EXEC)
        acct->text_pages += pages;
    if (vma->flags & VMA_FLAG_FILE)
        acct->file_pages += pages;
}

static void procfs_collect_vm_accounting(vm_t *vm,
                                         struct procfs_vm_accounting *acct)
{
    memset(acct, 0, sizeof(*acct));
    acct->vm = vm;
    if (vm == NULL)
        return;

    vm_rlock(vm);
    vma_t *vma;
    uint64 index = 0;
    mt_for_each(&vm->vm_mt, vma, index, UVMTOP - 1) {
        if ((vma->flags & VMA_FLAG_USER) == 0)
            continue;
        uint64 pages = (vma->end - vma->start) / PGSIZE;
        acct->size_pages += pages;
        if (vma->flags & VMA_FLAG_GROWSDOWN)
            acct->stack_pages += pages;
        if (vma->flags & PROT_EXEC)
            acct->exec_pages += pages;
        if ((vma->flags & PROT_WRITE) || (vma->flags & VMA_FLAG_GROWSUP) ||
            (vma->flags & VMA_FLAG_GROWSDOWN))
            acct->data_pages += pages;
    }
    uvm_visit_present_leafs(vm->pagetable, procfs_vm_count_leaf, acct);
    vm_runlock(vm);
}

static char *procfs_gen_status(int tgid) {
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    if (p == NULL) {
        rcu_read_unlock();
        return ERR_PTR(-ESRCH);
    }
    char name[17];
    memmove(name, p->name, 16);
    name[16] = '\0';
    int              ppid     = (p->parent != NULL) ? p->parent->tgid : 0;
    const char      *statestr = thread_state_short(__thread_state_get(p));
    const char      *statelong = thread_state_to_str(__thread_state_get(p));
    vm_t            *vm       = p->vm;
    int              real_tgid = p->tgid;
    int              pid      = p->pid;
    int              pgid     = p->pgid;
    int              sid      = p->sid;
    int              threads  = 1;
    int              kthread  = THREAD_USER_SPACE(p) ? 0 : 1;
    uint64           sigpnd   = smp_load_acquire(&p->signal.sig_pending_mask);
    uint64           sigblk   = p->signal.sig_mask;
    uint64           shdpnd   = 0;
    uint32           p_uid    = 0;
    uint32           p_gid    = 0;
    uint32           p_euid   = 0;
    uint32           p_egid   = 0;
    uint32           p_suid   = 0;
    uint32           p_sgid   = 0;
    uint32           groups[NGROUPS_MAX];
    int              ngroups  = 0;
    /* Cumulative CPU time (raw timer ticks at TIMEBASE_FREQUENCY Hz) */
    uint64 cputime_raw = 0;
    uint32 util_avg = 0;
    uint64 load_contrib = 0;
    if (p->sched_entity) {
        cputime_raw = p->sched_entity->sum_exec_runtime;
        util_avg = p->sched_entity->util_avg;
        load_contrib = p->sched_entity->load_avg_contrib;
    }
    /* Credentials */
    if (p->thread_group) {
        struct thread_group *tg = p->thread_group;
        threads = __atomic_load_n(&tg->live_threads, __ATOMIC_ACQUIRE);
        shdpnd = smp_load_acquire(&tg->shared_pending.sig_pending_mask);
        p_uid = tg->uid;
        p_gid = tg->gid;
        p_euid = tg->euid;
        p_egid = tg->egid;
        p_suid = tg->suid;
        p_sgid = tg->sgid;
        ngroups = tg->ngroups;
        if (ngroups > NGROUPS_MAX)
            ngroups = NGROUPS_MAX;
        for (int i = 0; i < ngroups; i++)
            groups[i] = tg->groups[i];
    }
    rcu_read_unlock();

    struct procfs_vm_accounting vmacct;
    procfs_collect_vm_accounting(vm, &vmacct);

    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    unsigned long vm_size_kb = (unsigned long)((vmacct.size_pages * PGSIZE) / 1024);
    unsigned long vm_rss_kb = (unsigned long)((vmacct.resident_pages * PGSIZE) / 1024);
    unsigned long rss_file_kb = (unsigned long)((vmacct.file_pages * PGSIZE) / 1024);
    if (rss_file_kb > vm_rss_kb)
        rss_file_kb = vm_rss_kb;
    unsigned long rss_anon_kb = vm_rss_kb - rss_file_kb;
    unsigned long vm_data_kb = (unsigned long)((vmacct.data_pages * PGSIZE) / 1024);
    unsigned long vm_stk_kb = (unsigned long)((vmacct.stack_pages * PGSIZE) / 1024);
    unsigned long vm_exe_kb = (unsigned long)((vmacct.exec_pages * PGSIZE) / 1024);
    unsigned long vm_pte_kb = (unsigned long)((vmacct.size_pages * sizeof(pte_t)) / 1024);
    if (vmacct.size_pages != 0 && vm_pte_kb == 0)
        vm_pte_kb = 4;

    int pos = snprintf(buf, PROCFS_BUF_SIZE,
                       "Name:\t%s\n"
                       "Umask:\t%u\n"
                       "State:\t%s (%s)\n"
                       "Tgid:\t%d\n"
                       "Ngid:\t0\n"
                       "Pid:\t%d\n"
                       "PPid:\t%d\n"
                       "TracerPid:\t0\n"
                       "Uid:\t%u\t%u\t%u\t%u\n"
                       "Gid:\t%u\t%u\t%u\t%u\n"
                       "FDSize:\t%d\n"
                       "Groups:\t",
                       name, 22, statestr, statelong, real_tgid, pid, ppid,
                       (unsigned)p_uid, (unsigned)p_euid,
                       (unsigned)p_suid, (unsigned)p_euid,
                       (unsigned)p_gid, (unsigned)p_egid,
                       (unsigned)p_sgid, (unsigned)p_egid,
                       NOFILE);
    if (pos < 0)
        pos = 0;
    if ((size_t)pos < PROCFS_BUF_SIZE && ngroups == 0) {
        int n = snprintf(buf + pos, PROCFS_BUF_SIZE - (size_t)pos, " ");
        if (n > 0)
            pos += n;
    }
    if ((size_t)pos < PROCFS_BUF_SIZE) {
        for (int i = 0; i < ngroups && (size_t)pos < PROCFS_BUF_SIZE; i++) {
            int n = snprintf(buf + pos, PROCFS_BUF_SIZE - (size_t)pos,
                             "%u ", (unsigned)groups[i]);
            if (n < 0)
                break;
            pos += n;
        }
    }
    int ncpu = cpu_possible_count();
    unsigned cpu_mask = (ncpu >= (int)(sizeof(unsigned) * 8))
        ? ~0U
        : ((1U << ncpu) - 1U);
    char cpu_list[24];
    if (ncpu == 1)
        snprintf(cpu_list, sizeof(cpu_list), "0");
    else
        snprintf(cpu_list, sizeof(cpu_list), "0-%d", ncpu - 1);

    if ((size_t)pos < PROCFS_BUF_SIZE) {
        snprintf(buf + pos, PROCFS_BUF_SIZE - (size_t)pos,
                 "\n"
                 "NStgid:\t%d\n"
                 "NSpid:\t%d\n"
                 "NSpgid:\t%d\n"
                 "NSsid:\t%d\n"
                 "Kthread:\t%d\n"
                 "VmPeak:\t%8lu kB\n"
                 "VmSize:\t%8lu kB\n"
                 "VmLck:\t%8u kB\n"
                 "VmPin:\t%8u kB\n"
                 "VmHWM:\t%8lu kB\n"
                 "VmRSS:\t%8lu kB\n"
                 "RssAnon:\t%8lu kB\n"
                 "RssFile:\t%8lu kB\n"
                 "RssShmem:\t%8u kB\n"
                 "VmData:\t%8lu kB\n"
                 "VmStk:\t%8lu kB\n"
                 "VmExe:\t%8lu kB\n"
                 "VmLib:\t%8u kB\n"
                 "VmPTE:\t%8lu kB\n"
                 "VmSwap:\t%8u kB\n"
                 "HugetlbPages:\t%8u kB\n"
                 "CoreDumping:\t0\n"
                 "THP_enabled:\t0\n"
                 "Threads:\t%d\n"
                 "SigQ:\t0/%d\n"
                 "SigPnd:\t%016llx\n"
                 "ShdPnd:\t%016llx\n"
                 "SigBlk:\t%016llx\n"
                 "SigIgn:\t%016llx\n"
                 "SigCgt:\t%016llx\n"
                 "CapInh:\t%016llx\n"
                 "CapPrm:\t%016llx\n"
                 "CapEff:\t%016llx\n"
                 "CapBnd:\t%016llx\n"
                 "CapAmb:\t%016llx\n"
                 "NoNewPrivs:\t0\n"
                 "Seccomp:\t0\n"
                 "Seccomp_filters:\t0\n"
                 "Cpus_allowed:\t%08x\n"
                 "Cpus_allowed_list:\t%s\n"
                 "Mems_allowed:\t00000001\n"
                 "Mems_allowed_list:\t0\n"
                 "voluntary_ctxt_switches:\t0\n"
                 "nonvoluntary_ctxt_switches:\t0\n"
                 "CpuTime:\t%lu\n"
                 "UtilAvg:\t%u\n"
                 "LoadContrib:\t%lu\n",
                 real_tgid, pid, pgid, sid, kthread,
                 vm_size_kb, vm_size_kb, 0, 0, vm_rss_kb, vm_rss_kb,
                 rss_anon_kb, rss_file_kb, 0, vm_data_kb, vm_stk_kb,
                 vm_exe_kb, 0, vm_pte_kb, 0, 0, threads, NSIG,
                 (unsigned long long)sigpnd,
                 (unsigned long long)shdpnd,
                 (unsigned long long)sigblk,
                 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL,
                 cpu_mask, cpu_list,
                 (unsigned long)cputime_raw,
                 (unsigned)util_avg,
                 (unsigned long)load_contrib);
    }
    return buf;
}

static char *procfs_gen_statm(int tgid) {
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    vm_t *vm = (p != NULL) ? p->vm : NULL;
    rcu_read_unlock();

    if (vm == NULL)
        return ERR_PTR(-ESRCH);

    struct procfs_vm_accounting acct;
    procfs_collect_vm_accounting(vm, &acct);

    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    /*
     * Linux /proc/<pid>/statm fields, in pages:
     * size resident shared text lib data dt.  lib and dt are legacy fields.
     */
    snprintf(buf, PROCFS_BUF_SIZE, "%lu %lu %lu %lu 0 %lu 0\n",
             (unsigned long)acct.size_pages,
             (unsigned long)acct.resident_pages,
             (unsigned long)acct.shared_pages,
             (unsigned long)acct.text_pages,
             (unsigned long)acct.data_pages);
    return buf;
}

static size_t procfs_strnlen(const char *s, size_t max)
{
    size_t len = 0;
    while (len < max && s[len] != '\0')
        len++;
    return len;
}

static int procfs_format_vma_header(char *buf, size_t size, vma_t *vma)
{
    char r = (vma->flags & PROT_READ) ? 'r' : '-';
    char w = (vma->flags & PROT_WRITE) ? 'w' : '-';
    char x = (vma->flags & PROT_EXEC) ? 'x' : '-';
    char s = (vma->flags & VMA_FLAG_SHARED) ? 's' : 'p';
    uint64 dev = 0;
    uint64 ino = 0;
    const char *path = NULL;

    if ((vma->flags & VMA_FLAG_FILE) && vma->file != NULL) {
        struct vfs_inode *inode = vfs_inode_deref(&vma->file->inode);
        if (inode != NULL) {
            struct stat st;

            ino = inode->ino;
            if (inode->ops != NULL && inode->ops->getattr != NULL &&
                inode->ops->getattr(inode, &st) == 0) {
                dev = st.st_dev;
                ino = st.st_ino;
            }
        }
        if (vma->file->opened_path != NULL &&
            vma->file->opened_path[0] != '\0')
            path = vma->file->opened_path;
    }

    if (path != NULL) {
        size_t path_len = procfs_strnlen(path, VFS_USER_PATH_MAX);
        int n = snprintf(buf, size,
                         "%016llx-%016llx %c%c%c%c %08llx %02llx:%02llx %llu ",
                         (unsigned long long)vma->start,
                         (unsigned long long)vma->end,
                         r, w, x, s,
                         (unsigned long long)vma->pgoff,
                         (unsigned long long)major(dev),
                         (unsigned long long)minor(dev),
                         (unsigned long long)ino);
        if (n < 0)
            return n;

        size_t need = (size_t)n + path_len + 1;
        if ((size_t)n < size) {
            size_t pos = (size_t)n;
            size_t room = size - pos;
            size_t copy = path_len;
            if (copy >= room)
                copy = (room > 0) ? room - 1 : 0;
            if (copy > 0) {
                memmove(buf + pos, path, copy);
                pos += copy;
                room -= copy;
            }
            if (room > 1) {
                buf[pos++] = '\n';
                buf[pos] = '\0';
            } else if (size > 0) {
                buf[size - 1] = '\0';
            }
        }
        return (int)need;
    }

    return snprintf(buf, size,
                    "%016llx-%016llx %c%c%c%c %08llx %02llx:%02llx %llu \n",
                    (unsigned long long)vma->start,
                    (unsigned long long)vma->end,
                    r, w, x, s,
                    (unsigned long long)vma->pgoff,
                    (unsigned long long)major(dev),
                    (unsigned long long)minor(dev),
                    (unsigned long long)ino);
}

static char *procfs_gen_maps(int tgid) {
    /* Get the vm pointer under RCU; then iterate VMAs under vm_rlock */
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    vm_t *vm = (p != NULL) ? p->vm : NULL;
    rcu_read_unlock();

    if (vm == NULL)
        return ERR_PTR(-ESRCH);

    size_t buf_size = PROCFS_MAPS_INITIAL_BUF_SIZE;

    for (;;) {
        char *buf = kvmalloc(buf_size);
        if (buf == NULL)
            return ERR_PTR(-ENOMEM);

        size_t pos = 0;
        bool truncated = false;

        vm_rlock(vm);
        vma_t  *vma;
        uint64  index = 0;
        for (;;) {
            vma = mt_find(&vm->vm_mt, &index, UVMTOP - 1);
            if (vma == NULL)
                break;
            if ((vma->flags & VMA_FLAG_USER) == 0)
                continue;
            if (pos >= buf_size ||
                buf_size - pos < PROCFS_MAPS_LINE_RESERVE) {
                truncated = true;
                break;
            }
            int n = procfs_format_vma_header(buf + pos, buf_size - pos, vma);
            if (n < 0 || (size_t)n >= buf_size - pos) {
                truncated = true;
                break;
            }
            pos += (size_t)n;
        }
        vm_runlock(vm);

        if (!truncated) {
            buf[pos] = '\0';
            return buf;
        }

        kvfree(buf);
        if (buf_size >= PROCFS_MAPS_MAX_BUF_SIZE)
            return ERR_PTR(-EOVERFLOW);
        buf_size *= 2;
        if (buf_size > PROCFS_MAPS_MAX_BUF_SIZE)
            buf_size = PROCFS_MAPS_MAX_BUF_SIZE;
    }
}

static char *procfs_gen_meminfo(void) {
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    const struct mm_watermark_state *wm = mm_watermark_get_state();
    uint64 total_pages = (wm != NULL) ? wm->total_pages : 0;
    if (total_pages == 0)
        total_pages = get_total_free_pages();

    uint64 free_pages = get_total_free_pages();
    uint64 cached_pages = (wm != NULL) ? wm->cached_pages : 0;
    uint64 low_pages = (wm != NULL) ? wm->wmark[WMARK_LOW] : 0;
    uint64 high_pages = (wm != NULL) ? wm->wmark[WMARK_HIGH] : low_pages;
    uint64 available_pages = free_pages;
    if (available_pages > low_pages)
        available_pages -= low_pages;
    if (available_pages > total_pages)
        available_pages = total_pages;

    uint64 total_kb = (total_pages * PGSIZE) / 1024;
    uint64 free_kb = (free_pages * PGSIZE) / 1024;
    uint64 available_kb = (available_pages * PGSIZE) / 1024;
    uint64 cached_kb = (cached_pages * PGSIZE) / 1024;
    uint64 active_file_kb = cached_kb / 2;
    uint64 inactive_file_kb = cached_kb - active_file_kb;
    uint64 commit_limit_kb = total_kb;
    uint64 slab_kb = (high_pages * PGSIZE) / 1024;

    snprintf(buf, PROCFS_BUF_SIZE,
             "MemTotal:        %8lu kB\n"
             "MemFree:         %8lu kB\n"
             "MemAvailable:    %8lu kB\n"
             "Buffers:         %8u kB\n"
             "Cached:          %8lu kB\n"
             "SwapCached:      %8u kB\n"
             "Active:          %8lu kB\n"
             "Inactive:        %8lu kB\n"
             "Active(anon):    %8u kB\n"
             "Inactive(anon):  %8u kB\n"
             "Active(file):    %8lu kB\n"
             "Inactive(file):  %8lu kB\n"
             "Unevictable:     %8u kB\n"
             "Mlocked:         %8u kB\n"
             "SwapTotal:       %8u kB\n"
             "SwapFree:        %8u kB\n"
             "Dirty:           %8u kB\n"
             "Writeback:       %8u kB\n"
             "AnonPages:       %8u kB\n"
             "Mapped:          %8u kB\n"
             "Shmem:           %8u kB\n"
             "KReclaimable:    %8lu kB\n"
             "Slab:            %8lu kB\n"
             "SReclaimable:    %8lu kB\n"
             "SUnreclaim:      %8u kB\n"
             "KernelStack:     %8u kB\n"
             "PageTables:      %8u kB\n"
             "CommitLimit:     %8lu kB\n"
             "Committed_AS:    %8u kB\n"
             "VmallocTotal:    %8lu kB\n"
             "VmallocUsed:     %8u kB\n"
             "VmallocChunk:    %8lu kB\n",
             (unsigned long)total_kb,
             (unsigned long)free_kb,
             (unsigned long)available_kb,
             0,
             (unsigned long)cached_kb,
             0,
             (unsigned long)active_file_kb,
             (unsigned long)inactive_file_kb,
             0,
             0,
             (unsigned long)active_file_kb,
             (unsigned long)inactive_file_kb,
             0,
             0,
             0,
             0,
             0,
             0,
             0,
             0,
             0,
             (unsigned long)slab_kb,
             (unsigned long)slab_kb,
             (unsigned long)slab_kb,
             0,
             0,
             0,
             (unsigned long)commit_limit_kb,
             0,
             (unsigned long)(~0ULL / 1024),
             0,
             (unsigned long)(~0ULL / 1024));
    return buf;
}

static char *procfs_gen_zoneinfo(void) {
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    const struct mm_watermark_state *wm = mm_watermark_get_state();
    uint64 total_pages = (wm != NULL) ? wm->total_pages : 0;
    if (total_pages == 0)
        total_pages = get_total_free_pages();
    uint64 free_pages = get_total_free_pages();
    uint64 min_pages = (wm != NULL) ? wm->wmark[WMARK_MIN] : 0;
    uint64 low_pages = (wm != NULL) ? wm->wmark[WMARK_LOW] : 0;
    uint64 high_pages = (wm != NULL) ? wm->wmark[WMARK_HIGH] : 0;

    snprintf(buf, PROCFS_BUF_SIZE,
             "Node 0, zone   Normal\n"
             "  pages free     %lu\n"
             "        min      %lu\n"
             "        low      %lu\n"
             "        high     %lu\n"
             "        spanned  %lu\n"
             "        present  %lu\n"
             "        managed  %lu\n"
             "        protection: (0, 0, 0)\n",
             (unsigned long)free_pages,
             (unsigned long)min_pages,
             (unsigned long)low_pages,
             (unsigned long)high_pages,
             (unsigned long)total_pages,
             (unsigned long)total_pages,
             (unsigned long)total_pages);
    return buf;
}

static char *procfs_gen_cgroup(int tgid) {
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    rcu_read_unlock();
    if (p == NULL)
        return ERR_PTR(-ESRCH);

    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE, "0::/init.scope\n");
    return buf;
}

static char *procfs_gen_cpuinfo(void) {
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
#ifdef __x86_64__
    int pos = 0;
    int ncpu = cpu_possible_count();
    const char *vendor = "GenuineIntel";
    const char *brand = "xv6 Linux ABI Virtual CPU";
    const char *flags =
        "fpu tsc msr pae cx8 apic sep cmov mmx fxsr sse sse2 syscall nx lm";

    for (int cpu = 0; cpu < ncpu && (size_t)pos < PROCFS_BUF_SIZE; cpu++) {
        size_t rem = PROCFS_BUF_SIZE - (size_t)pos;
        int n = snprintf(buf + pos, rem,
                         "processor\t: %d\n"
                         "vendor_id\t: %s\n"
                         "cpu family\t: 6\n"
                         "model\t\t: 158\n"
                         "model name\t: %s\n"
                         "stepping\t: 10\n"
                         "microcode\t: 0x0\n"
                         "cpu MHz\t\t: 0.000\n"
                         "cache size\t: 0 KB\n"
                         "physical id\t: 0\n"
                         "siblings\t: %d\n"
                         "core id\t\t: %d\n"
                         "cpu cores\t: %d\n"
                         "apicid\t\t: %d\n"
                         "initial apicid\t: %d\n"
                         "fpu\t\t: yes\n"
                         "fpu_exception\t: yes\n"
                         "cpuid level\t: 13\n"
                         "wp\t\t: yes\n"
                         "flags\t\t: %s\n"
                         "bugs\t\t:\n"
                         "bogomips\t: 0.00\n"
                         "clflush size\t: 64\n"
                         "cache_alignment\t: 64\n"
                         "address sizes\t: 40 bits physical, 48 bits virtual\n"
                         "power management:\n"
                         "\n",
                         cpu, vendor, brand, ncpu, cpu, ncpu, cpu, cpu,
                         flags);
        if (n < 0 || (size_t)n >= rem) {
            pos = PROCFS_BUF_SIZE - 1;
            break;
        }
        pos += n;
    }
#else
    snprintf(buf, PROCFS_BUF_SIZE,
             "processor\t: 0\n"
             "model name\t: RISC-V\n"
             "isa\t\t: rv64imafdc\n"
             "mmu\t\t: sv39\n");
#endif
    return buf;
}

static char *procfs_gen_cmdline(void) {
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    if (platform.has_cmdline)
        snprintf(buf, PROCFS_BUF_SIZE, "%s\n", platform.cmdline);
    else
        buf[0] = '\0';
    return buf;
}

static char *procfs_gen_version(void)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE,
             "Linux version 6.6.0-xv6 (xv6-os) #1 SMP PREEMPT %s %s\n",
             __DATE__, __TIME__);
    return buf;
}

static char *procfs_gen_uptime(void)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    uint64 ms = get_jiffs();
    snprintf(buf, PROCFS_BUF_SIZE, "%lu.%02lu %lu.%02lu\n",
             (unsigned long)(ms / 1000),
             (unsigned long)((ms % 1000) / 10),
             (unsigned long)(ms / 1000),
             (unsigned long)((ms % 1000) / 10));
    return buf;
}

static char *procfs_gen_stat(void)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    uint64 j = get_jiffs();
    uint64 now_sec = goldfish_rtc_read_ns() / NS_PER_SEC;
    uint64 boot_sec = now_sec > (j / 1000) ? now_sec - (j / 1000) : 0;
    int ncpu = cpu_possible_count();
    uint64 user = j / 20;
    uint64 system = j / 20;
    uint64 idle = j > user + system ? j - user - system : 0;
    int pos = snprintf(buf, PROCFS_BUF_SIZE,
                       "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n",
                       (unsigned long)user,
                       (unsigned long)system,
                       (unsigned long)idle);
    for (int cpu = 0; cpu < ncpu && (size_t)pos < PROCFS_BUF_SIZE; cpu++) {
        int n = snprintf(buf + pos, PROCFS_BUF_SIZE - (size_t)pos,
                         "cpu%d %lu 0 %lu %lu 0 0 0 0 0 0\n",
                         cpu, (unsigned long)(user / ncpu),
                         (unsigned long)(system / ncpu),
                         (unsigned long)(idle / ncpu));
        if (n < 0)
            break;
        pos += n;
    }
    if ((size_t)pos < PROCFS_BUF_SIZE) {
        snprintf(buf + pos, PROCFS_BUF_SIZE - (size_t)pos,
                 "intr 0\n"
                 "ctxt 0\n"
                 "btime %lu\n"
                 "processes 0\n"
                 "procs_running 1\n"
                 "procs_blocked 0\n",
                 (unsigned long)boot_sec);
    }
    return buf;
}

static char *procfs_gen_loadavg(void)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE, "0.00 0.00 0.00 1/%d %d\n",
             NR_THREAD, current ? current->tgid : 1);
    return buf;
}

static char *procfs_gen_filesystems(void)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE,
             "nodev\tproc\n"
             "nodev\tsysfs\n"
             "nodev\ttmpfs\n"
             "\text4\n");
    return buf;
}

static char *procfs_gen_mounts(void)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE,
             "rootfs / ext4 rw 0 0\n"
             "devtmpfs /dev devtmpfs rw,nosuid 0 0\n"
             "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n"
             "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
             "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
             "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n");
    return buf;
}

static char *procfs_gen_mountinfo(void)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    /*
     * Linux /proc/<pid>/mountinfo format:
     * mount-id parent-id major:minor root mountpoint options optional - type
     * source super-options.  GLib/GIO parses this before falling back to
     * /proc/mounts, so keep it structurally faithful even though xv6 has a
     * simple fixed mount topology.
     */
    snprintf(buf, PROCFS_BUF_SIZE,
             "21 0 8:1 / / rw,relatime - ext4 rootfs rw\n"
             "22 21 0:2 / /dev rw,nosuid - devtmpfs devtmpfs rw\n"
             "23 22 0:5 / /dev/shm rw,nosuid,nodev - tmpfs tmpfs rw\n"
             "24 21 0:3 / /proc rw,nosuid,nodev,noexec,relatime - proc proc rw\n"
             "25 21 0:6 / /sys rw,nosuid,nodev,noexec,relatime - sysfs sysfs rw\n"
             "26 21 0:4 / /tmp rw,nosuid,nodev - tmpfs tmpfs rw\n");
    return buf;
}

static char *procfs_gen_limits(void)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE,
             "Limit                     Soft Limit           Hard Limit           Units     \n"
             "Max cpu time              unlimited            unlimited            seconds   \n"
             "Max file size             unlimited            unlimited            bytes     \n"
             "Max data size             unlimited            unlimited            bytes     \n"
             "Max stack size            unlimited            unlimited            bytes     \n"
             "Max core file size        0                    0                    bytes     \n"
             "Max resident set          unlimited            unlimited            bytes     \n"
             "Max processes             %d                  %d                  processes \n"
             "Max open files            %d                  %d                  files     \n"
             "Max address space         unlimited            unlimited            bytes     \n",
             NR_THREAD, NR_THREAD, NOFILE, NOFILE);
    return buf;
}

static char *procfs_gen_setgroups(void)
{
    char *buf = kvmalloc(16);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, 16, "allow\n");
    return buf;
}

static char *procfs_gen_fdinfo(int pid, int fd)
{
    char *buf = kvmalloc(256);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(pid, &p);
    if (p == NULL || p->fdtable == NULL || fd < 0 || fd >= NOFILE) {
        rcu_read_unlock();
        kvfree(buf);
        return ERR_PTR(-ESRCH);
    }

    struct vfs_fdtable *ft = p->fdtable;
    spin_lock(&ft->lock);
    struct vfs_file *f = ft->files[fd];
    if (f == NULL) {
        spin_unlock(&ft->lock);
        rcu_read_unlock();
        kvfree(buf);
        return ERR_PTR(-EBADF);
    }

    int fdflags = 0;
    if (bits_test_bit64(&ft->cloexec_bitmap[fd >> 6], fd & 63))
        fdflags |= O_CLOEXEC;
    int flags = f->f_flags | fdflags;
    struct vfs_file *stat_file =
        (f->ops != NULL && f->ops->stat != NULL) ? vfs_fdup(f) : NULL;
    struct vfs_inode *fi = f->inode.inode;
    loff_t pos = (fi != NULL && S_ISREG(fi->mode)) ? f->f_pos : 0;
    uint64 ino = fi != NULL ? fi->ino : 0;
    spin_unlock(&ft->lock);
    rcu_read_unlock();

    if (stat_file != NULL) {
        struct stat st;

        if (vfs_filestat(stat_file, &st) == 0)
            ino = st.st_ino;
        vfs_fput(stat_file);
    }

    char flagbuf[11];
    flagbuf[sizeof(flagbuf) - 1] = '\0';
    uint value = (uint)flags;
    for (int i = (int)sizeof(flagbuf) - 2; i >= 0; i--) {
        flagbuf[i] = (char)('0' + (value & 7));
        value >>= 3;
    }

    snprintf(buf, 256, "pos:\t%lld\nflags:\t%s\nmnt_id:\t0\nino:\t%llu\n",
             (long long)pos, flagbuf, (unsigned long long)ino);
    return buf;
}

static char *procfs_gen_smaps(int tgid)
{
    /* A conservative smaps implementation: one header per VMA plus the
     * Linux fields most parsers look for.  Values are synthetic but stable. */
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    vm_t *vm = (p != NULL) ? p->vm : NULL;
    rcu_read_unlock();

    if (vm == NULL)
        return ERR_PTR(-ESRCH);

    size_t buf_size = PROCFS_MAPS_INITIAL_BUF_SIZE;
    for (;;) {
        char *buf = kvmalloc(buf_size);
        if (buf == NULL)
            return ERR_PTR(-ENOMEM);

        size_t pos = 0;
        bool truncated = false;

        vm_rlock(vm);
        vma_t *vma;
        uint64 index = 0;
        for (;;) {
            vma = mt_find(&vm->vm_mt, &index, UVMTOP - 1);
            if (vma == NULL)
                break;
            if ((vma->flags & VMA_FLAG_USER) == 0)
                continue;
            unsigned long size_kb =
                (unsigned long)((vma->end - vma->start) / 1024);
            if (pos >= buf_size ||
                buf_size - pos < PROCFS_SMAPS_ENTRY_RESERVE) {
                truncated = true;
                break;
            }
            int n = procfs_format_vma_header(buf + pos, buf_size - pos, vma);
            if (n < 0 || (size_t)n >= buf_size - pos) {
                truncated = true;
                break;
            }
            pos += (size_t)n;

            if (pos >= buf_size) {
                truncated = true;
                break;
            }
            n = snprintf(buf + pos, buf_size - pos,
                         "Size:           %8lu kB\n"
                         "KernelPageSize: %8u kB\n"
                         "MMUPageSize:    %8u kB\n"
                         "Rss:            %8u kB\n"
                         "Pss:            %8u kB\n"
                         "Shared_Clean:   %8u kB\n"
                         "Shared_Dirty:   %8u kB\n"
                         "Private_Clean:  %8u kB\n"
                         "Private_Dirty:  %8u kB\n"
                         "Referenced:     %8u kB\n"
                         "Anonymous:      %8u kB\n"
                         "Swap:           %8u kB\n"
                         "VmFlags: rd wr ex sh mr mw me ac\n",
                         size_kb, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            if (n < 0 || (size_t)n >= buf_size - pos) {
                truncated = true;
                break;
            }
            pos += (size_t)n;
        }
        vm_runlock(vm);

        if (!truncated) {
            buf[pos] = '\0';
            return buf;
        }

        kvfree(buf);
        if (buf_size >= PROCFS_MAPS_MAX_BUF_SIZE)
            return ERR_PTR(-EOVERFLOW);
        buf_size *= 2;
        if (buf_size > PROCFS_MAPS_MAX_BUF_SIZE)
            buf_size = PROCFS_MAPS_MAX_BUF_SIZE;
    }
}

static char *procfs_gen_pid_stat(int tgid)
{
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    if (p == NULL) {
        rcu_read_unlock();
        return ERR_PTR(-ESRCH);
    }
    char name[17];
    memmove(name, p->name, 16);
    name[16] = '\0';
    char state = thread_state_short(__thread_state_get(p))[0];
    int ppid = (p->parent != NULL) ? p->parent->tgid : 0;
    int pgid = p->pgid;
    int sid = p->sid;
    vm_t *vm = p->vm;
    uint64 cputime_raw = 0;
    int priority = LINUX_NICE_BIAS;
    int nice = 0;
    if (p->sched_entity)
        cputime_raw = p->sched_entity->sum_exec_runtime;
    if (p->sched_entity) {
        int sched_priority = p->sched_entity->priority;
        if (IS_EEVDF_PRIORITY(sched_priority))
            nice = sched_priority - EEVDF_PRIORITY_START - LINUX_NICE_BIAS;
        priority = LINUX_NICE_BIAS + nice;
    }
    rcu_read_unlock();

    struct procfs_vm_accounting acct;
    procfs_collect_vm_accounting(vm, &acct);
    uint64 hz = __timebase_frequency ? __timebase_frequency : 10000000UL;
    unsigned long cputime_ticks = (unsigned long)((cputime_raw * HZ) / hz);

    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE,
             "%d (%s) %c %d %d %d 0 -1 4194304 0 0 0 0 %lu 0 0 0 %d %d 1 0 0 %lu %lu 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
             tgid, name, state, ppid, pgid, sid, cputime_ticks,
             priority, nice, (unsigned long)(acct.size_pages * PGSIZE),
             (unsigned long)acct.resident_pages);
    return buf;
}

static struct procfs_blob *procfs_blob_alloc(size_t len)
{
    struct procfs_blob *blob = kvmalloc(sizeof(*blob) + len + 1);
    if (blob == NULL)
        return ERR_PTR(-ENOMEM);
    blob->len = len;
    if (len != 0)
        memset(blob->data, 0, len);
    blob->data[len] = '\0';
    return blob;
}

static struct procfs_blob *procfs_gen_pid_snapshot_blob(int tgid,
                                                        enum procfs_entry_type type)
{
    struct procfs_blob *blob =
        procfs_blob_alloc(TG_EXEC_SNAPSHOT_MAX_BYTES);
    if (IS_ERR(blob))
        return blob;

    pid_rlock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    if (p == NULL || p->thread_group == NULL) {
        pid_runlock();
        kvfree(blob);
        return ERR_PTR(-ESRCH);
    }

    const struct thread_group_exec_snapshot *snap =
        &p->thread_group->exec_snapshot;
    const char *src = NULL;
    size_t len = 0;

    switch (type) {
    case PROC_PID_CMDLINE:
    case PROC_TASK_CMDLINE:
        src = snap->cmdline;
        len = snap->cmdline_len;
        break;
    case PROC_PID_ENVIRON:
    case PROC_TASK_ENVIRON:
        src = snap->environ;
        len = snap->environ_len;
        break;
    case PROC_PID_AUXV:
    case PROC_TASK_AUXV:
        src = (const char *)snap->auxv;
        len = snap->auxv_len;
        break;
    default:
        pid_runlock();
        kvfree(blob);
        return ERR_PTR(-EINVAL);
    }

    if (len > TG_EXEC_SNAPSHOT_MAX_BYTES)
        len = TG_EXEC_SNAPSHOT_MAX_BYTES;
    if (src != NULL && len != 0)
        memmove(blob->data, src, len);
    blob->len = len;
    pid_runlock();
    return blob;
}

static struct procfs_blob *procfs_gen_pid_exec_blob(int tgid,
                                                    enum procfs_entry_type type)
{
    struct procfs_blob *blob =
        procfs_blob_alloc(TG_EXEC_SNAPSHOT_MAX_BYTES);
    if (IS_ERR(blob))
        return blob;

    uint64 addrs[MAXARG > MAXENV ? MAXARG : MAXENV] = {0};
    size_t lens[MAXARG > MAXENV ? MAXARG : MAXENV] = {0};
    size_t count = 0;
    vm_t *target_vm = NULL;

    pid_rlock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    if (p == NULL || p->thread_group == NULL) {
        pid_runlock();
        kvfree(blob);
        return ERR_PTR(-ESRCH);
    }

    const struct thread_group_exec_snapshot *snap =
        &p->thread_group->exec_snapshot;
    const char *fallback = NULL;
    size_t fallback_len = 0;

    switch (type) {
    case PROC_PID_CMDLINE:
    case PROC_TASK_CMDLINE:
        fallback = snap->cmdline;
        fallback_len = snap->cmdline_len;
        count = snap->cmdline_argc;
        if (count > MAXARG)
            count = MAXARG;
        memmove(addrs, snap->cmdline_addrs, count * sizeof(addrs[0]));
        memmove(lens, snap->cmdline_lens, count * sizeof(lens[0]));
        break;
    case PROC_PID_ENVIRON:
    case PROC_TASK_ENVIRON:
        fallback = snap->environ;
        fallback_len = snap->environ_len;
        count = snap->environ_count;
        if (count > MAXENV)
            count = MAXENV;
        memmove(addrs, snap->environ_addrs, count * sizeof(addrs[0]));
        memmove(lens, snap->environ_lens, count * sizeof(lens[0]));
        break;
    default:
        pid_runlock();
        kvfree(blob);
        return ERR_PTR(-EINVAL);
    }

    if (fallback_len > TG_EXEC_SNAPSHOT_MAX_BYTES)
        fallback_len = TG_EXEC_SNAPSHOT_MAX_BYTES;
    if (fallback != NULL && fallback_len != 0)
        memmove(blob->data, fallback, fallback_len);
    blob->len = fallback_len;

    if (p->vm != NULL && count != 0) {
        target_vm = p->vm;
        vm_dup(target_vm);
    }
    pid_runlock();

    if (target_vm == NULL)
        return blob;

    size_t pos = 0;
    int live_ok = 1;
    memset(blob->data, 0, TG_EXEC_SNAPSHOT_MAX_BYTES);
    for (size_t i = 0; i < count && pos < TG_EXEC_SNAPSHOT_MAX_BYTES; i++) {
        size_t len = lens[i];
        if (addrs[i] == 0 || len == 0)
            continue;
        if (len > TG_EXEC_SNAPSHOT_MAX_BYTES - pos)
            len = TG_EXEC_SNAPSHOT_MAX_BYTES - pos;
        if (vm_copyin(target_vm, blob->data + pos, addrs[i], len) != 0) {
            live_ok = 0;
            break;
        }
        pos += len;
    }
    vm_put(target_vm);

    if (live_ok) {
        blob->len = pos;
        blob->data[blob->len] = '\0';
        return blob;
    }

    memset(blob->data, 0, TG_EXEC_SNAPSHOT_MAX_BYTES);
    pid_rlock();
    p = NULL;
    get_pid_thread(tgid, &p);
    if (p == NULL || p->thread_group == NULL) {
        pid_runlock();
        blob->len = 0;
        return blob;
    }
    snap = &p->thread_group->exec_snapshot;
    fallback = NULL;
    fallback_len = 0;
    if (type == PROC_PID_CMDLINE || type == PROC_TASK_CMDLINE) {
        fallback = snap->cmdline;
        fallback_len = snap->cmdline_len;
    } else {
        fallback = snap->environ;
        fallback_len = snap->environ_len;
    }
    if (fallback_len > TG_EXEC_SNAPSHOT_MAX_BYTES)
        fallback_len = TG_EXEC_SNAPSHOT_MAX_BYTES;
    if (fallback != NULL && fallback_len != 0)
        memmove(blob->data, fallback, fallback_len);
    blob->len = fallback_len;
    pid_runlock();
    return blob;
}

static char *procfs_gen_pid_comm(int tgid)
{
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    if (p == NULL) {
        rcu_read_unlock();
        return ERR_PTR(-ESRCH);
    }
    char name[17];
    memmove(name, p->name, 16);
    name[16] = '\0';
    rcu_read_unlock();

    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE, "%s\n", name);
    return buf;
}

static char *procfs_gen_sys_file(enum procfs_entry_type type)
{
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    switch (type) {
    case PROC_SYS_KERNEL_OSTYPE:
        snprintf(buf, PROCFS_BUF_SIZE, "Linux\n");
        break;
    case PROC_SYS_KERNEL_OSRELEASE:
        snprintf(buf, PROCFS_BUF_SIZE, "6.6.0-xv6\n");
        break;
    case PROC_SYS_KERNEL_VERSION:
        snprintf(buf, PROCFS_BUF_SIZE, "#1 SMP PREEMPT %s %s\n",
                 __DATE__, __TIME__);
        break;
    case PROC_SYS_KERNEL_HOSTNAME:
        snprintf(buf, PROCFS_BUF_SIZE, "xv6\n");
        break;
    case PROC_SYS_KERNEL_DOMAINNAME:
        snprintf(buf, PROCFS_BUF_SIZE, "(none)\n");
        break;
    case PROC_SYS_KERNEL_RANDOMIZE_VA_SPACE:
        snprintf(buf, PROCFS_BUF_SIZE, "0\n");
        break;
    case PROC_SYS_KERNEL_PID_MAX:
        snprintf(buf, PROCFS_BUF_SIZE, "%d\n", MAXPID);
        break;
    case PROC_SYS_KERNEL_THREADS_MAX:
        snprintf(buf, PROCFS_BUF_SIZE, "%d\n", NR_THREAD);
        break;
    case PROC_SYS_VM_OVERCOMMIT_MEMORY:
        snprintf(buf, PROCFS_BUF_SIZE, "0\n");
        break;
    case PROC_SYS_VM_OVERCOMMIT_RATIO:
        snprintf(buf, PROCFS_BUF_SIZE, "50\n");
        break;
    case PROC_SYS_VM_MAX_MAP_COUNT:
        snprintf(buf, PROCFS_BUF_SIZE, "65530\n");
        break;
    case PROC_SYS_VM_MMAP_MIN_ADDR:
        snprintf(buf, PROCFS_BUF_SIZE, "4096\n");
        break;
    case PROC_SYS_VM_SWAPPINESS:
        snprintf(buf, PROCFS_BUF_SIZE, "0\n");
        break;
    case PROC_SYS_VM_DIRTY_RATIO:
        snprintf(buf, PROCFS_BUF_SIZE, "20\n");
        break;
    case PROC_SYS_VM_DIRTY_BACKGROUND_RATIO:
        snprintf(buf, PROCFS_BUF_SIZE, "10\n");
        break;
    case PROC_SYS_FS_FILE_MAX:
        snprintf(buf, PROCFS_BUF_SIZE, "%d\n", NFILE);
        break;
    case PROC_SYS_FS_FILE_NR:
        snprintf(buf, PROCFS_BUF_SIZE, "0\t0\t%d\n", NFILE);
        break;
    case PROC_SYS_FS_NR_OPEN:
        snprintf(buf, PROCFS_BUF_SIZE, "%d\n", NOFILE);
        break;
    case PROC_SYS_FS_PIPE_MAX_SIZE:
        snprintf(buf, PROCFS_BUF_SIZE, "1048576\n");
        break;
    case PROC_SYS_FS_INOTIFY_MAX_USER_WATCHES:
        snprintf(buf, PROCFS_BUF_SIZE, "8192\n");
        break;
    case PROC_SYS_FS_INOTIFY_MAX_USER_INSTANCES:
        snprintf(buf, PROCFS_BUF_SIZE, "128\n");
        break;
    case PROC_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS:
        snprintf(buf, PROCFS_BUF_SIZE, "16384\n");
        break;
    default:
        kvfree(buf);
        return ERR_PTR(-EINVAL);
    }
    return buf;
}

#define PROCFS_RESOURCES_BUF_SIZE 2048

static char *procfs_gen_resources(int tgid) {
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
    if (p == NULL || p->thread_group == NULL) {
        rcu_read_unlock();
        return ERR_PTR(-ESRCH);
    }
    struct thread_group *tg = p->thread_group;
    rcu_read_unlock();

    char *buf = kvmalloc(PROCFS_RESOURCES_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    acct_format(tg, buf, PROCFS_RESOURCES_BUF_SIZE);
    return buf;
}

static int procfs_open_mem(struct procfs_inode *pi, struct vfs_file *file,
                           int f_flags)
{
    (void)f_flags;

    struct procfs_mem_file *mf = kvmalloc(sizeof(*mf));
    if (mf == NULL)
        return -ENOMEM;
    memset(mf, 0, sizeof(*mf));

    int target = (pi->type == PROC_TASK_MEM && pi->tid > 0) ? pi->tid : pi->pid;
    vm_t *target_vm = NULL;
    int ret = 0;

    pid_rlock();
    struct thread *p = NULL;
    get_pid_thread(target, &p);
    if (p == NULL || p->vm == NULL || p->tgid != pi->pid ||
        p->thread_group == NULL || p->thread_group->is_kernel) {
        ret = -ESRCH;
        goto out_unlock;
    }

    struct thread_group *target_tg = p->thread_group;
    struct thread_group *current_tg = current != NULL ? current->thread_group : NULL;
    bool same_group = current != NULL && current->tgid == p->tgid;
    bool root = current_tg != NULL && current_tg->euid == 0;
    bool same_creds = current_tg != NULL &&
        current_tg->euid == target_tg->euid &&
        current_tg->egid == target_tg->egid &&
        current_tg->euid == target_tg->suid;
    int dumpable = __atomic_load_n(&target_tg->dumpable, __ATOMIC_ACQUIRE);

    if (!same_group && !root && !(same_creds && dumpable != 0)) {
        ret = -EACCES;
        goto out_unlock;
    }

    target_vm = p->vm;
    vm_dup(target_vm);

out_unlock:
    pid_runlock();

    if (ret != 0) {
        kvfree(mf);
        return ret;
    }

    mf->vm = target_vm;
    mf->pid = pi->pid;
    file->private_data = mf;
    file->ops = &procfs_mem_file_ops;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  procfs_open – set file ops; generate content for regular files   */
/* ------------------------------------------------------------------ */

static int procfs_open(struct vfs_inode *inode, struct vfs_file *file,
                       int f_flags) {
    struct procfs_inode *pi = procfs_i(inode);

    if (S_ISDIR(inode->mode)) {
        file->ops = &procfs_dir_file_ops;
        return 0;
    }

    if (S_ISLNK(inode->mode)) {
        /* Symlinks: readlink is invoked by VFS, no file ops needed */
        return 0;
    }

    if (pi->type == PROC_PID_OOM_SCORE_ADJ) {
        struct procfs_pid_file *pf = kvmalloc(sizeof(*pf));
        if (pf == NULL)
            return -ENOMEM;
        pf->pid = pi->pid;
        file->private_data = pf;
        file->ops = &procfs_oom_score_adj_file_ops;
        return 0;
    }

    if (pi->type == PROC_PID_MEM || pi->type == PROC_TASK_MEM)
        return procfs_open_mem(pi, file, f_flags);

    /* Regular file: generate content into a heap buffer */
    char *buf = NULL;
    struct procfs_blob *blob = NULL;

    switch (pi->type) {
    case PROC_STATUS:
        buf = procfs_gen_status(pi->pid);
        break;
    case PROC_TASK_STATUS:
        buf = procfs_gen_status(pi->tid);
        break;
    case PROC_PID_STAT:
        buf = procfs_gen_pid_stat(pi->pid);
        break;
    case PROC_TASK_STAT:
        buf = procfs_gen_pid_stat(pi->tid);
        break;
    case PROC_PID_CMDLINE:
    case PROC_TASK_CMDLINE:
        blob = procfs_gen_pid_exec_blob(pi->pid, pi->type);
        break;
    case PROC_PID_COMM:
        buf = procfs_gen_pid_comm(pi->pid);
        break;
    case PROC_TASK_COMM:
        buf = procfs_gen_pid_comm(pi->tid);
        break;
    case PROC_STATM:
    case PROC_TASK_STATM:
        buf = procfs_gen_statm(pi->pid);
        break;
    case PROC_MAPS:
    case PROC_TASK_MAPS:
        buf = procfs_gen_maps(pi->pid);
        break;
    case PROC_PID_SMAPS:
    case PROC_TASK_SMAPS:
        buf = procfs_gen_smaps(pi->pid);
        break;
    case PROC_MEMINFO:
        buf = procfs_gen_meminfo();
        break;
    case PROC_CPUINFO:
        buf = procfs_gen_cpuinfo();
        break;
    case PROC_ZONEINFO:
        buf = procfs_gen_zoneinfo();
        break;
    case PROC_CGROUP:
        buf = procfs_gen_cgroup(pi->pid);
        break;
    case PROC_TASK_CGROUP:
        buf = procfs_gen_cgroup(pi->tid);
        break;
    case PROC_CMDLINE:
        buf = procfs_gen_cmdline();
        break;
    case PROC_VERSION:
        buf = procfs_gen_version();
        break;
    case PROC_UPTIME:
        buf = procfs_gen_uptime();
        break;
    case PROC_STAT:
        buf = procfs_gen_stat();
        break;
    case PROC_LOADAVG:
        buf = procfs_gen_loadavg();
        break;
    case PROC_FILESYSTEMS:
        buf = procfs_gen_filesystems();
        break;
    case PROC_MOUNTS:
        buf = procfs_gen_mounts();
        break;
    case PROC_PID_MOUNTS:
    case PROC_TASK_MOUNTS:
        buf = procfs_gen_mounts();
        break;
    case PROC_PID_MOUNTINFO:
    case PROC_TASK_MOUNTINFO:
        buf = procfs_gen_mountinfo();
        break;
    case PROC_PID_LIMITS:
    case PROC_TASK_LIMITS:
        buf = procfs_gen_limits();
        break;
    case PROC_PID_SETGROUPS:
        buf = procfs_gen_setgroups();
        break;
    case PROC_PID_ENVIRON:
    case PROC_TASK_ENVIRON:
        blob = procfs_gen_pid_exec_blob(pi->pid, pi->type);
        break;
    case PROC_PID_AUXV:
    case PROC_TASK_AUXV:
        blob = procfs_gen_pid_snapshot_blob(pi->pid, pi->type);
        break;
    case PROC_FDINFO_ENTRY:
        buf = procfs_gen_fdinfo(pi->pid, pi->fd);
        break;
    case PROC_RESOURCES:
        buf = procfs_gen_resources(pi->pid);
        break;
    case PROC_CRASHES:
        buf = crash_log_generate();
        break;
    case PROC_SYS_KERNEL_OSTYPE:
    case PROC_SYS_KERNEL_OSRELEASE:
    case PROC_SYS_KERNEL_VERSION:
    case PROC_SYS_KERNEL_HOSTNAME:
    case PROC_SYS_KERNEL_DOMAINNAME:
    case PROC_SYS_KERNEL_RANDOMIZE_VA_SPACE:
    case PROC_SYS_KERNEL_PID_MAX:
    case PROC_SYS_KERNEL_THREADS_MAX:
    case PROC_SYS_VM_OVERCOMMIT_MEMORY:
    case PROC_SYS_VM_OVERCOMMIT_RATIO:
    case PROC_SYS_VM_MAX_MAP_COUNT:
    case PROC_SYS_VM_MMAP_MIN_ADDR:
    case PROC_SYS_VM_SWAPPINESS:
    case PROC_SYS_VM_DIRTY_RATIO:
    case PROC_SYS_VM_DIRTY_BACKGROUND_RATIO:
    case PROC_SYS_FS_FILE_MAX:
    case PROC_SYS_FS_FILE_NR:
    case PROC_SYS_FS_NR_OPEN:
    case PROC_SYS_FS_PIPE_MAX_SIZE:
    case PROC_SYS_FS_INOTIFY_MAX_USER_WATCHES:
    case PROC_SYS_FS_INOTIFY_MAX_USER_INSTANCES:
    case PROC_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS:
        buf = procfs_gen_sys_file(pi->type);
        break;
    default:
        return -EINVAL;
    }

    if (blob != NULL) {
        if (IS_ERR(blob))
            return (int)PTR_ERR(blob);
        file->private_data = blob;
        file->ops = &procfs_blob_file_ops;
        return 0;
    }

    if (IS_ERR(buf))
        return (int)PTR_ERR(buf);
    if (buf == NULL)
        return -ENOMEM;

    file->private_data = buf;
    file->ops          = &procfs_reg_file_ops;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  destroy_inode – procfs inodes carry no extra kernel resources     */
/* ------------------------------------------------------------------ */

static void procfs_destroy_inode(struct vfs_inode *inode) {
    /* Nothing to release in the inode itself;
     * per-file content lives in file->private_data and is freed by
     * procfs_reg_release(). */
    (void)inode;
}

/* ------------------------------------------------------------------ */
/*  Exported inode ops table                                          */
/* ------------------------------------------------------------------ */

struct vfs_inode_ops procfs_inode_ops = {
    .lookup         = procfs_lookup,
    .dir_iter       = procfs_dir_iter,
    .readlink       = procfs_readlink,
    .getattr        = procfs_getattr,
    .open           = procfs_open,
    .destroy_inode  = procfs_destroy_inode,
    .free_inode     = procfs_free_inode,
};
