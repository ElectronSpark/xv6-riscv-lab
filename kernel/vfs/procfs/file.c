/*
 * procfs/file.c - VFS file operations for procfs regular files and directories
 *
 * Regular files
 * -------------
 * Content is generated at open time by procfs_open() (inode.c) into a
 * kvmalloc'd, null-terminated buffer stored in file->private_data.
 * procfs_reg_read() serves bytes from that buffer.
 * procfs_reg_release() frees it.
 *
 * Directories
 * -----------
 * Iteration state is managed by inode.c/procfs_dir_iter() using
 * iter->index + iter->cookies (the integer child position).  No heap
 * memory is allocated per dir-file, so procfs_dir_release() is a no-op.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "lock/rcu.h"
#include "list.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "printf.h"
#include "procfs_private.h"

int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ------------------------------------------------------------------ */
/*  Regular-file operations                                           */
/* ------------------------------------------------------------------ */

/*
 * procfs_reg_read - copy content from file->private_data to @buf.
 *
 * @user: if non-zero @buf is a user-space pointer and we must go
 *        through either_copyout(); otherwise direct memmove.
 */
static ssize_t procfs_reg_read(struct vfs_file *file, char *buf, size_t count,
                               bool user) {
    char *data = (char *)file->private_data;
    if (data == NULL)
        return 0;

    size_t size = strlen(data); /* content is always null-terminated */
    loff_t pos  = file->f_pos;

    if (pos < 0 || (uint64)pos >= size)
        return 0;

    size_t remaining = size - (size_t)pos;
    size_t chunk     = (count < remaining) ? count : remaining;

    if (user) {
        int ret = either_copyout(1, (uint64)buf, data + pos, chunk);
        if (ret < 0)
            return ret;
    } else {
        memmove(buf, data + pos, chunk);
    }

    return (ssize_t)chunk;
}

/*
 * procfs_reg_llseek - adjust the file offset.
 */
static loff_t procfs_reg_llseek(struct vfs_file *file, loff_t offset,
                                int whence) {
    char *data = (char *)file->private_data;
    loff_t size = (data != NULL) ? (loff_t)strlen(data) : 0;
    loff_t new_pos;

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        new_pos = size + offset;
        break;
    default:
        return -EINVAL;
    }

    if (new_pos < 0)
        new_pos = 0;
    if (new_pos > size)
        new_pos = size;

    file->f_pos = new_pos;
    return new_pos;
}

/*
 * procfs_reg_release - free the content buffer allocated at open time.
 */
static int procfs_reg_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    if (file->private_data != NULL) {
        kvfree(file->private_data);
        file->private_data = NULL;
    }
    return 0;
}

struct vfs_file_ops procfs_reg_file_ops = {
    .read    = procfs_reg_read,
    .llseek  = procfs_reg_llseek,
    .release = procfs_reg_release,
};

static ssize_t procfs_blob_read(struct vfs_file *file, char *buf, size_t count,
                                bool user)
{
    struct procfs_blob *blob = (struct procfs_blob *)file->private_data;
    if (blob == NULL)
        return 0;

    loff_t pos = file->f_pos;
    if (pos < 0 || (uint64)pos >= blob->len)
        return 0;

    size_t remaining = blob->len - (size_t)pos;
    size_t chunk = (count < remaining) ? count : remaining;

    if (user) {
        int ret = either_copyout(1, (uint64)buf, blob->data + pos, chunk);
        if (ret < 0)
            return ret;
    } else {
        memmove(buf, blob->data + pos, chunk);
    }

    return (ssize_t)chunk;
}

static loff_t procfs_blob_llseek(struct vfs_file *file, loff_t offset,
                                 int whence)
{
    struct procfs_blob *blob = (struct procfs_blob *)file->private_data;
    loff_t size = (blob != NULL) ? (loff_t)blob->len : 0;
    loff_t new_pos;

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        new_pos = size + offset;
        break;
    default:
        return -EINVAL;
    }

    if (new_pos < 0)
        new_pos = 0;
    if (new_pos > size)
        new_pos = size;

    file->f_pos = new_pos;
    return new_pos;
}

struct vfs_file_ops procfs_blob_file_ops = {
    .read    = procfs_blob_read,
    .llseek  = procfs_blob_llseek,
    .release = procfs_reg_release,
};

static int procfs_oom_score_adj_get_tg(int pid, struct thread_group **out_tg)
{
    if (out_tg == NULL)
        return -EINVAL;
    *out_tg = NULL;

    rcu_read_lock();
    struct thread *p = NULL;
    if (get_pid_thread(pid, &p) != 0 || p == NULL || p->thread_group == NULL) {
        rcu_read_unlock();
        return -ESRCH;
    }

    struct thread_group *tg = p->thread_group;
    thread_group_get(tg);
    rcu_read_unlock();

    *out_tg = tg;
    return 0;
}

