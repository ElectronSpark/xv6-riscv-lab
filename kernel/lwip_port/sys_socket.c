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
#include <vfs/fs.h>
#include <vfs/fcntl.h>
#include <vfs/poll.h>
#include "proc/workqueue.h"
#include "accounting.h"

#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/netbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/ip.h"
#include "lwip/inet.h"
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
#include "proc/sched.h"
#include "cmdline.h"
#include "proc/chrome_lifecycle.h"
#include "lock/rcu.h"

/* From irq/syscall.c — argument fetching */
extern void argint(int n, int *ip);
extern void argint64(int n, int64 *ip);
extern void argaddr(int n, uint64 *ip);
extern int argstr(int n, char *buf, int max);
extern void sleep_ms(uint64 ms);

static void socket_unwind_created_fd(int fd)
{
    if (fd < 0)
        return;

    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = vfs_fdtable_dealloc_fd(current->fdtable, fd);
    spin_unlock(&current->fdtable->lock);

    if (f != NULL) {
        vfs_file_maybe_last_fd_close(f);
        vfs_fput(f);
    }
}

/* ========================================================================== */
/* Debug socket tracing (set to 1 to enable)                                  */
/* ========================================================================== */
#define SOCK_DEBUG 0
#if SOCK_DEBUG
#define sock_dbg(fmt, ...) printf("[sock] " fmt, ##__VA_ARGS__)
#else
#define sock_dbg(fmt, ...) ((void)0)
#endif

static int chrome_socket_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_socket_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        if (!enabled) {
            enabled = cmdline_get_param("chrome_unix_seqpacket_trace", value,
                                        sizeof(value)) == 0 &&
                value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        }
        initialized = 1;
    }
    return enabled;
}

static int chrome_socket_trace_process(void)
{
    return chrome_lifecycle_kernel_trace_process_match(current, 1, 1);
}

static int portal_socket_trace_process(void)
{
    if (current == NULL)
        return 0;
    if (strncmp(current->name, "xdg-desktop-por", 15) == 0 ||
        strncmp(current->name, "xdg-document-po", 15) == 0)
        return 1;
    if (current->thread_group == NULL)
        return 0;
    return strstr(current->thread_group->exec_path, "xdg-desktop-portal") != NULL ||
           strstr(current->thread_group->exec_path, "xdg-document-portal") != NULL;
}

static const char *chrome_socket_trace_path(struct vfs_file *f)
{
    if (f == NULL)
        return "(null)";
    if (f->opened_path != NULL)
        return f->opened_path;
    if (f->f_kind == VFS_FILE_KIND_PIPE)
        return "pipe";
    if (f->f_kind == VFS_FILE_KIND_LEGACY_SOCKET)
        return "socket";
    if (f->f_kind == VFS_FILE_KIND_CUSTOM)
        return "custom";
    if (f->f_kind == VFS_FILE_KIND_CDEV)
        return "cdev";
    if (f->f_kind == VFS_FILE_KIND_BDEV)
        return "bdev";
    return "(unknown)";
}

static int chrome_unix_ipc_trace_enabled(void);
static int audio_unix_ipc_trace_mode(void);
static int audio_unix_ipc_trace_process(void);
static int unix_ipc_trace_process(void);

#define CHROME_SOCKET_TRACE(fmt, ...)                                        \
    do {                                                                     \
        if (chrome_socket_trace_enabled() && chrome_socket_trace_process())   \
            printf("chrome-socket-trace: " fmt, ##__VA_ARGS__);              \
    } while (0)

#define CHROME_UNIX_SOCKET_TRACE(fmt, ...)                                   \
    do {                                                                     \
        if (unix_ipc_trace_process())                                         \
            printf("chrome-unix-ipc: " fmt, ##__VA_ARGS__);                  \
    } while (0)

static const char *chrome_socket_trace_evt_name(enum netconn_evt evt)
{
    switch (evt) {
    case NETCONN_EVT_RCVPLUS: return "rcvplus";
    case NETCONN_EVT_RCVMINUS: return "rcvminus";
    case NETCONN_EVT_SENDPLUS: return "sendplus";
    case NETCONN_EVT_SENDMINUS: return "sendminus";
    case NETCONN_EVT_ERROR: return "error";
    default: return "unknown";
    }
}

static int chrome_unix_ipc_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_unix_ipc_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int portal_unix_ipc_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("portal_unix_ipc_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static int audio_unix_ipc_trace_mode(void)
{
    static int initialized;
    static int mode;
    char value[8];

    if (!initialized) {
        if (cmdline_get_param("audio_unix_ipc_trace", value,
                              sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N') {
            if (value[0] == '3')
                mode = 3;
            else
                mode = (value[0] == '2' || value[0] == 'a' ||
                        value[0] == 'A') ? 2 : 1;
        }
        initialized = 1;
    }
    return mode;
}

static int audio_unix_ipc_trace_process(void)
{
    int mode;
    const char *name;
    const char *path;

    if (current == NULL)
        return 0;

    mode = audio_unix_ipc_trace_mode();
    if (mode == 0)
        return 0;

    name = current->name;
    if (mode >= 3) {
        if (strncmp(name, "pipewire-pulse", 14) == 0 ||
            strncmp(name, "chromium-pulse", 14) == 0)
            return 1;
        if (current->thread_group == NULL)
            return 0;
        path = current->thread_group->exec_path;
        return strstr(path, "/pipewire-pulse") != NULL ||
               strstr(path, "/chromium-pulse-stream-reducer") != NULL;
    }

    if (strncmp(name, "aplay", 5) == 0 ||
        strncmp(name, "pactl", 5) == 0 ||
        strncmp(name, "pacat", 5) == 0 ||
        strncmp(name, "paplay", 6) == 0 ||
        strncmp(name, "pw-cat", 6) == 0)
        return 1;

    if (mode >= 2 &&
        (strncmp(name, "pipewire", 8) == 0 ||
         strncmp(name, "wireplumber", 11) == 0))
        return 1;

    if (current->thread_group == NULL)
        return 0;

    path = current->thread_group->exec_path;
    if (strstr(path, "/aplay") != NULL ||
        strstr(path, "/pactl") != NULL ||
        strstr(path, "/pacat") != NULL ||
        strstr(path, "/paplay") != NULL ||
        strstr(path, "/pw-cat") != NULL)
        return 1;

    return mode >= 2 &&
           (strstr(path, "/pipewire") != NULL ||
            strstr(path, "/wireplumber") != NULL);
}

static int unix_ipc_trace_process(void)
{
    if (chrome_unix_ipc_trace_enabled() && chrome_socket_trace_process())
        return 1;
    if (portal_unix_ipc_trace_enabled() && portal_socket_trace_process())
        return 1;
    return audio_unix_ipc_trace_process();
}

static size_t chrome_unix_queue_count(int head, int tail, int max)
{
    if (tail >= head)
        return (size_t)(tail - head);
    return (size_t)(max - head + tail);
}

struct chrome_unix_file_snapshot {
    int is_unix;
    struct unix_sock *sk;
    struct unix_sock *peer;
    struct vfs_file *file;
    struct vfs_file *peer_file;
    int visible_refs;
    int peer_visible_refs;
    uint64 ino;
    uint64 peer_ino;
    int type;
    int state;
    int shutdown;
    int err;
    size_t tx_bytes;
    size_t tx_capacity;
    uint tx_nread;
    uint tx_nwrite;
    size_t scm;
    size_t packets;
    int peer_type;
    int peer_state;
    int peer_shutdown;
    int peer_err;
    size_t peer_tx_bytes;
    size_t peer_tx_capacity;
    uint peer_tx_nread;
    uint peer_tx_nwrite;
    size_t peer_scm;
    size_t peer_packets;
};

static void chrome_unix_file_snapshot_init(
    struct chrome_unix_file_snapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->type = -1;
    snap->state = -1;
    snap->peer_type = -1;
    snap->peer_state = -1;
}

static void chrome_unix_file_snapshot_fill(
    struct vfs_file *file, struct chrome_unix_file_snapshot *snap)
{
    chrome_unix_file_snapshot_init(snap);

    if (file == NULL || file->ops != &unix_socket_file_ops ||
        file->private_data == NULL)
        return;

    struct unix_sock *sk = (struct unix_sock *)file->private_data;
    struct unix_sock *peer = NULL;

    snap->is_unix = 1;
    snap->sk = sk;
    snap->file = file;
    snap->visible_refs = file->visible_fd_refs;

    spin_lock(&sk->lock);
    snap->ino = sk->proc_ino;
    snap->type = sk->type;
    snap->state = sk->state;
    snap->shutdown = sk->shutdown_flags;
    snap->err = sk->so_error;
    snap->tx_bytes = sk->tx.nwrite - sk->tx.nread;
    snap->tx_capacity = sk->tx.capacity;
    snap->tx_nread = sk->tx.nread;
    snap->tx_nwrite = sk->tx.nwrite;
    snap->scm = chrome_unix_queue_count(sk->scm_head, sk->scm_tail,
                                        UNIX_SCM_QUEUE_MAX);
    snap->packets = chrome_unix_queue_count(sk->packet_head,
                                            sk->packet_tail,
                                            UNIX_PACKET_QUEUE_MAX);
    peer = sk->peer;
    unix_sock_get_ref(peer);
    spin_unlock(&sk->lock);

    snap->peer = peer;
    if (peer != NULL) {
        spin_lock(&peer->lock);
        snap->peer_file = peer->file;
        if (peer->file != NULL)
            snap->peer_visible_refs = peer->file->visible_fd_refs;
        snap->peer_ino = peer->proc_ino;
        snap->peer_type = peer->type;
        snap->peer_state = peer->state;
        snap->peer_shutdown = peer->shutdown_flags;
        snap->peer_err = peer->so_error;
        snap->peer_tx_bytes = peer->tx.nwrite - peer->tx.nread;
        snap->peer_tx_capacity = peer->tx.capacity;
        snap->peer_tx_nread = peer->tx.nread;
        snap->peer_tx_nwrite = peer->tx.nwrite;
        snap->peer_scm = chrome_unix_queue_count(peer->scm_head,
                                                 peer->scm_tail,
                                                 UNIX_SCM_QUEUE_MAX);
        snap->peer_packets = chrome_unix_queue_count(peer->packet_head,
                                                     peer->packet_tail,
                                                     UNIX_PACKET_QUEUE_MAX);
        spin_unlock(&peer->lock);
        unix_sock_put_ref(peer);
    }
}

static int chrome_unix_scm_ready_locked(struct unix_sock *sk,
                                        uint *start_out, uint *end_out)
{
    if (sk == NULL || sk->scm_head == sk->scm_tail)
        return 0;

    uint start = sk->scm_queue[sk->scm_head].start_nread;
    uint end = sk->scm_queue[sk->scm_head].end_nread;
    uint nread = sk->tx.nread;

    if (start_out != NULL)
        *start_out = start;
    if (end_out != NULL)
        *end_out = end;
    return (start == end && (int)(nread - start) >= 0) ||
           (start != end && (int)(nread - start) > 0);
}

static void chrome_unix_ipc_trace_state(const char *op, int fd, ssize_t ret,
                                        size_t want, int flags,
                                        uint64 control, uint64 controllen,
                                        size_t cmsg_count, int has_cred,
                                        struct unix_sock *sk)
{
    if (!unix_ipc_trace_process())
        return;

    struct unix_sock *peer = NULL;
    int sk_type = -1, sk_state = -1, sk_shutdown = 0, sk_error = 0;
    int sk_passcred = 0, sk_refcount = 0;
    size_t sk_tx_bytes = 0, sk_tx_capacity = 0, sk_scm = 0, sk_packets = 0;
    uint sk_tx_nread = 0, sk_tx_nwrite = 0;
    int peer_type = -1, peer_state = -1, peer_shutdown = 0, peer_error = 0;
    int peer_passcred = 0, peer_refcount = 0, peer_first_scm_ready = 0;
    size_t peer_tx_bytes = 0, peer_tx_capacity = 0, peer_scm = 0;
    size_t peer_packets = 0;
    uint peer_tx_nread = 0, peer_tx_nwrite = 0;
    uint peer_first_scm_start = 0, peer_first_scm_end = 0;

    if (sk != NULL) {
        spin_lock(&sk->lock);
        sk_type = sk->type;
        sk_state = sk->state;
        sk_shutdown = sk->shutdown_flags;
        sk_error = sk->so_error;
        sk_passcred = sk->passcred;
        sk_refcount = sk->refcount;
        sk_tx_bytes = sk->tx.nwrite - sk->tx.nread;
        sk_tx_capacity = sk->tx.capacity;
        sk_tx_nread = sk->tx.nread;
        sk_tx_nwrite = sk->tx.nwrite;
        sk_scm = chrome_unix_queue_count(sk->scm_head, sk->scm_tail,
                                         UNIX_SCM_QUEUE_MAX);
        sk_packets = chrome_unix_queue_count(sk->packet_head, sk->packet_tail,
                                             UNIX_PACKET_QUEUE_MAX);
        peer = sk->peer;
        unix_sock_get_ref(peer);
        spin_unlock(&sk->lock);
    }

    if (peer != NULL) {
        spin_lock(&peer->lock);
        peer_type = peer->type;
        peer_state = peer->state;
        peer_shutdown = peer->shutdown_flags;
        peer_error = peer->so_error;
        peer_passcred = peer->passcred;
        peer_refcount = peer->refcount;
        peer_tx_bytes = peer->tx.nwrite - peer->tx.nread;
        peer_tx_capacity = peer->tx.capacity;
        peer_tx_nread = peer->tx.nread;
        peer_tx_nwrite = peer->tx.nwrite;
        peer_scm = chrome_unix_queue_count(peer->scm_head, peer->scm_tail,
                                           UNIX_SCM_QUEUE_MAX);
        peer_packets = chrome_unix_queue_count(peer->packet_head,
                                               peer->packet_tail,
                                               UNIX_PACKET_QUEUE_MAX);
        peer_first_scm_ready =
            chrome_unix_scm_ready_locked(peer, &peer_first_scm_start,
                                         &peer_first_scm_end);
        spin_unlock(&peer->lock);
    }

    printf("chrome-unix-ipc: %s pid=%d tgid=%d name=%s fd=%d ret=%ld "
           "want=%lu flags=0x%x control=0x%lx controllen=%lu cmsg=%lu "
           "has_cred=%d sk=%p peer=%p sk_type=%d sk_state=%d "
           "sk_shutdown=0x%x sk_err=%d sk_passcred=%d sk_ref=%d "
           "sk_tx=%lu/%lu sk_marks=%u:%u sk_scm=%lu sk_packets=%lu "
           "peer_type=%d peer_state=%d peer_shutdown=0x%x peer_err=%d "
           "peer_passcred=%d peer_ref=%d peer_tx=%lu/%lu "
           "peer_marks=%u:%u peer_scm=%lu peer_packets=%lu "
           "peer_first_scm=%u:%u ready=%d\n",
           op != NULL ? op : "?", current != NULL ? current->pid : -1,
           current != NULL ? current->tgid : -1,
           current != NULL ? current->name : "?", fd, (long)ret,
           (unsigned long)want, flags, (unsigned long)control,
           (unsigned long)controllen, (unsigned long)cmsg_count, has_cred,
           sk, peer, sk_type, sk_state, sk_shutdown, sk_error, sk_passcred,
           sk_refcount, (unsigned long)sk_tx_bytes,
           (unsigned long)sk_tx_capacity, sk_tx_nread, sk_tx_nwrite,
           (unsigned long)sk_scm, (unsigned long)sk_packets, peer_type,
           peer_state, peer_shutdown, peer_error, peer_passcred, peer_refcount,
           (unsigned long)peer_tx_bytes, (unsigned long)peer_tx_capacity,
           peer_tx_nread, peer_tx_nwrite, (unsigned long)peer_scm,
           (unsigned long)peer_packets, peer_first_scm_start,
           peer_first_scm_end, peer_first_scm_ready);

    unix_sock_put_ref(peer);
}

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
    int            domain;       /* AF_INET / AF_INET6 */
    struct netbuf  *lastbuf;     /* partially consumed recv buffer (UDP) */
    uint16         lastoffset;   /* offset into lastbuf */
    struct pbuf    *lastpbuf;    /* partially consumed TCP recv pbuf */
    uint16         lastpbuf_off; /* bytes already consumed from lastpbuf */
    /* Mirrors lwIP upstream sock->sendevent.  Updated by
     * sock_netconn_callback() in response to NETCONN_EVT_SEND{PLUS,MINUS}.
     * Reset to 0 when the TCP send buffer is full (non-blocking write
     * cannot push all data, or sndbuf <= TCP_SNDLOWAT) and to 1 when
     * lwIP signals that more space is available.  Used by sock_poll_ready()
     * so EPOLLOUT is only reported when the socket is genuinely writable. */
    uint8          sendevent;
    /* lwIP invalidates the TCP recvmbox after an orderly FIN.  The first
     * recv sees ERR_CLSD, but later recvs see ERR_CONN.  POSIX sockets keep
     * returning EOF after FIN, not ENOTCONN, so remember that the RX side
     * reached EOF. */
    uint8          rx_eof;
    int            fd;
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
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK  0x800
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC   0x80000
#endif
#define SOCK_TYPE_MASK 0xF

/* Address family constants */
#define AF_INET        2
#define AF_INET6       10

/* Protocol constants */
#define IPPROTO_IP     0
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define IPPROTO_ICMP   1
#define IPPROTO_IPV6   41

/* Shutdown constants */
#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

/* Message flags */
#define MSG_PEEK       0x02
#define MSG_CTRUNC     0x08
#define MSG_TRUNC      0x20
#define MSG_DONTWAIT      0x40
#define MSG_WAITALL       0x100
#define MSG_CMSG_CLOEXEC  0x40000000
#define MSG_NOSIGNAL      0x4000
#define MSG_WAITFORONE    0x10000

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

/* IPPROTO_IPV6 options */
#define IPV6_V6ONLY         26

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

struct sock_timespec {
    int64 tv_sec;
    int64 tv_nsec;
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

struct k_sockaddr_in6 {
    uint16 sin6_family;
    uint16 sin6_port;      /* network byte order */
    uint32 sin6_flowinfo;
    uchar  sin6_addr[16];
    uint32 sin6_scope_id;
};

#define K_SOCKADDR_IN6_SIZE 28

static void chrome_socket_trace_inet_endpoint(const char *op, int fd,
                                              const struct lwip_sock *sk,
                                              const struct k_sockaddr_in *sa,
                                              int nonblock, err_t err,
                                              int ret)
{
    if (!chrome_socket_trace_enabled() || !chrome_socket_trace_process())
        return;

    const uchar *addr = (const uchar *)&sa->sin_addr;
    const struct netconn *conn = sk != NULL ? sk->conn : NULL;
    printf("chrome-socket-trace: %s pid=%d tgid=%d name=%s fd=%d "
           "dst=%u.%u.%u.%u:%u nb=%d type=%d err=%d errno=%d ret=%d "
           "state=%d flags=0x%x pending=%d sendevent=%d\n",
           op, current->pid, current->tgid, current->name, fd,
           addr[0], addr[1], addr[2], addr[3], ntohs(sa->sin_port),
           nonblock, sk != NULL ? sk->type : -1, err,
           err == ERR_OK ? 0 : lwip_err_to_errno(err), ret,
           conn != NULL ? conn->state : -1,
           conn != NULL ? conn->flags : 0,
           conn != NULL ? conn->pending_err : 0,
           sk != NULL ? sk->sendevent : -1);
}

static int sock_copyin_inet_addr(int domain, uint64 uaddr, int addrlen,
                                 ip_addr_t *out, u16_t *port)
{
    if (out == NULL || port == NULL)
        return -EINVAL;

    if (domain == AF_INET) {
        if (addrlen < K_SOCKADDR_IN_SIZE)
            return -EINVAL;
        struct k_sockaddr_in sa;
        if (vm_copyin(current->vm, &sa, uaddr, sizeof(sa)) < 0)
            return -EFAULT;
        if (sa.sin_family != AF_INET)
            return -EAFNOSUPPORT;
        ip_addr_set_ip4_u32(out, sa.sin_addr);
        *port = ntohs(sa.sin_port);
        return 0;
    }

#if LWIP_IPV6
    if (domain == AF_INET6) {
        if (addrlen < K_SOCKADDR_IN6_SIZE)
            return -EINVAL;
        struct k_sockaddr_in6 sa6;
        struct in6_addr in6;
        ip6_addr_t ip6;

        if (vm_copyin(current->vm, &sa6, uaddr, sizeof(sa6)) < 0)
            return -EFAULT;
        if (sa6.sin6_family != AF_INET6)
            return -EAFNOSUPPORT;

        memmove(in6.s6_addr, sa6.sin6_addr, sizeof(sa6.sin6_addr));
        inet6_addr_to_ip6addr(&ip6, &in6);
        ip_addr_copy_from_ip6(*out, ip6);
        *port = ntohs(sa6.sin6_port);
        return 0;
    }
#endif

    return -EAFNOSUPPORT;
}

static int sock_pack_inet_addr(int domain, const ip_addr_t *addr, u16_t port,
                               char *storage, int *addrlen)
{
    if (addr == NULL || storage == NULL || addrlen == NULL)
        return -EINVAL;

#if LWIP_IPV6
    if (domain == AF_INET6) {
        struct k_sockaddr_in6 sa6;
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(port);

        if (IP_IS_V6(addr)) {
            struct in6_addr in6;
            inet6_addr_from_ip6addr(&in6, ip_2_ip6(addr));
            memmove(sa6.sin6_addr, in6.s6_addr, sizeof(sa6.sin6_addr));
        } else {
            sa6.sin6_addr[10] = 0xff;
            sa6.sin6_addr[11] = 0xff;
            uint32 ip4 = ip_addr_get_ip4_u32(addr);
            memmove(&sa6.sin6_addr[12], &ip4, sizeof(ip4));
        }

        memmove(storage, &sa6, sizeof(sa6));
        *addrlen = K_SOCKADDR_IN6_SIZE;
        return 0;
    }
#endif

    struct k_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = ip_addr_get_ip4_u32(addr);
    memmove(storage, &sa, sizeof(sa));
    *addrlen = K_SOCKADDR_IN_SIZE;
    return 0;
}

static int sock_copyout_inet_addr(int domain, const ip_addr_t *addr, u16_t port,
                                  uint64 uaddr, uint64 uaddrlen)
{
    char storage[K_SOCKADDR_IN6_SIZE];
    int addrlen = 0;
    int ret = sock_pack_inet_addr(domain, addr, port, storage, &addrlen);
    if (ret < 0)
        return ret;

    if (vm_copyout(current->vm, uaddrlen, &addrlen, sizeof(addrlen)) < 0)
        return -EFAULT;
    if (vm_copyout(current->vm, uaddr, storage, addrlen) < 0)
        return -EFAULT;
    return 0;
}

static void chrome_socket_trace_conn_state(const char *op, int fd,
                                           const struct lwip_sock *sk,
                                           short events, short revents)
{
    if (!chrome_socket_trace_enabled() || !chrome_socket_trace_process())
        return;

    const struct netconn *conn = sk != NULL ? sk->conn : NULL;
    printf("chrome-socket-trace: %s pid=%d tgid=%d name=%s fd=%d "
           "events=0x%x revents=0x%x type=%d state=%d flags=0x%x "
           "pending=%d sendevent=%d rx_eof=%d\n",
           op, current->pid, current->tgid, current->name, fd,
           events, revents, sk != NULL ? sk->type : -1,
           conn != NULL ? conn->state : -1,
           conn != NULL ? conn->flags : 0,
           conn != NULL ? conn->pending_err : 0,
           sk != NULL ? sk->sendevent : -1,
           sk != NULL ? sk->rx_eof : -1);
}

static int sock_is_nonblock(int fd, int msg_flags);
static ssize_t sock_tcp_send_common(struct lwip_sock *sk, int fd,
                                    uint64 ubuf, const char *kbuf,
                                    size_t count, int flags, int user);

static int sock_timespec_to_ms(const struct sock_timespec *ts, uint64 *ms)
{
    if (ts == NULL || ms == NULL)
        return -EINVAL;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL)
        return -EINVAL;

    uint64 sec = (uint64)ts->tv_sec;
    if (sec > ((uint64)-1) / 1000ULL)
        return -EINVAL;
    uint64 value = sec * 1000ULL;
    uint64 nsec_ms = ((uint64)ts->tv_nsec + 999999ULL) / 1000000ULL;
    if (value > ((uint64)-1) - nsec_ms)
        return -EINVAL;
    *ms = value + nsec_ms;
    return 0;
}

static uint32 sock_timeout_remaining_ms(uint64 deadline_ms, int *expired)
{
    uint64 now = sched_timer_now_ms();
    if (expired != NULL)
        *expired = 0;
    if (deadline_ms <= now) {
        if (expired != NULL)
            *expired = 1;
        return 0;
    }
    uint64 remaining = deadline_ms - now;
    if (remaining > (uint32)-1)
        return (uint32)-1;
    return (uint32)remaining;
}

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
    /*
     * Match upstream lwIP socket layer: a fresh socket is considered
     * writable (sendevent=1) and becomes non-writable only when lwIP
     * fires NETCONN_EVT_SENDMINUS.  This is correct for SOCK_DGRAM/RAW
     * (always writable) and for SOCK_STREAM where SENDPLUS will refresh
     * after connect() completes anyway.
     */
    sk->sendevent = 1;
    sk->fd = -1;
    return sk;
}

static int lwip_in_tcpip_thread(void)
{
    return current != NULL && strncmp(current->name, TCPIP_THREAD_NAME,
                                      sizeof(current->name)) == 0;
}

static void lwip_sock_free_direct(struct lwip_sock *sk)
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

static void lwip_sock_free_work(struct work_struct *work)
{
    struct lwip_sock *sk = (struct lwip_sock *)work->data;

    lwip_sock_free_direct(sk);
    free_work_struct(work);
}

static void lwip_sock_free(struct lwip_sock *sk)
{
    if (sk == NULL)
        return;

    /*
     * The netconn delete APIs are synchronous wrappers around the tcpip
     * mailbox.  If the last VFS reference is dropped from an lwIP callback,
     * running them inline would make the tcpip thread post to and wait on its
     * own mailbox, permanently stopping all later network API completions.
     */
    if (lwip_in_tcpip_thread()) {
        struct workqueue *wq = vfs_get_deferred_iput_wq();
        struct work_struct *work = NULL;

        if (wq != NULL)
            work = create_work_struct(lwip_sock_free_work, (uint64)sk);
        if (work != NULL && queue_work(wq, work))
            return;
        if (work != NULL)
            free_work_struct(work);

        printf("lwip_sock_free: failed to defer tcpip-thread socket free; "
               "leaking sk=%p conn=%p\n", sk, sk->conn);
        return;
    }

    lwip_sock_free_direct(sk);
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
    if (nonblock || current == NULL || !THREAD_USER_SPACE(current))
        return netconn_recv_tcp_pbuf_flags(sk->conn, p, apiflags);

    /*
     * Keep blocking TCP receive semantics, but do not rely on one infinite
     * mailbox sleep.  This makes the path robust against missed wakeups in
     * the kernel wait/notify plumbing and lets WebKit-sized transfers keep
     * checking signal state without changing the userspace ABI.
     */
    uint32 old_timeout = netconn_get_recvtimeout(sk->conn);
    if (old_timeout != 0)
        return netconn_recv_tcp_pbuf_flags(sk->conn, p, apiflags);

    netconn_set_recvtimeout(sk->conn, 1000);
    for (;;) {
        uint64 start = sched_timer_now_ms();
        err_t err = netconn_recv_tcp_pbuf_flags(sk->conn, p, apiflags);
        uint64 elapsed = sched_timer_now_ms() - start;
        if (err == ERR_TIMEOUT && !signal_pending(current)) {
            continue;
        }
        netconn_set_recvtimeout(sk->conn, old_timeout);
        return err;
    }
}

static err_t sock_tcp_recv_pbuf_timeout(struct lwip_sock *sk, struct pbuf **p,
                                        int nonblock, uint32 timeout_ms)
{
    if (sk == NULL || sk->conn == NULL)
        return ERR_ARG;
    if (nonblock || timeout_ms == 0)
        return sock_tcp_recv_pbuf(sk, p, nonblock);

    uint32 old_timeout = netconn_get_recvtimeout(sk->conn);
    netconn_set_recvtimeout(sk->conn, timeout_ms);
    err_t err = sock_tcp_recv_pbuf(sk, p, 0);
    netconn_set_recvtimeout(sk->conn, old_timeout);
    return err;
}

static err_t sock_netconn_recv_timeout(struct lwip_sock *sk,
                                       struct netbuf **nb,
                                       uint32 timeout_ms)
{
    if (sk == NULL || sk->conn == NULL)
        return ERR_ARG;
    if (timeout_ms == 0)
        return netconn_recv(sk->conn, nb);

    uint32 old_timeout = netconn_get_recvtimeout(sk->conn);
    netconn_set_recvtimeout(sk->conn, timeout_ms);
    err_t err = netconn_recv(sk->conn, nb);
    netconn_set_recvtimeout(sk->conn, old_timeout);
    return err;
}

static void sock_tcp_recvd(struct lwip_sock *sk, size_t len)
{
    if (sk == NULL || sk->conn == NULL || len == 0)
        return;

    (void)netconn_tcp_recvd(sk->conn, len);
}

static void sock_tcp_recvd_accum(struct lwip_sock *sk, size_t *accum,
                                 size_t len, int force)
{
    if (accum == NULL)
        return;
    if (len > 0)
        *accum += len;

    /*
     * With NETCONN_NOAUTORCVD, TCP receive-window credit is our
     * responsibility.  Large HTTPS bodies can otherwise drain one full lwIP
     * receive window into user space before the sender is allowed to continue.
     * Keep the syscall batching benefit, but advertise credit at lwIP's
     * normal window-update granularity.
     */
    if (*accum > 0 &&
        (force || *accum >= TCP_WND_UPDATE_THRESHOLD)) {
        sock_tcp_recvd(sk, *accum);
        *accum = 0;
    }
}

static void sock_tcp_save_remainder(struct lwip_sock *sk, struct pbuf *p,
                                    uint16 consumed)
{
    if (sk == NULL)
        return;

    if (p == NULL) {
        sk->lastpbuf = NULL;
        sk->lastpbuf_off = 0;
        return;
    }

    if (consumed < p->tot_len) {
        sk->lastpbuf = pbuf_free_header(p, consumed);
        sk->lastpbuf_off = 0;
    } else {
        pbuf_free(p);
        sk->lastpbuf = NULL;
        sk->lastpbuf_off = 0;
    }
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
    size_t recvd_acc = 0;

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
                if (err == ERR_CLSD) {
                    sk->rx_eof = 1;
                    sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                    return total > 0 ? (int)total : 0;
                }
                if (err == ERR_CONN && sk->rx_eof) {
                    sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                    return total > 0 ? (int)total : 0;
                }
                if (err == ERR_TIMEOUT && signal_pending(current)) {
                    sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                    return total > 0 ? (int)total : -EINTR;
                }
                if (total > 0 && (err == ERR_WOULDBLOCK || err == ERR_TIMEOUT)) {
                    if (sock_tcp_wait_for_more(sk, nonblock, total,
                                               (size_t)len, &wait_more))
                        continue;
                    break;
                }
                sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
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
                if (copied > 0 || reused_lastpbuf)
                    sock_tcp_save_remainder(sk, p, poff + copied);
                else
                    pbuf_free(p);
                if (recvd_acc > 0)
                    sock_tcp_recvd(sk, recvd_acc);
                return total > 0 ? (int)total : -EFAULT;
            }
            copied += got;
        }

        if (flags & MSG_PEEK) {
            sk->lastpbuf = p;
            sk->lastpbuf_off = poff;
        } else {
            sock_tcp_save_remainder(sk, p, poff + copied);
        }

        if (!(flags & MSG_PEEK))
            sock_tcp_recvd_accum(sk, &recvd_acc, copied, 0);

        total += copied;
        if (copied == 0 || (flags & MSG_PEEK))
            break;
    }

    /*
     * Batch the netconn_tcp_recvd() call across the whole recv() so we
     * post a single MSG_RECVD to tcpip_thread instead of one per pbuf
     * consumed.  Each MSG_RECVD is a tcpip-mbox round-trip plus a
     * tcpip_thread wake-up; for a streaming receiver pulling MTU-sized
     * packets that round-trip dominated end-to-end latency and capped
     * single-stream throughput well below link rate.
     */
    sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);


    if (!(flags & MSG_PEEK) && total > 0) {
        sock_notify_if_still_readable(fd, sk);
    }

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

    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) {
        if (sock_has_rx_data(sk)) {
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        }
        if (sk->type == SOCK_STREAM && sk->rx_eof) {
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
            revents |= POLLHUP;
            if (events & POLLRDHUP)
                revents |= POLLRDHUP;
        }
        if (conn->pending_err != ERR_OK)
            revents |= POLLERR;
        if (conn->flags & NETCONN_FLAG_MBOXCLOSED) {
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
            revents |= POLLHUP;
            if (events & POLLRDHUP)
                revents |= POLLRDHUP;
        }
    }

    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
        /*
         * EPOLLOUT must reflect actual TCP send-buffer availability, not be
         * unconditionally true.  Returning POLLOUT every wake makes GLib's
         * epoll-driven main loop spin servicing fake write readiness on
         * sockets registered for read+write, starving the read drain that
         * delivers large response bodies (observed for desktop YouTube's
         * 9.7 MB ytmainappweb script).  Mirror lwIP's upstream sockets.c:
         * track sendevent via NETCONN_EVT_SEND{PLUS,MINUS} and gate POLLOUT
         * on it.  Always report POLLOUT in error/closed states so writers
         * surface EPIPE/ECONNRESET promptly.
         */
        if (conn->pending_err != ERR_OK ||
            (conn->flags & NETCONN_FLAG_MBOXCLOSED)) {
            revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
        } else if (sk->sendevent) {
            revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
        }
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
                    if (err == ERR_CLSD) {
                        sk->rx_eof = 1;
                        return total > 0 ? total : 0;
                    }
                    if (err == ERR_CONN && sk->rx_eof)
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

            sock_tcp_save_remainder(sk, p, poff + copied);
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

        size_t datagram_len = netbuf_len(nb);
        size_t tocopy = count < datagram_len ? count : datagram_len;
        char tmpbuf[1500];
        size_t copied = 0;

        while (copied < tocopy) {
            size_t chunk = tocopy - copied;
            if (chunk > sizeof(tmpbuf))
                chunk = sizeof(tmpbuf);
            uint16 got = netbuf_copy_partial(nb, tmpbuf, (uint16)chunk,
                                             (uint16)copied);
            if (got == 0)
                break;
            if (user) {
                if (vm_copyout(current->vm, (uint64)buf + copied,
                               tmpbuf, got) < 0) {
                    netbuf_delete(nb);
                    return -EFAULT;
                }
            } else {
                memmove((char *)buf + copied, tmpbuf, got);
            }
            copied += got;
        }
        netbuf_delete(nb);
        return copied;
    }
}

