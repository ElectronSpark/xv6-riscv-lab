/*
 * pipe.c - Pipe implementation
 *
 * Provides pipe read/write/close operations.
 * Pipe file operations conform to the VFS file_ops interface.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "string.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "lock/mutex_types.h"
#include "lock/rwsem.h"
#include "vfs/pipe.h"
#include "vfs/file.h"
#include "kqueue_types.h"
#include "vfs/vfs_types.h"
#include "vfs/fcntl.h"
#include "vfs/poll.h"
#include <mm/vm.h>
#include "mm/slab.h"
#include "proc/sched.h"
#include "signal.h"

static slab_cache_t __pipe_cache = {0};

void pipe_init(void) {
    int ret = slab_cache_init(&__pipe_cache, "pipe_cache", sizeof(struct pipe),
                              SLAB_FLAG_STATIC);
    assert(ret == 0, "Failed to initialize pipe_cache slab cache, errno=%d",
           ret);
}

struct pipe *pipe_alloc(int flags) {
    struct pipe *pi = slab_alloc(&__pipe_cache);
    if (pi == NULL) {
        return ERR_PTR(-ENOMEM);
    }

    void *data = kvmalloc(PIPESIZE);
    if (data == NULL) {
        slab_free(pi);
        return ERR_PTR(-ENOMEM);
    }

    // Initialize pipe
    smp_store_release(&pi->flags, PIPE_FLAGS_RW | flags);
    pi->nwrite = 0;
    pi->nread = 0;
    pi->data = data;
    spin_init(&pi->reader_lock, "vfs_pipe_reader");
    spin_init(&pi->writer_lock, "vfs_pipe_writer");
    tq_init(&pi->nread_queue, "pipe_nread_queue", NULL);
    tq_init(&pi->nwrite_queue, "pipe_nwrite_queue", NULL);
    pi->read_file = NULL;
    pi->write_file = NULL;
    return pi;
}

void pipe_close(struct pipe *pi, int writable) {
    bool freed = false;
    struct vfs_file *notify_file = NULL;
    int notify_filter = 0;

    if (writable) {
        /*
         * Acquire writer_lock to synchronize with a concurrent close of
         * the read-end.  Both directions use writer_lock to protect the
         * cross-reference pointers (read_file / write_file).  Take a file
         * reference under the lock, then notify after unlocking; kqueue
         * notification can re-enter source poll/close paths.
         */
        spin_lock(&pi->writer_lock);
        struct vfs_file *rf = pi->read_file;
        pi->write_file = NULL;
        if (rf != NULL) {
            notify_file = vfs_fdup(rf);
            notify_filter = EVFILT_READ;
        }
        freed = PIPE_CLEAR_WRITABLE(pi);
        tq_wakeup_all(&pi->nread_queue, -1, 0);
        spin_unlock(&pi->writer_lock);
    } else {
        /*
         * Acquire writer_lock (same lock as close-write) to serialise
         * the cross-reference notification.  Clear our own back-pointer
         * so the write-end close (if concurrent) won't see a stale
         * read_file pointer.
         */
        spin_lock(&pi->writer_lock);
        struct vfs_file *wf = pi->write_file;
        pi->read_file = NULL;
        if (wf != NULL) {
            notify_file = vfs_fdup(wf);
            notify_filter = EVFILT_WRITE;
        }
        spin_unlock(&pi->writer_lock);

        spin_lock(&pi->reader_lock);
        freed = PIPE_CLEAR_READABLE(pi);
        tq_wakeup_all(&pi->nwrite_queue, -1, 0);
        spin_unlock(&pi->reader_lock);
    }
    if (notify_file != NULL) {
        vfs_file_knote_notify(notify_file, notify_filter, 0);
        vfs_fput(notify_file);
    }
    if (freed) {
        kvfree(pi->data);
        slab_free(pi);
    }
}

