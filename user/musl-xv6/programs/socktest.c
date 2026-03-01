/*
 * socktest — validate socket option, ioctl, and recv-flag support on xv6
 *
 * Exercises the following kernel features:
 *   - setsockopt / getsockopt: SO_REUSEADDR, SO_KEEPALIVE, SO_BROADCAST,
 *     SO_LINGER, SO_RCVBUF, SO_SNDBUF, SO_TYPE, SO_ERROR, SO_ACCEPTCONN
 *   - TCP-level: TCP_NODELAY, TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
 *   - IP-level:  IP_TTL, IP_TOS
 *   - ioctl:     FIONBIO, FIONREAD
 *   - MSG_PEEK   (UDP loopback peek + consume)
 *   - AF_UNIX:   socketpair, bind/listen/accept/connect, shutdown, getsockname
 *   - AF_NETLINK: RTM_GETLINK, RTM_GETADDR, RTM_GETROUTE dump queries
 *
 * Usage: socktest
 */

#define _GNU_SOURCE  /* struct ip_mreq, sendmmsg, SOL_TCP, etc. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/wait.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static int fails   = 0;
static int passes  = 0;

#define PASS(msg) do { printf("  PASS: %s\n", (msg)); passes++; } while (0)
#define FAIL(msg) do { printf("  FAIL: %s (errno=%d)\n", (msg), errno); fails++; } while (0)

/*
 * Discover local interface IP (QEMU SLIRP: 10.0.2.15).
 * Falls back to 127.0.0.1 when loopback is available.
 */
static in_addr_t get_local_ip(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return htonl(INADDR_LOOPBACK);

    struct sockaddr_in gw;
    memset(&gw, 0, sizeof(gw));
    gw.sin_family      = AF_INET;
    gw.sin_port         = htons(53);
    gw.sin_addr.s_addr  = htonl(0x0a000202); /* 10.0.2.2 */

    if (connect(s, (struct sockaddr *)&gw, sizeof(gw)) < 0) {
        close(s);
        return htonl(INADDR_LOOPBACK);
    }

    struct sockaddr_in me;
    socklen_t len = sizeof(me);
    if (getsockname(s, (struct sockaddr *)&me, &len) < 0) {
        close(s);
        return htonl(INADDR_LOOPBACK);
    }
    close(s);
    return me.sin_addr.s_addr;
}

/* ── Test: SOL_SOCKET get/set options on a UDP socket ───────────────── */

static void test_sol_socket_udp(void)
{
    printf("\n=== SOL_SOCKET options (UDP) ===\n");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { FAIL("socket(DGRAM)"); return; }

    int val;
    socklen_t olen;

    /* SO_TYPE */
    olen = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &val, &olen) == 0 && val == SOCK_DGRAM)
        PASS("SO_TYPE == SOCK_DGRAM");
    else
        FAIL("SO_TYPE");

    /* SO_ERROR (should be 0 on a fresh socket) */
    olen = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &val, &olen) == 0 && val == 0)
        PASS("SO_ERROR == 0");
    else
        FAIL("SO_ERROR");

    /* SO_BROADCAST: set then get */
    val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, SOL_SOCKET, SO_BROADCAST, &val, &olen);
        if (val) PASS("SO_BROADCAST set+get");
        else     FAIL("SO_BROADCAST readback");
    } else {
        FAIL("SO_BROADCAST setsockopt");
    }

    /* SO_REUSEADDR */
    val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, &olen);
        if (val) PASS("SO_REUSEADDR set+get");
        else     FAIL("SO_REUSEADDR readback");
    } else {
        FAIL("SO_REUSEADDR setsockopt");
    }

    /* SO_RCVBUF set then get */
    val = 8192;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, &olen);
        if (val > 0) PASS("SO_RCVBUF set+get");
        else         FAIL("SO_RCVBUF readback");
    } else {
        FAIL("SO_RCVBUF setsockopt");
    }

    /* SO_SNDBUF get (read-only in our impl) */
    olen = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, &olen) == 0 && val > 0)
        PASS("SO_SNDBUF get");
    else
        FAIL("SO_SNDBUF get");

    /* SO_LINGER set then get */
    struct linger lg = { .l_onoff = 1, .l_linger = 5 };
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0) {
        struct linger lg2 = {0};
        olen = sizeof(lg2);
        getsockopt(fd, SOL_SOCKET, SO_LINGER, &lg2, &olen);
        if (lg2.l_onoff && lg2.l_linger == 5) PASS("SO_LINGER set+get");
        else FAIL("SO_LINGER readback");
    } else {
        FAIL("SO_LINGER setsockopt");
    }

    /* SO_ACCEPTCONN (should be 0 — not listening) */
    olen = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &val, &olen) == 0 && val == 0)
        PASS("SO_ACCEPTCONN == 0 (not listening)");
    else
        FAIL("SO_ACCEPTCONN");

    close(fd);
}

/* ── Test: IPPROTO_IP options ───────────────────────────────────────── */

