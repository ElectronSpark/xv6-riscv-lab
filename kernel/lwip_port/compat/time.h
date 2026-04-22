/**
 * @file time.h
 * @brief Minimal time.h shim for lwIP in xv6 kernel space.
 *
 * Shadows the toolchain's time.h to avoid pulling in sys/types.h
 * (which conflicts with xv6's dev_t definition).
 * Provides time_t and ctime() backed by the Goldfish RTC.
 */
#ifndef _LWIP_COMPAT_TIME_H
#define _LWIP_COMPAT_TIME_H

#include "types.h"
#include "timer/goldfish_rtc.h"

typedef int64 time_t;

/**
 * ctime() — Format a Unix timestamp as a human-readable string.
 *
 * Returns a static buffer with "YYYY-MM-DD HH:MM:SS\n".
 * Minimal implementation (no timezone support, UTC only).
 */
static inline char *ctime(const time_t *timer)
{
    static char buf[32];
    uint64 t = (uint64)*timer;

    /* Break Unix timestamp into date/time components */
    uint64 secs_in_day = t % 86400;
    uint64 days = t / 86400;

    unsigned h = (unsigned)(secs_in_day / 3600);
    unsigned m = (unsigned)((secs_in_day % 3600) / 60);
    unsigned s = (unsigned)(secs_in_day % 60);

    /* Days since 1970-01-01 → year/month/day (simplified Gregorian) */
    unsigned y = 1970;
    while (1) {
        unsigned dy = 365 + ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0);
        if (days < dy)
            break;
        days -= dy;
        y++;
    }
    /* month lengths */
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    unsigned mdays[] = {31,28+leap,31,30,31,30,31,31,30,31,30,31};
    unsigned mon = 0;
    while (mon < 12 && days >= mdays[mon]) {
        days -= mdays[mon];
        mon++;
    }

    /* Manual decimal formatting (no snprintf available in kernel) */
    char *p = buf;

    /* YYYY- */
    p[0] = '0' + (y / 1000) % 10; p[1] = '0' + (y / 100) % 10;
    p[2] = '0' + (y / 10) % 10;   p[3] = '0' + y % 10;
    p[4] = '-'; p += 5;

    /* MM- */
    unsigned mo = mon + 1;
    p[0] = '0' + mo / 10; p[1] = '0' + mo % 10; p[2] = '-'; p += 3;

    /* DD */
    unsigned d = (unsigned)days + 1;
    p[0] = '0' + d / 10; p[1] = '0' + d % 10; p[2] = ' '; p += 3;

    /* HH:MM:SS */
    p[0] = '0' + h / 10; p[1] = '0' + h % 10; p[2] = ':'; p += 3;
    p[0] = '0' + m / 10; p[1] = '0' + m % 10; p[2] = ':'; p += 3;
    p[0] = '0' + s / 10; p[1] = '0' + s % 10; p += 2;

    *p++ = '\n';
    *p = '\0';

    return buf;
}

#endif /* _LWIP_COMPAT_TIME_H */
