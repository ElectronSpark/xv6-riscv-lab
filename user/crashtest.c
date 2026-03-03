#include "user/user.h"

int main(int argc, char **argv) {
    printf("triggering kernel crash via uptime()...\n");
    uptime();
    printf("should not reach here\n");
    exit(0);
}
