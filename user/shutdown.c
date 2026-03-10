#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int r = poweroff();
    if (r < 0) {
        fprintf(2, "shutdown: failed (are you root?)\n");
        exit(1);
    }
    /* Should not reach here */
    exit(0);
}
