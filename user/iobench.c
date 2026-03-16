/**
 * @file iobench.c
 * @brief I/O throughput benchmark
 *
 * Usage: iobench [-r|-w|-rw] [-s SIZE_MB] [-b BLOCK_KB] [-f FILE]
 *
 * Measures sequential read and write throughput on a file.
 * Defaults: read+write, 16 MB, 64 KB blocks, /iobench.tmp
 */

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define DEFAULT_SIZE_MB  16
#define DEFAULT_BLK_KB   64
#define MAX_BLK_KB       256
#define DEFAULT_FILE     "/iobench.tmp"

static char buf[MAX_BLK_KB * 1024];

static uint64 gettime_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint64)tv.tv_sec * 1000000ULL + (uint64)tv.tv_usec;
}

static void fill_buf(int seed, int len)
{
    for (int i = 0; i < len; i++)
        buf[i] = (char)(seed + i);
}

static int parse_int(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

int main(int argc, char *argv[])
{
    int do_read = 1, do_write = 1;
    int size_mb = DEFAULT_SIZE_MB;
    int blk_kb = DEFAULT_BLK_KB;
    const char *file = DEFAULT_FILE;

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "-r")) {
            do_read = 1; do_write = 0;
        } else if (streq(argv[i], "-w")) {
            do_read = 0; do_write = 1;
        } else if (streq(argv[i], "-rw")) {
            do_read = 1; do_write = 1;
        } else if (streq(argv[i], "-s") && i + 1 < argc) {
            size_mb = parse_int(argv[++i]);
        } else if (streq(argv[i], "-b") && i + 1 < argc) {
            blk_kb = parse_int(argv[++i]);
        } else if (streq(argv[i], "-f") && i + 1 < argc) {
            file = argv[++i];
        } else {
            fprintf(2, "Usage: iobench [-r|-w|-rw] [-s SIZE_MB] [-b BLOCK_KB] [-f FILE]\n");
            exit(1);
        }
    }

    if (blk_kb <= 0 || blk_kb > MAX_BLK_KB) {
        fprintf(2, "iobench: block size must be 1-%d KB\n", MAX_BLK_KB);
        exit(1);
    }
    if (size_mb <= 0 || size_mb > 512) {
        fprintf(2, "iobench: size must be 1-512 MB\n");
        exit(1);
    }

    int blk_bytes = blk_kb * 1024;
    int64 total_bytes = (int64)size_mb * 1024 * 1024;
    int64 nblocks = total_bytes / blk_bytes;

    fprintf(1, "iobench: file=%s size=%dMB block=%dKB blocks=%d\n",
            file, size_mb, blk_kb, (int)nblocks);

    /* ── Write benchmark ── */
    if (do_write) {
        fill_buf(0x42, blk_bytes);

        int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            fprintf(2, "iobench: cannot open %s for writing\n", file);
            exit(1);
        }

        uint64 t0 = gettime_us();
        int64 written = 0;
        for (int64 i = 0; i < nblocks; i++) {
            int n = write(fd, buf, blk_bytes);
            if (n != blk_bytes) {
                fprintf(2, "iobench: write error at block %d (ret=%d)\n",
                        (int)i, n);
                close(fd);
                exit(1);
            }
            written += n;
        }
        uint64 t1 = gettime_us();
        close(fd);

        uint64 dt = t1 - t0;
        if (dt == 0) dt = 1;
        uint64 mbps = (written * 1000000ULL) / (dt * 1024 * 1024);
        uint64 mbps_frac = ((written * 1000000ULL * 10) / (dt * 1024 * 1024)) % 10;

        fprintf(1, "WRITE: %d bytes in %d us = %d.%d MB/s\n",
                (int)written, (int)dt, (int)mbps, (int)mbps_frac);
    }

    /* ── Read benchmark ── */
    if (do_read) {
        /* Drop page cache so reads hit disk (cold read) */
        {
            int dfd = open(file, O_RDONLY);
            if (dfd >= 0) {
                posix_fadvise(dfd, 0, 0, POSIX_FADV_DONTNEED);
                close(dfd);
            }
        }

        int fd = open(file, O_RDONLY);
        if (fd < 0) {
            fprintf(2, "iobench: cannot open %s for reading\n", file);
            exit(1);
        }

        uint64 t0 = gettime_us();
        int64 readtotal = 0;
        for (;;) {
            int n = read(fd, buf, blk_bytes);
            if (n <= 0)
                break;
            readtotal += n;
        }
        close(fd);
        uint64 t1 = gettime_us();

        uint64 dt = t1 - t0;
        if (dt == 0) dt = 1;
        uint64 mbps = (readtotal * 1000000ULL) / (dt * 1024 * 1024);
        uint64 mbps_frac = ((readtotal * 1000000ULL * 10) / (dt * 1024 * 1024)) % 10;

        fprintf(1, "READ:  %d bytes in %d us = %d.%d MB/s\n",
                (int)readtotal, (int)dt, (int)mbps, (int)mbps_frac);
    }

    /* Clean up */
    unlink(file);
    exit(0);
}
