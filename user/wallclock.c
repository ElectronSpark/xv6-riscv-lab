#include "user/user.h"

int main(int argc, char **argv) {
    struct timeval tv;

    if (gettimeofday(&tv, 0) < 0) {
        fprintf(2, "wallclock: gettimeofday failed\n");
        exit(1);
    }

    printf("%d.%06d\n", (int)tv.tv_sec, (int)tv.tv_usec);
    exit(0);
}