static void test_ip_options(void)
{
    printf("\n=== IPPROTO_IP options ===\n");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { FAIL("socket(DGRAM) for IP opts"); return; }

    int val;
    socklen_t olen;

    /* IP_TTL set then get */
    val = 128;
    if (setsockopt(fd, IPPROTO_IP, IP_TTL, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, IPPROTO_IP, IP_TTL, &val, &olen);
        if (val == 128) PASS("IP_TTL set+get 128");
        else            FAIL("IP_TTL readback");
    } else {
        FAIL("IP_TTL setsockopt");
    }

    /* IP_TOS set then get */
    val = 0x10; /* IPTOS_LOWDELAY */
    if (setsockopt(fd, IPPROTO_IP, IP_TOS, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, IPPROTO_IP, IP_TOS, &val, &olen);
        if (val == 0x10) PASS("IP_TOS set+get 0x10");
        else             FAIL("IP_TOS readback");
    } else {
        FAIL("IP_TOS setsockopt");
    }

    close(fd);
}

/* ── Test: IPPROTO_TCP options on a TCP socket ──────────────────────── */

static void test_tcp_options(void)
{
    printf("\n=== IPPROTO_TCP options ===\n");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { FAIL("socket(STREAM) for TCP opts"); return; }

    int val;
    socklen_t olen;

    /* TCP_NODELAY */
    val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, &olen);
        if (val) PASS("TCP_NODELAY set+get");
        else     FAIL("TCP_NODELAY readback");
    } else {
        FAIL("TCP_NODELAY setsockopt");
    }

    /* SO_KEEPALIVE (on TCP socket — only TCP PCBs track this) */
    val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, &olen);
        if (val) PASS("SO_KEEPALIVE set+get (TCP)");
        else     FAIL("SO_KEEPALIVE readback");
    } else {
        FAIL("SO_KEEPALIVE setsockopt");
    }

    /* TCP_KEEPIDLE */
    val = 60;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, &olen);
        if (val == 60) PASS("TCP_KEEPIDLE set+get 60");
        else           FAIL("TCP_KEEPIDLE readback");
    } else {
        FAIL("TCP_KEEPIDLE setsockopt");
    }

    /* TCP_KEEPINTVL */
    val = 10;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, &olen);
        if (val == 10) PASS("TCP_KEEPINTVL set+get 10");
        else           FAIL("TCP_KEEPINTVL readback");
    } else {
        FAIL("TCP_KEEPINTVL setsockopt");
    }

    /* TCP_KEEPCNT */
    val = 5;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) == 0) {
        olen = sizeof(val); val = 0;
        getsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, &olen);
        if (val == 5) PASS("TCP_KEEPCNT set+get 5");
        else          FAIL("TCP_KEEPCNT readback");
    } else {
        FAIL("TCP_KEEPCNT setsockopt");
    }

    /* SO_TYPE on a STREAM socket */
    olen = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &val, &olen) == 0 && val == SOCK_STREAM)
        PASS("SO_TYPE == SOCK_STREAM");
    else
        FAIL("SO_TYPE on TCP");

    close(fd);
}

/* ── Test: ioctl FIONBIO / FIONREAD ─────────────────────────────────── */

static void test_ioctl(void)
{
    printf("\n=== ioctl (FIONBIO / FIONREAD) ===\n");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { FAIL("socket for ioctl"); return; }

    /* FIONBIO: set nonblock */
    int on = 1;
    if (ioctl(fd, FIONBIO, &on) == 0) {
        /* After FIONBIO, a recv should return EAGAIN immediately */
        char tmp[1];
        ssize_t n = recv(fd, tmp, 1, 0);
        if (n < 0 && errno == EAGAIN)
            PASS("FIONBIO: recv returns EAGAIN");
        else
            FAIL("FIONBIO: recv did not return EAGAIN");
    } else {
        FAIL("ioctl FIONBIO");
    }

    /* Bind to INADDR_ANY, send a packet to 127.0.0.1, then FIONREAD */
    struct sockaddr_in baddr, daddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sin_family      = AF_INET;
    baddr.sin_port        = htons(9871);
    baddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) {
        baddr.sin_port = htons(9872);
        if (bind(fd, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) {
            FAIL("bind for FIONREAD test");
            close(fd);
            return;
        }
    }

    /* Send to 127.0.0.1 on the same port we bound */
    memset(&daddr, 0, sizeof(daddr));
    daddr.sin_family      = AF_INET;
    daddr.sin_port        = baddr.sin_port;
    daddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const char *msg = "hello";
    ssize_t sent = sendto(fd, msg, 5, 0, (struct sockaddr *)&daddr, sizeof(daddr));
    if (sent == 5) {
        /* Wait for lwIP loopback delivery (may need multiple passes) */
        int avail = 0;
        int retries;
        for (retries = 0; retries < 20; retries++) {
            usleep(50000);
            if (ioctl(fd, FIONREAD, &avail) == 0 && avail > 0)
                break;
        }
        if (avail > 0)
            PASS("FIONREAD > 0 after sendto");
        else
            FAIL("FIONREAD after sendto");
    } else {
        FAIL("sendto for FIONREAD (loopback may not work)");
    }

    close(fd);
}

/* ── Test: MSG_PEEK (UDP) ───────────────────────────────────────────── */

