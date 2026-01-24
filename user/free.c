#include "kernel/inc/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    uint64 flags = MEMSTAT_VERBOSE | MEMSTAT_INCLUDE_BUDDY | MEMSTAT_INCLUDE_SLAB |
                   MEMSTAT_ADD_FREE | MEMSTAT_ADD_USED;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            flags |= MEMSTAT_DETAILED;
        }
    }

    memstat(flags);
    return 0;
}
