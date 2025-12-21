#ifndef __KERNEL_STRING_H
#define __KERNEL_STRING_H

#include "types.h"

void *memset(void *dst, int c, size_t n);
int memcmp(const void *v1, const void *v2, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int strncmp(const char *p, const char *q, size_t n);
char* strncpy(char *s, const char *t, size_t n);
char* safestrcpy(char *s, const char *t, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char *strcat(char *dest, const char *src);

#endif /* __KERNEL_STRING_H */
