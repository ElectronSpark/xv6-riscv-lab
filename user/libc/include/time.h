#ifndef _XV6_TIME_H
#define _XV6_TIME_H

#include "stdint.h"
#include "stddef.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef long time_t;
typedef long suseconds_t;

typedef struct timespec {
    time_t tv_sec;
    long   tv_nsec;
} timespec;

typedef struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
} timeval;

typedef int clockid_t;
typedef long clock_t;

typedef struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
} tm;

clock_t clock(void);
int clock_gettime(clockid_t clk_id, struct timespec *tp);
int nanosleep(const struct timespec *req, struct timespec *rem);
int gettimeofday(struct timeval *tv, void *tz);

#ifdef __cplusplus
}
#endif

#endif /* _XV6_TIME_H */
