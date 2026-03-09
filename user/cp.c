#include "kernel/inc/types.h"
#include "kernel/inc/param.h"
#include "kernel/inc/errno.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#include "fsutil.h"

int main(int argc, char *argv[]) {
    int recursive = 0;
    int first_path = 1;

    while (first_path < argc && argv[first_path][0] == '-' &&
           argv[first_path][1] != '\0') {
        for (int j = 1; argv[first_path][j] != '\0'; j++) {
            if (argv[first_path][j] == 'r' || argv[first_path][j] == 'R') {
                recursive = 1;
            } else {
                fprintf(2, "cp: unknown option -%c\n", argv[first_path][j]);
                exit(1);
            }
        }
        first_path++;
    }

    int nargs = argc - first_path;
    if (nargs < 2) {
        fprintf(2, "Usage: cp [-r|-R] source... destination\n");
        exit(1);
    }

    char *dst = argv[argc - 1];
    int many_sources = nargs > 2;
    int status = 0;

    for (int i = first_path; i < argc - 1; i++) {
        char final_dst[MAXPATH];
        int ret = fsutil_resolve_target(final_dst, sizeof(final_dst), argv[i],
                                        dst, many_sources);
        if (ret < 0) {
            fprintf(2, "cp: cannot use %s as destination for %s (%d)\n", dst,
                    argv[i], ret);
            status = 1;
            continue;
        }

        ret = fsutil_copy_path(argv[i], final_dst, recursive);
        if (ret < 0) {
            fprintf(2, "cp: failed to copy %s to %s (%d)\n", argv[i],
                    final_dst, ret);
            status = 1;
        }
    }

    exit(status);
}