/*
 * sysfs/file.c - File operations for read-only generated sysfs files.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include <mm/vm.h>
#include "sysfs_private.h"

static ssize_t sysfs_reg_read(struct vfs_file *file, char *buf, size_t count,
                              bool user)
{
    char *data = (char *)file->private_data;
    size_t size;
    loff_t pos;
    size_t chunk;

    if (data == NULL)
        return 0;

    size = strlen(data);
    pos = file->f_pos;
    if (pos < 0 || (uint64)pos >= size)
        return 0;

    chunk = size - (size_t)pos;
    if (chunk > count)
        chunk = count;

    if (user) {
        int ret = either_copyout(1, (uint64)buf, data + pos, chunk);
        if (ret < 0)
            return ret;
    } else {
        memmove(buf, data + pos, chunk);
    }

    file->f_pos += chunk;
    return (ssize_t)chunk;
}

static loff_t sysfs_reg_llseek(struct vfs_file *file, loff_t offset,
                               int whence)
{
    char *data = (char *)file->private_data;
    loff_t size = data ? (loff_t)strlen(data) : 0;
    loff_t pos;

    switch (whence) {
    case SEEK_SET:
        pos = offset;
        break;
    case SEEK_CUR:
        pos = file->f_pos + offset;
        break;
    case SEEK_END:
        pos = size + offset;
        break;
    default:
        return -EINVAL;
    }

    if (pos < 0)
        pos = 0;
    if (pos > size)
        pos = size;
    file->f_pos = pos;
    return pos;
}

static int sysfs_reg_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    if (file->private_data != NULL) {
        kvfree(file->private_data);
        file->private_data = NULL;
    }
    return 0;
}

static int sysfs_dir_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    (void)file;
    return 0;
}

struct vfs_file_ops sysfs_reg_file_ops = {
    .read = sysfs_reg_read,
    .llseek = sysfs_reg_llseek,
    .release = sysfs_reg_release,
};

struct vfs_file_ops sysfs_dir_file_ops = {
    .release = sysfs_dir_release,
};
