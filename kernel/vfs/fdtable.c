#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "atomic.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "stat.h"
#include "fcntl.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs_private.h"

#define IS_FD(fd) ((uint64)(fd) > NOFILE)

void vfs_fdtable_init(struct vfs_fdtable *fdtable) {
    if (fdtable == NULL) {
        printf("vfs_fdtable_init: fdtable is NULL\n");
        return;
    }
    fdtable->fd_count = 0;
    for (uint64 i = 0; i < NOFILE; i++) {
        fdtable->files[i] = (void *)(i + 1);
    }
    fdtable->files[NOFILE - 1] = NULL;
    fdtable->next_fd = 0;
}

int vfs_fdtable_alloc_fd(struct vfs_fdtable *fdtable, struct vfs_file *file) {
    if (fdtable == NULL || file == NULL) {
        return -EINVAL; // Invalid arguments
    }
    if (fdtable->fd_count >= NOFILE) {
        return -EMFILE; // Too many open files
    }
    assert(fdtable->next_fd >= 0 && fdtable->next_fd < NOFILE,
           "vfs_fdtable_alloc_fd: next_fd out of range");
    int fd = fdtable->next_fd;
    if (fd == -1) {
        return -EMFILE; // No free file descriptor
    }
    fdtable->next_fd = (int)(uint64)(fdtable->files[fd]);
    if (fdtable->next_fd == 0) {
        fdtable->next_fd = -1;
    }
    fdtable->files[fd] = file;
    fdtable->fd_count++;
    file->fd = fd;
    return fd;
}

int vfs_fdtable_clone(struct vfs_fdtable *dest, struct vfs_fdtable *src) {
    if (dest == NULL || src == NULL) {
        return -EINVAL; // Invalid arguments
    }
    dest->fd_count = 0;
    memset(dest->files, 0, sizeof(dest->files));

    // Duplicate file references
    for (int i = 0; i < NOFILE; i++) {
        struct vfs_file *src_file = src->files[i];
        if (IS_FD(src_file)) {
            struct vfs_file *dst_file = vfs_filedup(src_file);
            if (!IS_ERR_OR_NULL(dst_file)) {
                dest->files[i] = dst_file;
                dest->fd_count++;
            }
        }
    }
    
    // construct free list
    dest->next_fd = -1;
    int last_free_fd = -1;
    for (int i = 0; i < NOFILE; i++) {
        if (!IS_FD(dest->files[i])) {
            if (last_free_fd == -1) {
                dest->next_fd = i;
                dest->files[i] = NULL;
            } else {
                dest->files[last_free_fd] = (struct vfs_file *)(uint64)(i);
            }
            last_free_fd = i;
        }
    }

    if (last_free_fd != -1) {
        dest->files[last_free_fd] = NULL;
    }

    return 0;
}

void vfs_fdtable_destroy(struct vfs_fdtable *fdtable, int start_fd) {
    if (fdtable == NULL) {
        printf("vfs_fdtable_destroy: fdtable is NULL\n");
        return;
    }
    if ( start_fd >= NOFILE) {
        printf("vfs_fdtable_destroy: start_fd %d out of range\n", start_fd);
        return;
    }
    if (start_fd < 0) {
        printf("vfs_fdtable_destroy: start_fd %d out of range\n", start_fd);
        start_fd = 0;
    }
    // First, close all open files in the range [start_fd, NOFILE)
    for (int i = start_fd; i < NOFILE; i++) {
        struct vfs_file *file = fdtable->files[i];
        if (IS_FD(file)) {
            vfs_fileclose(file);
            fdtable->fd_count--;
        }
    }

    // Rebuild the free list for [start_fd, NOFILE)
    for (int i = start_fd; i < NOFILE - 1; i++) {
        fdtable->files[i] = (void *)(uint64)(i + 1);
    }
    fdtable->files[NOFILE - 1] = NULL;

    // Connect the old free list (entries < start_fd) with the new one
    // Look backwards from start_fd to find the last free entry
    int last_free_fd = start_fd - 1;
    while (last_free_fd >= 0 && IS_FD(fdtable->files[last_free_fd])) {
        last_free_fd--;
    }
    if (last_free_fd >= 0) {
        // Link the last free entry before start_fd to the new free list
        fdtable->files[last_free_fd] = (struct vfs_file *)(uint64)(start_fd);
    } else {
        // No free entries before start_fd, start_fd becomes the new head
        fdtable->next_fd = start_fd;
    }

    assert(fdtable->fd_count >= 0, "vfs_fdtable_destroy: fd_count negative");
}

struct vfs_file *vfs_fdtable_get_file(struct vfs_fdtable *fdtable, int fd) {
    if (fdtable == NULL || fd < 0 || fd >= NOFILE) {
        return NULL; // Invalid arguments
    }
    struct vfs_file *file = fdtable->files[fd];
    if (IS_FD(file)) {
        return file;
    }
    return NULL; // File descriptor not allocated
}

struct vfs_file *vfs_fdtable_dealloc_fd(struct vfs_fdtable *fdtable, int fd) {
    if (fdtable == NULL || fd < 0 || fd >= NOFILE) {
        return NULL; // Invalid arguments
    }
    struct vfs_file *file = fdtable->files[fd];
    if (!IS_FD(file)) {
        return NULL; // File descriptor not allocated
    }

    // Case 1: Table was full (next_fd == -1), this fd becomes the new head
    if (fdtable->next_fd == -1) {
        fdtable->files[fd] = NULL;
        fdtable->next_fd = fd;
        goto out;
    }

    // Case 2: This fd is smaller than next_fd, insert at head
    if (fd < fdtable->next_fd) {
        fdtable->files[fd] = (struct vfs_file *)(uint64)(fdtable->next_fd);
        fdtable->next_fd = fd;
        goto out;
    }

    // Case 3: Find the last free fd before this one and insert after it
    int last_free = fd - 1;
    while (last_free >= 0 && IS_FD(fdtable->files[last_free])) {
        last_free--;
    }
    if (last_free >= 0) {
        fdtable->files[fd] = fdtable->files[last_free];
        fdtable->files[last_free] = (struct vfs_file *)(uint64)(fd);
    } else {
        // No free slot before fd, but next_fd exists and is > fd
        // This shouldn't happen given the checks above, but handle defensively
        fdtable->files[fd] = (struct vfs_file *)(uint64)(fdtable->next_fd);
        fdtable->next_fd = fd;
    }

out:
    fdtable->fd_count--;
    return file;
}