static void test_msg_peek(void)
{
    printf("\n=== MSG_PEEK (UDP) ===\n");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { FAIL("socket for MSG_PEEK"); return; }

    /* Set nonblock so we don't hang */
    int on = 1;
    ioctl(fd, FIONBIO, &on);

    struct sockaddr_in baddr, daddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sin_family      = AF_INET;
    baddr.sin_port        = htons(9873);
    baddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) {
        baddr.sin_port = htons(9874);
        if (bind(fd, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) {
            FAIL("bind for MSG_PEEK");
            close(fd);
            return;
        }
    }

    /* Send to 127.0.0.1 on the same port we bound */
    memset(&daddr, 0, sizeof(daddr));
    daddr.sin_family      = AF_INET;
    daddr.sin_port        = baddr.sin_port;
    daddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const char *msg = "PEEK";
    sendto(fd, msg, 4, 0, (struct sockaddr *)&daddr, sizeof(daddr));

    /* Wait for lwIP loopback delivery */
    int retries;
    for (retries = 0; retries < 20; retries++) {
        usleep(50000);
        int avail = 0;
        ioctl(fd, FIONREAD, &avail);
        if (avail > 0)
            break;
    }

    /* Peek — should see data but not consume */
    char buf[16];
    ssize_t n1 = recvfrom(fd, buf, sizeof(buf), MSG_PEEK, NULL, NULL);
    if (n1 == 4 && memcmp(buf, "PEEK", 4) == 0) {
        PASS("MSG_PEEK returned data");

        /* Normal recv — should still get the same data */
        memset(buf, 0, sizeof(buf));
        ssize_t n2 = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n2 == 4 && memcmp(buf, "PEEK", 4) == 0)
            PASS("Normal recv after peek got same data");
        else
            FAIL("Normal recv after peek");

        /* Another recv should return EAGAIN (queue empty) */
        ssize_t n3 = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n3 < 0 && errno == EAGAIN)
            PASS("Queue empty after consume");
        else
            FAIL("Queue not empty after consume");
    } else {
        if (n1 < 0)
            FAIL("MSG_PEEK recvfrom (loopback may not work)");
        else
            FAIL("MSG_PEEK data mismatch");
    }

    close(fd);
}

/* ── Test: AF_UNIX socketpair (read/write) ───────────────────────────── */

static void test_unix_socketpair(void)
{
    printf("\n=== AF_UNIX socketpair ===\n");

    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (ret < 0) {
        FAIL("socketpair(AF_UNIX, SOCK_STREAM)");
        return;
    }
    PASS("socketpair created fd pair");

    /* Write from sv[0], read from sv[1] */
    const char *msg = "hello unix";
    ssize_t nw = write(sv[0], msg, 10);
    if (nw == 10)
        PASS("socketpair write 10 bytes");
    else
        FAIL("socketpair write");

    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(sv[1], buf, sizeof(buf));
    if (nr == 10 && memcmp(buf, "hello unix", 10) == 0)
        PASS("socketpair read matches written data");
    else
        FAIL("socketpair read");

    /* Write from sv[1], read from sv[0] (reverse direction) */
    const char *msg2 = "reply";
    nw = write(sv[1], msg2, 5);
    if (nw == 5) {
        memset(buf, 0, sizeof(buf));
        nr = read(sv[0], buf, sizeof(buf));
        if (nr == 5 && memcmp(buf, "reply", 5) == 0)
            PASS("socketpair bidirectional I/O");
        else
            FAIL("socketpair reverse read");
    } else {
        FAIL("socketpair reverse write");
    }

    /* Close one end, other should get EOF (0 bytes) on read */
    close(sv[0]);
    nr = read(sv[1], buf, sizeof(buf));
    if (nr == 0)
        PASS("socketpair EOF after peer close");
    else
        FAIL("socketpair expected EOF");

    close(sv[1]);
}

/* ── Test: AF_UNIX socketpair with SOCK_DGRAM ───────────────────────── */

static void test_unix_socketpair_dgram(void)
{
    printf("\n=== AF_UNIX socketpair (SOCK_DGRAM) ===\n");

    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    if (ret < 0) {
        FAIL("socketpair(AF_UNIX, SOCK_DGRAM)");
        return;
    }
    PASS("socketpair SOCK_DGRAM created");

    const char *msg = "dgram-test";
    ssize_t nw = write(sv[0], msg, 10);
    if (nw == 10) {
        char buf[32];
        memset(buf, 0, sizeof(buf));
        ssize_t nr = read(sv[1], buf, sizeof(buf));
        if (nr == 10 && memcmp(buf, "dgram-test", 10) == 0)
            PASS("SOCK_DGRAM socketpair I/O");
        else
            FAIL("SOCK_DGRAM socketpair read");
    } else {
        FAIL("SOCK_DGRAM socketpair write");
    }

    close(sv[0]);
    close(sv[1]);
}

/* ── Test: AF_UNIX bind + listen + connect + accept ─────────────────── */

