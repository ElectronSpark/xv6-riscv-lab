/*
 * mkdir.c — musl libc version for xv6
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: mkdir dirs...\n");
        exit(1);
    }

    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            fprintf(stderr, "mkdir: %s failed to create\n", argv[i]);
            break;
        }
    }
    return 0;
}
