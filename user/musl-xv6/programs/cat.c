/*
 * cat.c — musl libc version for xv6
 *
 * Concatenate files and print on the standard output.
 * Uses standard POSIX headers instead of kernel headers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

static char buf[512];

static void cat(int fd)
{
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(STDOUT_FILENO, buf, n) != n) {
            fprintf(stderr, "cat: write error\n");
            exit(1);
        }
    }
    if (n < 0) {
        fprintf(stderr, "cat: read error\n");
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    int fd, i;

    if (argc <= 1) {
        cat(STDIN_FILENO);
        return 0;
    }

    for (i = 1; i < argc; i++) {
        if ((fd = open(argv[i], O_RDONLY)) < 0) {
            fprintf(stderr, "cat: cannot open %s\n", argv[i]);
            exit(1);
        }
        cat(fd);
        close(fd);
    }
    return 0;
}