static void test_unix_connect(void)
{
    printf("\n=== AF_UNIX bind/listen/accept/connect ===\n");

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { FAIL("socket(AF_UNIX, SOCK_STREAM) server"); return; }

    /* Bind to an abstract-ish path */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/socktest.%d", getpid());

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        FAIL("bind(AF_UNIX)");
        close(sfd);
        return;
    }
    PASS("AF_UNIX bind");

    if (listen(sfd, 5) < 0) {
        FAIL("listen(AF_UNIX)");
        close(sfd);
        return;
    }
    PASS("AF_UNIX listen");

    /* getsockname on the server socket */
    struct sockaddr_un name;
    socklen_t nlen = sizeof(name);
    memset(&name, 0, sizeof(name));
    if (getsockname(sfd, (struct sockaddr *)&name, &nlen) == 0 &&
        name.sun_family == AF_UNIX &&
        strcmp(name.sun_path, addr.sun_path) == 0)
        PASS("AF_UNIX getsockname on server");
    else
        FAIL("AF_UNIX getsockname");

    /* Fork: child connects, parent accepts */
    int pid = fork();
    if (pid < 0) {
        FAIL("fork for AF_UNIX connect test");
        close(sfd);
        return;
    }

    if (pid == 0) {
        /* ── child: connect, write, read, exit ── */
        close(sfd);  /* child doesn't need the listening socket */
        int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cfd < 0) _exit(1);

        if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
            _exit(2);

        const char *cmsg = "from-child";
        write(cfd, cmsg, 10);

        char cbuf[32];
        ssize_t cn = read(cfd, cbuf, sizeof(cbuf));
        if (cn == 11 && memcmp(cbuf, "from-parent", 11) == 0)
            _exit(0);
        else
            _exit(3);
    }

    /* ── parent: accept, read, write ── */
    int afd = accept(sfd, NULL, NULL);
    if (afd < 0) {
        FAIL("accept(AF_UNIX)");
        close(sfd);
        return;
    }
    PASS("AF_UNIX accept");

    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(afd, buf, sizeof(buf));
    if (nr == 10 && memcmp(buf, "from-child", 10) == 0)
        PASS("AF_UNIX read from accepted connection");
    else
        FAIL("AF_UNIX read from accepted connection");

    const char *pmsg = "from-parent";
    ssize_t nw = write(afd, pmsg, 11);
    if (nw == 11)
        PASS("AF_UNIX write to accepted connection");
    else
        FAIL("AF_UNIX write to accepted connection");

    /* Wait for child */
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        PASS("AF_UNIX child connect+I/O succeeded");
    else
        FAIL("AF_UNIX child exit status");

    close(afd);
    close(sfd);
}

/* ── Test: AF_UNIX shutdown ─────────────────────────────────────────── */

static void test_unix_shutdown(void)
{
    printf("\n=== AF_UNIX shutdown ===\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        FAIL("socketpair for shutdown test");
        return;
    }

    /* Shutdown write on sv[0] */
    if (shutdown(sv[0], SHUT_WR) == 0)
        PASS("shutdown(SHUT_WR)");
    else
        FAIL("shutdown(SHUT_WR)");

    /* Read from sv[1] should get EOF */
    char buf[16];
    ssize_t nr = read(sv[1], buf, sizeof(buf));
    if (nr == 0)
        PASS("read returns EOF after peer SHUT_WR");
    else
        FAIL("read after peer SHUT_WR");

    /* Writing from sv[1] to sv[0] should still work (only WR side of sv[0] was shut) */
    ssize_t nw = write(sv[1], "ok", 2);
    if (nw == 2) {
        memset(buf, 0, sizeof(buf));
        nr = read(sv[0], buf, sizeof(buf));
        if (nr == 2 && memcmp(buf, "ok", 2) == 0)
            PASS("reverse direction still works after one-side SHUT_WR");
        else
            FAIL("reverse direction read after SHUT_WR");
    } else {
        FAIL("reverse direction write after SHUT_WR");
    }

    close(sv[0]);
    close(sv[1]);
}

/* ── Test: AF_UNIX getpeername ──────────────────────────────────────── */

static void test_unix_getpeername(void)
{
    printf("\n=== AF_UNIX getpeername ===\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        FAIL("socketpair for getpeername test");
        return;
    }

    struct sockaddr_un peer;
    socklen_t plen = sizeof(peer);
    memset(&peer, 0, sizeof(peer));

    if (getpeername(sv[0], (struct sockaddr *)&peer, &plen) == 0 &&
        peer.sun_family == AF_UNIX)
        PASS("getpeername on socketpair returns AF_UNIX");
    else
        FAIL("getpeername on socketpair");

    close(sv[0]);
    close(sv[1]);
}

/* ========================================================================== */
/* AF_NETLINK tests                                                           */
/* ========================================================================== */

/* Netlink structures - musl doesn't ship linux/netlink.h */
struct nl_msghdr {
    unsigned int   nlmsg_len;
    unsigned short nlmsg_type;
    unsigned short nlmsg_flags;
    unsigned int   nlmsg_seq;
    unsigned int   nlmsg_pid;
};

struct nl_sockaddr {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int   nl_pid;
    unsigned int   nl_groups;
};

/* rtnetlink */
struct nl_ifinfomsg {
    unsigned char  ifi_family;
    unsigned char  __pad;
    unsigned short ifi_type;
    int            ifi_index;
    unsigned int   ifi_flags;
    unsigned int   ifi_change;
};

struct nl_ifaddrmsg {
    unsigned char ifa_family;
    unsigned char ifa_prefixlen;
    unsigned char ifa_flags;
    unsigned char ifa_scope;
    unsigned int  ifa_index;
};

struct nl_rtmsg {
    unsigned char rtm_family;
    unsigned char rtm_dst_len;
    unsigned char rtm_src_len;
    unsigned char rtm_tos;
    unsigned char rtm_table;
    unsigned char rtm_protocol;
    unsigned char rtm_scope;
    unsigned char rtm_type;
    unsigned int  rtm_flags;
};

struct nl_rtattr {
    unsigned short rta_len;
    unsigned short rta_type;
};

#define NL_AF_NETLINK    16
#define NL_NETLINK_ROUTE 0
#define NL_RTM_GETLINK   18
#define NL_RTM_GETADDR   22
#define NL_RTM_GETROUTE  26
#define NL_NLM_F_REQUEST 0x0001
#define NL_NLM_F_DUMP    0x0300
#define NL_NLMSG_DONE    3
#define NL_NLMSG_ERROR   2
#define NL_NLMSG_HDRLEN  ((int)sizeof(struct nl_msghdr))
#define NL_NLMSG_ALIGN(l) (((l)+3)&~3)
#define NL_IFLA_IFNAME   3
#define NL_IFLA_MTU      4
#define NL_IFA_LOCAL     2
#define NL_IFA_LABEL     3
#define NL_RTA_GATEWAY   5
#define NL_RTA_OIF       4