void pipe_set_flags(struct pipe *pi, int flags) {
    int set = flags & PIPE_NONBLOCK_MASK;
    // Atomically clear both nonblock bits then set the requested ones
    int old;
    do {
        old = smp_load_acquire(&pi->flags);
    } while (!__atomic_compare_exchange_n(
        &pi->flags, &old, (old & ~PIPE_NONBLOCK_MASK) | set, false,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
}

int pipe_get_flags(struct pipe *pi) {
    return smp_load_acquire(&pi->flags) & PIPE_NONBLOCK_MASK;
}

#define PIPE_READABLE_SIZE(nwrite, nread) ((nwrite) - (nread))
#define PIPE_WRITABLE_SIZE(nwrite, nread)                                      \
    (PIPESIZE - PIPE_READABLE_SIZE(nwrite, nread))

static int __pipe_wait_writer(struct pipe *pi) {
    spin_lock(&pi->writer_lock);
    if (!PIPE_WRITABLE(pi) || killed(current)) {
        spin_unlock(&pi->writer_lock);
        // Return 0 to let caller re-check and detect EOF properly
        return 0;
    }
    /*
     * Re-check data availability after the reader→writer lock transition.
     *
     * pipe_read releases reader_lock before calling us.  A writer may
     * sneak in during that gap: write data, wake nread_queue (nobody on
     * it yet), and leave.  Without this re-check the reader would sleep
     * even though data is already in the pipe — a classic lost-wakeup.
     */
    if (smp_load_acquire(&pi->nwrite) != smp_load_acquire(&pi->nread)) {
        spin_unlock(&pi->writer_lock);
        return 0; /* data available, caller will re-check under reader_lock */
    }
    tq_wait_in_state(&pi->nread_queue, &pi->writer_lock, NULL,
                     THREAD_INTERRUPTIBLE);
    spin_unlock(&pi->writer_lock);
    if (signal_pending(current))
        return -EINTR;
    // Return 0 to re-check conditions (wakeup may be from close or data)
    return 0;
}

static int __pipe_wait_reader(struct pipe *pi) {
    spin_lock(&pi->reader_lock);
    if (!PIPE_READABLE(pi) || killed(current)) {
        spin_unlock(&pi->reader_lock);
        // Return 0 to let caller re-check and detect broken pipe properly
        return 0;
    }
    /*
     * Re-check space availability after the writer→reader lock transition.
     * Same lost-wakeup avoidance as __pipe_wait_writer (see comment there).
     */
    if (PIPE_WRITABLE_SIZE(smp_load_acquire(&pi->nwrite),
                           smp_load_acquire(&pi->nread)) > 0) {
        spin_unlock(&pi->reader_lock);
        return 0; /* space available, caller will re-check under writer_lock */
    }
    tq_wait_in_state(&pi->nwrite_queue, &pi->reader_lock, NULL,
                     THREAD_INTERRUPTIBLE);
    spin_unlock(&pi->reader_lock);
    if (signal_pending(current))
        return -EINTR;
    // Return 0 to re-check conditions (wakeup may be from close or space
    // available)
    return 0;
}

/******************************************************************************
 * Standalone pipe read/write
 *
 * These are the core pipe I/O routines.  They operate on a bare
 * `struct pipe *` and know nothing about VFS file structures.
 * The `user` flag selects user-space vs kernel-space buffer handling.
 ******************************************************************************/

ssize_t pipe_read(struct pipe *pi, char *buf, size_t count, bool user) {
    struct thread *pr = current;
    ssize_t total = 0;
    int ret = 0;
    char tmp[128];
    size_t tmp_pos = 0;
    size_t tmp_len = 0;
    bool nonblock = PIPE_NONBLOCK_RD(pi);

    while ((size_t)total < count) {
        spin_lock(&pi->reader_lock);
        while (tmp_len == 0) {
            uint nwrite = smp_load_acquire(&pi->nwrite);
            uint nread_old = pi->nread;
            size_t readable = PIPE_READABLE_SIZE(nwrite, nread_old);
            if (readable == 0) {
                if (!PIPE_WRITABLE(pi)) {
                    // Writer closed and no data left - EOF
                    spin_unlock(&pi->reader_lock);
                    goto out;
                }
                if (killed(pr)) {
                    spin_unlock(&pi->reader_lock);
                    return -1;
                }
                /*
                 * POSIX short-read: if we already copied some data to
                 * the caller, return it now instead of blocking for
                 * more.  This is essential for interactive / streaming
                 * consumers (terminals, telnet, etc.).
                 */
                if (total > 0) {
                    spin_unlock(&pi->reader_lock);
                    goto out;
                }
                if (nonblock) {
                    spin_unlock(&pi->reader_lock);
                    return -EAGAIN;
                }
                tq_wakeup_all(&pi->nwrite_queue, 0, 0);
                spin_unlock(&pi->reader_lock);

                ret = __pipe_wait_writer(pi);
                if (ret < 0) {
                    if (total > 0)
                        goto out;
                    return ret;
                }
                spin_lock(&pi->reader_lock);
            } else {
                size_t read_size =
                    min(min(count - (size_t)total, readable), sizeof(tmp));
                uint nread = nread_old + read_size;
                uint nread_idx = nread_old % PIPESIZE;

                if (nread_idx + read_size <= PIPESIZE) {
                    memmove(tmp, &pi->data[nread_idx], read_size);
                } else {
                    size_t first_part = PIPESIZE - nread_idx;
                    memmove(tmp, &pi->data[nread_idx], first_part);
                    memmove(&tmp[first_part], &pi->data[0],
                            read_size - first_part);
                }

                smp_store_release(&pi->nread, nread);
                tmp_len = read_size;
            }
        }
        spin_unlock(&pi->reader_lock);

        // Copy data out (user or kernel)
        size_t copy_size = min(tmp_len - tmp_pos, count - (size_t)total);
        if (user) {
            if (vm_copyout(pr->vm, (uint64)(buf + total), &tmp[tmp_pos],
                           copy_size) < 0)
                goto out;
        } else {
            memmove(buf + total, &tmp[tmp_pos], copy_size);
        }
        total += copy_size;
        tmp_pos += copy_size;
        if (tmp_pos >= tmp_len) {
            tmp_pos = 0;
            tmp_len = 0;
        }
    }
out:
    spin_lock(&pi->reader_lock);
    tq_wakeup_all(&pi->nwrite_queue, 0, 0);
    spin_unlock(&pi->reader_lock);
    return total;
}

ssize_t pipe_write(struct pipe *pi, const char *buf, size_t count, bool user) {
    struct thread *pr = current;
    ssize_t total = 0;
    int ret = 0;
    char tmp[128];
    size_t tmp_pos = 0;
    size_t tmp_len = 0;
    bool nonblock = PIPE_NONBLOCK_WR(pi);

    while ((size_t)total < count) {
        if (tmp_len == 0) {
            tmp_len = min(count - (size_t)total, sizeof(tmp));
            if (tmp_len == 0)
                goto out;
            if (user) {
                if (vm_copyin(pr->vm, tmp, (uint64)(buf + total), tmp_len) < 0)
                    goto out;
            } else {
                memmove(tmp, buf + total, tmp_len);
            }
        }
        total += tmp_len;
        spin_lock(&pi->writer_lock);
        while (tmp_len > tmp_pos) {
            uint nread = smp_load_acquire(&pi->nread);
            if (!PIPE_READABLE(pi) || killed(pr)) {
                spin_unlock(&pi->writer_lock);
                return -1;
            }
            uint nwrite_old = pi->nwrite;
            size_t writable = PIPE_WRITABLE_SIZE(nwrite_old, nread);
            if (writable == 0) {
                if (nonblock) {
                    spin_unlock(&pi->writer_lock);
                    if (total > (ssize_t)tmp_len)
                        goto out;
                    return -EAGAIN;
                }
                tq_wakeup_all(&pi->nread_queue, 0, 0);
                spin_unlock(&pi->writer_lock);

                ret = __pipe_wait_reader(pi);
                if (ret < 0) {
                    if (total > (ssize_t)tmp_len)
                        goto out;
                    return ret;
                }
                spin_lock(&pi->writer_lock);
            } else {
                size_t write_size = min(tmp_len - tmp_pos, writable);
                uint nwrite = nwrite_old + write_size;
                uint nwrite_idx = nwrite_old % PIPESIZE;

                if (nwrite_idx + write_size <= PIPESIZE) {
                    memmove(&pi->data[nwrite_idx], &tmp[tmp_pos], write_size);
                } else {
                    size_t first_part = PIPESIZE - nwrite_idx;
                    memmove(&pi->data[nwrite_idx], &tmp[tmp_pos], first_part);
                    memmove(&pi->data[0], &tmp[tmp_pos + first_part],
                            write_size - first_part);
                }

                smp_store_release(&pi->nwrite, nwrite);
                tmp_pos += write_size;
            }
        }
        spin_unlock(&pi->writer_lock);
        tmp_pos = 0;
        tmp_len = 0;
    }
out:
    spin_lock(&pi->writer_lock);
    tq_wakeup_all(&pi->nread_queue, 0, 0);
    spin_unlock(&pi->writer_lock);
    return total - (tmp_len - tmp_pos);
}

/******************************************************************************
 * VFS file_ops adapters
 *
 * Thin wrappers that extract `struct pipe *` from the VFS file and delegate
 * to the standalone pipe_read / pipe_write above.
 ******************************************************************************/

static ssize_t __pipe_file_read(struct vfs_file *file, char *buf, size_t count,
                                bool user) {
    struct pipe *pi = file->pipe;
    ssize_t ret = pipe_read(pi, buf, count, user);
    /* kqueue: after consuming data, notify the write-side that the pipe
     * is writable.  Hold writer_lock to serialise with pipe_close, which
     * clears the cross-reference pointers under the same lock.  Notify
     * outside the lock because kqueue can synchronously poll the source. */
    if (ret > 0) {
        struct vfs_file *wf_ref = NULL;
        spin_lock(&pi->writer_lock);
        struct vfs_file *wf = pi->write_file;
        if (wf != NULL)
            wf_ref = vfs_fdup(wf);
        spin_unlock(&pi->writer_lock);
        if (wf_ref != NULL) {
            vfs_file_knote_notify(wf_ref, EVFILT_WRITE, 0);
            vfs_fput(wf_ref);
        }
    }
    return ret;
}

static ssize_t __pipe_file_write(struct vfs_file *file, const char *buf,
                                 size_t count, bool user) {
    struct pipe *pi = file->pipe;
    ssize_t ret = pipe_write(pi, buf, count, user);
    /* kqueue: after producing data, notify the read-side that the pipe
     * is readable.  Hold writer_lock to serialise with pipe_close, which
     * clears the cross-reference pointers under the same lock.  Notify
     * outside the lock because kqueue can synchronously poll the source. */
    if (ret > 0) {
        struct vfs_file *rf_ref = NULL;
        spin_lock(&pi->writer_lock);
        struct vfs_file *rf = pi->read_file;
        if (rf != NULL)
            rf_ref = vfs_fdup(rf);
        spin_unlock(&pi->writer_lock);
        if (rf_ref != NULL) {
            vfs_file_knote_notify(rf_ref, EVFILT_READ, 0);
            vfs_fput(rf_ref);
        }
    }
    return ret;
}

static int __pipe_file_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode; // pipes have no inode
    if (file->pipe != NULL) {
        pipe_close(file->pipe, (file->f_flags & O_ACCMODE) != O_RDONLY);
        file->pipe = NULL;
    }
    return 0;
}

/*
 * __pipe_file_poll - check pipe readiness for I/O
 *
 * Checks the pipe's nread/nwrite counters and end-of-pipe flags
 * without blocking.  Returns the subset of @events that are ready.
 */
static int __pipe_file_poll(struct vfs_file *file, short events) {
    struct pipe *pi = file->pipe;
    short revents = 0;

    if (pi == NULL)
        return POLLNVAL;

    uint nwrite = smp_load_acquire(&pi->nwrite);
    uint nread  = smp_load_acquire(&pi->nread);
    size_t readable = (size_t)(nwrite - nread);
    size_t writable = PIPESIZE - readable;

    if (events & (POLLIN | POLLRDNORM | POLLRDBAND | POLLRDHUP)) {
        if (readable > 0 || !PIPE_WRITABLE(pi))
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
    }

    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
        if (writable > 0 && PIPE_READABLE(pi))
            revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
    }

    if (!PIPE_READABLE(pi))
        revents |= POLLERR;
    if (!PIPE_WRITABLE(pi)) {
        revents |= POLLHUP;
        if (events & POLLRDHUP)
            revents |= POLLRDHUP;
    }

    return revents;
}

static struct vfs_file_ops pipe_file_ops = {
    .read = __pipe_file_read,
    .write = __pipe_file_write,
    .llseek = NULL,
    .release = __pipe_file_release,
    .fsync = NULL,
    .poll = __pipe_file_poll,
    .fault = NULL,
};

void pipe_open(struct vfs_file *file, struct pipe *pi, int f_flags) {
    file->f_flags = f_flags;
    file->pipe = pi;
    file->ops = &pipe_file_ops;
    file->f_kind = VFS_FILE_KIND_PIPE;
    /* kqueue: record file endpoint for cross-notification */
    if ((f_flags & O_ACCMODE) == O_RDONLY)
        pi->read_file = file;
    else
        pi->write_file = file;
}
