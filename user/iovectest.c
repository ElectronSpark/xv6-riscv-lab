/**
 * @file iovectest.c
 * @brief Validation test for vectored I/O (readv / writev / preadv / pwritev2).
 *
 * Exercises the kernel's iovec subsystem through the following tests:
 *
 *  1. writev — scatter-write multiple buffers into a file
 *  2. readv  — gather-read them back and compare
 *  3. pwritev2 — positional scatter-write at an explicit offset
 *  4. preadv — positional gather-read at an explicit offset
 *  5. single-byte segments — stress the iterator advance logic
 *  6. zero-length segment — ensure it is silently skipped
 *  7. error paths — bad fd, invalid iovcnt
 *  8. pipe vectored I/O — writev into a pipe, readv out
 *  9. large multi-segment — many small segments in one call
 * 10. partial overwrite via pwritev2 — verify surrounding data intact
 */

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

#define TESTFILE "/iovectest_tmp"

static int failures = 0;

#define ASSERT(cond, msg)                                                      \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                    \
            failures++;                                                         \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_EQ(a, b, msg)                                                   \
    do {                                                                        \
        int64 __a = (int64)(a), __b = (int64)(b);                               \
        if (__a != __b) {                                                       \
            printf("  FAIL: %s: expected %d, got %d (line %d)\n",               \
                   msg, (int)__b, (int)__a, __LINE__);                          \
            failures++;                                                         \
            return;                                                             \
        }                                                                       \
    } while (0)

/* preadv / pwritev2 thin wrappers — syscall stubs defined in usys.S */
int preadv(int fd, const struct iovec *iov, int iovcnt, int64 offset);
int pwritev2(int fd, const struct iovec *iov, int iovcnt, int64 offset,
             int flags);

/* ── Test 1: basic writev + readv round-trip ─────────────────────── */

