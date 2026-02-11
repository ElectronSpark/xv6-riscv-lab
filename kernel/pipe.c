/*
 * pipe.c - Pipe implementation
 *
 * Provides pipe read/write/close operations for kernel and user space.
 * Legacy pipealloc removed - VFS uses vfs_pipealloc in kernel/vfs/file.c
 * instead.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "string.h"
#include "printf.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "lock/mutex_types.h"
#include "pipe.h"
#include <mm/vm.h>
#include "proc/sched.h"
#include "signal.h"

void pipeclose(struct pipe *pi, int writable) {
    bool freed = false;
    if (writable) {
        spin_lock(&pi->writer_lock);
        freed = PIPE_CLEAR_WRITABLE(pi);
        tq_wakeup_all(&pi->nread_queue, -1, 0);
        spin_unlock(&pi->writer_lock);
    } else {
        spin_lock(&pi->reader_lock);
        freed = PIPE_CLEAR_READABLE(pi);
        tq_wakeup_all(&pi->nwrite_queue, -1, 0);
        spin_unlock(&pi->reader_lock);
    }
    if (freed) {
        kfree((char *)pi);
    }
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
    tq_wait(&pi->nread_queue, &pi->writer_lock, NULL);
    spin_unlock(&pi->writer_lock);
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
    tq_wait(&pi->nwrite_queue, &pi->reader_lock, NULL);
    spin_unlock(&pi->reader_lock);
    // Return 0 to re-check conditions (wakeup may be from close or space
    // available)
    return 0;
}

int pipewrite(struct pipe *pi, uint64 addr, int n) {
    int i = 0;
    int ret = 0;
    struct thread *pr = current;
    char buf[128];
    size_t buf_pos = 0;
    size_t buf_len = 0;

    while (i < n) {
        if (buf_len == 0) {
            buf_len = min(n - i, sizeof(buf));
            if (buf_len == 0) {
                goto out;
            }
            if (vm_copyin(pr->vm, buf, addr + i, buf_len) == -1) {
                goto out;
            }
        }
        i += buf_len;
        spin_lock(&pi->writer_lock);
        while (buf_len > buf_pos) {
            uint nread = smp_load_acquire(&pi->nread);
            if (!PIPE_READABLE(pi) || killed(pr)) {
                spin_unlock(&pi->writer_lock);
                return -1;
            }
            uint nwrite_old = pi->nwrite;
            size_t writable = PIPE_WRITABLE_SIZE(nwrite_old, nread);
            if (writable == 0) {
                tq_wakeup_all(&pi->nread_queue, 0, 0);
                spin_unlock(&pi->writer_lock);

                ret = __pipe_wait_reader(
                    pi); // need to acquire reader_lock to wait for reader
                if (ret < 0) {
                    goto out;
                }
                spin_lock(&pi->writer_lock);
            } else {
                size_t write_size = min(buf_len - buf_pos, writable);
                uint nwrite = nwrite_old + write_size;
                uint nwrite_idx = nwrite_old % PIPESIZE;

                if (nwrite_idx + write_size <= PIPESIZE) {
                    // No wrap-around
                    memmove(&pi->data[nwrite_idx], &buf[buf_pos], write_size);
                } else {
                    // Wrap-around
                    size_t first_part = PIPESIZE - nwrite_idx;
                    memmove(&pi->data[nwrite_idx], &buf[buf_pos], first_part);
                    memmove(&pi->data[0], &buf[buf_pos + first_part],
                            write_size - first_part);
                }

                smp_store_release(&pi->nwrite, nwrite);
                buf_pos += write_size;
            }
        }
        spin_unlock(&pi->writer_lock);
        buf_pos = 0;
        buf_len = 0;
    }
out:
    spin_lock(&pi->writer_lock);
    tq_wakeup_all(&pi->nread_queue, 0, 0);
    spin_unlock(&pi->writer_lock);
    return i - (buf_len - buf_pos);
}

int piperead(struct pipe *pi, uint64 addr, int n) {
    int i = 0;
    int ret = 0;
    struct thread *pr = current;
    char buf[128];
    size_t buf_pos = 0;
    size_t buf_len = 0;

    while (i < n) {
        spin_lock(&pi->reader_lock);
        while (buf_len == 0) {
            uint nwrite = smp_load_acquire(&pi->nwrite);
            uint nread_old = pi->nread;
            size_t readable = PIPE_READABLE_SIZE(nwrite, nread_old);
            if (readable == 0) {
                // Pipe is empty - check if we should wait or return EOF
                if (!PIPE_WRITABLE(pi)) {
                    // Writer closed and no data left - EOF
                    spin_unlock(&pi->reader_lock);
                    goto out;
                }
                if (killed(pr)) {
                    spin_unlock(&pi->reader_lock);
                    return -1;
                }
                // Pipe empty but writer still open - wait for data
                tq_wakeup_all(&pi->nwrite_queue, 0, 0);
                spin_unlock(&pi->reader_lock);

                ret = __pipe_wait_writer(pi);
                if (ret < 0) {
                    goto out;
                }
                spin_lock(&pi->reader_lock);
            } else {
                // Data available - read it (even if writer closed)
                size_t read_size =
                    min(min((size_t)(n - i), readable), sizeof(buf));
                uint nread = nread_old + read_size;
                uint nread_idx = nread_old % PIPESIZE;

                if (nread_idx + read_size <= PIPESIZE) {
                    // No wrap-around
                    memmove(buf, &pi->data[nread_idx], read_size);
                } else {
                    // Wrap-around
                    size_t first_part = PIPESIZE - nread_idx;
                    memmove(buf, &pi->data[nread_idx], first_part);
                    memmove(&buf[first_part], &pi->data[0],
                            read_size - first_part);
                }

                smp_store_release(&pi->nread, nread);
                buf_len = read_size;
            }
        }
        spin_unlock(&pi->reader_lock);

        // Copy to user space outside the lock
        size_t copy_size = min(buf_len - buf_pos, (size_t)(n - i));
        if (vm_copyout(pr->vm, addr + i, &buf[buf_pos], copy_size) == -1) {
            goto out;
        }
        i += copy_size;
        buf_pos += copy_size;
        if (buf_pos >= buf_len) {
            buf_pos = 0;
            buf_len = 0;
        }
    }
out:
    spin_lock(&pi->reader_lock);
    tq_wakeup_all(&pi->nwrite_queue, 0, 0);
    spin_unlock(&pi->reader_lock);
    return i;
}

// Kernel-mode pipe read (for VFS layer)
int piperead_kernel(struct pipe *pi, char *buf, int n) {
    int i = 0;
    int ret = 0;
    struct thread *pr = current;

    while (i < n) {
        spin_lock(&pi->reader_lock);
        uint nwrite = smp_load_acquire(&pi->nwrite);
        uint nread_old = pi->nread;
        size_t readable = PIPE_READABLE_SIZE(nwrite, nread_old);

        if (readable == 0) {
            // Pipe is empty - check if we should wait or return EOF
            if (!PIPE_WRITABLE(pi)) {
                // Writer closed and no data left - EOF
                spin_unlock(&pi->reader_lock);
                goto out;
            }
            if (killed(pr)) {
                spin_unlock(&pi->reader_lock);
                return -1;
            }
            // Pipe empty but writer still open - wait for data
            tq_wakeup_all(&pi->nwrite_queue, 0, 0);
            spin_unlock(&pi->reader_lock);

            ret = __pipe_wait_writer(pi);
            if (ret < 0) {
                goto out;
            }
        } else {
            // Data available - read it (even if writer closed)
            size_t read_size = min((size_t)(n - i), readable);
            uint nread = nread_old + read_size;
            uint nread_idx = nread_old % PIPESIZE;

            if (nread_idx + read_size <= PIPESIZE) {
                // No wrap-around
                memmove(&buf[i], &pi->data[nread_idx], read_size);
            } else {
                // Wrap-around
                size_t first_part = PIPESIZE - nread_idx;
                memmove(&buf[i], &pi->data[nread_idx], first_part);
                memmove(&buf[i + first_part], &pi->data[0],
                        read_size - first_part);
            }

            smp_store_release(&pi->nread, nread);
            i += read_size;
            spin_unlock(&pi->reader_lock);
        }
    }
out:
    spin_lock(&pi->reader_lock);
    tq_wakeup_all(&pi->nwrite_queue, 0, 0);
    spin_unlock(&pi->reader_lock);
    return i;
}

// Kernel-mode pipe write (for VFS layer)
int pipewrite_kernel(struct pipe *pi, const char *buf, int n) {
    int i = 0;
    int ret = 0;
    struct thread *pr = current;

    while (i < n) {
        spin_lock(&pi->writer_lock);
        uint nread = smp_load_acquire(&pi->nread);
        if (!PIPE_READABLE(pi) || killed(pr)) {
            spin_unlock(&pi->writer_lock);
            return -1;
        }
        uint nwrite_old = pi->nwrite;
        size_t writable = PIPE_WRITABLE_SIZE(nwrite_old, nread);
        if (writable == 0) {
            tq_wakeup_all(&pi->nread_queue, 0, 0);
            spin_unlock(&pi->writer_lock);

            ret = __pipe_wait_reader(pi);
            if (ret < 0) {
                goto out;
            }
        } else {
            size_t write_size = min((size_t)(n - i), writable);
            uint nwrite = nwrite_old + write_size;
            uint nwrite_idx = nwrite_old % PIPESIZE;

            if (nwrite_idx + write_size <= PIPESIZE) {
                // No wrap-around
                memmove(&pi->data[nwrite_idx], &buf[i], write_size);
            } else {
                // Wrap-around
                size_t first_part = PIPESIZE - nwrite_idx;
                memmove(&pi->data[nwrite_idx], &buf[i], first_part);
                memmove(&pi->data[0], &buf[i + first_part],
                        write_size - first_part);
            }

            smp_store_release(&pi->nwrite, nwrite);
            i += write_size;
            spin_unlock(&pi->writer_lock);
        }
    }
out:
    spin_lock(&pi->writer_lock);
    tq_wakeup_all(&pi->nread_queue, 0, 0);
    spin_unlock(&pi->writer_lock);
    return i;
}
