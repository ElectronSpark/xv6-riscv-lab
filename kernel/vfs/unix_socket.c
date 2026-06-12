/*
 * unix_socket.c - AF_UNIX (UNIX domain) socket implementation
 *
 * Provides local inter-process communication via SOCK_STREAM (connection-
 * oriented byte stream), SOCK_SEQPACKET (connection-oriented packet stream),
 * and SOCK_DGRAM (connectionless datagram) types.
 *
 * Design:
 *   - Each connected socket has a TX ring buffer (like a pipe).
 *   - Reading from a socket reads from peer->tx (the peer's transmit buffer).
 *   - Writing to a socket writes to our own tx (which the peer reads).
 *   - For socketpair, two sockets are created and cross-linked.
 *   - Bind uses an abstract namespace stored in-kernel (no filesystem backing).
 *   - Listen/accept uses a pending-connection queue.
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
#include "vfs/unix_socket.h"
#include "vfs/file.h"
#include "vfs/vfs_types.h"
#include "vfs/fcntl.h"
#include "vfs/poll.h"
#include "kqueue_types.h"
#include <mm/vm.h>
#include "mm/slab.h"
#include "proc/sched.h"
#include "signal.h"

/* From irq/syscall.c — argument fetching */
extern void argint(int n, int *ip);
extern void argaddr(int n, uint64 *ip);

/* ========================================================================== */
/* Slab cache                                                                 */
/* ========================================================================== */

static slab_cache_t __unix_sock_cache = {0};
static slab_cache_t __unix_pending_cache = {0};

/* ========================================================================== */
/* Global bind registry — simple linked list of bound sockets                 */
/* ========================================================================== */

struct unix_bind_entry {
    char path[UNIX_PATH_MAX];
    size_t path_len;
    struct unix_sock *sock;
    struct unix_bind_entry *next;
};

static slab_cache_t __unix_bind_cache = {0};
static struct unix_bind_entry *bind_list_head = NULL;
static spinlock_t bind_list_lock;

/* ========================================================================== */
/* Forward declarations                                                       */
/* ========================================================================== */

static ssize_t unix_file_read(struct vfs_file *file, char *buf, size_t count,
                              bool user);
static ssize_t unix_file_write(struct vfs_file *file, const char *buf,
                               size_t count, bool user);
static int unix_file_release(struct vfs_inode *inode, struct vfs_file *file);
static int unix_file_poll(struct vfs_file *file, short events);
static int unix_file_ioctl(struct vfs_file *file, uint64 cmd, void *arg);

/* ========================================================================== */
/* File operations                                                            */
/* ========================================================================== */

struct vfs_file_ops unix_socket_file_ops = {
    .read    = unix_file_read,
    .write   = unix_file_write,
    .llseek  = NULL,
    .release = unix_file_release,
    .fsync   = NULL,
    .fflush  = NULL,
    .poll    = unix_file_poll,
    .ioctl   = unix_file_ioctl,
    .fault   = NULL,
};

/* ========================================================================== */
/* Ring buffer helpers                                                        */
/* ========================================================================== */

#define RING_READABLE(r) ((r)->nwrite - (r)->nread)
#define RING_WRITABLE(r) ((r)->capacity - RING_READABLE(r))
#define RING_WRITABLE_LIMIT(r, limit) \
    ((RING_READABLE(r) < (limit)) ? ((limit) - RING_READABLE(r)) : 0)

static inline bool unix_sock_connection_oriented(const struct unix_sock *sk)
{
    return sk->type == SOCK_STREAM || sk->type == SOCK_SEQPACKET;
}

static inline size_t unix_packet_count_locked(const struct unix_sock *sk)
{
    if (sk->packet_tail >= sk->packet_head)
        return sk->packet_tail - sk->packet_head;
    return UNIX_PACKET_QUEUE_MAX - sk->packet_head + sk->packet_tail;
}

static int unix_packet_enqueue_locked(struct unix_sock *sk, uint end_mark)
{
    if (sk->type != SOCK_SEQPACKET)
        return 0;
    if ((unsigned long)unix_packet_count_locked(sk) >= UNIX_PACKET_QUEUE_MAX - 1)
        return -EAGAIN;
    sk->packet_queue[sk->packet_tail] = end_mark;
    sk->packet_tail = (sk->packet_tail + 1) % UNIX_PACKET_QUEUE_MAX;
    return 0;
}

static int unix_packet_has_space_locked(struct unix_sock *sk)
{
    return sk->type != SOCK_SEQPACKET ||
        (unsigned long)unix_packet_count_locked(sk) < UNIX_PACKET_QUEUE_MAX - 1;
}

static int ring_alloc(struct unix_ring *r)
{
    r->data = kvmalloc(UNIX_BUF_DEFAULT_SIZE);
    if (r->data == NULL)
        return -ENOMEM;
    r->capacity = UNIX_BUF_DEFAULT_SIZE;
    r->nread = 0;
    r->nwrite = 0;
    return 0;
}

static void ring_free(struct unix_ring *r)
{
    if (r->data != NULL) {
        kvfree(r->data);
        r->data = NULL;
    }
    r->capacity = 0;
}

static int ring_grow_locked(struct unix_ring *r, size_t need_writable,
                            size_t max_capacity)
{
    size_t readable = RING_READABLE(r);
    size_t writable = RING_WRITABLE_LIMIT(r, max_capacity);
    if (writable >= need_writable)
        return 0;
    if (r->capacity >= max_capacity)
        return -ENOBUFS;

    size_t need_capacity = readable + need_writable;
    size_t new_capacity = r->capacity;
    while (new_capacity < need_capacity &&
           new_capacity < max_capacity)
        new_capacity *= 2;
    if (new_capacity < need_capacity)
        new_capacity = max_capacity;
    if (new_capacity < need_capacity)
        return -ENOBUFS;

    char *new_data = kvmalloc(new_capacity);
    if (new_data == NULL)
        return -ENOMEM;


    size_t old_capacity = r->capacity;
    size_t old_idx = r->nread % old_capacity;
    size_t new_idx = r->nread % new_capacity;
    size_t first = readable;
    if (first > old_capacity - old_idx)
        first = old_capacity - old_idx;

    if (first > 0) {
        if (new_idx + first <= new_capacity) {
            memmove(new_data + new_idx, r->data + old_idx, first);
        } else {
            size_t new_first = new_capacity - new_idx;
            memmove(new_data + new_idx, r->data + old_idx, new_first);
            memmove(new_data, r->data + old_idx + new_first,
                    first - new_first);
        }
    }

    size_t remaining = readable - first;
    if (remaining > 0) {
        size_t new_off = (new_idx + first) % new_capacity;
        if (new_off + remaining <= new_capacity) {
            memmove(new_data + new_off, r->data, remaining);
        } else {
            size_t new_first = new_capacity - new_off;
            memmove(new_data + new_off, r->data, new_first);
            memmove(new_data, r->data + new_first, remaining - new_first);
        }
    }

    kvfree(r->data);
    r->data = new_data;
    r->capacity = new_capacity;
    return 0;
}

/*
 * ring_write - copy data into a ring buffer (kernel-side)
 * Returns bytes written.
 */
static size_t ring_write(struct unix_ring *r, const char *buf, size_t len,
                         size_t max_capacity)
{
    size_t readable = RING_READABLE(r);
    size_t avail = RING_WRITABLE_LIMIT(r, max_capacity);
    size_t ring_avail = r->capacity > readable ? r->capacity - readable : 0;
    if (avail > ring_avail)
        avail = ring_avail;
    if (avail < len) {
        if (ring_grow_locked(r, len, max_capacity) == 0)
            avail = RING_WRITABLE_LIMIT(r, max_capacity);
        readable = RING_READABLE(r);
        ring_avail = r->capacity > readable ? r->capacity - readable : 0;
        if (avail > ring_avail)
            avail = ring_avail;
    }
    if (avail == 0)
        return 0;
    size_t towrite = len < avail ? len : avail;
    uint idx = r->nwrite % r->capacity;
    if (idx + towrite <= r->capacity) {
        memmove(&r->data[idx], buf, towrite);
    } else {
        size_t first = r->capacity - idx;
        memmove(&r->data[idx], buf, first);
        memmove(&r->data[0], buf + first, towrite - first);
    }
    smp_store_release(&r->nwrite, r->nwrite + towrite);
    return towrite;
}