static ssize_t sock_file_write(struct vfs_file *file, const char *buf,
                               size_t count, bool user)
{
    struct lwip_sock *sk = (struct lwip_sock *)file->private_data;
    if (sk == NULL || sk->conn == NULL)
        return -EBADF;

    if (sk->type == SOCK_STREAM) {
        return sock_tcp_send_common(sk, sk->fd, (uint64)buf, buf, count,
                                    0, user ? 1 : 0);

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

static int sock_file_set_flags(struct vfs_file *file, int old_flags,
                               int new_flags)
{
    struct lwip_sock *sk = (struct lwip_sock *)file->private_data;
    int old_nonblock = (old_flags & O_NONBLOCK) != 0;
    int new_nonblock = (new_flags & O_NONBLOCK) != 0;

    if (sk == NULL || sk->conn == NULL)
        return -EBADF;

    if (old_nonblock != new_nonblock)
        netconn_set_nonblocking(sk->conn, new_nonblock);
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

    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) {
        if (sock_has_rx_data(sk)) {
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        }
    }

    /* POLLERR and POLLHUP are output-only — always check regardless of events */
    if (conn->pending_err != ERR_OK) {
        revents |= POLLERR;
    }
    if (sk->type == SOCK_STREAM && sk->rx_eof) {
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        revents |= POLLHUP;
        if (events & POLLRDHUP)
            revents |= POLLRDHUP;
    }
    if (conn->flags & NETCONN_FLAG_MBOXCLOSED) {
        revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        revents |= POLLHUP;
        if (events & POLLRDHUP)
            revents |= POLLRDHUP;
    }

    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
        /*
         * Report write readiness only when lwIP has send space available.
         * This matches sock_poll_ready() and lwIP's own sockets layer, and
         * avoids epoll/kqueue loops being flooded by permanently ready write
         * knotes on TCP sockets that are registered for both directions.
         */
        if (conn->pending_err != ERR_OK ||
            (conn->flags & NETCONN_FLAG_MBOXCLOSED) ||
            sk->sendevent) {
            revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
        }
    }

    if (sk->type == SOCK_STREAM &&
        ((events & (POLLOUT | POLLWRNORM | POLLWRBAND)) ||
         conn->state == NETCONN_CONNECT || conn->pending_err != ERR_OK)) {
        static int trace_count;
        if (trace_count < 512) {
            trace_count++;
            chrome_socket_trace_conn_state("poll", sk->fd, sk, events,
                                           revents);
        }
    }

    /* Periodic trace for POLLOUT during connect (throttled) */
    static uint64 last_poll_log;
    uint64 now = r_time();
    if ((events & POLLOUT) && conn->state == NETCONN_CONNECT &&
        (now - last_poll_log > 2000000000ULL)) {
        last_poll_log = now;
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
    .flags   = VFS_FILE_OPS_F_POLL_NOTIFY_BACKED,
    .read    = sock_file_read,
    .write   = sock_file_write,
    .llseek  = NULL,
    .release = sock_file_release,
    .fsync   = NULL,
    .fflush  = NULL,
    .poll    = sock_file_poll,
    .ioctl   = sock_file_ioctl,
    .set_flags = sock_file_set_flags,
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

    /*
     * sk lookup: file->private_data is the lwip_sock if the file is a socket.
     * The vfs_file_ops pointer matches lwip_socket_file_ops only for our
     * sockets, so this is safe.
     */
    struct lwip_sock *sk = (file->ops == &lwip_socket_file_ops)
                               ? (struct lwip_sock *)file->private_data
                               : NULL;

    if (sk != NULL && sk->type == SOCK_STREAM) {
        static int trace_count;
        if (chrome_socket_trace_enabled() && trace_count < 512) {
            trace_count++;
            printf("chrome-socket-trace: callback pid=%d tgid=%d name=%s "
                   "fd=%d evt=%s len=%u state=%d flags=0x%x pending=%d "
                   "sendevent=%d\n",
                   current != NULL ? current->pid : -1,
                   current != NULL ? current->tgid : -1,
                   current != NULL ? current->name : "(null)",
                   sk->fd, chrome_socket_trace_evt_name(evt),
                   len, conn->state, conn->flags, conn->pending_err,
                   sk->sendevent);
        }
    }

    switch (evt) {
    case NETCONN_EVT_RCVPLUS:
        if (sk != NULL && sk->type == SOCK_STREAM && len == 0 &&
            conn->state != NETCONN_LISTEN && conn->pending_err == ERR_OK)
            sk->rx_eof = 1;
        vfs_file_knote_notify(file, EVFILT_READ, (int64)len);
        break;
    case NETCONN_EVT_SENDPLUS:
        if (sk != NULL)
            sk->sendevent = 1;
        vfs_file_knote_notify(file, EVFILT_WRITE, (int64)len);
        break;
    case NETCONN_EVT_SENDMINUS:
        /*
         * lwIP signals SENDMINUS when the TCP send buffer fills (or a
         * non-blocking sendto() could not enqueue everything).  Clear
         * sendevent so subsequent sock_poll_ready() calls stop reporting
         * POLLOUT until SENDPLUS arrives.  Do NOT notify EVFILT_WRITE here
         * (becoming non-writable is not a wake event).
         */
        if (sk != NULL)
            sk->sendevent = 0;
        break;
    case NETCONN_EVT_ERROR:
        if (sk != NULL)
            sk->sendevent = 1; /* surface error to writers via POLLOUT */
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
                         int domain, int file_flags)
{
    struct lwip_sock *sk = lwip_sock_alloc();
    if (sk == NULL)
        return -ENOMEM;

    sk->conn = conn;
    sk->type = type;
    sk->protocol = protocol;
    sk->domain = domain;

    /* Propagate O_NONBLOCK to the lwIP netconn layer so that
     * netconn_connect() etc. return ERR_INPROGRESS instead of blocking. */
    if (file_flags & O_NONBLOCK)
        netconn_set_nonblocking(conn, 1);

    int fd = vfs_custom_fd_alloc(&lwip_socket_file_ops, sk, file_flags);
    if (fd < 0) {
        lwip_sock_free(sk);
        return fd;
    }
    sk->fd = fd;

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
 * Returns AF_INET (2), AF_INET6 (10), AF_UNIX (1), AF_NETLINK (16), or 0 if
 * not a socket.
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
    if (f->ops == &lwip_socket_file_ops) {
        struct lwip_sock *sk = (struct lwip_sock *)f->private_data;
        return sk != NULL && sk->domain != 0 ? sk->domain : AF_INET;
    }
    if (f->ops == &unix_socket_file_ops)
        return AF_UNIX;
    if (f->ops == &netlink_socket_file_ops)
        return AF_NETLINK;
    return 0;
}

static int sock_fd_type_error(int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return -EBADF;

    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = current->fdtable->files[fd];
    spin_unlock(&current->fdtable->lock);

    return f == NULL ? -EBADF : -ENOTSOCK;
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

static void chrome_socket_trace_tcp_io(const char *op, struct lwip_sock *sk,
                                       int fd, size_t requested,
                                       ssize_t result, int flags,
                                       int user, uint64 first0,
                                       uint64 first1)
{
    if (!chrome_socket_trace_enabled() || !chrome_socket_trace_process())
        return;

    const struct netconn *conn = sk != NULL ? sk->conn : NULL;
    printf("chrome-socket-trace: %s pid=%d tgid=%d name=%s fd=%d "
           "requested=%lu result=%ld flags=0x%x user=%d type=%d state=%d "
           "conn_flags=0x%x pending=%d sendevent=%d rx_eof=%d "
           "first=0x%lx/0x%lx\n",
           op, current->pid, current->tgid, current->name, fd,
           (unsigned long)requested, (long)result, flags, user,
           sk != NULL ? sk->type : -1,
           conn != NULL ? conn->state : -1,
           conn != NULL ? conn->flags : 0,
           conn != NULL ? conn->pending_err : 0,
           sk != NULL ? sk->sendevent : -1,
           sk != NULL ? sk->rx_eof : -1,
           first0, first1);
}

static ssize_t sock_tcp_send_common(struct lwip_sock *sk, int fd,
                                    uint64 ubuf, const char *kbuf,
                                    size_t count, int flags, int user)
{
    char tmpbuf[1500];
    size_t written = 0;
    int nonblock;
    uint64 first0 = 0;
    uint64 first1 = 0;
    int have_first = 0;

    if (sk == NULL || sk->conn == NULL) {
        chrome_socket_trace_tcp_io("tcp-send", sk, fd, count, -EBADF,
                                   flags, user, 0, 0);
        return -EBADF;
    }
    if (count == 0) {
        chrome_socket_trace_tcp_io("tcp-send", sk, fd, count, 0,
                                   flags, user, 0, 0);
        return 0;
    }

    nonblock = sock_is_nonblock(fd, flags);

    while (written < count) {
        size_t chunk = count - written;
        size_t chunk_written = 0;
        err_t err;

        if (chunk > sizeof(tmpbuf))
            chunk = sizeof(tmpbuf);

        if (user) {
            if (vm_copyin(current->vm, tmpbuf, ubuf + written, chunk) < 0)
                return written > 0 ? (ssize_t)written : -EFAULT;
        } else {
            memmove(tmpbuf, kbuf + written, chunk);
        }
        if (!have_first) {
            size_t first_len = chunk < sizeof(first0) ? chunk : sizeof(first0);
            memmove(&first0, tmpbuf, first_len);
            if (chunk > sizeof(first0)) {
                size_t second_len = chunk - sizeof(first0);
                if (second_len > sizeof(first1))
                    second_len = sizeof(first1);
                memmove(&first1, tmpbuf + sizeof(first0), second_len);
            }
            have_first = 1;
        }

        u8_t apiflags = NETCONN_COPY;
        if (nonblock)
            apiflags |= NETCONN_DONTBLOCK;


        err = netconn_write_partly(sk->conn, tmpbuf, chunk, apiflags,
                                   &chunk_written);
        if (err != ERR_OK) {
            int posix = lwip_err_to_errno(err);


            if (err == ERR_WOULDBLOCK || err == ERR_TIMEOUT)
                posix = EAGAIN;
            if (written > 0) {
                chrome_socket_trace_tcp_io("tcp-send", sk, fd, count,
                                           (ssize_t)written, flags, user,
                                           first0, first1);
                return (ssize_t)written;
            }
            chrome_socket_trace_tcp_io("tcp-send", sk, fd, count, -posix,
                                       flags, user, first0, first1);
            return -posix;
        }

        if (chunk_written == 0) {
            if (written > 0) {
                chrome_socket_trace_tcp_io("tcp-send", sk, fd, count,
                                           (ssize_t)written, flags, user,
                                           first0, first1);
                return (ssize_t)written;
            }
            ssize_t ret = nonblock ? -EAGAIN : -EPIPE;
            chrome_socket_trace_tcp_io("tcp-send", sk, fd, count, ret,
                                       flags, user, first0, first1);
            return ret;
        }

        written += chunk_written;

        if (chunk_written < chunk) {
            if (nonblock)
                break;
        }

        if (signal_pending(current)) {
            ssize_t ret = written > 0 ? (ssize_t)written : -EINTR;
            chrome_socket_trace_tcp_io("tcp-send", sk, fd, count, ret,
                                       flags, user, first0, first1);
            return written > 0 ? (ssize_t)written : -EINTR;
        }
    }

    chrome_socket_trace_tcp_io("tcp-send", sk, fd, count, (ssize_t)written,
                               flags, user, first0, first1);
    return (ssize_t)written;
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

static enum netconn_type posix_to_netconn_type(int domain, int type,
                                               int protocol)
{
    (void)protocol;
    if (domain == AF_INET6) {
#if LWIP_IPV6
        switch (type) {
        case SOCK_STREAM:  return NETCONN_TCP_IPV6;
        case SOCK_DGRAM:   return NETCONN_UDP_IPV6;
        case SOCK_RAW:     return NETCONN_RAW_IPV6;
        default:           return NETCONN_INVALID;
        }
#else
        return NETCONN_INVALID;
#endif
    }

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

    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return (uint64)-EINVAL;

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

    /* ---- AF_INET / AF_INET6 (lwIP) ---- */
    if (domain != AF_INET && domain != AF_INET6)
        return (uint64)-EAFNOSUPPORT;

    enum netconn_type ntype = posix_to_netconn_type(domain, type, protocol);
    if (ntype == NETCONN_INVALID)
        return (uint64)-EPROTOTYPE;

    struct netconn *conn = netconn_new(ntype);
    if (conn == NULL)
        return (uint64)-ENOMEM;

    int fd = sock_fd_alloc(conn, type, protocol, domain, file_flags);
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

    ip_addr_t ipaddr;
    u16_t port;
    int addr_ret = sock_copyin_inet_addr(domain, uaddr, addrlen, &ipaddr,
                                         &port);
    if (addr_ret < 0)
        return (uint64)addr_ret;

    err_t err = netconn_bind(sk->conn, &ipaddr, port);
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

    int newfd = sock_fd_alloc(newconn, SOCK_STREAM, sk->protocol, sk->domain,
                              O_RDWR);
    if (newfd < 0) {
        netconn_delete(newconn);
        return (uint64)newfd;
    }

    /* Fill in remote address if requested */
    if (uaddr != 0 && uaddrlen != 0) {
        ip_addr_t raddr;
        u16_t rport;
        netconn_peer(newconn, &raddr, &rport);

        int out_ret = sock_copyout_inet_addr(sk->domain, &raddr, rport,
                                             uaddr, uaddrlen);
        if (out_ret < 0)
            return (uint64)out_ret;
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

    ip_addr_t ipaddr;
    u16_t port;
    int addr_ret = sock_copyin_inet_addr(domain, uaddr, addrlen, &ipaddr,
                                         &port);
    if (addr_ret < 0)
        return (uint64)addr_ret;

    uint32 ip4 = IP_IS_V4(&ipaddr) ? ip_addr_get_ip4_u32(&ipaddr) : 0;

    /*
     * For non-blocking TCP connects, mark the socket non-writable before
     * asking lwIP to start the handshake.  Loopback/Hyper-V paths can complete
     * quickly enough for NETCONN_EVT_SENDPLUS to fire before netconn_connect()
     * returns; clearing sendevent after ERR_INPROGRESS would lose that wake and
     * leave epoll users waiting forever for POLLOUT.
     */
    if (is_nb && sk->type == SOCK_STREAM)
        sk->sendevent = 0;

    err_t err = netconn_connect(sk->conn, &ipaddr, port);


    if (err == ERR_INPROGRESS) {
        /*
         * Non-blocking connect: TCP handshake started, not yet complete.
         * A newly-created TCP socket is initially writable, but POSIX poll
         * semantics require an in-progress connect to become writable only
         * when the connection completes or fails. lwIP reports that transition
         * through SENDPLUS/ERROR callbacks, which restore sendevent.
         */
        if (sk->conn->state == NETCONN_NONE)
            sk->sendevent = 1;
        ACCT_INC(current->thread_group, net_connects);
        if (domain == AF_INET) {
            struct k_sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(port);
            sa.sin_addr = ip4;
            chrome_socket_trace_inet_endpoint("connect", fd, sk, &sa, is_nb,
                                              err, -EINPROGRESS);
        }
        return (uint64)-EINPROGRESS;
    }
    if (err == ERR_OK)
        sk->sendevent = 1;
    if (err != ERR_OK) {
        if (domain == AF_INET) {
            struct k_sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(port);
            sa.sin_addr = ip4;
            chrome_socket_trace_inet_endpoint("connect", fd, sk, &sa, is_nb,
                                              err, -lwip_err_to_errno(err));
        }
        return (uint64)-lwip_err_to_errno(err);
    }

    ACCT_INC(current->thread_group, net_connects);
    if (domain == AF_INET) {
        struct k_sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr = ip4;
        chrome_socket_trace_inet_endpoint("connect", fd, sk, &sa, is_nb,
                                          err, 0);
    }
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
    /* AF_UNIX: send()/sendto(NULL) on a connected socket works like write. */
    if (domain == AF_UNIX) {
        if (uaddr != 0 || addrlen != 0)
            return (uint64)-EOPNOTSUPP;
        if (len < 0)
            return (uint64)-EINVAL;
        if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL))
            return (uint64)-EOPNOTSUPP;

        struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
        if (f == NULL || f->ops != &unix_socket_file_ops) {
            if (f != NULL)
                vfs_fput(f);
            return (uint64)-EBADF;
        }

        struct unix_sock *usk = (struct unix_sock *)f->private_data;
        if (usk == NULL) {
            vfs_fput(f);
            return (uint64)-EBADF;
        }
        if (len == 0 && usk->type == SOCK_STREAM) {
            vfs_fput(f);
            return 0;
        }

        int saved_fflags = f->f_flags;
        if (flags & MSG_DONTWAIT)
            f->f_flags |= O_NONBLOCK;
        ssize_t n = unix_socket_file_ops.write(f, (const char *)ubuf,
                                               (size_t)len, true);
        f->f_flags = saved_fflags;
        vfs_fput(f);
        return (uint64)n;
    }

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)sock_fd_type_error(fd);

    if (len <= 0)
        return 0;

    /*
     * For TCP, attempt the write and let lwIP return ERR_WOULDBLOCK or a
     * partial byte count.  The cached sendevent bit is for readiness
     * notification, not an authority for rejecting real sends.
     */
    if (sk->type != SOCK_STREAM && sock_is_nonblock(fd, flags)) {
        int ready = sock_poll_ready(sk, POLLOUT);
        if (!(ready & POLLOUT))
            return (uint64)-EAGAIN;
    }

    if (sk->type == SOCK_STREAM) {
        /* TCP: ignore dest_addr and use Linux-like blocking send semantics. */
        ssize_t n = sock_tcp_send_common(sk, fd, ubuf, NULL, (size_t)len,
                                         flags, 1);
        if (n > 0)
            ACCT_ADD(current->thread_group, net_bytes_sent, (uint64)n);
        return (uint64)n;

    } else {
        /* UDP/RAW: send one complete datagram. */
        if (len > 65535)
            return (uint64)-EMSGSIZE;

        struct netbuf *nb = netbuf_new();
        if (nb == NULL)
            return (uint64)-ENOMEM;

        uint16 sendlen = (uint16)len;
        void *data = netbuf_alloc(nb, sendlen);
        if (data == NULL) {
            netbuf_delete(nb);
            return (uint64)-ENOMEM;
        }

        if (vm_copyin(current->vm, data, ubuf, sendlen) < 0) {
            netbuf_delete(nb);
            return (uint64)-EFAULT;
        }

        err_t err;
        if (uaddr != 0) {
            ip_addr_t destip;
            u16_t port;
            int addr_ret = sock_copyin_inet_addr(domain, uaddr, addrlen,
                                                 &destip, &port);
            if (addr_ret < 0) {
                netbuf_delete(nb);
                return (uint64)addr_ret;
            }
            err = netconn_sendto(sk->conn, nb, &destip, port);
        } else {
            err = netconn_send(sk->conn, nb);
        }
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
    if (domain == AF_UNIX) {
        if (len <= 0)
            return 0;
        if (flags & ~(MSG_DONTWAIT))
            return (uint64)-EOPNOTSUPP;

        struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
        if (f == NULL || f->ops != &unix_socket_file_ops) {
            if (f != NULL)
                vfs_fput(f);
            return (uint64)-EBADF;
        }

        int saved_fflags = f->f_flags;
        if (flags & MSG_DONTWAIT)
            f->f_flags |= O_NONBLOCK;
        ssize_t n = unix_socket_file_ops.read(f, (char *)ubuf,
                                              (size_t)len, true);
        f->f_flags = saved_fflags;

        if (n >= 0 && uaddrlen != 0) {
            int zero = 0;
            (void)vm_copyout(current->vm, uaddrlen, &zero, sizeof(zero));
        }

        vfs_fput(f);
        return (uint64)n;
    }

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)sock_fd_type_error(fd);

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
                sock_tcp_save_remainder(sk, p, sk->lastpbuf_off + clen);
                sock_tcp_recvd(sk, clen);
            }
            ACCT_ADD(current->thread_group, net_bytes_recv, clen);
            return (uint64)clen;
        }

        err_t err = sock_tcp_recv_pbuf(sk, &p, sock_is_nonblock(fd, flags));
        if (err != ERR_OK) {
            if (err == ERR_CLSD) {
                sk->rx_eof = 1;
                return 0; /* EOF */
            }
            if (err == ERR_CONN && sk->rx_eof)
                return 0; /* EOF after FIN */
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
            sock_tcp_save_remainder(sk, p, clen);
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

        size_t datagram_len = netbuf_len(nb);
        size_t tocopy = (size_t)len < datagram_len ? (size_t)len : datagram_len;
        char tmpbuf[1500];
        size_t copied = 0;

        while (copied < tocopy) {
            size_t chunk = tocopy - copied;
            if (chunk > sizeof(tmpbuf))
                chunk = sizeof(tmpbuf);
            uint16 got = netbuf_copy_partial(nb, tmpbuf, (uint16)chunk,
                                             (uint16)copied);
            if (got == 0)
                break;
            if (vm_copyout(current->vm, ubuf + copied, tmpbuf, got) < 0) {
                if (sk->lastbuf == NULL)
                    netbuf_delete(nb);
                return (uint64)-EFAULT;
            }
            copied += got;
        }

        /* Fill in source address if requested */
        if (uaddr != 0 && uaddrlen != 0) {
            const ip_addr_t *fromaddr = netbuf_fromaddr(nb);
            u16_t fromport = netbuf_fromport(nb);

            int out_ret = sock_copyout_inet_addr(domain, fromaddr, fromport,
                                                 uaddr, uaddrlen);
            if (out_ret < 0) {
                if (sk->lastbuf == NULL)
                    netbuf_delete(nb);
                return (uint64)out_ret;
            }
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
        ACCT_ADD(current->thread_group, net_bytes_recv, copied);
        if ((flags & MSG_TRUNC) && datagram_len > copied)
            return (uint64)datagram_len;
        return (uint64)copied;
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

    if (level == IPPROTO_IPV6) {
        switch (optname) {
        case IPV6_V6ONLY:
#if LWIP_IPV6
            if (sk->domain != AF_INET6)
                return (uint64)-EINVAL;
            if (val)
                netconn_set_ipv6only(sk->conn, 1);
            else
                netconn_set_ipv6only(sk->conn, 0);
#endif
            return 0;
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
            CHROME_SOCKET_TRACE("getsockopt-so-error pid=%d tgid=%d "
                                "name=%s fd=%d val=%d state=%d flags=0x%x "
                                "sendevent=%d\n",
                                current->pid, current->tgid, current->name,
                                fd, val, sk->conn->state, sk->conn->flags,
                                sk->sendevent);
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
        case SO_DOMAIN:
            val = sk->domain;
            break;
        case SO_PROTOCOL:
            val = sk->protocol;
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
    } else if (level == IPPROTO_IPV6) {
        switch (optname) {
        case IPV6_V6ONLY:
#if LWIP_IPV6
            val = sk->domain == AF_INET6 ? netconn_get_ipv6only(sk->conn) : 0;
#else
            val = 0;
#endif
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

    return (uint64)sock_copyout_inet_addr(sk->domain, &raddr, rport,
                                          uaddr, uaddrlen);
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

    return (uint64)sock_copyout_inet_addr(sk->domain, &laddr, lport,
                                          uaddr, uaddrlen);
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
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return (uint64)-EINVAL;

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
    newsk->domain = sk->domain;

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

        int out_ret = sock_copyout_inet_addr(sk->domain, &raddr, rport,
                                             uaddr, uaddrlen);
        if (out_ret < 0)
            return (uint64)out_ret;
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
    uint64 msg_controllen;  /* size_t                  */
    int    msg_flags;       /* int                     */
};

struct k_iovec {
    uint64 iov_base;        /* void *  */
    uint64 iov_len;         /* size_t  */
};

/* Kernel-space cmsghdr — must match LP64 POSIX layout (musl) */
struct k_cmsghdr {
    uint64 cmsg_len;        /* data byte count incl. header */
    int    cmsg_level;
    int    cmsg_type;
    /* followed by cmsg data */
};

#define SCM_RIGHTS       1
#define SCM_CREDENTIALS  2
#define K_CMSG_ALIGN(n)  (((n) + sizeof(uint64) - 1) & ~(sizeof(uint64) - 1))
#define K_CMSG_DATA(cmsg) ((unsigned char *)((struct k_cmsghdr *)(cmsg) + 1))
#define K_CMSG_LEN(len)   (K_CMSG_ALIGN(sizeof(struct k_cmsghdr)) + (len))
#define K_CMSG_SPACE(len) (K_CMSG_ALIGN(sizeof(struct k_cmsghdr)) + K_CMSG_ALIGN(len))
#define K_CMSG_PARSE_MAX  4096

#define SENDMSG_MAX_IOV 16

static uint64 sys_recvmsg_netlink(int fd, uint64 umsg, int flags)
{
    struct k_msghdr mh;
    if (vm_copyin(current->vm, &mh, umsg, sizeof(mh)) < 0)
        return (uint64)-EFAULT;
    if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
        return (uint64)-EINVAL;

    struct k_iovec iovs[SENDMSG_MAX_IOV];
    uint64 iov_bytes = (uint64)mh.msg_iovlen * sizeof(struct k_iovec);
    if (iov_bytes != 0 &&
        vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
        return (uint64)-EFAULT;

    size_t total_len = 0;
    for (int i = 0; i < mh.msg_iovlen; i++) {
        if (iovs[i].iov_len > PAGE_SIZE ||
            total_len > PAGE_SIZE - iovs[i].iov_len)
            return (uint64)-EMSGSIZE;
        total_len += iovs[i].iov_len;
    }

    char *buf = kvmalloc(total_len ? total_len : 1);
    if (buf == NULL)
        return (uint64)-ENOMEM;

    int ret = netlink_sock_recv(fd, (uint64)buf, total_len, flags,
                                mh.msg_name,
                                umsg + __builtin_offsetof(struct k_msghdr,
                                                          msg_namelen),
                                false);
    if (ret < 0) {
        kvfree(buf);
        return (uint64)ret;
    }

    size_t copied = 0;
    for (int i = 0; i < mh.msg_iovlen && copied < (size_t)ret; i++) {
        size_t n = iovs[i].iov_len;
        if (n > (size_t)ret - copied)
            n = (size_t)ret - copied;
        if (n != 0 &&
            vm_copyout(current->vm, iovs[i].iov_base, buf + copied, n) < 0) {
            kvfree(buf);
            return (uint64)-EFAULT;
        }
        copied += n;
    }
    kvfree(buf);

    int msg_flags = 0;
    if (vm_copyout(current->vm,
                   umsg + __builtin_offsetof(struct k_msghdr, msg_flags),
                   &msg_flags, sizeof(msg_flags)) < 0)
        return (uint64)-EFAULT;
    return (uint64)ret;
}

static uint64 sys_sendmsg_netlink(int fd, uint64 umsg, int flags)
{
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL))
        return (uint64)-EOPNOTSUPP;

    struct k_msghdr mh;
    if (vm_copyin(current->vm, &mh, umsg, sizeof(mh)) < 0)
        return (uint64)-EFAULT;
    if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
        return (uint64)-EINVAL;

    struct k_iovec iovs[SENDMSG_MAX_IOV];
    uint64 iov_bytes = (uint64)mh.msg_iovlen * sizeof(struct k_iovec);
    if (iov_bytes != 0 &&
        vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
        return (uint64)-EFAULT;

    size_t total_len = 0;
    for (int i = 0; i < mh.msg_iovlen; i++) {
        if (iovs[i].iov_len > PAGE_SIZE ||
            total_len > PAGE_SIZE - iovs[i].iov_len)
            return (uint64)-EMSGSIZE;
        total_len += iovs[i].iov_len;
    }

    char *buf = kvmalloc(total_len ? total_len : 1);
    if (buf == NULL)
        return (uint64)-ENOMEM;

    size_t copied = 0;
    for (int i = 0; i < mh.msg_iovlen; i++) {
        if (iovs[i].iov_len == 0)
            continue;
        if (vm_copyin(current->vm, buf + copied, iovs[i].iov_base,
                      iovs[i].iov_len) < 0) {
            kvfree(buf);
            return (uint64)-EFAULT;
        }
        copied += iovs[i].iov_len;
    }

    struct vfs_file *file = vfs_fdtable_get_file(current->fdtable, fd);
    if (file == NULL) {
        kvfree(buf);
        return (uint64)-EBADF;
    }
    if (file->ops != &netlink_socket_file_ops) {
        vfs_fput(file);
        kvfree(buf);
        return (uint64)-EBADF;
    }

    ssize_t ret = netlink_socket_file_ops.write(file, buf, total_len, false);
    vfs_fput(file);
    kvfree(buf);
    return (uint64)ret;
}

struct k_ucred {
    int pid;
    uint32 uid;
    uint32 gid;
};

#define CHROME_UNIX_IPC_PAYLOAD_TRACE_MAX 512
#define CHROME_UNIX_IPC_PAYLOAD_TOKEN_MAX 128

static int chrome_unix_ipc_payload_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("chrome_unix_ipc_payload_trace", value,
                                    sizeof(value)) == 0 &&
            value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static void chrome_unix_ipc_payload_sanitize_ascii(char *dst, size_t dst_len,
                                                   const char *src,
                                                   size_t src_len)
{
    size_t n;

    if (dst_len == 0)
        return;
    n = src_len;
    if (n > dst_len - 1)
        n = dst_len - 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\') ?
            (char)c : '.';
    }
    dst[n] = '\0';
}

static void chrome_unix_ipc_payload_extract_token(const char *buf, size_t len,
                                                  const char *prefix,
                                                  char *out, size_t out_len)
{
    size_t prefix_len;
    const char *hit;
    size_t pos;
    size_t value_len;
    size_t arg_off;
    const char *arg_start;

    if (out_len == 0)
        return;
    if (out_len == 1) {
        out[0] = '\0';
        return;
    }
    out[0] = '-';
    out[1] = '\0';
    if (buf == NULL || prefix == NULL)
        return;

    prefix_len = strlen(prefix);
    hit = memmem(buf, len, prefix, prefix_len);
    if (hit == NULL)
        return;
    hit += prefix_len;

    pos = 0;
    value_len = len - (size_t)(hit - buf);
    arg_start = hit - prefix_len;
    arg_off = (size_t)(arg_start - buf);
    if (arg_off >= sizeof(uint32)) {
        const unsigned char *lenp =
            (const unsigned char *)(arg_start - sizeof(uint32));
        uint32 arg_len = (uint32)lenp[0] | ((uint32)lenp[1] << 8) |
            ((uint32)lenp[2] << 16) | ((uint32)lenp[3] << 24);
        size_t arg_avail = len - arg_off;

        if (arg_len >= prefix_len && arg_len <= arg_avail)
            value_len = arg_len - prefix_len;
    }

    while (pos < value_len && pos + 1 < out_len) {
        unsigned char c = (unsigned char)hit[pos];

        if (c <= 0x20 || c > 0x7e || c == '"' || c == '\\')
            break;
        out[pos] = (char)c;
        pos++;
    }
    out[pos] = '\0';
    if (pos == 0) {
        out[0] = '-';
        out[1] = '\0';
    }
}

static void chrome_unix_ipc_trace_payload_bytes(const char *op, int fd,
                                                ssize_t ret, size_t total,
                                                int flags, size_t cmsg_count,
                                                int has_cred,
                                                const char *buf, size_t len)
{
    static const char hexchars[] = "0123456789abcdef";
    char hex[CHROME_UNIX_IPC_PAYLOAD_TRACE_MAX * 2 + 1];
    char ascii[CHROME_UNIX_IPC_PAYLOAD_TRACE_MAX + 1];
    char type[CHROME_UNIX_IPC_PAYLOAD_TOKEN_MAX];
    char initial_client_fd[32];
    char utility_sub_type[CHROME_UNIX_IPC_PAYLOAD_TOKEN_MAX];
    char service_sandbox_type[CHROME_UNIX_IPC_PAYLOAD_TOKEN_MAX];
    char shared_files[CHROME_UNIX_IPC_PAYLOAD_TOKEN_MAX];
    char mojo_channel_handle[32];
    char renderer_client_id[32];
    char gpu_client_id[32];
    char render_node[64];
    char metrics_shmem[CHROME_UNIX_IPC_PAYLOAD_TOKEN_MAX];
    char field_trial[CHROME_UNIX_IPC_PAYLOAD_TOKEN_MAX];
    char trace_uuid[64];
    char time_ticks[64];
    size_t n;
    size_t token_len;
    uint32 roles = current != NULL ? chrome_lifecycle_roles(current) : 0;
    uint64 pid_seq = current != NULL ? current->pid_seq : 0;

    if (!chrome_unix_ipc_payload_trace_enabled() ||
        !chrome_socket_trace_process() || buf == NULL)
        return;

    n = len;
    if (n > CHROME_UNIX_IPC_PAYLOAD_TRACE_MAX)
        n = CHROME_UNIX_IPC_PAYLOAD_TRACE_MAX;
    for (size_t i = 0; i < n; i++) {
        unsigned char byte = (unsigned char)buf[i];
        hex[i * 2] = hexchars[byte >> 4];
        hex[i * 2 + 1] = hexchars[byte & 0xf];
    }
    hex[n * 2] = '\0';
    chrome_unix_ipc_payload_sanitize_ascii(ascii, sizeof(ascii), buf, n);

    /*
     * Keep the printable byte sample capped, but scan the whole captured
     * payload for structured Chromium launch argv tokens.  sendmsg tracing
     * has the full contiguous payload; recvmsg tracing may still have only
     * the clipped iovec sample.
     */
    token_len = len;
    chrome_unix_ipc_payload_extract_token(buf, token_len, "--type=", type,
                                          sizeof(type));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--initial-client-fd=",
                                          initial_client_fd,
                                          sizeof(initial_client_fd));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--utility-sub-type=",
                                          utility_sub_type,
                                          sizeof(utility_sub_type));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--service-sandbox-type=",
                                          service_sandbox_type,
                                          sizeof(service_sandbox_type));
    chrome_unix_ipc_payload_extract_token(buf, token_len, "--shared-files=",
                                          shared_files,
                                          sizeof(shared_files));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--mojo-platform-channel-handle=",
                                          mojo_channel_handle,
                                          sizeof(mojo_channel_handle));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--renderer-client-id=",
                                          renderer_client_id,
                                          sizeof(renderer_client_id));
    chrome_unix_ipc_payload_extract_token(buf, token_len, "--gpu-client-id=",
                                          gpu_client_id,
                                          sizeof(gpu_client_id));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--render-node-override=",
                                          render_node,
                                          sizeof(render_node));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--metrics-shmem-handle=",
                                          metrics_shmem,
                                          sizeof(metrics_shmem));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--field-trial-handle=",
                                          field_trial,
                                          sizeof(field_trial));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--trace-process-track-uuid=",
                                          trace_uuid,
                                          sizeof(trace_uuid));
    chrome_unix_ipc_payload_extract_token(buf, token_len,
                                          "--time-ticks-at-unix-epoch=",
                                          time_ticks,
                                          sizeof(time_ticks));

    printf("chrome-unix-ipc-payload: %s pid=%d tgid=%d name=%s fd=%d "
           "ret=%ld total=%lu flags=0x%x cmsg=%lu has_cred=%d "
           "sample_len=%lu full_len=%lu token_len=%lu "
           "pid_seq=%lu roles=0x%x "
           "type=%s initial_client_fd=%s utility=%s sandbox=%s "
           "shared_files=%s mojo_channel_handle=%s renderer_client_id=%s "
           "gpu_client_id=%s render_node=%s metrics=%s field_trial=%s "
           "trace_uuid=%s time_ticks=%s ascii=\"%s\" hex=%s\n",
           op != NULL ? op : "?", current != NULL ? current->pid : -1,
           current != NULL ? current->tgid : -1,
           current != NULL ? current->name : "?", fd, (long)ret,
           (unsigned long)total, flags, (unsigned long)cmsg_count, has_cred,
           (unsigned long)n, (unsigned long)total,
           (unsigned long)token_len, pid_seq, roles, type,
           initial_client_fd, utility_sub_type, service_sandbox_type,
           shared_files, mojo_channel_handle, renderer_client_id,
           gpu_client_id, render_node, metrics_shmem, field_trial,
           trace_uuid, time_ticks, ascii, hex);
}

