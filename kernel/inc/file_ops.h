#ifndef __KERNEL_FILE_OPS_H
#define __KERNEL_FILE_OPS_H

#include <types.h>

struct file;
struct inode;

typedef struct file_ops {
    int (*read)(struct file *file, void *buf, size_t count);
    int (*write)(struct file *file, const void *buf, size_t count);
    int (*open)(struct file *file);
    int (*close)(struct file *file);
}file_ops_t;

#endif // __KERNEL_FILE_OPS_H
