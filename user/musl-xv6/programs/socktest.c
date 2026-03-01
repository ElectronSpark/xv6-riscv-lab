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
 *   - socketpair (expected: -1 / EAFNOSUPPORT)
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

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

/* ── Test: socketpair (should fail with EAFNOSUPPORT) ───────────────── */

static void test_socketpair(void)
{
    printf("\n=== socketpair ===\n");

    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (ret < 0 && errno == EAFNOSUPPORT)
        PASS("socketpair returns EAFNOSUPPORT");
    else if (ret < 0)
        FAIL("socketpair unexpected errno");
    else {
        FAIL("socketpair unexpectedly succeeded");
        close(sv[0]); close(sv[1]);
    }
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

/* ── main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("socktest: validating socket options, ioctl, MSG_PEEK\n");

    test_sol_socket_udp();
    test_ip_options();
    test_tcp_options();
    test_ioctl();
    test_msg_peek();
    test_socketpair();
    test_timeouts();

    printf("\n========================================\n");
    printf("socktest: %d passed, %d failed\n", passes, fails);
    printf("========================================\n");

    return fails ? 1 : 0;
}
