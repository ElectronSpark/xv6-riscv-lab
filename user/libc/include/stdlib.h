#ifndef _XV6_STDLIB_H
#define _XV6_STDLIB_H

#include "stddef.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
int atexit(void (*func)(void));
int atoi(const char *nptr);
long strtol(const char *restrict nptr, char **restrict endptr, int base);
unsigned long strtoul(const char *restrict nptr, char **restrict endptr, int base);
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
int posix_memalign(void **memptr, size_t alignment, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* _XV6_STDLIB_H */
