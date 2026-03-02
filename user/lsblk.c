/*
 * lsblk — list block devices and partitions
 *
 * Usage:
 *   lsblk        Compact disk/partition listing (gendisk tree)
 *   lsblk -l     Detailed listing with partition offsets and sizes
 *   lsblk -a     List all registered block devices (flat)
 */

#include "kernel/inc/types.h"
#include "user/user.h"

static void usage(void) {
    printf("Usage: lsblk [options]\n");
    printf("  (none)  compact disk/partition tree\n");
    printf("  -l      detailed disk/partition info\n");
    printf("  -a      all registered block devices\n");
}

int main(int argc, char *argv[]) {
    int mode = 0;

    if (argc > 1) {
        if (argv[1][0] == '-' && argv[1][1] && !argv[1][2]) {
            switch (argv[1][1]) {
            case 'l':
                mode = 1;
                break;
            case 'a':
                mode = 2;
                break;
            default:
                usage();
                return 1;
            }
        } else {
            usage();
            return 1;
        }
    }

    dumpblk(mode);
    return 0;
}
