#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(2, "Usage: mount <source> <target> <fstype>\n");
        fprintf(2, "  source: device path (e.g., /dev/disk0)\n");
        fprintf(2, "  target: mount point directory\n");
        fprintf(2, "  fstype: filesystem type (e.g., xv6fs)\n");
        exit(1);
    }

    if (mount(argv[1], argv[2], argv[3]) < 0) {
        fprintf(2, "mount: failed to mount %s on %s\n", argv[1], argv[2]);
        exit(1);
    }

    exit(0);
}
