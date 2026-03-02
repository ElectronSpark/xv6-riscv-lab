/*
 * losetup — manage loop devices
 *
 * Usage:
 *   losetup                          list all loop devices
 *   losetup /dev/loopN <file>        attach file to loop device
 *   losetup -d /dev/loopN            detach loop device
 */
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

static int parse_loop_num(const char *dev) {
    /* Accept "loopN" or "/dev/loopN" */
    const char *p = dev;

    /* Skip "/dev/" prefix if present */
    if (p[0] == '/' && p[1] == 'd' && p[2] == 'e' &&
        p[3] == 'v' && p[4] == '/') {
        p += 5;
    }

    /* Expect "loop" prefix */
    if (p[0] != 'l' || p[1] != 'o' || p[2] != 'o' || p[3] != 'p') {
        return -1;
    }
    p += 4;

    /* Parse decimal number */
    int num = 0;
    if (*p == '\0')
        return -1;
    while (*p >= '0' && *p <= '9') {
        num = num * 10 + (*p - '0');
        p++;
    }
    if (*p != '\0')
        return -1;
    return num;
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        /* List all loop devices */
        losetup(2, 0, 0);
        exit(0);
    }

    if (argc == 3 && argv[1][0] == '-' && argv[1][1] == 'd') {
        /* Detach: losetup -d /dev/loopN */
        int num = parse_loop_num(argv[2]);
        if (num < 0) {
            fprintf(2, "losetup: invalid device: %s\n", argv[2]);
            exit(1);
        }
        int ret = losetup(1, num, 0);
        if (ret < 0) {
            fprintf(2, "losetup: failed to detach loop%d: %d\n", num, ret);
            exit(1);
        }
        exit(0);
    }

    if (argc == 3) {
        /* Setup: losetup /dev/loopN <file> */
        int num = parse_loop_num(argv[1]);
        if (num < 0) {
            fprintf(2, "losetup: invalid device: %s\n", argv[1]);
            exit(1);
        }
        int ret = losetup(0, num, argv[2]);
        if (ret < 0) {
            fprintf(2, "losetup: failed to attach %s to loop%d: %d\n",
                    argv[2], num, ret);
            exit(1);
        }
        exit(0);
    }

    fprintf(2, "Usage:\n");
    fprintf(2, "  losetup                     list loop devices\n");
    fprintf(2, "  losetup /dev/loopN <file>   attach file to loop device\n");
    fprintf(2, "  losetup -d /dev/loopN       detach loop device\n");
    exit(1);
}
