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
#define MSG_DONTWAIT   0x40
#define MSG_WAITALL    0x100
#define MSG_NOSIGNAL   0x4000

/* Socket option levels */
#define SOL_SOCKET     1

/* Socket options (SOL_SOCKET level) */
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_BROADCAST   6
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_KEEPALIVE   9
#define SO_LINGER      13
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ACCEPTCONN  30

/* IPPROTO_IP options */
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
        if (sk->lastbuf != NULL || sk->lastpbuf != NULL) {
            revents |= POLLIN;
        } else if (conn->state == NETCONN_LISTEN) {
#if LWIP_TCP
            if (sys_mbox_valid(&conn->acceptmbox) &&
                conn->acceptmbox.count > 0)
                revents |= POLLIN;
#endif
        } else {
            if (sys_mbox_valid(&conn->recvmbox) &&
                conn->recvmbox.count > 0)
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
        /* TCP: use netconn_recv_tcp_pbuf for raw bytes */
        struct pbuf *p = NULL;
        err_t err;

        /* Check for leftover pbuf data from previous partial TCP read */
        if (sk->lastpbuf != NULL) {
            p = sk->lastpbuf;
            uint16 avail = p->tot_len - sk->lastpbuf_off;
            uint16 tocopy = (count < avail) ? (uint16)count : avail;

            if (user) {
                char tmpbuf[1500];
                uint16 clen = (tocopy > sizeof(tmpbuf)) ? sizeof(tmpbuf) : tocopy;
                pbuf_copy_partial(p, tmpbuf, clen, sk->lastpbuf_off);
                if (vm_copyout(current->vm, (uint64)buf, tmpbuf, clen) < 0)
                    return -EFAULT;
                tocopy = clen;
            } else {
                pbuf_copy_partial(p, buf, tocopy, sk->lastpbuf_off);
            }

            sk->lastpbuf_off += tocopy;
            if (sk->lastpbuf_off >= p->tot_len) {
                pbuf_free(p);
                sk->lastpbuf = NULL;
                sk->lastpbuf_off = 0;
            }
            return tocopy;
        }

        /* Check for leftover netbuf data from previous recv (UDP-style) */
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

        /* O_NONBLOCK: check readiness before blocking */
        if (file->f_flags & O_NONBLOCK) {
            int ready = sock_poll_ready(sk, POLLIN);
            if (!(ready & (POLLIN | POLLHUP | POLLERR)))
                return -EAGAIN;
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
            /* Save remainder for future reads — prevents TCP data loss */
            sk->lastpbuf = p;
            sk->lastpbuf_off = tocopy;
        } else {
            pbuf_free(p);
        }
        return tocopy;

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
         * from calling sock_netconn_callback with a stale vfs_file ptr. */
        sk->conn->callback = NULL;
        sk->conn->callback_arg.ptr = NULL;
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
            int mbox_valid = sys_mbox_valid(&conn->recvmbox);
            int mbox_count = mbox_valid ? (int)conn->recvmbox.count : -1;
            if (mbox_valid && mbox_count > 0)
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
        }

        /* Also check the netconn recvmbox (approximate) */
        if (count == 0 && sk->conn->state != NETCONN_LISTEN) {
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
    struct vfs_file *file = (struct vfs_file *)conn->callback_arg.ptr;
    if (file == NULL)
        return;

    switch (evt) {
    case NETCONN_EVT_RCVPLUS:
        vfs_file_knote_notify(file, EVFILT_READ, (int64)len);
        break;
    case NETCONN_EVT_SENDPLUS:
        vfs_file_knote_notify(file, EVFILT_WRITE, (int64)len);
        break;
    case NETCONN_EVT_ERROR:
        vfs_file_knote_notify(file, EVFILT_READ, 0);
        vfs_file_knote_notify(file, EVFILT_WRITE, 0);
        break;
    default:
        break;
    }
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
        ACCT_ADD(current->thread_group, net_bytes_sent, chunk);
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
            }
            ACCT_ADD(current->thread_group, net_bytes_recv, clen);
            return (uint64)clen;
        }

        err_t err = netconn_recv_tcp_pbuf(sk->conn, &p);
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
        } else {
            pbuf_free(p);
        }
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
    } else {
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
    uint64 msg_iovlen;      /* size_t                  */
    uint64 msg_control;     /* void *msg_control       */
    uint64 msg_controllen;  /* size_t                  */
    int    msg_flags;       /* int                     */
};

struct k_iovec {
    uint64 iov_base;        /* void *  */
    uint64 iov_len;         /* size_t  */
};

#define SENDMSG_MAX_IOV 16

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

    (void)flags;   /* best-effort: MSG_NOSIGNAL etc. accepted but ignored */

    /* Domain dispatch — AF_UNIX/AF_NETLINK don't support sendmsg yet */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX || domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

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
    if (mh.msg_iovlen > SENDMSG_MAX_IOV)
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
        /* TCP: write each iov sequentially through netconn_write */
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
                err_t err = netconn_write(sk->conn, tmpbuf, chunk,
                                          NETCONN_COPY);
                if (err != ERR_OK)
                    return total > 0 ? (uint64)total
                                     : (uint64)-lwip_err_to_errno(err);
                sent += chunk;
                total += (ssize_t)chunk;
            }
        }
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

    (void)flags;

    /* Domain dispatch — AF_UNIX/AF_NETLINK don't support recvmsg yet */
    int domain = sock_domain_from_fd(fd);
    if (domain == AF_UNIX || domain == AF_NETLINK)
        return (uint64)-EOPNOTSUPP;

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
    if (mh.msg_iovlen > SENDMSG_MAX_IOV)
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
        /* TCP: receive one pbuf, scatter into iovecs */
        struct pbuf *p = NULL;
        err_t err = netconn_recv_tcp_pbuf(sk->conn, &p);
        if (err != ERR_OK) {
            if (err == ERR_CLSD)
                return 0;          /* EOF */
            if (err == ERR_TIMEOUT && signal_pending(current))
                return (uint64)-EINTR;
            return (uint64)-lwip_err_to_errno(err);
        }

        uint16 avail = p->tot_len;
        uint16 poff = 0;
        char tmpbuf[1500];

        for (uint64 i = 0; i < mh.msg_iovlen && poff < avail; i++) {
            size_t want = iovs[i].iov_len;
            uint16 remain = avail - poff;
            size_t tocopy = (want < remain) ? want : (size_t)remain;

            /* Copy in chunks through tmpbuf */
            size_t copied = 0;
            while (copied < tocopy) {
                size_t chunk = tocopy - copied;
                if (chunk > sizeof(tmpbuf))
                    chunk = sizeof(tmpbuf);
                uint16 got = pbuf_copy_partial(p, tmpbuf, (uint16)chunk, poff);
                if (got == 0)
                    break;
                if (vm_copyout(current->vm,
                               iovs[i].iov_base + copied,
                               tmpbuf, got) < 0) {
                    pbuf_free(p);
                    return total > 0 ? (uint64)total : (uint64)-EFAULT;
                }
                poff += got;
                copied += got;
                total += got;
            }
        }
        pbuf_free(p);

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
    uint64 zero = 0;
    vm_copyout(current->vm,
                umsg + __builtin_offsetof(struct k_msghdr, msg_controllen),
                &zero, sizeof(zero));

    if (total > 0)
        ACCT_ADD(current->thread_group, net_bytes_recv, (uint64)total);
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
 * Stub: not implemented.  Returns -ENOSYS.
 */
uint64 sys_sendmmsg(void)
{
    return (uint64)-ENOSYS;
}