/* Helper: send a netlink dump request and read response into buffer.
 * Returns total bytes received, or -1 on error.
 */
static int netlink_dump(int type, void *payload, int payload_len,
                        char *resp, int resp_size)
{
    int fd = socket(NL_AF_NETLINK, SOCK_DGRAM, NL_NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    /* Build request: nlmsghdr + payload */
    char req[128];
    memset(req, 0, sizeof(req));
    struct nl_msghdr *nlh = (struct nl_msghdr *)req;
    nlh->nlmsg_len   = NL_NLMSG_HDRLEN + payload_len;
    nlh->nlmsg_type  = type;
    nlh->nlmsg_flags = NL_NLM_F_REQUEST | NL_NLM_F_DUMP;
    nlh->nlmsg_seq   = 1;
    nlh->nlmsg_pid   = 0;
    if (payload_len > 0)
        memcpy(req + NL_NLMSG_HDRLEN, payload, payload_len);

    /* Send request */
    struct nl_sockaddr sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = NL_AF_NETLINK;
    ssize_t sent = sendto(fd, req, nlh->nlmsg_len, 0,
                          (struct sockaddr *)&sa, sizeof(sa));
    if (sent < 0) {
        close(fd);
        return -1;
    }

    /* Read response */
    ssize_t nr = read(fd, resp, resp_size);
    close(fd);
    return (int)nr;
}

/* Count netlink messages in a response buffer, stopping at NLMSG_DONE */
static int count_nl_msgs(const char *buf, int len) {
    int count = 0;
    int off = 0;
    while (off + NL_NLMSG_HDRLEN <= len) {
        const struct nl_msghdr *nlh = (const struct nl_msghdr *)(buf + off);
        if (nlh->nlmsg_len < NL_NLMSG_HDRLEN || (int)nlh->nlmsg_len > len - off)
            break;
        if (nlh->nlmsg_type == NL_NLMSG_DONE)
            break;
        if (nlh->nlmsg_type == NL_NLMSG_ERROR)
            return -1;
        count++;
        off += NL_NLMSG_ALIGN(nlh->nlmsg_len);
    }
    return count;
}

/* Find an rtattr of given type inside a netlink message payload */
static const struct nl_rtattr *find_rtattr(const char *payload, int payload_len,
                                           int rta_type)
{
    int off = 0;
    while (off + (int)sizeof(struct nl_rtattr) <= payload_len) {
        const struct nl_rtattr *rta = (const struct nl_rtattr *)(payload + off);
        if (rta->rta_len < sizeof(struct nl_rtattr))
            break;
        if (rta->rta_type == rta_type)
            return rta;
        off += NL_NLMSG_ALIGN(rta->rta_len);
    }
    return NULL;
}

/* ── Test: AF_NETLINK socket creation ───────────────────────────────── */

static void test_netlink_create(void)
{
    printf("\n=== AF_NETLINK socket creation ===\n");

    int fd = socket(NL_AF_NETLINK, SOCK_DGRAM, NL_NETLINK_ROUTE);
    if (fd < 0) {
        FAIL("socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE)");
        return;
    }
    PASS("AF_NETLINK socket created");

    /* SOCK_RAW should also work */
    int fd2 = socket(NL_AF_NETLINK, SOCK_RAW, NL_NETLINK_ROUTE);
    if (fd2 >= 0) {
        PASS("AF_NETLINK SOCK_RAW created");
        close(fd2);
    } else {
        FAIL("AF_NETLINK SOCK_RAW");
    }

    /* getsockname should report AF_NETLINK */
    struct nl_sockaddr sa;
    socklen_t slen = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) == 0 &&
        sa.nl_family == NL_AF_NETLINK)
        PASS("AF_NETLINK getsockname");
    else
        FAIL("AF_NETLINK getsockname");

    close(fd);
}

/* ── Test: RTM_GETLINK (enumerate interfaces) ───────────────────────── */