static size_t unix_tx_limit_locked(struct unix_sock *sk)
{
    size_t limit = sk->snd_buf ? sk->snd_buf : UNIX_BUF_MAX_SIZE;
    struct unix_sock *peer = sk->peer;

    if (peer != NULL && peer->rcv_buf != 0 && peer->rcv_buf < limit)
        limit = peer->rcv_buf;
    return limit;
}

/*
 * ring_read - copy data out of a ring buffer (kernel-side)
 * Returns bytes read.
 */
static size_t ring_read(struct unix_ring *r, char *buf, size_t len)
{
    size_t avail = RING_READABLE(r);
    if (avail == 0)
        return 0;
    size_t toread = len < avail ? len : avail;
    uint idx = r->nread % r->capacity;
    if (idx + toread <= r->capacity) {
        memmove(buf, &r->data[idx], toread);
    } else {
        size_t first = r->capacity - idx;
        memmove(buf, &r->data[idx], first);
        memmove(buf + first, &r->data[0], toread - first);
    }
    smp_store_release(&r->nread, r->nread + toread);
    return toread;
}

/* ring_peek removed — unused for now */

/* ========================================================================== */
/* Allocation / deallocation                                                  */
/* ========================================================================== */

static struct unix_sock *unix_sock_alloc(void)
{
    struct unix_sock *sk = slab_alloc(&__unix_sock_cache);
    if (sk == NULL)
        return NULL;
    memset(sk, 0, sizeof(*sk));
    sk->scm_queue = kvmalloc(sizeof(*sk->scm_queue) * UNIX_SCM_QUEUE_MAX);
    if (sk->scm_queue == NULL) {
        slab_free(sk);
        return NULL;
    }
    memset(sk->scm_queue, 0, sizeof(*sk->scm_queue) * UNIX_SCM_QUEUE_MAX);
    sk->packet_queue = kvmalloc(sizeof(*sk->packet_queue) * UNIX_PACKET_QUEUE_MAX);
    if (sk->packet_queue == NULL) {
        kvfree(sk->scm_queue);
        sk->scm_queue = NULL;
        slab_free(sk);
        return NULL;
    }
    memset(sk->packet_queue, 0, sizeof(*sk->packet_queue) * UNIX_PACKET_QUEUE_MAX);
    spin_init(&sk->lock, "unix_sock");
    sk->refcount = 1;
    tq_init(&sk->rd_queue, "unix_rd", NULL);
    tq_init(&sk->wr_queue, "unix_wr", NULL);
    tq_init(&sk->conn_queue, "unix_conn", NULL);
    sk->rcv_lowat = UNIX_LOWAT_DEFAULT;
    sk->snd_lowat = UNIX_LOWAT_DEFAULT;
    sk->rcv_buf = UNIX_BUF_MAX_SIZE;
    sk->snd_buf = UNIX_BUF_MAX_SIZE;
    sk->state = UNIX_STATE_UNCONNECTED;
    return sk;
}

static void unix_sock_free(struct unix_sock *sk)
{
    while (sk->scm_queue != NULL && sk->scm_head != sk->scm_tail) {
        struct vfs_file *pending = sk->scm_queue[sk->scm_head].file;
        sk->scm_queue[sk->scm_head].file = NULL;
        sk->scm_queue[sk->scm_head].start_nread = 0;
        sk->scm_queue[sk->scm_head].end_nread = 0;
        sk->scm_head = (sk->scm_head + 1) % UNIX_SCM_QUEUE_MAX;
        if (pending != NULL)
            vfs_fput(pending);
    }
    ring_free(&sk->tx);
    if (sk->scm_queue != NULL) {
        kvfree(sk->scm_queue);
        sk->scm_queue = NULL;
    }
    if (sk->packet_queue != NULL) {
        kvfree(sk->packet_queue);
        sk->packet_queue = NULL;
    }
    slab_free(sk);
}

/*
 * Reference counting helpers.
 * Each unix_sock starts with refcount=1 (for the owning vfs_file).
 * When A->peer = B, B gets +1 ref, and vice versa.
 */
static inline void unix_sock_get(struct unix_sock *sk)
{
    __atomic_fetch_add(&sk->refcount, 1, __ATOMIC_RELAXED);
}

static inline void unix_sock_put(struct unix_sock *sk)
{
    if (__atomic_sub_fetch(&sk->refcount, 1, __ATOMIC_ACQ_REL) == 0)
        unix_sock_free(sk);
}

void unix_sock_get_ref(struct unix_sock *sk)
{
    if (sk != NULL)
        unix_sock_get(sk);
}

void unix_sock_put_ref(struct unix_sock *sk)
{
    if (sk != NULL)
        unix_sock_put(sk);
}

static inline struct unix_sock *unix_sock_ref_peer_locked(struct unix_sock *sk)
{
    struct unix_sock *peer = sk->peer;
    if (peer != NULL)
        unix_sock_get(peer);
    return peer;
}

static void unix_sock_cancel_pending_connect(struct unix_sock *sk, int err)
{
    struct unix_sock *target;

    spin_lock(&sk->lock);
    target = sk->connect_target;
    if (target != NULL)
        unix_sock_get(target);
    spin_unlock(&sk->lock);

    if (target == NULL)
        return;

    bool removed = false;
    spin_lock(&target->lock);
    struct unix_pending **pp = &target->pending_head;
    struct unix_pending *prev = NULL;
    while (*pp != NULL) {
        struct unix_pending *p = *pp;
        if (p->sock == sk) {
            *pp = p->next;
            if (target->pending_tail == p)
                target->pending_tail = prev;
            target->pending_count--;
            slab_free(p);
            removed = true;
            break;
        }
        prev = p;
        pp = &(*pp)->next;
    }
    if (target->pending_head == NULL)
        target->pending_tail = NULL;
    spin_unlock(&target->lock);

    if (removed) {
        bool drop_target_ref = false;

        spin_lock(&sk->lock);
        if (sk->connect_target == target) {
            sk->connect_target = NULL;
            sk->so_error = err;
            if (sk->state == UNIX_STATE_CONNECTING)
                sk->state = UNIX_STATE_UNCONNECTED;
            drop_target_ref = true;
            tq_wakeup_all(&sk->conn_queue, -1, 0);
        }
        spin_unlock(&sk->lock);

        if (drop_target_ref)
            unix_sock_put(target);
        unix_sock_put(sk);  /* drop pending-queue reference */
    }

    unix_sock_put(target);  /* drop temporary reference */
}

/* ========================================================================== */
/* Bind registry                                                              */
/* ========================================================================== */

static size_t unix_addr_len(const struct sockaddr_un *sa, int addrlen)
{
    size_t max_len = (size_t)addrlen - sizeof(uint16);

    if (max_len > UNIX_PATH_MAX)
        max_len = UNIX_PATH_MAX;
    if (max_len == 0)
        return 0;
    if (sa->sun_path[0] == '\0')
        return max_len;
    return strnlen(sa->sun_path, max_len) + 1;
}

static struct unix_sock *bind_lookup(const char *path, size_t path_len)
{
    spin_lock(&bind_list_lock);
    struct unix_bind_entry *e = bind_list_head;
    while (e != NULL) {
        if (e->path_len == path_len &&
            memcmp(e->path, path, path_len) == 0) {
            struct unix_sock *sk = e->sock;
            unix_sock_get(sk);
            spin_unlock(&bind_list_lock);
            return sk;
        }
        e = e->next;
    }
    spin_unlock(&bind_list_lock);
    return NULL;
}

