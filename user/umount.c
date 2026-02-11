#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(2, "Usage: umount <target>\n");
        fprintf(2, "  target: mount point to unmount\n");
        exit(1);
    }

    if (umount(argv[1]) < 0) {
        fprintf(2, "umount: failed to unmount %s\n", argv[1]);
        exit(1);
    }

    exit(0);
}
