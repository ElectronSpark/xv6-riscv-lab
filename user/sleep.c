#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int 
main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("sleep must receive the number of miniseconds as parameter\n");
        exit(1);
    }
    int secs = atoi(argv[0]);
    sleep(secs);
    exit(0);
}