static int bind_register(const char *path, size_t path_len, struct unix_sock *sk)
{
    spin_lock(&bind_list_lock);

    /* Check for duplicate */
    struct unix_bind_entry *e = bind_list_head;
    while (e != NULL) {
        if (e->path_len == path_len &&
            memcmp(e->path, path, path_len) == 0) {
            spin_unlock(&bind_list_lock);
            return -EADDRINUSE;
        }
        e = e->next;
    }

    e = slab_alloc(&__unix_bind_cache);
    if (e == NULL) {
        spin_unlock(&bind_list_lock);
        return -ENOMEM;
    }
    memset(e->path, 0, sizeof(e->path));
    memmove(e->path, path, path_len);
    e->path_len = path_len;
    e->sock = sk;
    unix_sock_get(sk);
    e->next = bind_list_head;
    bind_list_head = e;
    spin_unlock(&bind_list_lock);
    return 0;
}

static void bind_unregister(const char *path, size_t path_len)
{
    spin_lock(&bind_list_lock);
    struct unix_bind_entry **pp = &bind_list_head;
    while (*pp != NULL) {
        if ((*pp)->path_len == path_len &&
            memcmp((*pp)->path, path, path_len) == 0) {
            struct unix_bind_entry *e = *pp;
            struct unix_sock *sk = e->sock;
            *pp = e->next;
            slab_free(e);
            spin_unlock(&bind_list_lock);
            unix_sock_put(sk);
            return;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&bind_list_lock);
}

/* ========================================================================== */
/* Initialization                                                             */
/* ========================================================================== */

void unix_socket_init(void)
{
    int ret;
    ret = slab_cache_init(&__unix_sock_cache, "unix_sock",
                         sizeof(struct unix_sock), SLAB_FLAG_STATIC);
    assert(ret == 0, "unix_sock slab init failed, errno=%d", ret);

    ret = slab_cache_init(&__unix_pending_cache, "unix_pend",
                         sizeof(struct unix_pending), SLAB_FLAG_STATIC);
    assert(ret == 0, "unix_pend slab init failed, errno=%d", ret);

    ret = slab_cache_init(&__unix_bind_cache, "unix_bind",
                         sizeof(struct unix_bind_entry), SLAB_FLAG_STATIC);
    assert(ret == 0, "unix_bind slab init failed, errno=%d", ret);

    spin_init(&bind_list_lock, "unix_bind_list");
}

/* ========================================================================== */
/* Socket fd allocation                                                       */
/* ========================================================================== */

static int unix_fd_alloc(struct unix_sock *sk, int file_flags)
{
    int fd = vfs_custom_fd_alloc(&unix_socket_file_ops, sk, file_flags);
    if (fd < 0)
        return fd;

    /* Back-reference for kqueue notifications */
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    sk->file = f;
    vfs_fput(f);

    return fd;
}

/* ========================================================================== */
/* Check if a vfs_file is an AF_UNIX socket                                   */
/* ========================================================================== */

int unix_sock_is_unix(struct vfs_file *f)
{
    return (f != NULL && f->ops == &unix_socket_file_ops);
}

struct unix_sock *unix_sock_from_fd(int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return NULL;

    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = current->fdtable->files[fd];
    spin_unlock(&current->fdtable->lock);

    if (f == NULL || f->ops != &unix_socket_file_ops)
        return NULL;

    return (struct unix_sock *)f->private_data;
}

/* ========================================================================== */
/* File operations implementation                                             */
/* ========================================================================== */

/*
 * unix_file_read - read data from a connected AF_UNIX socket
 *
 * For SOCK_STREAM: reads from peer->tx ring buffer (bytes the peer wrote).
 * Blocks if no data and not non-blocking.
 */
static ssize_t unix_file_read(struct vfs_file *file, char *buf, size_t count,
                              bool user)
{
    struct unix_sock *sk = (struct unix_sock *)file->private_data;
    if (sk == NULL)
        return -EBADF;

    if (sk->state != UNIX_STATE_CONNECTED &&
        unix_sock_connection_oriented(sk)) {
        if (sk->shutdown_flags & UNIX_SHUT_RD) {
            return 0;
        }
        return -ENOTCONN;
    }

    bool nonblock = (file->f_flags & O_NONBLOCK) != 0;
    ssize_t total = 0;
    char tmp[UNIX_IO_CHUNK];

    while ((size_t)total < count) {
        struct unix_sock *peer;
        size_t got = 0;

        spin_lock(&sk->lock);
        peer = unix_sock_ref_peer_locked(sk);
        if (peer == NULL || (sk->shutdown_flags & UNIX_SHUT_RD)) {
            spin_unlock(&sk->lock);
            /* EOF: peer disconnected or read-shut */
            break;
        }
        spin_unlock(&sk->lock);

        /* Read from peer's tx ring */
        spin_lock(&peer->lock);
        size_t want = count - (size_t)total;
        if (want > sizeof(tmp))
            want = sizeof(tmp);
        got = ring_read(&peer->tx, tmp, want);
        if (got > 0)
            tq_wakeup_all(&peer->wr_queue, 0, 0);
        struct vfs_file *peer_rd_file =
            (got > 0 && peer->file) ? vfs_fdup(peer->file) : NULL;
        spin_unlock(&peer->lock);

        if (peer_rd_file) {
            vfs_file_knote_notify(peer_rd_file, EVFILT_WRITE, 0);
            vfs_fput(peer_rd_file);
        }

        if (got == 0) {
            /* No data available */
            if (total > 0) {
                unix_sock_put(peer);
                break; /* short read */
            }

            /* Check if peer is gone (broken pipe = EOF) */
            bool eof;
            spin_lock(&sk->lock);
            eof = sk->peer == NULL;
            spin_unlock(&sk->lock);
            if (!eof) {
                spin_lock(&peer->lock);
                eof = (peer->shutdown_flags & UNIX_SHUT_WR) != 0;
                spin_unlock(&peer->lock);
            }
            if (eof) {
                unix_sock_put(peer);
                break; /* EOF */
            }

            if (nonblock) {
                unix_sock_put(peer);
                ssize_t ret = total > 0 ? total : -EAGAIN;
                return ret;
            }

            if (signal_pending(current)) {
                unix_sock_put(peer);
                ssize_t ret = total > 0 ? total : -EINTR;
                return ret;
            }

            /* Sleep waiting for data */
            unix_sock_put(peer);
            spin_lock(&sk->lock);
            tq_wait_in_state(&sk->rd_queue, &sk->lock, NULL,
                             THREAD_INTERRUPTIBLE);
            spin_unlock(&sk->lock);

            if (signal_pending(current)) {
                ssize_t ret = total > 0 ? total : -EINTR;
                return ret;
            }
            continue;
        }

        /* Copy to user/kernel buffer */
        if (user) {
            if (vm_copyout(current->vm, (uint64)(buf + total), tmp, got) < 0) {
                unix_sock_put(peer);
                break;
            }
        } else {
            memmove(buf + total, tmp, got);
        }
        total += (ssize_t)got;
        unix_sock_put(peer);
    }

    return total;
}

/*
 * unix_file_write - write data to a connected AF_UNIX socket
 *
 * Writes to our own tx ring buffer (which the peer reads from).
 */
static ssize_t unix_file_write(struct vfs_file *file, const char *buf,
                               size_t count, bool user)
{
    struct unix_sock *sk = (struct unix_sock *)file->private_data;
    if (sk == NULL)
        return -EBADF;

    if (sk->shutdown_flags & UNIX_SHUT_WR)
        return -EPIPE;

    if (sk->state != UNIX_STATE_CONNECTED &&
        unix_sock_connection_oriented(sk))
        return -ENOTCONN;

    bool nonblock = (file->f_flags & O_NONBLOCK) != 0;
    ssize_t total = 0;
    char tmp[UNIX_IO_CHUNK];

    for (;;) {
        spin_lock(&sk->lock);
        if (unix_packet_has_space_locked(sk)) {
            spin_unlock(&sk->lock);
            break;
        }
        if (nonblock) {
            spin_unlock(&sk->lock);
            return -EAGAIN;
        }
        if (signal_pending(current)) {
            spin_unlock(&sk->lock);
            return -EINTR;
        }
        tq_wait_in_state(&sk->wr_queue, &sk->lock, NULL,
                         THREAD_INTERRUPTIBLE);
        spin_unlock(&sk->lock);
        if (signal_pending(current))
            return -EINTR;
    }

    while ((size_t)total < count) {
        /* Copy from user */
        size_t chunk = count - (size_t)total;
        if (chunk > sizeof(tmp))
            chunk = sizeof(tmp);

        if (user) {
            if (vm_copyin(current->vm, tmp, (uint64)(buf + total), chunk) < 0)
                return total > 0 ? total : -EFAULT;
        } else {
            memmove(tmp, buf + total, chunk);
        }

        /* Write to our tx ring */
        size_t wrote = 0;
        while (wrote < chunk) {
            spin_lock(&sk->lock);
            struct unix_sock *peer = sk->peer;
            if (peer == NULL) {
                spin_unlock(&sk->lock);
                return total > 0 ? total : -EPIPE;
            }
            unix_sock_get(peer);
            size_t tx_limit = unix_tx_limit_locked(sk);
            size_t w = ring_write(&sk->tx, tmp + wrote, chunk - wrote,
                                  tx_limit);
            spin_unlock(&sk->lock);

            if (w > 0) {
                wrote += w;
                spin_lock(&peer->lock);
                tq_wakeup_all(&peer->rd_queue, 0, 0);
                struct vfs_file *pf =
                    peer->file ? vfs_fdup(peer->file) : NULL;
                spin_unlock(&peer->lock);
                if (pf) {
                    vfs_file_knote_notify(pf, EVFILT_READ, 0);
                    vfs_fput(pf);
                }
            }
            unix_sock_put(peer);

            if (wrote < chunk) {
                /* Ring full, need to wait for peer to read */
                if (nonblock) {
                    total += (ssize_t)wrote;
                    spin_lock(&sk->lock);
                    spin_unlock(&sk->lock);
                    return total > 0 ? total : -EAGAIN;
                }
                if (signal_pending(current)) {
                    total += (ssize_t)wrote;
                    return total > 0 ? total : -EINTR;
                }

                /* Sleep waiting for space */
                spin_lock(&sk->lock);
                tq_wait_in_state(&sk->wr_queue, &sk->lock, NULL,
                                 THREAD_INTERRUPTIBLE);
                spin_unlock(&sk->lock);

                if (signal_pending(current)) {
                    total += (ssize_t)wrote;
                    return total > 0 ? total : -EINTR;
                }
            }
        }
        total += (ssize_t)chunk;
    }

    struct unix_sock *packet_peer = NULL;
    struct vfs_file *packet_peer_file = NULL;
    spin_lock(&sk->lock);
    int pkt_ret = unix_packet_enqueue_locked(sk, sk->tx.nwrite);
    if (pkt_ret == 0 && sk->type == SOCK_SEQPACKET && sk->peer != NULL) {
        packet_peer = sk->peer;
        unix_sock_get(packet_peer);
    }
    spin_unlock(&sk->lock);
    if (pkt_ret < 0)
        return pkt_ret;

    if (packet_peer != NULL) {
        spin_lock(&packet_peer->lock);
        tq_wakeup_all(&packet_peer->rd_queue, 0, 0);
        if (packet_peer->file != NULL)
            packet_peer_file = vfs_fdup(packet_peer->file);
        spin_unlock(&packet_peer->lock);
        if (packet_peer_file != NULL) {
            vfs_file_knote_notify(packet_peer_file, EVFILT_READ, 0);
            vfs_fput(packet_peer_file);
        }
        unix_sock_put(packet_peer);
    }

    return total;
}

/*
 * unix_file_release - close an AF_UNIX socket
 */
static int unix_file_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    struct unix_sock *sk = (struct unix_sock *)file->private_data;
    if (sk == NULL)
        return 0;

    unix_sock_cancel_pending_connect(sk, ECONNRESET);

    spin_lock(&sk->lock);

    /*
     * Detach peer link.  Take a temporary ref on peer so it cannot be
     * freed between our unlock and the peer-notification lock below.
     * This avoids the AB-BA deadlock that would occur if we tried to
     * hold both sk->lock and peer->lock simultaneously.
     */
    struct unix_sock *peer = sk->peer;
    if (peer != NULL) {
        unix_sock_get(peer);  /* temp ref — keeps peer alive */
        sk->peer = NULL;
    }

    /* Clean up pending connections (if listening) */
    struct unix_pending *p = sk->pending_head;
    while (p != NULL) {
        struct unix_pending *next = p->next;
        bool drop_target_ref = false;

        /* Wake the connecting socket so it gets ECONNREFUSED */
        spin_lock(&p->sock->lock);
        if (p->sock->connect_target == sk) {
            p->sock->connect_target = NULL;
            drop_target_ref = true;
        }
        p->sock->so_error = ECONNREFUSED;
        p->sock->state = UNIX_STATE_UNCONNECTED;
        tq_wakeup_all(&p->sock->conn_queue, -1, 0);
        spin_unlock(&p->sock->lock);
        if (drop_target_ref)
            unix_sock_put(sk);
        unix_sock_put(p->sock);
        slab_free(p);
        p = next;
    }
    sk->pending_head = NULL;
    sk->pending_tail = NULL;

    /* Unbind if bound */
    if (sk->bound)
        bind_unregister(sk->bind_path, sk->bind_len);

    sk->file = NULL;

    spin_unlock(&sk->lock);

    /*
     * Notify peer (holding only peer->lock, never both locks).
     * If peer->peer still points to us, handle the disconnect.
     *
     * POSIX requires that close() on one end of a socketpair does NOT
     * discard data already buffered in the tx ring — the peer must be
     * able to read(2) it before seeing EOF.  If our tx ring still has
     * unread data, we keep peer->peer pointing to us (and its reference
     * on us) so the reader can drain the buffer.  We mark ourselves
     * with UNIX_SHUT_WR so the reader knows no more data will arrive
     * and returns EOF once the ring is empty.
     */
    if (peer != NULL) {
        bool cleared_backptr = false;

        /* Check if our tx ring has data the peer hasn't read yet.
         * We can safely inspect sk->tx under sk->lock alone (our own ring). */
        spin_lock(&sk->lock);
        bool has_data = (sk->tx.nwrite != sk->tx.nread);
        bool has_scm = (sk->scm_head != sk->scm_tail);
        sk->shutdown_flags |= UNIX_SHUT_WR;  /* no more writes from us */
        spin_unlock(&sk->lock);

        spin_lock(&peer->lock);
        if (peer->peer == sk) {
            if (has_data || has_scm) {
                /* Data remains in our tx ring.  Keep peer->peer alive
                 * so the reader can find our tx ring and SCM_RIGHTS queue.
                 * Do NOT set UNIX_SHUT_RD on peer — the read loop checks that
                 * flag and would skip reading the ring.  The reader will
                 * detect EOF via our UNIX_SHUT_WR flag once the ring is empty.
                 */
                /* cleared_backptr stays false → peer keeps its ref on us */
            } else {
                /* No buffered data — clean disconnect. */
                peer->peer = NULL;
                peer->shutdown_flags |= UNIX_SHUT_RD; /* peer sees EOF */
                cleared_backptr = true;
            }
        }
        /* Wake peer's readers and writers */
        tq_wakeup_all(&peer->rd_queue, -1, 0);
        tq_wakeup_all(&peer->wr_queue, -1, 0);
        struct vfs_file *peer_rel_file =
            peer->file ? vfs_fdup(peer->file) : NULL;
        spin_unlock(&peer->lock);
        if (peer_rel_file) {
            vfs_file_knote_notify(peer_rel_file, EVFILT_READ, 0);
            vfs_file_knote_notify(peer_rel_file, EVFILT_WRITE, 0);
            vfs_fput(peer_rel_file);
        }

        /* Drop ref for sk->peer link we just cleared */
        unix_sock_put(peer);
        /* Drop temp ref */
        unix_sock_put(peer);
        /* If we cleared the back-pointer, drop the ref peer had on us */
        if (cleared_backptr)
            unix_sock_put(sk);
    }

    /* Wake any remaining waiters on our side */
    spin_lock(&sk->lock);
    tq_wakeup_all(&sk->rd_queue, -1, 0);
    tq_wakeup_all(&sk->wr_queue, -1, 0);
    tq_wakeup_all(&sk->conn_queue, -1, 0);
    spin_unlock(&sk->lock);

    file->private_data = NULL;
    unix_sock_put(sk);  /* drop file ref */
    return 0;
}

