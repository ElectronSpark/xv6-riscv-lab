/*
 * telnetd — Kernel-resident telnet server
 *
 * Uses lwIP's netconn API for TCP and the kernel PTY subsystem to provide
 * remote shell access.  Each telnet session allocates a PTY pair, spawns
 * /bin/sh on the slave side, and bridges TCP ↔ PTY master with basic
 * RFC 854/855 telnet protocol handling.
 *
 * Architecture:
 *
 *   telnetd_listener (kthread)
 *     │  netconn_accept()
 *     ▼
 *   telnet_session (kthread, per-connection)
 *     ├── open /dev/ptmx → master vfs_file, /dev/pts/N
 *     ├── spawn_shell_on_pty(pty_idx)     [kthread → exec /bin/sh]
 *     ├── telnet_tx_thread (kthread)      [PTY master → TCP]
 *     └── (inline) TCP → PTY master loop
 *
 * Lock ordering:  session lock → tty lock → pipe lock
 *
 * QEMU network: 10.0.2.15:23, reachable from host as localhost:2323
 *   (QEMU SLIRP forwards host port to guest via -net user,hostfwd=...)
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "signal.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include <mm/vm.h>
#include <vfs/vfs_types.h>
#include <vfs/file.h>
#include <vfs/fcntl.h>
#include "tty/tty.h"
#include "tty/termios.h"
#include "tty/session.h"
#include "vfs/pipe.h"
#include "vfs/fs.h"
#include "vfs/stat.h"

#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/tcp.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Configuration                                                               */
/* ──────────────────────────────────────────────────────────────────────────── */

#define TELNETD_PORT      23
#define TELNETD_BACKLOG   4
#define TELNETD_MAX_SESS  8
#define PTY_BUF_SIZE      256
#define TELNET_BUF_SIZE   512

/* ──────────────────────────────────────────────────────────────────────────── */
/* Telnet protocol constants (RFC 854/855)                                     */
/* ──────────────────────────────────────────────────────────────────────────── */

#define IAC   255   /* Interpret As Command */
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250   /* Subnegotiation Begin */
#define SE    240   /* Subnegotiation End */

/* Telnet options */
#define TELOPT_ECHO    1
#define TELOPT_SGA     3   /* Suppress Go Ahead */
#define TELOPT_NAWS    31  /* Negotiate About Window Size */
#define TELOPT_LMODE   34  /* Linemode */

/* ──────────────────────────────────────────────────────────────────────────── */
/* Per-session state                                                           */
/* ──────────────────────────────────────────────────────────────────────────── */

struct telnet_session {
    struct netconn    *conn;        /* TCP connection (accepted socket) */
    struct tty        *slave_tty;   /* PTY slave (shell attached here) */
    struct vfs_file   *master_file; /* VFS file from /dev/ptmx open */
    volatile int       closing;     /* Shutdown flag */
    volatile int       tx_done;     /* TX thread has exited */
    int                shell_pid;   /* PID of the shell process */
    int                pty_idx;     /* PTY index (N in /dev/pts/N) */
    int                id;          /* Session index */
};

static int session_count;

/* ──────────────────────────────────────────────────────────────────────────── */
/* Kernel-internal VFS open helper                                             */
/*                                                                             */
/* Opens a path through the full VFS/cdev stack and installs the resulting     */
/* vfs_file into the current thread's fdtable (or just returns the file).      */
/* ──────────────────────────────────────────────────────────────────────────── */