static void test_writev_readv(void)
{
    printf("test_writev_readv...");

    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open for write");

    char a[] = "Hello";
    char b[] = ", ";
    char c[] = "iovec!";

    struct iovec wv[3] = {
        { a, strlen(a) },
        { b, strlen(b) },
        { c, strlen(c) },
    };

    int nw = writev(fd, wv, 3);
    ASSERT_EQ(nw, 13, "writev return");

    close(fd);

    /* Read back */
    fd = open(TESTFILE, O_RDONLY);
    ASSERT(fd >= 0, "open for read");

    char r1[6], r2[3], r3[7];
    memset(r1, 0, sizeof(r1));
    memset(r2, 0, sizeof(r2));
    memset(r3, 0, sizeof(r3));

    struct iovec rv[3] = {
        { r1, 5 },
        { r2, 2 },
        { r3, 6 },
    };

    int nr = readv(fd, rv, 3);
    ASSERT_EQ(nr, 13, "readv return");
    ASSERT(memcmp(r1, "Hello", 5) == 0, "segment 1 data");
    ASSERT(memcmp(r2, ", ", 2) == 0, "segment 2 data");
    ASSERT(memcmp(r3, "iovec!", 6) == 0, "segment 3 data");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Test 2: pwritev2 + preadv positional I/O ────────────────────── */

static void test_pwritev2_preadv(void)
{
    printf("test_pwritev2_preadv...");

    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open");

    /* Write "AAAAABBBBB" starting at offset 0 */
    char a[5], b[5];
    memset(a, 'A', 5);
    memset(b, 'B', 5);

    struct iovec wv[2] = {
        { a, 5 },
        { b, 5 },
    };

    int nw = pwritev2(fd, wv, 2, 0, 0);
    ASSERT_EQ(nw, 10, "pwritev2 return");

    /* Read back the second half at offset 5 */
    char rbuf[5];
    memset(rbuf, 0, sizeof(rbuf));
    struct iovec rv[1] = {
        { rbuf, 5 },
    };

    int nr = preadv(fd, rv, 1, 5);
    ASSERT_EQ(nr, 5, "preadv return");
    ASSERT(memcmp(rbuf, "BBBBB", 5) == 0, "preadv data at offset 5");

    /* Read back the first half */
    memset(rbuf, 0, sizeof(rbuf));
    nr = preadv(fd, rv, 1, 0);
    ASSERT_EQ(nr, 5, "preadv at offset 0 return");
    ASSERT(memcmp(rbuf, "AAAAA", 5) == 0, "preadv data at offset 0");

    /* Ensure file position was NOT moved by preadv/pwritev2 */
    char posbuf[1];
    struct iovec pv = { posbuf, 1 };
    nr = readv(fd, &pv, 1);
    /* File position should still be 0 (never moved by positional ops) */
    ASSERT_EQ(nr, 1, "readv after positional ops");
    ASSERT(posbuf[0] == 'A', "file position unchanged");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Test 3: single-byte segments (iterator advance stress) ──────── */

static void test_single_byte_segments(void)
{
    printf("test_single_byte_segments...");

    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open");

    char src[] = "ABCDEFGHIJ"; /* 10 bytes */
    struct iovec wv[10];
    for (int i = 0; i < 10; i++) {
        wv[i].iov_base = &src[i];
        wv[i].iov_len = 1;
    }

    int nw = writev(fd, wv, 10);
    ASSERT_EQ(nw, 10, "writev 10x1 return");

    close(fd);
    fd = open(TESTFILE, O_RDONLY);
    ASSERT(fd >= 0, "reopen");

    char dst[10];
    memset(dst, 0, sizeof(dst));
    struct iovec rv[10];
    for (int i = 0; i < 10; i++) {
        rv[i].iov_base = &dst[i];
        rv[i].iov_len = 1;
    }

    int nr = readv(fd, rv, 10);
    ASSERT_EQ(nr, 10, "readv 10x1 return");
    ASSERT(memcmp(dst, "ABCDEFGHIJ", 10) == 0, "single-byte data");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Test 4: zero-length segment ─────────────────────────────────── */

static void test_zero_length_segment(void)
{
    printf("test_zero_length_segment...");

    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open");

    char data[] = "XY";
    struct iovec wv[3] = {
        { data, 1 },       /* 'X' */
        { data, 0 },       /* zero-length, should be skipped */
        { data + 1, 1 },   /* 'Y' */
    };

    int nw = writev(fd, wv, 3);
    ASSERT_EQ(nw, 2, "writev with zero-len seg");

    close(fd);
    fd = open(TESTFILE, O_RDONLY);
    ASSERT(fd >= 0, "reopen");

    char rbuf[2];
    memset(rbuf, 0, sizeof(rbuf));
    struct iovec rv = { rbuf, 2 };
    int nr = readv(fd, &rv, 1);
    ASSERT_EQ(nr, 2, "readv return");
    ASSERT(rbuf[0] == 'X' && rbuf[1] == 'Y', "zero-len skip data");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Test 5: error paths ─────────────────────────────────────────── */

static void test_error_paths(void)
{
    printf("test_error_paths...");

    char buf[8];
    struct iovec v = { buf, 8 };

    /* Bad fd */
    int ret = readv(999, &v, 1);
    ASSERT(ret < 0, "readv bad fd");

    ret = writev(999, &v, 1);
    ASSERT(ret < 0, "writev bad fd");

    /* Invalid iovcnt */
    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open");

    ret = readv(fd, &v, 0);
    ASSERT(ret < 0, "readv iovcnt=0");

    ret = readv(fd, &v, -1);
    ASSERT(ret < 0, "readv iovcnt=-1");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Test 6: pipe vectored I/O ───────────────────────────────────── */

static void test_pipe_iovec(void)
{
    printf("test_pipe_iovec...");

    int pipefd[2];
    int ret = pipe(pipefd);
    ASSERT(ret == 0, "pipe");

    char s1[] = "pipe";
    char s2[] = "vec";
    struct iovec wv[2] = {
        { s1, 4 },
        { s2, 3 },
    };

    int pid = fork();
    ASSERT(pid >= 0, "fork");

    if (pid == 0) {
        /* Child: write to pipe */
        close(pipefd[0]);
        int nw = writev(pipefd[1], wv, 2);
        close(pipefd[1]);
        exit(nw == 7 ? 0 : 1);
    }

    /* Parent: read from pipe */
    close(pipefd[1]);

    char rbuf[7];
    memset(rbuf, 0, sizeof(rbuf));
    int nr = 0;
    while (nr < 7) {
        struct iovec rv = { rbuf + nr, 7 - nr };
        int n = readv(pipefd[0], &rv, 1);
        ASSERT(n >= 0, "readv from pipe");
        if (n == 0)
            break;
        nr += n;
    }
    close(pipefd[0]);

    int status;
    wait(&status);
    ASSERT_EQ(status, 0, "child exit status");
    ASSERT_EQ(nr, 7, "readv from pipe");
    ASSERT(memcmp(rbuf, "pipevec", 7) == 0, "pipe data");

    printf(" ok\n");
}

/* ── Test 7: many small segments ─────────────────────────────────── */

#define MANY_SEGS 32

static void test_many_segments(void)
{
    printf("test_many_segments...");

    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open");

    char src[MANY_SEGS];
    struct iovec wv[MANY_SEGS];
    for (int i = 0; i < MANY_SEGS; i++) {
        src[i] = (char)('0' + (i % 10));
        wv[i].iov_base = &src[i];
        wv[i].iov_len = 1;
    }

    int nw = writev(fd, wv, MANY_SEGS);
    ASSERT_EQ(nw, MANY_SEGS, "writev many segments");

    close(fd);
    fd = open(TESTFILE, O_RDONLY);
    ASSERT(fd >= 0, "reopen");

    char dst[MANY_SEGS];
    memset(dst, 0, sizeof(dst));
    struct iovec rv[MANY_SEGS];
    for (int i = 0; i < MANY_SEGS; i++) {
        rv[i].iov_base = &dst[i];
        rv[i].iov_len = 1;
    }

    int nr = readv(fd, rv, MANY_SEGS);
    ASSERT_EQ(nr, MANY_SEGS, "readv many segments");
    ASSERT(memcmp(dst, src, MANY_SEGS) == 0, "many segments data");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Test 8: partial overwrite with pwritev2 ─────────────────────── */

static void test_partial_overwrite(void)
{
    printf("test_partial_overwrite...");

    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open");

    /* Write initial 20 bytes: "AAAAAAAAAABBBBBBBBBB" */
    char init_a[10], init_b[10];
    memset(init_a, 'A', 10);
    memset(init_b, 'B', 10);
    struct iovec iv[2] = {
        { init_a, 10 },
        { init_b, 10 },
    };
    int nw = writev(fd, iv, 2);
    ASSERT_EQ(nw, 20, "initial write");

    /* Overwrite bytes 5-14 with "CCCCCDDDDD" using pwritev2 */
    char ov_c[5], ov_d[5];
    memset(ov_c, 'C', 5);
    memset(ov_d, 'D', 5);
    struct iovec ov[2] = {
        { ov_c, 5 },
        { ov_d, 5 },
    };
    nw = pwritev2(fd, ov, 2, 5, 0);
    ASSERT_EQ(nw, 10, "pwritev2 overwrite");

    /* Read entire file and verify: "AAAAACCCCCDDDDDBBBBBB" — wait:
     * bytes 0-4: AAAAA, bytes 5-9: CCCCC, bytes 10-14: DDDDD, bytes 15-19: BBBBB */
    char result[20];
    memset(result, 0, sizeof(result));
    struct iovec rv = { result, 20 };
    int nr = preadv(fd, &rv, 1, 0);
    ASSERT_EQ(nr, 20, "preadv full file");

    ASSERT(memcmp(result,      "AAAAA", 5) == 0, "prefix intact");
    ASSERT(memcmp(result + 5,  "CCCCC", 5) == 0, "overwrite part 1");
    ASSERT(memcmp(result + 10, "DDDDD", 5) == 0, "overwrite part 2");
    ASSERT(memcmp(result + 15, "BBBBB", 5) == 0, "suffix intact");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Test 9: large buffer writev + readv ─────────────────────────── */

static void test_large_buffer(void)
{
    printf("test_large_buffer...");

    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open");

    /* Two 512-byte buffers = 1 KB total (one disk block on xv6) */
    char wa[512], wb[512];
    memset(wa, 'P', 512);
    memset(wb, 'Q', 512);

    struct iovec wv[2] = {
        { wa, 512 },
        { wb, 512 },
    };

    int nw = writev(fd, wv, 2);
    ASSERT_EQ(nw, 1024, "writev 1KB");

    /* Read back in three uneven chunks */
    char r1[300], r2[400], r3[324];
    memset(r1, 0, sizeof(r1));
    memset(r2, 0, sizeof(r2));
    memset(r3, 0, sizeof(r3));

    struct iovec rv[3] = {
        { r1, 300 },
        { r2, 400 },
        { r3, 324 },
    };

    int nr = preadv(fd, rv, 3, 0);
    ASSERT_EQ(nr, 1024, "readv 1KB");

    /* Verify: first 512 are 'P', last 512 are 'Q' */
    int ok = 1;
    for (int i = 0; i < 300 && ok; i++)
        if (r1[i] != 'P') ok = 0;
    for (int i = 0; i < 212 && ok; i++)
        if (r2[i] != 'P') ok = 0;
    for (int i = 212; i < 400 && ok; i++)
        if (r2[i] != 'Q') ok = 0;
    for (int i = 0; i < 324 && ok; i++)
        if (r3[i] != 'Q') ok = 0;
    ASSERT(ok, "large buffer data integrity");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Test 10: pwritev2 offset=-1 (use file position) ────────────── */

static void test_pwritev2_curpos(void)
{
    printf("test_pwritev2_curpos...");

    int fd = open(TESTFILE, O_CREAT | O_RDWR | O_TRUNC);
    ASSERT(fd >= 0, "open");

    /* Write "AB" at file position (0) */
    char a = 'A', b = 'B';
    struct iovec wv[2] = { { &a, 1 }, { &b, 1 } };
    int nw = pwritev2(fd, wv, 2, -1, 0); /* offset=-1 ⇒ use f_pos */
    ASSERT_EQ(nw, 2, "pwritev2 offset=-1");

    /* File position should have advanced to 2.
     * Write "CD" at current position (2). */
    char c = 'C', d = 'D';
    struct iovec wv2[2] = { { &c, 1 }, { &d, 1 } };
    nw = pwritev2(fd, wv2, 2, -1, 0);
    ASSERT_EQ(nw, 2, "pwritev2 offset=-1 second");

    /* Read back "ABCD" */
    char rbuf[4];
    memset(rbuf, 0, sizeof(rbuf));
    struct iovec rv = { rbuf, 4 };
    int nr = preadv(fd, &rv, 1, 0);
    ASSERT_EQ(nr, 4, "preadv full");
    ASSERT(memcmp(rbuf, "ABCD", 4) == 0, "pwritev2 -1 data");

    close(fd);
    unlink(TESTFILE);
    printf(" ok\n");
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    printf("iovectest: starting\n");

    test_writev_readv();
    test_pwritev2_preadv();
    test_single_byte_segments();
    test_zero_length_segment();
    test_error_paths();
    test_pipe_iovec();
    test_many_segments();
    test_partial_overwrite();
    test_large_buffer();
    test_pwritev2_curpos();

    if (failures == 0)
        printf("iovectest: ALL TESTS PASSED\n");
    else
        printf("iovectest: %d TESTS FAILED\n", failures);

    exit(failures == 0 ? 0 : 1);
}
