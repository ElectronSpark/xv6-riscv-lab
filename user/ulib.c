#include "string.h"
#include "stddef.h"
#include "stdint.h"
#include "sys/stat.h"
#include "fcntl.h"
#include "unistd.h"
#include "stdlib.h"

// wrapper so that it's OK if main() does not call exit().
void
start(void)
{
  extern int main(void);
  main();
  exit(0);
}

#ifndef XV6_MUSL_PORT
char*
strcpy(char *dst, const char *src)
{
  char *out = dst;
  while ((*dst++ = *src++) != '\0')
    ;
  return out;
}

int
strcmp(const char *lhs, const char *rhs)
{
  while (*lhs && *lhs == *rhs)
    lhs++, rhs++;
  return (unsigned char)*lhs - (unsigned char)*rhs;
}

size_t
strlen(const char *s)
{
  size_t len = 0;
  while (s[len] != '\0')
    len++;
  return len;
}

void*
memset(void *dst, int c, size_t n)
{
  unsigned char *p = (unsigned char *)dst;
  for (size_t i = 0; i < n; i++)
    p[i] = (unsigned char)c;
  return dst;
}

char*
strchr(const char *s, int c)
{
  unsigned char uc = (unsigned char)c;
  while (*s) {
    if ((unsigned char)*s == uc)
      return (char *)s;
    s++;
  }
  return (uc == '\0') ? (char *)s : 0;
}

void*
memmove(void *dstv, const void *srcv, size_t n)
{
  unsigned char *dst = (unsigned char *)dstv;
  const unsigned char *src = (const unsigned char *)srcv;
  if (src > dst) {
    for (size_t i = 0; i < n; i++)
      dst[i] = src[i];
  } else if (n != 0) {
    for (size_t i = n; i > 0; i--)
      dst[i - 1] = src[i - 1];
  }
  return dstv;
}

int
memcmp(const void *s1, const void *s2, size_t n)
{
  const unsigned char *p1 = (const unsigned char *)s1;
  const unsigned char *p2 = (const unsigned char *)s2;
  for (size_t i = 0; i < n; i++) {
    if (p1[i] != p2[i])
      return (int)p1[i] - (int)p2[i];
  }
  return 0;
}

void*
memcpy(void *dst, const void *src, size_t n)
{
  return memmove(dst, src, n);
}

char*
strncpy(char *dst, const char *src, size_t n)
{
  size_t i = 0;
  for (; i < n && src[i] != '\0'; i++)
    dst[i] = src[i];
  for (; i < n; i++)
    dst[i] = '\0';
  return dst;
}

int
strncmp(const char *s1, const char *s2, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    unsigned char c1 = (unsigned char)s1[i];
    unsigned char c2 = (unsigned char)s2[i];
    if (c1 != c2 || c1 == '\0' || c2 == '\0')
      return (int)c1 - (int)c2;
  }
  return 0;
}

size_t
strnlen(const char *s, size_t maxlen)
{
  size_t len = 0;
  while (len < maxlen && s[len] != '\0')
    len++;
  return len;
}
#endif /* XV6_MUSL_PORT */

char*
gets(char *buf, int max)
{
  int i = 0;
  while (i + 1 < max) {
    char c;
    int cc = read(0, &c, 1);
    if (cc < 1)
      break;
    buf[i++] = c;
    if (c == '\n' || c == '\r')
      break;
  }
  buf[i] = '\0';
  return buf;
}

int
stat(const char *path, struct stat *st)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int r = fstat(fd, st);
  close(fd);
  return r;
}

int
atoi(const char *s)
{
  int n = 0;
  while (*s >= '0' && *s <= '9')
    n = n * 10 + *s++ - '0';
  return n;
}
