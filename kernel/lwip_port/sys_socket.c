/**
 * @file sys_socket.c
 * @brief BSD socket syscall handlers backed by lwIP netconn API
 *
 * Provides kernel-side implementations of socket(), bind(), listen(),
 * accept(), connect(), sendto(), recvfrom(), setsockopt(), getsockopt(),
 * shutdown(), getpeername(), getsockname().
 *
 * Each socket is represented as a vfs_file with file_ops that delegate
 * read/write/close to lwIP's netconn layer. User programs (via newlib)
 * call standard POSIX socket functions which enter the kernel through
 * ecall → syscall dispatch → these handlers.
 *
 * The lwIP stack itself lives entirely in kernel space.
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "proc/thread.h"
#include "proc/tq.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include <mm/vm.h>
#include <vfs/vfs_types.h>
#include <vfs/file.h>
#include <vfs/fcntl.h>
#include <vfs/poll.h>
#include "accounting.h"

#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/netbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/igmp.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "arch/sys_arch.h"
#include "signal.h"
#include "kqueue.h"
#include "kqueue_types.h"
#include "vfs/unix_socket.h"
#include "netlink.h"

/* From irq/syscall.c — argument fetching */
extern void argint(int n, int *ip);
extern void argint64(int n, int64 *ip);
extern void argaddr(int n, uint64 *ip);
extern int argstr(int n, char *buf, int max);
extern void sleep_ms(uint64 ms);

/* ========================================================================== */
/* Debug socket tracing (set to 1 to enable)                                  */
/* ========================================================================== */
#define SOCK_DEBUG 0
#if SOCK_DEBUG
#define sock_dbg(fmt, ...) printf("[sock] " fmt, ##__VA_ARGS__)
#else
#define sock_dbg(fmt, ...) ((void)0)
#endif

/* ========================================================================== */
/* lwIP error → POSIX errno mapping                                           */
/* ========================================================================== */
static int lwip_err_to_errno(err_t err)
{
    switch (err) {
    case ERR_OK:         return 0;
    case ERR_MEM:        return ENOMEM;
    case ERR_BUF:        return ENOBUFS;
    case ERR_TIMEOUT:    return ETIMEDOUT;
    case ERR_RTE:        return EHOSTUNREACH;
    case ERR_INPROGRESS: return EINPROGRESS;
    case ERR_VAL:        return EINVAL;
    case ERR_WOULDBLOCK: return EAGAIN;
    case ERR_USE:        return EADDRINUSE;
    case ERR_ALREADY:    return EALREADY;
    case ERR_ISCONN:     return EISCONN;
    case ERR_CONN:       return ENOTCONN;
    case ERR_ABRT:       return ECONNABORTED;
    case ERR_RST:        return ECONNRESET;
    case ERR_CLSD:       return EPIPE;
    case ERR_ARG:        return EINVAL;
    case ERR_IF:         return EIO;
    default:             return EIO;
    }
}

/* ========================================================================== */
/* Socket structure: wraps a lwIP netconn                                     */
/* ========================================================================== */

struct lwip_sock {
    struct netconn *conn;        /* lwIP netconn handle */
    int            type;         /* SOCK_STREAM / SOCK_DGRAM / SOCK_RAW */
    int            protocol;     /* protocol number */
    struct netbuf  *lastbuf;     /* partially consumed recv buffer (UDP) */
    uint16         lastoffset;   /* offset into lastbuf */
    struct pbuf    *lastpbuf;    /* partially consumed TCP recv pbuf */
    uint16         lastpbuf_off; /* bytes already consumed from lastpbuf */
};

/* Socket type constants (matching POSIX) — also defined in vfs/unix_socket.h */
#ifndef SOCK_STREAM
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#endif
#ifndef SOCK_SEQPACKET
#define SOCK_SEQPACKET 5
#endif
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000
#define SOCK_TYPE_MASK 0xF

/* Address family constants */
#define AF_INET        2

/* Protocol constants */
#define IPPROTO_IP     0
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define IPPROTO_ICMP   1

/* Shutdown constants */
#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

/* Message flags */
#define MSG_PEEK       0x02
#define MSG_TRUNC      0x20
#define MSG_DONTWAIT      0x40
#define MSG_WAITALL       0x100
#define MSG_CMSG_CLOEXEC  0x40000000
#define MSG_NOSIGNAL      0x4000

/* Socket option levels */
#define SOL_SOCKET     1

/* Socket options (SOL_SOCKET level) */
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_DONTROUTE   5
#define SO_BROADCAST   6
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_KEEPALIVE   9
#define SO_OOBINLINE   10
#define SO_LINGER      13
#define SO_REUSEPORT   15
#define SO_PASSCRED    16
#define SO_PEERCRED    17
#define SO_RCVLOWAT    18
#define SO_SNDLOWAT    19
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ACCEPTCONN  30
#define SO_PROTOCOL    38
#define SO_DOMAIN      39

/* IPPROTO_IP options */
#define IP_OPTIONS          4
#define IP_TOS              1
#define IP_TTL              2
#define IP_ADD_MEMBERSHIP   35
#define IP_DROP_MEMBERSHIP  36
#define IP_MULTICAST_IF     32
#define IP_MULTICAST_TTL    33
#define IP_MULTICAST_LOOP   34

/* IPPROTO_TCP options (beyond TCP_NODELAY=1) */
#define TCP_NODELAY    1
#define TCP_KEEPIDLE   4
#define TCP_KEEPINTVL  5
#define TCP_KEEPCNT    6

/* ioctl commands for sockets */
#define FIONREAD       0x541B
#define FIONBIO        0x5421

/* Multicast group request (IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP) */
struct ip_mreq {
    uint32 imr_multiaddr;   /* IP multicast group address (network order) */
    uint32 imr_interface;   /* local interface address (network order) */
};

/* SO_LINGER structure */
struct k_linger {
    int l_onoff;    /* linger active */
    int l_linger;   /* how many seconds to linger for */
};

/* User-space sockaddr_in layout (must match newlib header) */
struct k_sockaddr_in {
    uint16 sin_family;
    uint16 sin_port;      /* network byte order */
    uint32 sin_addr;      /* network byte order */
    char   sin_zero[8];
};

#define K_SOCKADDR_IN_SIZE  16

static int sock_is_nonblock(int fd, int msg_flags);

/* ========================================================================== */
/* Allocate / free lwip_sock                                                  */
/* ========================================================================== */

static struct lwip_sock *lwip_sock_alloc(void)
{
    /* Use kalloc (page alloc) — struct is small but we have no kmalloc */
    struct lwip_sock *sk = (struct lwip_sock *)kalloc();
    if (sk == NULL)
        return NULL;
    memset(sk, 0, sizeof(*sk));
    return sk;
}

static void lwip_sock_free(struct lwip_sock *sk)
{
    if (sk == NULL)
        return;
    if (sk->lastbuf != NULL)
        netbuf_delete(sk->lastbuf);
    if (sk->lastpbuf != NULL)
        pbuf_free(sk->lastpbuf);
    if (sk->conn != NULL) {
        /*
         * Follow lwIP reference (lwip_close in sockets.c):
         * 1. netconn_prepare_delete() — synchronous TCP close, sets pcb=NULL
         *    and MBOXINVALID
         * 2. netconn_delete() — sees MBOXINVALID, goes to netconn_free()
         *
         * Calling netconn_delete() alone would also call prepare_delete
         * internally, but only if MBOXINVALID is not yet set. Calling
         * prepare_delete explicitly first is safer and matches the
         * reference lwIP socket layer.
         */
        netconn_prepare_delete(sk->conn);
        netconn_delete(sk->conn);
    }
    kfree((void *)sk);
}

static int sock_tcp_recv_avail(struct netconn *conn)
{
    int recv_avail = 0;

    if (conn == NULL)
        return 0;

#if LWIP_SO_RCVBUF
    SYS_ARCH_GET(conn->recv_avail, recv_avail);
    if (recv_avail < 0)
        recv_avail = 0;
#endif

    return recv_avail;
}

static int sock_has_rx_data(struct lwip_sock *sk)
{
    struct netconn *conn;

    if (sk == NULL || sk->conn == NULL)
        return 0;

    if (sk->lastbuf != NULL || sk->lastpbuf != NULL)
        return 1;

    conn = sk->conn;
    if (conn->state == NETCONN_LISTEN) {
#if LWIP_TCP
        return sys_mbox_valid(&conn->acceptmbox) && conn->acceptmbox.count > 0;
#else
        return 0;
#endif
    }

    if (sk->type == SOCK_STREAM && sock_tcp_recv_avail(conn) > 0)
        return 1;

    return sys_mbox_valid(&conn->recvmbox) && conn->recvmbox.count > 0;
}

static err_t sock_tcp_recv_pbuf(struct lwip_sock *sk, struct pbuf **p,
                                int nonblock)
{
    u8_t apiflags = NETCONN_NOAUTORCVD;

    if (nonblock)
        apiflags |= NETCONN_DONTBLOCK;
    return netconn_recv_tcp_pbuf_flags(sk->conn, p, apiflags);
}

static void sock_tcp_recvd(struct lwip_sock *sk, size_t len)
{
    if (sk == NULL || sk->conn == NULL || len == 0)
        return;

    (void)netconn_tcp_recvd(sk->conn, len);
}

static void sock_notify_if_still_readable(int fd, struct lwip_sock *sk)
{
    if (sk == NULL || !sock_has_rx_data(sk))
        return;

    struct vfs_file *file = vfs_fdtable_get_file(current->fdtable, fd);
    if (file == NULL)
        return;

    vfs_file_knote_notify(file, EVFILT_READ, 0);
    vfs_fput(file);
}

static void sock_notify_file_if_still_readable(struct vfs_file *file,
                                               struct lwip_sock *sk)
{
    if (file == NULL || sk == NULL || !sock_has_rx_data(sk))
        return;

    vfs_file_knote_notify(file, EVFILT_READ, 0);
}

static int sock_tcp_wait_for_more(struct lwip_sock *sk, int nonblock,
                                  ssize_t total, size_t target, int *waits)
{
    if (nonblock || total <= 0 || (size_t)total >= target)
        return 0;
    if (waits == NULL || *waits >= 4)
        return 0;
    if (signal_pending(current))
        return 0;

    (*waits)++;
    sleep_ms(1);
    return sock_has_rx_data(sk);
}

static int sock_tcp_recv_copyout(struct lwip_sock *sk, int fd,
                                 uint64 ubuf, int len, int flags)
{
    ssize_t total = 0;
    int nonblock = sock_is_nonblock(fd, flags);
    int wait_more = 0;

    while (total < len) {
        struct pbuf *p = NULL;
        uint16 poff = 0;
        bool reused_lastpbuf = false;

        if (sk->lastpbuf != NULL) {
            p = sk->lastpbuf;
            poff = sk->lastpbuf_off;
            reused_lastpbuf = true;
        } else {
            err_t err = sock_tcp_recv_pbuf(sk, &p,
                nonblock || total > 0);
            if (err != ERR_OK) {
                if (err == ERR_CLSD)
                    return total > 0 ? (int)total : 0;
                if (err == ERR_TIMEOUT && signal_pending(current))
                    return total > 0 ? (int)total : -EINTR;
                if (total > 0 && (err == ERR_WOULDBLOCK || err == ERR_TIMEOUT)) {
                    if (sock_tcp_wait_for_more(sk, nonblock, total,
                                               (size_t)len, &wait_more))
                        continue;
                    return (int)total;
                }
                return -lwip_err_to_errno(err);
            }
        }

        uint16 avail = p->tot_len - poff;
        uint16 tocopy = (len - total < avail) ? (uint16)(len - total) : avail;
        char tmpbuf[1500];
        uint16 copied = 0;

        while (copied < tocopy) {
            uint16 chunk = tocopy - copied;
            if (chunk > sizeof(tmpbuf))
                chunk = sizeof(tmpbuf);
            uint16 got = pbuf_copy_partial(p, tmpbuf, chunk, poff + copied);
            if (got == 0)
                break;
            if (vm_copyout(current->vm, ubuf + total + copied, tmpbuf, got) < 0) {
                if (!reused_lastpbuf)
                    pbuf_free(p);
                return total > 0 ? (int)total : -EFAULT;
            }
            copied += got;
        }

        if (flags & MSG_PEEK) {
            sk->lastpbuf = p;
            sk->lastpbuf_off = poff;
        } else if ((uint16)(poff + copied) < p->tot_len) {
            sk->lastpbuf = p;
            sk->lastpbuf_off = poff + copied;
        } else {
            pbuf_free(p);
            sk->lastpbuf = NULL;
            sk->lastpbuf_off = 0;
        }

        if (!(flags & MSG_PEEK))
            sock_tcp_recvd(sk, copied);

        total += copied;
        if (copied == 0 || (flags & MSG_PEEK))
            break;
    }

    if (!(flags & MSG_PEEK) && total > 0)
        sock_notify_if_still_readable(fd, sk);

    return (int)total;
}

/* ========================================================================== */
/* VFS file_ops for sockets — allows read()/write()/close() on socket fds     */
/* ========================================================================== */

/*
 * sock_poll_ready - quick non-blocking readiness check for a netconn
 *
 * @sk: socket
 * @events: POLLIN or POLLOUT
 * Returns: non-zero if ready (or error/HUP), 0 if would block.
 */
static int sock_poll_ready(struct lwip_sock *sk, short events)
{
    struct netconn *conn = sk->conn;
    if (conn == NULL)
        return POLLNVAL;

    short revents = 0;

    if (events & POLLIN) {
        if (sock_has_rx_data(sk)) {
            revents |= POLLIN;
        }
        if (conn->pending_err != ERR_OK)
            revents |= POLLERR;
        if (conn->flags & NETCONN_FLAG_MBOXCLOSED)
            revents |= POLLHUP;
    }

    if (events & POLLOUT) {
        revents |= POLLOUT;
    }

    return revents;
}

static ssize_t sock_file_read(struct vfs_file *file, char *buf, size_t count,
                              bool user)
{
    struct lwip_sock *sk = (struct lwip_sock *)file->private_data;
    if (sk == NULL || sk->conn == NULL)
        return -EBADF;

    if (sk->type == SOCK_STREAM) {
        ssize_t total = 0;
        int nonblock = (file->f_flags & O_NONBLOCK) != 0;
        int wait_more = 0;

        while ((size_t)total < count) {
            struct pbuf *p = NULL;
            uint16 poff = 0;
            uint16 avail;
            uint16 tocopy;
            uint16 copied = 0;

            if (sk->lastpbuf != NULL) {
                p = sk->lastpbuf;
                poff = sk->lastpbuf_off;
            } else {
                err_t err = sock_tcp_recv_pbuf(sk, &p,
                    nonblock || total > 0);
                if (err != ERR_OK) {
                    if (err == ERR_CLSD)
                        return total > 0 ? total : 0;
                    if (total > 0 &&
                        (err == ERR_WOULDBLOCK || err == ERR_TIMEOUT)) {
                        if (sock_tcp_wait_for_more(sk, nonblock, total,
                                                   count, &wait_more))
                            continue;
                        return total;
                    }
                    return -lwip_err_to_errno(err);
                }
            }

            avail = p->tot_len - poff;
            tocopy = (count - total < avail) ? (uint16)(count - total) : avail;
            while (copied < tocopy) {
                uint16 chunk = tocopy - copied;
                uint16 got;
                char tmpbuf[1500];

                if (chunk > sizeof(tmpbuf))
                    chunk = sizeof(tmpbuf);
                got = pbuf_copy_partial(p,
                    user ? tmpbuf : buf + total + copied,
                    chunk, poff + copied);
                if (got == 0)
                    break;
                if (user &&
                    vm_copyout(current->vm, (uint64)buf + total + copied,
                               tmpbuf, got) < 0) {
                    if (sk->lastpbuf != p)
                        pbuf_free(p);
                    return total > 0 ? total : -EFAULT;
                }
                copied += got;
            }

            if ((uint16)(poff + copied) < p->tot_len) {
                sk->lastpbuf = p;
                sk->lastpbuf_off = poff + copied;
            } else {
                pbuf_free(p);
                sk->lastpbuf = NULL;
                sk->lastpbuf_off = 0;
            }
            sock_tcp_recvd(sk, copied);
            total += copied;
            if (copied == 0)
                break;
        }

        if (total > 0)
            sock_notify_file_if_still_readable(file, sk);

        return total;

    } else {
        /* UDP/RAW: use netconn_recv → netbuf */
        struct netbuf *nb = NULL;

        /* O_NONBLOCK: check readiness before blocking */
        if (file->f_flags & O_NONBLOCK) {
            int ready = sock_poll_ready(sk, POLLIN);
            if (!(ready & (POLLIN | POLLHUP | POLLERR)))
                return -EAGAIN;
        }

        err_t err = netconn_recv(sk->conn, &nb);
        if (err != ERR_OK)
            return -lwip_err_to_errno(err);

        void *data;
        u16_t len;
        netbuf_data(nb, &data, &len);

        uint16 tocopy = (count < len) ? (uint16)count : len;

        if (user) {
            if (vm_copyout(current->vm, (uint64)buf, data, tocopy) < 0) {
                netbuf_delete(nb);
                return -EFAULT;
            }
        } else {
            memmove(buf, data, tocopy);
        }
        netbuf_delete(nb);
        return tocopy;
    }
}