static void chrome_unix_ipc_trace_recv_payload_iov(const char *op, int fd,
                                                   ssize_t ret, size_t total,
                                                   int flags,
                                                   size_t cmsg_count,
                                                   int has_cred,
                                                   const struct k_iovec *iovs,
                                                   int iovlen)
{
    char sample[CHROME_UNIX_IPC_PAYLOAD_TRACE_MAX];
    size_t want;
    size_t copied = 0;

    if (ret <= 0 || total == 0 ||
        !chrome_unix_ipc_payload_trace_enabled() ||
        !chrome_socket_trace_process())
        return;

    want = total;
    if (want > CHROME_UNIX_IPC_PAYLOAD_TRACE_MAX)
        want = CHROME_UNIX_IPC_PAYLOAD_TRACE_MAX;

    for (int i = 0; i < iovlen && copied < want; i++) {
        size_t n = iovs[i].iov_len;
        if (n > want - copied)
            n = want - copied;
        if (n == 0)
            continue;
        if (vm_copyin(current->vm, sample + copied, iovs[i].iov_base, n) < 0) {
            printf("chrome-unix-ipc-payload: %s pid=%d tgid=%d name=%s "
                   "fd=%d ret=%ld total=%lu flags=0x%x cmsg=%lu "
                   "has_cred=%d sample_copy=EFAULT copied=%lu want=%lu\n",
                   op != NULL ? op : "?",
                   current != NULL ? current->pid : -1,
                   current != NULL ? current->tgid : -1,
                   current != NULL ? current->name : "?", fd, (long)ret,
                   (unsigned long)total, flags, (unsigned long)cmsg_count,
                   has_cred, (unsigned long)copied, (unsigned long)want);
            return;
        }
        copied += n;
    }

    chrome_unix_ipc_trace_payload_bytes(op, fd, ret, total, flags,
                                        cmsg_count, has_cred, sample, copied);
}