/*
 * unix_file_poll - check socket readiness for I/O
 */
static int unix_file_poll(struct vfs_file *file, short events)
{
    struct unix_sock *sk = (struct unix_sock *)file->private_data;
    if (sk == NULL)
        return POLLNVAL;

    short revents = 0;
    struct unix_sock *peer = NULL;
    int state;
    int type;
    int shutdown_flags;
    int so_error;
    size_t rcv_lowat;
    size_t snd_lowat;
    size_t tx_writable = 0;
    int packet_writable;

    spin_lock(&sk->lock);

    /* Listening socket: readable if pending connections */
    if (sk->state == UNIX_STATE_LISTENING) {
        if ((events & (POLLIN | POLLRDNORM)) && sk->pending_count > 0)
            revents |= (events & (POLLIN | POLLRDNORM));
        spin_unlock(&sk->lock);
        return revents;
    }

    state = sk->state;
    type = sk->type;
    shutdown_flags = sk->shutdown_flags;
    so_error = sk->so_error;
    rcv_lowat = sk->rcv_lowat ? sk->rcv_lowat : UNIX_LOWAT_DEFAULT;
    snd_lowat = sk->snd_lowat ? sk->snd_lowat : UNIX_LOWAT_DEFAULT;
    peer = sk->peer;
    if (peer != NULL)
        unix_sock_get(peer);
    tx_writable = unix_tx_limit_locked(sk);
    if (tx_writable > (unsigned long)RING_READABLE(&sk->tx))
        tx_writable -= (unsigned long)RING_READABLE(&sk->tx);
    else
        tx_writable = 0;
    packet_writable = unix_packet_has_space_locked(sk);
    spin_unlock(&sk->lock);

    /* Check readability: data in peer's tx ring (which we read), or EOF. */
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) {
        if (peer != NULL) {
            if (type == SOCK_SEQPACKET) {
                spin_lock(&peer->lock);
                bool packet_ready = unix_packet_count_locked(peer) > 0;
                bool peer_wr_shutdown =
                    (peer->shutdown_flags & UNIX_SHUT_WR) != 0;
                spin_unlock(&peer->lock);
                if (packet_ready || peer_wr_shutdown)
                    revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
            } else {
                spin_lock(&peer->lock);
                bool scm_ready = false;
                if (peer->scm_head != peer->scm_tail) {
                    uint start = peer->scm_queue[peer->scm_head].start_nread;
                    uint end = peer->scm_queue[peer->scm_head].end_nread;
                    uint nread = peer->tx.nread;
                    scm_ready =
                        (start == end && (int)(nread - start) >= 0) ||
                        (start != end && (int)(nread - end) >= 0);
                }
                bool peer_wr_shutdown =
                    (peer->shutdown_flags & UNIX_SHUT_WR) != 0;
                bool stream_ready =
                    RING_READABLE(&peer->tx) >= rcv_lowat || scm_ready ||
                    peer_wr_shutdown;
                spin_unlock(&peer->lock);
                if (stream_ready)
                    revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
            }
        }
        else if (peer == NULL || (sk->shutdown_flags & UNIX_SHUT_RD)) {
            if (state != UNIX_STATE_CONNECTING)
                revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND)); /* EOF */
        }
    }

    /* Check writability: space in our tx ring */
    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
        if (state == UNIX_STATE_CONNECTING) {
            if (peer != NULL)
                revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
        } else if (peer == NULL) {
            revents |= POLLERR;
        } else if (packet_writable && tx_writable >= snd_lowat) {
            revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
        }
    }

    /* Error/hangup conditions */
    if (so_error)
        revents |= POLLERR;
    if (peer == NULL && state == UNIX_STATE_CONNECTED) {
        revents |= POLLHUP;
        if (events & POLLRDHUP)
            revents |= POLLRDHUP;
    }
    if (peer != NULL) {
        spin_lock(&peer->lock);
        if ((events & POLLRDHUP) &&
            (peer->shutdown_flags & UNIX_SHUT_WR))
            revents |= POLLRDHUP;
        spin_unlock(&peer->lock);
    }
    if (shutdown_flags & UNIX_SHUT_WR)
        revents |= POLLHUP;

    if (peer != NULL)
        unix_sock_put(peer);
    return revents;
}

