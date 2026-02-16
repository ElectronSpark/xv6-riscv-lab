// Create a zombie process that
// must be reparented at exit.

#include "kernel/inc/uabi/stat.h"
#include "user/user.h"

int main(void) {
    if (fork() > 0)
        sleep(500); // Let child exit before parent.
    exit(0);
}
