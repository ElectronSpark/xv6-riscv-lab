#ifndef _XV6_STDIO_H
#define _XV6_STDIO_H

#include <stdarg.h>

#include "stddef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EOF (-1)

int printf(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
int fprintf(int fd, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
int vprintf(int fd, const char *fmt, va_list ap) __attribute__ ((format (printf, 2, 0)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__ ((format (printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* _XV6_STDIO_H */