static int kern_open(const char *path, int flags)
{
    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR_OR_NULL(inode))
        return IS_ERR(inode) ? (int)PTR_ERR(inode) : -ENOENT;

    struct vfs_file *f = vfs_fileopen(inode, flags);
    vfs_iput(inode);
    if (IS_ERR(f))
        return (int)PTR_ERR(f);

    spin_lock(&current->fdtable->lock);
    int fd = vfs_fdtable_alloc_fd(current->fdtable, f);
    spin_unlock(&current->fdtable->lock);
    vfs_fput(f); /* fdtable_alloc_fd added a ref; drop ours */
    return fd;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Telnet protocol helpers                                                     */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Send initial telnet option negotiations.
 * We tell the client that we will echo (so it shouldn't locally echo),
 * we will suppress go-ahead, and we'd like to know the window size.
 */
static void telnet_send_initial_negotiation(struct netconn *conn)
{
    static const unsigned char neg[] = {
        IAC, WILL, TELOPT_ECHO,      /* Server will echo */
        IAC, WILL, TELOPT_SGA,       /* Suppress Go Ahead */
        IAC, DO,   TELOPT_NAWS,      /* Request window size */
        IAC, DO,   TELOPT_SGA,       /* Ask client to SGA too */
        IAC, DONT, TELOPT_LMODE,     /* Disable linemode */
    };
    netconn_write(conn, neg, sizeof(neg), NETCONN_COPY);
}

/*
 * Process a telnet IAC command.  Returns the number of bytes consumed
 * from buf (including the IAC byte).  Handles WILL/WONT/DO/DONT and
 * subnegotiation (NAWS for window size).
 */
static int telnet_process_iac(struct telnet_session *sess,
                              const unsigned char *buf, int len)
{
    if (len < 2)
        return 0; /* Need more data */

    unsigned char cmd = buf[1];

    switch (cmd) {
    case IAC:
        /* Escaped 0xFF — pass through (handled by caller) */
        return 2;

    case WILL:
    case WONT:
    case DO:
    case DONT:
        if (len < 3)
            return 0; /* Need the option byte */
        /* Acknowledge everything passively */
        return 3;

    case SB:
        /* Subnegotiation — scan for IAC SE */
        for (int i = 2; i < len - 1; i++) {
            if (buf[i] == IAC && buf[i + 1] == SE) {
                /* Check for NAWS: SB NAWS <4 bytes> IAC SE */
                if (len >= 9 && buf[2] == TELOPT_NAWS) {
                    uint16 cols = ((uint16)buf[3] << 8) | buf[4];
                    uint16 rows = ((uint16)buf[5] << 8) | buf[6];
                    if (sess->slave_tty) {
                        struct winsize ws;
                        ws.ws_col = cols;
                        ws.ws_row = rows;
                        ws.ws_xpixel = 0;
                        ws.ws_ypixel = 0;
                        tty_ioctl(sess->slave_tty, TIOCSWINSZ, &ws);
                    }
                }
                return i + 2;
            }
        }
        return 0; /* Incomplete subnegotiation */

    default:
        return 2; /* Unknown command — skip IAC + cmd */
    }
}

/*
 * Strip telnet commands from raw TCP data and write the resulting
 * user data into the PTY master (which feeds the slave's line discipline).
 *
 * Returns the number of user-data bytes delivered, or < 0 on error.
 */
static ssize_t telnet_feed_pty(struct telnet_session *sess,
                               const unsigned char *data, int datalen)
{
    ssize_t total = 0;
    char outbuf[TELNET_BUF_SIZE];
    int outpos = 0;

    /* Flush helper — send accumulated bytes to PTY master in one call */
    #define FEED_FLUSH() do {                                          \
        if (outpos > 0) {                                              \
            pty_master_write(sess->slave_tty, outbuf, outpos, false);  \
            total += outpos;                                           \
            outpos = 0;                                                \
        }                                                              \
    } while (0)

    while (datalen > 0) {
        if (*data == IAC) {
            if (datalen >= 2 && data[1] == IAC) {
                /* Escaped IAC → literal 0xFF byte */
                outbuf[outpos++] = (char)0xff;
                if (outpos >= (int)sizeof(outbuf))
                    FEED_FLUSH();
                data += 2;
                datalen -= 2;
                continue;
            }
            /* Flush before processing IAC command (may change tty state) */
            FEED_FLUSH();
            int consumed = telnet_process_iac(sess, data, datalen);
            if (consumed == 0)
                break; /* Incomplete command at end of buffer */
            data += consumed;
            datalen -= consumed;
            continue;
        }

        /* Find the next IAC or end of data */
        int span = 0;
        while (span < datalen && data[span] != IAC)
            span++;

        if (span > 0) {
            /* Handle CR NUL → CR and CR LF → LF conversions */
            for (int i = 0; i < span; i++) {
                char ch = (char)data[i];
                if (ch == '\r') {
                    if (i + 1 < span && data[i + 1] == '\0') {
                        /* CR NUL → CR (keep \r) */
                        i++; /* skip the NUL */
                    } else if (i + 1 < span && data[i + 1] == '\n') {
                        /* CR LF → just \n for Unix */
                        ch = '\n';
                        i++; /* skip the LF — we send \n */
                    } else {
                        /* Bare CR → \n for Unix */
                        ch = '\n';
                    }
                }
                outbuf[outpos++] = ch;
                if (outpos >= (int)sizeof(outbuf))
                    FEED_FLUSH();
            }
            data += span;
            datalen -= span;
        }
    }

    FEED_FLUSH();
    #undef FEED_FLUSH

    return total;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Shell spawning                                                              */
/*                                                                             */
/* Creates a kthread that sets itself up as a user process with PTY slave      */
/* as stdin/stdout/stderr, then exec's /bin/sh.                                */
/* ──────────────────────────────────────────────────────────────────────────── */

static void telnet_shell_entry(uint64 arg1, uint64 arg2)
{
    int pty_idx = (int)arg1;
    struct tty *slave_tty = (struct tty *)arg2;

    /*
     * 1. Replace fdtable with a fresh empty one.
     */
    struct vfs_fdtable *old_fdt = current->fdtable;
    current->fdtable = vfs_fdtable_init();
    if (old_fdt)
        vfs_fdtable_put(old_fdt);

    /*
     * 2. Open /dev/pts/N three times via VFS for fd 0/1/2.
     *    This goes through cdev → pts_open_file → vfs_file_ops path.
     */
    char pts_path[32];
    {
        /* Build "/dev/pts/<idx>" manually (no snprintf in kernel) */
        const char *prefix = "/dev/pts/";
        int pi = 0;
        while (prefix[pi]) {
            pts_path[pi] = prefix[pi];
            pi++;
        }
        /* Convert pty_idx to string */
        char tmp[8];
        int ti = 0;
        int v = pty_idx;
        if (v == 0) {
            tmp[ti++] = '0';
        } else {
            while (v > 0) {
                tmp[ti++] = '0' + (v % 10);
                v /= 10;
            }
        }
        for (int i = ti - 1; i >= 0; i--)
            pts_path[pi++] = tmp[i];
        pts_path[pi] = '\0';
    }

    int fd0 = kern_open(pts_path, O_RDWR);
    int fd1 = kern_open(pts_path, O_RDWR);
    int fd2 = kern_open(pts_path, O_RDWR);

    if (fd0 < 0 || fd1 < 0 || fd2 < 0) {
        printf("telnetd-sh: FAILED to open %s fds: %d %d %d\n",
               pts_path, fd0, fd1, fd2);
        return;
    }

    /*
     * 3. Set up session and controlling terminal.
     */
    session_setsid();
    if (current->session && slave_tty)
        session_set_ctrl_tty(current->session, slave_tty);

    /*
     * 4. Set the foreground process group.
     */
    if (current->session)
        session_set_fg_pgid(current->session, current->pid);

    /*
     * 5. Allocate user-space VM and signal handling.
     */
    current->vm = vm_init();
    current->sigacts = sigacts_init();
    THREAD_SET_USER_SPACE(current);

    /*
     * 6. Exec the shell.
     */
    char *argv[] = {"sh", 0};
    int ret = exec("/bin/sh", argv, 0);
    if (ret < 0) {
        printf("telnetd: exec /bin/sh FAILED: %d\n", ret);
        return;
    }

    /* Jump to user space — never returns */
    smp_mb();
    usertrapret();
}

static struct thread *spawn_shell_on_pty(int pty_idx, struct tty *slave_tty)
{
    struct thread *t = kthread_create("telnet-sh",
                                      telnet_shell_entry,
                                      (uint64)pty_idx,
                                      (uint64)slave_tty,
                                      KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("telnetd: kthread_create for shell FAILED\n");
        return NULL;
    }
    wakeup(t);
    return t;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* PTY → TCP transmit thread                                                   */
/*                                                                             */
/* Reads output from the shell (via PTY master) and sends it over TCP          */
/* to the remote telnet client.  IAC (0xFF) bytes are escaped.                 */
/* ──────────────────────────────────────────────────────────────────────────── */

static void telnet_tx_thread(uint64 arg1, uint64 arg2)
{
    struct telnet_session *sess = (struct telnet_session *)arg1;
    (void)arg2;

    char rbuf[PTY_BUF_SIZE];
    unsigned char tbuf[PTY_BUF_SIZE * 2]; /* Worst case: every byte is IAC */

    while (!sess->closing) {
        ssize_t n = pty_master_read(sess->slave_tty, rbuf, sizeof(rbuf),
                                    false);
        if (n <= 0) {
            /* Slave closed or error — shell has exited */
            sess->closing = 1;
            break;
        }

        /* Re-check closing flag after the (possibly long) blocking read
         * to avoid calling netconn_write on a connection that the RX
         * thread is about to tear down. */
        if (sess->closing)
            break;

        /* Escape IAC bytes and convert bare LF → CR LF for telnet */
        int tlen = 0;
        for (int i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)rbuf[i];
            if (ch == IAC) {
                tbuf[tlen++] = IAC;
                tbuf[tlen++] = IAC;
            } else if (ch == '\n') {
                tbuf[tlen++] = '\r';
                tbuf[tlen++] = '\n';
            } else {
                tbuf[tlen++] = ch;
            }
        }

        err_t err = netconn_write(sess->conn, tbuf, (size_t)tlen,
                                  NETCONN_COPY);
        if (err != ERR_OK) {
            sess->closing = 1;
            break;
        }
    }

    /*
     * The TX thread is the ONLY thread that calls netconn_write, so it
     * must also be the one to call netconn_close.  The lwIP netconn API
     * is NOT safe for concurrent use from multiple threads on the same
     * connection — both netconn_write() and netconn_close() use
     * conn->current_msg and conn->op_completed, and calling them from
     * different threads corrupts lwIP's internal message pool.
     *
     * After this, the RX thread (which is waiting on tx_done) will call
     * netconn_delete() to free the connection resources.
     */
    netconn_close(sess->conn);

    /* Release the per-thread lwIP semaphore so the slot can be reused. */
    LWIP_NETCONN_THREAD_SEM_FREE();

    smp_store_release(&sess->tx_done, 1);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Per-connection session handler                                              */
/*                                                                             */
/* Runs in its own kthread.  Allocates PTY, spawns shell, bridges              */
/* TCP (rx) → PTY master.  The reverse direction (PTY → TCP) runs in           */
/* a separate kthread (telnet_tx_thread).                                      */
/* ──────────────────────────────────────────────────────────────────────────── */

static void telnet_session_handler(uint64 arg1, uint64 arg2)
{
    struct netconn *conn = (struct netconn *)arg1;
    (void)arg2;

    int sess_id = __atomic_fetch_add(&session_count, 1, __ATOMIC_RELAXED);

    /*
     * Allocate PTY pair via /dev/ptmx device wrapper.
     * This goes through the full VFS → cdev → ptmx_open_file path,
     * which registers /dev/pts/N and creates the slave cdev.
     */
    struct vfs_inode *ptmx_inode = vfs_namei("/dev/ptmx", 9);
    if (IS_ERR_OR_NULL(ptmx_inode)) {
        printf("telnetd: vfs_namei /dev/ptmx failed\n");
        netconn_close(conn);
        netconn_delete(conn);
        return;
    }

    struct vfs_file *master_file = vfs_fileopen(ptmx_inode, O_RDWR);
    vfs_iput(ptmx_inode);
    if (IS_ERR(master_file)) {
        printf("telnetd: vfs_fileopen /dev/ptmx failed: %ld\n",
               PTR_ERR(master_file));
        netconn_close(conn);
        netconn_delete(conn);
        return;
    }

    /* Extract the pty_pair and slave tty from the master file */
    struct tty *slave_tty = NULL;
    int pty_idx = -1;
    {
        /* master_file->private_data is a struct pty_pair* (set by ptmx_open_file) */
        /* We need the pty index for /dev/pts/N and the slave tty for I/O */
        int idx_val = 0;
        if (master_file->ops && master_file->ops->ioctl) {
            int ret = master_file->ops->ioctl(master_file, TIOCGPTN, &idx_val);
            if (ret == 0)
                pty_idx = idx_val;
        }

        /* Get slave_tty: the private_data is pty_pair, and pty_pair->slave
         * is the tty. We access it via pty_master_read/write which take
         * slave_tty. We need a way to get it. Use the ptmx_fops_read
         * path: file->private_data is struct pty_pair*, ->slave is the tty.
         * Since struct pty_pair is defined in ptmx.c, we cast carefully. */
        struct {
            struct tty *slave;
        } *pair_hdr = master_file->private_data;
        if (pair_hdr)
            slave_tty = pair_hdr->slave;
    }

    if (slave_tty == NULL || pty_idx < 0) {
        printf("telnetd: failed to get PTY pair from /dev/ptmx (idx=%d)\n",
               pty_idx);
        vfs_fput(master_file);
        netconn_close(conn);
        netconn_delete(conn);
        return;
    }

    printf("telnetd: opened /dev/ptmx → /dev/pts/%d for session %d\n",
           pty_idx, sess_id);

    /* Set PTY slave termios to reasonable defaults for telnet */
    struct termios *tp = &slave_tty->termios;
    tp->c_iflag = ICRNL;                     /* Map CR to NL on input */
    tp->c_oflag = OPOST | ONLCR;            /* Map NL to CR-NL on output */
    tp->c_cflag = CS8 | CREAD;              /* 8-bit, enable receiver */
    tp->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;

    /* Set default window size */
    slave_tty->winsize.ws_col = 80;
    slave_tty->winsize.ws_row = 24;

    /*
     * Heap-allocate session context so the TX kthread can safely
     * reference it after this function's stack frame is gone.
     */
    struct telnet_session *sess = (struct telnet_session *)kalloc();
    if (sess == NULL) {
        printf("telnetd: kalloc failed for session\n");
        vfs_fput(master_file); /* triggers ptmx_fops_release → cleanup */
        netconn_close(conn);
        netconn_delete(conn);
        return;
    }
    memset(sess, 0, sizeof(*sess));
    sess->conn        = conn;
    sess->slave_tty   = slave_tty;
    sess->master_file = master_file;
    sess->closing     = 0;
    sess->tx_done     = 0;
    sess->shell_pid   = -1;
    sess->pty_idx     = pty_idx;
    sess->id          = -1; /* updated to shell PID after spawn */

    /* Send telnet negotiations */
    telnet_send_initial_negotiation(conn);

    /* Send a login banner */
    static const char banner[] =
        "\r\nxv6 telnet server\r\n\r\n";
    netconn_write(conn, banner, sizeof(banner) - 1, NETCONN_COPY);

    /* Spawn the shell on the slave side of the PTY */
    struct thread *shell_thread = spawn_shell_on_pty(pty_idx, slave_tty);
    if (shell_thread == NULL) {
        kfree(sess);
        vfs_fput(master_file);
        netconn_close(conn);
        netconn_delete(conn);
        return;
    }
    sess->shell_pid = shell_thread->pid;
    sess->id        = shell_thread->pid; /* shell PID = kernel session ID */

    printf("telnetd: session %d started (pts/%d)\n", sess->id, pty_idx);

    /* Start the TX bridge (PTY → TCP) in a separate kthread */
    struct thread *tx = kthread_create("telnet-tx",
                                       telnet_tx_thread,
                                       (uint64)sess, 0,
                                       KERNEL_STACK_ORDER);
    if (!IS_ERR_OR_NULL(tx))
        wakeup(tx);

    /*
     * Main loop: TCP → PTY (RX direction)
     *
     * Read TCP data, strip telnet commands, feed user data to PTY master.
     */
    while (!sess->closing) {
        struct pbuf *p = NULL;
        err_t err = netconn_recv_tcp_pbuf(conn, &p);
        if (err != ERR_OK || p == NULL) {
            sess->closing = 1;
            break;
        }

        /* Extract data from pbuf chain */
        unsigned char buf[TELNET_BUF_SIZE];
        uint16 plen = p->tot_len;
        if (plen > sizeof(buf))
            plen = sizeof(buf);
        pbuf_copy_partial(p, buf, plen, 0);
        pbuf_free(p);

        telnet_feed_pty(sess, buf, (int)plen);
    }

    /*
     * Cleanup:
     * 1. Set closing flag
     * 2. Close the write end of the output pipe to unblock the TX
     *    thread (which is blocked in pipe_read on the output pipe).
     * 3. Wait for the TX thread to actually exit.
     * 4. Close the TCP connection.
     * 5. Hang up the PTY (sends SIGHUP to the shell's process group).
     * 6. Release the TTY and free session.
     */
    sess->closing = 1;

    /*
     * Kill the shell — this makes it exit so its fds get released and
     * the TTY ref count drops.  Send SIGHUP first (polite), then
     * SIGKILL to guarantee termination.
     */
    if (sess->shell_pid > 0) {
        kill(sess->shell_pid, SIGHUP);
        sleep_ms(50);
        kill(sess->shell_pid, SIGKILL);
    }

    /* Unblock the TX thread: closing the write end of output_pipe
     * causes pipe_read to return 0 (EOF) because PIPE_WRITABLE becomes
     * false and there is no remaining data.  This unblocks the TX thread
     * if it is currently inside pty_master_read(). */
    if (slave_tty->output_pipe)
        pipe_close(slave_tty->output_pipe, 1);

    /* Also close the write end of the input pipe so any canonical-mode
     * reader gets EOF. */
    if (slave_tty->input_pipe)
        pipe_close(slave_tty->input_pipe, 1);

    /*
     * Wait for the TX thread to exit.
     *
     * The TX thread handles netconn_close() itself — see the comment
     * at the end of telnet_tx_thread().  We do NOT call any netconn
     * functions here because the lwIP netconn API is NOT thread-safe
     * for the same connection.  Calling netconn_close() from this
     * thread while the TX thread is inside netconn_write() would
     * corrupt conn->current_msg / conn->op_completed and crash the
     * tcpip thread.
     *
     * The TX thread will be unblocked by one of:
     *   a) pipe_close above → pty_master_read returns EOF
     *   b) Remote disconnect → TCP RST/FIN processed by lwIP →
     *      err_tcp callback signals conn->op_completed →
     *      netconn_write returns ERR_RST/ERR_CLSD
     */
    for (int i = 0; i < 500 && !smp_load_acquire(&sess->tx_done); i++)
        sleep_ms(10);

    if (!smp_load_acquire(&sess->tx_done))
        printf("telnetd: WARNING: TX thread did not exit in time\n");

    /* TX thread has called netconn_close — safe to free the conn */
    netconn_delete(conn);

    /* Release the per-thread lwIP semaphore so the slot can be reused. */
    LWIP_NETCONN_THREAD_SEM_FREE();

    tty_hangup(slave_tty);

    /* Give the shell a moment to fully exit and release its fds */
    sleep_ms(100);

    /*
     * Release the master VFS file.  This triggers ptmx_fops_release()
     * which removes /dev/pts/N from devtmpfs, unregisters the slave
     * cdev, and (if no user fds remain) frees the pty_pair + tty.
     *
     * We must NOT call pty_dealloc / tty_unref manually — the device
     * wrapper owns the PTY lifecycle now.
     */
    vfs_fput(sess->master_file);

    printf("telnetd: session %d ended\n", sess->id);
    kfree(sess);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Listener thread                                                             */
/*                                                                             */
/* Binds to port 23 and accepts incoming TCP connections, spawning a           */
/* session handler kthread for each one.                                       */
/* ──────────────────────────────────────────────────────────────────────────── */

static void telnetd_listener(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;

    /* Create TCP listening socket */
    struct netconn *listener = netconn_new(NETCONN_TCP);
    if (listener == NULL) {
        printf("telnetd: netconn_new failed\n");
        return;
    }

    /* Allow address reuse */
#if SO_REUSE
    ip_set_option(listener->pcb.ip, SOF_REUSEADDR);
#endif

    err_t err = netconn_bind(listener, IP_ADDR_ANY, TELNETD_PORT);
    if (err != ERR_OK) {
        printf("telnetd: bind to port %d failed: %d\n", TELNETD_PORT, err);
        netconn_delete(listener);
        return;
    }

    err = netconn_listen_with_backlog(listener, TELNETD_BACKLOG);
    if (err != ERR_OK) {
        printf("telnetd: listen failed: %d\n", err);
        netconn_delete(listener);
        return;
    }

    printf("telnetd: listening on port %d\n", TELNETD_PORT);

    for (;;) {
        struct netconn *client = NULL;
        err = netconn_accept(listener, &client);
        if (err != ERR_OK) {
            if (err == ERR_ABRT || err == ERR_CLSD)
                break; /* Listener closed */
            sleep_ms(100);
            continue;
        }

        /* Disable Nagle for interactive echo */
        tcp_nagle_disable(client->pcb.tcp);

        ip_addr_t raddr;
        u16_t rport;
        netconn_getaddr(client, &raddr, &rport, 0);
        printf("telnetd: connection from %d.%d.%d.%d:%d\n",
               ip4_addr1_16(ip_2_ip4(&raddr)),
               ip4_addr2_16(ip_2_ip4(&raddr)),
               ip4_addr3_16(ip_2_ip4(&raddr)),
               ip4_addr4_16(ip_2_ip4(&raddr)),
               rport);

        /* Spawn a session handler kthread */
        struct thread *t = kthread_create("telnet-sess",
                                          telnet_session_handler,
                                          (uint64)client, 0,
                                          KERNEL_STACK_ORDER);
        if (IS_ERR_OR_NULL(t)) {
            printf("telnetd: failed to create session thread\n");
            netconn_close(client);
            netconn_delete(client);
        } else {
            wakeup(t);
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Module init                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

void telnetd_init(void)
{
    struct thread *t = kthread_create("telnetd", telnetd_listener, 0, 0,
                                      KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("telnetd: failed to create listener thread\n");
        return;
    }
    wakeup(t);
    printf("telnetd: daemon started\n");
}
