#include "kernel/inc/types.h"
#include "kernel/inc/stat.h"
#include "kernel/inc/fcntl.h"
#include "user/user.h"

int main(void) {
    dumppcache();
    return 0;
}