/*
 * unix_file_ioctl - ioctl on AF_UNIX socket
 */
static int unix_file_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct unix_sock *sk = (struct unix_sock *)file->private_data;
    if (sk == NULL)
        return -EBADF;

    switch (cmd) {
    case 0x541B: { /* FIONREAD */
        int count = 0;
        spin_lock(&sk->lock);
        struct unix_sock *peer = unix_sock_ref_peer_locked(sk);
        spin_unlock(&sk->lock);
        if (peer != NULL) {
            spin_lock(&peer->lock);
            count = (int)RING_READABLE(&peer->tx);
            spin_unlock(&peer->lock);
            unix_sock_put(peer);
        }
        if (vm_copyout(current->vm, (uint64)arg, &count, sizeof(count)) < 0)
            return -EFAULT;
        return 0;
    }
    case 0x5421: { /* FIONBIO */
        int val;
        if (vm_copyin(current->vm, &val, (uint64)arg, sizeof(val)) < 0)
            return -EFAULT;
        if (val)
            file->f_flags |= O_NONBLOCK;
        else
            file->f_flags &= ~O_NONBLOCK;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/* ========================================================================== */
/* Syscall-level operations                                                   */
/* ========================================================================== */

/*
 * unix_sock_create - create a new AF_UNIX socket
 */
int unix_sock_create(int type, int protocol, int file_flags)
{
    if (type != SOCK_STREAM && type != SOCK_SEQPACKET && type != SOCK_DGRAM)
        return -EPROTOTYPE;

    if (protocol != 0)
        return -EPROTONOSUPPORT;

    struct unix_sock *sk = unix_sock_alloc();
    if (sk == NULL)
        return -ENOMEM;

    sk->type = type;
    sk->protocol = protocol;

    /* Allocate tx ring buffer */
    int ret = ring_alloc(&sk->tx);
    if (ret < 0) {
        unix_sock_free(sk);
        return ret;
    }

    int fd = unix_fd_alloc(sk, file_flags);
    if (fd < 0) {
        unix_sock_free(sk);
        return fd;
    }

    return fd;
}

/*
 * unix_sock_bind - bind a UNIX socket to a path
 */
int unix_sock_bind(int fd, uint64 uaddr, int addrlen)
{
    struct unix_sock *sk = unix_sock_from_fd(fd);
    if (sk == NULL)
        return -EBADF;

    if (addrlen < (int)(sizeof(uint16) + 1) ||
        addrlen > (int)sizeof(struct sockaddr_un))
        return -EINVAL;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    if (vm_copyin(current->vm, &sa, uaddr, addrlen) < 0)
        return -EFAULT;

    if (sa.sun_family != AF_UNIX)
        return -EAFNOSUPPORT;

    size_t path_len = unix_addr_len(&sa, addrlen);
    if (path_len == 0)
        return -EINVAL;

    /* Keep pathname sockets null-terminated for callers that inspect them. */
    sa.sun_path[UNIX_PATH_MAX - 1] = '\0';

    spin_lock(&sk->lock);
    if (sk->bound) {
        spin_unlock(&sk->lock);
        return -EINVAL;
    }

    int ret = bind_register(sa.sun_path, path_len, sk);
    if (ret < 0) {
        spin_unlock(&sk->lock);
        return ret;
    }

    memset(sk->bind_path, 0, sizeof(sk->bind_path));
    memmove(sk->bind_path, sa.sun_path, path_len);
    sk->bind_len = path_len;
    sk->bound = 1;
    if (sk->state == UNIX_STATE_UNCONNECTED)
        sk->state = UNIX_STATE_BOUND;
    spin_unlock(&sk->lock);
    return 0;
}

/*
 * unix_sock_listen - mark a bound UNIX socket as listening
 */
int unix_sock_listen(int fd, int backlog)
{
    struct unix_sock *sk = unix_sock_from_fd(fd);
    if (sk == NULL)
        return -EBADF;

    if (sk->type != SOCK_STREAM && sk->type != SOCK_SEQPACKET)
        return -EOPNOTSUPP;

    spin_lock(&sk->lock);
    if (!sk->bound) {
        spin_unlock(&sk->lock);
        return -EINVAL;
    }

    sk->state = UNIX_STATE_LISTENING;
    sk->backlog = backlog > 0 ? (backlog < UNIX_BACKLOG_MAX ? backlog
                                                             : UNIX_BACKLOG_MAX)
                              : 1;
    spin_unlock(&sk->lock);
    return 0;
}

/*
 * unix_sock_accept - accept a connection on a listening UNIX socket
 */
int unix_sock_accept(int fd, uint64 uaddr, uint64 uaddrlen, int flags)
{
    struct unix_sock *sk = unix_sock_from_fd(fd);
    if (sk == NULL)
        return -EBADF;
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;


    spin_lock(&sk->lock);
    if (sk->state != UNIX_STATE_LISTENING) {
        spin_unlock(&sk->lock);
        return -EINVAL;
    }

    /* Try to dequeue a pending connection; block if none */
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    bool nonblock = (f != NULL && (f->f_flags & O_NONBLOCK)) || (flags & 0x800);
    if (f)
        vfs_fput(f);

    while (sk->pending_head == NULL) {
        if (nonblock) {
            spin_unlock(&sk->lock);
            return -EAGAIN;
        }
        tq_wait_in_state(&sk->conn_queue, &sk->lock, NULL,
                         THREAD_INTERRUPTIBLE);
        spin_unlock(&sk->lock);

        if (signal_pending(current)) {
            return -EINTR;
        }

        spin_lock(&sk->lock);
        if (sk->state != UNIX_STATE_LISTENING) {
            spin_unlock(&sk->lock);
            return -EINVAL;
        }
    }


    /* Dequeue first pending connection */
    struct unix_pending *pen = sk->pending_head;
    sk->pending_head = pen->next;
    if (sk->pending_head == NULL)
        sk->pending_tail = NULL;
    sk->pending_count--;
    struct unix_sock *client = pen->sock;
    spin_unlock(&sk->lock);
    slab_free(pen);

    bool drop_target_ref = false;
    spin_lock(&client->lock);
    if (client->connect_target == sk) {
        client->connect_target = NULL;
        drop_target_ref = true;
    }
    spin_unlock(&client->lock);
    if (drop_target_ref)
        unix_sock_put(sk);

    /* Create a new server-side socket to pair with the client */
    struct unix_sock *server = unix_sock_alloc();
    if (server == NULL) {
        /* Reject the connection */
        spin_lock(&client->lock);
        client->so_error = ECONNREFUSED;
        client->state = UNIX_STATE_UNCONNECTED;
        tq_wakeup_all(&client->conn_queue, -1, 0);
        spin_unlock(&client->lock);
        unix_sock_put(client);
        return -ENOMEM;
    }
    server->type = sk->type;
    server->protocol = 0;
    if (ring_alloc(&server->tx) < 0) {
        unix_sock_free(server);
        spin_lock(&client->lock);
        client->so_error = ECONNREFUSED;
        client->state = UNIX_STATE_UNCONNECTED;
        tq_wakeup_all(&client->conn_queue, -1, 0);
        spin_unlock(&client->lock);
        unix_sock_put(client);
        return -ENOMEM;
    }

    /* Determine file flags for new socket */
    int file_flags = O_RDWR;
    if (flags & 0x800) /* SOCK_NONBLOCK */
        file_flags |= O_NONBLOCK;

    int newfd = unix_fd_alloc(server, file_flags);
    if (newfd < 0) {
        unix_sock_free(server);
        spin_lock(&client->lock);
        client->so_error = ECONNREFUSED;
        client->state = UNIX_STATE_UNCONNECTED;
        tq_wakeup_all(&client->conn_queue, -1, 0);
        spin_unlock(&client->lock);
        unix_sock_put(client);
        return newfd;
    }
    if (flags & SOCK_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        vfs_fdtable_set_fdflags(current->fdtable, newfd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

    /* Cross-link client ↔ server (each takes a ref on the other) */
    spin_lock(&client->lock);
    spin_lock(&server->lock);
    client->peer = server;
    unix_sock_get(server);  /* client holds ref on server */
    server->peer = client;
    unix_sock_get(client);  /* server holds ref on client */
    /* Copy credentials: server sees client's creds, client sees server's */
    server->peer_pid = client->peer_pid;
    server->peer_uid = client->peer_uid;
    server->peer_gid = client->peer_gid;
    client->peer_pid = current->tgid;
    client->peer_uid = current->thread_group->uid;
    client->peer_gid = current->thread_group->gid;
    client->state = UNIX_STATE_CONNECTED;
    server->state = UNIX_STATE_CONNECTED;
    /* Wake the connecting client */
    tq_wakeup_all(&client->conn_queue, 0, 0);
    struct vfs_file *client_file =
        client->file ? vfs_fdup(client->file) : NULL;
    spin_unlock(&server->lock);
    spin_unlock(&client->lock);

    if (client_file) {
        vfs_file_knote_notify(client_file, EVFILT_WRITE, 0);
        vfs_file_knote_notify(client_file, EVFILT_READ, 0);
        vfs_fput(client_file);
    }

    /* Copy peer address back if requested */
    if (uaddr != 0 && uaddrlen != 0) {
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        size_t bind_len = 0;
        if (client->bound) {
            bind_len = client->bind_len;
            memmove(sa.sun_path, client->bind_path, bind_len);
        }

        int alen = bind_len ? (int)(sizeof(uint16) + bind_len) :
                              (int)sizeof(uint16);
        vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen));
        vm_copyout(current->vm, uaddr, &sa, alen);
    }

    unix_sock_put(client);  /* drop pending-queue reference */
    return newfd;
}

/*
 * unix_sock_connect - connect to a listening UNIX socket
 */
int unix_sock_connect(int fd, uint64 uaddr, int addrlen)
{
    struct unix_sock *sk = unix_sock_from_fd(fd);
    if (sk == NULL)
        return -EBADF;

    if (addrlen < (int)(sizeof(uint16) + 1) ||
        addrlen > (int)sizeof(struct sockaddr_un))
        return -EINVAL;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    if (vm_copyin(current->vm, &sa, uaddr, addrlen) < 0)
        return -EFAULT;

    if (sa.sun_family != AF_UNIX)
        return -EAFNOSUPPORT;

    size_t path_len = unix_addr_len(&sa, addrlen);
    if (path_len == 0)
        return -EINVAL;
    sa.sun_path[UNIX_PATH_MAX - 1] = '\0';

    spin_lock(&sk->lock);
    if (sk->state == UNIX_STATE_CONNECTED) {
        spin_unlock(&sk->lock);
        return -EISCONN;
    }
    if (sk->state == UNIX_STATE_CONNECTING) {
        spin_unlock(&sk->lock);
        return -EALREADY;
    }
    if (sk->state == UNIX_STATE_LISTENING) {
        spin_unlock(&sk->lock);
        return -EINVAL;
    }
    spin_unlock(&sk->lock);

    /* Find the target listening socket */
    struct unix_sock *target = bind_lookup(sa.sun_path, path_len);
    if (target == NULL) {
        return -ECONNREFUSED;
    }

    spin_lock(&target->lock);
    if (target->state != UNIX_STATE_LISTENING) {
        spin_unlock(&target->lock);
        unix_sock_put(target);
        return -ECONNREFUSED;
    }
    if (target->pending_count >= target->backlog) {
        spin_unlock(&target->lock);
        unix_sock_put(target);
        return -EAGAIN;
    }

    /* Enqueue this socket as a pending connection */
    struct unix_pending *pen = slab_alloc(&__unix_pending_cache);
    if (pen == NULL) {
        spin_unlock(&target->lock);
        unix_sock_put(target);
        return -ENOMEM;
    }

    spin_lock(&sk->lock);
    sk->peer_pid = current->tgid;
    sk->peer_uid = current->thread_group->uid;
    sk->peer_gid = current->thread_group->gid;
    sk->so_error = 0;
    sk->state = UNIX_STATE_CONNECTING;
    sk->connect_target = target;
    unix_sock_get(target);
    spin_unlock(&sk->lock);

    pen->sock = sk;
    pen->next = NULL;
    unix_sock_get(sk);  /* pending queue owns this reference */
    if (target->pending_tail != NULL)
        target->pending_tail->next = pen;
    else
        target->pending_head = pen;
    target->pending_tail = pen;
    target->pending_count++;

    /* Wake up accept() */
    tq_wakeup_all(&target->conn_queue, 0, 0);
    struct vfs_file *target_file =
        target->file ? vfs_fdup(target->file) : NULL;
    spin_unlock(&target->lock);
    if (target_file) {
        vfs_file_knote_notify(target_file, EVFILT_READ, 0);
        vfs_fput(target_file);
    }
    unix_sock_put(target);

    /* Check if non-blocking */
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    bool nonblock = (f != NULL && (f->f_flags & O_NONBLOCK));
    if (f)
        vfs_fput(f);

    if (nonblock) {
        return -EINPROGRESS;
    }

    /* Block until accepted or error */
    spin_lock(&sk->lock);
    while (sk->state != UNIX_STATE_CONNECTED) {
        tq_wait_in_state(&sk->conn_queue, &sk->lock, NULL,
                         THREAD_INTERRUPTIBLE);
        spin_unlock(&sk->lock);

        if (signal_pending(current)) {
            spin_lock(&sk->lock);
            bool connected = sk->state == UNIX_STATE_CONNECTED;
            spin_unlock(&sk->lock);
            if (connected)
                return 0;
            unix_sock_cancel_pending_connect(sk, ECONNRESET);
            return -EINTR;
        }

        spin_lock(&sk->lock);
        if (sk->so_error != 0) {
            int err = sk->so_error;
            sk->so_error = 0;
            spin_unlock(&sk->lock);
            return -err;
        }
    }
    spin_unlock(&sk->lock);

    return 0;
}

/*
 * unix_sock_shutdown - shut down part of a full-duplex connection
 */
int unix_sock_shutdown(int fd, int how)
{
    struct unix_sock *sk = unix_sock_from_fd(fd);
    if (sk == NULL)
        return -EBADF;

    spin_lock(&sk->lock);

    if (how == 0 || how == 2) /* SHUT_RD or SHUT_RDWR */
        sk->shutdown_flags |= UNIX_SHUT_RD;
    if (how == 1 || how == 2) /* SHUT_WR or SHUT_RDWR */
        sk->shutdown_flags |= UNIX_SHUT_WR;

    struct unix_sock *peer = unix_sock_ref_peer_locked(sk);
    spin_unlock(&sk->lock);

    if (peer != NULL) {
        spin_lock(&peer->lock);
        tq_wakeup_all(&peer->rd_queue, 0, 0);
        tq_wakeup_all(&peer->wr_queue, 0, 0);
        struct vfs_file *peer_sd_file =
            peer->file ? vfs_fdup(peer->file) : NULL;
        spin_unlock(&peer->lock);
        if (peer_sd_file) {
            vfs_file_knote_notify(peer_sd_file, EVFILT_READ, 0);
            vfs_file_knote_notify(peer_sd_file, EVFILT_WRITE, 0);
            vfs_fput(peer_sd_file);
        }
    }

    spin_lock(&sk->lock);
    tq_wakeup_all(&sk->rd_queue, 0, 0);
    tq_wakeup_all(&sk->wr_queue, 0, 0);
    spin_unlock(&sk->lock);
    if (peer != NULL)
        unix_sock_put(peer);

    return 0;
}

/*
 * unix_sock_getpeername - get address of connected peer
 */
int unix_sock_getpeername(int fd, uint64 uaddr, uint64 uaddrlen)
{
    struct unix_sock *sk = unix_sock_from_fd(fd);
    if (sk == NULL)
        return -EBADF;
    if (uaddr == 0 || uaddrlen == 0)
        return -EFAULT;

    spin_lock(&sk->lock);
    struct unix_sock *peer = sk->peer;
    if (peer == NULL) {
        spin_unlock(&sk->lock);
        return -ENOTCONN;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t bind_len = 0;
    if (peer->bound) {
        bind_len = peer->bind_len;
        memmove(sa.sun_path, peer->bind_path, bind_len);
    }
    spin_unlock(&sk->lock);

    int user_len = 0;
    if (vm_copyin(current->vm, &user_len, uaddrlen, sizeof(user_len)) < 0)
        return -EFAULT;
    if (user_len < 0)
        return -EINVAL;
    int alen = bind_len ? (int)(sizeof(uint16) + bind_len) :
                          (int)sizeof(uint16);
    int copy_len = user_len < alen ? user_len : alen;
    if (vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen)) < 0)
        return -EFAULT;
    if (copy_len > 0 && vm_copyout(current->vm, uaddr, &sa, copy_len) < 0)
        return -EFAULT;
    return 0;
}

/*
 * unix_sock_getsockname - get our own bound address
 */
int unix_sock_getsockname(int fd, uint64 uaddr, uint64 uaddrlen)
{
    struct unix_sock *sk = unix_sock_from_fd(fd);
    if (sk == NULL)
        return -EBADF;
    if (uaddr == 0 || uaddrlen == 0)
        return -EFAULT;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;

    spin_lock(&sk->lock);
    size_t bind_len = 0;
    if (sk->bound) {
        bind_len = sk->bind_len;
        memmove(sa.sun_path, sk->bind_path, bind_len);
    }
    spin_unlock(&sk->lock);

    int user_len = 0;
    if (vm_copyin(current->vm, &user_len, uaddrlen, sizeof(user_len)) < 0)
        return -EFAULT;
    if (user_len < 0)
        return -EINVAL;
    int alen = bind_len ? (int)(sizeof(uint16) + bind_len) :
                          (int)sizeof(uint16);
    int copy_len = user_len < alen ? user_len : alen;
    if (vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen)) < 0)
        return -EFAULT;
    if (copy_len > 0 && vm_copyout(current->vm, uaddr, &sa, copy_len) < 0)
        return -EFAULT;
    return 0;
}