static ssize_t sock_file_write(struct vfs_file *file, const char *buf,
                               size_t count, bool user)
{
    struct lwip_sock *sk = (struct lwip_sock *)file->private_data;
    if (sk == NULL || sk->conn == NULL)
        return -EBADF;

    if (sk->type == SOCK_STREAM) {
        /* TCP: use netconn_write_partly (non-blocking requires bytes_written) */
        char tmpbuf[1500];
        size_t written = 0;

        while (written < count) {
            size_t chunk = count - written;
            if (chunk > sizeof(tmpbuf))
                chunk = sizeof(tmpbuf);

            if (user) {
                if (vm_copyin(current->vm, tmpbuf,
                              (uint64)(buf + written), chunk) < 0)
                    return written > 0 ? (ssize_t)written : -EFAULT;
            } else {
                memmove(tmpbuf, buf + written, chunk);
            }

            size_t chunk_written = 0;
            err_t err = netconn_write_partly(sk->conn, tmpbuf, chunk,
                                             NETCONN_COPY, &chunk_written);
            if (err != ERR_OK) {
                sock_dbg("write TCP err=%d (errno=%d) chunk=%d written=%d nb=%d pending_err=%d\n",
                         (int)err, lwip_err_to_errno(err), (int)chunk,
                         (int)written, netconn_is_nonblocking(sk->conn),
                         (int)sk->conn->pending_err);
                if (err == ERR_WOULDBLOCK) {
                    if (written > 0)
                        return (ssize_t)written;
                    return -EAGAIN;
                }
                if (written > 0)
                    return (ssize_t)written;
                return -lwip_err_to_errno(err);
            }
            written += chunk_written;
            if (chunk_written < chunk)
                break;  /* partial write — return what we have */
        }
        return (ssize_t)written;

    } else {
        /* Clamp to uint16 max to avoid truncation in netbuf_alloc */
        if (count > 65535) {
            return -EMSGSIZE;
        }

        /* UDP: use netconn_send with netbuf */
        struct netbuf *nb = netbuf_new();
        if (nb == NULL)
            return -ENOMEM;

        void *data = netbuf_alloc(nb, (uint16)count);
        if (data == NULL) {
            netbuf_delete(nb);
            return -ENOMEM;
        }

        if (user) {
            if (vm_copyin(current->vm, data, (uint64)buf, count) < 0) {
                netbuf_delete(nb);
                return -EFAULT;
            }
        } else {
            memmove(data, buf, count);
        }

        err_t err = netconn_send(sk->conn, nb);
        netbuf_delete(nb);
        if (err != ERR_OK)
            return -lwip_err_to_errno(err);
        return (ssize_t)count;
    }
}

static int sock_file_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    struct lwip_sock *sk = (struct lwip_sock *)file->private_data;
    if (sk != NULL && sk->conn != NULL) {
        /* Disconnect callback before delete to prevent the tcpip thread
         * from calling sock_netconn_callback with a stale vfs_file ptr.
         * Use a release store so the acquire load in the callback sees
         * the NULL before any subsequent memory is freed. */
        sk->conn->callback = NULL;
        __atomic_store_n((void **)&sk->conn->callback_arg.ptr, NULL,
                         __ATOMIC_RELEASE);
    }
    lwip_sock_free(sk);
    file->private_data = NULL;
    return 0;
}

/*
 * sock_file_poll - check whether a lwIP socket is ready for I/O
 *
 * Checks for buffered data (lastbuf) and the netconn receive mailbox
 * to determine POLLIN readiness.  For TCP listening sockets, checks
 * the accept mailbox.  POLLOUT is always reported as ready (lwIP
 * netconn_write / netconn_send will handle back-pressure internally).
 */
static int sock_file_poll(struct vfs_file *file, short events)
{
    struct lwip_sock *sk = (struct lwip_sock *)file->private_data;
    short revents = 0;

    if (sk == NULL || sk->conn == NULL)
        return POLLNVAL;

    struct netconn *conn = sk->conn;

    if (events & (POLLIN | POLLRDNORM)) {
        if (sock_has_rx_data(sk)) {
            revents |= (events & (POLLIN | POLLRDNORM));
        }
    }

    /* POLLERR and POLLHUP are output-only — always check regardless of events */
    if (conn->pending_err != ERR_OK) {
        revents |= POLLERR;
    }
    if (conn->flags & NETCONN_FLAG_MBOXCLOSED) {
        revents |= POLLHUP;
    }

    if (events & (POLLOUT | POLLWRNORM)) {
        /* If a non-blocking TCP connect is in progress, don't report
         * writable until the handshake completes (state leaves NETCONN_CONNECT). */
        if (conn->state == NETCONN_CONNECT) {
            /* Still connecting — not writable yet. */
        } else {
            revents |= (events & (POLLOUT | POLLWRNORM));
        }
    }

    /* Periodic trace for POLLOUT during connect (throttled) */
    static uint64 last_poll_log;
    uint64 now = r_time();
    if ((events & POLLOUT) && conn->state == NETCONN_CONNECT &&
        (now - last_poll_log > 2000000000ULL)) {
        last_poll_log = now;
        sock_dbg("poll: POLLOUT requested, conn_state=CONNECT pending_err=%d revents=0x%x\n",
                 (int)conn->pending_err, revents);
    }

    return revents;
}

/*
 * sock_file_ioctl - handle ioctl commands on socket file descriptors
 *
 * Supports FIONREAD (bytes available to read), FIONBIO (set nonblock),
 * and SIOC* network interface/routing ioctls.
 *
 * The arg pointer comes from sys_vfs_ioctl's default case, which passes
 * the raw user-space arg value as an opaque void*.
 */

/* ---- SIOC* constants (match musl <sys/ioctl.h>) ---- */
#define SIOCADDRT       0x890B
#define SIOCDELRT       0x890C
#define SIOCGIFNAME     0x8910
#define SIOCGIFCONF     0x8912
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFDSTADDR  0x8917
#define SIOCGIFBRDADDR  0x8919
#define SIOCGIFNETMASK  0x891b
#define SIOCSIFNETMASK  0x891c
#define SIOCGIFMTU      0x8921
#define SIOCSIFMTU      0x8922
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933

/* IFF_* flag values (match Linux / musl <net/if.h>) */
#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_LOOPBACK    0x8
#define IFF_RUNNING     0x40
#define IFF_MULTICAST   0x1000

/* AF_INET for sockaddr_in family */
#define SIOC_AF_INET    2

/* ARPHRD_ETHER for ifr_hwaddr */
#define SIOC_ARPHRD_ETHER    1
#define SIOC_ARPHRD_LOOPBACK 772

/*
 * struct ifreq / rtentry layout (matches musl <net/if.h>, <net/route.h>)
 * We define our own kernel-side versions to avoid header conflicts.
 * The kernel doesn't have a BSD struct sockaddr, so we define a
 * layout-compatible one here.
 */
#define SIOC_IFNAMSIZ 16

/* snprintf — implemented in sys_arch.c */
int snprintf(char *buf, size_t size, const char *fmt, ...);

/*
 * k_sockaddr matches the musl struct sockaddr layout:
 *   uint16 sa_family + char sa_data[14]  =  16 bytes total
 */
struct k_sockaddr {
    uint16 sa_family;
    char   sa_data[14];
};

struct k_ifreq {
    char            ifr_name[SIOC_IFNAMSIZ];
    union {
        struct k_sockaddr ifr_addr;
        struct k_sockaddr ifr_dstaddr;
        struct k_sockaddr ifr_broadaddr;
        struct k_sockaddr ifr_netmask;
        struct k_sockaddr ifr_hwaddr;
        short           ifr_flags;
        int             ifr_ifindex;
        int             ifr_mtu;
        char            _pad[24];     /* room for the full union */
    };
};

/*
 * struct rtentry layout (matches musl <net/route.h>).
 * We only use the fields we know how to handle.
 */
#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002
#define RTF_HOST    0x0004

struct k_rtentry {
    unsigned long      rt_pad1;
    struct k_sockaddr  rt_dst;
    struct k_sockaddr  rt_gateway;
    struct k_sockaddr  rt_genmask;
    unsigned short     rt_flags;
    short              rt_pad2;
    unsigned long      rt_pad3;
    void              *rt_pad4;
    short              rt_metric;
    char              *rt_dev;        /* user-space pointer to device name */
    unsigned long      rt_mtu;
    unsigned long      rt_window;
    unsigned short     rt_irtt;
};

/*
 * sioc_find_netif - lookup a lwIP netif by ifreq name (e.g. "en1", "lo")
 *
 * lwIP names are 2-char + num: nif->name[0..1] + nif->num.
 * We format each as "%c%c%d" and compare against ifr_name.
 * Special case: "lo" matches the loopback pseudo-interface.
 */
static struct netif *sioc_find_netif(const char *name)
{
    /* Try netif_find first (it handles "xx0" 3-char names) */
    struct netif *nif = netif_find(name);
    if (nif != NULL)
        return nif;

    /* Manual search: format each netif name and strcmp */
    char ifname[16];
    NETIF_FOREACH(nif) {
        snprintf(ifname, sizeof(ifname), "%c%c%d",
                 nif->name[0], nif->name[1], nif->num);
        if (strncmp(ifname, name, SIOC_IFNAMSIZ) == 0)
            return nif;
    }
    return NULL;
}

/*
 * sioc_netif_to_flags - convert lwIP netif flags to IFF_* flags
 */
static unsigned int sioc_netif_to_flags(struct netif *nif)
{
    unsigned int flags = 0;
    if (nif->flags & NETIF_FLAG_UP)
        flags |= IFF_UP | IFF_RUNNING;
    if (nif->flags & NETIF_FLAG_BROADCAST)
        flags |= IFF_BROADCAST;
    if (nif->flags & NETIF_FLAG_IGMP)
        flags |= IFF_MULTICAST;
    if (!(nif->flags & NETIF_FLAG_ETHARP))
        flags |= IFF_LOOPBACK;
    return flags;
}

/*
 * sioc_set_sockaddr_in - fill a k_sockaddr with AF_INET + ip4 address
 */
static void sioc_set_sockaddr_in(struct k_sockaddr *sa, uint32 ip4_addr_nbo)
{
    memset(sa, 0, sizeof(*sa));
    sa->sa_family = SIOC_AF_INET;
    /* sockaddr_in layout: family(2) + port(2) + addr(4) */
    /* addr sits at offset 4 in sa_data */
    memmove(&sa->sa_data[2], &ip4_addr_nbo, 4);
}

/*
 * sioc_get_sockaddr_in - extract ip4 address (network byte order) from k_sockaddr
 */
static uint32 sioc_get_sockaddr_in(const struct k_sockaddr *sa)
{
    uint32 addr;
    memmove(&addr, &sa->sa_data[2], 4);
    return addr;
}

/*
 * sioc_handle_ifreq - process SIOCGxxx / SIOCSxxx ioctls.
 * The ifreq struct has already been copied in from user space.
 * Returns 0 on success, -errno on error.
 * set *copyback = 1 if the ifreq should be copied back out.
 */
static int sioc_handle_ifreq(uint64 cmd, struct k_ifreq *ifr, int *copyback)
{
    *copyback = 0;
    ifr->ifr_name[SIOC_IFNAMSIZ - 1] = '\0';

    struct netif *nif = sioc_find_netif(ifr->ifr_name);
    if (nif == NULL)
        return -ENODEV;

    switch (cmd) {
    case SIOCGIFFLAGS:
        ifr->ifr_flags = (short)sioc_netif_to_flags(nif);
        *copyback = 1;
        return 0;

    case SIOCSIFFLAGS: {
        if (ifr->ifr_flags & IFF_UP) {
            if (!(nif->flags & NETIF_FLAG_UP))
                netif_set_up(nif);
        } else {
            if (nif->flags & NETIF_FLAG_UP)
                netif_set_down(nif);
        }
        return 0;
    }

    case SIOCGIFADDR: {
        uint32 addr = netif_ip4_addr(nif)->addr;
        sioc_set_sockaddr_in(&ifr->ifr_addr, addr);
        *copyback = 1;
        return 0;
    }

    case SIOCSIFADDR: {
        if (ifr->ifr_addr.sa_family != SIOC_AF_INET)
            return -EAFNOSUPPORT;
        ip4_addr_t addr;
        addr.addr = sioc_get_sockaddr_in(&ifr->ifr_addr);
        netif_set_ipaddr(nif, &addr);
        return 0;
    }

    case SIOCGIFNETMASK: {
        uint32 mask = netif_ip4_netmask(nif)->addr;
        sioc_set_sockaddr_in(&ifr->ifr_netmask, mask);
        *copyback = 1;
        return 0;
    }

    case SIOCSIFNETMASK: {
        if (ifr->ifr_netmask.sa_family != SIOC_AF_INET)
            return -EAFNOSUPPORT;
        ip4_addr_t mask;
        mask.addr = sioc_get_sockaddr_in(&ifr->ifr_netmask);
        netif_set_netmask(nif, &mask);
        return 0;
    }

    case SIOCGIFBRDADDR: {
        /* broadcast = ip | ~netmask */
        uint32 addr = netif_ip4_addr(nif)->addr;
        uint32 mask = netif_ip4_netmask(nif)->addr;
        uint32 brd = addr | ~mask;
        sioc_set_sockaddr_in(&ifr->ifr_broadaddr, brd);
        *copyback = 1;
        return 0;
    }

    case SIOCGIFMTU:
        ifr->ifr_mtu = nif->mtu;
        *copyback = 1;
        return 0;

    case SIOCSIFMTU:
        if (ifr->ifr_mtu < 68 || ifr->ifr_mtu > 65535)
            return -EINVAL;
        nif->mtu = (uint16)ifr->ifr_mtu;
        return 0;

    case SIOCGIFHWADDR: {
        memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
        if (nif->flags & NETIF_FLAG_ETHARP)
            ifr->ifr_hwaddr.sa_family = SIOC_ARPHRD_ETHER;
        else
            ifr->ifr_hwaddr.sa_family = SIOC_ARPHRD_LOOPBACK;
        if (nif->hwaddr_len > 0 && nif->hwaddr_len <= 14)
            memmove(ifr->ifr_hwaddr.sa_data, nif->hwaddr, nif->hwaddr_len);
        *copyback = 1;
        return 0;
    }

    case SIOCGIFINDEX: {
        /* Assign sequential index matching netlink handle_getlink */
        int idx = 1;
        struct netif *n;
        NETIF_FOREACH(n) {
            if (n == nif) { ifr->ifr_ifindex = idx; break; }
            idx++;
        }
        *copyback = 1;
        return 0;
    }

    case SIOCGIFNAME: {
        /* Given ifr_ifindex, fill ifr_name */
        int target_idx = ifr->ifr_ifindex;
        int idx = 1;
        struct netif *n;
        NETIF_FOREACH(n) {
            if (idx == target_idx) {
                snprintf(ifr->ifr_name, SIOC_IFNAMSIZ, "%c%c%d",
                         n->name[0], n->name[1], n->num);
                *copyback = 1;
                return 0;
            }
            idx++;
        }
        return -ENODEV;
    }

    default:
        return -ENOTTY;
    }
}

/*
 * sioc_handle_route - process SIOCADDRT / SIOCDELRT ioctls
 *
 * lwIP doesn't have a real routing table — routing is implicit via
 * interface IP/netmask and netif_default gateway.  We support:
 *   - Adding/changing default gateway: set netif_default + netif gw
 *   - Deleting default route: clear gateway
 */
static int sioc_handle_route(uint64 cmd, struct k_rtentry *rt)
{
    if (cmd == SIOCADDRT) {
        if (rt->rt_dst.sa_family != SIOC_AF_INET)
            return -EAFNOSUPPORT;

        uint32 dst_addr = sioc_get_sockaddr_in(&rt->rt_dst);
        uint32 gw_addr = sioc_get_sockaddr_in(&rt->rt_gateway);
        uint32 mask_addr = sioc_get_sockaddr_in(&rt->rt_genmask);

        if (rt->rt_flags & RTF_GATEWAY) {
            /* Find target interface */
            struct netif *nif = NULL;

            if (rt->rt_dev != NULL) {
                /* User specified device name — copy it in */
                char devname[SIOC_IFNAMSIZ];
                if (vm_copyin(current->vm, devname, (uint64)rt->rt_dev,
                              SIOC_IFNAMSIZ) < 0)
                    return -EFAULT;
                devname[SIOC_IFNAMSIZ - 1] = '\0';
                nif = sioc_find_netif(devname);
            }

            if (nif == NULL) {
                /* Use first interface that's UP */
                NETIF_FOREACH(nif) {
                    if (nif->flags & NETIF_FLAG_UP)
                        break;
                }
            }

            if (nif == NULL)
                return -ENETUNREACH;

            /* Default route: dst == 0.0.0.0 */
            if (dst_addr == 0) {
                ip4_addr_t gw;
                gw.addr = gw_addr;
                netif_set_gw(nif, &gw);
                netif_set_default(nif);
                return 0;
            }

            /* Non-default route: set the gateway on the matching interface.
             * lwIP's routing is subnet-based, so we just set the gateway. */
            ip4_addr_t gw;
            gw.addr = gw_addr;
            netif_set_gw(nif, &gw);
            return 0;
        }

        /* Non-gateway route: ensure the interface's IP/netmask match */
        (void)dst_addr;
        (void)mask_addr;
        return 0;  /* accept silently — lwIP handles subnet routing automatically */

    } else if (cmd == SIOCDELRT) {
        if (rt->rt_dst.sa_family != SIOC_AF_INET)
            return -EAFNOSUPPORT;

        uint32 dst_addr = sioc_get_sockaddr_in(&rt->rt_dst);

        /* Delete default route */
        if (dst_addr == 0) {
            struct netif *nif = netif_default;
            if (nif != NULL) {
                ip4_addr_t zero;
                zero.addr = 0;
                netif_set_gw(nif, &zero);
            }
            return 0;
        }

        /* Non-default route deletion — no-op in lwIP */
        return 0;
    }

    return -EINVAL;
}

