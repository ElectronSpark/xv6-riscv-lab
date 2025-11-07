#ifndef _XV6_STRING_H
#define _XV6_STRING_H

#include "stddef.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
int strncmp(const char *s1, const char *s2, size_t n);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strnlen(const char *s, size_t maxlen);
void *memchr(const void *s, int c, size_t n);
char *strchrnul(const char *s, int c);
char *stpcpy(char *dst, const char *src);
char *__stpcpy(char *dst, const char *src);
char *stpncpy(char *dst, const char *src, size_t n);
char *__stpncpy(char *dst, const char *src, size_t n);
char *__strchrnul(const char *s, int c);

#ifdef __cplusplus
}
#endif

#endif /* _XV6_STRING_H */
