#include "user/user.h"

int main(int argc, char **argv) {
    unsigned char buf[32];
    static char hex[] = "0123456789abcdef";
    char out[32 * 2 + 1];

    int n = getrandom(buf, sizeof(buf));
    if (n < 0) {
        fprintf(2, "randtest: getrandom failed: %d\n", n);
        exit(1);
    }

    for (int i = 0; i < n; i++) {
        out[i * 2] = hex[(buf[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[buf[i] & 0xF];
    }
    out[n * 2] = '\0';

    printf("%s\n", out);
    exit(0);
}
