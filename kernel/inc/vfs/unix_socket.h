/*
 * unix_socket.h - AF_UNIX (UNIX domain) socket declarations
 *
 * UNIX domain sockets provide local inter-process communication.
 * Supports SOCK_STREAM (connection-oriented byte stream), SOCK_SEQPACKET
 * (connection-oriented packet stream), and SOCK_DGRAM (connectionless
 * datagram) types.
 */

#ifndef __KERNEL_VFS_UNIX_SOCKET_H
#define __KERNEL_VFS_UNIX_SOCKET_H

#include "types.h"
#include "lock/spinlock.h"
#include "proc/tq_type.h"
#include "param.h"

struct vfs_file;
struct vfs_file_ops;

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

#define AF_UNIX       1
#define AF_LOCAL      AF_UNIX
#define UNIX_PATH_MAX 108

/* Socket type constants (matching POSIX) — shared with sys_socket.c */
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
#define UNIX_BUF_DEFAULT_SIZE (64 * PAGE_SIZE)        /* initial ring size */
#define UNIX_BUF_MAX_SIZE     (16 * 1024 * 1024)      /* maximum growable ring */
#define UNIX_BUF_SIZE         UNIX_BUF_DEFAULT_SIZE   /* compatibility alias */
#define UNIX_LOWAT_DEFAULT    1
#define UNIX_IO_CHUNK         PAGE_SIZE

/* Maximum number of pending connections for a listening socket */
#define UNIX_BACKLOG_MAX 128

/* Socket states */
#define UNIX_STATE_UNCONNECTED  0
#define UNIX_STATE_BOUND        1
#define UNIX_STATE_LISTENING    2
#define UNIX_STATE_CONNECTED    3
#define UNIX_STATE_SHUTDOWN     4
#define UNIX_STATE_CONNECTING   5

/* Shutdown flags */
#define UNIX_SHUT_RD   1
#define UNIX_SHUT_WR   2

/* ========================================================================== */
/* sockaddr_un - UNIX domain socket address                                   */
/* ========================================================================== */

struct sockaddr_un {
    uint16 sun_family;              /* AF_UNIX */
    char   sun_path[UNIX_PATH_MAX]; /* pathname */
};

/* ========================================================================== */
/* UNIX domain socket structure                                               */
/* ========================================================================== */

/*
 * Ring buffer for one direction of data flow.
 * Uses the same nread/nwrite wrap-around scheme as kernel pipes.
 */
struct unix_ring {
    char  *data;            /* growable byte ring */
    size_t capacity;        /* allocated bytes in data */
    uint   nread;           /* bytes read (monotonic) */
    uint   nwrite;          /* bytes written (monotonic) */
};

/*
 * Accept queue entry - queued pending connections on a listening socket.
 */
struct unix_pending {
    struct unix_sock *sock;    /* the connecting socket */
    struct unix_pending *next; /* linked-list next pointer */
};

struct unix_scm_entry {
    struct vfs_file *file;
    uint start_nread;
    uint end_nread;
};

/*
 * struct unix_sock - per-socket state for an AF_UNIX socket
 */
struct unix_sock {
    spinlock_t lock;            /* protects most fields */
    int        refcount;        /* atomic reference count */
    int        state;           /* UNIX_STATE_* */
    int        type;            /* SOCK_STREAM, SOCK_SEQPACKET, or SOCK_DGRAM */
    int        protocol;        /* always 0 for AF_UNIX */
    int        shutdown_flags;  /* UNIX_SHUT_RD | UNIX_SHUT_WR */
    int        so_error;        /* pending async socket error for SO_ERROR */
    int        passcred;        /* SO_PASSCRED: attach SCM_CREDENTIALS */
    size_t     rcv_lowat;       /* SO_RCVLOWAT */
    size_t     snd_lowat;       /* SO_SNDLOWAT */
    size_t     rcv_buf;         /* SO_RCVBUF cap for peer writes */
    size_t     snd_buf;         /* SO_SNDBUF cap for our writes */