static int sock_file_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct lwip_sock *sk = (struct lwip_sock *)file->private_data;
    if (sk == NULL || sk->conn == NULL)
        return -EBADF;

    switch (cmd) {
    case FIONREAD: {
        /* Return bytes available in the receive buffer */
        int count = 0;

        /* Check for buffered data from partial reads */
        if (sk->lastpbuf != NULL) {
            count = (int)(sk->lastpbuf->tot_len - sk->lastpbuf_off);
        } else if (sk->lastbuf != NULL) {
            count = (int)(netbuf_len(sk->lastbuf) - sk->lastoffset);
        } else if (sk->type == SOCK_STREAM) {
            count = sock_tcp_recv_avail(sk->conn);
        }

        /* Datagram sockets fall back to the recv mailbox depth. */
        if (count == 0 && sk->type != SOCK_STREAM &&
            sk->conn->state != NETCONN_LISTEN) {
            if (sys_mbox_valid(&sk->conn->recvmbox))
                count = (int)sk->conn->recvmbox.count;
        }

        /* arg is a user-space pointer to int */
        if (vm_copyout(current->vm, (uint64)arg, &count, sizeof(count)) < 0)
            return -EFAULT;
        return 0;
    }
    case FIONBIO: {
        /* Set or clear O_NONBLOCK on this file descriptor */
        int val;
        if (vm_copyin(current->vm, &val, (uint64)arg, sizeof(val)) < 0)
            return -EFAULT;
        if (val)
            file->f_flags |= O_NONBLOCK;
        else
            file->f_flags &= ~O_NONBLOCK;
        /* Keep lwIP netconn non-blocking flag in sync */
        netconn_set_nonblocking(sk->conn, val != 0);
        return 0;
    }

    /* ---- Network interface ioctls ---- */
    case SIOCGIFFLAGS: case SIOCSIFFLAGS:
    case SIOCGIFADDR:  case SIOCSIFADDR:
    case SIOCGIFNETMASK: case SIOCSIFNETMASK:
    case SIOCGIFBRDADDR: case SIOCGIFDSTADDR:
    case SIOCGIFMTU: case SIOCSIFMTU:
    case SIOCGIFHWADDR: case SIOCGIFINDEX: case SIOCGIFNAME:
    {
        struct k_ifreq ifr;
        if (vm_copyin(current->vm, &ifr, (uint64)arg, sizeof(ifr)) < 0)
            return -EFAULT;
        int copyback = 0;
        int ret = sioc_handle_ifreq(cmd, &ifr, &copyback);
        if (ret == 0 && copyback) {
            if (vm_copyout(current->vm, (uint64)arg, &ifr, sizeof(ifr)) < 0)
                return -EFAULT;
        }
        return ret;
    }

    /* ---- Routing ioctls ---- */
    case SIOCADDRT: case SIOCDELRT:
    {
        struct k_rtentry rt;
        if (vm_copyin(current->vm, &rt, (uint64)arg, sizeof(rt)) < 0)
            return -EFAULT;
        return sioc_handle_route(cmd, &rt);
    }

    default:
        return -ENOTTY;
    }
}

static struct vfs_file_ops lwip_socket_file_ops = {
    .read    = sock_file_read,
    .write   = sock_file_write,
    .llseek  = NULL,
    .release = sock_file_release,
    .fsync   = NULL,
    .fflush  = NULL,
    .poll    = sock_file_poll,
    .ioctl   = sock_file_ioctl,
    .fault   = NULL,
};

/* ========================================================================== */
/* lwIP netconn callback → kqueue push notification                           */
/* ========================================================================== */

/*
 * sock_netconn_callback - called by lwIP core (tcpip thread) when an event
 * occurs on a netconn.  We translate these into kqueue knote notifications
 * on the owning vfs_file so that epoll_wait / kevent callers are woken up
 * immediately.
 *
 * The vfs_file pointer is stashed in conn->callback_arg.ptr by
 * sock_fd_alloc() at socket-creation time.
 */
static void sock_netconn_callback(struct netconn *conn,
                                  enum netconn_evt evt, u16_t len)
{
    /*
     * The close path removes the fdtable entry and defers vfs_fput via
     * call_rcu.  By holding an RCU read lock here we guarantee that the
     * RCU-deferred slab_free in __vfs_file_free cannot run while we are
     * accessing the file memory — so the vfs_fdup() below always touches
     * valid slab memory (even if ref_count has already reached 0).
     *
     * Use an acquire load so we see the NULL that sock_file_release
     * stores with a release barrier before freeing the socket.
     */
    rcu_read_lock();
    struct vfs_file *file = (struct vfs_file *)
        __atomic_load_n((void **)&conn->callback_arg.ptr, __ATOMIC_ACQUIRE);
    if (file != NULL) {
        /*
         * Take a real reference.  If the file's ref_count has already
         * reached 0 (last vfs_fput ran), vfs_fdup returns NULL and we
         * skip the notification — the file is being torn down.
         */
        file = vfs_fdup(file);
    }
    rcu_read_unlock();

    if (file == NULL)
        return;

    switch (evt) {
    case NETCONN_EVT_RCVPLUS:
        vfs_file_knote_notify(file, EVFILT_READ, (int64)len);
        break;
    case NETCONN_EVT_SENDPLUS:
        sock_dbg("callback SENDPLUS conn=%p state=%d len=%d\n",
                 conn, (int)conn->state, (int)len);
        vfs_file_knote_notify(file, EVFILT_WRITE, (int64)len);
        break;
    case NETCONN_EVT_ERROR:
        sock_dbg("callback ERROR conn=%p pending_err=%d\n",
                 conn, (int)conn->pending_err);
        vfs_file_knote_notify(file, EVFILT_READ, 0);
        vfs_file_knote_notify(file, EVFILT_WRITE, 0);
        break;
    default:
        break;
    }
    vfs_fput(file);
}

/* ========================================================================== */
/* Helper: create a socket fd from a netconn                                  */
/* ========================================================================== */

static int sock_fd_alloc(struct netconn *conn, int type, int protocol,
                         int file_flags)
{
    struct lwip_sock *sk = lwip_sock_alloc();
    if (sk == NULL)
        return -ENOMEM;

    sk->conn = conn;
    sk->type = type;
    sk->protocol = protocol;

    /* Propagate O_NONBLOCK to the lwIP netconn layer so that
     * netconn_connect() etc. return ERR_INPROGRESS instead of blocking. */
    if (file_flags & O_NONBLOCK)
        netconn_set_nonblocking(conn, 1);

    int fd = vfs_custom_fd_alloc(&lwip_socket_file_ops, sk, file_flags);
    if (fd < 0) {
        lwip_sock_free(sk);
        return fd;
    }

    /* Wire up kqueue push notification: store the vfs_file pointer
     * so sock_netconn_callback() can call vfs_file_knote_notify(). */
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    conn->callback_arg.ptr = f;
    conn->callback = sock_netconn_callback;
    vfs_fput(f);  /* drop lookup ref — fd table owns the real ref */

    return fd;
}

/* Helper: get lwip_sock from fd */
static struct lwip_sock *sock_from_fd(int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return NULL;

    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = current->fdtable->files[fd];
    spin_unlock(&current->fdtable->lock);

    if (f == NULL || f->ops != &lwip_socket_file_ops)
        return NULL;

    return (struct lwip_sock *)f->private_data;
}

/*
 * sock_domain_from_fd - detect domain of a socket fd
 * Returns AF_INET (2), AF_UNIX (1), AF_NETLINK (16), or 0 if not a socket.
 */
static int sock_domain_from_fd(int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return 0;

    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = current->fdtable->files[fd];
    spin_unlock(&current->fdtable->lock);

    if (f == NULL)
        return 0;
    if (f->ops == &lwip_socket_file_ops)
        return AF_INET;
    if (f->ops == &unix_socket_file_ops)
        return AF_UNIX;
    if (f->ops == &netlink_socket_file_ops)
        return AF_NETLINK;
    return 0;
}

/*
 * sock_is_nonblock - check if a socket fd should use non-blocking I/O
 *
 * Returns true if the file has O_NONBLOCK set or if MSG_DONTWAIT is
 * in the flags argument.
 */
static int sock_is_nonblock(int fd, int msg_flags)
{
    if (msg_flags & MSG_DONTWAIT)
        return 1;

    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = current->fdtable->files[fd];
    spin_unlock(&current->fdtable->lock);

    if (f == NULL)
        return 0;
    return (f->f_flags & O_NONBLOCK) != 0;
}

static size_t unix_ring_peek(const struct unix_ring *r, uint start_off,
                             char *buf, size_t len)
{
    size_t avail = r->nwrite - r->nread;
    if (start_off >= avail)
        return 0;

    avail -= start_off;
    size_t toread = len < avail ? len : avail;
    uint idx = (r->nread + start_off) % r->capacity;

    if (idx + toread <= r->capacity) {
        memmove(buf, &r->data[idx], toread);
    } else {
        size_t first = r->capacity - idx;
        memmove(buf, &r->data[idx], first);
        memmove(buf + first, &r->data[0], toread - first);
    }

    return toread;
}

/* ========================================================================== */
/* Map POSIX socket type → lwIP netconn type                                  */
/* ========================================================================== */

static enum netconn_type posix_to_netconn_type(int type, int protocol)
{
    switch (type) {
    case SOCK_STREAM:  return NETCONN_TCP;
    case SOCK_DGRAM:   return NETCONN_UDP;
    case SOCK_RAW:     return NETCONN_RAW;
    default:           return NETCONN_INVALID;
    }
}

/* ========================================================================== */
/* Syscall handlers                                                           */
/* ========================================================================== */

/*
 * sys_socket(domain, type, protocol) → fd
 */
