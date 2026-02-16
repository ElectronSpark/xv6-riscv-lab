/**
 * @file string.h
 * @brief String shim for lwext4 in xv6 kernel space.
 *
 * Redirect to the kernel's own string.h.
 */
#ifndef _EXT4_COMPAT_STRING_H
#define _EXT4_COMPAT_STRING_H

#include "types.h"
#include <stddef.h>

/* The kernel's string.c provides: memset, memcpy, memmove, memcmp,
 * strncpy, safestrcpy, strlen, strcmp, strncmp.
 *
 * lwext4 also needs: strcpy, strcat, strrchr, strncat, strchr
 *
 * Note: kernel functions use 'uint' for sizes; we declare with the same
 * types to match the actual implementations.  lwext4 passes size_t
 * (unsigned long on LP64) but on RV64 the calling convention makes
 * this safe — high 32 bits are simply ignored where applicable. */

/* Declarations of kernel-provided functions (from kernel/string.c) */
void *memset(void *, int, uint);
void *memmove(void *, const void *, uint);
void *memcpy(void *, const void *, uint);
int memcmp(const void *, const void *, uint);
int strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, uint);
char *strncpy(char *, const char *, int);

/* Additional string functions needed by lwext4 */
static inline char *strcpy(char *dst, const char *src)
{
    char *ret = dst;
    while ((*dst++ = *src++) != '\0')
        ;
    return ret;
}

static inline char *strcat(char *dst, const char *src)
{
    char *ret = dst;
    while (*dst)
        dst++;
    while ((*dst++ = *src++) != '\0')
        ;
    return ret;
}

static inline char *strchr(const char *s, int c)
{
    for (; *s; s++) {
        if (*s == (char)c)
            return (char *)s;
    }
    return (c == '\0') ? (char *)s : 0;
}

static inline char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (; *s; s++) {
        if (*s == (char)c)
            last = s;
    }
    if (c == '\0')
        return (char *)s;
    return (char *)last;
}

static inline size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n])
        n++;
    return n;
}

/* strndup — provided by the kernel (kernel/string.c) */
char *strndup(const char *s, size_t n);

#endif /* _EXT4_COMPAT_STRING_H */
