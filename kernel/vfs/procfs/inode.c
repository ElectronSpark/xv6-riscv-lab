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
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/sched.h"
#include "maple_tree.h"
#include "printf.h"
#include "procfs_private.h"
#include "accounting.h"
#include "timer/timer.h"

/* snprintf is provided by lwip_port/sys_arch.c – forward-declare it here */
int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/*  VFS_DITER_INDEX_CURRENT is the value iter->index holds before     */
/*  the VFS advances it to VFS_DITER_INDEX_PARENT (= 2).              */
/*  Children start at index 3 (VFS increments from 2 → 3 first).     */
/* ------------------------------------------------------------------ */
#define PROCFS_CHILDREN_START 3

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
/*  Helper: find the n-th open fd (0-based) in a fdtable              */
/* ------------------------------------------------------------------ */

static int procfs_nth_fd(int tgid, int n) {
    rcu_read_lock();
    struct thread *p = NULL;
    get_pid_thread(tgid, &p);
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
        if (name_len == 4 && memcmp(name, "self", 4) == 0) {
            dentry->ino = PROCFS_INO_SELF;
            return 0;
        }
        if (name_len == 7 && memcmp(name, "meminfo", 7) == 0) {
            dentry->ino = PROCFS_INO_MEMINFO;
            return 0;
        }
        if (name_len == 7 && memcmp(name, "cpuinfo", 7) == 0) {
            dentry->ino = PROCFS_INO_CPUINFO;
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
        if (name_len == 6 && memcmp(name, "status", 6) == 0) {
            dentry->ino = PROCFS_PID_STATUS_INO(pi->pid);
            return 0;
        }
        if (name_len == 4 && memcmp(name, "maps", 4) == 0) {
            dentry->ino = PROCFS_PID_MAPS_INO(pi->pid);
            return 0;
        }
        if (name_len == 3 && memcmp(name, "exe", 3) == 0) {
            dentry->ino = PROCFS_PID_EXE_INO(pi->pid);
            return 0;
        }
        if (name_len == 2 && memcmp(name, "fd", 2) == 0) {
            dentry->ino = PROCFS_PID_FDDIR_INO(pi->pid);
            return 0;
        }
        if (name_len == 9 && memcmp(name, "resources", 9) == 0) {
            dentry->ino = PROCFS_PID_RESOURCES_INO(pi->pid);
            return 0;
        }
        return -ENOENT;
    }

    case PROC_FDDIR: {
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
        get_pid_thread(pi->pid, &p);
        int valid = (p != NULL && p->fdtable != NULL &&
                     p->fdtable->files[fdnum] != NULL);
        rcu_read_unlock();

        if (!valid)
            return -ENOENT;

        dentry->ino = PROCFS_FD_INO(pi->pid, fdnum);
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
        } else if (pi->type == PROC_FDDIR) {
            parent_ino = PROCFS_PID_DIR_INO(pi->pid);
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
        const char *name = NULL;
        uint64      ino  = 0;

        if (child_idx == 0) {
            name = "self";
            ino  = PROCFS_INO_SELF;
        } else if (child_idx == 1) {
            name = "meminfo";
            ino  = PROCFS_INO_MEMINFO;
        } else if (child_idx == 2) {
            name = "cpuinfo";
            ino  = PROCFS_INO_CPUINFO;
        } else {
            /* pid entries start at child_idx 3 */
            int nth  = child_idx - 3;
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

        vfs_release_dentry(ret_dentry);
        ret_dentry->name     = strdup(name);
        if (ret_dentry->name == NULL)
            return -ENOMEM;
        ret_dentry->name_len = (uint16)strlen(name);
        ret_dentry->ino      = ino;
        ret_dentry->cookies  = (int64)iter->index;
        return 0;
    }

    /* ---- /proc/<tgid>/ children ---- */
    case PROC_PID_DIR: {
        const char *name = NULL;
        uint64      ino  = 0;

        switch (child_idx) {
        case 0: name = "status"; ino = PROCFS_PID_STATUS_INO(pi->pid); break;
        case 1: name = "maps";   ino = PROCFS_PID_MAPS_INO(pi->pid);   break;
        case 2: name = "exe";    ino = PROCFS_PID_EXE_INO(pi->pid);    break;
        case 3: name = "fd";     ino = PROCFS_PID_FDDIR_INO(pi->pid);  break;
        case 4: name = "resources"; ino = PROCFS_PID_RESOURCES_INO(pi->pid); break;
        default:
            vfs_release_dentry(ret_dentry);
            ret_dentry->name = NULL;
            return 0;
        }

        vfs_release_dentry(ret_dentry);
        ret_dentry->name     = strdup(name);
        if (ret_dentry->name == NULL)
            return -ENOMEM;
        ret_dentry->name_len = (uint16)strlen(name);
        ret_dentry->ino      = ino;
        ret_dentry->cookies  = (int64)iter->index;
        return 0;
    }

    /* ---- /proc/<tgid>/fd/ children ---- */
    case PROC_FDDIR: {
        int fd = procfs_nth_fd(pi->pid, child_idx);
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
        ret_dentry->ino      = PROCFS_FD_INO(pi->pid, fd);
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

    case PROC_FD_ENTRY: {
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
        /* Read the inode pointer from the file reference under RCU */
        struct vfs_inode *fi = f->inode.inode;
        mode_t fmode = (fi != NULL) ? fi->mode : 0;
        uint64 fino  = (fi != NULL) ? fi->ino  : 0;
        rcu_read_unlock();

        if (S_ISFIFO(fmode))
            n = snprintf(buf, buflen, "pipe:[%llu]",
                         (unsigned long long)fino);
        else if (S_ISSOCK(fmode))
            n = snprintf(buf, buflen, "socket:[%llu]",
                         (unsigned long long)fino);
        else
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
    st->ino   = inode->ino;
    st->mode  = inode->mode;
    st->nlink = inode->n_links;
    st->size  = inode->size;
    st->dev   = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Content generation helpers (called from procfs_open)             */
/* ------------------------------------------------------------------ */

#define PROCFS_BUF_SIZE 4096

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
    unsigned long    heap_kb  = 0;
    if (p->vm != NULL)
        heap_kb = (unsigned long)(p->vm->heap_size / 1024);
    /* Cumulative CPU time (raw timer ticks at TIMEBASE_FREQUENCY Hz) */
    uint64 cputime_raw = 0;
    uint32 util_avg = 0;
    uint64 load_contrib = 0;
    if (p->sched_entity) {
        cputime_raw = p->sched_entity->sum_exec_runtime;
        util_avg = p->sched_entity->util_avg;
        load_contrib = p->sched_entity->load_avg_contrib;
    }
    rcu_read_unlock();

    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    snprintf(buf, PROCFS_BUF_SIZE,
             "Name:\t%s\n"
             "Pid:\t%d\n"
             "PPid:\t%d\n"
             "State:\t%s\n"
             "VmSize:\t%lu kB\n"
             "CpuTime:\t%lu\n"
             "UtilAvg:\t%u\n"
             "LoadContrib:\t%lu\n",
             name, tgid, ppid, statestr, heap_kb,
             (unsigned long)cputime_raw,
             (unsigned)util_avg,
             (unsigned long)load_contrib);
    return buf;
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

    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);

    int pos = 0;
    vm_rlock(vm);
    vma_t  *vma;
    uint64  index = 0;
    mt_for_each(&vm->vm_mt, vma, index, (uint64)(-1ULL)) {
        char r = (vma->flags & PROT_READ)      ? 'r' : '-';
        char w = (vma->flags & PROT_WRITE)     ? 'w' : '-';
        char x = (vma->flags & PROT_EXEC)      ? 'x' : '-';
        char s = (vma->flags & VMA_FLAG_SHARED) ? 's' : 'p';
        int n = snprintf(buf + pos, PROCFS_BUF_SIZE - pos,
                         "%016llx-%016llx %c%c%c%c %08llx 00:00 0\n",
                         (unsigned long long)vma->start,
                         (unsigned long long)vma->end,
                         r, w, x, s,
                         (unsigned long long)vma->pgoff);
        if (n < 0 || pos + n >= PROCFS_BUF_SIZE - 1)
            break;
        pos += n;
    }
    vm_runlock(vm);
    buf[pos] = '\0';
    return buf;
}

static char *procfs_gen_meminfo(void) {
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE,
             "MemTotal:        1048576 kB\n"
             "MemFree:          524288 kB\n"
             "Buffers:               0 kB\n"
             "Cached:                0 kB\n");
    return buf;
}

static char *procfs_gen_cpuinfo(void) {
    char *buf = kvmalloc(PROCFS_BUF_SIZE);
    if (buf == NULL)
        return ERR_PTR(-ENOMEM);
    snprintf(buf, PROCFS_BUF_SIZE,
             "processor\t: 0\n"
             "model name\t: RISC-V\n"
             "isa\t\t: rv64imafdc\n"
             "mmu\t\t: sv39\n");
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

/* ------------------------------------------------------------------ */
/*  procfs_open – set file ops; generate content for regular files   */
/* ------------------------------------------------------------------ */

static int procfs_open(struct vfs_inode *inode, struct vfs_file *file,
                       int f_flags) {
    (void)f_flags;
    struct procfs_inode *pi = procfs_i(inode);

    if (S_ISDIR(inode->mode)) {
        file->ops = &procfs_dir_file_ops;
        return 0;
    }

    if (S_ISLNK(inode->mode)) {
        /* Symlinks: readlink is invoked by VFS, no file ops needed */
        return 0;
    }

    /* Regular file: generate content into a heap buffer */
    char *buf = NULL;

    switch (pi->type) {
    case PROC_STATUS:
        buf = procfs_gen_status(pi->pid);
        break;
    case PROC_MAPS:
        buf = procfs_gen_maps(pi->pid);
        break;
    case PROC_MEMINFO:
        buf = procfs_gen_meminfo();
        break;
    case PROC_CPUINFO:
        buf = procfs_gen_cpuinfo();
        break;
    case PROC_RESOURCES:
        buf = procfs_gen_resources(pi->pid);
        break;
    default:
        return -EINVAL;
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
