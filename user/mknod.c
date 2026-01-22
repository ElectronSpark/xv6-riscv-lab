#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

void
usage(void)
{
    fprintf(2, "Usage: mknod [-b|-c] <name> <major> <minor>\n");
    fprintf(2, "  -b:    create block device (default: character device)\n");
    fprintf(2, "  -c:    create character device (default)\n");
    fprintf(2, "  name:  path of the device node to create\n");
    fprintf(2, "  major: major device number\n");
    fprintf(2, "  minor: minor device number\n");
    exit(1);
}

int
main(int argc, char *argv[])
{
    mode_t mode = S_IFCHR | 0666;  // Default to character device
    int argidx = 1;
    
    if (argc < 4) {
        usage();
    }
    
    // Parse optional type flag
    if (argv[1][0] == '-') {
        if (argv[1][1] == 'b') {
            mode = S_IFBLK | 0666;
        } else if (argv[1][1] == 'c') {
            mode = S_IFCHR | 0666;
        } else {
            usage();
        }
        argidx = 2;
        if (argc < 5) {
            usage();
        }
    }

    char *name = argv[argidx];
    int major = atoi(argv[argidx + 1]);
    int minor = atoi(argv[argidx + 2]);

    if (mknod(name, mode, major, minor) < 0) {
        fprintf(2, "mknod: failed to create %s (mode=0x%x, %d, %d)\n", name, mode, major, minor);
        exit(1);
    }

    exit(0);
}
