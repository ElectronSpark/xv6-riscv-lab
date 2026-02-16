#include "kernel/inc/uabi/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    sync();
    exit(0);
}