#define LINUX_OVERFLOW_UID 65534U
#define LINUX_OVERFLOW_GID 65534U

static ssize_t sock_udp_copy_netbuf_to_iovs(struct netbuf *nb,
                                            const struct k_iovec *iovs,
                                            uint64 iovlen,
                                            size_t buf_total,
                                            int recv_flags,
                                            int *out_flags)
{
    size_t datagram_len = netbuf_len(nb);
    size_t to_copy_total = datagram_len < buf_total ? datagram_len : buf_total;
    size_t copied = 0;
    uint64 iov_idx = 0;
    size_t iov_off = 0;
    char tmpbuf[1500];

    while (copied < to_copy_total && iov_idx < iovlen) {
        if (iovs[iov_idx].iov_len == 0 ||
            iov_off >= iovs[iov_idx].iov_len) {
            iov_idx++;
            iov_off = 0;
            continue;
        }

        size_t want = iovs[iov_idx].iov_len - iov_off;
        size_t remain = to_copy_total - copied;
        size_t chunk = want < remain ? want : remain;
        if (chunk > sizeof(tmpbuf))
            chunk = sizeof(tmpbuf);

        uint16 got = netbuf_copy_partial(nb, tmpbuf, (uint16)chunk,
                                         (uint16)copied);
        if (got == 0)
            break;

        if (vm_copyout(current->vm, iovs[iov_idx].iov_base + iov_off,
                       tmpbuf, got) < 0)
            return -EFAULT;

        copied += got;
        iov_off += got;
    }

    if (datagram_len > buf_total && out_flags != NULL)
        *out_flags |= MSG_TRUNC;

    if ((recv_flags & MSG_TRUNC) && datagram_len > copied)
        return (ssize_t)datagram_len;
    return (ssize_t)copied;
}

static size_t k_cmsg_payload_len(struct k_cmsghdr *cmsg)
{
    uint64 cmsg_len = cmsg->cmsg_len;
    if (cmsg_len < K_CMSG_LEN(0))
        return 0;
    return cmsg_len - K_CMSG_LEN(0);
}

static int k_cmsg_readable(struct k_cmsghdr *cmsg, size_t copied,
                           size_t payload_len)
{
    uint64 cmsg_len = cmsg->cmsg_len;

    if (copied < K_CMSG_LEN(0) || cmsg_len < K_CMSG_LEN(payload_len))
        return 0;
    if (cmsg_len > copied)
        return 0;
    return 1;
}

static int k_cmsg_append_common(unsigned char *buf, size_t buflen, size_t *used,
                                int level, int type, const void *data,
                                size_t data_len, int allow_partial_data,
                                int *partial_data)
{
    size_t space = K_CMSG_SPACE(data_len);
    size_t len = K_CMSG_LEN(data_len);
    size_t hdr_len = K_CMSG_LEN(0);
    size_t avail;
    size_t consume;
    size_t cmsg_len;
    size_t copy_len;

    if (partial_data != NULL)
        *partial_data = 0;
    if (*used > buflen)
        return -ENOSPC;
    avail = buflen - *used;
    if (avail < hdr_len)
        return -ENOSPC;
    if (avail < len) {
        if (!allow_partial_data)
            return -ENOSPC;
        cmsg_len = avail;
        copy_len = avail - hdr_len;
        consume = avail;
        if (partial_data != NULL)
            *partial_data = 1;
    } else {
        cmsg_len = len;
        copy_len = data_len;
        consume = avail < space ? avail : space;
    }
    if (copy_len > data_len)
        copy_len = data_len;
    if (consume < hdr_len)
        return -ENOSPC;

    struct k_cmsghdr *cmsg = (struct k_cmsghdr *)(buf + *used);
    memset(buf + *used, 0, consume);
    cmsg->cmsg_len = cmsg_len;
    cmsg->cmsg_level = level;
    cmsg->cmsg_type = type;
    if (copy_len != 0)
        memmove(K_CMSG_DATA(cmsg), data, copy_len);
    *used += consume;
    return 0;
}

static int k_cmsg_append(unsigned char *buf, size_t buflen, size_t *used,
                         int level, int type, const void *data,
                         size_t data_len)
{
    return k_cmsg_append_common(buf, buflen, used, level, type, data,
                                data_len, 0, NULL);
}

static int k_cmsg_append_partial_data(unsigned char *buf, size_t buflen,
                                      size_t *used, int level, int type,
                                      const void *data, size_t data_len,
                                      int *partial_data)
{
    return k_cmsg_append_common(buf, buflen, used, level, type, data,
                                data_len, 1, partial_data);
}

static struct unix_scm_cred unix_current_scm_cred(void);

static int unix_scm_cred_validate(const struct unix_scm_cred *cred)
{
    struct unix_scm_cred current_cred = unix_current_scm_cred();
    struct thread *target = NULL;

    if (cred->pid <= 0)
        return -ESRCH;
    if (current_cred.uid == 0) {
        rcu_read_lock();
        get_pid_thread(cred->pid, &target);
        rcu_read_unlock();
        return target != NULL ? 0 : -ESRCH;
    }
    if (cred->pid != current_cred.pid)
        return -EPERM;
    if (cred->uid != current_cred.uid)
        return -EPERM;
    if (cred->gid != current_cred.gid)
        return -EPERM;
    return 0;
}

static int unix_collect_scm_from_control(uint64 ucontrol, uint64 controllen,
                                         struct vfs_file **scm_files,
                                         size_t *scm_count,
                                         struct unix_scm_cred *scm_cred,
                                         int *has_scm_cred)
{
    unsigned char cmsg_buf[K_CMSG_PARSE_MAX];
    uint64 copy_len = controllen;

    if (has_scm_cred != NULL)
        *has_scm_cred = 0;
    if (ucontrol == 0 || controllen < K_CMSG_LEN(0))
        return 0;
    if (copy_len > sizeof(cmsg_buf))
        copy_len = sizeof(cmsg_buf);
    if (vm_copyin(current->vm, cmsg_buf, ucontrol, copy_len) < 0)
        return -EFAULT;

    for (size_t off = 0; off + K_CMSG_LEN(0) <= copy_len; ) {
        struct k_cmsghdr *cmsg = (struct k_cmsghdr *)(cmsg_buf + off);
        uint64 cmsg_len = cmsg->cmsg_len;

        if (cmsg_len < K_CMSG_LEN(0))
            return -EINVAL;
        if (cmsg_len > copy_len - off) {
            if (controllen > copy_len)
                return -EMSGSIZE;
            return -EINVAL;
        }

        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_CREDENTIALS) {
            size_t payload_len = k_cmsg_payload_len(cmsg);
            struct k_ucred *ucred = (struct k_ucred *)K_CMSG_DATA(cmsg);
            struct unix_scm_cred cred;

            if (payload_len != sizeof(*ucred))
                return -EINVAL;
            cred.pid = ucred->pid;
            cred.uid = ucred->uid;
            cred.gid = ucred->gid;
            int cred_ret = unix_scm_cred_validate(&cred);
            if (cred_ret < 0)
                return cred_ret;
            if (scm_cred != NULL) {
                *scm_cred = cred;
                if (has_scm_cred != NULL)
                    *has_scm_cred = 1;
            }
            CHROME_UNIX_SOCKET_TRACE("scm-send-cred pid=%d tgid=%d name=%s "
                                     "cred_pid=%d cred_uid=%u cred_gid=%u\n",
                                     current->pid, current->tgid,
                                     current->name, cred.pid, cred.uid,
                                     cred.gid);
        } else if (cmsg->cmsg_level == SOL_SOCKET &&
                   cmsg->cmsg_type == SCM_RIGHTS) {
            size_t payload_len = k_cmsg_payload_len(cmsg);

            if ((payload_len % sizeof(int)) != 0)
                return -EINVAL;

            size_t nfds = payload_len / sizeof(int);
            int *pass_fds = (int *)K_CMSG_DATA(cmsg);
            for (size_t i = 0; i < nfds; i++) {
                if (*scm_count >= UNIX_SCM_QUEUE_MAX - 1)
                    return -EMSGSIZE;

                struct vfs_file *file =
                    vfs_fdtable_get_file(current->fdtable, pass_fds[i]);
                if (file == NULL)
                    return -EBADF;

                scm_files[*scm_count] = vfs_fdup(file);
                vfs_fput(file);
                if (scm_files[*scm_count] == NULL)
                    return -ENOMEM;
                if (unix_ipc_trace_process()) {
                    struct chrome_unix_file_snapshot snap;
                    chrome_unix_file_snapshot_fill(scm_files[*scm_count],
                                                   &snap);
                    printf("chrome-unix-ipc: scm-send pid=%d tgid=%d "
                           "name=%s pass_fd=%d path=%s file=%p "
                           "f_flags=0x%x index=%lu pass_is_unix=%d "
                           "pass_sk=%p pass_peer=%p pass_ino=%llu "
                           "pass_peer_ino=%llu pass_file_refs=%d "
                           "pass_peer_file=%p pass_peer_file_refs=%d "
                           "pass_type=%d pass_state=%d pass_shutdown=0x%x "
                           "pass_err=%d pass_tx=%lu/%lu "
                           "pass_marks=%u:%u pass_scm=%lu pass_packets=%lu "
                           "pass_peer_type=%d "
                           "pass_peer_state=%d pass_peer_shutdown=0x%x "
                           "pass_peer_err=%d pass_peer_tx=%lu/%lu "
                           "pass_peer_marks=%u:%u pass_peer_scm=%lu "
                           "pass_peer_packets=%lu\n",
                           current->pid, current->tgid, current->name,
                           pass_fds[i],
                           chrome_socket_trace_path(scm_files[*scm_count]),
                           scm_files[*scm_count],
                           scm_files[*scm_count]->f_flags,
                           (unsigned long)*scm_count, snap.is_unix,
                           snap.sk, snap.peer, (unsigned long long)snap.ino,
                           (unsigned long long)snap.peer_ino,
                           snap.visible_refs, snap.peer_file,
                           snap.peer_visible_refs, snap.type, snap.state,
                           snap.shutdown, snap.err,
                           (unsigned long)snap.tx_bytes,
                           (unsigned long)snap.tx_capacity, snap.tx_nread,
                           snap.tx_nwrite, (unsigned long)snap.scm,
                           (unsigned long)snap.packets, snap.peer_type,
                           snap.peer_state, snap.peer_shutdown,
                           snap.peer_err,
                           (unsigned long)snap.peer_tx_bytes,
                           (unsigned long)snap.peer_tx_capacity,
                           snap.peer_tx_nread, snap.peer_tx_nwrite,
                           (unsigned long)snap.peer_scm,
                           (unsigned long)snap.peer_packets);
                }
                (*scm_count)++;
            }
        }

        size_t next = off + K_CMSG_ALIGN(cmsg_len);
        if (next <= off)
            return -EINVAL;
        off = next;
    }

    return 0;
}