uint64 sys_socket(void)
{
    int domain, type, protocol;
    argint(0, &domain);
    argint(1, &type);
    argint(2, &protocol);

    /* Strip Linux-style flags from type field */
    int flags = type & ~SOCK_TYPE_MASK;
    type &= SOCK_TYPE_MASK;

    int file_flags = O_RDWR;
    if (flags & SOCK_NONBLOCK)
        file_flags |= O_NONBLOCK;

    /* ---- AF_UNIX dispatch ---- */
    if (domain == AF_UNIX) {
        int fd = unix_sock_create(type, protocol, file_flags);
        if (fd < 0)
            return (uint64)fd;
        if (flags & SOCK_CLOEXEC) {
            spin_lock(&current->fdtable->lock);
            vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
            spin_unlock(&current->fdtable->lock);
        }
        ACCT_INC(current->thread_group, net_sockets);
        return (uint64)fd;
    }

    /* ---- AF_NETLINK dispatch ---- */
    if (domain == AF_NETLINK) {
        int fd = netlink_sock_create(type, protocol, file_flags);
        if (fd < 0)
            return (uint64)fd;
        if (flags & SOCK_CLOEXEC) {
            spin_lock(&current->fdtable->lock);
            vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
            spin_unlock(&current->fdtable->lock);
        }
        ACCT_INC(current->thread_group, net_sockets);
        return (uint64)fd;
    }

    /* ---- AF_INET (lwIP) ---- */
    if (domain != AF_INET)
        return (uint64)-EAFNOSUPPORT;

    enum netconn_type ntype = posix_to_netconn_type(type, protocol);
    if (ntype == NETCONN_INVALID)
        return (uint64)-EPROTOTYPE;

    struct netconn *conn = netconn_new(ntype);
    if (conn == NULL)
        return (uint64)-ENOMEM;

    int fd = sock_fd_alloc(conn, type, protocol, file_flags);
    if (fd < 0) {
        netconn_delete(conn);
        return (uint64)fd;
    }

    if (flags & SOCK_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

    ACCT_INC(current->thread_group, net_sockets);
    sock_dbg("socket(%d,%d,%d) = fd %d file_flags=0x%x\\n",
             domain, type, protocol, fd, file_flags);
    return (uint64)fd;
}

/*
 * sys_bind(fd, addr, addrlen) → 0 / -errno
 */
uint64 sys_bind(void)
{
    int fd, addrlen;
    uint64 uaddr;
    argint(0, &fd);
    argaddr(1, &uaddr);
    argint(2, &addrlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_bind(fd, uaddr, addrlen);
    if (domain == AF_NETLINK)
        return (uint64)netlink_sock_bind(fd, uaddr, addrlen);

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    if (addrlen < K_SOCKADDR_IN_SIZE)
        return (uint64)-EINVAL;

    struct k_sockaddr_in sa;
    if (vm_copyin(current->vm, &sa, uaddr, sizeof(sa)) < 0)
        return (uint64)-EFAULT;

    if (sa.sin_family != AF_INET)
        return (uint64)-EAFNOSUPPORT;

    ip_addr_t ipaddr;
    ip_addr_set_ip4_u32(&ipaddr, sa.sin_addr);

    err_t err = netconn_bind(sk->conn, &ipaddr, ntohs(sa.sin_port));
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

    return 0;
}

/*
 * sys_listen(fd, backlog) → 0 / -errno
 */
uint64 sys_listen(void)
{
    int fd, backlog;
    argint(0, &fd);
    argint(1, &backlog);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_listen(fd, backlog);
    if (domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    if (sk->type != SOCK_STREAM)
        return (uint64)-EOPNOTSUPP;

    err_t err = netconn_listen_with_backlog(sk->conn,
                                            (u8_t)(backlog > 255 ? 255 : backlog));
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

    return 0;
}

/*
 * sys_accept(fd, addr, addrlen_ptr) → new_fd / -errno
 */
uint64 sys_accept(void)
{
    int fd;
    uint64 uaddr, uaddrlen;
    argint(0, &fd);
    argaddr(1, &uaddr);
    argaddr(2, &uaddrlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_accept(fd, uaddr, uaddrlen, 0);
    if (domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    /* O_NONBLOCK: check if connections are pending */
    if (sock_is_nonblock(fd, 0)) {
        int ready = sock_poll_ready(sk, POLLIN);
        if (!(ready & (POLLIN | POLLHUP | POLLERR)))
            return (uint64)-EAGAIN;
    }

    struct netconn *newconn = NULL;
    err_t err = netconn_accept(sk->conn, &newconn);
    if (err != ERR_OK) {
        /* If lwIP returned timeout due to signal interruption, report EINTR */
        if (err == ERR_TIMEOUT && signal_pending(current))
            return (uint64)-EINTR;
        return (uint64)-lwip_err_to_errno(err);
    }

    int newfd = sock_fd_alloc(newconn, SOCK_STREAM, sk->protocol, O_RDWR);
    if (newfd < 0) {
        netconn_delete(newconn);
        return (uint64)newfd;
    }

    /* Fill in remote address if requested */
    if (uaddr != 0 && uaddrlen != 0) {
        ip_addr_t raddr;
        u16_t rport;
        netconn_peer(newconn, &raddr, &rport);

        struct k_sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(rport);
        sa.sin_addr = ip_addr_get_ip4_u32(&raddr);

        int alen = K_SOCKADDR_IN_SIZE;
        vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen));
        vm_copyout(current->vm, uaddr, &sa, sizeof(sa));
    }

    ACCT_INC(current->thread_group, net_accepts);
    return (uint64)newfd;
}

/*
 * sys_sconnect(fd, addr, addrlen) → 0 / -errno
 * Named "sconnect" to avoid collision with legacy SYS_connect (36).
 */
uint64 sys_sconnect(void)
{
    int fd, addrlen;
    uint64 uaddr;
    argint(0, &fd);
    argaddr(1, &uaddr);
    argint(2, &addrlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_connect(fd, uaddr, addrlen);
    if (domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    /* Sync lwIP non-blocking flag from VFS file flags.
     * fcntl(F_SETFL, O_NONBLOCK) only updates f_flags, so we must
     * propagate it to the netconn before any blocking lwIP call. */
    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    int is_nb = 0;
    if (f) {
        is_nb = (f->f_flags & O_NONBLOCK) != 0;
        netconn_set_nonblocking(sk->conn, is_nb);
        vfs_fput(f);
    }

    if (addrlen < K_SOCKADDR_IN_SIZE)
        return (uint64)-EINVAL;

    struct k_sockaddr_in sa;
    if (vm_copyin(current->vm, &sa, uaddr, sizeof(sa)) < 0)
        return (uint64)-EFAULT;

    if (sa.sin_family != AF_INET)
        return (uint64)-EAFNOSUPPORT;

    ip_addr_t ipaddr;
    ip_addr_set_ip4_u32(&ipaddr, sa.sin_addr);

    uint32 ip4 = ip_addr_get_ip4_u32(&ipaddr);
    sock_dbg("connect fd=%d %d.%d.%d.%d:%d nb=%d conn_flags=0x%x\n",
             fd,
             (ip4 >> 0) & 0xff, (ip4 >> 8) & 0xff,
             (ip4 >> 16) & 0xff, (ip4 >> 24) & 0xff,
             ntohs(sa.sin_port), is_nb,
             (int)sk->conn->flags);

    err_t err = netconn_connect(sk->conn, &ipaddr, ntohs(sa.sin_port));

    sock_dbg("connect fd=%d result err=%d (errno=%d) conn_state=%d\n",
             fd, (int)err, lwip_err_to_errno(err), (int)sk->conn->state);

    if (err == ERR_INPROGRESS) {
        /* Non-blocking connect: TCP handshake started, not yet complete */
        ACCT_INC(current->thread_group, net_connects);
        return (uint64)-EINPROGRESS;
    }
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

    ACCT_INC(current->thread_group, net_connects);
    return 0;
}

/*
 * sys_sendto(fd, buf, len, flags, dest_addr, addrlen) → nbytes / -errno
 */
uint64 sys_sendto(void)
{
    int fd, len, flags, addrlen;
    uint64 ubuf, uaddr;
    argint(0, &fd);
    argaddr(1, &ubuf);
    argint(2, &len);
    argint(3, &flags);
    argaddr(4, &uaddr);
    argint(5, &addrlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_NETLINK)
        return (uint64)netlink_sock_sendto(fd, ubuf, (size_t)len, flags,
                                           uaddr, addrlen);
    /* AF_UNIX: sendto on connected socket works like write */
    if (domain == AF_UNIX)
        return (uint64)-EOPNOTSUPP;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    if (len <= 0)
        return 0;

    /* MSG_DONTWAIT / O_NONBLOCK: check send readiness */
    if (sock_is_nonblock(fd, flags)) {
        int ready = sock_poll_ready(sk, POLLOUT);
        if (!(ready & POLLOUT))
            return (uint64)-EAGAIN;
    }

    if (sk->type == SOCK_STREAM) {
        /* TCP: ignore dest_addr, use netconn_write_partly (NB-safe) */
        char tmpbuf[1500];
        size_t chunk = (size_t)len;
        if (chunk > sizeof(tmpbuf))
            chunk = sizeof(tmpbuf);

        if (vm_copyin(current->vm, tmpbuf, ubuf, chunk) < 0)
            return (uint64)-EFAULT;

        size_t bytes_written = 0;
        err_t err = netconn_write_partly(sk->conn, tmpbuf, chunk,
                                         NETCONN_COPY, &bytes_written);
        if (err != ERR_OK) {
            sock_dbg("sendto fd=%d TCP write err=%d (errno=%d) len=%d nb=%d\n",
                     fd, (int)err, lwip_err_to_errno(err), (int)chunk,
                     netconn_is_nonblocking(sk->conn));
            if (err == ERR_WOULDBLOCK)
                return (uint64)-EAGAIN;
            return (uint64)-lwip_err_to_errno(err);
        }
        ACCT_ADD(current->thread_group, net_bytes_sent, bytes_written);
        return (uint64)bytes_written;

    } else {
        /* UDP/RAW: use netconn_sendto */
        struct netbuf *nb = netbuf_new();
        if (nb == NULL)
            return (uint64)-ENOMEM;

        uint16 sendlen = (len > 1460) ? 1460 : (uint16)len;
        void *data = netbuf_alloc(nb, sendlen);
        if (data == NULL) {
            netbuf_delete(nb);
            return (uint64)-ENOMEM;
        }

        if (vm_copyin(current->vm, data, ubuf, sendlen) < 0) {
            netbuf_delete(nb);
            return (uint64)-EFAULT;
        }

        /* Set destination if provided */
        if (uaddr != 0 && addrlen >= K_SOCKADDR_IN_SIZE) {
            struct k_sockaddr_in sa;
            if (vm_copyin(current->vm, &sa, uaddr, sizeof(sa)) < 0) {
                netbuf_delete(nb);
                return (uint64)-EFAULT;
            }
            ip_addr_t destip;
            ip_addr_set_ip4_u32(&destip, sa.sin_addr);
            err_t err = netconn_connect(sk->conn, &destip, ntohs(sa.sin_port));
            if (err != ERR_OK && err != ERR_ISCONN) {
                netbuf_delete(nb);
                return (uint64)-lwip_err_to_errno(err);
            }
        }

        err_t err = netconn_send(sk->conn, nb);
        netbuf_delete(nb);
        if (err != ERR_OK)
            return (uint64)-lwip_err_to_errno(err);
        ACCT_ADD(current->thread_group, net_bytes_sent, sendlen);
        return (uint64)sendlen;
    }
}

/*
 * sys_recvfrom(fd, buf, len, flags, src_addr, addrlen_ptr) → nbytes / -errno
 */
uint64 sys_recvfrom(void)
{
    int fd, len, flags;
    uint64 ubuf, uaddr, uaddrlen;
    argint(0, &fd);
    argaddr(1, &ubuf);
    argint(2, &len);
    argint(3, &flags);
    argaddr(4, &uaddr);
    argaddr(5, &uaddrlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_NETLINK)
        return (uint64)netlink_sock_recvfrom(fd, ubuf, (size_t)len, flags,
                                             uaddr, uaddrlen);
    if (domain == AF_UNIX)
        return (uint64)-EOPNOTSUPP;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    if (len <= 0)
        return 0;

    if (sk->type == SOCK_STREAM) {
        int n = sock_tcp_recv_copyout(sk, fd, ubuf, len, flags);
        if (n >= 0)
            ACCT_ADD(current->thread_group, net_bytes_recv, n);
        return (uint64)n;
    }

    /* MSG_DONTWAIT / O_NONBLOCK: check recv readiness */
    if (sock_is_nonblock(fd, flags)) {
        int ready = sock_poll_ready(sk, POLLIN);
        if (!(ready & (POLLIN | POLLHUP | POLLERR)))
            return (uint64)-EAGAIN;
    }

    if (sk->type == SOCK_STREAM) {
        /* TCP: use netconn_recv_tcp_pbuf */
        struct pbuf *p = NULL;

        /* Check for buffered partial pbuf from a previous peek/partial read */
        if (sk->lastpbuf != NULL) {
            p = sk->lastpbuf;
            uint16 avail = p->tot_len - sk->lastpbuf_off;
            uint16 tocopy = (len < (int)avail) ? (uint16)len : avail;
            char tmpbuf[1500];
            uint16 clen = (tocopy > sizeof(tmpbuf)) ? sizeof(tmpbuf) : tocopy;
            pbuf_copy_partial(p, tmpbuf, clen, sk->lastpbuf_off);

            if (vm_copyout(current->vm, ubuf, tmpbuf, clen) < 0)
                return (uint64)-EFAULT;

            if (!(flags & MSG_PEEK)) {
                sk->lastpbuf_off += clen;
                if (sk->lastpbuf_off >= p->tot_len) {
                    pbuf_free(p);
                    sk->lastpbuf = NULL;
                    sk->lastpbuf_off = 0;
                }
                sock_tcp_recvd(sk, clen);
            }
            ACCT_ADD(current->thread_group, net_bytes_recv, clen);
            return (uint64)clen;
        }

        err_t err = sock_tcp_recv_pbuf(sk, &p, sock_is_nonblock(fd, flags));
        if (err != ERR_OK) {
            if (err == ERR_CLSD)
                return 0; /* EOF */
            if (err == ERR_TIMEOUT && signal_pending(current))
                return (uint64)-EINTR;
            return (uint64)-lwip_err_to_errno(err);
        }

        uint16 tocopy = (len < (int)p->tot_len) ? (uint16)len : p->tot_len;
        char tmpbuf[1500];
        uint16 clen = (tocopy > sizeof(tmpbuf)) ? sizeof(tmpbuf) : tocopy;
        pbuf_copy_partial(p, tmpbuf, clen, 0);

        if (vm_copyout(current->vm, ubuf, tmpbuf, clen) < 0) {
            pbuf_free(p);
            return (uint64)-EFAULT;
        }

        if (flags & MSG_PEEK) {
            /* Save pbuf for next recv call to consume */
            sk->lastpbuf = p;
            sk->lastpbuf_off = 0;
        } else if (clen < p->tot_len) {
            /* Save remainder for future reads — prevents TCP data loss */
            sk->lastpbuf = p;
            sk->lastpbuf_off = clen;
        } else {
            pbuf_free(p);
        }
        if (!(flags & MSG_PEEK))
            sock_tcp_recvd(sk, clen);
        ACCT_ADD(current->thread_group, net_bytes_recv, clen);
        return (uint64)clen;

    } else {
        /* UDP/RAW: use netconn_recv → netbuf */
        struct netbuf *nb = NULL;

        /* Check for buffered netbuf from a previous peek/partial read */
        if (sk->lastbuf != NULL) {
            nb = sk->lastbuf;
        } else {
            err_t err = netconn_recv(sk->conn, &nb);
            if (err != ERR_OK) {
                if (err == ERR_TIMEOUT && signal_pending(current))
                    return (uint64)-EINTR;
                return (uint64)-lwip_err_to_errno(err);
            }
        }

        void *data;
        u16_t dlen;
        netbuf_data(nb, &data, &dlen);

        uint16 tocopy = (len < (int)dlen) ? (uint16)len : dlen;

        if (vm_copyout(current->vm, ubuf, data, tocopy) < 0) {
            if (sk->lastbuf == NULL)
                netbuf_delete(nb);
            return (uint64)-EFAULT;
        }

        /* Fill in source address if requested */
        if (uaddr != 0 && uaddrlen != 0) {
            const ip_addr_t *fromaddr = netbuf_fromaddr(nb);
            u16_t fromport = netbuf_fromport(nb);

            struct k_sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(fromport);
            sa.sin_addr = ip_addr_get_ip4_u32(fromaddr);

            int alen = K_SOCKADDR_IN_SIZE;
            vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen));
            vm_copyout(current->vm, uaddr, &sa, sizeof(sa));
        }

        if (flags & MSG_PEEK) {
            /* Save netbuf for next recv call to consume */
            sk->lastbuf = nb;
            sk->lastoffset = 0;
        } else {
            sk->lastbuf = NULL;
            sk->lastoffset = 0;
            netbuf_delete(nb);
        }
        ACCT_ADD(current->thread_group, net_bytes_recv, tocopy);
        return (uint64)tocopy;
    }
}

/*
 * sys_setsockopt(fd, level, optname, optval, optlen) → 0 / -errno
 */
uint64 sys_setsockopt(void)
{
    int fd, level, optname, optlen;
    uint64 uoptval;
    argint(0, &fd);
    argint(1, &level);
    argint(2, &optname);
    argaddr(3, &uoptval);
    argint(4, &optlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_setsockopt(fd, level, optname, uoptval,
                                            optlen);
    if (domain == AF_NETLINK)
        return 0; /* silently accept for netlink */

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;
    if (sk->conn == NULL || sk->conn->pcb.ip == NULL)
        return (uint64)-EINVAL;

    /* Helper: read an int option from user space */
    int val = 0;
    if (optlen >= (int)sizeof(int) && uoptval != 0) {
        if (vm_copyin(current->vm, &val, uoptval, sizeof(val)) < 0)
            return (uint64)-EFAULT;
    }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
            if (val)
                ip_set_option(sk->conn->pcb.ip, SOF_REUSEADDR);
            else
                ip_reset_option(sk->conn->pcb.ip, SOF_REUSEADDR);
            return 0;
        case SO_KEEPALIVE:
            if (sk->type == SOCK_STREAM) {
                if (val)
                    ip_set_option(sk->conn->pcb.ip, SOF_KEEPALIVE);
                else
                    ip_reset_option(sk->conn->pcb.ip, SOF_KEEPALIVE);
            }
            return 0;
        case SO_BROADCAST:
            if (val)
                ip_set_option(sk->conn->pcb.ip, SOF_BROADCAST);
            else
                ip_reset_option(sk->conn->pcb.ip, SOF_BROADCAST);
            return 0;
        case SO_RCVTIMEO:
            /* Accept both int (ms) and struct timeval */
            if (optlen >= (int)sizeof(int))
                netconn_set_recvtimeout(sk->conn, val);
            return 0;
        case SO_SNDTIMEO:
            if (optlen >= (int)sizeof(int))
                netconn_set_sendtimeout(sk->conn, val);
            return 0;
        case SO_RCVBUF:
            /* lwIP: netconn_set_recvbufsize expects bytes */
            if (val > 0)
                netconn_set_recvbufsize(sk->conn, (int)val);
            return 0;
        case SO_SNDBUF:
            /* lwIP doesn't have per-socket send buffer control for netconn.
             * Accept silently for compatibility. */
            return 0;
        case SO_LINGER: {
            if (optlen < (int)sizeof(struct k_linger))
                return (uint64)-EINVAL;
            struct k_linger lg;
            if (vm_copyin(current->vm, &lg, uoptval, sizeof(lg)) < 0)
                return (uint64)-EFAULT;
            if (lg.l_onoff) {
                int lingersec = lg.l_linger;
                if (lingersec < 0)
                    return (uint64)-EINVAL;
                if (lingersec > 0x7FFF)
                    lingersec = 0x7FFF;
                sk->conn->linger = (s16_t)lingersec;
            } else {
                sk->conn->linger = -1;
            }
            return 0;
        }
        default:
            return 0; /* silently accept unknown SOL_SOCKET options */
        }
    }

    if (level == IPPROTO_TCP) {
        if (sk->type != SOCK_STREAM || sk->conn->pcb.tcp == NULL)
            return (uint64)-EINVAL;

        struct tcp_pcb *pcb = sk->conn->pcb.tcp;
        switch (optname) {
        case TCP_NODELAY:
            if (val)
                tcp_nagle_disable(pcb);
            else
                tcp_nagle_enable(pcb);
            return 0;
#if LWIP_TCP_KEEPALIVE
        case TCP_KEEPIDLE:
            /* User passes seconds, lwIP stores milliseconds */
            pcb->keep_idle = (uint32)val * 1000;
            return 0;
        case TCP_KEEPINTVL:
            pcb->keep_intvl = (uint32)val * 1000;
            return 0;
        case TCP_KEEPCNT:
            pcb->keep_cnt = (uint32)val;
            return 0;
#endif
        default:
            return 0;
        }
    }

    if (level == IPPROTO_IP) {
        switch (optname) {
        case IP_TTL:
            sk->conn->pcb.ip->ttl = (uint8)val;
            return 0;
        case IP_TOS:
            sk->conn->pcb.ip->tos = (uint8)val;
            return 0;
#if LWIP_IGMP
        case IP_ADD_MEMBERSHIP:
        case IP_DROP_MEMBERSHIP: {
            if (optlen < (int)sizeof(struct ip_mreq))
                return (uint64)-EINVAL;
            struct ip_mreq mreq;
            if (vm_copyin(current->vm, &mreq, uoptval, sizeof(mreq)) < 0)
                return (uint64)-EFAULT;

            ip4_addr_t groupaddr, ifaddr;
            groupaddr.addr = mreq.imr_multiaddr;
            ifaddr.addr = mreq.imr_interface;

            err_t err;
            if (optname == IP_ADD_MEMBERSHIP)
                err = igmp_joingroup(&ifaddr, &groupaddr);
            else
                err = igmp_leavegroup(&ifaddr, &groupaddr);

            if (err != ERR_OK)
                return (uint64)-lwip_err_to_errno(err);
            return 0;
        }
        case IP_MULTICAST_TTL:
            if (sk->type == SOCK_DGRAM && sk->conn->pcb.udp != NULL)
                udp_set_multicast_ttl(sk->conn->pcb.udp, (uint8)val);
            return 0;
        case IP_MULTICAST_LOOP:
            if (sk->type == SOCK_DGRAM && sk->conn->pcb.udp != NULL) {
                if (val)
                    udp_set_flags(sk->conn->pcb.udp, UDP_FLAGS_MULTICAST_LOOP);
                else
                    udp_clear_flags(sk->conn->pcb.udp, UDP_FLAGS_MULTICAST_LOOP);
            }
            return 0;
        case IP_MULTICAST_IF: {
            if (optlen < (int)sizeof(uint32))
                return (uint64)-EINVAL;
            uint32 addr;
            if (vm_copyin(current->vm, &addr, uoptval, sizeof(addr)) < 0)
                return (uint64)-EFAULT;
            if (sk->type == SOCK_DGRAM && sk->conn->pcb.udp != NULL) {
                ip4_addr_t ifaddr;
                ifaddr.addr = addr;
                udp_set_multicast_netif_addr(sk->conn->pcb.udp, &ifaddr);
            }
            return 0;
        }
#endif /* LWIP_IGMP */
        default:
            return 0;
        }
    }

    if (level == IPPROTO_UDP || level == IPPROTO_ICMP) {
        /* Accept silently */
        return 0;
    }

    /* Other levels: silently accept */
    return 0;
}

/*
 * sys_getsockopt(fd, level, optname, optval, optlen_ptr) → 0 / -errno
 */
uint64 sys_getsockopt(void)
{
    int fd, level, optname;
    uint64 uoptval, uoptlen;
    argint(0, &fd);
    argint(1, &level);
    argint(2, &optname);
    argaddr(3, &uoptval);
    argaddr(4, &uoptlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_getsockopt(fd, level, optname, uoptval,
                                            uoptlen);
    if (domain == AF_NETLINK)
        return (uint64)-ENOPROTOOPT;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;
    if (sk->conn == NULL)
        return (uint64)-EINVAL;

    /* Most options return a single int */
    int val = 0;
    int olen = sizeof(int);
    int is_linger = 0;
    struct k_linger lg = {0, 0};

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_ERROR: {
            /* Read and clear pending error */
            val = lwip_err_to_errno(sk->conn->pending_err);
            sk->conn->pending_err = ERR_OK;
            sock_dbg("getsockopt fd=%d SO_ERROR=%d conn_state=%d\\n",
                     fd, val, (int)sk->conn->state);
            break;
        }
        case SO_TYPE:
            val = sk->type;
            break;
        case SO_REUSEADDR:
            val = ip_get_option(sk->conn->pcb.ip, SOF_REUSEADDR) ? 1 : 0;
            break;
        case SO_KEEPALIVE:
            val = ip_get_option(sk->conn->pcb.ip, SOF_KEEPALIVE) ? 1 : 0;
            break;
        case SO_BROADCAST:
            val = ip_get_option(sk->conn->pcb.ip, SOF_BROADCAST) ? 1 : 0;
            break;
        case SO_RCVTIMEO:
            val = netconn_get_recvtimeout(sk->conn);
            break;
        case SO_SNDTIMEO:
            val = netconn_get_sendtimeout(sk->conn);
            break;
        case SO_RCVBUF:
            val = netconn_get_recvbufsize(sk->conn);
            break;
        case SO_SNDBUF:
            /* lwIP doesn't track per-socket send buffer; return TCP_SND_BUF */
            val = (sk->type == SOCK_STREAM) ? TCP_SND_BUF : 65535;
            break;
        case SO_ACCEPTCONN:
            val = (sk->conn->state == NETCONN_LISTEN) ? 1 : 0;
            break;
        case SO_LINGER:
            is_linger = 1;
            if (sk->conn->linger >= 0) {
                lg.l_onoff = 1;
                lg.l_linger = (int)sk->conn->linger;
            } else {
                lg.l_onoff = 0;
                lg.l_linger = 0;
            }
            olen = sizeof(struct k_linger);
            break;
        default:
            val = 0; /* unknown option: return 0 */
            break;
        }
    } else if (level == IPPROTO_TCP) {
        if (sk->type != SOCK_STREAM || sk->conn->pcb.tcp == NULL) {
            val = 0;
        } else {
            struct tcp_pcb *pcb = sk->conn->pcb.tcp;
            switch (optname) {
            case TCP_NODELAY:
                val = tcp_nagle_disabled(pcb) ? 1 : 0;
                break;
#if LWIP_TCP_KEEPALIVE
            case TCP_KEEPIDLE:
                val = (int)(pcb->keep_idle / 1000); /* ms → sec */
                break;
            case TCP_KEEPINTVL:
                val = (int)(pcb->keep_intvl / 1000);
                break;
            case TCP_KEEPCNT:
                val = (int)pcb->keep_cnt;
                break;
#endif
            default:
                val = 0;
                break;
            }
        }
    } else if (level == IPPROTO_IP) {
        switch (optname) {
        case IP_OPTIONS:
            /* No IP options set — return zero-length result so callers
             * (e.g. OpenSSH check_ip_options) don't see phantom data. */
            olen = 0;
            break;
        case IP_TTL:
            val = sk->conn->pcb.ip->ttl;
            break;
        case IP_TOS:
            val = sk->conn->pcb.ip->tos;
            break;
        default:
            val = 0;
            break;
        }
    } else {
        val = 0;
    }

    /* Copy result to user space */
    if (is_linger) {
        vm_copyout(current->vm, uoptval, &lg, sizeof(lg));
    } else if (olen > 0) {
        vm_copyout(current->vm, uoptval, &val, sizeof(val));
    }
    vm_copyout(current->vm, uoptlen, &olen, sizeof(olen));
    return 0;
}

/*
 * sys_shutdown(fd, how) → 0 / -errno
 */
uint64 sys_shutdown(void)
{
    int fd, how;
    argint(0, &fd);
    argint(1, &how);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_shutdown(fd, how);
    if (domain == AF_NETLINK)
        return 0;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    u8_t shut_rx = (how == SHUT_RD || how == SHUT_RDWR) ? 1 : 0;
    u8_t shut_tx = (how == SHUT_WR || how == SHUT_RDWR) ? 1 : 0;

    err_t err = netconn_shutdown(sk->conn, shut_rx, shut_tx);
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

    return 0;
}

/*
 * sys_getpeername(fd, addr, addrlen_ptr) → 0 / -errno
 */
uint64 sys_getpeername(void)
{
    int fd;
    uint64 uaddr, uaddrlen;
    argint(0, &fd);
    argaddr(1, &uaddr);
    argaddr(2, &uaddrlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_getpeername(fd, uaddr, uaddrlen);
    if (domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    ip_addr_t raddr;
    u16_t rport;
    err_t err = netconn_peer(sk->conn, &raddr, &rport);
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

    struct k_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(rport);
    sa.sin_addr = ip_addr_get_ip4_u32(&raddr);

    int alen = K_SOCKADDR_IN_SIZE;
    vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen));
    vm_copyout(current->vm, uaddr, &sa, sizeof(sa));
    return 0;
}

/*
 * sys_getsockname(fd, addr, addrlen_ptr) → 0 / -errno
 */
uint64 sys_getsockname(void)
{
    int fd;
    uint64 uaddr, uaddrlen;
    argint(0, &fd);
    argaddr(1, &uaddr);
    argaddr(2, &uaddrlen);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_getsockname(fd, uaddr, uaddrlen);
    if (domain == AF_NETLINK)
        return (uint64)netlink_sock_getsockname(fd, uaddr, uaddrlen);

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    ip_addr_t laddr;
    u16_t lport;
    err_t err = netconn_getaddr(sk->conn, &laddr, &lport, 1 /* local */);
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

    /* Linux resolves INADDR_ANY to the actual source IP for connected
     * sockets.  lwIP doesn't do this automatically, so resolve it here
     * by routing to the remote address. */
    if (ip_addr_isany(&laddr) && sk->conn->pcb.ip != NULL) {
        ip_addr_t raddr;
        u16_t rport;
        err_t re = netconn_getaddr(sk->conn, &raddr, &rport, 0 /* remote */);
        if (re == ERR_OK && !ip_addr_isany(&raddr)) {
            const struct netif *nif = (const struct netif *)ip_route(&laddr, &raddr);
            if (nif != NULL) {
                ip_addr_copy(laddr, nif->ip_addr);
            }
        }
    }

    struct k_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(lport);
    sa.sin_addr = ip_addr_get_ip4_u32(&laddr);

    int alen = K_SOCKADDR_IN_SIZE;
    vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen));
    vm_copyout(current->vm, uaddr, &sa, sizeof(sa));
    return 0;
}

