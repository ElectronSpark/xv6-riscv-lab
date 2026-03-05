/*
 * chmod.c — Change file mode bits for xv6
 *
 * Usage: chmod mode file...
 *
 * Mode can be an octal number (e.g., 755) or symbolic (not yet supported).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: chmod mode file...\n");
        return 1;
    }

    /* Parse octal mode */
    char *end;
    long mode = strtol(argv[1], &end, 8);
    if (*end != '\0' || mode < 0 || mode > 07777) {
        fprintf(stderr, "chmod: invalid mode '%s'\n", argv[1]);
        return 1;
    }

    int ret = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], (mode_t)mode) < 0) {
            perror(argv[i]);
            ret = 1;
        }
    }
    return ret;
}
