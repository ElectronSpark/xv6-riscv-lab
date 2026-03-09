#include "kernel/inc/types.h"
#include "kernel/inc/param.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#include "fsutil.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(2, "Usage: mv source... destination\n");
        exit(1);
    }

    char *dst = argv[argc - 1];
    int many_sources = argc > 3;
    int status = 0;

    for (int i = 1; i < argc - 1; i++) {
        char final_dst[MAXPATH];
        int ret = fsutil_resolve_target(final_dst, sizeof(final_dst), argv[i],
                                        dst, many_sources);
        if (ret < 0) {
            fprintf(2, "mv: cannot use %s as destination for %s (%d)\n", dst,
                    argv[i], ret);
            status = 1;
            continue;
        }

        ret = fsutil_move_path(argv[i], final_dst);
        if (ret < 0) {
            fprintf(2, "mv: failed to move %s to %s (%d)\n", argv[i],
                    final_dst, ret);
            status = 1;
        }
    }

    exit(status);
}