static bool procfs_oom_score_adj_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

static int procfs_parse_oom_score_adj(const char *buf, size_t len, int *out)
{
    size_t i = 0;
    int sign = 1;
    int value = 0;
    bool saw_digit = false;

    if (out == NULL)
        return -EINVAL;

    while (i < len && procfs_oom_score_adj_is_space(buf[i]))
        i++;

    if (i < len && (buf[i] == '+' || buf[i] == '-')) {
        if (buf[i] == '-')
            sign = -1;
        i++;
    }

    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        saw_digit = true;
        value = value * 10 + (buf[i] - '0');
        if (value > 1000)
            return -EINVAL;
        i++;
    }

    if (!saw_digit)
        return -EINVAL;

    while (i < len && procfs_oom_score_adj_is_space(buf[i]))
        i++;

    if (i != len)
        return -EINVAL;

    value *= sign;
    if (value < -1000 || value > 1000)
        return -EINVAL;

    *out = value;
    return 0;
}

static ssize_t procfs_oom_score_adj_read(struct vfs_file *file, char *buf,
                                         size_t count, bool user)
{
    struct procfs_pid_file *pf = (struct procfs_pid_file *)file->private_data;
    if (pf == NULL)
        return -EIO;

    struct thread_group *tg = NULL;
    int ret = procfs_oom_score_adj_get_tg(pf->pid, &tg);
    if (ret < 0)
        return ret;

    int value = __atomic_load_n(&tg->oom_score_adj, __ATOMIC_SEQ_CST);
    thread_group_put(tg);

    char data[16];
    int len = snprintf(data, sizeof(data), "%d\n", value);
    if (len < 0)
        return len;

    loff_t pos = file->f_pos;
    if (pos < 0 || pos >= len)
        return 0;

    size_t remaining = (size_t)len - (size_t)pos;
    size_t chunk = (count < remaining) ? count : remaining;

    if (user) {
        ret = either_copyout(1, (uint64)buf, data + pos, chunk);
        if (ret < 0)
            return ret;
    } else {
        memmove(buf, data + pos, chunk);
    }

    return (ssize_t)chunk;
}

static ssize_t procfs_oom_score_adj_write(struct vfs_file *file,
                                          const char *buf, size_t count,
                                          bool user)
{
    struct procfs_pid_file *pf = (struct procfs_pid_file *)file->private_data;
    if (pf == NULL)
        return -EIO;
    if (count >= 32)
        return -EINVAL;

    char data[32];
    if (user) {
        int ret = either_copyin(data, 1, (uint64)buf, count);
        if (ret < 0)
            return ret;
    } else {
        memmove(data, buf, count);
    }
    data[count] = '\0';

    int value;
    int ret = procfs_parse_oom_score_adj(data, count, &value);
    if (ret < 0)
        return ret;

    struct thread_group *tg = NULL;
    ret = procfs_oom_score_adj_get_tg(pf->pid, &tg);
    if (ret < 0)
        return ret;

    __atomic_store_n(&tg->oom_score_adj, value, __ATOMIC_SEQ_CST);
    thread_group_put(tg);

    return (ssize_t)count;
}

static loff_t procfs_oom_score_adj_llseek(struct vfs_file *file,
                                          loff_t offset, int whence)
{
    struct procfs_pid_file *pf = (struct procfs_pid_file *)file->private_data;
    if (pf == NULL)
        return -EIO;

    struct thread_group *tg = NULL;
    int ret = procfs_oom_score_adj_get_tg(pf->pid, &tg);
    if (ret < 0)
        return ret;

    int value = __atomic_load_n(&tg->oom_score_adj, __ATOMIC_SEQ_CST);
    thread_group_put(tg);

    char data[16];
    int size = snprintf(data, sizeof(data), "%d\n", value);
    if (size < 0)
        return size;

    loff_t new_pos;
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        new_pos = (loff_t)size + offset;
        break;
    default:
        return -EINVAL;
    }

    if (new_pos < 0)
        new_pos = 0;
    if (new_pos > size)
        new_pos = size;

    file->f_pos = new_pos;
    return new_pos;
}

struct vfs_file_ops procfs_oom_score_adj_file_ops = {
    .read    = procfs_oom_score_adj_read,
    .write   = procfs_oom_score_adj_write,
    .llseek  = procfs_oom_score_adj_llseek,
    .release = procfs_reg_release,
};

