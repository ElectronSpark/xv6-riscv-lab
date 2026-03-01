/*
 * epolltest — exercise epoll and O_NONBLOCK on xv6
 *
 * Creates a UDP socket pair (sender + receiver), registers the receiver
 * with epoll, sends a datagram, and verifies epoll_wait reports it ready.
 * Also tests SOCK_NONBLOCK / EAGAIN behaviour.
 *
 * Note: xv6's lwIP has LWIP_NETIF_LOOPBACK=0, so we cannot use 127.0.0.1.
 * Instead we discover the interface's own IP (typically 10.0.2.15 under QEMU
 * SLIRP) via a UDP connect+getsockname trick and send to that.
 *
 * Usage: epolltest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define TEST_PORT 9753
#define PASS(msg) printf("  PASS: %s\n", (msg))
#define FAIL(msg) do { printf("  FAIL: %s (errno=%d)\n", (msg), errno); fails++; } while(0)

static int fails = 0;

/*
 * Discover the local interface IP by connect()ing a UDP socket to an
 * external address (the QEMU gateway 10.0.2.2) then reading back the
 * source address with getsockname().  Falls back to INADDR_LOOPBACK.
 */
static in_addr_t get_local_ip(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return htonl(INADDR_LOOPBACK);

    struct sockaddr_in gw;
    memset(&gw, 0, sizeof(gw));
    gw.sin_family = AF_INET;
    gw.sin_port = htons(53);                  /* any port — no packet sent */
    gw.sin_addr.s_addr = htonl(0x0a000202);   /* 10.0.2.2 (QEMU gateway) */

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
    return me.sin_addr.s_addr;   /* already in network byte order */
}

/* ------------------------------------------------------------------ */
/* Test 1: epoll_create1 / epoll_ctl / epoll_wait                     */
/* ------------------------------------------------------------------ */
static void test_epoll_basic(void)
{
    printf("[test_epoll_basic]\n");

    /* 1. Create epoll instance */
    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL("epoll_create1"); return; }
    PASS("epoll_create1 returned fd");

    /* 2. Create a UDP receiver socket */
    int rfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rfd < 0) { FAIL("socket(receiver)"); close(epfd); return; }

    /* Bind receiver to INADDR_ANY (loopback disabled in lwIP) */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(rfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        FAIL("bind(receiver)");
        close(rfd); close(epfd);
        return;
    }
    PASS("receiver bound");

    /* 3. Register receiver fd with epoll for EPOLLIN */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = rfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, rfd, &ev) < 0) {
        FAIL("epoll_ctl ADD");
        close(rfd); close(epfd);
        return;
    }
    PASS("epoll_ctl ADD");

    /* 4. epoll_wait with timeout=0 — should return 0 (no data yet) */
    struct epoll_event out[4];
    int n = epoll_wait(epfd, out, 4, 0);
    if (n == 0) {
        PASS("epoll_wait poll returned 0 (no data)");
    } else {
        FAIL("epoll_wait poll expected 0");
    }

    /* 5. Send a datagram to the receiver via the interface's own IP */
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) { FAIL("socket(sender)"); close(rfd); close(epfd); return; }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(TEST_PORT);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* 127.0.0.1 via loopif */

    const char *msg = "hello epoll";
    ssize_t sent = sendto(sfd, msg, strlen(msg), 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        FAIL("sendto");
        close(sfd); close(rfd); close(epfd);
        return;
    }
    PASS("datagram sent");

    /* 6. epoll_wait with timeout=1000 — should return 1 event */
    n = epoll_wait(epfd, out, 4, 1000);
    if (n == 1 && (out[0].events & EPOLLIN)) {
        PASS("epoll_wait returned 1 EPOLLIN event");
    } else {
        printf("    n=%d events=0x%x\n", n, n > 0 ? out[0].events : 0);
        FAIL("epoll_wait expected 1 EPOLLIN");
    }

    /* 7. Verify data.fd is preserved */
    if (n == 1 && out[0].data.fd == rfd) {
        PASS("epoll_event.data.fd preserved");
    } else if (n == 1) {
        printf("    data.fd=%d expected=%d\n", out[0].data.fd, rfd);
        FAIL("epoll_event.data.fd mismatch");
    }

    /* 8. Read the datagram */
    char buf[64];
    ssize_t rn = recv(rfd, buf, sizeof(buf), 0);
    if (rn == (ssize_t)strlen(msg) && memcmp(buf, msg, rn) == 0) {
        PASS("recv got correct data");
    } else {
        FAIL("recv data mismatch");
    }

    /* 9. EPOLL_CTL_DEL */
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, rfd, NULL) < 0) {
        FAIL("epoll_ctl DEL");
    } else {
        PASS("epoll_ctl DEL");
    }

    close(sfd);
    close(rfd);
    close(epfd);
}