static void test_netlink_getlink(void)
{
    printf("\n=== AF_NETLINK RTM_GETLINK ===\n");

    struct nl_ifinfomsg ifm;
    memset(&ifm, 0, sizeof(ifm));
    ifm.ifi_family = 0; /* AF_UNSPEC */

    char resp[4096];
    int nr = netlink_dump(NL_RTM_GETLINK, &ifm, sizeof(ifm), resp, sizeof(resp));
    if (nr < 0) {
        FAIL("RTM_GETLINK dump request");
        return;
    }

    int nmsg = count_nl_msgs(resp, nr);
    if (nmsg > 0)
        PASS("RTM_GETLINK returned interface(s)");
    else
        FAIL("RTM_GETLINK no interfaces");

    /* Check first interface has an IFLA_IFNAME attribute */
    if (nmsg > 0) {
        const struct nl_msghdr *nlh = (const struct nl_msghdr *)resp;
        int attr_off = NL_NLMSG_HDRLEN + NL_NLMSG_ALIGN(sizeof(struct nl_ifinfomsg));
        int attr_len = (int)nlh->nlmsg_len - attr_off;
        if (attr_len > 0) {
            const struct nl_rtattr *rta = find_rtattr(resp + attr_off, attr_len, NL_IFLA_IFNAME);
            if (rta) {
                const char *ifname = (const char *)rta + NL_NLMSG_ALIGN(sizeof(struct nl_rtattr));
                printf("    First interface: %s\n", ifname);
                PASS("RTM_GETLINK has IFLA_IFNAME");
            } else {
                FAIL("RTM_GETLINK missing IFLA_IFNAME");
            }
        } else {
            FAIL("RTM_GETLINK no attributes");
        }
    }

    /* Check for NLMSG_DONE at the end */
    {
        int off = 0;
        int found_done = 0;
        while (off + NL_NLMSG_HDRLEN <= nr) {
            const struct nl_msghdr *nlh = (const struct nl_msghdr *)(resp + off);
            if (nlh->nlmsg_len < (unsigned)NL_NLMSG_HDRLEN)
                break;
            if (nlh->nlmsg_type == NL_NLMSG_DONE) {
                found_done = 1;
                break;
            }
            off += NL_NLMSG_ALIGN(nlh->nlmsg_len);
        }
        if (found_done)
            PASS("RTM_GETLINK ends with NLMSG_DONE");
        else
            FAIL("RTM_GETLINK missing NLMSG_DONE");
    }
}

/* ── Test: RTM_GETADDR (enumerate addresses) ────────────────────────── */

static void test_netlink_getaddr(void)
{
    printf("\n=== AF_NETLINK RTM_GETADDR ===\n");

    struct nl_ifaddrmsg ifa;
    memset(&ifa, 0, sizeof(ifa));
    ifa.ifa_family = 2; /* AF_INET */

    char resp[4096];
    int nr = netlink_dump(NL_RTM_GETADDR, &ifa, sizeof(ifa), resp, sizeof(resp));
    if (nr < 0) {
        FAIL("RTM_GETADDR dump request");
        return;
    }

    int nmsg = count_nl_msgs(resp, nr);
    if (nmsg > 0)
        PASS("RTM_GETADDR returned address(es)");
    else
        FAIL("RTM_GETADDR no addresses");

    /* Check first address has IFA_LOCAL */
    if (nmsg > 0) {
        const struct nl_msghdr *nlh = (const struct nl_msghdr *)resp;
        int attr_off = NL_NLMSG_HDRLEN + NL_NLMSG_ALIGN(sizeof(struct nl_ifaddrmsg));
        int attr_len = (int)nlh->nlmsg_len - attr_off;
        if (attr_len > 0) {
            const struct nl_rtattr *rta = find_rtattr(resp + attr_off, attr_len, NL_IFA_LOCAL);
            if (rta && rta->rta_len >= (int)sizeof(struct nl_rtattr) + 4) {
                unsigned char *ip = (unsigned char *)rta + NL_NLMSG_ALIGN(sizeof(struct nl_rtattr));
                printf("    First address: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
                PASS("RTM_GETADDR has IFA_LOCAL");
            } else {
                FAIL("RTM_GETADDR missing IFA_LOCAL");
            }
        } else {
            FAIL("RTM_GETADDR no attributes");
        }
    }
}

/* ── Test: RTM_GETROUTE (enumerate routes) ──────────────────────────── */

static void test_netlink_getroute(void)
{
    printf("\n=== AF_NETLINK RTM_GETROUTE ===\n");

    struct nl_rtmsg rtm;
    memset(&rtm, 0, sizeof(rtm));
    rtm.rtm_family = 2; /* AF_INET */

    char resp[4096];
    int nr = netlink_dump(NL_RTM_GETROUTE, &rtm, sizeof(rtm), resp, sizeof(resp));
    if (nr < 0) {
        FAIL("RTM_GETROUTE dump request");
        return;
    }

    int nmsg = count_nl_msgs(resp, nr);
    if (nmsg > 0)
        PASS("RTM_GETROUTE returned route(s)");
    else
        FAIL("RTM_GETROUTE no routes");
}

/* ── Test: SO_RCVTIMEO / SO_SNDTIMEO ───────────────────────────────── */

static void test_timeouts(void)
{
    printf("\n=== SO_RCVTIMEO / SO_SNDTIMEO ===\n");

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { FAIL("socket for timeouts"); return; }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 500000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0) {
        struct timeval tv2 = {0};
        socklen_t olen = sizeof(tv2);
        getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, &olen);
        /* lwIP stores in ms granularity, so check approximately */
        if (tv2.tv_sec == 2)
            PASS("SO_RCVTIMEO set+get ~2.5s");
        else
            FAIL("SO_RCVTIMEO readback");
    } else {
        FAIL("SO_RCVTIMEO setsockopt");
    }

    tv.tv_sec = 3; tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0) {
        struct timeval tv2 = {0};
        socklen_t olen = sizeof(tv2);
        getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv2, &olen);
        if (tv2.tv_sec == 3)
            PASS("SO_SNDTIMEO set+get 3s");
        else
            FAIL("SO_SNDTIMEO readback");
    } else {
        FAIL("SO_SNDTIMEO setsockopt");
    }

    close(fd);
}

/* ========================================================================== */
/* Stress tests                                                               */
/* ========================================================================== */

/*
 * Stress: rapid socketpair create/close cycles.
 * Exercises the refcount logic and unix_file_release concurrency —
 * exactly the scenario that triggered the AB-BA deadlock on close.
 */
