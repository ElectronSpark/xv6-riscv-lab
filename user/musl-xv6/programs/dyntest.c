/*
 * dyntest.c - Dynamic linking diagnostic tool
 *
 * This program is dynamically linked against musl libc.so, the same way
 * as Python. Use it to verify that dynamic linking, the interpreter
 * (ld-musl-riscv64.so.1), and libc.so are all working on the target
 * platform.
 *
 * If this runs but Python doesn't, the issue is Python-specific.
 * If this also fails, the issue is in the dynamic linker / ELF loader.
 *
 * Usage: dyntest [options]
 *   -m    test malloc/mmap
 *   -f    test file I/O
 *   -s    test string/math functions from libc.so
 *   (no args) run all tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* Write directly to fd 1 using write() — no libc buffering.
 * This works even if stdio is broken. */
static void direct_write(const char *msg)
{
    write(1, msg, strlen(msg));
}

/* Convert a 64-bit value to hex string */
static void hex64(unsigned long val, char *buf)
{
    const char *hex = "0123456789abcdef";
    buf[0]  = '0';
    buf[1]  = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hex[(val >> (i * 4)) & 0xf];
    }
    buf[18] = '\0';
}

/* Write address for debugging */
static void print_addr(const char *label, void *addr)
{
    char buf[20];
    direct_write(label);
    direct_write(": ");
    hex64((unsigned long)addr, buf);
    direct_write(buf);
    direct_write("\n");
}

static int test_basic(void)
{
    direct_write("[dyntest] === Basic Dynamic Link Test ===\n");
    direct_write("[dyntest] If you see this, the dynamic linker worked!\n");

    /* Show addresses of key functions/objects to verify relocation */
    print_addr("[dyntest] &printf     ", (void *)(unsigned long)printf);
    print_addr("[dyntest] &malloc     ", (void *)(unsigned long)malloc);
    print_addr("[dyntest] &errno      ", (void *)&errno);

    return 0;
}

static int test_stdio(void)
{
    direct_write("[dyntest] === stdio test ===\n");

    /* Test printf (uses GOT/PLT relocation to libc.so) */
    printf("[dyntest] printf works! pid=%d\n", getpid());

    /* Test fprintf */
    fprintf(stdout, "[dyntest] fprintf(stdout) works!\n");
    fflush(stdout);

    /* Test snprintf */
    char buf[64];
    snprintf(buf, sizeof(buf), "[dyntest] snprintf works: %d + %d = %d\n",
             17, 25, 17 + 25);
    direct_write(buf);

    return 0;
}

static int test_malloc(void)
{
    direct_write("[dyntest] === malloc/mmap test ===\n");

    /* Small allocation (should use brk or mmap) */
    void *p1 = malloc(64);
    if (!p1) {
        direct_write("[dyntest] FAIL: malloc(64) returned NULL\n");
        return 1;
    }
    print_addr("[dyntest] malloc(64)  ", p1);
    memset(p1, 0xAA, 64);

    /* Medium allocation */
    void *p2 = malloc(4096);
    if (!p2) {
        direct_write("[dyntest] FAIL: malloc(4096) returned NULL\n");
        free(p1);
        return 1;
    }
    print_addr("[dyntest] malloc(4096)", p2);
    memset(p2, 0xBB, 4096);

    /* Large allocation (forces mmap) */
    void *p3 = malloc(256 * 1024);
    if (!p3) {
        direct_write("[dyntest] FAIL: malloc(256K) returned NULL\n");
        free(p2);
        free(p1);
        return 1;
    }
    print_addr("[dyntest] malloc(256K)", p3);
    memset(p3, 0xCC, 256 * 1024);

    free(p3);
    free(p2);
    free(p1);
    direct_write("[dyntest] malloc/free OK\n");
    return 0;
}

static int test_fileio(void)
{
    direct_write("[dyntest] === file I/O test ===\n");

    /* Test reading a known file */
    FILE *f = fopen("/lib/libc.so", "rb");
    if (!f) {
        direct_write("[dyntest] FAIL: cannot open /lib/libc.so\n");

        /* Check if the file exists via direct open */
        int fd = open("/lib/libc.so", O_RDONLY);
        if (fd >= 0) {
            direct_write("[dyntest]   ...but open() succeeded (fd=");
            char b[4];
            b[0] = '0' + fd;
            b[1] = ')';
            b[2] = '\n';
            b[3] = 0;
            direct_write(b);
            close(fd);
        } else {
            direct_write("[dyntest]   open() also failed\n");
        }
        return 1;
    }

    /* Read ELF magic */
    unsigned char magic[4];
    size_t nread = fread(magic, 1, 4, f);
    fclose(f);

    if (nread == 4 && magic[0] == 0x7f && magic[1] == 'E' &&
        magic[2] == 'L' && magic[3] == 'F') {
        direct_write("[dyntest] /lib/libc.so: valid ELF\n");
    } else {
        direct_write("[dyntest] FAIL: /lib/libc.so: bad ELF magic\n");
        return 1;
    }

    /* Check if ld-musl symlink exists */
    struct stat st;
#ifdef __x86_64__
    const char *ldmusl = "/lib/ld-musl-x86_64.so.1";
#else
    const char *ldmusl = "/lib/ld-musl-riscv64.so.1";
#endif
    if (stat(ldmusl, &st) == 0) {
        printf("[dyntest] %s: size=%ld\n", ldmusl, (long)st.st_size);
    } else {
        printf("[dyntest] WARN: stat(%s) failed\n", ldmusl);
    }

    /* Check if python binary exists */
    if (stat("/python", &st) == 0) {
        printf("[dyntest] /python: size=%ld\n", (long)st.st_size);
    } else if (stat("/bin/python", &st) == 0) {
        printf("[dyntest] /bin/python: size=%ld\n", (long)st.st_size);
    } else {
        direct_write("[dyntest] WARN: python binary not found at /python or /bin/python\n");
    }

    return 0;
}

static int test_strings(void)
{
    direct_write("[dyntest] === string/math test ===\n");

    /* Test string functions from libc.so */
    char buf[128];
    strcpy(buf, "hello");
    strcat(buf, " world");
    if (strcmp(buf, "hello world") != 0) {
        direct_write("[dyntest] FAIL: string ops\n");
        return 1;
    }
    printf("[dyntest] strlen(\"%s\") = %zu\n", buf, strlen(buf));

    /* Test memcpy/memset from libc.so */
    char a[32], b[32];
    memset(a, 'X', 16);
    a[16] = '\0';
    memcpy(b, a, 17);
    if (strcmp(b, "XXXXXXXXXXXXXXXX") != 0) {
        direct_write("[dyntest] FAIL: memcpy/memset\n");
        return 1;
    }
    direct_write("[dyntest] string/mem ops OK\n");
    return 0;
}

int main(int argc, char *argv[])
{
    direct_write("[dyntest] starting (dynamically linked against libc.so)\n");

    int do_all = (argc <= 1);
    int ret = 0;

    ret |= test_basic();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0)
            ret |= test_malloc();
        else if (strcmp(argv[i], "-f") == 0)
            ret |= test_fileio();
        else if (strcmp(argv[i], "-s") == 0)
            ret |= test_strings();
    }

    if (do_all) {
        ret |= test_stdio();
        ret |= test_malloc();
        ret |= test_fileio();
        ret |= test_strings();
    }

    if (ret == 0) {
        direct_write("[dyntest] ALL TESTS PASSED\n");
    } else {
        direct_write("[dyntest] SOME TESTS FAILED\n");
    }

    return ret;
}
