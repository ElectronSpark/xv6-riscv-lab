#include "types.h"

void *memset(void *dst, int c, size_t n)
{
    char *cdst = (char *) dst;
    for(size_t i = 0; i < n; i++) {
        cdst[i] = c;
    }
    return dst;
}


int memcmp(const void *v1, const void *v2, size_t n)
{
    const char *s1 = v1;
    const char *s2 = v2;

    while(n-- > 0){
        if(*s1 != *s2) {
            return *s1 - *s2;
        }
        s1++, s2++;
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n)
{
    if(n == 0) {
        return dst;
    }
    const char *s = src;
    char *d = dst;

    if(s < d && s + n > d){
        s += n;
        d += n;
        while(n-- > 0)
        *--d = *--s;
    } else
        while(n-- > 0)
        *d++ = *s++;

    return dst;
}


// memcpy exists to placate GCC.  Use memmove.
void *memcpy(void *dst, const void *src, size_t n)
{
  return memmove(dst, src, n);
}


int strncmp(const char *p, const char *q, size_t n)
{
    while(n > 0 && *p && *p == *q) {
        n--, p++, q++;
    }
    if(n == 0) {
        return 0;
    }
    return *p - *q;
}

char* strncpy(char *s, const char *t, size_t n)
{
    char *os = s;
    while(n > 0 && (*s++ = *t++) != 0) {
        n--;
    }
    while(n-- > 0) {
        *s++ = 0;
    }
    return os;
}

// Like strncpy but guaranteed to NUL-terminate.
char* safestrcpy(char *s, const char *t, size_t n)
{
    char *os = s;
    if(n == 0) {
        return os;
    }
    while(--n > 0 && (*s++ = *t++) != 0);
    *s = 0;
    return os;
}

size_t strlen(const char *s)
{
    size_t n;
    for(n = 0; s[n]; n++);
    return n;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t n;
    for(n = 0; n < maxlen && s[n]; n++);
    return n;
}

char *strcat(char *dest, const char *src)
{
    size_t n = strlen(dest);
    size_t m = strlen(src);
    strncpy(dest + n, src, m);
    dest[n + m] = '\0';
    return dest;
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *token;

    // If str is NULL, continue from saved position
    if (str == 0)
        str = *saveptr;

    // Skip leading delimiters
    while (*str != '\0') {
        const char *d = delim;
        int is_delim = 0;
        while (*d != '\0') {
            if (*str == *d) {
                is_delim = 1;
                break;
            }
            d++;
        }
        if (!is_delim)
            break;
        str++;
    }

    // If we reached end of string, no more tokens
    if (*str == '\0') {
        *saveptr = str;
        return 0;
    }

    // Mark the start of the token
    token = str;

    // Find the end of the token
    while (*str != '\0') {
        const char *d = delim;
        while (*d != '\0') {
            if (*str == *d) {
                *str = '\0';
                *saveptr = str + 1;
                return token;
            }
            d++;
        }
        str++;
    }

    // Reached end of string, save position for next call
    *saveptr = str;
    return token;
}

char *strtok(char *str, const char *delim)
{
    static char *saveptr;
    return strtok_r(str, delim, &saveptr);
}