static void stress_socketpair_create_close(void)
{
    printf("\n=== STRESS: socketpair rapid create/close ===\n");

    const int ROUNDS = 200;
    int ok = 0;
    for (int i = 0; i < ROUNDS; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            FAIL("stress socketpair create");
            return;
        }
        close(sv[0]);
        close(sv[1]);
        ok++;
    }
    if (ok == ROUNDS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d socketpair create/close cycles", ROUNDS);
        PASS(msg);
    }
}

/*
 * Stress: concurrent close of socketpair ends from parent + child.
 * Each side closes its end simultaneously — tests the refcount fix.
 */
static void stress_socketpair_concurrent_close(void)
{
    printf("\n=== STRESS: concurrent socketpair close ===\n");

    const int ROUNDS = 50;
    int ok = 0;
    for (int i = 0; i < ROUNDS; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            FAIL("stress concurrent close: socketpair");
            return;
        }

        int pid = fork();
        if (pid < 0) {
            FAIL("stress concurrent close: fork");
            close(sv[0]);
            close(sv[1]);
            return;
        }
        if (pid == 0) {
            /* child: close its end and exit */
            close(sv[0]);
            close(sv[1]);
            _exit(0);
        }
        /* parent: close both ends too (races with child) */
        close(sv[0]);
        close(sv[1]);

        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            FAIL("stress concurrent close: child failed");
            return;
        }
        ok++;
    }
    if (ok == ROUNDS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d concurrent close rounds", ROUNDS);
        PASS(msg);
    }
}

/*
 * Stress: fill and drain the ring buffer multiple times.
 * Writes PAGE_SIZE worth of data in small chunks, reads it all back,
 * repeats several times to exercise wrap-around.
 */
static void stress_socketpair_data_volume(void)
{
    printf("\n=== STRESS: socketpair large data transfer ===\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        FAIL("stress data: socketpair");
        return;
    }

    /* Set both ends non-blocking so we don't deadlock on full ring */
    int flags = fcntl(sv[0], F_GETFL);
    fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(sv[1], F_GETFL);
    fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);

    const int TOTAL = 4096 * 8; /* 32KB total — 8x ring buffer size */
    unsigned char wbuf[128];
    unsigned char rbuf[128];
    int total_written = 0, total_read = 0;
    unsigned char wseq = 0, rseq = 0;

    /* Interleave writes on sv[0] and reads on sv[1] */
    while (total_read < TOTAL) {
        /* Try to write */
        if (total_written < TOTAL) {
            int chunk = TOTAL - total_written;
            if (chunk > (int)sizeof(wbuf)) chunk = (int)sizeof(wbuf);
            for (int j = 0; j < chunk; j++)
                wbuf[j] = wseq++;
            ssize_t nw = write(sv[0], wbuf, chunk);
            if (nw > 0)
                total_written += (int)nw;
            /* EAGAIN is fine — ring full */
        }

        /* Try to read */
        ssize_t nr = read(sv[1], rbuf, sizeof(rbuf));
        if (nr > 0) {
            /* Verify sequence */
            for (int j = 0; j < (int)nr; j++) {
                if (rbuf[j] != rseq) {
                    FAIL("stress data: sequence mismatch");
                    close(sv[0]);
                    close(sv[1]);
                    return;
                }
                rseq++;
            }
            total_read += (int)nr;
        }
        /* If nothing happened on either side, yield briefly */
        if (nr <= 0 && total_written >= TOTAL)
            usleep(1000);
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "%d bytes transferred with sequence verify", TOTAL);
    PASS(msg);

    close(sv[0]);
    close(sv[1]);
}

/*
 * Stress: parent-child ping-pong over socketpair.
 * Exercises concurrent read/write from both sides.
 */
static void stress_socketpair_pingpong(void)
{
    printf("\n=== STRESS: socketpair ping-pong ===\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        FAIL("stress pingpong: socketpair");
        return;
    }

    const int ROUNDS = 100;

    int pid = fork();
    if (pid < 0) {
        FAIL("stress pingpong: fork");
        close(sv[0]);
        close(sv[1]);
        return;
    }

    if (pid == 0) {
        /* child: read a byte, write it back + 1 */
        close(sv[0]);
        for (int i = 0; i < ROUNDS; i++) {
            unsigned char b;
            ssize_t nr = read(sv[1], &b, 1);
            if (nr != 1) _exit(1);
            b++;
            ssize_t nw = write(sv[1], &b, 1);
            if (nw != 1) _exit(2);
        }
        close(sv[1]);
        _exit(0);
    }

    /* parent: write a byte, read response */
    close(sv[1]);
    int ok = 1;
    for (int i = 0; i < ROUNDS; i++) {
        unsigned char b = (unsigned char)(i & 0xff);
        ssize_t nw = write(sv[0], &b, 1);
        if (nw != 1) { ok = 0; break; }
        unsigned char r;
        ssize_t nr = read(sv[0], &r, 1);
        if (nr != 1) { ok = 0; break; }
        if (r != (unsigned char)((b + 1) & 0xff)) { ok = 0; break; }
    }
    close(sv[0]);

    int status;
    waitpid(pid, &status, 0);
    if (ok && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d ping-pong rounds", ROUNDS);
        PASS(msg);
    } else {
        FAIL("stress pingpong failed");
    }
}

/*
 * Stress: many children connect to one server.
 * Tests accept queue, concurrent connect, and cleanup.
 */