/* ========================================================================== */
/* accept4(fd, addr, addrlen, flags) → new_fd / -errno                        */
/* ========================================================================== */

/*
 * sys_accept4(fd, addr, addrlen_ptr, flags) → new_fd / -errno
 *
 * Like accept(), but atomically sets SOCK_NONBLOCK / SOCK_CLOEXEC on the
 * returned fd.
 */
uint64 sys_accept4(void)
{
    int fd, flags;
    uint64 uaddr, uaddrlen;
    argint(0, &fd);
    argaddr(1, &uaddr);
    argaddr(2, &uaddrlen);
    argint(3, &flags);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX)
        return (uint64)unix_sock_accept(fd, uaddr, uaddrlen, flags);
    if (domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    /* O_NONBLOCK on listening socket: check if connections are pending */
    if (sock_is_nonblock(fd, 0)) {
        int ready = sock_poll_ready(sk, POLLIN);
        if (!(ready & (POLLIN | POLLHUP | POLLERR)))
            return (uint64)-EAGAIN;
    }

    struct netconn *newconn = NULL;
    err_t err = netconn_accept(sk->conn, &newconn);
    if (err != ERR_OK) {
        if (err == ERR_TIMEOUT && signal_pending(current))
            return (uint64)-EINTR;
        return (uint64)-lwip_err_to_errno(err);
    }

    /* The newconn inherits the listening socket's callback from lwIP's
       accept_function(), but callback_arg.ptr is not yet set to the
       owning vfs_file.  Disable the callback now to prevent a crash
       if the tcpip thread fires it before we finish setup below. */
    newconn->callback = NULL;
    newconn->callback_arg.ptr = NULL;

    /* Allocate a new socket + fd for the accepted connection */
    int file_flags = O_RDWR;
    if (flags & SOCK_NONBLOCK)
        file_flags |= O_NONBLOCK;

    struct lwip_sock *newsk = lwip_sock_alloc();
    if (newsk == NULL) {
        netconn_delete(newconn);
        return (uint64)-ENOMEM;
    }
    newsk->conn = newconn;
    newsk->type = SOCK_STREAM;
    newsk->protocol = sk->protocol;

    int newfd = vfs_custom_fd_alloc(&lwip_socket_file_ops, newsk, file_flags);
    if (newfd < 0) {
        lwip_sock_free(newsk);
        return (uint64)newfd;
    }

    /* Wire up kqueue push notification for the accepted connection */
    struct vfs_file *newf = vfs_fdtable_get_file(current->fdtable, newfd);
    newconn->callback_arg.ptr = newf;
    newconn->callback = sock_netconn_callback;
    vfs_fput(newf);  /* drop lookup ref — fd table owns the real ref */

    /* Set close-on-exec if requested */
    if (flags & SOCK_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        vfs_fdtable_set_fdflags(current->fdtable, newfd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

    /* Fill in remote address if requested */
    if (uaddr != 0 && uaddrlen != 0) {
        ip_addr_t raddr;
        u16_t rport;
        netconn_peer(newconn, &raddr, &rport);

        struct k_sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(rport);
        sa.sin_addr = ip_addr_get_ip4_u32(&raddr);

        int alen = K_SOCKADDR_IN_SIZE;
        vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen));
        vm_copyout(current->vm, uaddr, &sa, sizeof(sa));
    }

    ACCT_INC(current->thread_group, net_accepts);
    return (uint64)newfd;
}

/* ========================================================================== */
/* sendmsg / recvmsg                                                          */
/* ========================================================================== */

/*
 * Kernel-space msghdr / iovec — must match the LP64 POSIX layout
 * that musl passes from user space.
 */
struct k_msghdr {
    uint64 msg_name;        /* void *msg_name          */
    uint32 msg_namelen;     /* socklen_t               */
    uint32 __pad0;
    uint64 msg_iov;         /* struct iovec *msg_iov   */
    int    msg_iovlen;      /* int                     */
    int    __pad1;
    uint64 msg_control;     /* void *msg_control       */
    uint32 msg_controllen;  /* socklen_t               */
    uint32 __pad2;
    int    msg_flags;       /* int                     */
};

struct k_iovec {
    uint64 iov_base;        /* void *  */
    uint64 iov_len;         /* size_t  */
};

/* Kernel-space cmsghdr — must match LP64 POSIX layout (musl) */
struct k_cmsghdr {
    uint32 cmsg_len;        /* data byte count incl. header */
    uint32 __pad1;
    int    cmsg_level;
    int    cmsg_type;
    /* followed by cmsg data */
};

#define SCM_RIGHTS       1
#define K_CMSG_ALIGN(n)  (((n) + sizeof(uint64) - 1) & ~(sizeof(uint64) - 1))
#define K_CMSG_DATA(cmsg) ((unsigned char *)((struct k_cmsghdr *)(cmsg) + 1))
#define K_CMSG_LEN(len)   (K_CMSG_ALIGN(sizeof(struct k_cmsghdr)) + (len))
#define K_CMSG_SPACE(len) (K_CMSG_ALIGN(sizeof(struct k_cmsghdr)) + K_CMSG_ALIGN(len))

#define SENDMSG_MAX_IOV 16

static size_t k_cmsg_payload_len(struct k_cmsghdr *cmsg)
{
    uint32 cmsg_len = (uint32)cmsg->cmsg_len;
    if (cmsg_len < K_CMSG_LEN(0))
        return 0;
    return cmsg_len - K_CMSG_LEN(0);
}

static size_t unix_scm_count_locked(struct unix_sock *sk)
{
    if (sk->scm_tail >= sk->scm_head)
        return sk->scm_tail - sk->scm_head;
    return UNIX_SCM_QUEUE_MAX - sk->scm_head + sk->scm_tail;
}

static size_t unix_packet_count_locked(struct unix_sock *sk)
{
    if (sk->packet_tail >= sk->packet_head)
        return sk->packet_tail - sk->packet_head;
    return UNIX_PACKET_QUEUE_MAX - sk->packet_head + sk->packet_tail;
}

static int unix_packet_enqueue_locked(struct unix_sock *sk, uint end_mark)
{
    if (sk->type != SOCK_SEQPACKET)
        return 0;
    if (unix_packet_count_locked(sk) >= UNIX_PACKET_QUEUE_MAX - 1)
        return -EAGAIN;
    sk->packet_queue[sk->packet_tail] = end_mark;
    sk->packet_tail = (sk->packet_tail + 1) % UNIX_PACKET_QUEUE_MAX;
    return 0;
}

static int unix_packet_has_space_locked(struct unix_sock *sk)
{
    return sk->type != SOCK_SEQPACKET ||
        unix_packet_count_locked(sk) < UNIX_PACKET_QUEUE_MAX - 1;
}

static int unix_packet_next_len_locked(struct unix_sock *peer, size_t *len)
{
    if (peer->packet_head == peer->packet_tail)
        return -EAGAIN;
    uint mark = peer->packet_queue[peer->packet_head];
    if ((int)(mark - peer->tx.nread) < 0)
        return -EIO;
    *len = mark - peer->tx.nread;
    return 0;
}

static void unix_packet_pop_locked(struct unix_sock *peer)
{
    if (peer->packet_head != peer->packet_tail) {
        peer->packet_queue[peer->packet_head] = 0;
        peer->packet_head = (peer->packet_head + 1) % UNIX_PACKET_QUEUE_MAX;
    }
}

static size_t unix_dequeue_scm_rights(struct unix_sock *peer,
                                      struct vfs_file **files,
                                      size_t max_files)
{
    size_t count = 0;
    if (peer == NULL || max_files == 0)
        return 0;

    spin_lock(&peer->lock);
    while (peer->scm_head != peer->scm_tail && count < max_files &&
           (int)(peer->scm_queue[peer->scm_head].mark_nread -
                 peer->tx.nread) <= 0) {
        files[count] = peer->scm_queue[peer->scm_head].file;
        peer->scm_queue[peer->scm_head].file = NULL;
        peer->scm_queue[peer->scm_head].mark_nread = 0;
        peer->scm_head = (peer->scm_head + 1) % UNIX_SCM_QUEUE_MAX;
        count++;
    }
    if (count > 0)
        tq_wakeup_all(&peer->wr_queue, 0, 0);
    spin_unlock(&peer->lock);
    return count;
}

static int unix_next_scm_barrier_locked(struct unix_sock *peer, size_t *bytes)
{
    uint mark;

    if (peer == NULL || peer->scm_head == peer->scm_tail)
        return 0;

    mark = peer->scm_queue[peer->scm_head].mark_nread;
    if ((int)(mark - peer->tx.nread) <= 0) {
        *bytes = 0;
        return 1;
    }

    *bytes = (size_t)(mark - peer->tx.nread);
    return 1;
}