/*
 * unix_sock_socketpair - create a connected pair of UNIX sockets
 */
int unix_sock_socketpair(int type, int protocol, int file_flags, int sv[2])
{
    if (type != SOCK_STREAM && type != SOCK_SEQPACKET && type != SOCK_DGRAM)
        return -EPROTOTYPE;

    if (protocol != 0)
        return -EPROTONOSUPPORT;

    /* Allocate two sockets */
    struct unix_sock *sk0 = unix_sock_alloc();
    if (sk0 == NULL)
        return -ENOMEM;
    struct unix_sock *sk1 = unix_sock_alloc();
    if (sk1 == NULL) {
        unix_sock_free(sk0);
        return -ENOMEM;
    }

    sk0->type = type;
    sk0->protocol = protocol;
    sk1->type = type;
    sk1->protocol = protocol;

    /* Allocate ring buffers */
    if (ring_alloc(&sk0->tx) < 0) {
        unix_sock_free(sk0);
        unix_sock_free(sk1);
        return -ENOMEM;
    }
    if (ring_alloc(&sk1->tx) < 0) {
        unix_sock_free(sk0);
        unix_sock_free(sk1);
        return -ENOMEM;
    }

    /* Allocate fds */
    int fd0 = unix_fd_alloc(sk0, file_flags);
    if (fd0 < 0) {
        unix_sock_free(sk0);
        unix_sock_free(sk1);
        return fd0;
    }
    int fd1 = unix_fd_alloc(sk1, file_flags);
    if (fd1 < 0) {
        /* Close fd0 properly */
        struct vfs_file *f0 = vfs_fdtable_get_file(current->fdtable, fd0);
        if (f0) {
            vfs_fput(f0);
            /* Let the release handler clean up sk0 */
        }
        unix_sock_free(sk1);
        return fd1;
    }

    /* Cross-link (each takes a ref on the other) */
    sk0->peer = sk1;
    unix_sock_get(sk1);  /* sk0 holds ref on sk1 */
    sk1->peer = sk0;
    unix_sock_get(sk0);  /* sk1 holds ref on sk0 */
    sk0->peer_pid = current->tgid;
    sk0->peer_uid = current->thread_group->uid;
    sk0->peer_gid = current->thread_group->gid;
    sk1->peer_pid = current->tgid;
    sk1->peer_uid = current->thread_group->uid;
    sk1->peer_gid = current->thread_group->gid;
    sk0->state = UNIX_STATE_CONNECTED;
    sk1->state = UNIX_STATE_CONNECTED;

    sv[0] = fd0;
    sv[1] = fd1;
    return 0;
}

