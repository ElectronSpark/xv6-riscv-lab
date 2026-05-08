#include "types.h"
#include "defs.h"
#include "errno.h"
#include "string.h"
#include "list.h"
#include "proc/thread.h"
#include "proc/tq.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "vfs/file_lock.h"
#include "vfs/fs.h"
#include "vfs/stat.h"
#include <mm/slab.h>

#define VFS_FILE_LOCK_EOF ((loff_t)(((uint64)-1) >> 1))

struct vfs_file_lock_range {
    list_node_t entry;
    pid_t owner;
    short type;
    loff_t start;
    loff_t end;
};

static slab_cache_t __vfs_file_lock_cache = {0};

static struct vfs_file_lock_range *__vfs_file_lock_alloc(void) {
    struct vfs_file_lock_range *lock = slab_alloc(&__vfs_file_lock_cache);
    if (lock != NULL) {
        memset(lock, 0, sizeof(*lock));
        list_entry_init(&lock->entry);
    }
    return lock;
}

static void __vfs_file_lock_free(struct vfs_file_lock_range *lock) {
    if (lock != NULL) {
        slab_free(lock);
    }
}

void __vfs_file_lock_global_init(void) {
    int ret = slab_cache_init(&__vfs_file_lock_cache, "vfs_file_lock_cache",
                              sizeof(struct vfs_file_lock_range),
                              SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0,
           "Failed to initialize vfs_file_lock_cache slab cache, errno=%d",
           ret);
}

static int __vfs_file_lock_normalize(struct vfs_file *file,
                                     const struct flock *in,
                                     struct flock *out,
                                     loff_t *start_out,
                                     loff_t *end_out) {
    if (file == NULL || in == NULL || out == NULL || start_out == NULL ||
        end_out == NULL) {
        return -EINVAL;
    }

    if (in->l_type != F_RDLCK && in->l_type != F_WRLCK &&
        in->l_type != F_UNLCK) {
        return -EINVAL;
    }

    loff_t base = 0;
    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    switch (in->l_whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = file->f_pos;
        break;
    case SEEK_END:
        if (inode == NULL) {
            return -EINVAL;
        }
        vfs_ilock(inode);
        base = inode->size;
        vfs_iunlock(inode);
        break;
    default:
        return -EINVAL;
    }

    loff_t rel = in->l_start;
    loff_t start = base + rel;
    loff_t end = start;
    if (start < 0) {
        return -EINVAL;
    }

    if (in->l_len > 0) {
        end = start + in->l_len - 1;
        if (end < start) {
            return -EINVAL;
        }
    } else if (in->l_len == 0) {
        end = VFS_FILE_LOCK_EOF;
    } else {
        start = start + in->l_len;
        end = base + rel - 1;
        if (start < 0 || end < start) {
            return -EINVAL;
        }
    }

    *out = *in;
    out->l_whence = SEEK_SET;
    out->l_start = start;
    out->l_len = (end == VFS_FILE_LOCK_EOF) ? 0 : (end - start + 1);
    *start_out = start;
    *end_out = end;
    return 0;
}

static bool __vfs_file_lock_overlaps(loff_t start1, loff_t end1, loff_t start2,
                                     loff_t end2) {
    return !(end1 < start2 || end2 < start1);
}

static bool __vfs_file_lock_conflicts(const struct vfs_file_lock_range *lock,
                                      pid_t owner, short type, loff_t start,
                                      loff_t end) {
    if (lock->owner == owner) {
        return false;
    }
    if (!__vfs_file_lock_overlaps(lock->start, lock->end, start, end)) {
        return false;
    }
    return lock->type == F_WRLCK || type == F_WRLCK;
}

static struct vfs_file_lock_range *__vfs_file_lock_find_conflict(
    struct vfs_inode *inode, pid_t owner, short type, loff_t start,
    loff_t end) {
    struct vfs_file_lock_range *lock = NULL;
    struct vfs_file_lock_range *tmp = NULL;

    list_foreach_node_safe(&inode->file_locks, lock, tmp, entry) {
        if (__vfs_file_lock_conflicts(lock, owner, type, start, end)) {
            return lock;
        }
    }
    return NULL;
}

static void __vfs_file_lock_insert_sorted(struct vfs_inode *inode,
                                          struct vfs_file_lock_range *lock) {
    struct vfs_file_lock_range *pos = NULL;
    struct vfs_file_lock_range *tmp = NULL;

    list_foreach_node_safe(&inode->file_locks, pos, tmp, entry) {
        if (lock->start < pos->start ||
            (lock->start == pos->start && lock->owner < pos->owner)) {
            list_entry_insert(pos->entry.prev, &lock->entry);
            return;
        }
    }

    list_node_push(&inode->file_locks, lock, entry);
}

static int __vfs_file_lock_trim_owner(struct vfs_inode *inode, pid_t owner,
                                      loff_t start, loff_t end) {
    struct vfs_file_lock_range *lock = NULL;
    struct vfs_file_lock_range *tmp = NULL;

    list_foreach_node_safe(&inode->file_locks, lock, tmp, entry) {
        if (lock->owner != owner ||
            !__vfs_file_lock_overlaps(lock->start, lock->end, start, end)) {
            continue;
        }

        if (start <= lock->start && end >= lock->end) {
            list_node_detach(lock, entry);
            __vfs_file_lock_free(lock);
            continue;
        }

        if (start > lock->start && end < lock->end) {
            struct vfs_file_lock_range *right = __vfs_file_lock_alloc();
            if (right == NULL) {
                return -ENOMEM;
            }
            right->owner = lock->owner;
            right->type = lock->type;
            right->start = end + 1;
            right->end = lock->end;
            lock->end = start - 1;
            __vfs_file_lock_insert_sorted(inode, right);
            continue;
        }

        if (start <= lock->start) {
            lock->start = end + 1;
        } else {
            lock->end = start - 1;
        }
    }

    return 0;
}

