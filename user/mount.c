#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(2, "Usage: mount <source> <target> <fstype>\n");
        fprintf(2, "\n");
        fprintf(2, "  source can be:\n");
        fprintf(2, "    /dev/disk1p1           block device path\n");
        fprintf(2, "    UUID=xxxx-xxxx-...     partition GUID\n");
        fprintf(2, "    /path/to/image.ext4    file (auto loop device)\n");
        fprintf(2, "    none                   pseudo fs (tmpfs, devtmpfs)\n");
        fprintf(2, "  target: mount point directory\n");
        fprintf(2, "  fstype: filesystem type (e.g., ext4, xv6fs, tmpfs)\n");
        exit(1);
    }

    if (mount(argv[1], argv[2], argv[3]) < 0) {
        fprintf(2, "mount: failed to mount %s on %s\n", argv[1], argv[2]);
        exit(1);
    }

    exit(0);
}
