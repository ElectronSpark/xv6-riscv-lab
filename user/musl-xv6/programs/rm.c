/*
 * rm.c — musl libc version for xv6
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: rm files...\n");
        exit(1);
    }

    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) < 0) {
            fprintf(stderr, "rm: %s failed to delete\n", argv[i]);
            break;
        }
    }
    return 0;
}