    /* Bind address */
    char       bind_path[UNIX_PATH_MAX];
    int        bound;           /* non-zero if bound */

    /* Connected peer (SOCK_STREAM) */
    struct unix_sock *peer;
    struct unix_sock *connect_target;

    /* Peer credentials (set during connect, read via SO_PEERCRED) */
    int  peer_pid;
    uint32 peer_uid;
    uint32 peer_gid;

    /* Bidirectional ring buffers for connected sockets.
     * tx = data written by this socket, read by peer
     * tx is allocated on the socket that "sends", and peer reads from it.
     * For simplicity: each connected socket has its own tx ring.
     * Reading from a socket reads from peer->tx ring.
     */
    struct unix_ring tx;

    /* Wait queues */
    tq_t  rd_queue;  /* readers sleep here waiting for data */
    tq_t  wr_queue;  /* writers sleep here waiting for space */
    tq_t  conn_queue;/* accept() sleeps here; connect() wakes it */

    /* Listening state */
    int    backlog;          /* max pending connections */
    int    pending_count;    /* current pending connections */
    struct unix_pending *pending_head;
    struct unix_pending *pending_tail;

    /* VFS file back-reference (for kqueue/epoll notification) */
    struct vfs_file *file;

    /*
     * SCM_RIGHTS entries are attached to the sender tx stream position.  The
     * receiver may read a Wayland message in pieces, so descriptors must not
     * be delivered before the byte stream has reached the message that carried
     * them.
     */
#define UNIX_SCM_QUEUE_MAX 256
    struct unix_scm_entry *scm_queue;
    int scm_head;  /* next slot to dequeue from */
    int scm_tail;  /* next slot to enqueue into */

/*
 * SOCK_SEQPACKET stores payload bytes in the growable tx ring, but it also
 * needs one boundary mark per queued packet.  WebKit IPC commonly transfers
 * multi-megabyte resources as many 2048-byte seqpacket messages; keep enough
 * marks to cover a full 16 MiB ring at that granularity without reporting
 * artificial EAGAIN while byte capacity remains.
 */
#define UNIX_PACKET_QUEUE_MAX 8192
    uint *packet_queue;
    int packet_head;  /* next packet end mark to consume */
    int packet_tail;  /* next packet end mark to enqueue */
};

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

void unix_socket_init(void);

/* Create an AF_UNIX socket, returns fd or -errno */
int  unix_sock_create(int type, int protocol, int file_flags);

/* Syscall-level operations on AF_UNIX sockets.
 * These are called from sys_bind/sys_listen/etc. after domain dispatch.
 * They take the raw syscall arguments (already fetched).
 */
int  unix_sock_bind(int fd, uint64 uaddr, int addrlen);
int  unix_sock_listen(int fd, int backlog);
int  unix_sock_accept(int fd, uint64 uaddr, uint64 uaddrlen, int flags);
int  unix_sock_connect(int fd, uint64 uaddr, int addrlen);
int  unix_sock_shutdown(int fd, int how);
int  unix_sock_getpeername(int fd, uint64 uaddr, uint64 uaddrlen);
int  unix_sock_getsockname(int fd, uint64 uaddr, uint64 uaddrlen);
int  unix_sock_socketpair(int type, int protocol, int file_flags, int sv[2]);
int  unix_sock_setsockopt(int fd, int level, int optname, uint64 optval,
                          int optlen);
int  unix_sock_getsockopt(int fd, int level, int optname, uint64 optval,
                          uint64 optlen_ptr);

/* Check if a vfs_file is an AF_UNIX socket */
int  unix_sock_is_unix(struct vfs_file *f);

/* Get unix_sock from fd (returns NULL if not a unix socket) */
struct unix_sock *unix_sock_from_fd(int fd);
void unix_sock_get_ref(struct unix_sock *sk);
void unix_sock_put_ref(struct unix_sock *sk);

/* Extern file_ops for AF_UNIX sockets */
extern struct vfs_file_ops unix_socket_file_ops;

#endif /* __KERNEL_VFS_UNIX_SOCKET_H */
