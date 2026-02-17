#ifndef __USER_ABI_TIME_H
#define __USER_ABI_TIME_H

#include "types.h"

struct timeval {
    int64 tv_sec;
    int64 tv_usec;
};

struct timespec {
    int64 tv_sec;
    int64 tv_nsec;
};

#endif /* __USER_ABI_TIME_H */
