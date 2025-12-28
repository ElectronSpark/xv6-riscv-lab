/*
 * VFS System Call Implementation
 *
 * This file implements the VFS-based system calls that replace the
 * original xv6 file system calls. All file operations now go through
 * the VFS layer.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "vfs/stat.h"
#include "fs.h"
#include "vm.h"
#include "printf.h"
#include "cdev.h"

// Forward declaration for syscall argument helpers
void argint(int n, int *ip);
void argaddr(int n, uint64 *ip);
int argstr(int n, char *buf, int max);

/******************************************************************************
 * Helper functions
 ******************************************************************************/

// Get file from fd in current process's VFS fdtable
static struct vfs_file *__vfs_argfd(int fd) {
    struct proc *p = myproc();
    if (fd < 0 || fd >= NOFILE) {
        return NULL;
    }
    return vfs_fdtable_get_file(&p->fs.fdtable, fd);
}

// Allocate a fd for the given file in current process
static int __vfs_fdalloc(struct vfs_file *file) {
    struct proc *p = myproc();
    return vfs_fdtable_alloc_fd(&p->fs.fdtable, file);
}

// Deallocate fd and return the file
static struct vfs_file *__vfs_fdfree(int fd) {
    struct proc *p = myproc();
    return vfs_fdtable_dealloc_fd(&p->fs.fdtable, fd);
}

/******************************************************************************
 * File Operations Syscalls
 ******************************************************************************/

uint64 sys_vfs_dup(void) {
    int fd;
    argint(0, &fd);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    struct vfs_file *nf = vfs_filedup(f);
    if (nf == NULL) {
        return -ENOMEM;
    }
    
    int newfd = __vfs_fdalloc(nf);
    if (newfd < 0) {
        vfs_fileclose(nf);
        return newfd;
    }
    
    return newfd;
}

uint64 sys_vfs_read(void) {
    int fd, n;
    uint64 p;
    
    argint(0, &fd);
    argaddr(1, &p);
    argint(2, &n);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    // For user-space reads, we need to use copyout
    char *kbuf = kmm_alloc(n);
    if (kbuf == NULL) {
        return -ENOMEM;
    }
    
    ssize_t ret = vfs_fileread(f, kbuf, n);
    if (ret > 0) {
        if (vm_copyout(myproc()->vm, p, kbuf, ret) < 0) {
            ret = -EFAULT;
        }
    }
    
    kmm_free(kbuf);
    return ret;
}

uint64 sys_vfs_write(void) {
    int fd, n;
    uint64 p;
    
    argint(0, &fd);
    argaddr(1, &p);
    argint(2, &n);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    // For user-space writes, we need to use copyin
    char *kbuf = kmm_alloc(n);
    if (kbuf == NULL) {
        return -ENOMEM;
    }
    
    if (vm_copyin(myproc()->vm, kbuf, p, n) < 0) {
        kmm_free(kbuf);
        return -EFAULT;
    }
    
    ssize_t ret = vfs_filewrite(f, kbuf, n);
    kmm_free(kbuf);
    return ret;
}