/*
 * unix_sock_setsockopt / unix_sock_getsockopt - minimal option support
 */
int unix_sock_setsockopt(int fd, int level, int optname, uint64 optval,
                         int optlen)
{

    /* AF_UNIX sockets have very few options; return success for common ones */
    if (level == 1 /* SOL_SOCKET */) {
        switch (optname) {
        case 2:  /* SO_REUSEADDR - no-op */
        case 5:  /* SO_DONTROUTE - no-op for UNIX sockets */
        case 6:  /* SO_BROADCAST - no-op */
        case 9:  /* SO_KEEPALIVE - no-op for UNIX sockets */
        case 10: /* SO_OOBINLINE - no-op */
        case 13: /* SO_LINGER - no-op */
        case 15: /* SO_REUSEPORT - no-op */
        case 20: /* SO_RCVTIMEO_OLD */
        case 21: /* SO_SNDTIMEO_OLD */
            return 0;
        case 16: { /* SO_PASSCRED */
            int val;
            struct unix_sock *sk;

            if (optlen < (int)sizeof(val))
                return -EINVAL;
            if (vm_copyin(current->vm, &val, optval, sizeof(val)) < 0)
                return -EFAULT;
            sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            spin_lock(&sk->lock);
            sk->passcred = val != 0;
            spin_unlock(&sk->lock);
            return 0;
        }
        case 7:  /* SO_SNDBUF */
        case 8: { /* SO_RCVBUF */
            int val;
            struct unix_sock *sk;

            if (optlen < (int)sizeof(val))
                return -EINVAL;
            if (vm_copyin(current->vm, &val, optval, sizeof(val)) < 0)
                return -EFAULT;
            if (val < 1)
                return -EINVAL;
            if ((size_t)val > UNIX_BUF_MAX_SIZE)
                val = UNIX_BUF_MAX_SIZE;
            sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            spin_lock(&sk->lock);
            if (optname == 7)
                sk->snd_buf = (size_t)val;
            else
                sk->rcv_buf = (size_t)val;
            spin_unlock(&sk->lock);
            return 0;
        }
        case 18: /* SO_RCVLOWAT */
        case 19: { /* SO_SNDLOWAT */
            int val;
            struct unix_sock *sk;

            if (optlen < (int)sizeof(val))
                return -EINVAL;
            if (vm_copyin(current->vm, &val, optval, sizeof(val)) < 0)
                return -EFAULT;
            if (val < 1)
                return -EINVAL;
            sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            spin_lock(&sk->lock);
            if (optname == 18)
                sk->rcv_lowat = (size_t)val;
            else
                sk->snd_lowat = (size_t)val;
            spin_unlock(&sk->lock);
            return 0;
        }
        }
    }
    return -ENOPROTOOPT;
}

