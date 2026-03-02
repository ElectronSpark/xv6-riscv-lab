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
#include "list.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include "procfs_private.h"

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

    file->f_pos += (loff_t)chunk;
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
