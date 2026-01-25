#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "atomic.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "vfs/fcntl.h"
#include "lock/spinlock.h"
#include "proc/proc.h"
#include "lock/mutex_types.h"
#include "lock/rwlock.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs_private.h"
#include <mm/slab.h>

#define IS_FD(fd) ((uint64)(fd) > NOFILE)

static slab_cache_t __vfs_fdtable_slab = { 0 };

void __vfs_fdtable_global_init(void) {
    int ret = slab_cache_init(&__vfs_fdtable_slab,
                              "vfs_fdtable_cache",
                              sizeof(struct vfs_fdtable),
                              SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0, "Failed to initialize vfs_fdtable_cache slab cache, errno=%d", ret);
}

static struct vfs_fdtable *__vfs_fdtable_alloc_init(void) {
    struct vfs_fdtable *fdtable = slab_alloc(&__vfs_fdtable_slab);
    if (fdtable == NULL) {
        return NULL;
    }
    memset(fdtable, 0, sizeof(*fdtable));
    spin_init(&fdtable->lock, "vfs_fdtable_lock");
    fdtable->ref_count = 1;
    return fdtable;
}

static void __vfs_fdtable_free(struct vfs_fdtable *fdtable) {
    if (fdtable) {
        slab_free(fdtable);
    }
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

struct vfs_fdtable *vfs_fdtable_init(void) {
    struct vfs_fdtable *fdtable = __vfs_fdtable_alloc_init();
    assert (fdtable != NULL, "vfs_fdtable_init: fdtable is NULL\n");
    for (uint64 i = 0; i < NOFILE; i++) {
        fdtable->files[i] = (void *)(i + 1);
    }
    fdtable->files[NOFILE - 1] = NULL;
    fdtable->next_fd = 0;
    return fdtable;
}

struct vfs_fdtable *vfs_fdtable_clone(struct vfs_fdtable *src, int clone_flags) {
    if (src == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }

    if (clone_flags & CLONE_FILES) {
        // share the fdtable
        atomic_inc(&src->ref_count);
        return src;
    }

    struct vfs_fdtable *dest = __vfs_fdtable_alloc_init();
    if (dest == NULL) {
        return ERR_PTR(-ENOMEM); // Allocation failed
    }
    dest->fd_count = 0;
    memset(dest->files, 0, sizeof(dest->files));

    // Duplicate file references
    spin_lock(&src->lock);
    for (int i = 0; i < NOFILE; i++) {
        struct vfs_file *src_file = src->files[i];
        if (IS_FD(src_file)) {
            struct vfs_file *dst_file = vfs_fdup(src_file);
            if (!IS_ERR_OR_NULL(dst_file)) {
                dest->files[i] = dst_file;
                dest->fd_count++;
            }
        }
    }
    spin_unlock(&src->lock);
    
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

    return dest;
}

void vfs_fdtable_put(struct vfs_fdtable *fdtable) {
    if (fdtable == NULL) {
        return;
    }

    if (atomic_dec_unless(&fdtable->ref_count, 1)) {
        // Still has references
        return;
    }

    // First, close all open files in the range [0, NOFILE)
    for (int i = 0; i < NOFILE; i++) {
        struct vfs_file *file = fdtable->files[i];
        if (IS_FD(file)) {
            vfs_fput(file);
            fdtable->files[i] = NULL;
            fdtable->fd_count--;
        }
    }

    assert(fdtable->fd_count >= 0, "vfs_fdtable_destroy: fd_count negative");
    __vfs_fdtable_free(fdtable);
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