static size_t unix_ring_write_locked(struct unix_ring *r, const char *buf,
                                     size_t len)
{
    size_t readable = r->nwrite - r->nread;
    size_t avail = r->capacity - readable;
    size_t towrite = len < avail ? len : avail;
    uint idx = r->nwrite % r->capacity;

    if (towrite == 0)
        return 0;
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

static size_t unix_ring_read_locked(struct unix_ring *r, char *buf, size_t len)
{
    size_t readable = r->nwrite - r->nread;
    size_t toread = len < readable ? len : readable;
    uint idx = r->nread % r->capacity;

    if (toread == 0)
        return 0;
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

static int unix_ring_capacity_for(size_t needed, size_t *capacity)
{
    if (needed > UNIX_BUF_MAX_SIZE)
        return -EMSGSIZE;

    size_t new_capacity = UNIX_BUF_DEFAULT_SIZE;
    while (new_capacity < needed) {
        if (new_capacity > UNIX_BUF_MAX_SIZE / 2) {
            new_capacity = UNIX_BUF_MAX_SIZE;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity < needed)
        return -EMSGSIZE;

    *capacity = new_capacity;
    return 0;
}

static void unix_rebase_stream_marks_locked(struct unix_sock *sk,
                                            uint old_nread)
{
    int idx;

    for (idx = sk->scm_head; idx != sk->scm_tail;
         idx = (idx + 1) % UNIX_SCM_QUEUE_MAX) {
        uint mark = sk->scm_queue[idx].mark_nread;
        sk->scm_queue[idx].mark_nread =
            (uint)((int)(mark - old_nread) < 0 ? 0 : mark - old_nread);
    }

    for (idx = sk->packet_head; idx != sk->packet_tail;
         idx = (idx + 1) % UNIX_PACKET_QUEUE_MAX) {
        uint mark = sk->packet_queue[idx];
        sk->packet_queue[idx] =
            (uint)((int)(mark - old_nread) < 0 ? 0 : mark - old_nread);
    }
}

static char *unix_ring_install_storage_locked(struct unix_sock *sk,
                                              char *new_data,
                                              size_t new_capacity)
{
    struct unix_ring *r = &sk->tx;

    if (new_capacity <= r->capacity)
        return new_data;

    char *old_data = r->data;
    size_t old_capacity = r->capacity;
    uint old_nread = r->nread;
    size_t readable = r->nwrite - r->nread;
    size_t copied = 0;

    while (copied < readable) {
        uint idx = (r->nread + copied) % old_capacity;
        size_t chunk = old_capacity - idx;
        if (chunk > readable - copied)
            chunk = readable - copied;
        memmove(new_data + copied, old_data + idx, chunk);
        copied += chunk;
    }

    r->data = new_data;
    r->capacity = new_capacity;
    r->nread = 0;
    r->nwrite = (uint)readable;
    unix_rebase_stream_marks_locked(sk, old_nread);
    return old_data;
}

static int unix_sendmsg_growth_needed_locked(struct unix_sock *sk, size_t len,
                                             size_t *capacity)
{
    if (len > UNIX_BUF_MAX_SIZE)
        return -EMSGSIZE;

    size_t readable = sk->tx.nwrite - sk->tx.nread;
    if (readable > UNIX_BUF_MAX_SIZE || len > UNIX_BUF_MAX_SIZE - readable)
        return -EAGAIN;

    size_t needed = readable + len;
    if (needed <= sk->tx.capacity)
        return 0;
    return unix_ring_capacity_for(needed, capacity);
}

static int unix_sendmsg_atomic_locked(struct unix_sock *sk, const char *buf,
                                      size_t len,
                                      struct vfs_file **scm_files,
                                      size_t scm_count)
{
    if (sk->shutdown_flags & UNIX_SHUT_WR)
        return -EPIPE;
    if (sk->state != UNIX_STATE_CONNECTED && sk->type == SOCK_STREAM)
        return -ENOTCONN;
    if (sk->peer == NULL)
        return -EPIPE;
    size_t readable = sk->tx.nwrite - sk->tx.nread;
    size_t writable = sk->tx.capacity - readable;
    if (writable < len)
        return -EAGAIN;

    size_t scm_used = unix_scm_count_locked(sk);
    size_t scm_free = UNIX_SCM_QUEUE_MAX - 1 - scm_used;
    if (scm_free < scm_count)
        return -EAGAIN;
    if (!unix_packet_has_space_locked(sk))
        return -EAGAIN;

    uint scm_mark = sk->tx.nwrite;
    size_t wrote = unix_ring_write_locked(&sk->tx, buf, len);
    if (wrote != len)
        panic("unix_sendmsg_atomic_locked: short atomic write");
    uint mark = sk->tx.nwrite;
    if (scm_count > 0 && len > 0)
        scm_mark++;
    for (size_t i = 0; i < scm_count; i++) {
        sk->scm_queue[sk->scm_tail].file = scm_files[i];
        sk->scm_queue[sk->scm_tail].mark_nread = scm_mark;
        sk->scm_tail = (sk->scm_tail + 1) % UNIX_SCM_QUEUE_MAX;
    }
    int pkt_ret = unix_packet_enqueue_locked(sk, mark);
    if (pkt_ret < 0)
        panic("unix_sendmsg_atomic_locked: packet queue lost reservation");
    return 0;
}

/*
 * sys_sendmsg(fd, msg, flags) → nbytes / -errno
 *
 * Gather-write to a socket.  For TCP, writes each iov sequentially.
 * For UDP, concatenates all iovecs into a single datagram.
 */
uint64 sys_sendmsg(void)
{
    int fd, flags;
    uint64 umsg;
    argint(0, &fd);
    argaddr(1, &umsg);
    argint(2, &flags);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

    if (domain == AF_UNIX) {
        /* ---- AF_UNIX sendmsg with optional SCM_RIGHTS ---- */
        struct unix_sock *sk = unix_sock_from_fd(fd);
        if (sk == NULL)
            return (uint64)-EBADF;

        struct k_msghdr mh;
        if (vm_copyin(current->vm, &mh, umsg, sizeof(mh)) < 0)
            return (uint64)-EFAULT;

        if (mh.msg_iovlen == 0)
            return 0;
        if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
            return (uint64)-EMSGSIZE;

        struct k_iovec iovs[SENDMSG_MAX_IOV];
        uint64 iov_bytes = mh.msg_iovlen * sizeof(struct k_iovec);
        if (vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
            return (uint64)-EFAULT;

        size_t total_len = 0;
        for (uint64 i = 0; i < mh.msg_iovlen; i++)
            total_len += iovs[i].iov_len;

        if (total_len > UNIX_BUF_MAX_SIZE)
            return (uint64)-EMSGSIZE;

        char *msg_buf = kvmalloc(total_len ? total_len : 1);
        if (msg_buf == NULL)
            return (uint64)-ENOMEM;
        size_t off = 0;
        for (uint64 i = 0; i < mh.msg_iovlen; i++) {
            if (iovs[i].iov_len == 0)
                continue;
            if (vm_copyin(current->vm, msg_buf + off, iovs[i].iov_base,
                          iovs[i].iov_len) < 0) {
                kvfree(msg_buf);
                return (uint64)-EFAULT;
            }
            off += iovs[i].iov_len;
        }

        struct vfs_file *scm_files[UNIX_SCM_QUEUE_MAX];
        memset(scm_files, 0, sizeof(scm_files));
        size_t scm_count = 0;

        if (mh.msg_control != 0 && mh.msg_controllen >= K_CMSG_LEN(sizeof(int))) {
            unsigned char cmsg_buf[K_CMSG_SPACE(sizeof(int) * UNIX_SCM_QUEUE_MAX)];
            uint64 copy_len = mh.msg_controllen;
            if (copy_len > sizeof(cmsg_buf))
                copy_len = sizeof(cmsg_buf);
            if (vm_copyin(current->vm, cmsg_buf, mh.msg_control, copy_len) < 0) {
                kvfree(msg_buf);
                return (uint64)-EFAULT;
            }

            struct k_cmsghdr *cmsg = (struct k_cmsghdr *)cmsg_buf;
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                (uint32)cmsg->cmsg_len >= K_CMSG_LEN(sizeof(int))) {
                size_t payload_len = k_cmsg_payload_len(cmsg);
                size_t nfds = payload_len / sizeof(int);
                size_t max_fds = (copy_len >= K_CMSG_LEN(0))
                    ? (copy_len - K_CMSG_LEN(0)) / sizeof(int) : 0;
                if (nfds > max_fds)
                    nfds = max_fds;
                if (nfds >= UNIX_SCM_QUEUE_MAX) {
                    kvfree(msg_buf);
                    return (uint64)-EMSGSIZE;
                }
                int *pass_fds = (int *)K_CMSG_DATA(cmsg);
                for (size_t i = 0; i < nfds; i++) {
                    struct vfs_file *file =
                        vfs_fdtable_get_file(current->fdtable, pass_fds[i]);
                    if (file == NULL) {
                        for (size_t j = 0; j < scm_count; j++)
                            vfs_fput(scm_files[j]);
                        kvfree(msg_buf);
                        return (uint64)-EBADF;
                    }

                    scm_files[scm_count] = vfs_fdup(file);
                    vfs_fput(file);
                    if (scm_files[scm_count] == NULL) {
                        for (size_t j = 0; j < scm_count; j++)
                            vfs_fput(scm_files[j]);
                        kvfree(msg_buf);
                        return (uint64)-ENOMEM;
                    }
                    scm_count++;
                }
            }
        }

        int ret;
        int nonblock = sock_is_nonblock(fd, flags);
        struct vfs_file *notify_file = NULL;
        char *grow_data = NULL;
        size_t grow_capacity = 0;
        for (;;) {
            spin_lock(&sk->lock);
            char *old_data = NULL;
            if (grow_data != NULL) {
                old_data = unix_ring_install_storage_locked(sk,
                                                            grow_data,
                                                            grow_capacity);
                grow_data = NULL;
                grow_capacity = 0;
            }
            size_t needed_capacity = 0;
            ret = unix_sendmsg_growth_needed_locked(sk, total_len,
                                                    &needed_capacity);
            if (old_data != NULL) {
                spin_unlock(&sk->lock);
                kvfree(old_data);
                continue;
            }
            if (ret < 0) {
                spin_unlock(&sk->lock);
                break;
            }

            if (needed_capacity != 0) {
                spin_unlock(&sk->lock);
                grow_data = kvmalloc(needed_capacity);
                if (grow_data == NULL) {
                    ret = -ENOMEM;
                    break;
                }
                grow_capacity = needed_capacity;
                continue;
            }

            ret = unix_sendmsg_atomic_locked(sk, msg_buf, total_len,
                                             scm_files, scm_count);
            if (ret == 0 && sk->peer != NULL) {
                tq_wakeup_all(&sk->peer->rd_queue, 0, 0);
                if (sk->peer->file != NULL)
                    notify_file = vfs_fdup(sk->peer->file);
            }
            if (ret == 0 || ret != -EAGAIN || nonblock) {
                spin_unlock(&sk->lock);
                break;
            }
            if (signal_pending(current)) {
                ret = -EINTR;
                spin_unlock(&sk->lock);
                break;
            }
            tq_wait_in_state(&sk->wr_queue, &sk->lock, NULL,
                             THREAD_INTERRUPTIBLE);
            spin_unlock(&sk->lock);

            if (signal_pending(current)) {
                ret = -EINTR;
                break;
            }
        }

        if (grow_data != NULL)
            kvfree(grow_data);
        kvfree(msg_buf);
        if (ret < 0) {
            for (size_t i = 0; i < scm_count; i++)
                vfs_fput(scm_files[i]);
            if (notify_file != NULL)
                vfs_fput(notify_file);
            return (uint64)ret;
        }

        if (notify_file != NULL) {
            vfs_file_knote_notify(notify_file, EVFILT_READ, 0);
            vfs_fput(notify_file);
        }
        return (uint64)total_len;
    }

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    /* MSG_DONTWAIT / O_NONBLOCK: check send readiness */
    if (sock_is_nonblock(fd, flags)) {
        int ready = sock_poll_ready(sk, POLLOUT);
        if (!(ready & POLLOUT))
            return (uint64)-EAGAIN;
    }

    /* Copy the msghdr from user space */
    struct k_msghdr mh;
    if (vm_copyin(current->vm, &mh, umsg, sizeof(mh)) < 0)
        return (uint64)-EFAULT;

    if (mh.msg_iovlen == 0)
        return 0;
    if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
        return (uint64)-EMSGSIZE;

    /* Copy iovec array from user space */
    struct k_iovec iovs[SENDMSG_MAX_IOV];
    uint64 iov_bytes = mh.msg_iovlen * sizeof(struct k_iovec);
    if (vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
        return (uint64)-EFAULT;

    /* Set destination address if provided (UDP with msg_name) */
    if (mh.msg_name != 0 && mh.msg_namelen >= K_SOCKADDR_IN_SIZE &&
        sk->type != SOCK_STREAM) {
        struct k_sockaddr_in sa;
        if (vm_copyin(current->vm, &sa, mh.msg_name, sizeof(sa)) < 0)
            return (uint64)-EFAULT;
        if (sa.sin_family != AF_INET)
            return (uint64)-EAFNOSUPPORT;

        ip_addr_t destip;
        ip_addr_set_ip4_u32(&destip, sa.sin_addr);
        err_t cerr = netconn_connect(sk->conn, &destip, ntohs(sa.sin_port));
        if (cerr != ERR_OK && cerr != ERR_ISCONN)
            return (uint64)-lwip_err_to_errno(cerr);
    }

    ssize_t total = 0;

    if (sk->type == SOCK_STREAM) {
        /* TCP: write each iov sequentially via netconn_write_partly (NB-safe) */
        char tmpbuf[1500];
        for (uint64 i = 0; i < mh.msg_iovlen; i++) {
            uint64 base = iovs[i].iov_base;
            size_t ilen = iovs[i].iov_len;
            size_t sent = 0;
            while (sent < ilen) {
                size_t chunk = ilen - sent;
                if (chunk > sizeof(tmpbuf))
                    chunk = sizeof(tmpbuf);
                if (vm_copyin(current->vm, tmpbuf, base + sent, chunk) < 0)
                    return total > 0 ? (uint64)total : (uint64)-EFAULT;
                size_t chunk_written = 0;
                err_t err = netconn_write_partly(sk->conn, tmpbuf, chunk,
                                                 NETCONN_COPY, &chunk_written);
                if (err != ERR_OK) {
                    if (err == ERR_WOULDBLOCK)
                        return total > 0 ? (uint64)total : (uint64)-EAGAIN;
                    return total > 0 ? (uint64)total
                                     : (uint64)-lwip_err_to_errno(err);
                }
                sent += chunk_written;
                total += (ssize_t)chunk_written;
                if (chunk_written < chunk)
                    goto sendmsg_done;  /* partial — return what we have */
            }
        }
sendmsg_done:
        ;   /* partial write — fall through to return total */
    } else {
        /* UDP: gather all iovecs into a single netbuf datagram */
        size_t total_len = 0;
        for (uint64 i = 0; i < mh.msg_iovlen; i++)
            total_len += iovs[i].iov_len;
        if (total_len > 65535)
            return (uint64)-EMSGSIZE;

        struct netbuf *nb = netbuf_new();
        if (nb == NULL)
            return (uint64)-ENOMEM;
        void *data = netbuf_alloc(nb, (uint16)total_len);
        if (data == NULL) {
            netbuf_delete(nb);
            return (uint64)-ENOMEM;
        }

        size_t off = 0;
        for (uint64 i = 0; i < mh.msg_iovlen; i++) {
            if (iovs[i].iov_len == 0)
                continue;
            if (vm_copyin(current->vm, (char *)data + off,
                          iovs[i].iov_base, iovs[i].iov_len) < 0) {
                netbuf_delete(nb);
                return (uint64)-EFAULT;
            }
            off += iovs[i].iov_len;
        }

        err_t err = netconn_send(sk->conn, nb);
        netbuf_delete(nb);
        if (err != ERR_OK)
            return (uint64)-lwip_err_to_errno(err);
        total = (ssize_t)total_len;
    }

    if (total > 0)
        ACCT_ADD(current->thread_group, net_bytes_sent, (uint64)total);
    return (uint64)total;
}

/*
 * sys_recvmsg(fd, msg, flags) → nbytes / -errno
 *
 * Scatter-read from a socket.  Sets msg_flags with MSG_TRUNC if the
 * datagram was larger than the supplied buffer (critical for DNS).
 */
uint64 sys_recvmsg(void)
{
    int fd, flags;
    uint64 umsg;
    argint(0, &fd);
    argaddr(1, &umsg);
    argint(2, &flags);

    /* Domain dispatch */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

    if (domain == AF_UNIX) {
        /* ---- AF_UNIX recvmsg with optional SCM_RIGHTS ---- */
        struct unix_sock *sk = unix_sock_from_fd(fd);
        if (sk == NULL)
            return (uint64)-EBADF;

        struct k_msghdr mh;
        if (vm_copyin(current->vm, &mh, umsg, sizeof(mh)) < 0)
            return (uint64)-EFAULT;

        if (mh.msg_iovlen == 0)
            return 0;
        if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
            return (uint64)-EINVAL;

        struct k_iovec iovs[SENDMSG_MAX_IOV];
        uint64 iov_bytes = mh.msg_iovlen * sizeof(struct k_iovec);
        if (vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
            return (uint64)-EFAULT;

        /* Read data through the normal unix socket read path */
        struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
        if (f == NULL)
            return (uint64)-EBADF;

        /* Honor MSG_DONTWAIT: temporarily set O_NONBLOCK */
        int saved_fflags = f->f_flags;
        if (flags & MSG_DONTWAIT)
            f->f_flags |= O_NONBLOCK;

        ssize_t total = 0;
        int unix_out_flags = 0;
        if (sk->type == SOCK_SEQPACKET) {
            struct unix_sock *peer = NULL;
            size_t packet_len = 0;

            for (;;) {
                bool read_shutdown;
                bool peer_write_shutdown = false;
                int pkt_ret = -EAGAIN;

                spin_lock(&sk->lock);
                peer = sk->peer;
                unix_sock_get_ref(peer);
                read_shutdown = (sk->shutdown_flags & UNIX_SHUT_RD) != 0;
                spin_unlock(&sk->lock);

                if (peer == NULL) {
                    unix_sock_put_ref(peer);
                    goto unix_recvmsg_done;
                }

                spin_lock(&peer->lock);
                pkt_ret = unix_packet_next_len_locked(peer, &packet_len);
                peer_write_shutdown =
                    (peer->shutdown_flags & UNIX_SHUT_WR) != 0;
                spin_unlock(&peer->lock);
                if (pkt_ret == 0)
                    break;

                if (read_shutdown || peer_write_shutdown) {
                    unix_sock_put_ref(peer);
                    goto unix_recvmsg_done;
                }

                if (saved_fflags & O_NONBLOCK) {
                    unix_sock_put_ref(peer);
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    return (uint64)-EAGAIN;
                }
                if (signal_pending(current)) {
                    unix_sock_put_ref(peer);
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    return (uint64)-EINTR;
                }

                unix_sock_put_ref(peer);
                peer = NULL;
                spin_lock(&sk->lock);
                tq_wait_in_state(&sk->rd_queue, &sk->lock, NULL,
                                 THREAD_INTERRUPTIBLE);
                spin_unlock(&sk->lock);

                if (signal_pending(current)) {
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    return (uint64)-EINTR;
                }
            }

            size_t capacity = 0;
            for (uint64 i = 0; i < mh.msg_iovlen; i++)
                capacity += iovs[i].iov_len;
            size_t to_copy = packet_len < capacity ? packet_len : capacity;
            if (packet_len > capacity) {
                unix_out_flags |= MSG_TRUNC;
                printf("unix_recvmsg: seqpacket trunc pid=%d fd=%d packet=%lu capacity=%lu\n",
                       current->pid, fd, packet_len, capacity);
            }

            char tmp[128];
            size_t copied_packet = 0;
            for (uint64 i = 0; i < mh.msg_iovlen && copied_packet < to_copy; i++) {
                size_t copied_iov = 0;
                while (copied_iov < iovs[i].iov_len &&
                       copied_packet < to_copy) {
                    size_t want = iovs[i].iov_len - copied_iov;
                    if (want > to_copy - copied_packet)
                        want = to_copy - copied_packet;
                    if (want > sizeof(tmp))
                        want = sizeof(tmp);

                    spin_lock(&peer->lock);
                    size_t got;
                    if (flags & MSG_PEEK)
                        got = unix_ring_peek(&peer->tx, copied_packet,
                                             tmp, want);
                    else
                        got = unix_ring_read_locked(&peer->tx, tmp, want);
                    spin_unlock(&peer->lock);

                    if (got == 0) {
                        printf("unix_recvmsg: seqpacket short pid=%d fd=%d packet=%lu copied=%lu want=%lu\n",
                               current->pid, fd, packet_len, copied_packet,
                               to_copy);
                        unix_sock_put_ref(peer);
                        goto unix_recvmsg_done;
                    }

                    if (vm_copyout(current->vm,
                                   iovs[i].iov_base + copied_iov,
                                   tmp, got) < 0) {
                        unix_sock_put_ref(peer);
                        f->f_flags = saved_fflags;
                        vfs_fput(f);
                        return total > 0 ? (uint64)total : (uint64)-EFAULT;
                    }

                    copied_iov += got;
                    copied_packet += got;
                    total += got;
                }
            }

            if (!(flags & MSG_PEEK)) {
                spin_lock(&peer->lock);
                if (packet_len > copied_packet) {
                    uint mark = peer->packet_queue[peer->packet_head];
                    smp_store_release(&peer->tx.nread, mark);
                }
                unix_packet_pop_locked(peer);
                tq_wakeup_all(&peer->wr_queue, 0, 0);
                spin_unlock(&peer->lock);
            }
            unix_sock_put_ref(peer);
        } else if (flags & MSG_PEEK) {
            if (sk->state != UNIX_STATE_CONNECTED && sk->type == SOCK_STREAM) {
                f->f_flags = saved_fflags;
                vfs_fput(f);
                if (sk->shutdown_flags & UNIX_SHUT_RD)
                    return 0;
                return (uint64)-ENOTCONN;
            }

            size_t peek_off = 0;
            char tmp[128];

            for (uint64 i = 0; i < mh.msg_iovlen; i++) {
                size_t copied = 0;

                while (copied < iovs[i].iov_len) {
                    struct unix_sock *peer;
                    size_t want = iovs[i].iov_len - copied;
                    if (want > sizeof(tmp))
                        want = sizeof(tmp);

                    spin_lock(&sk->lock);
                    peer = sk->peer;
                    unix_sock_get_ref(peer);
                    spin_unlock(&sk->lock);

                    size_t got = 0;
                    if (peer != NULL) {
                        spin_lock(&peer->lock);
                        got = unix_ring_peek(&peer->tx, peek_off, tmp, want);
                        spin_unlock(&peer->lock);
                    }

                    if (got == 0) {
                        if (total > 0) {
                            unix_sock_put_ref(peer);
                            goto unix_recvmsg_done;
                        }

                        unix_sock_put_ref(peer);
                        peer = NULL;
                        spin_lock(&sk->lock);
                        peer = sk->peer;
                        unix_sock_get_ref(peer);
                        bool eof = (peer == NULL)
                            || (sk->shutdown_flags & UNIX_SHUT_RD)
                            || (peer != NULL && (peer->shutdown_flags & UNIX_SHUT_WR));
                        spin_unlock(&sk->lock);

                        if (eof) {
                            unix_sock_put_ref(peer);
                            goto unix_recvmsg_done;
                        }
                        if (saved_fflags & O_NONBLOCK) {
                            unix_sock_put_ref(peer);
                            f->f_flags = saved_fflags;
                            vfs_fput(f);
                            return (uint64)-EAGAIN;
                        }
                        if (signal_pending(current)) {
                            unix_sock_put_ref(peer);
                            f->f_flags = saved_fflags;
                            vfs_fput(f);
                            return (uint64)-EINTR;
                        }
                        unix_sock_put_ref(peer);

                        spin_lock(&sk->lock);
                        tq_wait_in_state(&sk->rd_queue, &sk->lock, NULL,
                                         THREAD_INTERRUPTIBLE);
                        spin_unlock(&sk->lock);

                        if (signal_pending(current)) {
                            f->f_flags = saved_fflags;
                            vfs_fput(f);
                            return (uint64)-EINTR;
                        }
                        continue;
                    }

                    if (vm_copyout(current->vm,
                                   iovs[i].iov_base + copied,
                                   tmp, got) < 0) {
                        unix_sock_put_ref(peer);
                        f->f_flags = saved_fflags;
                        vfs_fput(f);
                        return total > 0 ? (uint64)total : (uint64)-EFAULT;
                    }

                    copied += got;
                    peek_off += got;
                    total += got;
                    unix_sock_put_ref(peer);

                    if (got < want)
                        break;
                }
            }
        } else {
            int hit_scm_barrier = 0;
            for (uint64 i = 0; i < mh.msg_iovlen; i++) {
                size_t copied_iov = 0;
                while (copied_iov < iovs[i].iov_len) {
                    struct unix_sock *peer;
                    size_t want = iovs[i].iov_len - copied_iov;
                    size_t barrier_bytes = 0;

                    spin_lock(&sk->lock);
                    peer = sk->peer;
                    unix_sock_get_ref(peer);
                    spin_unlock(&sk->lock);

                    if (peer != NULL) {
                        spin_lock(&peer->lock);
                        if (unix_next_scm_barrier_locked(peer,
                                                         &barrier_bytes)) {
                            if (barrier_bytes == 0)
                                hit_scm_barrier = 1;
                            else if (want > barrier_bytes)
                                want = barrier_bytes;
                        }
                        spin_unlock(&peer->lock);
                    }
                    unix_sock_put_ref(peer);

                    if (want == 0 || hit_scm_barrier)
                        break;

                    ssize_t n = unix_socket_file_ops.read(
                        f, (char *)(iovs[i].iov_base + copied_iov), want, 1);
                    if (n < 0) {
                        f->f_flags = saved_fflags;
                        vfs_fput(f);
                        return total > 0 ? (uint64)total : (uint64)n;
                    }
                    total += n;
                    copied_iov += (size_t)n;
                    if ((size_t)n < want)
                        break;
                    if (barrier_bytes != 0 && (size_t)n == barrier_bytes) {
                        hit_scm_barrier = 1;
                        break;
                    }
                }
                if (hit_scm_barrier)
                    break;
            }
        }

unix_recvmsg_done:
        f->f_flags = saved_fflags;
        vfs_fput(f);

        /* Check for pending SCM_RIGHTS file from peer */
        if (mh.msg_control != 0 &&
            mh.msg_controllen >= K_CMSG_LEN(sizeof(int))) {
            if (flags & MSG_PEEK) {
                uint32 zero = 0;
                vm_copyout(current->vm,
                           umsg + __builtin_offsetof(struct k_msghdr, msg_controllen),
                           &zero, sizeof(zero));
            } else {
            struct unix_sock *peer = NULL;
            spin_lock(&sk->lock);
            peer = sk->peer;
            unix_sock_get_ref(peer);
            spin_unlock(&sk->lock);

            size_t max_fds = (mh.msg_controllen >= K_CMSG_SPACE(0))
                ? (mh.msg_controllen - K_CMSG_SPACE(0)) / sizeof(int) : 0;
            if (max_fds > UNIX_SCM_QUEUE_MAX)
                max_fds = UNIX_SCM_QUEUE_MAX;

            struct vfs_file *scm_files[UNIX_SCM_QUEUE_MAX];
            memset(scm_files, 0, sizeof(scm_files));
            size_t scm_count = unix_dequeue_scm_rights(peer, scm_files, max_fds);
            unix_sock_put_ref(peer);

            if (scm_count > 0) {
                int newfds[UNIX_SCM_QUEUE_MAX];
                size_t installed = 0;

                for (; installed < scm_count; installed++) {
                    spin_lock(&current->fdtable->lock);
                    int newfd = vfs_fdtable_alloc_fd(current->fdtable,
                                                     scm_files[installed]);
                    if (newfd >= 0 && (flags & MSG_CMSG_CLOEXEC))
                        vfs_fdtable_set_fdflags(current->fdtable, newfd,
                                                FD_CLOEXEC);
                    spin_unlock(&current->fdtable->lock);
                    vfs_fput(scm_files[installed]);
                    if (newfd < 0)
                        break;
                    newfds[installed] = newfd;
                }

                for (size_t i = installed + 1; i < scm_count; i++)
                    vfs_fput(scm_files[i]);

                if (installed > 0) {
                    unsigned char cmsg_buf[K_CMSG_SPACE(sizeof(int) * UNIX_SCM_QUEUE_MAX)];
                    memset(cmsg_buf, 0, sizeof(cmsg_buf));
                    struct k_cmsghdr *cmsg = (struct k_cmsghdr *)cmsg_buf;
                    cmsg->cmsg_len = K_CMSG_LEN(sizeof(int) * installed);
                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SCM_RIGHTS;
                    memmove(K_CMSG_DATA(cmsg), newfds, sizeof(int) * installed);

                    size_t clen = K_CMSG_SPACE(sizeof(int) * installed);
                    vm_copyout(current->vm, mh.msg_control, cmsg_buf, clen);

                    uint32 uclen = (uint32)clen;
                    vm_copyout(current->vm,
                               umsg + __builtin_offsetof(struct k_msghdr, msg_controllen),
                               &uclen, sizeof(uclen));
                } else {
                    uint32 zero = 0;
                    vm_copyout(current->vm,
                               umsg + __builtin_offsetof(struct k_msghdr, msg_controllen),
                               &zero, sizeof(zero));
                }
            } else {
                /* No pending fd — set msg_controllen = 0 */
                uint32 zero = 0;
                vm_copyout(current->vm,
                           umsg + __builtin_offsetof(struct k_msghdr, msg_controllen),
                           &zero, sizeof(zero));
            }
            }
        }

        /* Clear msg_flags */
        vm_copyout(current->vm,
                   umsg + __builtin_offsetof(struct k_msghdr, msg_flags),
                   &unix_out_flags, sizeof(unix_out_flags));

        return (uint64)total;
    }

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    /* MSG_DONTWAIT / O_NONBLOCK: check recv readiness */
    if (sock_is_nonblock(fd, flags)) {
        int ready = sock_poll_ready(sk, POLLIN);
        if (!(ready & (POLLIN | POLLHUP | POLLERR)))
            return (uint64)-EAGAIN;
    }

    /* Copy the msghdr from user space */
    struct k_msghdr mh;
    if (vm_copyin(current->vm, &mh, umsg, sizeof(mh)) < 0)
        return (uint64)-EFAULT;

    if (mh.msg_iovlen == 0)
        return 0;
    if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
        return (uint64)-EINVAL;

    /* Copy iovec array */
    struct k_iovec iovs[SENDMSG_MAX_IOV];
    uint64 iov_bytes = mh.msg_iovlen * sizeof(struct k_iovec);
    if (vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
        return (uint64)-EFAULT;

    /* Compute total buffer capacity */
    size_t buf_total = 0;
    for (uint64 i = 0; i < mh.msg_iovlen; i++)
        buf_total += iovs[i].iov_len;

    int out_flags = 0;
    ssize_t total = 0;

    if (sk->type == SOCK_STREAM) {
        /*
         * TCP: drain available pbufs into the scatter list.  Returning a
         * single MSS-sized pbuf for every recvmsg() makes large HTTPS
         * streams depend on a tight readiness/read loop and can leave user
         * space asleep with the TCP window unadvertised.
         */
        uint64 iov_idx = 0;
        size_t iov_off = 0;
        int nonblock = sock_is_nonblock(fd, flags);
        int wait_more = 0;
        char tmpbuf[1500];

        while ((size_t)total < buf_total && iov_idx < mh.msg_iovlen) {
            struct pbuf *p = NULL;
            uint16 poff = 0;
            uint16 start_off = 0;
            bool reused_lastpbuf = false;
            size_t copied_from_pbuf = 0;

            if (sk->lastpbuf != NULL) {
                p = sk->lastpbuf;
                poff = sk->lastpbuf_off;
                reused_lastpbuf = true;
            } else {
                err_t err = sock_tcp_recv_pbuf(sk, &p, nonblock || total > 0);
                if (err != ERR_OK) {
                    if (err == ERR_CLSD)
                        return total > 0 ? (uint64)total : 0;
                    if (err == ERR_TIMEOUT && signal_pending(current))
                        return total > 0 ? (uint64)total : (uint64)-EINTR;
                    if (total > 0 && (err == ERR_WOULDBLOCK || err == ERR_TIMEOUT)) {
                        if (sock_tcp_wait_for_more(sk, nonblock, total,
                                                   buf_total, &wait_more))
                            continue;
                        break;
                    }
                    return (uint64)-lwip_err_to_errno(err);
                }
            }

            start_off = poff;
            while (poff < p->tot_len && iov_idx < mh.msg_iovlen) {
                if (iovs[iov_idx].iov_len == 0 ||
                    iov_off >= iovs[iov_idx].iov_len) {
                    iov_idx++;
                    iov_off = 0;
                    continue;
                }

                size_t want = iovs[iov_idx].iov_len - iov_off;
                uint16 remain = p->tot_len - poff;
                size_t chunk = want < remain ? want : (size_t)remain;
                if (chunk > sizeof(tmpbuf))
                    chunk = sizeof(tmpbuf);

                uint16 got = pbuf_copy_partial(p, tmpbuf, (uint16)chunk, poff);
                if (got == 0)
                    break;

                if (vm_copyout(current->vm,
                               iovs[iov_idx].iov_base + iov_off,
                               tmpbuf, got) < 0) {
                    if (!(flags & MSG_PEEK) && copied_from_pbuf > 0) {
                        sk->lastpbuf = p;
                        sk->lastpbuf_off = poff;
                    } else if (!reused_lastpbuf) {
                        pbuf_free(p);
                    }
                    if (!(flags & MSG_PEEK) && copied_from_pbuf > 0)
                        sock_tcp_recvd(sk, copied_from_pbuf);
                    return total > 0 ? (uint64)total : (uint64)-EFAULT;
                }

                poff += got;
                iov_off += got;
                total += got;
                copied_from_pbuf += got;
            }

            if (flags & MSG_PEEK) {
                sk->lastpbuf = p;
                sk->lastpbuf_off = start_off;
                break;
            } else if (poff < p->tot_len) {
                sk->lastpbuf = p;
                sk->lastpbuf_off = poff;
            } else {
                pbuf_free(p);
                sk->lastpbuf = NULL;
                sk->lastpbuf_off = 0;
            }

            sock_tcp_recvd(sk, copied_from_pbuf);
            if (copied_from_pbuf == 0)
                break;
        }
    } else {
        /* UDP/RAW: receive one datagram, scatter into iovecs */
        struct netbuf *nb = NULL;
        err_t err = netconn_recv(sk->conn, &nb);
        if (err != ERR_OK) {
            if (err == ERR_TIMEOUT && signal_pending(current))
                return (uint64)-EINTR;
            return (uint64)-lwip_err_to_errno(err);
        }

        void *data;
        u16_t dlen;
        netbuf_data(nb, &data, &dlen);

        /* Scatter data into iovecs */
        size_t doff = 0;
        for (uint64 i = 0; i < mh.msg_iovlen && doff < dlen; i++) {
            size_t want = iovs[i].iov_len;
            size_t remain = (size_t)(dlen - doff);
            size_t tocopy = (want < remain) ? want : remain;
            if (tocopy > 0) {
                if (vm_copyout(current->vm, iovs[i].iov_base,
                               (char *)data + doff, tocopy) < 0) {
                    netbuf_delete(nb);
                    return total > 0 ? (uint64)total : (uint64)-EFAULT;
                }
                doff += tocopy;
                total += (ssize_t)tocopy;
            }
        }

        /* MSG_TRUNC: datagram was larger than supplied buffer */
        if ((size_t)dlen > buf_total)
            out_flags |= MSG_TRUNC;

        /* Copy source address into msg_name if requested */
        if (mh.msg_name != 0 && mh.msg_namelen >= K_SOCKADDR_IN_SIZE) {
            const ip_addr_t *fromaddr = netbuf_fromaddr(nb);
            u16_t fromport = netbuf_fromport(nb);

            struct k_sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(fromport);
            sa.sin_addr = ip_addr_get_ip4_u32(fromaddr);

            vm_copyout(current->vm, mh.msg_name, &sa, sizeof(sa));
            /* Update msg_namelen in user msghdr */
            uint32 nlen = K_SOCKADDR_IN_SIZE;
            vm_copyout(current->vm,
                        umsg + __builtin_offsetof(struct k_msghdr, msg_namelen),
                        &nlen, sizeof(nlen));
        }

        netbuf_delete(nb);
    }

    /* Write back msg_flags to user space */
    vm_copyout(current->vm,
                umsg + __builtin_offsetof(struct k_msghdr, msg_flags),
                &out_flags, sizeof(out_flags));

    /* Clear msg_controllen (no ancillary data support) */
    uint32 zero = 0;
    vm_copyout(current->vm,
                umsg + __builtin_offsetof(struct k_msghdr, msg_controllen),
                &zero, sizeof(zero));

    if (total > 0)
        ACCT_ADD(current->thread_group, net_bytes_recv, (uint64)total);
    if (sk->type == SOCK_STREAM && !(flags & MSG_PEEK) && total > 0)
        sock_notify_if_still_readable(fd, sk);
    return (uint64)total;
}

/*
 * sys_socketpair(domain, type, protocol, sv[2]) → 0 / -errno
 *
 * Only AF_UNIX is supported for socketpair.
 */
uint64 sys_socketpair(void)
{
    int domain, type, protocol;
    uint64 usv;
    argint(0, &domain);
    argint(1, &type);
    argint(2, &protocol);
    argaddr(3, &usv);

    /* Strip flags from type */
    int flags = type & ~SOCK_TYPE_MASK;
    type &= SOCK_TYPE_MASK;

    if (domain != AF_UNIX)
        return (uint64)-EAFNOSUPPORT;

    int file_flags = O_RDWR;
    if (flags & SOCK_NONBLOCK)
        file_flags |= O_NONBLOCK;

    int sv[2];
    int ret = unix_sock_socketpair(type, protocol, file_flags, sv);
    if (ret < 0)
        return (uint64)ret;

    if (flags & SOCK_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        vfs_fdtable_set_fdflags(current->fdtable, sv[0], FD_CLOEXEC);
        vfs_fdtable_set_fdflags(current->fdtable, sv[1], FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

    /* Copy sv[] to user space */
    if (vm_copyout(current->vm, usv, sv, sizeof(sv)) < 0)
        return (uint64)-EFAULT;

    return 0;
}

/*
 * sys_sendmmsg(sockfd, msgvec, vlen, flags) → count / -errno
 *
 * Send multiple messages on a socket in a single syscall.
 * msgvec is an array of struct mmsghdr:
 *   struct mmsghdr { struct msghdr msg_hdr; unsigned int msg_len; };
 */
uint64 sys_sendmmsg(void)
{
    int sockfd, vlen, flags;
    uint64 umsgvec;
    argint(0, &sockfd);
    argaddr(1, &umsgvec);
    argint(2, &vlen);
    argint(3, &flags);

    if (vlen <= 0)
        return (uint64)-EINVAL;
    if (vlen > 1024)
        vlen = 1024;

    /*
     * struct mmsghdr layout (LP64):
     *   struct msghdr msg_hdr;  (56 bytes = sizeof(struct k_msghdr))
     *   unsigned int  msg_len;  (4 bytes)
     *   + 4 bytes padding       = 64 bytes total
     */
    const uint64 mmsghdr_size = sizeof(struct k_msghdr) + 8;

    int sent = 0;
    for (int i = 0; i < vlen; i++) {
        uint64 entry_addr = umsgvec + (uint64)i * mmsghdr_size;

        struct k_msghdr mh;
        if (vm_copyin(current->vm, &mh, entry_addr, sizeof(mh)) < 0)
            return sent > 0 ? (uint64)sent : (uint64)-EFAULT;

        if (mh.msg_iovlen == 0 || mh.msg_iovlen < 0 ||
            mh.msg_iovlen > SENDMSG_MAX_IOV)
            return sent > 0 ? (uint64)sent : (uint64)-EINVAL;

        struct k_iovec iovs[SENDMSG_MAX_IOV];
        if (vm_copyin(current->vm, iovs, mh.msg_iov,
                      mh.msg_iovlen * sizeof(struct k_iovec)) < 0)
            return sent > 0 ? (uint64)sent : (uint64)-EFAULT;

        /* Gather data */
        size_t total = 0;
        char tmpbuf[8192];
        for (uint64 j = 0; j < mh.msg_iovlen; j++) {
            if (iovs[j].iov_len > sizeof(tmpbuf) - total)
                return sent > 0 ? (uint64)sent : (uint64)-EMSGSIZE;
            size_t chunk = iovs[j].iov_len;
            if (vm_copyin(current->vm, tmpbuf + total,
                          iovs[j].iov_base, chunk) < 0)
                return sent > 0 ? (uint64)sent : (uint64)-EFAULT;
            total += chunk;
        }

        /* Send via the lwip send path */
        int domain = sock_domain_from_fd(sockfd);
        ssize_t n;

        if (domain == AF_UNIX) {
            struct unix_sock *usk = unix_sock_from_fd(sockfd);
            if (usk == NULL)
                return sent > 0 ? (uint64)sent : (uint64)-EBADF;

            if (total > UNIX_BUF_MAX_SIZE)
                return sent > 0 ? (uint64)sent : (uint64)-EMSGSIZE;

            struct vfs_file *scm_files[UNIX_SCM_QUEUE_MAX];
            memset(scm_files, 0, sizeof(scm_files));
            size_t scm_count = 0;

            if (mh.msg_control != 0 &&
                mh.msg_controllen >= K_CMSG_LEN(sizeof(int))) {
                unsigned char cmsg_buf[K_CMSG_SPACE(sizeof(int) * UNIX_SCM_QUEUE_MAX)];
                uint64 copy_len = mh.msg_controllen;
                if (copy_len > sizeof(cmsg_buf))
                    copy_len = sizeof(cmsg_buf);
                if (vm_copyin(current->vm, cmsg_buf, mh.msg_control, copy_len) < 0)
                    return sent > 0 ? (uint64)sent : (uint64)-EFAULT;

                struct k_cmsghdr *cmsg = (struct k_cmsghdr *)cmsg_buf;
                if (cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SCM_RIGHTS &&
                    cmsg->cmsg_len >= K_CMSG_LEN(sizeof(int))) {
                    size_t payload_len = k_cmsg_payload_len(cmsg);
                    size_t nfds = payload_len / sizeof(int);
                    size_t max_fds = (copy_len >= K_CMSG_LEN(0))
                        ? (copy_len - K_CMSG_LEN(0)) / sizeof(int) : 0;
                    if (nfds > max_fds)
                        nfds = max_fds;
                    if (nfds >= UNIX_SCM_QUEUE_MAX)
                        return sent > 0 ? (uint64)sent : (uint64)-EMSGSIZE;
                    if (nfds > 0) {
                        int *pass_fds = (int *)K_CMSG_DATA(cmsg);
                        for (size_t k = 0; k < nfds; k++) {
                            struct vfs_file *file =
                                vfs_fdtable_get_file(current->fdtable, pass_fds[k]);
                            if (file == NULL) {
                                for (size_t l = 0; l < scm_count; l++)
                                    vfs_fput(scm_files[l]);
                                return sent > 0 ? (uint64)sent : (uint64)-EBADF;
                            }

                            scm_files[scm_count] = vfs_fdup(file);
                            vfs_fput(file);
                            if (scm_files[scm_count] == NULL) {
                                for (size_t l = 0; l < scm_count; l++)
                                    vfs_fput(scm_files[l]);
                                return sent > 0 ? (uint64)sent : (uint64)-ENOMEM;
                            }
                            scm_count++;
                        }
                    }
                }
            }

            struct vfs_file *notify_file = NULL;
            spin_lock(&usk->lock);
            n = unix_sendmsg_atomic_locked(usk, tmpbuf, total,
                                           scm_files, scm_count);
            if (n == 0 && usk->peer != NULL) {
                tq_wakeup_all(&usk->peer->rd_queue, 0, 0);
                if (usk->peer->file != NULL)
                    notify_file = vfs_fdup(usk->peer->file);
            }
            spin_unlock(&usk->lock);

            if (n < 0) {
                for (size_t k = 0; k < scm_count; k++)
                    vfs_fput(scm_files[k]);
            } else {
                n = (ssize_t)total;
            }
            if (notify_file != NULL) {
                vfs_file_knote_notify(notify_file, EVFILT_READ, 0);
                vfs_fput(notify_file);
            }
        } else {
            struct lwip_sock *sk = sock_from_fd(sockfd);
            if (sk == NULL)
                return sent > 0 ? (uint64)sent : (uint64)-EBADF;

            if (sk->type == SOCK_STREAM) {
                size_t written = 0;

                if (sock_is_nonblock(sockfd, flags)) {
                    int ready = sock_poll_ready(sk, POLLOUT);
                    if (!(ready & POLLOUT))
                        return sent > 0 ? (uint64)sent : (uint64)-EAGAIN;
                }

                while (written < total) {
                    size_t chunk = total - written;
                    size_t chunk_written = 0;
                    err_t err;

                    if (chunk > sizeof(tmpbuf))
                        chunk = sizeof(tmpbuf);

                    err = netconn_write_partly(sk->conn, tmpbuf + written, chunk,
                                               NETCONN_COPY, &chunk_written);
                    if (err != ERR_OK) {
                        if (err == ERR_WOULDBLOCK)
                            break;
                        n = written > 0 ? (ssize_t)written
                                        : -(ssize_t)lwip_err_to_errno(err);
                        goto sendmmsg_message_done;
                    }

                    written += chunk_written;
                    if (chunk_written < chunk)
                        break;
                }

                n = (ssize_t)written;
            } else {
                struct netbuf nb;
                memset(&nb, 0, sizeof(nb));
                struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (uint16)total, PBUF_RAM);
                if (p == NULL)
                    return sent > 0 ? (uint64)sent : (uint64)-ENOMEM;
                pbuf_take(p, tmpbuf, (uint16)total);
                nb.p = p;
                err_t err = netconn_send(sk->conn, &nb);
                pbuf_free(p);
                n = (err == ERR_OK) ? (ssize_t)total : -(ssize_t)lwip_err_to_errno(err);
            }
        }

sendmmsg_message_done:

        if (n < 0)
            return sent > 0 ? (uint64)sent : (uint64)n;

        /* Write msg_len back */
        uint32 msg_len = (uint32)n;
        vm_copyout(current->vm,
                   entry_addr + sizeof(struct k_msghdr),
                   &msg_len, sizeof(msg_len));
        sent++;
    }

    return (uint64)sent;
}

/*
 * sys_recvmmsg(sockfd, msgvec, vlen, flags, timeout) → count / -errno
 *
 * Receive multiple messages from a socket in a single syscall.
 * This is the time64 variant (SYS_recvmmsg_time64).
 */
uint64 sys_recvmmsg(void)
{
    int sockfd, vlen, flags;
    uint64 umsgvec;
    /* uint64 utimeout; — ignored for now */
    argint(0, &sockfd);
    argaddr(1, &umsgvec);
    argint(2, &vlen);
    argint(3, &flags);
    /* argaddr(4, &utimeout); */

    if (vlen <= 0)
        return (uint64)-EINVAL;
    if (vlen > 1024)
        vlen = 1024;

    const uint64 mmsghdr_size = sizeof(struct k_msghdr) + 8;
    int received = 0;

    for (int i = 0; i < vlen; i++) {
        uint64 entry_addr = umsgvec + (uint64)i * mmsghdr_size;

        struct k_msghdr mh;
        if (vm_copyin(current->vm, &mh, entry_addr, sizeof(mh)) < 0)
            return received > 0 ? (uint64)received : (uint64)-EFAULT;

        if (mh.msg_iovlen == 0 || mh.msg_iovlen < 0 ||
            mh.msg_iovlen > SENDMSG_MAX_IOV)
            return received > 0 ? (uint64)received : (uint64)-EINVAL;

        struct k_iovec iovs[SENDMSG_MAX_IOV];
        if (vm_copyin(current->vm, iovs, mh.msg_iov,
                      mh.msg_iovlen * sizeof(struct k_iovec)) < 0)
            return received > 0 ? (uint64)received : (uint64)-EFAULT;

        /* Determine domain */
        int domain = sock_domain_from_fd(sockfd);
        ssize_t total = 0;

        if (domain == AF_UNIX) {
            struct unix_sock *usk = unix_sock_from_fd(sockfd);
            if (usk == NULL)
                return received > 0 ? (uint64)received : (uint64)-EBADF;

            struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, sockfd);
            if (f == NULL)
                return received > 0 ? (uint64)received : (uint64)-EBADF;

            int saved_fflags = f->f_flags;
            if (flags & MSG_DONTWAIT)
                f->f_flags |= O_NONBLOCK;

            for (uint64 j = 0; j < mh.msg_iovlen; j++) {
                if (iovs[j].iov_len == 0)
                    continue;
                ssize_t n = unix_socket_file_ops.read(
                    f, (char *)(iovs[j].iov_base), iovs[j].iov_len, 1);
                if (n < 0) {
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    return received > 0 ? (uint64)received : (uint64)n;
                }
                total += n;
                if ((size_t)n < iovs[j].iov_len)
                    break;
            }
            f->f_flags = saved_fflags;
            vfs_fput(f);

            if (mh.msg_control != 0 &&
                mh.msg_controllen >= K_CMSG_LEN(sizeof(int))) {
                struct unix_sock *peer = NULL;
                spin_lock(&usk->lock);
                peer = usk->peer;
                spin_unlock(&usk->lock);

                size_t max_fds = (mh.msg_controllen >= K_CMSG_SPACE(0))
                    ? (mh.msg_controllen - K_CMSG_SPACE(0)) / sizeof(int) : 0;
                if (max_fds > UNIX_SCM_QUEUE_MAX)
                    max_fds = UNIX_SCM_QUEUE_MAX;

                struct vfs_file *scm_files[UNIX_SCM_QUEUE_MAX];
                memset(scm_files, 0, sizeof(scm_files));
                size_t scm_count = unix_dequeue_scm_rights(peer, scm_files,
                                                           max_fds);

                if (scm_count > 0) {
                    int newfds[UNIX_SCM_QUEUE_MAX];
                    size_t installed = 0;

                    for (; installed < scm_count; installed++) {
                        spin_lock(&current->fdtable->lock);
                        int newfd = vfs_fdtable_alloc_fd(current->fdtable,
                                                         scm_files[installed]);
                        if (newfd >= 0 && (flags & MSG_CMSG_CLOEXEC))
                            vfs_fdtable_set_fdflags(current->fdtable, newfd,
                                                    FD_CLOEXEC);
                        spin_unlock(&current->fdtable->lock);
                        vfs_fput(scm_files[installed]);
                        if (newfd < 0)
                            break;
                        newfds[installed] = newfd;
                    }

                    for (size_t j = installed + 1; j < scm_count; j++)
                        vfs_fput(scm_files[j]);

                    if (installed > 0) {
                        unsigned char cmsg_buf[K_CMSG_SPACE(sizeof(int) *
                                                            UNIX_SCM_QUEUE_MAX)];
                        memset(cmsg_buf, 0, sizeof(cmsg_buf));
                        struct k_cmsghdr *cmsg =
                            (struct k_cmsghdr *)cmsg_buf;
                        cmsg->cmsg_len = K_CMSG_LEN(sizeof(int) * installed);
                        cmsg->cmsg_level = SOL_SOCKET;
                        cmsg->cmsg_type = SCM_RIGHTS;
                        memmove(K_CMSG_DATA(cmsg), newfds,
                                sizeof(int) * installed);

                        uint32 clen = (uint32)K_CMSG_SPACE(sizeof(int) * installed);
                        vm_copyout(current->vm, mh.msg_control,
                                   cmsg_buf, clen);
                        vm_copyout(current->vm,
                                   entry_addr + __builtin_offsetof(struct k_msghdr,
                                                                   msg_controllen),
                                   &clen, sizeof(clen));
                    } else {
                        uint32 zero = 0;
                        vm_copyout(current->vm,
                                   entry_addr + __builtin_offsetof(struct k_msghdr,
                                                                   msg_controllen),
                                   &zero, sizeof(zero));
                    }
                } else {
                    uint32 zero = 0;
                    vm_copyout(current->vm,
                               entry_addr + __builtin_offsetof(struct k_msghdr,
                                                               msg_controllen),
                               &zero, sizeof(zero));
                }
            }

            int zero_flags = 0;
            vm_copyout(current->vm,
                       entry_addr + __builtin_offsetof(struct k_msghdr,
                                                       msg_flags),
                       &zero_flags, sizeof(zero_flags));
        } else {
            struct lwip_sock *sk = sock_from_fd(sockfd);
            if (sk == NULL)
                return received > 0 ? (uint64)received : (uint64)-EBADF;

            /* For the first message, block normally.  For subsequent
             * messages, use MSG_DONTWAIT to avoid blocking. */
            int recv_flags = (i > 0) ? (flags | 0x40 /*MSG_DONTWAIT*/) : flags;

            if (sock_is_nonblock(sockfd, recv_flags)) {
                int ready = sock_poll_ready(sk, POLLIN);
                if (!(ready & (POLLIN | POLLHUP | POLLERR))) {
                    if (received > 0) break;
                    return (uint64)-EAGAIN;
                }
            }

            if (sk->type == SOCK_STREAM) {
                struct pbuf *p = NULL;
                uint16 poff = 0;
                uint16 start_off = 0;
                bool reused_lastpbuf = false;

                if (sk->lastpbuf != NULL) {
                    p = sk->lastpbuf;
                    poff = sk->lastpbuf_off;
                    reused_lastpbuf = true;
                } else {
                    err_t err = sock_tcp_recv_pbuf(sk, &p,
                        sock_is_nonblock(sockfd, recv_flags));
                    if (err != ERR_OK) {
                        if (err == ERR_CLSD && received > 0) break;
                        if (err == ERR_CLSD) return 0;
                        if (received > 0) break;
                        return (uint64)-lwip_err_to_errno(err);
                    }
                }

                start_off = poff;

                char tmpbuf[1500];
                for (uint64 j = 0; j < mh.msg_iovlen && poff < p->tot_len; j++) {
                    size_t want = iovs[j].iov_len;
                    uint16 remain = p->tot_len - poff;
                    size_t tocopy = (want < remain) ? want : (size_t)remain;
                    size_t copied = 0;
                    while (copied < tocopy) {
                        size_t chunk = tocopy - copied;
                        if (chunk > sizeof(tmpbuf)) chunk = sizeof(tmpbuf);
                        uint16 got = pbuf_copy_partial(p, tmpbuf, (uint16)chunk, poff);
                        if (got == 0) break;
                        if (vm_copyout(current->vm, iovs[j].iov_base + copied,
                                       tmpbuf, got) < 0) {
                            if (!reused_lastpbuf)
                                pbuf_free(p);
                            return received > 0 ? (uint64)received : (uint64)-EFAULT;
                        }
                        poff += got;
                        copied += got;
                        total += got;
                    }
                }

                if (flags & MSG_PEEK) {
                    sk->lastpbuf = p;
                    sk->lastpbuf_off = start_off;
                } else if (poff < p->tot_len) {
                    sk->lastpbuf = p;
                    sk->lastpbuf_off = poff;
                } else {
                    pbuf_free(p);
                    sk->lastpbuf = NULL;
                    sk->lastpbuf_off = 0;
                }
                if (!(flags & MSG_PEEK))
                    sock_tcp_recvd(sk, (size_t)total);
            } else {
                struct netbuf *nb = NULL;
                err_t err = netconn_recv(sk->conn, &nb);
                if (err != ERR_OK) {
                    if (received > 0) break;
                    return (uint64)-lwip_err_to_errno(err);
                }
                void *data;
                u16_t dlen;
                netbuf_data(nb, &data, &dlen);

                uint16 doff = 0;
                for (uint64 j = 0; j < mh.msg_iovlen && doff < dlen; j++) {
                    size_t want = iovs[j].iov_len;
                    size_t remain = dlen - doff;
                    size_t tocopy = (want < remain) ? want : remain;
                    if (vm_copyout(current->vm, iovs[j].iov_base,
                                   (char *)data + doff, tocopy) < 0) {
                        netbuf_delete(nb);
                        return received > 0 ? (uint64)received : (uint64)-EFAULT;
                    }
                    doff += (uint16)tocopy;
                    total += (ssize_t)tocopy;
                }
                netbuf_delete(nb);
            }
        }

        /* Write msg_len back to user mmsghdr */
        uint32 msg_len = (uint32)total;
        vm_copyout(current->vm,
                   entry_addr + sizeof(struct k_msghdr),
                   &msg_len, sizeof(msg_len));

        /* Clear msg_flags */
        int zero_flags = 0;
        vm_copyout(current->vm,
                   entry_addr + __builtin_offsetof(struct k_msghdr, msg_flags),
                   &zero_flags, sizeof(zero_flags));

        received++;

        /* If we got 0 bytes (EOF on stream), stop */
        if (total == 0)
            break;
    }

    return (uint64)received;
}