static ssize_t procfs_mem_read(struct vfs_file *file, char *buf, size_t count,
                               bool user)
{
    struct procfs_mem_file *mf = (struct procfs_mem_file *)file->private_data;
    if (mf == NULL || mf->vm == NULL)
        return -ESRCH;
    if (count == 0)
        return 0;
    if (file->f_pos < 0)
        return -EIO;

    char *tmp = kvmalloc(PGSIZE);
    if (tmp == NULL)
        return -ENOMEM;

    ssize_t done = 0;
    while ((size_t)done < count) {
        uint64 addr = (uint64)file->f_pos + (uint64)done;
        if (addr < (uint64)file->f_pos) {
            if (done == 0)
                done = -EIO;
            break;
        }

        size_t chunk = count - (size_t)done;
        uint64 page_left = PGSIZE - (addr & (PGSIZE - 1));
        if (chunk > page_left)
            chunk = page_left;
        if (chunk > PGSIZE)
            chunk = PGSIZE;

        int ret = vm_copyin(mf->vm, tmp, addr, chunk);
        if (ret < 0) {
            if (done == 0)
                done = -EIO;
            break;
        }

        if (user) {
            ret = either_copyout(1, (uint64)buf + (uint64)done, tmp, chunk);
            if (ret < 0) {
                if (done == 0)
                    done = ret;
                break;
            }
        } else {
            memmove(buf + done, tmp, chunk);
        }
        done += (ssize_t)chunk;
    }

    kvfree(tmp);
    if (done > 0)
        file->f_pos += (loff_t)done;
    return done;
}

static ssize_t procfs_mem_write(struct vfs_file *file, const char *buf,
                                size_t count, bool user)
{
    struct procfs_mem_file *mf = (struct procfs_mem_file *)file->private_data;
    if (mf == NULL || mf->vm == NULL)
        return -ESRCH;
    if (count == 0)
        return 0;
    if (file->f_pos < 0)
        return -EIO;

    char *tmp = kvmalloc(PGSIZE);
    if (tmp == NULL)
        return -ENOMEM;

    ssize_t done = 0;
    while ((size_t)done < count) {
        uint64 addr = (uint64)file->f_pos + (uint64)done;
        if (addr < (uint64)file->f_pos) {
            if (done == 0)
                done = -EIO;
            break;
        }

        size_t chunk = count - (size_t)done;
        uint64 page_left = PGSIZE - (addr & (PGSIZE - 1));
        if (chunk > page_left)
            chunk = page_left;
        if (chunk > PGSIZE)
            chunk = PGSIZE;

        int ret;
        if (user) {
            ret = either_copyin(tmp, 1, (uint64)buf + (uint64)done, chunk);
            if (ret < 0) {
                if (done == 0)
                    done = ret;
                break;
            }
        } else {
            memmove(tmp, buf + done, chunk);
        }

        ret = vm_copyout(mf->vm, addr, tmp, chunk);
        if (ret < 0) {
            if (done == 0)
                done = -EIO;
            break;
        }
        done += (ssize_t)chunk;
    }

    kvfree(tmp);
    if (done > 0)
        file->f_pos += (loff_t)done;
    return done;
}

static loff_t procfs_mem_llseek(struct vfs_file *file, loff_t offset,
                                int whence)
{
    loff_t new_pos;

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    default:
        return -EINVAL;
    }

    if (new_pos < 0)
        return -EINVAL;
    file->f_pos = new_pos;
    return new_pos;
}

static int procfs_mem_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    struct procfs_mem_file *mf = (struct procfs_mem_file *)file->private_data;
    if (mf != NULL) {
        if (mf->vm != NULL)
            vm_put(mf->vm);
        kvfree(mf);
        file->private_data = NULL;
    }
    return 0;
}

struct vfs_file_ops procfs_mem_file_ops = {
    .read    = procfs_mem_read,
    .write   = procfs_mem_write,
    .llseek  = procfs_mem_llseek,
    .release = procfs_mem_release,
};

/* ------------------------------------------------------------------ */
/*  Directory file operations                                         */
/* ------------------------------------------------------------------ */

/*
 * procfs_dir_release - clean up any per-file directory state.
 *
 * Because procfs uses linear index-based iteration without caching
 * a snapshot in iter->cookies (it re-scans the process table on every
 * getdents call), there is no heap memory to free here.
 */
static int procfs_dir_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    (void)file;
    return 0;
}

struct vfs_file_ops procfs_dir_file_ops = {
    .release = procfs_dir_release,
};