static int unix_sock_passcred_enabled(struct unix_sock *sk)
{
    int passcred;

    spin_lock(&sk->lock);
    passcred = sk->passcred;
    spin_unlock(&sk->lock);
    return passcred;
}

static struct unix_scm_cred unix_current_scm_cred(void)
{
    struct unix_scm_cred cred;

    cred.pid = current ? current->tgid : 0;
    cred.uid = (current && current->thread_group) ? current->thread_group->uid : 0;
    cred.gid = (current && current->thread_group) ? current->thread_group->gid : 0;
    return cred;
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

static int unix_packet_enqueue_locked(struct unix_sock *sk, uint start_mark,
                                      uint end_mark)
{
    if (sk->type != SOCK_SEQPACKET)
        return 0;
    if ((unsigned long)unix_packet_count_locked(sk) >= UNIX_PACKET_QUEUE_MAX - 1)
        return -EAGAIN;
    sk->packet_queue[sk->packet_tail].start_nread = start_mark;
    sk->packet_queue[sk->packet_tail].end_nread = end_mark;
    sk->packet_queue[sk->packet_tail].data = NULL;
    sk->packet_queue[sk->packet_tail].len = 0;
    sk->packet_tail = (sk->packet_tail + 1) % UNIX_PACKET_QUEUE_MAX;
    return 0;
}

static int unix_packet_enqueue_payload_locked(struct unix_sock *sk,
                                              uint start_mark,
                                              uint end_mark,
                                              char *data,
                                              size_t len)
{
    if (sk->type != SOCK_SEQPACKET)
        return 0;
    if ((unsigned long)unix_packet_count_locked(sk) >= UNIX_PACKET_QUEUE_MAX - 1)
        return -EAGAIN;
    sk->packet_queue[sk->packet_tail].start_nread = start_mark;
    sk->packet_queue[sk->packet_tail].end_nread = end_mark;
    sk->packet_queue[sk->packet_tail].data = data;
    sk->packet_queue[sk->packet_tail].len = len;
    sk->packet_tail = (sk->packet_tail + 1) % UNIX_PACKET_QUEUE_MAX;
    return 0;
}

static int unix_packet_has_space_locked(struct unix_sock *sk)
{
    return sk->type != SOCK_SEQPACKET ||
        (unsigned long)unix_packet_count_locked(sk) < UNIX_PACKET_QUEUE_MAX - 1;
}

static int unix_packet_next_len_locked(struct unix_sock *peer, size_t *len)
{
    if (peer->packet_head == peer->packet_tail)
        return -EAGAIN;
    uint start = peer->packet_queue[peer->packet_head].start_nread;
    uint end = peer->packet_queue[peer->packet_head].end_nread;
    uint cursor = peer->tx.nread;
    if ((int)(end - cursor) < 0 || (int)(cursor - start) < 0)
        return -EIO;
    *len = end - cursor;
    return 0;
}

static void unix_packet_pop_locked(struct unix_sock *peer)
{
    if (peer->packet_head != peer->packet_tail) {
        if (peer->packet_queue[peer->packet_head].data != NULL) {
            kvfree(peer->packet_queue[peer->packet_head].data);
            peer->packet_queue[peer->packet_head].data = NULL;
        }
        peer->packet_queue[peer->packet_head].start_nread = 0;
        peer->packet_queue[peer->packet_head].end_nread = 0;
        peer->packet_queue[peer->packet_head].len = 0;
        peer->packet_head = (peer->packet_head + 1) % UNIX_PACKET_QUEUE_MAX;
    }
}

static int unix_scm_ready_at_nread(const struct unix_scm_entry *entry,
                                   uint virtual_nread, int sock_type);
static int unix_scm_ready_for_stream_zero_recv_locked(
    const struct unix_scm_entry *entry, const struct unix_sock *peer);

static size_t unix_dequeue_scm_locked(struct unix_sock *peer,
                                      struct vfs_file **files,
                                      size_t max_files,
                                      struct unix_scm_cred *cred,
                                      int *has_cred)
{
    size_t count = 0;
    if (has_cred != NULL)
        *has_cred = 0;
    if (peer == NULL)
        return 0;

    while (peer->scm_head != peer->scm_tail &&
           unix_scm_ready_at_nread(&peer->scm_queue[peer->scm_head],
                                   peer->tx.nread, peer->type)) {
        if (peer->scm_queue[peer->scm_head].file != NULL) {
            if (count >= max_files)
                break;
            files[count++] = peer->scm_queue[peer->scm_head].file;
        }
        if (peer->scm_queue[peer->scm_head].has_cred &&
            has_cred != NULL && *has_cred == 0) {
            *cred = peer->scm_queue[peer->scm_head].cred;
            *has_cred = 1;
        }
        peer->scm_queue[peer->scm_head].file = NULL;
        peer->scm_queue[peer->scm_head].has_cred = 0;
        memset(&peer->scm_queue[peer->scm_head].cred, 0,
               sizeof(peer->scm_queue[peer->scm_head].cred));
        peer->scm_queue[peer->scm_head].start_nread = 0;
        peer->scm_queue[peer->scm_head].end_nread = 0;
        peer->scm_head = (peer->scm_head + 1) % UNIX_SCM_QUEUE_MAX;
    }
    if (count > 0 || (has_cred != NULL && *has_cred))
        tq_wakeup_all(&peer->wr_queue, 0, 0);
    return count;
}

static size_t unix_dequeue_scm(struct unix_sock *peer,
                               struct vfs_file **files,
                               size_t max_files,
                               struct unix_scm_cred *cred,
                               int *has_cred)
{
    size_t count;

    if (has_cred != NULL)
        *has_cred = 0;
    if (peer == NULL)
        return 0;

    spin_lock(&peer->lock);
    count = unix_dequeue_scm_locked(peer, files, max_files, cred, has_cred);
    spin_unlock(&peer->lock);
    return count;
}

static size_t unix_dequeue_scm_stream_zero_recv(struct unix_sock *peer,
                                                struct vfs_file **files,
                                                size_t max_files,
                                                struct unix_scm_cred *cred,
                                                int *has_cred)
{
    size_t count = 0;

    if (has_cred != NULL)
        *has_cred = 0;
    if (peer == NULL)
        return 0;

    spin_lock(&peer->lock);
    while (peer->scm_head != peer->scm_tail &&
           unix_scm_ready_for_stream_zero_recv_locked(
               &peer->scm_queue[peer->scm_head], peer)) {
        if (peer->scm_queue[peer->scm_head].file != NULL) {
            if (count >= max_files)
                break;
            files[count++] = peer->scm_queue[peer->scm_head].file;
        }
        if (peer->scm_queue[peer->scm_head].has_cred &&
            has_cred != NULL && *has_cred == 0) {
            *cred = peer->scm_queue[peer->scm_head].cred;
            *has_cred = 1;
        }
        peer->scm_queue[peer->scm_head].file = NULL;
        peer->scm_queue[peer->scm_head].has_cred = 0;
        memset(&peer->scm_queue[peer->scm_head].cred, 0,
               sizeof(peer->scm_queue[peer->scm_head].cred));
        peer->scm_queue[peer->scm_head].start_nread = 0;
        peer->scm_queue[peer->scm_head].end_nread = 0;
        peer->scm_head = (peer->scm_head + 1) % UNIX_SCM_QUEUE_MAX;
    }
    if (count > 0 || (has_cred != NULL && *has_cred))
        tq_wakeup_all(&peer->wr_queue, 0, 0);
    spin_unlock(&peer->lock);
    return count;
}

static int unix_scm_ready_at_nread(const struct unix_scm_entry *entry,
                                   uint virtual_nread, int sock_type)
{
    if (entry->start_nread == entry->end_nread)
        return (int)(virtual_nread - entry->start_nread) >= 0;
    if (sock_type == SOCK_SEQPACKET)
        return (int)(virtual_nread - entry->end_nread) >= 0;
    /*
     * Linux stream sockets deliver SCM_RIGHTS/SCM_CREDENTIALS with the first
     * byte read from the sendmsg() segment that carried the control message,
     * not after the whole segment has drained.
     */
    return (int)(virtual_nread - entry->start_nread) > 0;
}

static int unix_scm_ready_for_stream_zero_recv_locked(
    const struct unix_scm_entry *entry, const struct unix_sock *peer)
{
    size_t readable;

    if (peer == NULL)
        return 0;
    if (unix_scm_ready_at_nread(entry, peer->tx.nread, SOCK_STREAM))
        return 1;

    readable = peer->tx.nwrite - peer->tx.nread;
    return readable > 0 && entry->start_nread == peer->tx.nread;
}

static int unix_peek_scm(struct unix_sock *peer, uint virtual_nread,
                         struct vfs_file **files, size_t max_files,
                         struct unix_scm_cred *cred, int *has_cred,
                         size_t *out_count)
{
    size_t count = 0;

    if (has_cred != NULL)
        *has_cred = 0;
    if (out_count != NULL)
        *out_count = 0;
    if (peer == NULL)
        return 0;

    spin_lock(&peer->lock);
    for (int idx = peer->scm_head; idx != peer->scm_tail;
         idx = (idx + 1) % UNIX_SCM_QUEUE_MAX) {
        struct unix_scm_entry *entry = &peer->scm_queue[idx];

        if (!unix_scm_ready_at_nread(entry, virtual_nread, peer->type))
            break;

        if (entry->file != NULL) {
            if (count >= max_files)
                break;
            files[count] = vfs_fdup(entry->file);
            if (files[count] == NULL) {
                spin_unlock(&peer->lock);
                for (size_t i = 0; i < count; i++) {
                    vfs_fput(files[i]);
                    files[i] = NULL;
                }
                return -ENOMEM;
            }
            count++;
        }
        if (entry->has_cred && has_cred != NULL && *has_cred == 0) {
            *cred = entry->cred;
            *has_cred = 1;
        }
    }
    spin_unlock(&peer->lock);

    if (out_count != NULL)
        *out_count = count;
    return 0;
}

static void unix_discard_scm_rights(struct vfs_file **files, size_t start,
                                    size_t count)
{
    for (size_t i = start; i < count; i++) {
        if (files[i] != NULL) {
            vfs_fput(files[i]);
            files[i] = NULL;
        }
    }
}

static void unix_rollback_recvmsg_fds(const int *fds, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (fds[i] < 0)
            continue;
        spin_lock(&current->fdtable->lock);
        struct vfs_file *f = vfs_fdtable_dealloc_fd(current->fdtable, fds[i]);
        spin_unlock(&current->fdtable->lock);
        if (f != NULL)
            vfs_fput(f);
    }
}

static struct k_ucred unix_unknown_scm_ucred(void)
{
    struct k_ucred ucred;

    /*
     * Linux still emits SCM_CREDENTIALS when SO_PASSCRED is enabled after a
     * message was already queued without credentials.  Those credentials are
     * intentionally "unknown", not the stale socket peer credentials captured
     * at socketpair/connect time.
     */
    ucred.pid = 0;
    ucred.uid = LINUX_OVERFLOW_UID;
    ucred.gid = LINUX_OVERFLOW_GID;
    return ucred;
}

static int unix_emit_recvmsg_scm(struct unix_sock *sk,
                                 const struct k_msghdr *mh,
                                 uint64 msg_addr, int sockfd,
                                 int recv_flags,
                                 ssize_t payload_bytes,
                                 int message_present,
                                 struct vfs_file **scm_files,
                                 size_t scm_count,
                                 const struct unix_scm_cred *scm_cred,
                                 int has_scm_cred,
                                 int *out_flags)
{
    unsigned char cmsg_buf[K_CMSG_SPACE(sizeof(struct k_ucred)) +
                           K_CMSG_SPACE(sizeof(int) * UNIX_SCM_QUEUE_MAX)];
    int installed_fds[UNIX_SCM_QUEUE_MAX];
    size_t cmsg_used = 0;
    size_t cmsg_cap = mh->msg_controllen;
    size_t installed = 0;

    if (cmsg_cap > sizeof(cmsg_buf))
        cmsg_cap = sizeof(cmsg_buf);
    for (size_t i = 0; i < UNIX_SCM_QUEUE_MAX; i++)
        installed_fds[i] = -1;

    /*
     * Linux attaches SCM_CREDENTIALS to the received message when
     * SO_PASSCRED is enabled; this is not conditional on a positive payload
     * length.  AF_UNIX users can send zero-byte seqpacket/control messages,
     * and dropping the queued credential there permanently loses the control
     * state after unix_dequeue_scm().
     */
    if (unix_sock_passcred_enabled(sk) &&
        (message_present || payload_bytes > 0 || has_scm_cred ||
         scm_count > 0)) {
        struct k_ucred ucred;
        if (has_scm_cred && scm_cred != NULL) {
            ucred.pid = scm_cred->pid;
            ucred.uid = scm_cred->uid;
            ucred.gid = scm_cred->gid;
        } else {
            ucred = unix_unknown_scm_ucred();
        }
        CHROME_UNIX_SOCKET_TRACE("scm-emit-cred pid=%d tgid=%d name=%s fd=%d "
                                 "source=%s cred_pid=%d cred_uid=%u "
                                 "cred_gid=%u payload=%ld scm_count=%lu\n",
                                 current->pid, current->tgid, current->name,
                                 sockfd,
                                 has_scm_cred ? "queued" : "unknown",
                                 ucred.pid, ucred.uid, ucred.gid,
                                 (long)payload_bytes,
                                 (unsigned long)scm_count);
        int partial_cred = 0;
        if (mh->msg_control == 0 ||
            k_cmsg_append_partial_data(cmsg_buf, cmsg_cap, &cmsg_used,
                                       SOL_SOCKET, SCM_CREDENTIALS,
                                       &ucred, sizeof(ucred),
                                       &partial_cred) < 0 ||
            partial_cred)
            *out_flags |= MSG_CTRUNC;
    }

    if (scm_count > 0) {
        int newfds[UNIX_SCM_QUEUE_MAX];
        size_t max_fds = 0;
        size_t to_install;

        if (mh->msg_control != 0 && cmsg_cap > cmsg_used) {
            size_t remain = cmsg_cap - cmsg_used;
            while (max_fds < scm_count &&
                   K_CMSG_LEN(sizeof(int) * (max_fds + 1)) <= remain)
                max_fds++;
        }
        to_install = scm_count < max_fds ? scm_count : max_fds;

        while (installed < to_install) {
            struct vfs_file *install_file = scm_files[installed];
            spin_lock(&current->fdtable->lock);
            int newfd = vfs_fdtable_alloc_fd(current->fdtable, install_file);
            if (newfd >= 0 && (recv_flags & MSG_CMSG_CLOEXEC))
                vfs_fdtable_set_fdflags(current->fdtable, newfd, FD_CLOEXEC);
            spin_unlock(&current->fdtable->lock);
            if (unix_ipc_trace_process()) {
                struct chrome_unix_file_snapshot snap;
                chrome_unix_file_snapshot_fill(install_file, &snap);
                printf("chrome-unix-ipc: scm-recv pid=%d tgid=%d name=%s "
                       "sockfd=%d newfd=%d path=%s file=%p f_flags=0x%x "
                       "index=%lu cloexec=%d peek=%d install_is_unix=%d "
                       "install_sk=%p install_peer=%p install_ino=%llu "
                       "install_peer_ino=%llu install_file_refs=%d "
                       "install_peer_file=%p install_peer_file_refs=%d "
                       "install_type=%d install_state=%d "
                       "install_shutdown=0x%x install_err=%d "
                       "install_tx=%lu/%lu "
                       "install_marks=%u:%u install_scm=%lu "
                       "install_packets=%lu install_peer_type=%d "
                       "install_peer_state=%d install_peer_shutdown=0x%x "
                       "install_peer_err=%d install_peer_tx=%lu/%lu "
                       "install_peer_marks=%u:%u install_peer_scm=%lu "
                       "install_peer_packets=%lu\n",
                       current->pid, current->tgid, current->name, sockfd,
                       newfd, chrome_socket_trace_path(install_file),
                       install_file,
                       install_file != NULL ? install_file->f_flags : 0,
                       (unsigned long)installed,
                       (recv_flags & MSG_CMSG_CLOEXEC) != 0,
                       (recv_flags & MSG_PEEK) != 0, snap.is_unix,
                       snap.sk, snap.peer, (unsigned long long)snap.ino,
                       (unsigned long long)snap.peer_ino,
                       snap.visible_refs, snap.peer_file,
                       snap.peer_visible_refs, snap.type, snap.state,
                       snap.shutdown, snap.err,
                       (unsigned long)snap.tx_bytes,
                       (unsigned long)snap.tx_capacity, snap.tx_nread,
                       snap.tx_nwrite, (unsigned long)snap.scm,
                       (unsigned long)snap.packets, snap.peer_type,
                       snap.peer_state, snap.peer_shutdown, snap.peer_err,
                       (unsigned long)snap.peer_tx_bytes,
                       (unsigned long)snap.peer_tx_capacity,
                       snap.peer_tx_nread, snap.peer_tx_nwrite,
                       (unsigned long)snap.peer_scm,
                       (unsigned long)snap.peer_packets);
            }
            vfs_fput(scm_files[installed]);
            scm_files[installed] = NULL;
            if (newfd < 0)
                break;
            newfds[installed] = newfd;
            installed_fds[installed] = newfd;
            installed++;
        }

        unix_discard_scm_rights(scm_files, installed, scm_count);
        if (installed < scm_count)
            *out_flags |= MSG_CTRUNC;
        if (installed > 0 &&
            k_cmsg_append(cmsg_buf, cmsg_cap, &cmsg_used,
                          SOL_SOCKET, SCM_RIGHTS, newfds,
                          sizeof(int) * installed) < 0) {
            unix_rollback_recvmsg_fds(installed_fds, installed);
            installed = 0;
            *out_flags |= MSG_CTRUNC;
        }
    }

    if (mh->msg_control != 0 && cmsg_used > 0 &&
        vm_copyout(current->vm, mh->msg_control, cmsg_buf, cmsg_used) < 0) {
        unix_rollback_recvmsg_fds(installed_fds, installed);
        return -EFAULT;
    }

    uint64 uclen = cmsg_used;
    if (vm_copyout(current->vm,
                   msg_addr + __builtin_offsetof(struct k_msghdr,
                                                 msg_controllen),
                   &uclen, sizeof(uclen)) < 0) {
        unix_rollback_recvmsg_fds(installed_fds, installed);
        return -EFAULT;
    }
    return 0;
}

static int unix_next_scm_read_window_locked(struct unix_sock *peer,
                                            size_t *bytes,
                                            int *stop_after_read)
{
    uint start;
    uint end;
    uint nread;

    if (peer == NULL || peer->scm_head == peer->scm_tail)
        return 0;

    start = peer->scm_queue[peer->scm_head].start_nread;
    end = peer->scm_queue[peer->scm_head].end_nread;
    nread = peer->tx.nread;
    *stop_after_read = 0;

    if ((int)(start - nread) > 0) {
        *bytes = (size_t)(start - nread);
        return 1;
    }

    if (start == end || (int)(nread - start) > 0) {
        *bytes = 0;
        *stop_after_read = 1;
        return 1;
    }

    *bytes = (size_t)(end - nread);
    *stop_after_read = 1;
    return 1;
}

static size_t unix_tx_limit_locked(struct unix_sock *sk)
{
    size_t limit = sk->snd_buf ? sk->snd_buf : UNIX_SOCKBUF_DEFAULT;
    struct unix_sock *peer = sk->peer;

    if (peer != NULL && peer->rcv_buf != 0 && peer->rcv_buf < limit)
        limit = peer->rcv_buf;
    return limit;
}

