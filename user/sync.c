#include "kernel/inc/types.h"
#include "kernel/inc/stat.h"
#include "user/user.h"

int 
main(int argc, char *argv[])
{
    sync();
    exit(0);
}