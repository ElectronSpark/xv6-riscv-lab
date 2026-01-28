#ifndef __KERNEL_STRING_H
#define __KERNEL_STRING_H

#include "types.h"

void *memset(void *dst, int c, size_t n);
int memcmp(const void *v1, const void *v2, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int strcmp(const char *p, const char *q);
int strncmp(const char *p, const char *q, size_t n);
char* strncpy(char *s, const char *t, size_t n);
char* safestrcpy(char *s, const char *t, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char *strcat(char *dest, const char *src);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strtok(char *str, const char *delim);
const char* strstr(const char *haystack, const char *needle);
char *strndup(const char *s, size_t n);
char *strdup(const char *s);

// Only define strtoul if not already provided by host stdlib (for unit tests)
#ifndef __KERNEL_SKIP_STRTOUL
static inline uint64
strtoul(const char *nptr, char **endptr, int base)
{
    uint64 result = 0;
    const char *p = NULL;
    for (p = nptr; *p; p++) {
        int digit = 0;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            digit = *p - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        result = result * base + digit;
    }
    if (endptr) {
        *endptr = (char *)p;
    }
    return result;
}
#endif // __KERNEL_SKIP_STRTOUL

// None standard functions
static inline bool str_startswith(const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static inline bool str_endswith(const char *str, const char *suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) {
        return false;
    }
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

#endif /* __KERNEL_STRING_H */