/* ------------------------------------------------------------------ */
/* Test 2: SOCK_NONBLOCK + EAGAIN                                     */
/* ------------------------------------------------------------------ */
static void test_nonblock(void)
{
    printf("[test_nonblock]\n");

    /* Create a nonblocking UDP socket */
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { FAIL("socket(SOCK_NONBLOCK)"); return; }
    PASS("socket(SOCK_NONBLOCK) ok");

    /* Verify O_NONBLOCK is set via fcntl */
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0 && (fl & O_NONBLOCK)) {
        PASS("fcntl F_GETFL shows O_NONBLOCK");
    } else {
        printf("    flags=0x%x\n", fl);
        FAIL("O_NONBLOCK not set");
    }

    /* Bind to a port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 1);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        FAIL("bind(nonblock)");
        close(fd);
        return;
    }

    /* recv on empty nonblocking socket → EAGAIN */
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0 && errno == EAGAIN) {
        PASS("recv on empty nonblock socket → EAGAIN");
    } else {
        printf("    n=%zd errno=%d\n", n, errno);
        FAIL("expected EAGAIN");
    }

    close(fd);
}

/* ------------------------------------------------------------------ */
/* Test 3: MSG_DONTWAIT                                               */
/* ------------------------------------------------------------------ */
static void test_msg_dontwait(void)
{
    printf("[test_msg_dontwait]\n");

    /* Create a normal (blocking) UDP socket */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { FAIL("socket"); return; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 2);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        FAIL("bind");
        close(fd);
        return;
    }

    /* recvfrom with MSG_DONTWAIT on empty socket → EAGAIN */
    char buf[64];
    ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
    if (n < 0 && errno == EAGAIN) {
        PASS("recvfrom MSG_DONTWAIT → EAGAIN");
    } else {
        printf("    n=%zd errno=%d\n", n, errno);
        FAIL("expected EAGAIN from MSG_DONTWAIT");
    }

    close(fd);
}

/* ------------------------------------------------------------------ */
/* Test 4: fcntl F_SETFL O_NONBLOCK                                   */
/* ------------------------------------------------------------------ */
static void test_fcntl_nonblock(void)
{
    printf("[test_fcntl_nonblock]\n");

    /* Create a blocking socket, then make it nonblocking via fcntl */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { FAIL("socket"); return; }

    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) { FAIL("F_GETFL"); close(fd); return; }

    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        FAIL("F_SETFL O_NONBLOCK");
        close(fd);
        return;
    }
    PASS("fcntl F_SETFL O_NONBLOCK");

    /* Verify it's set */
    fl = fcntl(fd, F_GETFL);
    if (fl >= 0 && (fl & O_NONBLOCK)) {
        PASS("F_GETFL confirms O_NONBLOCK");
    } else {
        FAIL("O_NONBLOCK not confirmed");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 3);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        FAIL("bind");
        close(fd);
        return;
    }

    /* recv should return EAGAIN */
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0 && errno == EAGAIN) {
        PASS("recv after fcntl O_NONBLOCK → EAGAIN");
    } else {
        printf("    n=%zd errno=%d\n", n, errno);
        FAIL("expected EAGAIN");
    }

    close(fd);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== epolltest: epoll + nonblock test suite ===\n");

    test_epoll_basic();
    test_nonblock();
    test_msg_dontwait();
    test_fcntl_nonblock();

    printf("=== %s: %d failure(s) ===\n",
           fails == 0 ? "ALL PASSED" : "SOME FAILED", fails);
    return fails ? 1 : 0;
}
