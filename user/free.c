#include "kernel/inc/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    int detailed = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            detailed = 1;
        }
    }
    
    memstat(detailed);
    return 0;
}
