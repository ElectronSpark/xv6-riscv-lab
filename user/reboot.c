#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int r = reboot();
    if (r < 0) {
        fprintf(2, "reboot: failed (are you root?)\n");
        exit(1);
    }
    /* Should not reach here */
    exit(0);
}
