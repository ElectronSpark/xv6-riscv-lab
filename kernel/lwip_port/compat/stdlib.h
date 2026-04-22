/**
 * @file stdlib.h
 * @brief Minimal stdlib.h shim for lwIP in xv6 kernel space.
 *
 * This shadows newlib's stdlib.h to avoid type conflicts.
 * Only provides what lwIP actually needs (atoi).
 */
#ifndef _LWIP_COMPAT_STDLIB_H
#define _LWIP_COMPAT_STDLIB_H

#include "types.h"

/* atoi — used by netdb.c (only when LWIP_SOCKET is enabled) */
static inline int atoi(const char *s)
{
    int n = 0, neg = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s++ - '0');
    }
    return neg ? -n : n;
}

#endif /* _LWIP_COMPAT_STDLIB_H */