static void __vfs_file_lock_merge_owner(struct vfs_inode *inode,
                                        struct vfs_file_lock_range *lock) {
    struct vfs_file_lock_range *pos = NULL;
    struct vfs_file_lock_range *tmp = NULL;

    list_foreach_node_safe(&inode->file_locks, pos, tmp, entry) {
        if (pos == lock || pos->owner != lock->owner || pos->type != lock->type) {
            continue;
        }
        if (pos->end != VFS_FILE_LOCK_EOF && pos->end + 1 < lock->start) {
            continue;
        }
        if (lock->end != VFS_FILE_LOCK_EOF && lock->end + 1 < pos->start) {
            continue;
        }
        if (!__vfs_file_lock_overlaps(pos->start,
                                      pos->end == VFS_FILE_LOCK_EOF
                                          ? VFS_FILE_LOCK_EOF
                                          : pos->end + 1,
                                      lock->start,
                                      lock->end == VFS_FILE_LOCK_EOF
                                          ? VFS_FILE_LOCK_EOF
                                          : lock->end + 1)) {
            continue;
        }

        if (pos->start < lock->start) {
            lock->start = pos->start;
        }
        if (pos->end == VFS_FILE_LOCK_EOF || lock->end == VFS_FILE_LOCK_EOF) {
            lock->end = VFS_FILE_LOCK_EOF;
        } else if (pos->end > lock->end) {
            lock->end = pos->end;
        }
        list_node_detach(pos, entry);
        __vfs_file_lock_free(pos);
    }
}

static int __vfs_file_lock_apply(struct vfs_inode *inode, pid_t owner,
                                 short type, loff_t start, loff_t end) {
    int ret = __vfs_file_lock_trim_owner(inode, owner, start, end);
    if (ret != 0 || type == F_UNLCK) {
        return ret;
    }

    struct vfs_file_lock_range *lock = __vfs_file_lock_alloc();
    if (lock == NULL) {
        return -ENOMEM;
    }
    lock->owner = owner;
    lock->type = type;
    lock->start = start;
    lock->end = end;
    __vfs_file_lock_insert_sorted(inode, lock);
    __vfs_file_lock_merge_owner(inode, lock);
    return 0;
}

int vfs_file_lock_ctl(struct vfs_file *file, pid_t owner, int cmd,
                      struct flock *fl) {
    if (file == NULL || fl == NULL) {
        return -EINVAL;
    }

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL || S_ISFIFO(inode->mode) || S_ISSOCK(inode->mode)) {
        return -EINVAL;
    }

    struct flock normalized = {0};
    loff_t start = 0;
    loff_t end = 0;
    int ret = __vfs_file_lock_normalize(file, fl, &normalized, &start, &end);
    if (ret != 0) {
        return ret;
    }

    bool getlk = cmd == F_GETLK || cmd == F_OFD_GETLK;
    bool setlkw = cmd == F_SETLKW || cmd == F_OFD_SETLKW;

    for (;;) {
        spin_lock(&inode->file_lock);
        struct vfs_file_lock_range *conflict =
            __vfs_file_lock_find_conflict(inode, owner, normalized.l_type,
                                          start, end);

        if (getlk) {
            if (conflict == NULL) {
                fl->l_type = F_UNLCK;
                fl->l_whence = SEEK_SET;
                fl->l_start = normalized.l_start;
                fl->l_len = normalized.l_len;
                fl->l_pid = 0;
            } else {
                fl->l_type = conflict->type;
                fl->l_whence = SEEK_SET;
                fl->l_start = conflict->start;
                fl->l_len = (conflict->end == VFS_FILE_LOCK_EOF)
                                ? 0
                                : (conflict->end - conflict->start + 1);
                fl->l_pid = conflict->owner < 0 ? -1 : conflict->owner;
            }
            spin_unlock(&inode->file_lock);
            return 0;
        }

        if (conflict == NULL) {
            ret = __vfs_file_lock_apply(inode, owner, normalized.l_type, start,
                                        end);
            if (ret == 0) {
                (void)tq_wakeup_all(&inode->file_lock_waiters, 0, 0);
            }
            spin_unlock(&inode->file_lock);
            return ret;
        }

        if (!setlkw) {
            spin_unlock(&inode->file_lock);
            return -EAGAIN;
        }

        ret = tq_wait_in_state(&inode->file_lock_waiters, &inode->file_lock,
                               NULL, THREAD_INTERRUPTIBLE);
        if (ret != 0) {
            return ret;
        }
    }
}

void vfs_file_lock_release(struct vfs_file *file, pid_t owner) {
    if (file == NULL) {
        return;
    }

    struct vfs_inode *inode = vfs_inode_deref(&file->inode);
    if (inode == NULL) {
        return;
    }

    spin_lock(&inode->file_lock);
    if (__vfs_file_lock_trim_owner(inode, owner, 0, VFS_FILE_LOCK_EOF) == 0) {
        (void)tq_wakeup_all(&inode->file_lock_waiters, 0, 0);
    }
    spin_unlock(&inode->file_lock);
}