static void stress_unix_many_clients(void)
{
    printf("\n=== STRESS: AF_UNIX many clients ===\n");

    const int NCLIENTS = 10;
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { FAIL("stress clients: socket"); return; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/stress.%d", getpid());

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        FAIL("stress clients: bind");
        close(sfd);
        return;
    }
    if (listen(sfd, NCLIENTS) < 0) {
        FAIL("stress clients: listen");
        close(sfd);
        return;
    }

    /* Fork NCLIENTS children, each connects, sends its index, reads echo */
    int cpids[10]; /* NCLIENTS */
    for (int i = 0; i < NCLIENTS; i++) {
        int pid = fork();
        if (pid < 0) {
            FAIL("stress clients: fork");
            /* clean up already-forked children */
            for (int j = 0; j < i; j++) waitpid(cpids[j], NULL, 0);
            close(sfd);
            return;
        }
        if (pid == 0) {
            close(sfd);
            int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (cfd < 0) _exit(1);
            if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
                _exit(2);
            unsigned char idx = (unsigned char)i;
            if (write(cfd, &idx, 1) != 1) _exit(3);
            unsigned char reply;
            if (read(cfd, &reply, 1) != 1) _exit(4);
            close(cfd);
            _exit(reply == idx ? 0 : 5);
        }
        cpids[i] = pid;
    }

    /* Server: accept NCLIENTS, echo back */
    int accepted = 0;
    for (int i = 0; i < NCLIENTS; i++) {
        int afd = accept(sfd, NULL, NULL);
        if (afd < 0) continue;
        unsigned char idx;
        if (read(afd, &idx, 1) == 1)
            write(afd, &idx, 1);
        close(afd);
        accepted++;
    }
    close(sfd);

    /* Wait for all children */
    int all_ok = 1;
    for (int i = 0; i < NCLIENTS; i++) {
        int status;
        waitpid(cpids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            all_ok = 0;
    }

    if (all_ok && accepted == NCLIENTS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d clients connect/echo/close", NCLIENTS);
        PASS(msg);
    } else {
        FAIL("stress many clients");
    }
}

/*
 * Stress: rapid AF_NETLINK socket create + query + close.
 * Tests netlink allocation/deallocation churn.
 */
static void stress_netlink_churn(void)
{
    printf("\n=== STRESS: netlink rapid create/query/close ===\n");

    const int ROUNDS = 50;
    int ok = 0;
    for (int i = 0; i < ROUNDS; i++) {
        struct nl_ifinfomsg ifm;
        memset(&ifm, 0, sizeof(ifm));
        char resp[2048];
        int nr = netlink_dump(NL_RTM_GETLINK, &ifm, sizeof(ifm), resp, sizeof(resp));
        if (nr > 0 && count_nl_msgs(resp, nr) > 0)
            ok++;
    }
    if (ok == ROUNDS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d netlink create/query/close cycles", ROUNDS);
        PASS(msg);
    } else {
        FAIL("stress netlink churn");
    }
}

/*
 * Stress: socketpair with fork — both sides do heavy I/O then close.
 * Parent writes a burst, child reads and verifies, then both close.
 * Tests cleanup under I/O contention.
 */
static void stress_socketpair_fork_io(void)
{
    printf("\n=== STRESS: socketpair fork + heavy I/O ===\n");

    const int ROUNDS = 20;
    int ok = 0;

    for (int i = 0; i < ROUNDS; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            FAIL("stress fork-io: socketpair");
            return;
        }

        int pid = fork();
        if (pid < 0) {
            close(sv[0]);
            close(sv[1]);
            FAIL("stress fork-io: fork");
            return;
        }

        if (pid == 0) {
            /* child: read all data until EOF, verify total */
            close(sv[0]);
            char buf[256];
            int total = 0;
            ssize_t nr;
            while ((nr = read(sv[1], buf, sizeof(buf))) > 0)
                total += (int)nr;
            close(sv[1]);
            _exit(total == 512 ? 0 : 1);
        }

        /* parent: write 512 bytes then close */
        close(sv[1]);
        char data[512];
        memset(data, 'A' + (i % 26), sizeof(data));
        int written = 0;
        while (written < (int)sizeof(data)) {
            ssize_t nw = write(sv[0], data + written, sizeof(data) - written);
            if (nw <= 0) break;
            written += (int)nw;
        }
        close(sv[0]);

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && written == 512)
            ok++;
    }

    if (ok == ROUNDS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d fork+write+EOF rounds", ROUNDS);
        PASS(msg);
    } else {
        FAIL("stress fork-io");
    }
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("socktest: validating socket options, ioctl, MSG_PEEK, AF_UNIX, AF_NETLINK\n");

    test_sol_socket_udp();
    test_ip_options();
    test_tcp_options();
    test_ioctl();
    test_msg_peek();
    test_timeouts();

    /* AF_UNIX tests */
    test_unix_socketpair();
    test_unix_socketpair_dgram();
    test_unix_connect();
    test_unix_shutdown();
    test_unix_getpeername();

    /* AF_NETLINK tests */
    test_netlink_create();
    test_netlink_getlink();
    test_netlink_getaddr();
    test_netlink_getroute();

    /* Stress tests */
    stress_socketpair_create_close();
    stress_socketpair_concurrent_close();
    stress_socketpair_data_volume();
    stress_socketpair_pingpong();
    stress_unix_many_clients();
    stress_netlink_churn();
    stress_socketpair_fork_io();

    printf("\n========================================\n");
    printf("socktest: %d passed, %d failed\n", passes, fails);
    printf("========================================\n");

    return fails ? 1 : 0;
}