static size_t unix_ring_write_locked(struct unix_ring *r, const char *buf,
                                     size_t len, size_t max_capacity)
{
    size_t readable = r->nwrite - r->nread;
    size_t avail = readable < max_capacity ? max_capacity - readable : 0;
    if (avail > r->capacity - readable)
        avail = r->capacity - readable;
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

static int unix_ring_capacity_for(size_t needed, size_t max_capacity,
                                  size_t *capacity)
{
    if (needed > max_capacity)
        return -EMSGSIZE;

    size_t new_capacity = UNIX_BUF_DEFAULT_SIZE;
    while (new_capacity < needed) {
        if (new_capacity > max_capacity / 2) {
            new_capacity = max_capacity;
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
        uint start = sk->scm_queue[idx].start_nread;
        uint end = sk->scm_queue[idx].end_nread;
        sk->scm_queue[idx].start_nread =
            (uint)((int)(start - old_nread) < 0 ? 0 : start - old_nread);
        sk->scm_queue[idx].end_nread =
            (uint)((int)(end - old_nread) < 0 ? 0 : end - old_nread);
    }

    for (idx = sk->packet_head; idx != sk->packet_tail;
         idx = (idx + 1) % UNIX_PACKET_QUEUE_MAX) {
        uint start = sk->packet_queue[idx].start_nread;
        uint end = sk->packet_queue[idx].end_nread;
        sk->packet_queue[idx].start_nread =
            (uint)((int)(start - old_nread) < 0 ? 0 : start - old_nread);
        sk->packet_queue[idx].end_nread =
            (uint)((int)(end - old_nread) < 0 ? 0 : end - old_nread);
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
    size_t max_capacity = unix_tx_limit_locked(sk);

    if (len > max_capacity)
        return -EMSGSIZE;

    size_t readable = (unsigned long)(sk->tx.nwrite - sk->tx.nread);
    if (readable > max_capacity || len > max_capacity - readable)
        return -EAGAIN;

    size_t needed = readable + len;
    if (needed <= sk->tx.capacity)
        return 0;
    return unix_ring_capacity_for(needed, max_capacity, capacity);
}

static int unix_sendmsg_atomic_locked(struct unix_sock *sk, const char *buf,
                                      size_t len,
                                      struct vfs_file **scm_files,
                                      size_t scm_count,
                                      const struct unix_scm_cred *scm_cred,
                                      int has_scm_cred,
                                      int *buf_queued)
{
    if (buf_queued != NULL)
        *buf_queued = 0;
    if (sk->shutdown_flags & UNIX_SHUT_WR)
        return -EPIPE;
    if (sk->state != UNIX_STATE_CONNECTED && sk->type == SOCK_STREAM)
        return -ENOTCONN;
    if (sk->peer == NULL)
        return -EPIPE;
    size_t readable = (unsigned long)(sk->tx.nwrite - sk->tx.nread);
    size_t max_capacity = unix_tx_limit_locked(sk);
    size_t writable = readable < max_capacity ? max_capacity - readable : 0;
    if (writable > sk->tx.capacity - readable)
        writable = sk->tx.capacity - readable;
    if (writable < len)
        return -EAGAIN;

    int send_auto_cred = sk->peer != NULL &&
        __atomic_load_n(&sk->peer->passcred, __ATOMIC_ACQUIRE) != 0;
    int send_cred = has_scm_cred || send_auto_cred;
    size_t scm_used = unix_scm_count_locked(sk);
    size_t scm_free = UNIX_SCM_QUEUE_MAX - 1 - scm_used;
    if (scm_free < scm_count + (send_cred ? 1 : 0))
        return -EAGAIN;
    if (!unix_packet_has_space_locked(sk))
        return -EAGAIN;

    uint scm_start = sk->tx.nwrite;
    uint mark = scm_start + (uint)len;
    if (send_cred) {
        sk->scm_queue[sk->scm_tail].file = NULL;
        sk->scm_queue[sk->scm_tail].has_cred = 1;
        sk->scm_queue[sk->scm_tail].cred = has_scm_cred
            ? *scm_cred
            : unix_current_scm_cred();
        sk->scm_queue[sk->scm_tail].start_nread = scm_start;
        sk->scm_queue[sk->scm_tail].end_nread = mark;
        sk->scm_tail = (sk->scm_tail + 1) % UNIX_SCM_QUEUE_MAX;
    }
    for (size_t i = 0; i < scm_count; i++) {
        sk->scm_queue[sk->scm_tail].file = scm_files[i];
        sk->scm_queue[sk->scm_tail].has_cred = 0;
        memset(&sk->scm_queue[sk->scm_tail].cred, 0,
               sizeof(sk->scm_queue[sk->scm_tail].cred));
        sk->scm_queue[sk->scm_tail].start_nread = scm_start;
        sk->scm_queue[sk->scm_tail].end_nread = mark;
        sk->scm_tail = (sk->scm_tail + 1) % UNIX_SCM_QUEUE_MAX;
    }
    int pkt_ret;
    size_t wrote = unix_ring_write_locked(&sk->tx, buf, len, max_capacity);
    if (wrote != len)
        panic("unix_sendmsg_atomic_locked: short atomic write");
    pkt_ret = unix_packet_enqueue_locked(sk, scm_start, sk->tx.nwrite);
    if (pkt_ret < 0)
        panic("unix_sendmsg_atomic_locked: packet queue lost reservation");
    return 0;
}

static int unix_seqpacket_recv_buffer(struct unix_sock *peer, int flags,
                                      size_t capacity, char **packet_buf,
                                      size_t *copied_packet,
                                      size_t *packet_len,
                                      uint *scm_virtual_nread,
                                      int *have_scm_virtual_nread,
                                      struct vfs_file **scm_files,
                                      size_t scm_max_files,
                                      struct unix_scm_cred *scm_cred,
                                      int *has_scm_cred,
                                      size_t *scm_count,
                                      int *scm_dequeued_with_packet,
                                      struct vfs_file **peer_wr_file)
{
    *packet_buf = NULL;
    *copied_packet = 0;
    *packet_len = 0;
    if (peer_wr_file != NULL)
        *peer_wr_file = NULL;

    for (;;) {
        size_t observed_packet_len = 0;
        int pkt_ret;

        spin_lock(&peer->lock);
        pkt_ret = unix_packet_next_len_locked(peer, &observed_packet_len);
        spin_unlock(&peer->lock);
        if (pkt_ret != 0)
            return pkt_ret;

        size_t observed_to_copy =
            observed_packet_len < capacity ? observed_packet_len : capacity;
        char *buf = kvmalloc(observed_to_copy ? observed_to_copy : 1);
        if (buf == NULL)
            return -ENOMEM;

        spin_lock(&peer->lock);
        size_t locked_packet_len = 0;
        pkt_ret = unix_packet_next_len_locked(peer, &locked_packet_len);
        if (pkt_ret != 0) {
            spin_unlock(&peer->lock);
            kvfree(buf);
            if (pkt_ret == -EAGAIN)
                continue;
            return pkt_ret;
        }

        size_t locked_to_copy =
            locked_packet_len < capacity ? locked_packet_len : capacity;
        if (locked_to_copy > observed_to_copy) {
            spin_unlock(&peer->lock);
            kvfree(buf);
            continue;
        }

        size_t copied;
        struct unix_packet_entry *entry =
            &peer->packet_queue[peer->packet_head];
        char *packet_data = entry->data;
        if (flags & MSG_PEEK) {
            if (scm_virtual_nread != NULL)
                *scm_virtual_nread = peer->tx.nread + (uint)locked_packet_len;
            if (have_scm_virtual_nread != NULL)
                *have_scm_virtual_nread = 1;
            if (packet_data != NULL) {
                if (locked_to_copy > entry->len) {
                    spin_unlock(&peer->lock);
                    kvfree(buf);
                    return -EIO;
                }
                memmove(buf, packet_data, locked_to_copy);
                copied = locked_to_copy;
            } else {
                copied = unix_ring_peek(&peer->tx, 0, buf, locked_to_copy);
            }
        } else {
            if (packet_data != NULL) {
                if (locked_to_copy > entry->len) {
                    spin_unlock(&peer->lock);
                    kvfree(buf);
                    return -EIO;
                }
                memmove(buf, packet_data, locked_to_copy);
                copied = locked_to_copy;
                smp_store_release(&peer->tx.nread, entry->end_nread);
            } else {
                copied = unix_ring_read_locked(&peer->tx, buf, locked_to_copy);
                if (locked_packet_len > copied) {
                    uint mark =
                        peer->packet_queue[peer->packet_head].end_nread;
                    smp_store_release(&peer->tx.nread, mark);
                }
            }
            unix_packet_pop_locked(peer);
            if (scm_count != NULL) {
                *scm_count = unix_dequeue_scm_locked(peer, scm_files,
                                                     scm_max_files,
                                                     scm_cred,
                                                     has_scm_cred);
            }
            if (scm_dequeued_with_packet != NULL)
                *scm_dequeued_with_packet = 1;
            tq_wakeup_all(&peer->wr_queue, 0, 0);
            if (peer_wr_file != NULL)
                *peer_wr_file = peer->file ? vfs_fdup(peer->file) : NULL;
        }
        spin_unlock(&peer->lock);

        if (copied != locked_to_copy) {
            kvfree(buf);
            return -EIO;
        }

        *packet_buf = buf;
        *copied_packet = copied;
        *packet_len = locked_packet_len;
        return 0;
    }
}

struct webkit_ipc_stress_header {
    uint magic;
    uint seq;
    uint len;
    uint checksum;
};

static int webkit_seqpacket_verify_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param("webkit_seqpacket_verify", value,
                                    sizeof(value)) == 0 &&
            value[0] != '\0' && value[0] != '0' &&
            value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static uint webkit_ipc_stress_checksum(const uchar *buf, uint len)
{
    uint h = 2166136261u;

    for (uint i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 16777619u;
    }
    return h;
}

static void unix_seqpacket_verify_webkit_packet(const char *op,
                                                const char *proc_name,
                                                const char *exec_path,
                                                const char *buf,
                                                size_t copied,
                                                size_t packet_len)
{
    if (!webkit_seqpacket_verify_enabled())
        return;
    if (buf == NULL || copied < sizeof(struct webkit_ipc_stress_header))
        return;

    const struct webkit_ipc_stress_header *hdr =
        (const struct webkit_ipc_stress_header *)buf;
    if (hdr->magic != 0x574b4950u)
        return;
    if (packet_len != sizeof(*hdr) + 2048 || copied != packet_len ||
        hdr->len != 2048 || hdr->len > copied - sizeof(*hdr))
        return;

    uint actual = webkit_ipc_stress_checksum(
        (const uchar *)(buf + sizeof(*hdr)), hdr->len);
    if (actual != hdr->checksum) {
        printf("webkit-seqpacket-verify: op=%s role=%s exec=%s "
               "seq=%u packet=%lu copied=%lu len=%u hdr_checksum=0x%x "
               "actual_checksum=0x%x\n",
               op != NULL ? op : "?",
               proc_name != NULL ? proc_name : "?",
               exec_path != NULL ? exec_path : "?",
               hdr->seq, packet_len, copied, hdr->len,
               hdr->checksum, actual);
    }
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
        return sys_sendmsg_netlink(fd, umsg, flags);

    if (domain == AF_UNIX) {
        /* ---- AF_UNIX sendmsg with optional SCM_RIGHTS ---- */
        struct unix_sock *sk = unix_sock_from_fd(fd);
        if (sk == NULL)
            return (uint64)-EBADF;

        struct k_msghdr mh;
        if (vm_copyin(current->vm, &mh, umsg, sizeof(mh)) < 0)
            return (uint64)-EFAULT;

        if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
            return (uint64)-EMSGSIZE;

        struct k_iovec iovs[SENDMSG_MAX_IOV];
        uint64 iov_bytes = mh.msg_iovlen * sizeof(struct k_iovec);
        if (iov_bytes != 0 &&
            vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
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
        struct unix_scm_cred scm_cred;
        memset(&scm_cred, 0, sizeof(scm_cred));
        int has_scm_cred = 0;

        int scm_ret = unix_collect_scm_from_control(mh.msg_control,
                                                    mh.msg_controllen,
                                                    scm_files,
                                                    &scm_count,
                                                    &scm_cred,
                                                    &has_scm_cred);
        CHROME_UNIX_SOCKET_TRACE("sendmsg-enter pid=%d tgid=%d name=%s fd=%d "
                                 "bytes=%lu flags=0x%x control=0x%lx "
                                 "controllen=%lu scm_count=%lu has_cred=%d "
                                 "ret=%d\n",
                                 current->pid, current->tgid, current->name,
                                 fd, (unsigned long)total_len, flags,
                                 mh.msg_control, mh.msg_controllen,
                                 (unsigned long)scm_count, has_scm_cred,
                                 scm_ret);
        chrome_unix_ipc_trace_payload_bytes("sendmsg-enter", fd,
                                            (ssize_t)total_len, total_len,
                                            flags, scm_count, has_scm_cred,
                                            msg_buf, total_len);
        if (scm_ret < 0) {
            chrome_unix_ipc_trace_state("sendmsg-cmsg-error", fd, scm_ret,
                                        total_len, flags, mh.msg_control,
                                        mh.msg_controllen, scm_count,
                                        has_scm_cred, sk);
            unix_discard_scm_rights(scm_files, 0, scm_count);
            kvfree(msg_buf);
            return (uint64)scm_ret;
        }
        if (total_len == 0 && sk->type == SOCK_STREAM) {
            chrome_unix_ipc_trace_state("sendmsg-zero-stream", fd, 0,
                                        total_len, flags, mh.msg_control,
                                        mh.msg_controllen, scm_count,
                                        has_scm_cred, sk);
            unix_discard_scm_rights(scm_files, 0, scm_count);
            kvfree(msg_buf);
            return 0;
        }

        int ret;
        int nonblock = sock_is_nonblock(fd, flags);
        struct vfs_file *notify_file = NULL;
        char *grow_data = NULL;
        size_t grow_capacity = 0;
        int msg_buf_queued = 0;
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
                                             scm_files, scm_count,
                                             &scm_cred, has_scm_cred,
                                             &msg_buf_queued);
            struct unix_sock *notify_peer = NULL;
            if (ret == 0 && sk->peer != NULL) {
                notify_peer = sk->peer;
                unix_sock_get_ref(notify_peer);
            }
            if (ret == 0 || ret != -EAGAIN || nonblock) {
                spin_unlock(&sk->lock);
                if (notify_peer != NULL) {
                    spin_lock(&notify_peer->lock);
                    tq_wakeup_all(&notify_peer->rd_queue, 0, 0);
                    if (notify_peer->file != NULL)
                        notify_file = vfs_fdup(notify_peer->file);
                    spin_unlock(&notify_peer->lock);
                    unix_sock_put_ref(notify_peer);
                }
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
        if (!msg_buf_queued)
            kvfree(msg_buf);
        if (ret < 0) {
            CHROME_UNIX_SOCKET_TRACE("sendmsg-exit pid=%d tgid=%d name=%s "
                                     "fd=%d ret=%d bytes=%lu scm_count=%lu "
                                     "has_cred=%d\n",
                                     current->pid, current->tgid,
                                     current->name, fd, ret,
                                     (unsigned long)total_len,
                                     (unsigned long)scm_count, has_scm_cred);
            chrome_unix_ipc_trace_state("sendmsg-exit", fd, ret, total_len,
                                        flags, mh.msg_control,
                                        mh.msg_controllen, scm_count,
                                        has_scm_cred, sk);
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

        CHROME_UNIX_SOCKET_TRACE("sendmsg-exit pid=%d tgid=%d name=%s fd=%d "
                                 "ret=%lu bytes=%lu scm_count=%lu\n",
                                 current->pid, current->tgid, current->name,
                                 fd, (unsigned long)total_len,
                                 (unsigned long)total_len,
                                 (unsigned long)scm_count);
        chrome_unix_ipc_trace_state("sendmsg-exit", fd, (ssize_t)total_len,
                                    total_len, flags, mh.msg_control,
                                    mh.msg_controllen, scm_count,
                                    has_scm_cred, sk);
        return (uint64)total_len;
    }

    struct lwip_sock *sk = sock_from_fd(fd);
    if (sk == NULL)
        return (uint64)-EBADF;

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

    /* Destination address for datagram sendmsg(). */
    ip_addr_t msg_destip;
    u16_t msg_destport = 0;
    int have_msg_dest = 0;
    if (mh.msg_name != 0 && sk->type != SOCK_STREAM) {
        int addr_ret = sock_copyin_inet_addr(domain, mh.msg_name,
                                             mh.msg_namelen, &msg_destip,
                                             &msg_destport);
        if (addr_ret < 0)
            return (uint64)addr_ret;
        have_msg_dest = 1;
    }

    ssize_t total = 0;

    if (sk->type == SOCK_STREAM) {
        for (uint64 i = 0; i < mh.msg_iovlen; i++) {
            uint64 base = iovs[i].iov_base;
            size_t ilen = iovs[i].iov_len;
            ssize_t n;

            if (ilen == 0)
                continue;

            n = sock_tcp_send_common(sk, fd, base, NULL, ilen, flags, 1);
            if (n < 0)
                return total > 0 ? (uint64)total : (uint64)n;
            total += n;
            if ((size_t)n < ilen)
                goto sendmsg_done;
        }
sendmsg_done:
        ;   /* partial write — fall through to return total */
    } else {
        if (sock_is_nonblock(fd, flags)) {
            int ready = sock_poll_ready(sk, POLLOUT);
            if (!(ready & POLLOUT))
                return (uint64)-EAGAIN;
        }

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

        err_t err;
        if (have_msg_dest)
            err = netconn_sendto(sk->conn, nb, &msg_destip, msg_destport);
        else
            err = netconn_send(sk->conn, nb);
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

        if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
            return (uint64)-EINVAL;

        struct k_iovec iovs[SENDMSG_MAX_IOV];
        uint64 iov_bytes = mh.msg_iovlen * sizeof(struct k_iovec);
        if (iov_bytes != 0 &&
            vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
            return (uint64)-EFAULT;

        /* Read data through the normal unix socket read path */
        struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
        if (f == NULL)
            return (uint64)-EBADF;

        /* Honor MSG_DONTWAIT: temporarily set O_NONBLOCK */
        int saved_fflags = f->f_flags;
        int recv_nonblock = (saved_fflags & O_NONBLOCK) ||
                            (flags & MSG_DONTWAIT);
        if (flags & MSG_DONTWAIT)
            f->f_flags |= O_NONBLOCK;

        ssize_t total = 0;
        ssize_t ret_total = -1;
        int unix_out_flags = 0;
        uint scm_virtual_nread = 0;
        int have_scm_virtual_nread = 0;
        struct vfs_file *scm_files[UNIX_SCM_QUEUE_MAX];
        memset(scm_files, 0, sizeof(scm_files));
        struct unix_scm_cred scm_cred;
        memset(&scm_cred, 0, sizeof(scm_cred));
        int has_scm_cred = 0;
        size_t scm_count = 0;
        int scm_ret = 0;
        int scm_dequeued_with_packet = 0;
        int message_present = 0;
        size_t recv_capacity = 0;
        for (uint64 i = 0; i < mh.msg_iovlen; i++)
            recv_capacity += iovs[i].iov_len;

        if (sk->type == SOCK_SEQPACKET) {
            struct unix_sock *peer = NULL;
            size_t packet_len = 0;

seqpacket_recvmsg_wait:
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

                if (recv_nonblock) {
                    unix_sock_put_ref(peer);
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    chrome_unix_ipc_trace_state("recvmsg-eagain-seqpacket",
                                                fd, -EAGAIN, recv_capacity,
                                                flags, mh.msg_control,
                                                mh.msg_controllen, 0, 0, sk);
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
            char *packet_buf = NULL;
            size_t copied_packet = 0;
            struct vfs_file *peer_wr_file = NULL;
            int recv_ret = unix_seqpacket_recv_buffer(peer, flags,
                                                      recv_capacity,
                                                      &packet_buf,
                                                      &copied_packet,
                                                      &packet_len,
                                                      &scm_virtual_nread,
                                                      &have_scm_virtual_nread,
                                                      scm_files,
                                                      UNIX_SCM_QUEUE_MAX,
                                                      &scm_cred,
                                                      &has_scm_cred,
                                                      &scm_count,
                                                      &scm_dequeued_with_packet,
                                                      &peer_wr_file);
            if (recv_ret != 0) {
                unix_sock_put_ref(peer);
                if (recv_ret == -EAGAIN && !recv_nonblock)
                    goto seqpacket_recvmsg_wait;
                f->f_flags = saved_fflags;
                vfs_fput(f);
                return (uint64)recv_ret;
            }
            message_present = 1;
            if (peer_wr_file != NULL) {
                vfs_file_knote_notify(peer_wr_file, EVFILT_WRITE, 0);
                vfs_fput(peer_wr_file);
            }
            unix_seqpacket_verify_webkit_packet(
                "recvmsg",
                current != NULL ? current->name : NULL,
                (current != NULL && current->thread_group != NULL)
                    ? current->thread_group->exec_path : NULL,
                packet_buf, copied_packet, packet_len);
            if (packet_len > recv_capacity)
                unix_out_flags |= MSG_TRUNC;
            size_t packet_off = 0;
            for (uint64 i = 0; i < mh.msg_iovlen && packet_off < copied_packet; i++) {
                size_t want = iovs[i].iov_len;
                if (want > copied_packet - packet_off)
                    want = copied_packet - packet_off;
                if (want == 0)
                    continue;
                if (vm_copyout(current->vm, iovs[i].iov_base,
                               packet_buf + packet_off, want) < 0) {
                    kvfree(packet_buf);
                    unix_sock_put_ref(peer);
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    return total > 0 ? (uint64)total : (uint64)-EFAULT;
                }
                packet_off += want;
                total += want;
            }
            kvfree(packet_buf);
            if ((flags & MSG_TRUNC) && packet_len > recv_capacity)
                ret_total = (ssize_t)packet_len;
            unix_sock_put_ref(peer);
        } else if (sk->type == SOCK_STREAM && recv_capacity == 0) {
            struct unix_sock *peer = NULL;
            int stream_zero_ready_scm = 0;

            for (;;) {
                bool read_shutdown;
                bool peer_write_shutdown = false;
                size_t readable = 0;

                spin_lock(&sk->lock);
                if (sk->state != UNIX_STATE_CONNECTED &&
                    !(sk->shutdown_flags & UNIX_SHUT_RD)) {
                    spin_unlock(&sk->lock);
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    return (uint64)-ENOTCONN;
                }
                peer = sk->peer;
                unix_sock_get_ref(peer);
                read_shutdown = (sk->shutdown_flags & UNIX_SHUT_RD) != 0;
                spin_unlock(&sk->lock);

                if (peer != NULL) {
                    spin_lock(&peer->lock);
                    readable = peer->tx.nwrite - peer->tx.nread;
                    peer_write_shutdown =
                        (peer->shutdown_flags & UNIX_SHUT_WR) != 0;
                    if (peer->scm_head != peer->scm_tail &&
                        unix_scm_ready_for_stream_zero_recv_locked(
                            &peer->scm_queue[peer->scm_head], peer))
                        stream_zero_ready_scm = 1;
                    spin_unlock(&peer->lock);
                }

                if (readable > 0 || stream_zero_ready_scm ||
                    read_shutdown || peer == NULL || peer_write_shutdown)
                    break;

                unix_sock_put_ref(peer);
                peer = NULL;
                if (recv_nonblock) {
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    chrome_unix_ipc_trace_state("recvmsg-eagain-stream-zero",
                                                fd, -EAGAIN, recv_capacity,
                                                flags, mh.msg_control,
                                                mh.msg_controllen, 0, 0, sk);
                    return (uint64)-EAGAIN;
                }
                if (signal_pending(current)) {
                    f->f_flags = saved_fflags;
                    vfs_fput(f);
                    return (uint64)-EINTR;
                }

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

            if (!(flags & MSG_PEEK) && stream_zero_ready_scm) {
                scm_count = unix_dequeue_scm_stream_zero_recv(
                    peer, scm_files, UNIX_SCM_QUEUE_MAX, &scm_cred,
                    &has_scm_cred);
                scm_dequeued_with_packet = 1;
            }
            if (flags & MSG_PEEK && stream_zero_ready_scm && peer != NULL) {
                spin_lock(&peer->lock);
                scm_virtual_nread = peer->tx.nread + 1;
                spin_unlock(&peer->lock);
                have_scm_virtual_nread = 1;
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
            char tmp[UNIX_IO_CHUNK];

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
                        if (recv_nonblock) {
                            unix_sock_put_ref(peer);
                            f->f_flags = saved_fflags;
                            vfs_fput(f);
                            chrome_unix_ipc_trace_state("recvmsg-eagain-peek",
                                                        fd, -EAGAIN,
                                                        recv_capacity, flags,
                                                        mh.msg_control,
                                                        mh.msg_controllen, 0,
                                                        0, sk);
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
                if (copied < iovs[i].iov_len)
                    break;
            }
        } else {
            int hit_scm_barrier = 0;
            for (uint64 i = 0; i < mh.msg_iovlen; i++) {
                size_t copied_iov = 0;
                while (copied_iov < iovs[i].iov_len) {
                    struct unix_sock *peer;
                    size_t want = iovs[i].iov_len - copied_iov;
                    size_t barrier_bytes = 0;
                    size_t readable = 0;
                    int stop_after_read = 0;

                    spin_lock(&sk->lock);
                    peer = sk->peer;
                    unix_sock_get_ref(peer);
                    spin_unlock(&sk->lock);

                    if (peer != NULL) {
                        spin_lock(&peer->lock);
                        readable = peer->tx.nwrite - peer->tx.nread;
                        if (unix_next_scm_read_window_locked(peer,
                                                             &barrier_bytes,
                                                             &stop_after_read)) {
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
                    if (readable == 0 && total > 0)
                        break;
                    if (readable > 0 && want > readable)
                        want = readable;

                    ssize_t n = unix_sock_read_preserve_scm(
                        f, (char *)(iovs[i].iov_base + copied_iov), want, 1);
                    if (n < 0) {
                        if (total > 0)
                            goto unix_recvmsg_done;
                        f->f_flags = saved_fflags;
                        vfs_fput(f);
                        chrome_unix_ipc_trace_state("recvmsg-read-error", fd,
                                                    n, recv_capacity, flags,
                                                    mh.msg_control,
                                                    mh.msg_controllen, 0, 0,
                                                    sk);
                        return (uint64)n;
                    }
                    total += n;
                    copied_iov += (size_t)n;
                    if ((size_t)n < want)
                        break;
                    if (stop_after_read) {
                        hit_scm_barrier = 1;
                        break;
                    }
                }
                if (hit_scm_barrier || copied_iov < iovs[i].iov_len)
                    break;
            }
        }

unix_recvmsg_done:
        if (ret_total < 0)
            ret_total = total;
        if (total > 0)
            message_present = 1;
        f->f_flags = saved_fflags;
        vfs_fput(f);

        /* Check for pending ancillary data from peer. */
        struct unix_sock *peer = NULL;
        spin_lock(&sk->lock);
        peer = sk->peer;
        unix_sock_get_ref(peer);
        spin_unlock(&sk->lock);

        if (flags & MSG_PEEK) {
            if (!have_scm_virtual_nread && peer != NULL) {
                spin_lock(&peer->lock);
                scm_virtual_nread = peer->tx.nread + (uint)total;
                spin_unlock(&peer->lock);
                have_scm_virtual_nread = 1;
            }
            scm_ret = unix_peek_scm(peer, scm_virtual_nread, scm_files,
                                    UNIX_SCM_QUEUE_MAX, &scm_cred,
                                    &has_scm_cred, &scm_count);
        } else if (!scm_dequeued_with_packet) {
            scm_count = unix_dequeue_scm(peer, scm_files, UNIX_SCM_QUEUE_MAX,
                                         &scm_cred, &has_scm_cred);
        }
        CHROME_UNIX_SOCKET_TRACE("recvmsg-scm pid=%d tgid=%d name=%s fd=%d "
                                 "bytes=%ld ret_bytes=%ld flags=0x%x "
                                 "control=0x%lx controllen=%lu scm_count=%lu "
                                 "has_cred=%d peek=%d scm_ret=%d\n",
                                 current->pid, current->tgid, current->name,
                                 fd, (long)total, (long)ret_total, flags,
                                 mh.msg_control, mh.msg_controllen,
                                 (unsigned long)scm_count, has_scm_cred,
                                 (flags & MSG_PEEK) != 0, scm_ret);
        chrome_unix_ipc_trace_state("recvmsg-done", fd, ret_total,
                                    recv_capacity, flags, mh.msg_control,
                                    mh.msg_controllen, scm_count,
                                    has_scm_cred, sk);
        chrome_unix_ipc_trace_recv_payload_iov("recvmsg-done", fd, ret_total,
                                               (size_t)total, flags,
                                               scm_count, has_scm_cred, iovs,
                                               mh.msg_iovlen);
        unix_sock_put_ref(peer);

        int emit_ret = 0;
        if (scm_ret < 0) {
            unix_out_flags |= MSG_CTRUNC;
        } else {
            emit_ret = unix_emit_recvmsg_scm(sk, &mh, umsg, fd, flags, total,
                                             message_present, scm_files,
                                             scm_count, &scm_cred,
                                             has_scm_cred, &unix_out_flags);
            if (emit_ret < 0) {
                chrome_unix_ipc_trace_state("recvmsg-emit-error", fd,
                                            emit_ret, recv_capacity, flags,
                                            mh.msg_control,
                                            mh.msg_controllen, scm_count,
                                            has_scm_cred, sk);
                return (uint64)emit_ret;
            }
        }

        /* Clear msg_flags */
        if (vm_copyout(current->vm,
                       umsg + __builtin_offsetof(struct k_msghdr, msg_flags),
                       &unix_out_flags, sizeof(unix_out_flags)) < 0)
            return (uint64)-EFAULT;

        return (uint64)ret_total;
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
        size_t recvd_acc = 0;

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
                    if (err == ERR_CLSD) {
                        sk->rx_eof = 1;
                        sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                        return total > 0 ? (uint64)total : 0;
                    }
                    if (err == ERR_CONN && sk->rx_eof) {
                        sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                        return total > 0 ? (uint64)total : 0;
                    }
                    if (err == ERR_TIMEOUT && signal_pending(current)) {
                        sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                        return total > 0 ? (uint64)total : (uint64)-EINTR;
                    }
                    if (total > 0 && (err == ERR_WOULDBLOCK || err == ERR_TIMEOUT)) {
                        if (sock_tcp_wait_for_more(sk, nonblock, total,
                                                   buf_total, &wait_more))
                            continue;
                        break;
                    }
                    sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
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
                        sock_tcp_save_remainder(sk, p, poff);
                    } else if (!reused_lastpbuf) {
                        pbuf_free(p);
                    }
                    if (!(flags & MSG_PEEK) && copied_from_pbuf > 0)
                        sock_tcp_recvd_accum(sk, &recvd_acc,
                                             copied_from_pbuf, 1);
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
            } else {
                sock_tcp_save_remainder(sk, p, poff);
            }

            if (!(flags & MSG_PEEK))
                sock_tcp_recvd_accum(sk, &recvd_acc, copied_from_pbuf, 0);
            if (copied_from_pbuf == 0)
                break;
        }

        if (!(flags & MSG_PEEK))
            sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
    } else {
        /* UDP/RAW: receive one datagram, scatter into iovecs */
        struct netbuf *nb = NULL;
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

        total = sock_udp_copy_netbuf_to_iovs(nb, iovs, mh.msg_iovlen,
                                             buf_total, flags, &out_flags);
        if (total < 0) {
            netbuf_delete(nb);
            return (uint64)total;
        }

        /* Copy source address into msg_name if requested */
        if (mh.msg_name != 0) {
            const ip_addr_t *fromaddr = netbuf_fromaddr(nb);
            u16_t fromport = netbuf_fromport(nb);

            char storage[K_SOCKADDR_IN6_SIZE];
            int nlen = 0;
            int pack_ret = sock_pack_inet_addr(domain, fromaddr, fromport,
                                               storage, &nlen);
            if (pack_ret < 0) {
                netbuf_delete(nb);
                return (uint64)pack_ret;
            }
            if (mh.msg_namelen >= (uint32)nlen &&
                vm_copyout(current->vm, mh.msg_name, storage, nlen) < 0) {
                netbuf_delete(nb);
                return (uint64)-EFAULT;
            }
            /* Update msg_namelen in user msghdr */
            uint32 out_nlen = (uint32)nlen;
            vm_copyout(current->vm,
                        umsg + __builtin_offsetof(struct k_msghdr, msg_namelen),
                        &out_nlen, sizeof(out_nlen));
        }

        if (flags & MSG_PEEK) {
            sk->lastbuf = nb;
            sk->lastoffset = 0;
        } else {
            sk->lastbuf = NULL;
            sk->lastoffset = 0;
            netbuf_delete(nb);
        }
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
    if (sk->type == SOCK_STREAM && !(flags & MSG_PEEK) && total > 0)
        sock_notify_if_still_readable(fd, sk);
    return (uint64)total;
}

/*
 * sys_socketpair(domain, type, protocol, sv[2]) → 0 / -errno
 *
 * Only AF_UNIX is supported for socketpair.
 */
static void chrome_unix_ipc_trace_socketpair_result(int fd0, int fd1,
                                                    int type, int protocol,
                                                    int flags)
{
    if (!unix_ipc_trace_process())
        return;

    struct vfs_file *file0 = vfs_fdtable_get_file(current->fdtable, fd0);
    struct vfs_file *file1 = vfs_fdtable_get_file(current->fdtable, fd1);
    struct chrome_unix_file_snapshot snap0;
    struct chrome_unix_file_snapshot snap1;

    chrome_unix_file_snapshot_fill(file0, &snap0);
    chrome_unix_file_snapshot_fill(file1, &snap1);

    printf("chrome-unix-ipc: socketpair pid=%d tgid=%d name=%s "
           "fd0=%d fd1=%d type=%d protocol=%d flags=0x%x cloexec=%d "
           "path0=%s path1=%s file0=%p file1=%p f_flags0=0x%x "
           "f_flags1=0x%x fd0_is_unix=%d fd0_sk=%p fd0_peer=%p "
           "fd0_ino=%llu fd0_peer_ino=%llu fd0_file_refs=%d "
           "fd0_peer_file=%p fd0_peer_file_refs=%d fd0_type=%d "
           "fd0_state=%d fd0_shutdown=0x%x fd0_err=%d fd1_is_unix=%d "
           "fd1_sk=%p fd1_peer=%p fd1_ino=%llu fd1_peer_ino=%llu "
           "fd1_file_refs=%d fd1_peer_file=%p fd1_peer_file_refs=%d "
           "fd1_type=%d fd1_state=%d fd1_shutdown=0x%x fd1_err=%d\n",
           current->pid, current->tgid, current->name, fd0, fd1, type,
           protocol, flags, (flags & SOCK_CLOEXEC) != 0,
           chrome_socket_trace_path(file0), chrome_socket_trace_path(file1),
           file0, file1, file0 != NULL ? file0->f_flags : 0,
           file1 != NULL ? file1->f_flags : 0, snap0.is_unix, snap0.sk,
           snap0.peer, (unsigned long long)snap0.ino,
           (unsigned long long)snap0.peer_ino, snap0.visible_refs,
           snap0.peer_file, snap0.peer_visible_refs, snap0.type,
           snap0.state, snap0.shutdown, snap0.err, snap1.is_unix,
           snap1.sk, snap1.peer, (unsigned long long)snap1.ino,
           (unsigned long long)snap1.peer_ino, snap1.visible_refs,
           snap1.peer_file, snap1.peer_visible_refs, snap1.type,
           snap1.state, snap1.shutdown, snap1.err);

    if (file0 != NULL)
        vfs_fput(file0);
    if (file1 != NULL)
        vfs_fput(file1);
}

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

    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return (uint64)-EINVAL;

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
    if (vm_copyout(current->vm, usv, sv, sizeof(sv)) < 0) {
        socket_unwind_created_fd(sv[0]);
        socket_unwind_created_fd(sv[1]);
        return (uint64)-EFAULT;
    }

    chrome_unix_ipc_trace_socketpair_result(sv[0], sv[1], type, protocol,
                                            flags);

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

        if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
            return sent > 0 ? (uint64)sent : (uint64)-EINVAL;

        struct k_iovec iovs[SENDMSG_MAX_IOV];
        uint64 iov_bytes = (uint64)mh.msg_iovlen * sizeof(struct k_iovec);
        if (iov_bytes != 0 &&
            vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
            return sent > 0 ? (uint64)sent : (uint64)-EFAULT;

        /* Gather data */
        size_t total = 0;
        for (uint64 j = 0; j < mh.msg_iovlen; j++) {
            if (iovs[j].iov_len > UNIX_BUF_MAX_SIZE - total)
                return sent > 0 ? (uint64)sent : (uint64)-EMSGSIZE;
            total += iovs[j].iov_len;
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

            char *msg_buf = kvmalloc(total ? total : 1);
            if (msg_buf == NULL)
                return sent > 0 ? (uint64)sent : (uint64)-ENOMEM;
            size_t off = 0;
            for (uint64 j = 0; j < mh.msg_iovlen; j++) {
                if (iovs[j].iov_len == 0)
                    continue;
                if (vm_copyin(current->vm, msg_buf + off,
                              iovs[j].iov_base, iovs[j].iov_len) < 0) {
                    kvfree(msg_buf);
                    return sent > 0 ? (uint64)sent : (uint64)-EFAULT;
                }
                off += iovs[j].iov_len;
            }

            struct vfs_file *scm_files[UNIX_SCM_QUEUE_MAX];
            memset(scm_files, 0, sizeof(scm_files));
            size_t scm_count = 0;
            struct unix_scm_cred scm_cred;
            memset(&scm_cred, 0, sizeof(scm_cred));
            int has_scm_cred = 0;

            int scm_ret = unix_collect_scm_from_control(mh.msg_control,
                                                        mh.msg_controllen,
                                                        scm_files,
                                                        &scm_count,
                                                        &scm_cred,
                                                        &has_scm_cred);
            if (scm_ret < 0) {
                unix_discard_scm_rights(scm_files, 0, scm_count);
                kvfree(msg_buf);
                return sent > 0 ? (uint64)sent : (uint64)scm_ret;
            }

            struct vfs_file *notify_file = NULL;
            char *grow_data = NULL;
            size_t grow_capacity = 0;
            int ret;
            int nonblock = sock_is_nonblock(sockfd, flags);
            int msg_buf_queued = 0;
            if (total == 0 && usk->type == SOCK_STREAM) {
                unix_discard_scm_rights(scm_files, 0, scm_count);
                kvfree(msg_buf);
                n = 0;
                goto sendmmsg_entry_done;
            }
            for (;;) {
                spin_lock(&usk->lock);
                char *old_data = NULL;
                if (grow_data != NULL) {
                    old_data = unix_ring_install_storage_locked(usk,
                                                                grow_data,
                                                                grow_capacity);
                    grow_data = NULL;
                    grow_capacity = 0;
                }

                size_t needed_capacity = 0;
                ret = unix_sendmsg_growth_needed_locked(usk, total,
                                                        &needed_capacity);
                if (old_data != NULL) {
                    spin_unlock(&usk->lock);
                    kvfree(old_data);
                    continue;
                }
                if (ret < 0) {
                    spin_unlock(&usk->lock);
                    break;
                }
                if (needed_capacity != 0) {
                    spin_unlock(&usk->lock);
                    grow_data = kvmalloc(needed_capacity);
                    if (grow_data == NULL) {
                        ret = -ENOMEM;
                        break;
                    }
                    grow_capacity = needed_capacity;
                    continue;
                }

                ret = unix_sendmsg_atomic_locked(usk, msg_buf, total,
                                                 scm_files, scm_count,
                                                 &scm_cred, has_scm_cred,
                                                 &msg_buf_queued);
                if (ret == 0 && usk->peer != NULL) {
                    tq_wakeup_all(&usk->peer->rd_queue, 0, 0);
                    if (usk->peer->file != NULL)
                        notify_file = vfs_fdup(usk->peer->file);
                }
                if (ret == 0 || ret != -EAGAIN || nonblock) {
                    spin_unlock(&usk->lock);
                    break;
                }
                if (signal_pending(current)) {
                    ret = -EINTR;
                    spin_unlock(&usk->lock);
                    break;
                }
                tq_wait_in_state(&usk->wr_queue, &usk->lock, NULL,
                                 THREAD_INTERRUPTIBLE);
                spin_unlock(&usk->lock);

                if (signal_pending(current)) {
                    ret = -EINTR;
                    break;
                }
            }
            if (grow_data != NULL)
                kvfree(grow_data);
            if (!msg_buf_queued)
                kvfree(msg_buf);

            if (ret < 0) {
                for (size_t k = 0; k < scm_count; k++)
                    vfs_fput(scm_files[k]);
            } else {
                n = (ssize_t)total;
            }
            if (notify_file != NULL) {
                vfs_file_knote_notify(notify_file, EVFILT_READ, 0);
                vfs_fput(notify_file);
            }
            if (ret < 0)
                n = ret;
        } else {
            struct lwip_sock *sk = sock_from_fd(sockfd);
            if (sk == NULL)
                return sent > 0 ? (uint64)sent
                                : (uint64)sock_fd_type_error(sockfd);

            char tmpbuf[8192];
            if (total > sizeof(tmpbuf))
                return sent > 0 ? (uint64)sent : (uint64)-EMSGSIZE;
            size_t off = 0;
            for (uint64 j = 0; j < mh.msg_iovlen; j++) {
                size_t chunk = iovs[j].iov_len;
                if (vm_copyin(current->vm, tmpbuf + off,
                              iovs[j].iov_base, chunk) < 0)
                    return sent > 0 ? (uint64)sent : (uint64)-EFAULT;
                off += chunk;
            }

            if (sk->type == SOCK_STREAM) {
                n = sock_tcp_send_common(sk, sockfd, 0, tmpbuf, total,
                                         flags, 0);
            } else {
                ip_addr_t msg_destip;
                u16_t msg_destport = 0;
                int have_msg_dest = 0;
                if (mh.msg_name != 0) {
                    int addr_ret = sock_copyin_inet_addr(domain, mh.msg_name,
                                                         mh.msg_namelen,
                                                         &msg_destip,
                                                         &msg_destport);
                    if (addr_ret < 0)
                        return sent > 0 ? (uint64)sent : (uint64)addr_ret;
                    have_msg_dest = 1;
                }
                struct netbuf nb;
                memset(&nb, 0, sizeof(nb));
                struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (uint16)total, PBUF_RAM);
                if (p == NULL)
                    return sent > 0 ? (uint64)sent : (uint64)-ENOMEM;
                pbuf_take(p, tmpbuf, (uint16)total);
                nb.p = p;
                err_t err = have_msg_dest
                    ? netconn_sendto(sk->conn, &nb, &msg_destip, msg_destport)
                    : netconn_send(sk->conn, &nb);
                pbuf_free(p);
                n = (err == ERR_OK) ? (ssize_t)total : -(ssize_t)lwip_err_to_errno(err);
            }
        }

        if (n < 0)
            return sent > 0 ? (uint64)sent : (uint64)n;

sendmmsg_entry_done:
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
    uint64 utimeout;
    argint(0, &sockfd);
    argaddr(1, &umsgvec);
    argint(2, &vlen);
    argint(3, &flags);
    argaddr(4, &utimeout);

    if (vlen <= 0)
        return (uint64)-EINVAL;
    if (vlen > 1024)
        vlen = 1024;

    int has_timeout = 0;
    int timeout_is_zero = 0;
    uint64 timeout_ms = 0;
    uint64 deadline_ms = 0;
    if (utimeout != 0) {
        struct sock_timespec ts;
        if (vm_copyin(current->vm, &ts, utimeout, sizeof(ts)) < 0)
            return (uint64)-EFAULT;
        int ret = sock_timespec_to_ms(&ts, &timeout_ms);
        if (ret != 0)
            return (uint64)ret;
        has_timeout = 1;
        timeout_is_zero = timeout_ms == 0;
        if (!timeout_is_zero) {
            uint64 now = sched_timer_now_ms();
            deadline_ms = ((uint64)-1) - now < timeout_ms
                ? (uint64)-1 : now + timeout_ms;
        }
    }

    const uint64 mmsghdr_size = sizeof(struct k_msghdr) + 8;
    int received = 0;

    for (int i = 0; i < vlen; i++) {
        uint64 entry_addr = umsgvec + (uint64)i * mmsghdr_size;

        struct k_msghdr mh;
        if (vm_copyin(current->vm, &mh, entry_addr, sizeof(mh)) < 0)
            return received > 0 ? (uint64)received : (uint64)-EFAULT;

        if (mh.msg_iovlen < 0 || mh.msg_iovlen > SENDMSG_MAX_IOV)
            return received > 0 ? (uint64)received : (uint64)-EINVAL;

        struct k_iovec iovs[SENDMSG_MAX_IOV];
        uint64 iov_bytes = (uint64)mh.msg_iovlen * sizeof(struct k_iovec);
        if (iov_bytes != 0 &&
            vm_copyin(current->vm, iovs, mh.msg_iov, iov_bytes) < 0)
            return received > 0 ? (uint64)received : (uint64)-EFAULT;

        /* Determine domain */
        int domain = sock_domain_from_fd(sockfd);
        int recv_flags = (i > 0) ? (flags | MSG_DONTWAIT) : flags;
        if (timeout_is_zero)
            recv_flags |= MSG_DONTWAIT;
        if ((flags & MSG_WAITFORONE) && received > 0)
            recv_flags |= MSG_DONTWAIT;
        ssize_t total = 0;
        ssize_t msg_len_total = -1;
        int msg_flags = 0;
        int count_zero_message = 0;
        uint scm_virtual_nread = 0;
        int have_scm_virtual_nread = 0;

        if (has_timeout && !timeout_is_zero && received == 0) {
            int expired = 0;
            (void)sock_timeout_remaining_ms(deadline_ms, &expired);
            if (expired)
                return 0;
        }

        if (domain == AF_UNIX) {
            struct unix_sock *usk = unix_sock_from_fd(sockfd);
            if (usk == NULL)
                return received > 0 ? (uint64)received : (uint64)-EBADF;

            struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, sockfd);
            if (f == NULL)
                return received > 0 ? (uint64)received : (uint64)-EBADF;

            int saved_fflags = f->f_flags;
            int recv_nonblock = (saved_fflags & O_NONBLOCK) ||
                                (recv_flags & MSG_DONTWAIT);
            if (recv_flags & MSG_DONTWAIT)
                f->f_flags |= O_NONBLOCK;

            int out_flags = 0;
            struct vfs_file *scm_files[UNIX_SCM_QUEUE_MAX];
            memset(scm_files, 0, sizeof(scm_files));
            struct unix_scm_cred scm_cred;
            memset(&scm_cred, 0, sizeof(scm_cred));
            int has_scm_cred = 0;
            size_t scm_count = 0;
            int scm_ret = 0;
            int scm_dequeued_with_packet = 0;
            if (usk->type == SOCK_SEQPACKET) {
                struct unix_sock *peer = NULL;
                size_t packet_len = 0;

seqpacket_recvmmsg_wait:
                for (;;) {
                    bool read_shutdown;
                    bool peer_write_shutdown = false;
                    int pkt_ret;

                    spin_lock(&usk->lock);
                    peer = usk->peer;
                    unix_sock_get_ref(peer);
                    read_shutdown = (usk->shutdown_flags & UNIX_SHUT_RD) != 0;
                    spin_unlock(&usk->lock);

                    if (peer == NULL) {
                        unix_sock_put_ref(peer);
                        break;
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
                        peer = NULL;
                        break;
                    }
                    if (recv_nonblock) {
                        unix_sock_put_ref(peer);
                        f->f_flags = saved_fflags;
                        vfs_fput(f);
                        chrome_unix_ipc_trace_state("recvmmsg-eagain-seqpacket",
                                                    sockfd, -EAGAIN, 0,
                                                    recv_flags, mh.msg_control,
                                                    mh.msg_controllen, 0, 0,
                                                    usk);
                        return received > 0 ? (uint64)received : (uint64)-EAGAIN;
                    }
                    if (signal_pending(current)) {
                        unix_sock_put_ref(peer);
                        f->f_flags = saved_fflags;
                        vfs_fput(f);
                        return received > 0 ? (uint64)received : (uint64)-EINTR;
                    }

                    unix_sock_put_ref(peer);
                    peer = NULL;
                    spin_lock(&usk->lock);
                    tq_wait_in_state(&usk->rd_queue, &usk->lock, NULL,
                                     THREAD_INTERRUPTIBLE);
                    spin_unlock(&usk->lock);
                    if (signal_pending(current)) {
                        f->f_flags = saved_fflags;
                        vfs_fput(f);
                        return received > 0 ? (uint64)received : (uint64)-EINTR;
                    }
                }

                if (peer != NULL) {
                    count_zero_message = 1;
                    size_t capacity = 0;
                    for (uint64 j = 0; j < mh.msg_iovlen; j++)
                        capacity += iovs[j].iov_len;

                    char *packet_buf = NULL;
                    size_t copied_packet = 0;
                    struct vfs_file *peer_wr_file = NULL;
                    int recv_ret = unix_seqpacket_recv_buffer(peer,
                                                              recv_flags,
                                                              capacity,
                                                              &packet_buf,
                                                              &copied_packet,
                                                              &packet_len,
                                                              &scm_virtual_nread,
                                                              &have_scm_virtual_nread,
                                                              scm_files,
                                                              UNIX_SCM_QUEUE_MAX,
                                                              &scm_cred,
                                                              &has_scm_cred,
                                                              &scm_count,
                                                              &scm_dequeued_with_packet,
                                                              &peer_wr_file);
                    if (recv_ret != 0) {
                        unix_sock_put_ref(peer);
                        if (recv_ret == -EAGAIN && !recv_nonblock)
                            goto seqpacket_recvmmsg_wait;
                        f->f_flags = saved_fflags;
                        vfs_fput(f);
                        return received > 0 ? (uint64)received : (uint64)recv_ret;
                    }
                    if (peer_wr_file != NULL) {
                        vfs_file_knote_notify(peer_wr_file, EVFILT_WRITE, 0);
                        vfs_fput(peer_wr_file);
                    }
                    unix_seqpacket_verify_webkit_packet(
                        "recvmmsg",
                        current != NULL ? current->name : NULL,
                        (current != NULL && current->thread_group != NULL)
                            ? current->thread_group->exec_path : NULL,
                        packet_buf, copied_packet, packet_len);
                    if (packet_len > capacity)
                        out_flags |= MSG_TRUNC;

                    size_t packet_off = 0;
                    for (uint64 j = 0; j < mh.msg_iovlen &&
                         packet_off < copied_packet; j++) {
                        size_t want = iovs[j].iov_len;
                        if (want > copied_packet - packet_off)
                            want = copied_packet - packet_off;
                        if (want == 0)
                            continue;
                        if (vm_copyout(current->vm, iovs[j].iov_base,
                                       packet_buf + packet_off, want) < 0) {
                            kvfree(packet_buf);
                            unix_sock_put_ref(peer);
                            f->f_flags = saved_fflags;
                            vfs_fput(f);
                            return received > 0 ? (uint64)received : (uint64)-EFAULT;
                        }
                        packet_off += want;
                        total += want;
                    }
                    kvfree(packet_buf);
                    if ((recv_flags & MSG_TRUNC) && packet_len > capacity)
                        msg_len_total = (ssize_t)packet_len;
                    unix_sock_put_ref(peer);
                }
            } else if (recv_flags & MSG_PEEK) {
                size_t peek_off = 0;
                char tmp[UNIX_IO_CHUNK];

                for (uint64 j = 0; j < mh.msg_iovlen; j++) {
                    size_t copied_iov = 0;
                    while (copied_iov < iovs[j].iov_len) {
                        struct unix_sock *peer;
                        size_t want = iovs[j].iov_len - copied_iov;
                        if (want > sizeof(tmp))
                            want = sizeof(tmp);

                        spin_lock(&usk->lock);
                        peer = usk->peer;
                        unix_sock_get_ref(peer);
                        spin_unlock(&usk->lock);

                        size_t got = 0;
                        if (peer != NULL) {
                            spin_lock(&peer->lock);
                            got = unix_ring_peek(&peer->tx, peek_off, tmp, want);
                            spin_unlock(&peer->lock);
                        }

                        if (got == 0) {
                            unix_sock_put_ref(peer);
                            break;
                        }
                        if (vm_copyout(current->vm,
                                       iovs[j].iov_base + copied_iov,
                                       tmp, got) < 0) {
                            unix_sock_put_ref(peer);
                            f->f_flags = saved_fflags;
                            vfs_fput(f);
                            return received > 0 ? (uint64)received : (uint64)-EFAULT;
                        }
                        copied_iov += got;
                        peek_off += got;
                        total += got;
                        unix_sock_put_ref(peer);
                        if (got < want)
                            break;
                    }
                    if (copied_iov < iovs[j].iov_len)
                        break;
                }
            } else {
                int hit_scm_barrier = 0;
                for (uint64 j = 0; j < mh.msg_iovlen; j++) {
                    size_t copied_iov = 0;
                    while (copied_iov < iovs[j].iov_len) {
                        struct unix_sock *peer;
                        size_t want = iovs[j].iov_len - copied_iov;
                        size_t barrier_bytes = 0;
                        size_t readable = 0;
                        int stop_after_read = 0;

                        spin_lock(&usk->lock);
                        peer = usk->peer;
                        unix_sock_get_ref(peer);
                        spin_unlock(&usk->lock);

                        if (peer != NULL) {
                            spin_lock(&peer->lock);
                            readable = peer->tx.nwrite - peer->tx.nread;
                            if (unix_next_scm_read_window_locked(peer,
                                                                 &barrier_bytes,
                                                                 &stop_after_read)) {
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
                        if (readable == 0 && total > 0)
                            break;
                        if (readable > 0 && want > readable)
                            want = readable;

                        ssize_t n = unix_sock_read_preserve_scm(
                            f, (char *)(iovs[j].iov_base + copied_iov),
                            want, 1);
                        if (n < 0) {
                            f->f_flags = saved_fflags;
                            vfs_fput(f);
                            chrome_unix_ipc_trace_state("recvmmsg-read-error",
                                                        sockfd, n, 0,
                                                        recv_flags,
                                                        mh.msg_control,
                                                        mh.msg_controllen, 0,
                                                        0, usk);
                            return received > 0 ? (uint64)received : (uint64)n;
                        }
                        total += n;
                        copied_iov += (size_t)n;
                        if ((size_t)n < want)
                            break;
                        if (stop_after_read) {
                            hit_scm_barrier = 1;
                            break;
                        }
                    }
                    if (hit_scm_barrier || copied_iov < iovs[j].iov_len)
                        break;
                }
            }
            f->f_flags = saved_fflags;
            vfs_fput(f);

            struct unix_sock *peer = NULL;
            spin_lock(&usk->lock);
            peer = usk->peer;
            unix_sock_get_ref(peer);
            spin_unlock(&usk->lock);

            if (recv_flags & MSG_PEEK) {
                if (!have_scm_virtual_nread && peer != NULL) {
                    spin_lock(&peer->lock);
                    scm_virtual_nread = peer->tx.nread + (uint)total;
                    spin_unlock(&peer->lock);
                    have_scm_virtual_nread = 1;
                }
                scm_ret = unix_peek_scm(peer, scm_virtual_nread, scm_files,
                                        UNIX_SCM_QUEUE_MAX, &scm_cred,
                                        &has_scm_cred, &scm_count);
            } else if (!scm_dequeued_with_packet) {
                scm_count = unix_dequeue_scm(peer, scm_files,
                                             UNIX_SCM_QUEUE_MAX,
                                             &scm_cred, &has_scm_cred);
            }
            unix_sock_put_ref(peer);

            int emit_ret = 0;
            if (scm_ret < 0) {
                out_flags |= MSG_CTRUNC;
            } else {
                emit_ret = unix_emit_recvmsg_scm(usk, &mh, entry_addr, sockfd,
                                                 recv_flags, total,
                                                 count_zero_message,
                                                 scm_files, scm_count,
                                                 &scm_cred, has_scm_cred,
                                                 &out_flags);
                if (emit_ret < 0)
                    return received > 0 ? (uint64)received : (uint64)emit_ret;
            }

            msg_flags = out_flags;
            chrome_unix_ipc_trace_state("recvmmsg-done", sockfd,
                                        msg_len_total < 0 ? total : msg_len_total,
                                        0, recv_flags, mh.msg_control,
                                        mh.msg_controllen, scm_count,
                                        has_scm_cred, usk);
        } else {
            struct lwip_sock *sk = sock_from_fd(sockfd);
            if (sk == NULL)
                return received > 0 ? (uint64)received : (uint64)-EBADF;

            if (sock_is_nonblock(sockfd, recv_flags)) {
                int ready = sock_poll_ready(sk, POLLIN);
                if (!(ready & (POLLIN | POLLHUP | POLLERR))) {
                    if (received > 0) break;
                    return (uint64)-EAGAIN;
                }
            }

            if (sk->type == SOCK_STREAM) {
                size_t buf_total = 0;
                for (uint64 j = 0; j < mh.msg_iovlen; j++)
                    buf_total += iovs[j].iov_len;

                uint64 iov_idx = 0;
                size_t iov_off = 0;
                int nonblock = sock_is_nonblock(sockfd, recv_flags);
                int wait_more = 0;
                char tmpbuf[1500];
                size_t recvd_acc = 0;

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
                        uint32 recv_timeout_ms = 0;
                        if (has_timeout && !timeout_is_zero && !nonblock &&
                            total == 0 && received == 0) {
                            int expired = 0;
                            recv_timeout_ms =
                                sock_timeout_remaining_ms(deadline_ms,
                                                          &expired);
                            if (expired)
                                return 0;
                            if (recv_timeout_ms == 0)
                                recv_timeout_ms = 1;
                        }
                        err_t err = sock_tcp_recv_pbuf_timeout(sk, &p,
                            nonblock || total > 0, recv_timeout_ms);
                        if (err != ERR_OK) {
                            if (err == ERR_TIMEOUT && has_timeout &&
                                received == 0 && total == 0) {
                                sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                                return 0;
                            }
                            if (err == ERR_CLSD && received > 0) {
                                sk->rx_eof = 1;
                                sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                                break;
                            }
                            if (err == ERR_CLSD) {
                                sk->rx_eof = 1;
                                sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                                return 0;
                            }
                            if (err == ERR_CONN && sk->rx_eof) {
                                sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                                if (received > 0)
                                    break;
                                return total > 0 ? (uint64)total : 0;
                            }
                            if (err == ERR_TIMEOUT && signal_pending(current)) {
                                sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                                if (received > 0)
                                    break;
                                return total > 0 ? (uint64)total : (uint64)-EINTR;
                            }
                            if (total > 0 &&
                                (err == ERR_WOULDBLOCK || err == ERR_TIMEOUT)) {
                                if (sock_tcp_wait_for_more(sk, nonblock, total,
                                                           buf_total, &wait_more))
                                    continue;
                                break;
                            }
                            sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);
                            if (received > 0)
                                break;
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

                        uint16 got = pbuf_copy_partial(p, tmpbuf, (uint16)chunk,
                                                       poff);
                        if (got == 0)
                            break;
                        if (vm_copyout(current->vm,
                                       iovs[iov_idx].iov_base + iov_off,
                                       tmpbuf, got) < 0) {
                            if (!(recv_flags & MSG_PEEK) && copied_from_pbuf > 0) {
                                sock_tcp_save_remainder(sk, p, poff);
                            } else if (!reused_lastpbuf) {
                                pbuf_free(p);
                            }
                            if (!(recv_flags & MSG_PEEK))
                                sock_tcp_recvd_accum(sk, &recvd_acc,
                                                     copied_from_pbuf, 1);
                            return received > 0 ? (uint64)received : (uint64)-EFAULT;
                        }

                        poff += got;
                        iov_off += got;
                        total += got;
                        copied_from_pbuf += got;
                    }

                    if (recv_flags & MSG_PEEK) {
                        sk->lastpbuf = p;
                        sk->lastpbuf_off = start_off;
                        break;
                    } else {
                        sock_tcp_save_remainder(sk, p, poff);
                    }

                    if (!(recv_flags & MSG_PEEK))
                        sock_tcp_recvd_accum(sk, &recvd_acc,
                                             copied_from_pbuf, 0);
                    if (copied_from_pbuf == 0)
                        break;
                }

                /* Single MSG_RECVD round-trip per recvmmsg() entry. */
                if (!(recv_flags & MSG_PEEK))
                    sock_tcp_recvd_accum(sk, &recvd_acc, 0, 1);


                if (!(recv_flags & MSG_PEEK) && total > 0) {
                    sock_notify_if_still_readable(sockfd, sk);
                }
            } else {
                struct netbuf *nb = NULL;
                size_t buf_total = 0;
                for (uint64 j = 0; j < mh.msg_iovlen; j++)
                    buf_total += iovs[j].iov_len;

                uint32 recv_timeout_ms = 0;
                if (has_timeout && !timeout_is_zero &&
                    !sock_is_nonblock(sockfd, recv_flags) && received == 0) {
                    int expired = 0;
                    recv_timeout_ms =
                        sock_timeout_remaining_ms(deadline_ms, &expired);
                    if (expired)
                        return 0;
                    if (recv_timeout_ms == 0)
                        recv_timeout_ms = 1;
                }
                if (sk->lastbuf != NULL) {
                    nb = sk->lastbuf;
                } else {
                    err_t err = sock_netconn_recv_timeout(sk, &nb,
                                                          recv_timeout_ms);
                    if (err != ERR_OK) {
                        if (err == ERR_TIMEOUT && has_timeout && received == 0)
                            return 0;
                        if (received > 0) break;
                        return (uint64)-lwip_err_to_errno(err);
                    }
                }
                count_zero_message = 1;
                total = sock_udp_copy_netbuf_to_iovs(nb, iovs, mh.msg_iovlen,
                                                     buf_total, recv_flags,
                                                     &msg_flags);
                if (total < 0) {
                    netbuf_delete(nb);
                    return received > 0 ? (uint64)received : (uint64)total;
                }

                if (mh.msg_name != 0) {
                    const ip_addr_t *fromaddr = netbuf_fromaddr(nb);
                    u16_t fromport = netbuf_fromport(nb);
                    char storage[K_SOCKADDR_IN6_SIZE];
                    int nlen = 0;
                    int pack_ret = sock_pack_inet_addr(domain, fromaddr,
                                                       fromport, storage,
                                                       &nlen);
                    if (pack_ret < 0) {
                        netbuf_delete(nb);
                        return received > 0 ? (uint64)received
                                            : (uint64)pack_ret;
                    }
                    if (mh.msg_namelen >= (uint32)nlen &&
                        vm_copyout(current->vm, mh.msg_name, storage,
                                   nlen) < 0) {
                        netbuf_delete(nb);
                        return received > 0 ? (uint64)received
                                            : (uint64)-EFAULT;
                    }
                    uint32 out_nlen = (uint32)nlen;
                    vm_copyout(current->vm,
                               entry_addr + __builtin_offsetof(struct k_msghdr,
                                                               msg_namelen),
                               &out_nlen, sizeof(out_nlen));
                }

                if (recv_flags & MSG_PEEK) {
                    sk->lastbuf = nb;
                    sk->lastoffset = 0;
                } else {
                    sk->lastbuf = NULL;
                    sk->lastoffset = 0;
                    netbuf_delete(nb);
                }
            }
        }

        /* Write msg_len back to user mmsghdr */
        if (msg_len_total < 0)
            msg_len_total = total;
        uint32 msg_len = (uint32)msg_len_total;
        vm_copyout(current->vm,
                   entry_addr + sizeof(struct k_msghdr),
                   &msg_len, sizeof(msg_len));

        vm_copyout(current->vm,
                   entry_addr + __builtin_offsetof(struct k_msghdr, msg_flags),
                   &msg_flags, sizeof(msg_flags));

        /*
         * Zero-length datagrams/seqpackets are real messages.  A
         * zero-length stream result is EOF (or no additional data after
         * previous stream messages) and must not consume a mmsghdr slot.
         */
        if (total == 0 && !count_zero_message)
            break;

        received++;

        if (total == 0)
            break;
    }

    return (uint64)received;
}