uint64 sys_vfs_close(void) {
    int fd;
    argint(0, &fd);
    
    struct vfs_file *f = __vfs_fdfree(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    vfs_fileclose(f);
    return 0;
}

uint64 sys_vfs_fstat(void) {
    int fd;
    uint64 st;
    
    argint(0, &fd);
    argaddr(1, &st);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    struct stat kst;
    int ret = vfs_filestat(f, &kst);
    if (ret != 0) {
        return ret;
    }
    
    if (vm_copyout(myproc()->vm, st, (char *)&kst, sizeof(kst)) < 0) {
        return -EFAULT;
    }
    
    return 0;
}

/******************************************************************************
 * File System Namespace Syscalls
 ******************************************************************************/

uint64 sys_vfs_open(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int omode;
    int n;
    
    argint(1, &omode);
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *inode = NULL;
    int ret = 0;
    
    if (omode & O_CREAT) {
        // Create file if it doesn't exist
        struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
        if (IS_ERR(parent)) {
            return PTR_ERR(parent);
        }
        if (parent == NULL) {
            return -ENOENT;
        }
        
        size_t name_len = strlen(name);
        
        // Try to create
        inode = vfs_create(parent, 0644, name, name_len);
        vfs_iput(parent);
        
        if (IS_ERR(inode)) {
            if (PTR_ERR(inode) == -EEXIST && !(omode & O_EXCL)) {
                // File exists, try to open it
                inode = vfs_namei(path, n);
            } else {
                return PTR_ERR(inode);
            }
        }
    } else {
        // Open existing file
        inode = vfs_namei(path, n);
    }
    
    if (IS_ERR(inode)) {
        return PTR_ERR(inode);
    }
    if (inode == NULL) {
        return -ENOENT;
    }
    
    // Check if trying to write to a directory
    if (S_ISDIR(inode->mode) && (omode & O_WRONLY || omode & O_RDWR)) {
        vfs_iput(inode);
        return -EISDIR;
    }
    
    struct vfs_file *f = vfs_fileopen(inode, omode);
    vfs_iput(inode);
    
    if (IS_ERR(f)) {
        return PTR_ERR(f);
    }
    
    // Handle O_TRUNC
    if ((omode & O_TRUNC) && S_ISREG(inode->mode)) {
        ret = vfs_itruncate(vfs_inode_deref(&f->inode), 0);
        if (ret != 0) {
            vfs_fileclose(f);
            return ret;
        }
    }
    
    int fd = __vfs_fdalloc(f);
    if (fd < 0) {
        vfs_fileclose(f);
        return fd;
    }

    return fd;
}

uint64 sys_vfs_mkdir(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    struct vfs_inode *dir = vfs_mkdir(parent, 0755, name, name_len);
    vfs_iput(parent);
    
    if (IS_ERR(dir)) {
        return PTR_ERR(dir);
    }
    
    vfs_iput(dir);
    return 0;
}

uint64 sys_vfs_mknod(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int major, minor;
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    argint(1, &major);
    argint(2, &minor);
    
    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    // Create character device node
    dev_t dev = mkdev(major, minor);
    struct vfs_inode *node = vfs_mknod(parent, S_IFCHR | 0666, dev, name, name_len);
    vfs_iput(parent);
    
    if (IS_ERR(node)) {
        return PTR_ERR(node);
    }
    
    vfs_iput(node);
    return 0;
}

uint64 sys_vfs_unlink(void) {
    char path[MAXPATH];
    char name[DIRSIZ];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *parent = vfs_nameiparent(path, n, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    // Cannot unlink "." or ".."
    if ((name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.')) {
        vfs_iput(parent);
        return -EINVAL;
    }
    
    int ret = vfs_unlink(parent, name, name_len);
    vfs_iput(parent);
    
    return ret;
}

uint64 sys_vfs_link(void) {
    char old[MAXPATH], new[MAXPATH];
    char name[DIRSIZ];
    int n1, n2;
    
    if ((n1 = argstr(0, old, MAXPATH)) < 0 ||
        (n2 = argstr(1, new, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    // Get the source inode
    struct vfs_inode *src = vfs_namei(old, n1);
    if (IS_ERR(src)) {
        return PTR_ERR(src);
    }
    if (src == NULL) {
        return -ENOENT;
    }
    
    // Cannot link directories
    if (S_ISDIR(src->mode)) {
        vfs_iput(src);
        return -EPERM;
    }
    
    // Get parent directory of new path
    struct vfs_inode *parent = vfs_nameiparent(new, n2, name, DIRSIZ);
    if (IS_ERR(parent)) {
        vfs_iput(src);
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        vfs_iput(src);
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    // Create a dentry for the source
    struct vfs_dentry old_dentry = {
        .sb = src->sb,
        .ino = src->ino,
        .name = NULL,
        .name_len = 0,
    };
    
    int ret = vfs_link(&old_dentry, parent, name, name_len);
    
    vfs_iput(src);
    vfs_iput(parent);
    
    return ret;
}

uint64 sys_vfs_symlink(void) {
    char target[MAXPATH], linkpath[MAXPATH];
    char name[DIRSIZ];
    int n1, n2;
    
    if ((n1 = argstr(0, target, MAXPATH)) < 0 ||
        (n2 = argstr(1, linkpath, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *parent = vfs_nameiparent(linkpath, n2, name, DIRSIZ);
    if (IS_ERR(parent)) {
        return PTR_ERR(parent);
    }
    if (parent == NULL) {
        return -ENOENT;
    }
    
    size_t name_len = strlen(name);
    
    struct vfs_inode *sym = vfs_symlink(parent, 0777, name, name_len, target, n1);
    vfs_iput(parent);
    
    if (IS_ERR(sym)) {
        return PTR_ERR(sym);
    }
    
    vfs_iput(sym);
    return 0;
}

uint64 sys_vfs_chdir(void) {
    char path[MAXPATH];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *inode = vfs_namei(path, n);
    if (IS_ERR(inode)) {
        return PTR_ERR(inode);
    }
    if (inode == NULL) {
        return -ENOENT;
    }
    
    if (!S_ISDIR(inode->mode)) {
        vfs_iput(inode);
        return -ENOTDIR;
    }
    
    // Update process cwd
    struct proc *p = myproc();
    proc_lock(p);
    
    // Release old cwd
    vfs_inode_put_ref(&p->fs.cwd);
    
    // Set new cwd
    vfs_inode_get_ref(inode, &p->fs.cwd);
    
    proc_unlock(p);
    vfs_iput(inode);
    
    return 0;
}

/******************************************************************************
 * Pipe Syscall
 ******************************************************************************/

uint64 sys_vfs_pipe(void) {
    uint64 fdarray;
    argaddr(0, &fdarray);
    
    struct vfs_file *rf = NULL, *wf = NULL;
    int ret = vfs_pipealloc(&rf, &wf);
    if (ret != 0) {
        return ret;
    }
    
    int fd0 = __vfs_fdalloc(rf);
    if (fd0 < 0) {
        vfs_fileclose(rf);
        vfs_fileclose(wf);
        return fd0;
    }
    
    int fd1 = __vfs_fdalloc(wf);
    if (fd1 < 0) {
        __vfs_fdfree(fd0);
        vfs_fileclose(rf);
        vfs_fileclose(wf);
        return fd1;
    }
    
    struct proc *p = myproc();
    if (vm_copyout(p->vm, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
        vm_copyout(p->vm, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0) {
        __vfs_fdfree(fd0);
        __vfs_fdfree(fd1);
        vfs_fileclose(rf);
        vfs_fileclose(wf);
        return -EFAULT;
    }
    
    return 0;
}

/******************************************************************************
 * Socket Syscall
 ******************************************************************************/

uint64 sys_vfs_connect(void) {
    uint32 raddr, lport, rport;
    
    argint(0, (int *)&raddr);
    argint(1, (int *)&lport);
    argint(2, (int *)&rport);
    
    struct vfs_file *f = NULL;
    int ret = vfs_sockalloc(&f, raddr, (uint16)lport, (uint16)rport);
    if (ret != 0) {
        return ret;
    }
    
    int fd = __vfs_fdalloc(f);
    if (fd < 0) {
        vfs_fileclose(f);
        return fd;
    }
    
    return fd;
}

/******************************************************************************
 * Directory Operations - getdents
 ******************************************************************************/

// Linux-compatible dirent structure
struct linux_dirent64 {
    uint64 d_ino;      // Inode number
    int64 d_off;       // Offset to next structure
    uint16 d_reclen;   // Size of this dirent
    uint8 d_type;      // File type
    char d_name[];     // Filename (null-terminated)
};

// File type constants
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12

static uint8 __mode_to_dtype(mode_t mode) {
    if (S_ISREG(mode)) return DT_REG;
    if (S_ISDIR(mode)) return DT_DIR;
    if (S_ISCHR(mode)) return DT_CHR;
    if (S_ISBLK(mode)) return DT_BLK;
    if (S_ISFIFO(mode)) return DT_FIFO;
    if (S_ISLNK(mode)) return DT_LNK;
    if (S_ISSOCK(mode)) return DT_SOCK;
    return DT_UNKNOWN;
}

uint64 sys_getdents(void) {
    int fd;
    uint64 dirp;
    int count;
    
    argint(0, &fd);
    argaddr(1, &dirp);
    argint(2, &count);
    
    struct vfs_file *f = __vfs_argfd(fd);
    if (f == NULL) {
        return -EBADF;
    }
    
    struct vfs_inode *inode = vfs_inode_deref(&f->inode);
    if (inode == NULL || !S_ISDIR(inode->mode)) {
        return -ENOTDIR;
    }
    
    // Allocate kernel buffer
    char *kbuf = kmm_alloc(count);
    if (kbuf == NULL) {
        return -ENOMEM;
    }
    
    int bytes_written = 0;
    struct vfs_dentry dentry = {0};
    int ret;
    
    while (bytes_written < count) {
        ret = vfs_dir_iter(inode, &f->dir_iter, &dentry);
        if (ret != 0) {
            kmm_free(kbuf);
            return ret;
        }
        
        if (dentry.name == NULL) {
            // End of directory
            break;
        }
        
        // Calculate record length (must be 8-byte aligned)
        size_t name_len = dentry.name_len;
        size_t reclen = sizeof(struct linux_dirent64) + name_len + 1;
        reclen = (reclen + 7) & ~7; // Align to 8 bytes
        
        if (bytes_written + (int)reclen > count) {
            // Not enough space, put back for next call
            vfs_release_dentry(&dentry);
            break;
        }
        
        // Get inode info for d_type
        struct vfs_inode *child = vfs_get_dentry_inode(&dentry);
        uint8 d_type = DT_UNKNOWN;
        if (!IS_ERR_OR_NULL(child)) {
            d_type = __mode_to_dtype(child->mode);
            vfs_iput(child);
        }
        
        // Fill dirent
        struct linux_dirent64 *de = (struct linux_dirent64 *)(kbuf + bytes_written);
        de->d_ino = dentry.ino;
        de->d_off = f->dir_iter.index;
        de->d_reclen = reclen;
        de->d_type = d_type;
        memmove(de->d_name, dentry.name, name_len);
        de->d_name[name_len] = '\0';
        
        bytes_written += reclen;
        vfs_release_dentry(&dentry);
        memset(&dentry, 0, sizeof(dentry));
    }
    
    // Copy to user space
    if (bytes_written > 0) {
        if (vm_copyout(myproc()->vm, dirp, kbuf, bytes_written) < 0) {
            kmm_free(kbuf);
            return -EFAULT;
        }
    }
    
    kmm_free(kbuf);
    return bytes_written;
}

/******************************************************************************
 * chroot - Change root directory
 ******************************************************************************/

uint64 sys_chroot(void) {
    char path[MAXPATH];
    int n;
    
    if ((n = argstr(0, path, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    struct vfs_inode *new_root = vfs_namei(path, n);
    if (IS_ERR(new_root)) {
        return PTR_ERR(new_root);
    }
    if (new_root == NULL) {
        return -ENOENT;
    }
    
    if (!S_ISDIR(new_root->mode)) {
        vfs_iput(new_root);
        return -ENOTDIR;
    }
    
    // Use the VFS helper functions
    int ret = vfs_chroot(new_root);
    if (ret < 0) {
        vfs_iput(new_root);
        return ret;
    }
    
    ret = vfs_chdir(new_root);
    vfs_iput(new_root);
    
    return ret;
}

/******************************************************************************
 * mount - Mount a filesystem
 ******************************************************************************/

uint64 sys_mount(void) {
    char source[MAXPATH];
    char target[MAXPATH];
    char fstype[32];
    int n1, n2;
    
    if ((n1 = argstr(0, source, MAXPATH)) < 0 ||
        (n2 = argstr(1, target, MAXPATH)) < 0 ||
        argstr(2, fstype, 32) < 0) {
        return -EFAULT;
    }
    
    // Look up target directory
    struct vfs_inode *target_dir = vfs_namei(target, n2);
    if (IS_ERR(target_dir)) {
        return PTR_ERR(target_dir);
    }
    if (target_dir == NULL) {
        return -ENOENT;
    }
    
    if (!S_ISDIR(target_dir->mode)) {
        vfs_iput(target_dir);
        return -ENOTDIR;
    }
    
    // Parse source device (for block-device-based filesystems)
    struct vfs_inode *source_dev = vfs_namei(source, n1);
    struct vfs_inode *source_inode = NULL;
    if (!IS_ERR_OR_NULL(source_dev)) {
        if (S_ISBLK(source_dev->mode)) {
            source_inode = source_dev;
        } else {
            vfs_iput(source_dev);
        }
    }
    
    // Mount the filesystem
    int ret = vfs_mount(fstype, target_dir, source_inode, 0, NULL);
    if (source_inode) {
        vfs_iput(source_inode);
    }
    if (ret != 0) {
        vfs_iput(target_dir);
        return ret;
    }
    
    // target_dir ownership transferred to mount point
    return 0;
}

/******************************************************************************
 * umount - Unmount a filesystem
 ******************************************************************************/

uint64 sys_umount(void) {
    char target[MAXPATH];
    int n;
    
    if ((n = argstr(0, target, MAXPATH)) < 0) {
        return -EFAULT;
    }
    
    // Look up target directory
    struct vfs_inode *target_dir = vfs_namei(target, n);
    if (IS_ERR(target_dir)) {
        return PTR_ERR(target_dir);
    }
    if (target_dir == NULL) {
        return -ENOENT;
    }
    
    int ret = vfs_unmount(target_dir);
    vfs_iput(target_dir);
    
    return ret;
}
