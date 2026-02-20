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
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include <mm/vm.h>
#include <vfs/vfs_types.h>
#include <vfs/file.h>
#include <vfs/fcntl.h>
#include <vfs/poll.h>

#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/netbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "arch/sys_arch.h"

/* From irq/syscall.c — argument fetching */
extern void argint(int n, int *ip);
extern void argint64(int n, int64 *ip);
extern void argaddr(int n, uint64 *ip);
extern int argstr(int n, char *buf, int max);

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
    struct netbuf  *lastbuf;     /* partially consumed recv buffer (TCP/UDP) */
    uint16         lastoffset;   /* offset into lastbuf */
};

/* Socket type constants (matching POSIX) */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
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

/* Socket option levels */
#define SOL_SOCKET     1

/* Socket options */
#define SO_REUSEADDR   2
#define SO_KEEPALIVE   9
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_RCVBUF      8
#define SO_SNDBUF      7
#define SO_ERROR       4

/* User-space sockaddr_in layout (must match newlib header) */
struct k_sockaddr_in {
    uint16 sin_family;
    uint16 sin_port;      /* network byte order */
    uint32 sin_addr;      /* network byte order */
    char   sin_zero[8];
};

#define K_SOCKADDR_IN_SIZE  16

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
    if (sk->conn != NULL)
        netconn_delete(sk->conn);
    kfree((void *)sk);
}

/* ========================================================================== */
/* VFS file_ops for sockets — allows read()/write()/close() on socket fds     */
/* ========================================================================== */

static ssize_t sock_file_read(struct vfs_file *file, char *buf, size_t count,
                              bool user)
{
    struct lwip_sock *sk = (struct lwip_sock *)file->private_data;
    if (sk == NULL || sk->conn == NULL)
        return -EBADF;

    if (sk->type == SOCK_STREAM) {
        /* TCP: use netconn_recv_tcp_pbuf for raw bytes */
        struct pbuf *p = NULL;
        err_t err;

        /* Check for leftover data from previous recv */
        if (sk->lastbuf != NULL) {
            struct netbuf *nb = sk->lastbuf;
            uint16 avail = netbuf_len(nb) - sk->lastoffset;
            uint16 tocopy = (count < avail) ? (uint16)count : avail;

            void *data;
            u16_t len;
            netbuf_data(nb, &data, &len);

            if (user) {
                if (vm_copyout(current->vm, (uint64)buf,
                               (char *)data + sk->lastoffset, tocopy) < 0)
                    return -EFAULT;
            } else {
                memmove(buf, (char *)data + sk->lastoffset, tocopy);
            }

            sk->lastoffset += tocopy;
            if (sk->lastoffset >= netbuf_len(nb)) {
                netbuf_delete(nb);
                sk->lastbuf = NULL;
                sk->lastoffset = 0;
            }
            return tocopy;
        }

        err = netconn_recv_tcp_pbuf(sk->conn, &p);
        if (err != ERR_OK) {
            if (err == ERR_CLSD)
                return 0; /* EOF */
            return -lwip_err_to_errno(err);
        }

        uint16 tocopy = (count < p->tot_len) ? (uint16)count : p->tot_len;

        if (user) {
            /* Copy pbuf data to user space (may be chained) */
            char tmpbuf[1500];
            uint16 clen = (tocopy > sizeof(tmpbuf)) ? sizeof(tmpbuf) : tocopy;
            pbuf_copy_partial(p, tmpbuf, clen, 0);
            if (vm_copyout(current->vm, (uint64)buf, tmpbuf, clen) < 0) {
                pbuf_free(p);
                return -EFAULT;
            }
            tocopy = clen;
        } else {
            pbuf_copy_partial(p, buf, tocopy, 0);
        }

        if (tocopy < p->tot_len) {
            /* Save remainder — wrap in netbuf for consistency */
            /* For TCP we just free and accept short read (standard POSIX) */
        }
        pbuf_free(p);
        return tocopy;

    } else {
        /* UDP/RAW: use netconn_recv → netbuf */
        struct netbuf *nb = NULL;
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
        /* TCP: use netconn_write */
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

            err_t err = netconn_write(sk->conn, tmpbuf, chunk,
                                      NETCONN_COPY);
            if (err != ERR_OK) {
                if (written > 0)
                    return (ssize_t)written;
                return -lwip_err_to_errno(err);
            }
            written += chunk;
        }
        return (ssize_t)written;

    } else {
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
        /* Buffered data from a partial previous read? */
        if (sk->lastbuf != NULL) {
            revents |= (events & (POLLIN | POLLRDNORM));
        } else if (conn->state == NETCONN_LISTEN) {
#if LWIP_TCP
            /* Listening socket: check acceptmbox */
            if (sys_mbox_valid(&conn->acceptmbox) &&
                conn->acceptmbox.count > 0)
                revents |= (events & (POLLIN | POLLRDNORM));
#endif
        } else {
            /* Data socket: check recvmbox */
            if (sys_mbox_valid(&conn->recvmbox) &&
                conn->recvmbox.count > 0)
                revents |= (events & (POLLIN | POLLRDNORM));
        }

        /* Connection closed / error on the receive side */
        if (conn->pending_err != ERR_OK) {
            revents |= POLLERR;
        }
        if (conn->flags & NETCONN_FLAG_MBOXCLOSED) {
            revents |= POLLHUP;
        }
    }

    if (events & (POLLOUT | POLLWRNORM)) {
        /* Writing is generally always possible (lwIP buffers internally) */
        revents |= (events & (POLLOUT | POLLWRNORM));
    }

    return revents;
}

