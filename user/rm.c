#include "kernel/inc/types.h"
#include "kernel/inc/errno.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#include "fsutil.h"

int main(int argc, char *argv[]) {
    int i;
    int recursive = 0;
    int force = 0;

    if (argc < 2) {
        fprintf(2, "Usage: rm [-f] [-r|-R] files...\n");
        exit(1);
    }

    int first_path = 1;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            break;
        }
        for (int j = 1; argv[i][j] != '\0'; j++) {
            switch (argv[i][j]) {
            case 'f':
                force = 1;
                break;
            case 'r':
            case 'R':
                recursive = 1;
                break;
            default:
                fprintf(2, "rm: unknown option -%c\n", argv[i][j]);
                exit(1);
            }
        }
        first_path = i + 1;
    }

    if (first_path >= argc) {
        fprintf(2, "rm: missing operand\n");
        exit(1);
    }

    int status = 0;
    for (i = first_path; i < argc; i++) {
        int ret = fsutil_remove_path(argv[i], recursive, force);
        if (ret < 0 && !(force && ret == -ENOENT)) {
            fprintf(2, "rm: %s failed to delete (%d)\n", argv[i], ret);
            status = 1;
        }
    }

    exit(status);
}