int unix_sock_getsockopt(int fd, int level, int optname, uint64 optval,
                         uint64 optlen_ptr)
{

    if (level == 1 /* SOL_SOCKET */) {
        int val = 0;
        switch (optname) {
        case 3: /* SO_TYPE */ {
            struct unix_sock *sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            val = sk->type;
            break;
        }
        case 4: { /* SO_ERROR */
            struct unix_sock *sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            spin_lock(&sk->lock);
            if (sk->so_error != 0) {
                val = sk->so_error;
                sk->so_error = 0;
            } else if (sk->state == UNIX_STATE_CONNECTING) {
                val = EINPROGRESS;
            } else if (unix_sock_connection_oriented(sk) &&
                       sk->state != UNIX_STATE_CONNECTED &&
                       sk->state != UNIX_STATE_BOUND &&
                       sk->state != UNIX_STATE_UNCONNECTED) {
                val = ENOTCONN;
            } else {
                val = 0;
            }
            spin_unlock(&sk->lock);
            break;
        }
        case 2:  /* SO_REUSEADDR */
        case 5:  /* SO_DONTROUTE */
        case 6:  /* SO_BROADCAST */
        case 9:  /* SO_KEEPALIVE */
        case 10: /* SO_OOBINLINE */
        case 13: /* SO_LINGER */
        case 15: /* SO_REUSEPORT */
            val = 0;
            break;
        case 16: { /* SO_PASSCRED */
            struct unix_sock *sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            spin_lock(&sk->lock);
            val = sk->passcred ? 1 : 0;
            spin_unlock(&sk->lock);
            break;
        }
        case 7:  /* SO_SNDBUF */
        case 8: { /* SO_RCVBUF */
            struct unix_sock *sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            spin_lock(&sk->lock);
            val = (int)(optname == 7 ? sk->snd_buf : sk->rcv_buf);
            spin_unlock(&sk->lock);
            break;
        }
        case 18: /* SO_RCVLOWAT */
        case 19: { /* SO_SNDLOWAT */
            struct unix_sock *sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            spin_lock(&sk->lock);
            val = (int)(optname == 18 ? sk->rcv_lowat : sk->snd_lowat);
            spin_unlock(&sk->lock);
            break;
        }
        case 20: /* SO_RCVTIMEO_OLD */
        case 21: /* SO_SNDTIMEO_OLD */
            val = 0;
            break;
        case 38: /* SO_PROTOCOL */
            val = 0;
            break;
        case 39: /* SO_DOMAIN */
            val = AF_UNIX;
            break;
        case 30: { /* SO_ACCEPTCONN */
            struct unix_sock *sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            val = (sk->state == UNIX_STATE_LISTENING) ? 1 : 0;
            break;
        }
        case 17: { /* SO_PEERCRED */
            struct unix_sock *sk = unix_sock_from_fd(fd);
            if (sk == NULL)
                return -EBADF;
            struct {
                int    pid;
                uint32 uid;
                uint32 gid;
            } ucred;
            spin_lock(&sk->lock);
            ucred.pid = sk->peer_pid;
            ucred.uid = sk->peer_uid;
            ucred.gid = sk->peer_gid;
            spin_unlock(&sk->lock);
            int clen = sizeof(ucred);
            vm_copyout(current->vm, optlen_ptr, &clen, sizeof(clen));
            vm_copyout(current->vm, optval, &ucred, sizeof(ucred));
            return 0;
        }
        default:
            return -ENOPROTOOPT;
        }

        int olen = sizeof(val);
        vm_copyout(current->vm, optlen_ptr, &olen, sizeof(olen));
        vm_copyout(current->vm, optval, &val, sizeof(val));
        return 0;
    }
    return -ENOPROTOOPT;
}
