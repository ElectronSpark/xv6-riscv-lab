#include "kernel/inc/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc > 1) {
        // Dump inodes for the superblock containing the given path
        dumpinode(argv[1]);
    } else {
        // Dump all inodes
        dumpinode(0);
    }
    return 0;
}