static struct vfs_file_ops lwip_socket_file_ops = {
    .read    = sock_file_read,
    .write   = sock_file_write,
    .llseek  = NULL,
    .release = sock_file_release,
    .fsync   = NULL,
    .fflush  = NULL,
    .poll    = sock_file_poll,
    .fault   = NULL,
};

/* ========================================================================== */
/* Helper: create a socket fd from a netconn                                  */
/* ========================================================================== */

static int sock_fd_alloc(struct netconn *conn, int type, int protocol)
{
    struct lwip_sock *sk = lwip_sock_alloc();
    if (sk == NULL)
        return -ENOMEM;

    sk->conn = conn;
    sk->type = type;
    sk->protocol = protocol;

    int fd = vfs_custom_fd_alloc(&lwip_socket_file_ops, sk, O_RDWR);
    if (fd < 0) {
        lwip_sock_free(sk);
        return fd;
    }

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

    if (domain != AF_INET)
        return (uint64)-EAFNOSUPPORT;

    enum netconn_type ntype = posix_to_netconn_type(type, protocol);
    if (ntype == NETCONN_INVALID)
        return (uint64)-EPROTOTYPE;

    struct netconn *conn = netconn_new(ntype);
    if (conn == NULL)
        return (uint64)-ENOMEM;

    int fd = sock_fd_alloc(conn, type, protocol);
    if (fd < 0) {
        netconn_delete(conn);
        return (uint64)fd;
    }

    if (flags & SOCK_CLOEXEC) {
        spin_lock(&current->fdtable->lock);
        vfs_fdtable_set_fdflags(current->fdtable, fd, FD_CLOEXEC);
        spin_unlock(&current->fdtable->lock);
    }

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

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    struct netconn *newconn = NULL;
    err_t err = netconn_accept(sk->conn, &newconn);
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

    int newfd = sock_fd_alloc(newconn, SOCK_STREAM, sk->protocol);
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

    err_t err = netconn_connect(sk->conn, &ipaddr, ntohs(sa.sin_port));
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

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

    (void)flags; /* flags not supported for now */

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    if (len <= 0)
        return 0;

    if (sk->type == SOCK_STREAM) {
        /* TCP: ignore dest_addr, use netconn_write */
        char tmpbuf[1500];
        size_t chunk = (size_t)len;
        if (chunk > sizeof(tmpbuf))
            chunk = sizeof(tmpbuf);

        if (vm_copyin(current->vm, tmpbuf, ubuf, chunk) < 0)
            return (uint64)-EFAULT;

        err_t err = netconn_write(sk->conn, tmpbuf, chunk, NETCONN_COPY);
        if (err != ERR_OK)
            return (uint64)-lwip_err_to_errno(err);
        return (uint64)chunk;

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

    (void)flags; /* flags not supported for now */

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    if (len <= 0)
        return 0;

    if (sk->type == SOCK_STREAM) {
        /* TCP: use netconn_recv_tcp_pbuf */
        struct pbuf *p = NULL;
        err_t err = netconn_recv_tcp_pbuf(sk->conn, &p);
        if (err != ERR_OK) {
            if (err == ERR_CLSD)
                return 0; /* EOF */
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
        pbuf_free(p);
        return (uint64)clen;

    } else {
        /* UDP/RAW: use netconn_recv → netbuf */
        struct netbuf *nb = NULL;
        err_t err = netconn_recv(sk->conn, &nb);
        if (err != ERR_OK)
            return (uint64)-lwip_err_to_errno(err);

        void *data;
        u16_t dlen;
        netbuf_data(nb, &data, &dlen);

        uint16 tocopy = (len < (int)dlen) ? (uint16)len : dlen;

        if (vm_copyout(current->vm, ubuf, data, tocopy) < 0) {
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

        netbuf_delete(nb);
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

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    /* Minimal implementation — accept common options silently */
    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
            if (optlen >= (int)sizeof(int)) {
                int val;
                if (vm_copyin(current->vm, &val, uoptval, sizeof(val)) < 0)
                    return (uint64)-EFAULT;
                if (val)
                    ip_set_option(sk->conn->pcb.ip, SOF_REUSEADDR);
                else
                    ip_reset_option(sk->conn->pcb.ip, SOF_REUSEADDR);
            }
            return 0;
        case SO_KEEPALIVE:
            if (optlen >= (int)sizeof(int) && sk->type == SOCK_STREAM) {
                int val;
                if (vm_copyin(current->vm, &val, uoptval, sizeof(val)) < 0)
                    return (uint64)-EFAULT;
                if (val)
                    ip_set_option(sk->conn->pcb.ip, SOF_KEEPALIVE);
                else
                    ip_reset_option(sk->conn->pcb.ip, SOF_KEEPALIVE);
            }
            return 0;
        case SO_RCVTIMEO:
            if (optlen >= (int)sizeof(int)) {
                int val;
                if (vm_copyin(current->vm, &val, uoptval, sizeof(val)) < 0)
                    return (uint64)-EFAULT;
                netconn_set_recvtimeout(sk->conn, val);
            }
            return 0;
        case SO_SNDTIMEO:
            if (optlen >= (int)sizeof(int)) {
                int val;
                if (vm_copyin(current->vm, &val, uoptval, sizeof(val)) < 0)
                    return (uint64)-EFAULT;
                netconn_set_sendtimeout(sk->conn, val);
            }
            return 0;
        default:
            return 0; /* silently accept unknown options */
        }
    }
    if (level == IPPROTO_TCP) {
        switch (optname) {
        case 1: /* TCP_NODELAY */ {
            if (sk->type != SOCK_STREAM || sk->conn->pcb.tcp == NULL)
                return (uint64)-EINVAL;
            if (optlen >= (int)sizeof(int)) {
                int val;
                if (vm_copyin(current->vm, &val, uoptval, sizeof(val)) < 0)
                    return (uint64)-EFAULT;
                if (val)
                    tcp_nagle_disable(sk->conn->pcb.tcp);
                else
                    tcp_nagle_enable(sk->conn->pcb.tcp);
            }
            return 0;
        }
        default:
            return 0;
        }
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

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    if (level == SOL_SOCKET && optname == SO_ERROR) {
        int sockerr = 0;
        int olen = sizeof(int);
        vm_copyout(current->vm, uoptval, &sockerr, sizeof(sockerr));
        vm_copyout(current->vm, uoptlen, &olen, sizeof(olen));
        return 0;
    }

    /* For unsupported options, return 0 */
    int val = 0;
    int olen = sizeof(int);
    vm_copyout(current->vm, uoptval, &val, sizeof(val));
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

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

    ip_addr_t laddr;
    u16_t lport;
    err_t err = netconn_getaddr(sk->conn, &laddr, &lport, 1 /* local */);
    if (err != ERR_OK)
        return (uint64)-lwip_err_to_errno(err);

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
