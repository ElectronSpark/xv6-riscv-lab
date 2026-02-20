/*
 * sleep.c — musl libc version for xv6
 *
 * Note: The xv6 original uses a custom sleep() with ticks.
 * With musl, we use standard POSIX sleep() which takes seconds,
 * or nanosleep() for finer granularity.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        exit(1);
    }

    int secs = atoi(argv[1]);
    struct timespec ts = { .tv_sec = secs, .tv_nsec = 0 };
    nanosleep(&ts, NULL);

    return 0;
}
